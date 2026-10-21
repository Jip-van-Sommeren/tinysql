#include "db_database.h"
#include "db_storage_engine.h"
#include "db_test_directory.h"
#include "db_test_journal.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/openat2.h>
#include <map>
#include <set>
#include <sys/syscall.h>
#include <unistd.h>

// A deterministic storage model behind the production native-I/O interface.
// The real files are only the process-visible cache. File fsync persists inode
// bytes/length; directory fsync separately persists names. At each native
// mutation, materialize crash images from durable state, dropping unsynced data
// and names, allowing early writeback, and tearing each dirty physical sector.
// Thus closing/reopening alone is NOT the simulated power loss.
namespace
{
    using Path = std::filesystem::path;
    using Bytes = journal_test::Bytes;
    using Image = std::map<Path, Bytes>;
    constexpr std::size_t Sector = 4096; // Independent of production constants.

    void require(bool value, const std::string &message)
    {
        if (!value) throw std::runtime_error(message);
    }

    Path descriptorPath(int fd)
    {
        char buffer[4096];
        const auto size = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), buffer, sizeof(buffer));
        require(size >= 0, "Cannot resolve model descriptor");
        return std::string(buffer, static_cast<std::size_t>(size));
    }

    struct Inode
    {
        Bytes cache;
        Bytes durable;
        std::set<std::size_t> dirty;
    };
    struct State
    {
        std::map<Path, std::size_t> names;
        std::map<Path, std::size_t> durableNames;
        std::vector<Inode> inodes;
    };
    struct Cut
    {
        State state;
        std::string event;
    };
    struct Model
    {
        Path root;
        State state;
        std::vector<Cut> cuts;
        std::set<std::string> operations;

        explicit Model(Path path) : root(std::move(path))
        {
            for (const auto &entry : std::filesystem::recursive_directory_iterator(root))
            {
                if (!entry.is_regular_file()) continue;
                const auto name = entry.path().lexically_relative(root);
                const auto bytes = journal_test::readFile(entry.path());
                state.names.emplace(name, state.inodes.size());
                state.inodes.push_back({bytes, bytes, {}});
            }
            state.durableNames = state.names;
        }
        Path relative(const Path &path) const { return path.lexically_relative(root); }
        void cut(const std::string &operation, const Path &path, const char *side)
        {
            operations.insert(operation);
            cuts.push_back({state, operation + " " + relative(path).string() + " " + side});
        }
        void create(const Path &path)
        {
            state.names.emplace(relative(path), state.inodes.size());
            state.inodes.emplace_back();
        }
        void write(const Path &path, const void *buffer, std::size_t size, std::size_t offset)
        {
            auto &node = state.inodes.at(state.names.at(relative(path)));
            node.cache.resize(std::max(node.cache.size(), offset + size));
            const auto *bytes = static_cast<const std::byte *>(buffer);
            std::copy_n(bytes, size, node.cache.begin() + static_cast<std::ptrdiff_t>(offset));
            for (auto sector = offset / Sector; sector <= (offset + size - 1) / Sector; ++sector)
                node.dirty.insert(sector);
        }
        void resize(const Path &path, std::size_t size)
        {
            state.inodes.at(state.names.at(relative(path))).cache.resize(size);
        }
        void sync(const Path &path)
        {
            const auto name = relative(path);
            if (const auto it = state.names.find(name); it != state.names.end())
            {
                auto &node = state.inodes.at(it->second);
                node.durable = node.cache;
                node.dirty.clear();
            }
            else
            {
                std::erase_if(state.durableNames, [&](const auto &entry) { return entry.first.parent_path() == name; });
                for (const auto &[entry, inode] : state.names)
                    if (entry.parent_path() == name) state.durableNames[entry] = inode;
            }
        }
    };
    Model *active = nullptr;
    struct Observe
    {
        explicit Observe(Model &model) { active = &model; }
        ~Observe() { active = nullptr; }
    };

    Image files(const Path &root)
    {
        Image result;
        for (const auto &entry : std::filesystem::directory_iterator(root / "tables"))
            result.emplace(entry.path().filename(), journal_test::readFile(entry.path()));
        return result;
    }

    // Rebuild the cache from a chosen physical image, with all old descriptors
    // closed. std::ofstream is deliberately outside the wrapped backend.
    void install(const Path &root, const Image &image)
    {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "tables");
        std::filesystem::create_directories(root / "journal");
        for (const auto &[name, bytes] : image)
        {
            std::ofstream file(root / name, std::ios::binary);
            file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            require(file.good(), "Cannot install crash image");
        }
    }

    Image crashImage(const State &state, unsigned namesMode, unsigned dataMode)
    {
        auto names = state.durableNames;
        // Independent early persistence of tables/ and journal/ entries. A
        // rename within journal/ is atomic, but need not survive without sync.
        for (const auto &[directory, bit] : {std::pair{Path{"tables"}, 1u}, std::pair{Path{"journal"}, 2u}})
        {
            if (!(namesMode & bit)) continue;
            std::erase_if(names, [&](const auto &entry) { return entry.first.parent_path() == directory; });
            for (const auto &[name, id] : state.names)
                if (name.parent_path() == directory) names[name] = id;
        }
        Image result;
        for (const auto &[name, id] : names)
        {
            const auto &node = state.inodes.at(id);
            auto bytes = dataMode == 1 ? node.cache : node.durable;
            if (dataMode == 2)
            {
                bytes.resize(node.cache.size());
                for (std::size_t offset = 0; offset < bytes.size(); ++offset)
                    if ((offset / Sector) % 2 == 0) bytes[offset] = node.cache[offset];
            }
            result.emplace(name, std::move(bytes));
        }
        return result;
    }

    std::size_t checked = 0;
    std::size_t torn = 0;
    void verify(const Path &root, const Image &image, const Image &before, const Image &after,
                const std::string &context)
    {
        install(root, image);
        try
        {
            auto engine = StorageEngine::open(root);
            const auto result = files(root);
            require(result == before || result == after, "Mixed/partial table state");
            require(!std::filesystem::exists(root / "journal/journal.log") &&
                    !std::filesystem::exists(root / "journal/journal.pending"), "Recovery left a journal");
            // New work must be permitted after restart, including cache access.
            require(engine.getTableHeader("items").totalRowCount != 0, "Invalid recovered header");
            engine.beginStatement();
            engine.commitStatement();
        }
        catch (const std::exception &error)
        {
            throw std::runtime_error(context + ": " + error.what());
        }
        ++checked;
    }

    void exercise(const Model &model, const Path &root, const Image &before, const Image &after,
                  const std::string &scenario)
    {
        std::size_t index = 0;
        for (const auto &cut : model.cuts)
        {
            const auto context = scenario + " cut " + std::to_string(index++) + " " + cut.event;
            for (unsigned namesMode = 0; namesMode < 4; ++namesMode)
            for (unsigned dataMode = 0; dataMode < 3; ++dataMode)
            {
                const auto image = crashImage(cut.state, namesMode, dataMode);
                verify(root, image, before, after, context);
                if (dataMode == 2) continue;
                for (const auto &[name, bytes] : image)
                {
                    (void)bytes;
                    // Names may refer to an unlinked inode retained by the
                    // durable directory. Locate it in the selected namespace.
                    const auto &names = ((name.parent_path() == "tables" && (namesMode & 1)) ||
                                         (name.parent_path() == "journal" && (namesMode & 2)))
                        ? cut.state.names : cut.state.durableNames;
                    const auto &node = cut.state.inodes.at(names.at(name));
                    for (const auto sector : node.dirty)
                    for (unsigned pattern = 0; pattern < 3; ++pattern)
                    {
                        auto damaged = image;
                        auto &data = damaged.at(name);
                        const auto start = sector * Sector;
                        data.resize(std::max(data.size(), std::min(start + Sector, node.cache.size())));
                        for (auto offset = start; offset < std::min(start + Sector, data.size()); ++offset)
                        {
                            // Garbage anywhere in the affected sector (even
                            // outside the requested bytes), plus old/new tears
                            // progressing from either end of the sector.
                            if (pattern == 0) data[offset] = std::byte{0xa5};
                            else
                            {
                                const bool useNew = pattern == 1 ? offset - start < Sector / 2 : offset - start >= Sector / 2;
                                const auto &source = useNew ? node.cache : node.durable;
                                data[offset] = offset < source.size() ? source[offset] : std::byte{0};
                            }
                        }
                        verify(root, damaged, before, after, context + " torn " + name.string() + " sector " + std::to_string(sector));
                        ++torn;
                    }
                }
            }
        }
    }

    void seed(const Path &root)
    {
        auto database = Database::create(root, "test");
        for (const std::string table : {"items", "other"})
        {
            database.executeSql("CREATE TABLE " + table + " (id INT, value VARCHAR(2000));");
            database.insertRows(table, {Row{{std::int32_t{1}, std::string(1500, 'a')}}});
        }
    }

    void modify(StorageEngine &engine)
    {
        engine.beginStatement();
        auto first = engine.openTable("items");
        auto second = engine.openTable("other");
        first.insertRows({.tableName = "items", .rows = {
            Row{{std::int32_t{2}, std::string(1500, 'b')}}, Row{{std::int32_t{3}, std::string(1500, 'c')}}}});
        second.insertRows({.tableName = "other", .rows = {Row{{std::int32_t{2}, std::string(1500, 'd')}}}});
        // Append new before-images after earlier database writes and rotate both
        // boundary slots several times within the same statement.
        { auto added = engine.createTable("added", "test", {}, {}); }
        first.insertRows({.tableName = "items", .rows = {Row{{std::int32_t{4}, std::string(1500, 'e')}}}});
    }

    void testCommitAndRecoveryPowerLoss()
    {
        TestDirectory directory{"db-power-loss"};
        const auto root = directory.path / "db";
        const auto replay = directory.path / "replay";
        seed(root);
        const auto before = files(root);
        Model commit{root};
        {
            auto engine = StorageEngine::open(root);
            Observe observe{commit};
            modify(engine);
            engine.commitStatement();
        }
        const auto after = files(root);
        require(before != after && after.contains("added.table"), "Commit fixture did not change all files");
        exercise(commit, replay, before, after, "commit");
        verify(replay, crashImage(commit.state, 0, 0), after, after, "acknowledged commit");
        for (const auto &operation : {"create", "write", "sync", "rename", "unlink"})
            require(commit.operations.contains(operation), "Missing commit boundary");

        // Generate durable, unfinished work, then trace startup recovery itself.
        // Each interrupted recovery image is reopened and must finish undo.
        {
            auto engine = StorageEngine::open(root);
            engine.beginStatement();
            auto table = engine.openTable("items");
            table.insertRows({.tableName = "items", .rows = {Row{{std::int32_t{5}, std::string(1500, 'f')}}}});
            { auto added = engine.createTable("abandoned", "test", {}, {}); }
        }
        Model recovery{root};
        {
            Observe observe{recovery};
            auto engine = StorageEngine::open(root);
        }
        require(files(root) == after, "Uninterrupted recovery failed");
        exercise(recovery, replay, after, after, "recovery");
        for (const auto &operation : {"write", "sync", "resize", "unlink"})
            require(recovery.operations.contains(operation), "Missing recovery boundary");
        std::cout << "Verified " << checked << " power-loss images (" << torn << " torn sectors), "
                  << commit.cuts.size() + recovery.cuts.size() << " before/after persistence cuts\n";
    }
}

extern "C"
{
    long __real_syscall(long, ...);
    ssize_t __real_pwrite(int, const void *, size_t, off_t);
    int __real_ftruncate(int, off_t);
    int __real_unlinkat(int, const char *, int);
    int __real_renameat(int, const char *, int, const char *);

    long __wrap_syscall(long number, ...)
    {
        if (number != SYS_openat2) { errno = ENOSYS; return -1; }
        va_list args;
        va_start(args, number);
        const int parent = va_arg(args, int);
        const char *path = va_arg(args, const char *);
        const auto *how = va_arg(args, struct open_how *);
        const auto size = va_arg(args, size_t);
        va_end(args);
        const bool tracked = active && (how->flags & O_CREAT);
        const auto name = tracked ? descriptorPath(parent) / path : Path{};
        if (tracked) active->cut("create", name, "before");
        const auto result = __real_syscall(number, parent, path, how, size);
        if (tracked && result >= 0)
        {
            active->create(name);
            active->cut("create", name, "after");
        }
        return result;
    }
    ssize_t __wrap_pwrite(int fd, const void *bytes, size_t count, off_t offset)
    {
        const auto path = active ? descriptorPath(fd) : Path{};
        if (active) active->cut("write", path, "before");
        const auto result = __real_pwrite(fd, bytes, count, offset);
        if (active && result > 0)
        {
            active->write(path, bytes, static_cast<std::size_t>(result), static_cast<std::size_t>(offset));
            active->cut("write", path, "after");
        }
        return result;
    }
    int __wrap_fsync(int fd)
    {
        // Setup and verification also use these files only as a cache. Host
        // disk flushes cannot improve the model and make the crash matrix
        // unnecessarily dependent on the speed of the test machine's disk.
        if (!active) return 0;
        const auto path = descriptorPath(fd);
        active->cut("sync", path, "before");
        active->sync(path);
        active->cut("sync", path, "after");
        return 0; // Only the model's durable state is the simulated disk.
    }
    int __wrap_ftruncate(int fd, off_t size)
    {
        const auto path = active ? descriptorPath(fd) : Path{};
        if (active) active->cut("resize", path, "before");
        const auto result = __real_ftruncate(fd, size);
        if (active && result == 0)
        {
            active->resize(path, static_cast<std::size_t>(size));
            active->cut("resize", path, "after");
        }
        return result;
    }
    int __wrap_unlinkat(int fd, const char *name, int flags)
    {
        const auto path = active ? descriptorPath(fd) / name : Path{};
        if (active) active->cut("unlink", path, "before");
        const auto result = __real_unlinkat(fd, name, flags);
        if (active && result == 0)
        {
            active->state.names.erase(active->relative(path));
            active->cut("unlink", path, "after");
        }
        return result;
    }
    int __wrap_renameat(int from, const char *source, int to, const char *target)
    {
        const auto path = active ? descriptorPath(from) / source : Path{};
        if (active) active->cut("rename", path, "before");
        const auto result = __real_renameat(from, source, to, target);
        if (active && result == 0)
        {
            auto node = active->state.names.extract(active->relative(path));
            node.key() = active->relative(descriptorPath(to) / target);
            active->state.names.insert(std::move(node));
            active->cut("rename", path, "after");
        }
        return result;
    }
}

int main()
{
    try { testCommitAndRecoveryPowerLoss(); }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

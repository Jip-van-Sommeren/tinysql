#include "db_linux_file.h"
#include "db_journal_file.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace
{
    struct Faults
    {
        unsigned openInterrupts = 0, readInterrupts = 0, writeInterrupts = 0;
        unsigned statInterrupts = 0, resizeInterrupts = 0, syncInterrupts = 0;
        int openError = 0, readError = 0, writeError = 0;
        int statError = 0, resizeError = 0, syncError = 0;
        bool zeroWrite = false, closeInterrupt = false;
        std::size_t maxTransfer = std::numeric_limits<std::size_t>::max();
        std::optional<std::size_t> writeBudget;
        unsigned reads = 0, writes = 0, closes = 0;
        int lastOpened = -1;
    } faults;

    bool interrupted(unsigned &remaining)
    {
        if (remaining == 0)
            return false;
        --remaining;
        errno = EINTR;
        return true;
    }

    void require(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    template <typename Error, typename Function>
    void requireThrows(Function function, const std::string &message = "")
    {
        try
        {
            function();
        }
        catch (const Error &error)
        {
            require(std::string{error.what()}.find(message) != std::string::npos,
                    "Unexpected error: " + std::string{error.what()});
            return;
        }
        throw std::runtime_error("Expected exception: " + message);
    }

    struct TestDirectory
    {
        std::filesystem::path path;
        TestDirectory()
        {
            auto pattern = (std::filesystem::temp_directory_path() / "db-linux-file-test-XXXXXX").string();
            const char *created = ::mkdtemp(pattern.data());
            if (!created)
                throw std::runtime_error("Cannot create test directory");
            path = created;
        }
        ~TestDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };
}

// Link-time wrappers inject short transfers and EINTR without changing the
// production API. Real calls still operate on isolated temporary files.
extern "C"
{
    int __real_open(const char *, int, ...);
    ssize_t __real_pread(int, void *, size_t, off_t);
    ssize_t __real_pwrite(int, const void *, size_t, off_t);
    int __real_fstat(int, struct stat *);
    int __real_ftruncate(int, off_t);
    int __real_fsync(int);
    int __real_close(int);

    int __wrap_open(const char *path, int flags, ...)
    {
        if (interrupted(faults.openInterrupts))
            return -1;
        if (faults.openError)
        {
            errno = faults.openError;
            return -1;
        }
        int result;
        if (flags & O_CREAT)
        {
            va_list args;
            va_start(args, flags);
            const int mode = va_arg(args, int);
            va_end(args);
            result = __real_open(path, flags, mode);
        }
        else
        {
            result = __real_open(path, flags);
        }
        if (result != -1)
            faults.lastOpened = result;
        return result;
    }

    ssize_t __wrap_pread(int fd, void *data, size_t count, off_t offset)
    {
        ++faults.reads;
        if (interrupted(faults.readInterrupts))
            return -1;
        if (faults.readError)
        {
            errno = faults.readError;
            return -1;
        }
        return __real_pread(fd, data, std::min(count, faults.maxTransfer), offset);
    }

    ssize_t __wrap_pwrite(int fd, const void *data, size_t count, off_t offset)
    {
        ++faults.writes;
        if (interrupted(faults.writeInterrupts))
            return -1;
        if (faults.writeError)
        {
            errno = faults.writeError;
            return -1;
        }
        if (faults.zeroWrite)
            return 0;
        count = std::min(count, faults.maxTransfer);
        if (faults.writeBudget)
        {
            if (*faults.writeBudget == 0)
            {
                errno = ENOSPC;
                return -1;
            }
            count = std::min(count, *faults.writeBudget);
        }
        const auto result = __real_pwrite(fd, data, count, offset);
        if (result > 0 && faults.writeBudget)
            *faults.writeBudget -= static_cast<std::size_t>(result);
        return result;
    }

    int __wrap_fstat(int fd, struct stat *info)
    {
        if (interrupted(faults.statInterrupts))
            return -1;
        if (faults.statError)
        {
            errno = faults.statError;
            return -1;
        }
        return __real_fstat(fd, info);
    }

    int __wrap_ftruncate(int fd, off_t size)
    {
        if (interrupted(faults.resizeInterrupts))
            return -1;
        if (faults.resizeError)
        {
            errno = faults.resizeError;
            return -1;
        }
        return __real_ftruncate(fd, size);
    }

    int __wrap_fsync(int fd)
    {
        if (interrupted(faults.syncInterrupts))
            return -1;
        if (faults.syncError)
        {
            errno = faults.syncError;
            return -1;
        }
        return __real_fsync(fd);
    }

    int __wrap_close(int fd)
    {
        ++faults.closes;
        const int result = __real_close(fd);
        if (faults.closeInterrupt)
        {
            errno = EINTR;
            return -1;
        }
        return result;
    }
}

namespace
{
    using Mode = LinuxFile::OpenMode;

    void testOpeningAndOwnership(const std::filesystem::path &directory)
    {
        const auto path = directory / "ownership";
        requireThrows<std::system_error>([&] { LinuxFile missing(path); }, "open");
        require(!std::filesystem::exists(path), "OpenExisting created a file");
        faults.openInterrupts = 2;
        LinuxFile first(path, Mode::CreateNew);
        const int firstFd = faults.lastOpened;
        require((::fcntl(firstFd, F_GETFD) & FD_CLOEXEC) != 0, "Missing close-on-exec");
        const std::array<std::byte, 3> bytes{std::byte{1}, std::byte{2}, std::byte{3}};
        first.writeAllAt(0, bytes);
        requireThrows<std::system_error>([&] { LinuxFile duplicate(path, Mode::CreateNew); }, "open");
        require(first.size() == 3, "Exclusive creation overwrote a file");

        LinuxFile moved(std::move(first));
        requireThrows<std::system_error>([&] { first.size(); }, "closed");
        require(moved.size() == 3, "Move construction lost the descriptor");
        unsigned closesBeforeDestruction = 0;
        {
            LinuxFile target(directory / "target", Mode::CreateNew);
            const int oldFd = faults.lastOpened;
            target = std::move(moved);
            require(::fcntl(oldFd, F_GETFD) == -1 && errno == EBADF,
                    "Move assignment leaked the destination descriptor");
            requireThrows<std::system_error>([&] { moved.size(); }, "closed");
            auto &self = target;
            target = std::move(self);
            require(target.size() == 3, "Self-move lost the descriptor");
            closesBeforeDestruction = faults.closes;
            faults.closeInterrupt = true;
            // The following scope destruction must make exactly one close call.
        }
        faults.closeInterrupt = false;
        require(faults.closes == closesBeforeDestruction + 1, "close was retried after EINTR");
        require(::fcntl(firstFd, F_GETFD) == -1 && errno == EBADF, "Destructor leaked descriptor");

        LinuxFile reopened(path);
        require(reopened.size() == 3, "Reopening lost data");
        LinuxFile readOnly(path, Mode::ReadOnly);
        std::array<std::byte, 3> restored{};
        readOnly.readExactAt(0, restored);
        require(restored == bytes, "Read-only open could not read");
        requireThrows<std::system_error>([&] { readOnly.writeAllAt(0, bytes); }, "pwrite");
        const auto closesBefore = faults.closes;
        requireThrows<std::runtime_error>([&] { LinuxFile invalid(directory, Mode::ReadOnly); }, "regular file");
        require(faults.closes == closesBefore + 1, "Constructor failure leaked descriptor");
        faults.openError = EACCES;
        requireThrows<std::system_error>([&] { LinuxFile denied(path); }, path.string());
        faults.openError = 0;
    }

    void testTransfersAndErrors(const std::filesystem::path &directory)
    {
        LinuxFile file(directory / "transfers", Mode::CreateNew);
        const std::array<std::byte, 5> bytes{
            std::byte{5}, std::byte{4}, std::byte{3}, std::byte{2}, std::byte{1}};
        faults.maxTransfer = 2;
        faults.writeInterrupts = 2;
        file.writeAllAt(7, bytes);
        require(faults.writes >= 5 && file.size() == 12, "Partial write was not completed");
        std::array<std::byte, 5> restored{};
        faults.readInterrupts = 2;
        file.readExactAt(7, restored);
        require(restored == bytes && faults.reads >= 5, "Partial read was not completed");
        requireThrows<std::runtime_error>([&] { file.readExactAt(10, restored); }, "EOF");
        requireThrows<std::runtime_error>([&] { file.readExactAt(12, restored); }, "EOF");

        const unsigned readCalls = faults.reads, writeCalls = faults.writes;
        file.readExactAt(0, {});
        file.writeAllAt(0, {});
        require(faults.reads == readCalls && faults.writes == writeCalls, "Empty I/O made a syscall");
        const auto maxOffset = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        requireThrows<std::out_of_range>([&] { file.readExactAt(maxOffset, restored); });
        requireThrows<std::out_of_range>([&] { file.writeAllAt(maxOffset, bytes); });
        requireThrows<std::out_of_range>([&] { file.resize(std::numeric_limits<std::uint64_t>::max()); });
        require(file.size() == 12, "Rejected resize changed the file");

        faults.readError = EIO;
        requireThrows<std::system_error>([&] { file.readExactAt(0, restored); }, "pread");
        faults.readError = 0;
        faults.writeError = ENOSPC;
        requireThrows<std::system_error>([&] { file.writeAllAt(0, bytes); }, "pwrite");
        faults.writeError = 0;
        faults.zeroWrite = true;
        requireThrows<std::system_error>([&] { file.writeAllAt(0, bytes); }, "no progress");
        faults.zeroWrite = false;
        faults.statError = EIO;
        requireThrows<std::system_error>([&] { file.size(); }, "fstat");
        faults.statError = 0;
        faults.statInterrupts = 2;
        require(file.size() == 12, "fstat EINTR retry failed");
        faults.resizeError = EIO;
        requireThrows<std::system_error>([&] { file.resize(0); }, "ftruncate");
        faults.resizeError = 0;
        faults.resizeInterrupts = 2;
        file.resize(2);
        require(file.size() == 2, "Resize did not shrink file");
        file.resize(8);
        std::array<std::byte, 6> zeros{};
        std::array<std::byte, 6> extension{};
        extension.fill(std::byte{0xff});
        file.readExactAt(2, extension);
        require(extension == zeros, "File extension was not zero-filled");
        faults.syncError = EIO;
        requireThrows<std::system_error>([&] { file.sync(); }, "fsync");
        faults.syncError = 0;
        faults.syncInterrupts = 2;
        file.sync();
    }

    void testFailedJournalAppend(const std::filesystem::path &directory)
    {
        const auto path = directory / "partial-journal";
        JournalFile journal(path);
        const PageBeforeImage record{.relativeFilePath = "tables/a.table", .pageId = 1, .originalPage = {}};
        faults.writeBudget = 10;
        requireThrows<std::system_error>([&] { journal.writePage(record); }, "pwrite");
        faults.writeBudget.reset();
        require(std::filesystem::file_size(path) == 10, "Partial-write injection did not run");
        requireThrows<std::runtime_error>([&] { journal.writePage(record); }, "previously failed");
        requireThrows<std::runtime_error>([&] { journal.sync(); }, "previously failed");
        requireThrows<std::runtime_error>([&] { journal.readNext(); }, "Truncated");
        require(std::filesystem::file_size(path) == 10, "Failed journal was appended again");
    }
}

int main()
{
    static_assert(!std::is_copy_constructible_v<LinuxFile>);
    static_assert(!std::is_copy_assignable_v<LinuxFile>);
    static_assert(std::is_nothrow_move_constructible_v<LinuxFile>);
    static_assert(std::is_nothrow_move_assignable_v<LinuxFile>);
    TestDirectory directory;
    testOpeningAndOwnership(directory.path);
    faults = {};
    testTransfersAndErrors(directory.path);
    faults = {};
    testFailedJournalAppend(directory.path);
}

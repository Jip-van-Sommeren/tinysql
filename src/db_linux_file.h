#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

// Owns a Linux descriptor. Writes reach the OS cache; only sync() requests
// durable file contents. Creating/removing directory entries needs a separate
// directory sync as part of the eventual recovery protocol.
class LinuxFile
{
public:
    enum class OpenMode
    {
        OpenExisting,
        CreateNew,
        ReadOnly
    };

    explicit LinuxFile(
        const std::filesystem::path &path,
        OpenMode mode = OpenMode::OpenExisting);
    ~LinuxFile() noexcept;

    LinuxFile(const LinuxFile &) = delete;
    LinuxFile &operator=(const LinuxFile &) = delete;
    LinuxFile(LinuxFile &&other) noexcept;
    LinuxFile &operator=(LinuxFile &&other) noexcept;

    // On an I/O error a prefix may already have been transferred. Neither
    // method provides rollback; successful return guarantees the full transfer.
    void readExactAt(std::uint64_t offset, std::span<std::byte> destination);
    void writeAllAt(std::uint64_t offset, std::span<const std::byte> source);
    std::uint64_t size() const;
    void resize(std::uint64_t size);
    void sync();

private:
    std::filesystem::path path_;
    int fd_ = -1;

    void closeDescriptor() noexcept;
    void ensureOpen() const;
    void validateRange(std::uint64_t offset, std::size_t count) const;
    [[noreturn]] void throwError(const char *operation, int error) const;
};

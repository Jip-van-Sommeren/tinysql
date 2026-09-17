#include "db_linux_file.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

LinuxFile::LinuxFile(const std::filesystem::path &path, OpenMode mode)
    : path_(path)
{
    int flags = O_CLOEXEC;
    switch (mode)
    {
    case OpenMode::OpenExisting:
        flags |= O_RDWR;
        break;
    case OpenMode::CreateNew:
        flags |= O_RDWR | O_CREAT | O_EXCL;
        break;

    case OpenMode::ReadOnly:
        flags |= O_RDONLY;
        break;

    default:
        throw std::invalid_argument("Invalid LinuxFile open mode");
    }

    do
    {
        fd_ = ::open(path_.c_str(), flags, 0600);
    } while (fd_ == -1 && errno == EINTR);
    if (fd_ == -1)
    {
        throwError("open", errno);
    }

    try
    {
        (void)size(); // Reject directories and other non-regular files.
    }
    catch (...)
    {
        closeDescriptor();
        throw;
    }
}

LinuxFile::~LinuxFile() noexcept
{
    closeDescriptor();
}

LinuxFile::LinuxFile(LinuxFile &&other) noexcept
    : path_(std::move(other.path_)), fd_(std::exchange(other.fd_, -1))
{
}

LinuxFile &LinuxFile::operator=(LinuxFile &&other) noexcept
{
    if (this != &other)
    {
        closeDescriptor();
        path_ = std::move(other.path_);
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void LinuxFile::closeDescriptor() noexcept
{
    if (fd_ != -1)
    {
        const int savedError = errno;
        // Linux releases the descriptor even when close reports EINTR or a
        // delayed I/O error. Retrying could close an unrelated reused descriptor.
        // Call sync() explicitly to report persistence failures before teardown.
        (void)::close(std::exchange(fd_, -1));
        errno = savedError;
    }
}

void LinuxFile::ensureOpen() const
{
    if (fd_ == -1)
    {
        throwError("use of closed file", EBADF);
    }
}

void LinuxFile::validateRange(std::uint64_t offset, std::size_t count) const
{
    const auto maxOffset = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (offset > maxOffset || count > maxOffset - offset)
    {
        throw std::out_of_range("File byte range exceeds off_t: " + path_.string());
    }
}

void LinuxFile::readExactAt(std::uint64_t offset, std::span<std::byte> destination)
{
    ensureOpen();
    validateRange(offset, destination.size_bytes());
    std::size_t done = 0;
    while (done < destination.size())
    {
        const auto count = std::min(destination.size() - done,
                                    static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t received = ::pread(
            fd_, destination.data() + done, count, static_cast<off_t>(offset + done));
        if (received == -1)
        {
            const int error = errno;
            if (error == EINTR)
            {
                continue;
            }
            throwError("pread", error);
        }
        if (received == 0)
        {
            throw std::runtime_error("Unexpected EOF during read: " + path_.string());
        }
        done += static_cast<std::size_t>(received);
    }
}

void LinuxFile::writeAllAt(std::uint64_t offset, std::span<const std::byte> source)
{
    ensureOpen();
    validateRange(offset, source.size_bytes());
    std::size_t done = 0;
    while (done < source.size())
    {
        const auto count = std::min(source.size() - done,
                                    static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t written = ::pwrite(
            fd_, source.data() + done, count, static_cast<off_t>(offset + done));
        if (written == -1)
        {
            const int error = errno;
            if (error == EINTR)
            {
                continue;
            }
            throwError("pwrite", error);
        }
        if (written == 0)
        {
            throwError("pwrite made no progress", EIO);
        }
        done += static_cast<std::size_t>(written);
    }
}

std::uint64_t LinuxFile::size() const
{
    ensureOpen();
    struct stat info{};
    while (::fstat(fd_, &info) == -1)
    {
        const int error = errno;
        if (error != EINTR)
        {
            throwError("fstat", error);
        }
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0)
    {
        throw std::runtime_error("Expected a regular file with a valid size: " + path_.string());
    }
    return static_cast<std::uint64_t>(info.st_size);
}

void LinuxFile::resize(std::uint64_t size)
{
    ensureOpen();
    validateRange(size, 0);
    while (::ftruncate(fd_, static_cast<off_t>(size)) == -1)
    {
        const int error = errno;
        if (error != EINTR)
        {
            throwError("ftruncate", error);
        }
    }
}

void LinuxFile::sync()
{
    ensureOpen();
    while (::fsync(fd_) == -1)
    {
        const int error = errno;
        if (error != EINTR)
        {
            throwError("fsync", error);
        }
    }
}

void LinuxFile::throwError(const char *operation, int error) const
{
    throw std::system_error(error, std::generic_category(),
                            std::string{operation} + " failed: " + path_.string());
}

LinuxDirectory::LinuxDirectory(const std::filesystem::path &path)
    : path_(path)
{

    do
    {
        fd_ = ::open(
            path.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (fd_ == -1 && errno == EINTR);

    if (fd_ == -1)
    {
        const int error = errno;
        throwError("open directory", error);
    }
}

LinuxDirectory::~LinuxDirectory() noexcept
{
    closeDescriptor();
}

LinuxDirectory::LinuxDirectory(LinuxDirectory &&other) noexcept
    : path_(std::move(other.path_)), fd_(std::exchange(other.fd_, -1))
{
}

LinuxDirectory &LinuxDirectory::operator=(LinuxDirectory &&other) noexcept
{
    if (this != &other)
    {
        closeDescriptor();
        path_ = std::move(other.path_);
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void LinuxDirectory::closeDescriptor() noexcept
{
    if (fd_ != -1)
    {
        const int savedError = errno;
        // Linux releases the descriptor even when close reports EINTR or a
        // delayed I/O error. Retrying could close an unrelated reused descriptor.
        // Call sync() explicitly to report persistence failures before teardown.
        (void)::close(std::exchange(fd_, -1));
        errno = savedError;
    }
}

void LinuxDirectory::ensureOpen() const
{
    if (fd_ == -1)
    {
        throwError("use of closed file", EBADF);
    }
}

void LinuxDirectory::sync()
{
    ensureOpen();
    while (::fsync(fd_) == -1)
    {
        const int error = errno;
        if (error != EINTR)
        {
            throwError("fsync", error);
        }
    }
}

void LinuxDirectory::throwError(const char *operation, int error) const
{
    throw std::system_error(error, std::generic_category(),
                            std::string{operation} + " failed: " + path_.string());
}
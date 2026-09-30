#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

// Own a unique parent directory; Database::create() creates the child "db".
// Destroy this fixture after the database and its borrowed handles.
struct TestDirectory
{
    std::filesystem::path path;

    explicit TestDirectory(const std::string &name)
    {
        auto pattern = (std::filesystem::temp_directory_path() /
                        (name + "-XXXXXX")).string();
        const char *created = ::mkdtemp(pattern.data());
        if (!created)
            throw std::runtime_error("Cannot create test directory");
        path = created;
    }

    TestDirectory(const TestDirectory &) = delete;
    TestDirectory &operator=(const TestDirectory &) = delete;

    ~TestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

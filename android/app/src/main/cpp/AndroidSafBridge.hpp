#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Accessors for the read-only Storage Access Framework tree retained by the
// launcher. Returned file descriptors are owned by the caller.
struct AndroidSafEntry
{
    std::string name;
    std::int64_t size = -1;
    bool directory = false;
};

bool AndroidSafListDirectory(const char* relativePath, std::vector<AndroidSafEntry>& entries);
int AndroidSafOpenFile(const char* relativePath);

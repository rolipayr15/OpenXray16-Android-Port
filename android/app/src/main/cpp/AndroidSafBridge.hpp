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

// One entry of a complete tree walk; path is relative to the tree root and
// '/'-separated, modified is in seconds since the epoch.
struct AndroidSafTreeEntry
{
    std::string path;
    std::int64_t size = -1;
    std::int64_t modified = 0;
    bool directory = false;
};

bool AndroidSafListDirectory(const char* relativePath, std::vector<AndroidSafEntry>& entries);
// Walks the whole tree in Java (one provider query per directory) and lets
// later AndroidSafOpenFile calls reuse the cached document ids.
bool AndroidSafWalkTree(std::vector<AndroidSafTreeEntry>& entries);
int AndroidSafOpenFile(const char* relativePath);

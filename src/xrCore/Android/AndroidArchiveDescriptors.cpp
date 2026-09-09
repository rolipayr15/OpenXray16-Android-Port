#include "stdafx.h"

#include "AndroidArchiveDescriptors.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unistd.h>

namespace
{
std::mutex descriptorMutex;
std::unordered_map<std::string, int> archiveDescriptors;

std::string NormalizePath(pcstr path)
{
    std::string normalized = path ? path : "";
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    // Android exposes the same app directory as both /data/user/0 and
    // /data/data. CLocatorAPI canonicalizes the parent while the Java host
    // supplies getFilesDir(), so normalize the parent without following the
    // archive's intentionally descriptor-backed symlink.
    const size_t separator = normalized.find_last_of('/');
    if (separator != std::string::npos)
    {
        char canonicalParent[PATH_MAX];
        const std::string parent = normalized.substr(0, separator);
        if (realpath(parent.c_str(), canonicalParent))
            normalized = std::string(canonicalParent) + normalized.substr(separator);
    }

    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return normalized;
}
} // namespace

bool xrRegisterAndroidArchiveDescriptor(pcstr path, int descriptor)
{
    if (!path || !path[0] || descriptor < 0)
        return false;

    std::lock_guard<std::mutex> lock(descriptorMutex);
    return archiveDescriptors.emplace(NormalizePath(path), descriptor).second;
}

int xrDuplicateAndroidArchiveDescriptor(pcstr path)
{
    std::lock_guard<std::mutex> lock(descriptorMutex);
    const auto descriptor = archiveDescriptors.find(NormalizePath(path));
    return descriptor == archiveDescriptors.end() ? -1 : dup(descriptor->second);
}

void xrClearAndroidArchiveDescriptors()
{
    std::lock_guard<std::mutex> lock(descriptorMutex);
    archiveDescriptors.clear();
}

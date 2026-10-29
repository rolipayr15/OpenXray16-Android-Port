#include "stdafx.h"

#include "AndroidArchiveDescriptors.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <fcntl.h>
#include <limits.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
std::mutex descriptorMutex;
std::unordered_map<std::string, int> archiveDescriptors;

struct VirtualFile
{
    std::string sourceId;
    s64 size = 0;
    s64 modified = 0;
};

std::mutex virtualMutex;
xrAndroidVirtualFileOpener virtualOpener = nullptr;
// Pairs of (alternate spelling, registered spelling) for host-owned roots.
std::vector<std::pair<std::string, std::string>> virtualRoots;
std::unordered_map<std::string, VirtualFile> virtualFiles;
// Directory -> lower-case child names (files and subdirectories).
std::unordered_map<std::string, std::unordered_set<std::string>> virtualDirectories;

std::string LowerSlashes(pcstr path)
{
    std::string normalized = path ? path : "";
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    while (normalized.size() > 1 && normalized.back() == '/')
        normalized.pop_back();
    return normalized;
}

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
    return LowerSlashes(normalized.c_str());
}

// Caller holds virtualMutex.
std::string NormalizeVirtualPath(pcstr path)
{
    std::string normalized = LowerSlashes(path);
    for (const auto& [alternate, registered] : virtualRoots)
    {
        if (normalized.size() >= alternate.size() &&
            normalized.compare(0, alternate.size(), alternate) == 0 &&
            (normalized.size() == alternate.size() || normalized[alternate.size()] == '/'))
        {
            return registered + normalized.substr(alternate.size());
        }
    }
    return normalized;
}

bool IsUnderVirtualRoot(const std::string& normalized)
{
    for (const auto& root : virtualRoots)
    {
        const std::string& registered = root.second;
        if (normalized.size() > registered.size() &&
            normalized.compare(0, registered.size(), registered) == 0 &&
            normalized[registered.size()] == '/')
        {
            return true;
        }
    }
    return false;
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

void xrSetAndroidVirtualFileOpener(xrAndroidVirtualFileOpener opener)
{
    std::lock_guard<std::mutex> lock(virtualMutex);
    virtualOpener = opener;
}

void xrAddAndroidVirtualRoot(pcstr root)
{
    if (!root || !root[0])
        return;

    const std::string registered = LowerSlashes(root);
    char canonical[PATH_MAX];
    const std::string alternate = realpath(root, canonical) ? LowerSlashes(canonical) : registered;

    std::lock_guard<std::mutex> lock(virtualMutex);
    for (const auto& existing : virtualRoots)
    {
        if (existing.second == registered)
            return;
    }
    if (alternate != registered)
        virtualRoots.emplace_back(alternate, registered);
    virtualRoots.emplace_back(registered, registered);
}

bool xrRegisterAndroidVirtualFile(pcstr path, pcstr sourceId, s64 size, s64 modified)
{
    if (!path || !path[0] || !sourceId)
        return false;

    std::lock_guard<std::mutex> lock(virtualMutex);
    const std::string normalized = NormalizeVirtualPath(path);
    if (!IsUnderVirtualRoot(normalized))
        return false;

    virtualFiles[normalized] = VirtualFile{ sourceId, size, modified };

    // Record every ancestor below the root so directory scans can descend
    // into folders that exist only in the SAF tree.
    std::string child = normalized;
    while (true)
    {
        const size_t slash = child.find_last_of('/');
        if (slash == std::string::npos || slash == 0)
            break;
        const std::string parent = child.substr(0, slash);
        auto& children = virtualDirectories[parent];
        const bool inserted = children.insert(child.substr(slash + 1)).second;
        if (!inserted || !IsUnderVirtualRoot(parent))
            break;
        child = parent;
    }
    return true;
}

void xrClearAndroidVirtualFiles()
{
    std::lock_guard<std::mutex> lock(virtualMutex);
    virtualFiles.clear();
    virtualDirectories.clear();
}

size_t xrAndroidVirtualFileCount()
{
    std::lock_guard<std::mutex> lock(virtualMutex);
    return virtualFiles.size();
}

bool xrIsAndroidVirtualPath(pcstr path)
{
    std::lock_guard<std::mutex> lock(virtualMutex);
    if (virtualFiles.empty())
        return false;
    const std::string normalized = NormalizeVirtualPath(path);
    return virtualFiles.count(normalized) != 0 || virtualDirectories.count(normalized) != 0;
}

bool xrStatAndroidVirtualPath(pcstr path, struct stat& status)
{
    std::lock_guard<std::mutex> lock(virtualMutex);
    if (virtualFiles.empty())
        return false;

    const std::string normalized = NormalizeVirtualPath(path);
    std::memset(&status, 0, sizeof(status));
    const auto file = virtualFiles.find(normalized);
    if (file != virtualFiles.end())
    {
        status.st_mode = S_IFREG | S_IRUSR;
        status.st_size = static_cast<off_t>(std::max<s64>(file->second.size, 0));
        status.st_mtime = static_cast<time_t>(file->second.modified);
        status.st_atime = status.st_mtime;
        status.st_ctime = status.st_mtime;
        return true;
    }
    if (virtualDirectories.count(normalized) != 0)
    {
        status.st_mode = S_IFDIR | S_IRUSR | S_IXUSR;
        return true;
    }
    return false;
}

void xrListAndroidVirtualDirectory(pcstr directory, std::vector<std::string>& children)
{
    children.clear();
    std::lock_guard<std::mutex> lock(virtualMutex);
    if (virtualDirectories.empty())
        return;

    std::string requested = LowerSlashes(directory);
    const std::string normalized = NormalizeVirtualPath(directory);
    const auto entry = virtualDirectories.find(normalized);
    if (entry == virtualDirectories.end())
        return;

    // Return full paths spelled like the caller's directory so the locator
    // keeps registering names below the root it is scanning.
    children.reserve(entry->second.size());
    for (const std::string& name : entry->second)
        children.push_back(requested + "/" + name);
}

bool xrAndroidResolvePathCase(pcstr path, std::string& resolved)
{
    resolved = path ? path : "";
    if (resolved.empty() || access(resolved.c_str(), F_OK) == 0)
        return !resolved.empty();

    // Walk the components, keeping every one that exists as spelled and
    // looking up the others case-insensitively in their parent directory.
    std::string current = resolved[0] == '/' ? "/" : "";
    size_t start = resolved[0] == '/' ? 1 : 0;
    while (start <= resolved.size())
    {
        const size_t slash = resolved.find('/', start);
        const size_t end = slash == std::string::npos ? resolved.size() : slash;
        const std::string component = resolved.substr(start, end - start);
        if (!component.empty())
        {
            std::string candidate = current + component;
            if (access(candidate.c_str(), F_OK) != 0)
            {
                bool found = false;
                if (DIR* directory = opendir(current.empty() ? "." : current.c_str()))
                {
                    while (dirent* entry = readdir(directory))
                    {
                        if (strcasecmp(entry->d_name, component.c_str()) == 0)
                        {
                            candidate = current + entry->d_name;
                            found = true;
                            break;
                        }
                    }
                    closedir(directory);
                }
                if (!found)
                    return false;
            }
            current = std::move(candidate);
        }
        if (slash == std::string::npos)
            break;
        current += '/';
        start = slash + 1;
    }
    resolved = std::move(current);
    return true;
}

int xrAndroidUnlink(const char* path)
{
    const int result = ::unlink(path);
    if (result == 0 || errno != ENOENT)
        return result;

    std::string resolved;
    return xrAndroidResolvePathCase(path, resolved) ? ::unlink(resolved.c_str()) : result;
}

int xrAndroidOpenReadOnly(pcstr path)
{
    const int descriptor = ::open(path, O_RDONLY);
    if (descriptor != -1)
        return descriptor;
    if (errno != ENOENT)
        return -1;

    std::string sourceId;
    xrAndroidVirtualFileOpener opener = nullptr;
    {
        std::lock_guard<std::mutex> lock(virtualMutex);
        if (!virtualFiles.empty() && virtualOpener)
        {
            const auto file = virtualFiles.find(NormalizeVirtualPath(path));
            if (file != virtualFiles.end())
            {
                sourceId = file->second.sourceId;
                opener = virtualOpener;
            }
        }
    }
    if (opener)
    {
        // The opener calls into Java; never hold the registry lock across it.
        return opener(sourceId.c_str());
    }

    std::string resolved;
    return xrAndroidResolvePathCase(path, resolved) ? ::open(resolved.c_str(), O_RDONLY) : -1;
}

#pragma once

#include "xrCore/xrCore.h"

#include <string>
#include <vector>

struct stat;

// The descriptor remains owned by the Android host. xrCore duplicates it each
// time an archive instance is opened and closes only its own duplicate.
XRCORE_API bool xrRegisterAndroidArchiveDescriptor(pcstr path, int descriptor);
XRCORE_API int xrDuplicateAndroidArchiveDescriptor(pcstr path);
XRCORE_API void xrClearAndroidArchiveDescriptors();

// Loose files that live in a read-only Storage Access Framework tree have no
// filesystem path the engine could open. The host registers each one under the
// private mirror path the locator scans, together with an opaque source id; the
// opener turns that id into a fresh read-only descriptor on demand. Lookups are
// case-insensitive, matching the Windows semantics game data was authored for.
using xrAndroidVirtualFileOpener = int (*)(pcstr sourceId);

XRCORE_API void xrSetAndroidVirtualFileOpener(xrAndroidVirtualFileOpener opener);
// Declares a directory prefix the host owns. The locator may see it through its
// canonical (realpath) spelling, e.g. /data/data instead of /data/user/0.
XRCORE_API void xrAddAndroidVirtualRoot(pcstr root);
XRCORE_API bool xrRegisterAndroidVirtualFile(pcstr path, pcstr sourceId, s64 size, s64 modified);
XRCORE_API void xrClearAndroidVirtualFiles();
XRCORE_API size_t xrAndroidVirtualFileCount();

// Engine-side helpers. They are no-ops for paths the host did not register.
XRCORE_API bool xrIsAndroidVirtualPath(pcstr path);
XRCORE_API bool xrStatAndroidVirtualPath(pcstr path, struct stat& status);
XRCORE_API void xrListAndroidVirtualDirectory(pcstr directory, std::vector<std::string>& children);
// open(path, O_RDONLY) that also resolves registered virtual files and, for
// real files, the on-disk letter case of a lower-cased locator path.
XRCORE_API int xrAndroidOpenReadOnly(pcstr path);
XRCORE_API bool xrAndroidResolvePathCase(pcstr path, std::string& resolved);

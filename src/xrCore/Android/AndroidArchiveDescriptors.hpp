#pragma once

#include "xrCore/xrCore.h"

// The descriptor remains owned by the Android host. xrCore duplicates it each
// time an archive instance is opened and closes only its own duplicate.
XRCORE_API bool xrRegisterAndroidArchiveDescriptor(pcstr path, int descriptor);
XRCORE_API int xrDuplicateAndroidArchiveDescriptor(pcstr path);
XRCORE_API void xrClearAndroidArchiveDescriptors();

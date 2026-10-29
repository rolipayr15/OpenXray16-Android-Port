#pragma once

#include <android/log.h>

// Android uses Bionic and the POSIX compatibility definitions shared with the
// Linux port, but it remains a distinct platform at every engine call site.
#define XR_PLATFORM_CUSTOM_OUTPUT_DEBUG_STRING
#define XR_PLATFORM_CUSTOM_FS_CASE
#include "Common/PlatformLinux.inl"
#undef XR_PLATFORM_CUSTOM_OUTPUT_DEBUG_STRING

inline void OutputDebugString(const char* string)
{
    if (string)
        __android_log_write(ANDROID_LOG_DEBUG, "OpenXRay", string);
}

// Game data, mods and saves come from Windows installations and reference
// files with arbitrary letter case. Android's shared storage is
// case-insensitive while app-private storage is not, so the locator keys its
// registry in lower case (as on Windows) and the low-level file operations
// resolve the on-disk spelling when the lower-case path does not exist.
#define xr_fs_strlwr(str) xr_strlwr(str)

// Implemented in xrCore/Android/AndroidArchiveDescriptors.cpp.
int xrAndroidUnlink(const char* path);

inline int xr_unlink(const char* path)
{
    char* conv_fn = strdup(path);
    convert_path_separators(conv_fn);
    const int result = xrAndroidUnlink(conv_fn);
    free(conv_fn);
    return result;
}

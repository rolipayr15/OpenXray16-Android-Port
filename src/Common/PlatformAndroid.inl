#pragma once

#include <android/log.h>

// Android uses Bionic and the POSIX compatibility definitions shared with the
// Linux port, but it remains a distinct platform at every engine call site.
#define XR_PLATFORM_CUSTOM_OUTPUT_DEBUG_STRING
#include "Common/PlatformLinux.inl"
#undef XR_PLATFORM_CUSTOM_OUTPUT_DEBUG_STRING

inline void OutputDebugString(const char* string)
{
    if (string)
        __android_log_write(ANDROID_LOG_DEBUG, "OpenXRay", string);
}

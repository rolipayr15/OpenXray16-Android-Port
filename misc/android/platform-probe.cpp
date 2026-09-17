#include "Common/Platform.hpp"

#if !defined(XR_PLATFORM_ANDROID)
#error XR_PLATFORM_ANDROID must be defined for an Android NDK build
#endif

#if !defined(XR_PLATFORM_POSIX)
#error Android must use the POSIX engine contract
#endif

#if defined(XR_PLATFORM_LINUX)
#error Android must not be classified as desktop Linux
#endif

#if defined(__aarch64__) && !defined(XR_ARCHITECTURE_ARM64)
#error AArch64 must select XR_ARCHITECTURE_ARM64
#endif

#if defined(__arm__) && !defined(XR_ARCHITECTURE_ARM)
#error ARMv7 must select XR_ARCHITECTURE_ARM
#endif

int openxray_android_platform_probe()
{
    return sizeof(void*) == 4 || sizeof(void*) == 8 ? 0 : 1;
}

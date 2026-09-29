#pragma once

#include "Common/Platform.hpp"

#ifdef XRAY_STATIC_BUILD
#   define XRGAMESPY_API
#else
#   ifdef XRGAMESPY_EXPORTS
#      define XRGAMESPY_API XR_EXPORT
#   else
#      define XRGAMESPY_API XR_IMPORT
#   endif
#endif

#pragma once

#include "xrEngine/Render.h"

// On Android the shared render layer serves two renderers. The Vulkan host
// bridge (android/app/src/main/cpp) reuses this layer's model and light
// loading but owns every GPU resource itself, so the GL-side shader, buffer
// and skinning setup must be skipped. The GL renderer running on OpenGL ES
// needs exactly that setup. GEnv.Render is assigned in SetupEnv, before any
// model or light is loaded, so it identifies the active backend.
inline bool UsesAndroidHostRenderer()
{
#if defined(XR_PLATFORM_ANDROID)
    return GEnv.Render && GEnv.Render->GetBackendAPI() == IRender::BackendAPI::Vulkan;
#else
    return false;
#endif
}

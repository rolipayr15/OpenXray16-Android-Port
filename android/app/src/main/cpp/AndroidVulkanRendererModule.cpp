#include "xrEngine/stdafx.h"

#include "AndroidVulkanRendererModule.hpp"

#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"

#include <android/log.h>

namespace
{
constexpr pcstr VulkanMode = "renderer_vulkan";
constexpr int VulkanModeId = 7;
constexpr pcstr LogTag = "OpenXRay";
}

AndroidVulkanRendererModule::AndroidVulkanRendererModule(const char* appFilesPath)
    : renderDevice(appFilesPath)
{
    depthOfField.set(-1.25f, 1.4f, 600.0f);
}

AndroidVulkanRendererModule::~AndroidVulkanRendererModule()
{
    ClearEnv();
}

const xr_vector<std::pair<pcstr, int>>& AndroidVulkanRendererModule::ObtainSupportedModes()
{
    if (modes.empty())
        modes.emplace_back(VulkanMode, VulkanModeId);
    return modes;
}

bool AndroidVulkanRendererModule::CheckGameRequirements()
{
    // AndroidVulkanRenderer performs the authoritative instance/device/surface
    // capability checks when CRenderDevice creates the SDL window.
    return true;
}

void AndroidVulkanRendererModule::SetupEnv(pcstr mode)
{
    R_ASSERT2(mode && xr_strcmp(mode, VulkanMode) == 0, "Unexpected Android renderer mode");

    renderServices.Attach(renderDevice);

    // ShoC reads this renderer-owned console value while constructing
    // CGamePersistent. Keep the compatibility setting available until the
    // native Vulkan post-processing implementation owns it directly.
    if (!depthOfFieldCommand)
    {
        Fvector minimum;
        Fvector maximum;
        minimum.set(-10000.0f, -10000.0f, -10000.0f);
        maximum.set(10000.0f, 10000.0f, 10000.0f);
        depthOfFieldCommand = std::make_unique<CCC_Vector3>(
            "r2_dof", &depthOfField, minimum, maximum);
        Console->AddCommand(depthOfFieldCommand.get());
    }

    __android_log_write(
        ANDROID_LOG_INFO, LogTag, "Android Vulkan renderer module selected by xrEngine");
}

void AndroidVulkanRendererModule::ClearEnv()
{
    depthOfFieldCommand.reset();
    renderServices.Detach();
    modes.clear();
}

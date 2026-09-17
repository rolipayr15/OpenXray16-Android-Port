#pragma once

#include "AndroidVulkanRenderDevice.hpp"
#include "AndroidVulkanRenderServices.hpp"
#include "xrEngine/EngineAPI.h"

#include <memory>

class CCC_Vector3;

// Registers the Android-native Vulkan backend with the regular xrEngine
// renderer-selection lifecycle. The device and all GEnv services outlive
// CApplication and are detached only after the engine has shut down.
class AndroidVulkanRendererModule final : public RendererModule
{
public:
    explicit AndroidVulkanRendererModule(const char* appFilesPath);
    ~AndroidVulkanRendererModule() override;

    const xr_vector<std::pair<pcstr, int>>& ObtainSupportedModes() override;
    bool CheckGameRequirements() override;
    void SetupEnv(pcstr mode) override;
    void ClearEnv() override;

private:
    AndroidVulkanRenderDevice renderDevice;
    AndroidVulkanRenderServices renderServices;
    xr_vector<std::pair<pcstr, int>> modes;
    Fvector depthOfField;
    std::unique_ptr<CCC_Vector3> depthOfFieldCommand;
};

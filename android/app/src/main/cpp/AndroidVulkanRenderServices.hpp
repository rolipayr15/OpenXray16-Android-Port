#pragma once

#include "AndroidVulkanBootstrap.hpp"

#include <cstdint>
#include <memory>

class AndroidVulkanRenderDevice;

// Shared DDS/VFS texture path used by both the UI and the world renderer.
// The cache is keyed by the engine texture name, so a material referenced by
// several level visuals is decoded and uploaded only once.
bool AndroidLoadVulkanTexture(AndroidVulkanRenderDevice& renderDevice, const char* textureName,
    AndroidVulkanUiTexture& texture, std::uint32_t& width, std::uint32_t& height);

// Owns the non-device portions of GEnv's renderer contract. The initial
// implementations are lifecycle-safe bootstrap services; they let the engine
// construct its UI/environment objects while Vulkan resource rendering is
// brought up incrementally.
class AndroidVulkanRenderServices final
{
public:
    AndroidVulkanRenderServices();
    ~AndroidVulkanRenderServices();

    AndroidVulkanRenderServices(const AndroidVulkanRenderServices&) = delete;
    AndroidVulkanRenderServices& operator=(const AndroidVulkanRenderServices&) = delete;

    void Attach(AndroidVulkanRenderDevice& renderDevice);
    void Detach();

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation;
};

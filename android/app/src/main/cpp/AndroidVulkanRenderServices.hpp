#pragma once

#include <memory>

class AndroidVulkanRenderDevice;

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

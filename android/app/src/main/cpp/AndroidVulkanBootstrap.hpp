#pragma once

#include <cstdint>
#include <memory>

struct SDL_Window;

// Owns the native Vulkan bootstrap for the complete SDL window lifetime.  The
// interface is deliberately renderer-agnostic so the Android host can keep its
// event/frame loop stable while the bootstrap implementation is replaced by
// the real OpenXRay Vulkan backend incrementally.
class AndroidVulkanRenderer final
{
public:
    AndroidVulkanRenderer();
    ~AndroidVulkanRenderer();

    AndroidVulkanRenderer(const AndroidVulkanRenderer&) = delete;
    AndroidVulkanRenderer& operator=(const AndroidVulkanRenderer&) = delete;

    bool Initialize(SDL_Window* window, const char* appFilesPath);
    bool DrawFrame(std::uint64_t frameIndex, float elapsedSeconds);
    void Shutdown();

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation;
};

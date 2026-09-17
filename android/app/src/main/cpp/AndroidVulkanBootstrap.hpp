#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

struct SDL_Window;

enum class AndroidVulkanUiPrimitive : std::uint8_t
{
    TriangleList,
    TriangleStrip,
    LineList,
    LineStrip
};

struct AndroidVulkanUiVertex
{
    float x;
    float y;
    float z;
    std::uint32_t color;
    float u;
    float v;
};

struct AndroidVulkanUiScissor
{
    bool enabled{};
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

using AndroidVulkanUiTexture = std::uint64_t;

struct AndroidVulkanWorldVertex
{
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    float u;
    float v;
};

using AndroidVulkanWorldMesh = std::uint64_t;

enum class AndroidVulkanUiTextureMode : std::uint32_t
{
    Normal,
    HudFont,
    Font2
};

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
    AndroidVulkanUiTexture CreateUiTexture(const char* name, std::uint32_t width,
        std::uint32_t height, const std::uint8_t* rgbaPixels, std::size_t byteCount);
    bool UpdateUiTexture(AndroidVulkanUiTexture texture,
        const std::uint8_t* rgbaPixels, std::size_t byteCount);
    AndroidVulkanWorldMesh CreateWorldMesh(const AndroidVulkanWorldVertex* vertices,
        std::size_t vertexCount, const std::uint16_t* indices, std::size_t indexCount);
    bool UpdateWorldMesh(AndroidVulkanWorldMesh mesh, const AndroidVulkanWorldVertex* vertices,
        std::size_t vertexCount);
    void DestroyWorldMesh(AndroidVulkanWorldMesh mesh);
    void BeginUiFrame();
    void BeginWorldFrame();
    void SubmitUiBatch(const AndroidVulkanUiVertex* vertices, std::size_t vertexCount,
        AndroidVulkanUiPrimitive primitive, const AndroidVulkanUiScissor& scissor,
        AndroidVulkanUiTexture texture,
        AndroidVulkanUiTextureMode textureMode = AndroidVulkanUiTextureMode::Normal);
    void SubmitWorldMesh(AndroidVulkanWorldMesh mesh, std::uint32_t firstIndex,
        std::uint32_t indexCount, std::int32_t vertexOffset,
        const float* worldViewProjection, AndroidVulkanUiTexture texture);
    bool DrawFrame(std::uint64_t frameIndex, float elapsedSeconds);
    void GetSurfaceSize(std::uint32_t& width, std::uint32_t& height) const;
    void Shutdown();

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation;
};

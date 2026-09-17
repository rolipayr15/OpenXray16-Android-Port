#include "AndroidVulkanBootstrap.hpp"
#include "AndroidBootstrapFragSpv.hpp"
#include "AndroidBootstrapVertSpv.hpp"
#include "AndroidWorldFragSpv.hpp"
#include "AndroidWorldVertSpv.hpp"

#include <SDL.h>
#include <vulkan/vulkan.h>
#include <SDL_vulkan.h>

#include <algorithm>
#include <android/log.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

namespace
{
constexpr const char* LogTag = "OpenXRay";

struct UiBatch
{
    std::uint32_t firstVertex{};
    std::uint32_t vertexCount{};
    AndroidVulkanUiPrimitive primitive{};
    AndroidVulkanUiScissor scissor{};
    AndroidVulkanUiTexture texture{};
    AndroidVulkanUiTextureMode textureMode{AndroidVulkanUiTextureMode::Normal};
};

struct UiPushConstants
{
    float width{};
    float height{};
    std::uint32_t textureMode{};
    std::uint32_t reserved{};
};

struct UiTexture
{
    std::string name;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    std::uint32_t width{};
    std::uint32_t height{};
};

struct WorldGpuMesh
{
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    std::uint32_t vertexCount{};
    std::uint32_t indexCount{};
    bool alive{};
};

struct WorldDraw
{
    AndroidVulkanWorldMesh mesh{};
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
    std::int32_t vertexOffset{};
    AndroidVulkanUiTexture texture{};
    float worldViewProjection[16]{};
};

struct WorldPushConstants
{
    float worldViewProjection[16];
};

void LogVulkanError(const char* operation, VkResult result)
{
    __android_log_print(ANDROID_LOG_ERROR, LogTag, "%s failed with VkResult %d", operation, result);
}

bool HasExtension(VkPhysicalDevice device, const char* required)
{
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS)
        return false;

    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()) != VK_SUCCESS)
        return false;

    return std::any_of(extensions.begin(), extensions.end(), [required](const VkExtensionProperties& extension)
    {
        return std::strcmp(extension.extensionName, required) == 0;
    });
}

std::vector<VkExtensionProperties> EnumerateExtensions(VkPhysicalDevice device)
{
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS)
        return {};

    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()) != VK_SUCCESS)
        return {};
    extensions.resize(count);
    return extensions;
}

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* required)
{
    return std::any_of(extensions.begin(), extensions.end(), [required](const VkExtensionProperties& extension)
    {
        return std::strcmp(extension.extensionName, required) == 0;
    });
}

bool VulkanVersionAtLeast(uint32_t version, uint32_t major, uint32_t minor)
{
    return VK_VERSION_MAJOR(version) > major ||
        (VK_VERSION_MAJOR(version) == major && VK_VERSION_MINOR(version) >= minor);
}

void LogZinkGl41Preflight(VkPhysicalDevice device, const VkPhysicalDeviceProperties& properties,
    const VkPhysicalDeviceFeatures& features)
{
    // OpenXRay's current GL renderer requests an OpenGL 4.1 core context. This
    // deliberately conservative check follows Mesa 24.3's documented Zink
    // requirements. Passing it is not enough to enable Zink: extension feature
    // structs and the actual Mesa/EGL integration must still initialize first.
    std::vector<std::string> missing;
    const auto require = [&missing](bool available, const char* name)
    {
        if (!available)
            missing.emplace_back(name);
    };

    require(features.logicOp, "logicOp");
    require(features.fillModeNonSolid, "fillModeNonSolid");
    require(features.alphaToOne, "alphaToOne");
    require(features.shaderClipDistance, "shaderClipDistance");
    require(features.independentBlend, "independentBlend");
    require(features.depthClamp, "depthClamp");
    require(features.geometryShader, "geometryShader");
    require(features.shaderTessellationAndGeometryPointSize, "tessellationGeometryPointSize");
    require(features.dualSrcBlend, "dualSrcBlend");
    require(features.sampleRateShading, "sampleRateShading");
    require(features.tessellationShader, "tessellationShader");
    require(features.imageCubeArray, "imageCubeArray");
    require(features.multiViewport, "multiViewport");

    const VkPhysicalDeviceLimits& limits = properties.limits;
    require(limits.maxPerStageDescriptorSamplers >= 16, "descriptorSamplers>=16");
    require(limits.maxImageDimension1D >= 16384, "image1D>=16384");
    require(limits.maxImageDimension2D >= 16384, "image2D>=16384");
    require(limits.maxImageDimension3D >= 2048, "image3D>=2048");
    require(limits.maxImageDimensionCube >= 16384, "imageCube>=16384");
    require(limits.maxImageArrayLayers >= 2048, "arrayLayers>=2048");
    require(limits.maxViewports >= 16, "maxViewports>=16");

    const std::vector<VkExtensionProperties> extensions = EnumerateExtensions(device);
    const auto requireExtension = [&extensions, &missing, apiVersion = properties.apiVersion](
        const char* name, uint32_t promotedMajor = 0, uint32_t promotedMinor = 0)
    {
        const bool promoted = promotedMajor != 0 && VulkanVersionAtLeast(apiVersion, promotedMajor, promotedMinor);
        if (!promoted && !HasExtension(extensions, name))
            missing.emplace_back(name);
    };

    requireExtension("VK_KHR_maintenance1", 1, 1);
    requireExtension("VK_KHR_create_renderpass2", 1, 2);
    requireExtension("VK_KHR_imageless_framebuffer", 1, 2);
    requireExtension("VK_KHR_timeline_semaphore", 1, 2);
    requireExtension("VK_EXT_custom_border_color");
    requireExtension("VK_EXT_provoking_vertex");
    requireExtension("VK_EXT_line_rasterization");
    requireExtension("VK_KHR_swapchain_mutable_format");
    requireExtension("VK_EXT_border_color_swizzle");
    requireExtension("VK_KHR_descriptor_update_template", 1, 1);
    requireExtension("VK_KHR_external_memory", 1, 1);
    if (!VulkanVersionAtLeast(properties.apiVersion, 1, 2))
        requireExtension("VK_EXT_scalar_block_layout");
    requireExtension("VK_EXT_transform_feedback");
    requireExtension("VK_EXT_conditional_rendering");
    requireExtension("VK_EXT_depth_clip_enable");
    requireExtension("VK_EXT_vertex_attribute_divisor");
    requireExtension("VK_KHR_maintenance2", 1, 1);

    const VkFormat zinkGl40Formats[] = {
        VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_UINT
    };
    for (const VkFormat format : zinkGl40Formats)
    {
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(device, format, &formatProperties);
        if ((formatProperties.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) == 0)
        {
            missing.emplace_back("RGB32 uniform texel buffer formats");
            break;
        }
    }

    if (missing.empty())
    {
        __android_log_write(ANDROID_LOG_INFO, LogTag,
            "Renderer path: native Vulkan bootstrap; Zink OpenGL 4.1 core preflight passed, "
            "but Mesa/EGL and extended-feature validation are still required");
        return;
    }

    std::string summary;
    constexpr size_t MaxReportedMissing = 12;
    for (size_t index = 0; index < std::min(missing.size(), MaxReportedMissing); ++index)
    {
        if (!summary.empty())
            summary += ", ";
        summary += missing[index];
    }
    if (missing.size() > MaxReportedMissing)
        summary += ", ...";

    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "Renderer path: native Vulkan (Zink OpenGL 4.1 preflight unavailable: %zu missing: %s)",
        missing.size(), summary.c_str());
}

struct QueueFamilies
{
    uint32_t graphics = UINT32_MAX;
    uint32_t present = UINT32_MAX;

    bool Complete() const { return graphics != UINT32_MAX && present != UINT32_MAX; }
};

QueueFamilies FindQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> properties(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, properties.data());

    QueueFamilies families;
    for (uint32_t index = 0; index < count; ++index)
    {
        if ((properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && families.graphics == UINT32_MAX)
            families.graphics = index;

        VkBool32 supportsPresent = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(device, index, surface, &supportsPresent) == VK_SUCCESS &&
            supportsPresent == VK_TRUE && families.present == UINT32_MAX)
        {
            families.present = index;
        }
    }
    return families;
}

bool ReadFile(const std::string& path, std::vector<unsigned char>& contents)
{
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return errno == ENOENT;

    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        std::fclose(file);
        return false;
    }
    const long length = std::ftell(file);
    if (length < 0 || std::fseek(file, 0, SEEK_SET) != 0)
    {
        std::fclose(file);
        return false;
    }

    contents.resize(static_cast<size_t>(length));
    const bool read = contents.empty() || std::fread(contents.data(), 1, contents.size(), file) == contents.size();
    return std::fclose(file) == 0 && read;
}

bool WriteFile(const std::string& path, const std::vector<unsigned char>& contents)
{
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;

    const bool written = contents.empty() ||
        std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    return std::fclose(file) == 0 && written;
}

class VulkanBootstrap
{
public:
    ~VulkanBootstrap() { Shutdown(); }

    bool Initialize(SDL_Window* sdlWindow, const char* appFilesPath)
    {
        if (initialized)
            return false;

        window = sdlWindow;
        if (!window || !appFilesPath || !appFilesPath[0])
            return false;

        cacheDirectory = std::string(appFilesPath) + "/renderer-cache";
        cachePath = cacheDirectory + "/vulkan-pipeline-cache.bin";
        if (mkdir(cacheDirectory.c_str(), 0700) != 0 && errno != EEXIST)
        {
            __android_log_print(ANDROID_LOG_ERROR, LogTag,
                "Could not create private renderer cache directory: %s", std::strerror(errno));
            return false;
        }

        initialized = CreateInstance() && CreateSurface() && SelectPhysicalDevice() && CreateDevice() &&
            CreatePipelineCache() && CreateSwapchain() && CreateGraphicsPipeline() && CreateCommands() &&
            CreateFallbackTexture() && SavePipelineCache();
        return initialized;
    }

    AndroidVulkanUiTexture CreateUiTexture(const char* name, std::uint32_t width,
        std::uint32_t height, const std::uint8_t* rgbaPixels, std::size_t byteCount)
    {
        if (!name || !name[0] || !rgbaPixels || width == 0 || height == 0 ||
            byteCount != static_cast<std::size_t>(width) * height * 4 ||
            device == VK_NULL_HANDLE || commandPool == VK_NULL_HANDLE)
        {
            return 0;
        }

        const auto existing = uiTextureByName.find(name);
        if (existing != uiTextureByName.end())
            return existing->second;

        UiTexture uploaded;
        if (!UploadUiTexture(width, height, rgbaPixels, byteCount, uploaded))
            return 0;
        uploaded.name = name;

        uiTextures.push_back(uploaded);
        const AndroidVulkanUiTexture handle = static_cast<AndroidVulkanUiTexture>(uiTextures.size());
        uiTextureByName.emplace(name, handle);
        return handle;
    }

    bool UpdateUiTexture(AndroidVulkanUiTexture handle,
        const std::uint8_t* rgbaPixels, std::size_t byteCount)
    {
        if (handle == 0 || handle > uiTextures.size() || !rgbaPixels ||
            device == VK_NULL_HANDLE || commandPool == VK_NULL_HANDLE)
        {
            return false;
        }

        UiTexture& texture = uiTextures[static_cast<std::size_t>(handle - 1)];
        const std::size_t requiredBytes =
            static_cast<std::size_t>(texture.width) * texture.height * 4;
        if (byteCount != requiredBytes)
            return false;

        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        VkCommandBuffer uploadCommand = VK_NULL_HANDLE;
        const auto cleanup = [&]()
        {
            if (uploadCommand != VK_NULL_HANDLE)
                vkFreeCommandBuffers(device, commandPool, 1, &uploadCommand);
            if (stagingBuffer != VK_NULL_HANDLE)
                vkDestroyBuffer(device, stagingBuffer, nullptr);
            if (stagingMemory != VK_NULL_HANDLE)
                vkFreeMemory(device, stagingMemory, nullptr);
        };

        if (!CreateBuffer(byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingMemory))
        {
            cleanup();
            return false;
        }

        void* mapped = nullptr;
        VkResult result = vkMapMemory(device, stagingMemory, 0, byteCount, 0, &mapped);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkMapMemory(dynamic UI texture)", result);
            cleanup();
            return false;
        }
        std::memcpy(mapped, rgbaPixels, byteCount);
        vkUnmapMemory(device, stagingMemory);

        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = commandPool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(device, &allocation, &uploadCommand);
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (result == VK_SUCCESS)
            result = vkBeginCommandBuffer(uploadCommand, &beginInfo);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("begin dynamic UI texture upload", result);
            cleanup();
            return false;
        }

        VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = texture.image;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(uploadCommand, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {texture.width, texture.height, 1};
        vkCmdCopyBufferToImage(uploadCommand, stagingBuffer, texture.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        VkImageMemoryBarrier toShader{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.image = texture.image;
        toShader.subresourceRange = toTransfer.subresourceRange;
        vkCmdPipelineBarrier(uploadCommand, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShader);

        result = vkEndCommandBuffer(uploadCommand);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &uploadCommand;
        if (result == VK_SUCCESS)
            result = vkQueueSubmit(graphicsQueue, 1, &submit, VK_NULL_HANDLE);
        if (result == VK_SUCCESS)
            result = vkQueueWaitIdle(graphicsQueue);
        if (result != VK_SUCCESS)
            LogVulkanError("submit dynamic UI texture upload", result);
        cleanup();
        return result == VK_SUCCESS;
    }

    AndroidVulkanWorldMesh CreateWorldMesh(const AndroidVulkanWorldVertex* vertices,
        std::size_t vertexCount, const std::uint16_t* indices, std::size_t indexCount)
    {
        if (!vertices || !indices || vertexCount == 0 || indexCount == 0 ||
            vertexCount > UINT32_MAX || indexCount > UINT32_MAX || device == VK_NULL_HANDLE)
        {
            return 0;
        }

        WorldGpuMesh mesh;
        const VkDeviceSize vertexBytes = vertexCount * sizeof(AndroidVulkanWorldVertex);
        const VkDeviceSize indexBytes = indexCount * sizeof(std::uint16_t);
        const VkMemoryPropertyFlags hostMemory =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (!CreateBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hostMemory,
                mesh.vertexBuffer, mesh.vertexMemory) ||
            !CreateBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, hostMemory,
                mesh.indexBuffer, mesh.indexMemory))
        {
            DestroyWorldMeshResources(mesh);
            return 0;
        }

        void* mapped = nullptr;
        VkResult result = vkMapMemory(device, mesh.vertexMemory, 0, vertexBytes, 0, &mapped);
        if (result == VK_SUCCESS)
        {
            std::memcpy(mapped, vertices, static_cast<std::size_t>(vertexBytes));
            vkUnmapMemory(device, mesh.vertexMemory);
            mapped = nullptr;
            result = vkMapMemory(device, mesh.indexMemory, 0, indexBytes, 0, &mapped);
        }
        if (result == VK_SUCCESS)
        {
            std::memcpy(mapped, indices, static_cast<std::size_t>(indexBytes));
            vkUnmapMemory(device, mesh.indexMemory);
        }
        if (result != VK_SUCCESS)
        {
            LogVulkanError("upload world mesh", result);
            DestroyWorldMeshResources(mesh);
            return 0;
        }

        mesh.vertexCount = static_cast<std::uint32_t>(vertexCount);
        mesh.indexCount = static_cast<std::uint32_t>(indexCount);
        mesh.alive = true;
        worldMeshes.push_back(mesh);
        return static_cast<AndroidVulkanWorldMesh>(worldMeshes.size());
    }

    bool UpdateWorldMesh(AndroidVulkanWorldMesh handle, const AndroidVulkanWorldVertex* vertices,
        std::size_t vertexCount)
    {
        if (handle == 0 || handle > worldMeshes.size() || !vertices || device == VK_NULL_HANDLE)
            return false;
        WorldGpuMesh& mesh = worldMeshes[static_cast<std::size_t>(handle - 1)];
        if (!mesh.alive || vertexCount != mesh.vertexCount)
            return false;

        const VkDeviceSize vertexBytes = vertexCount * sizeof(AndroidVulkanWorldVertex);
        void* mapped = nullptr;
        const VkResult result = vkMapMemory(device, mesh.vertexMemory, 0, vertexBytes, 0, &mapped);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("update world mesh", result);
            return false;
        }
        std::memcpy(mapped, vertices, static_cast<std::size_t>(vertexBytes));
        vkUnmapMemory(device, mesh.vertexMemory);
        return true;
    }

    void DestroyWorldMesh(AndroidVulkanWorldMesh handle)
    {
        if (handle == 0 || handle > worldMeshes.size() || device == VK_NULL_HANDLE)
            return;
        WorldGpuMesh& mesh = worldMeshes[static_cast<std::size_t>(handle - 1)];
        if (!mesh.alive)
            return;
        vkDeviceWaitIdle(device);
        DestroyWorldMeshResources(mesh);
    }

    void BeginWorldFrame()
    {
        worldDraws.clear();
    }

    void SubmitWorldMesh(AndroidVulkanWorldMesh handle, std::uint32_t firstIndex,
        std::uint32_t indexCount, std::int32_t vertexOffset, const float* worldViewProjection,
        AndroidVulkanUiTexture texture)
    {
        if (handle == 0 || handle > worldMeshes.size() || !worldViewProjection)
            return;
        const WorldGpuMesh& mesh = worldMeshes[static_cast<std::size_t>(handle - 1)];
        if (!mesh.alive || firstIndex >= mesh.indexCount || indexCount == 0 ||
            indexCount > mesh.indexCount - firstIndex)
        {
            return;
        }

        WorldDraw draw;
        draw.mesh = handle;
        draw.firstIndex = firstIndex;
        draw.indexCount = indexCount;
        draw.vertexOffset = vertexOffset;
        draw.texture = texture;
        std::memcpy(draw.worldViewProjection, worldViewProjection, sizeof(draw.worldViewProjection));
        worldDraws.push_back(draw);
    }

    bool DrawFrame(std::uint64_t frameIndex, float elapsedSeconds)
    {
        return initialized && PresentFrame(frameIndex, elapsedSeconds);
    }

    void GetSurfaceSize(std::uint32_t& width, std::uint32_t& height) const
    {
        width = extent.width;
        height = extent.height;
    }

    void BeginUiFrame()
    {
        uiVertices.clear();
        uiBatches.clear();
    }

    void SubmitUiBatch(const AndroidVulkanUiVertex* vertices, std::size_t vertexCount,
        AndroidVulkanUiPrimitive primitive, const AndroidVulkanUiScissor& scissor,
        AndroidVulkanUiTexture texture, AndroidVulkanUiTextureMode textureMode)
    {
        if (!vertices || vertexCount == 0 || vertexCount > UINT32_MAX)
            return;

        UiBatch batch;
        batch.firstVertex = static_cast<std::uint32_t>(uiVertices.size());
        batch.vertexCount = static_cast<std::uint32_t>(vertexCount);
        batch.primitive = primitive;
        batch.scissor = scissor;
        batch.texture = texture;
        batch.textureMode = textureMode;
        uiVertices.insert(uiVertices.end(), vertices, vertices + vertexCount);
        uiBatches.push_back(batch);
    }

    void Shutdown()
    {
        if (initialized)
            SavePipelineCache();
        Destroy();
        initialized = false;
    }

private:
    bool CreateInstance()
    {
        unsigned extensionCount = 0;
        if (SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, nullptr) != SDL_TRUE)
        {
            __android_log_print(ANDROID_LOG_ERROR, LogTag,
                "SDL Vulkan extension query failed: %s", SDL_GetError());
            return false;
        }
        std::vector<const char*> extensions(extensionCount);
        if (SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, extensions.data()) != SDL_TRUE)
        {
            __android_log_print(ANDROID_LOG_ERROR, LogTag,
                "SDL Vulkan extension list failed: %s", SDL_GetError());
            return false;
        }

        uint32_t loaderVersion = VK_API_VERSION_1_0;
        const auto enumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
        if (enumerateInstanceVersion)
            enumerateInstanceVersion(&loaderVersion);

        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "OpenXRay Android";
        application.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        application.pEngineName = "OpenXRay";
        application.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        application.apiVersion = std::min(loaderVersion, VK_API_VERSION_1_1);

        VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        createInfo.pApplicationInfo = &application;
        createInfo.enabledExtensionCount = extensionCount;
        createInfo.ppEnabledExtensionNames = extensions.data();
        const VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateInstance", result);
            return false;
        }

        __android_log_print(ANDROID_LOG_INFO, LogTag, "Vulkan loader API %u.%u.%u initialized",
            VK_VERSION_MAJOR(loaderVersion), VK_VERSION_MINOR(loaderVersion), VK_VERSION_PATCH(loaderVersion));
        return true;
    }

    bool CreateSurface()
    {
        if (SDL_Vulkan_CreateSurface(window, instance, &surface) != SDL_TRUE)
        {
            __android_log_print(ANDROID_LOG_ERROR, LogTag,
                "SDL Vulkan surface creation failed: %s", SDL_GetError());
            return false;
        }
        return true;
    }

    bool SelectPhysicalDevice()
    {
        uint32_t count = 0;
        VkResult result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (result != VK_SUCCESS || count == 0)
        {
            LogVulkanError("vkEnumeratePhysicalDevices", result);
            return false;
        }

        std::vector<VkPhysicalDevice> devices(count);
        result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkEnumeratePhysicalDevices", result);
            return false;
        }

        for (VkPhysicalDevice candidate : devices)
        {
            const QueueFamilies candidateFamilies = FindQueueFamilies(candidate, surface);
            if (!candidateFamilies.Complete() || !HasExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                continue;

            uint32_t formatCount = 0;
            uint32_t presentModeCount = 0;
            if (vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface, &formatCount, nullptr) != VK_SUCCESS ||
                vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface, &presentModeCount, nullptr) != VK_SUCCESS ||
                formatCount == 0 || presentModeCount == 0)
            {
                continue;
            }

            physicalDevice = candidate;
            queueFamilies = candidateFamilies;
            break;
        }

        if (physicalDevice == VK_NULL_HANDLE)
        {
            __android_log_write(ANDROID_LOG_ERROR, LogTag,
                "No Vulkan device with graphics, presentation, and VK_KHR_swapchain was found");
            return false;
        }

        vkGetPhysicalDeviceProperties(physicalDevice, &physicalProperties);
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(physicalDevice, &features);
        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "Vulkan GPU: %s (API %u.%u.%u, vendor 0x%04x, device 0x%04x, geometry=%u, tessellation=%u, BC=%u)",
            physicalProperties.deviceName,
            VK_VERSION_MAJOR(physicalProperties.apiVersion), VK_VERSION_MINOR(physicalProperties.apiVersion),
            VK_VERSION_PATCH(physicalProperties.apiVersion), physicalProperties.vendorID,
            physicalProperties.deviceID, features.geometryShader, features.tessellationShader,
            features.textureCompressionBC);
        LogZinkGl41Preflight(physicalDevice, physicalProperties, features);
        return true;
    }

    bool CreateDevice()
    {
        const float priority = 1.0f;
        std::vector<uint32_t> uniqueFamilies{queueFamilies.graphics};
        if (queueFamilies.present != queueFamilies.graphics)
            uniqueFamilies.push_back(queueFamilies.present);

        std::vector<VkDeviceQueueCreateInfo> queues;
        for (const uint32_t family : uniqueFamilies)
        {
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = family;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            queues.push_back(queue);
        }

        const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queues.size());
        createInfo.pQueueCreateInfos = queues.data();
        createInfo.enabledExtensionCount = 1;
        createInfo.ppEnabledExtensionNames = extensions;

        const VkResult result = vkCreateDevice(physicalDevice, &createInfo, nullptr, &device);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateDevice", result);
            return false;
        }
        vkGetDeviceQueue(device, queueFamilies.graphics, 0, &graphicsQueue);
        vkGetDeviceQueue(device, queueFamilies.present, 0, &presentQueue);
        return true;
    }

    bool CreatePipelineCache()
    {
        std::vector<unsigned char> initialData;
        if (!ReadFile(cachePath, initialData))
        {
            __android_log_write(ANDROID_LOG_ERROR, LogTag, "Could not read the private Vulkan pipeline cache");
            return false;
        }

        VkPipelineCacheCreateInfo createInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        createInfo.initialDataSize = initialData.size();
        createInfo.pInitialData = initialData.empty() ? nullptr : initialData.data();
        VkResult result = vkCreatePipelineCache(device, &createInfo, nullptr, &pipelineCache);
        if (result != VK_SUCCESS && !initialData.empty())
        {
            __android_log_write(ANDROID_LOG_WARN, LogTag,
                "Discarding an incompatible Vulkan pipeline cache and starting clean");
            createInfo.initialDataSize = 0;
            createInfo.pInitialData = nullptr;
            result = vkCreatePipelineCache(device, &createInfo, nullptr, &pipelineCache);
        }
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreatePipelineCache", result);
            return false;
        }
        return true;
    }

    bool CreateSwapchain()
    {
        VkSurfaceCapabilitiesKHR capabilities{};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
            return false;
        }
        if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
        {
            __android_log_write(ANDROID_LOG_ERROR, LogTag,
                "Vulkan surface images cannot be used as color attachments");
            return false;
        }

        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());
        surfaceFormat = formats.front();
        for (const VkSurfaceFormatKHR& format : formats)
        {
            if ((format.format == VK_FORMAT_R8G8B8A8_UNORM || format.format == VK_FORMAT_B8G8R8A8_UNORM) &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                surfaceFormat = format;
                break;
            }
        }

        if (capabilities.currentExtent.width != UINT32_MAX)
        {
            extent = capabilities.currentExtent;
        }
        else
        {
            int width = 0;
            int height = 0;
            SDL_Vulkan_GetDrawableSize(window, &width, &height);
            extent.width = std::clamp(static_cast<uint32_t>(std::max(width, 1)),
                capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent.height = std::clamp(static_cast<uint32_t>(std::max(height, 1)),
                capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }

        uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0)
            imageCount = std::min(imageCount, capabilities.maxImageCount);

        VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if ((capabilities.supportedCompositeAlpha & compositeAlpha) == 0)
        {
            const VkCompositeAlphaFlagBitsKHR choices[] = {
                VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
            };
            for (const auto choice : choices)
            {
                if ((capabilities.supportedCompositeAlpha & choice) != 0)
                {
                    compositeAlpha = choice;
                    break;
                }
            }
        }

        const uint32_t familyIndices[] = {queueFamilies.graphics, queueFamilies.present};
        VkSwapchainCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        createInfo.surface = surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.imageSharingMode = queueFamilies.graphics == queueFamilies.present
            ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
        if (createInfo.imageSharingMode == VK_SHARING_MODE_CONCURRENT)
        {
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = familyIndices;
        }
        // SDL exposes both rendering and input coordinates in the current
        // display orientation. Keep the swapchain canvas in that same space;
        // selecting Android's currentTransform here rotates it once more.
        createInfo.preTransform =
            (capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0
            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : capabilities.currentTransform;
        createInfo.compositeAlpha = compositeAlpha;
        createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        createInfo.clipped = VK_TRUE;

        result = vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateSwapchainKHR", result);
            return false;
        }
        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "Vulkan surface transform: current=0x%x selected=0x%x supported=0x%x",
            static_cast<unsigned>(capabilities.currentTransform),
            static_cast<unsigned>(createInfo.preTransform),
            static_cast<unsigned>(capabilities.supportedTransforms));

        vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
        swapchainImages.resize(imageCount);
        result = vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data());
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkGetSwapchainImagesKHR", result);
            return false;
        }
        return true;
    }

    bool CreateShaderModule(const unsigned char* bytes, size_t size, VkShaderModule& module)
    {
        if (!bytes || size == 0 || (size % sizeof(uint32_t)) != 0)
        {
            __android_log_write(ANDROID_LOG_ERROR, LogTag, "Embedded SPIR-V shader is invalid");
            return false;
        }

        VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        createInfo.codeSize = size;
        createInfo.pCode = reinterpret_cast<const uint32_t*>(bytes);
        const VkResult result = vkCreateShaderModule(device, &createInfo, nullptr, &module);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateShaderModule", result);
            return false;
        }
        return true;
    }

    std::uint32_t FindMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags required) const
    {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
        for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index)
        {
            if ((typeBits & (1u << index)) != 0 &&
                (properties.memoryTypes[index].propertyFlags & required) == required)
            {
                return index;
            }
        }
        return UINT32_MAX;
    }

    bool EnsureUiVertexBuffer(std::size_t requiredBytes)
    {
        if (requiredBytes <= uiVertexCapacity)
            return true;

        if (uiVertexBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, uiVertexBuffer, nullptr);
        if (uiVertexMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, uiVertexMemory, nullptr);
        uiVertexBuffer = VK_NULL_HANDLE;
        uiVertexMemory = VK_NULL_HANDLE;
        uiVertexCapacity = 0;

        std::size_t capacity = 64 * 1024;
        while (capacity < requiredBytes)
            capacity *= 2;

        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = capacity;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = vkCreateBuffer(device, &bufferInfo, nullptr, &uiVertexBuffer);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateBuffer(UI)", result);
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, uiVertexBuffer, &requirements);
        const std::uint32_t memoryType = FindMemoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memoryType == UINT32_MAX)
        {
            __android_log_write(ANDROID_LOG_ERROR, LogTag, "No host-coherent Vulkan memory for UI vertices");
            return false;
        }

        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        result = vkAllocateMemory(device, &allocation, nullptr, &uiVertexMemory);
        if (result == VK_SUCCESS)
            result = vkBindBufferMemory(device, uiVertexBuffer, uiVertexMemory, 0);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("UI vertex memory allocation", result);
            return false;
        }
        uiVertexCapacity = capacity;
        return true;
    }

    void DestroyWorldMeshResources(WorldGpuMesh& mesh)
    {
        if (mesh.vertexBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, mesh.vertexBuffer, nullptr);
        if (mesh.vertexMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, mesh.vertexMemory, nullptr);
        if (mesh.indexBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, mesh.indexBuffer, nullptr);
        if (mesh.indexMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, mesh.indexMemory, nullptr);
        mesh = {};
    }

    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memoryProperties,
        VkBuffer& buffer, VkDeviceMemory& memory)
    {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = vkCreateBuffer(device, &bufferInfo, nullptr, &buffer);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateBuffer(texture staging)", result);
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        const std::uint32_t memoryType = FindMemoryType(requirements.memoryTypeBits, memoryProperties);
        if (memoryType == UINT32_MAX)
            return false;

        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        result = vkAllocateMemory(device, &allocation, nullptr, &memory);
        if (result == VK_SUCCESS)
            result = vkBindBufferMemory(device, buffer, memory, 0);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("texture staging memory", result);
            return false;
        }
        return true;
    }

    bool CreateUiDescriptors()
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        VkResult result = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &uiDescriptorSetLayout);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateDescriptorSetLayout(UI)", result);
            return false;
        }

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 4096;
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 4096;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        result = vkCreateDescriptorPool(device, &poolInfo, nullptr, &uiDescriptorPool);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateDescriptorPool(UI)", result);
            return false;
        }

        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        // X-Ray world materials routinely tile outside the 0..1 UV range.
        // UI atlases stay inside that range, so a shared repeating sampler is
        // valid for both paths until material-specific samplers are exposed.
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0f;
        result = vkCreateSampler(device, &samplerInfo, nullptr, &uiSampler);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateSampler(UI)", result);
            return false;
        }
        return true;
    }

    bool UploadUiTexture(std::uint32_t width, std::uint32_t height, const std::uint8_t* pixels,
        std::size_t byteCount, UiTexture& texture)
    {
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        VkCommandBuffer uploadCommand = VK_NULL_HANDLE;
        const auto cleanupStaging = [&]()
        {
            if (uploadCommand != VK_NULL_HANDLE)
                vkFreeCommandBuffers(device, commandPool, 1, &uploadCommand);
            if (stagingBuffer != VK_NULL_HANDLE)
                vkDestroyBuffer(device, stagingBuffer, nullptr);
            if (stagingMemory != VK_NULL_HANDLE)
                vkFreeMemory(device, stagingMemory, nullptr);
        };
        const auto cleanupTexture = [&]()
        {
            if (texture.view != VK_NULL_HANDLE)
                vkDestroyImageView(device, texture.view, nullptr);
            if (texture.image != VK_NULL_HANDLE)
                vkDestroyImage(device, texture.image, nullptr);
            if (texture.memory != VK_NULL_HANDLE)
                vkFreeMemory(device, texture.memory, nullptr);
            texture = {};
        };

        if (!CreateBuffer(byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingMemory))
        {
            cleanupStaging();
            return false;
        }

        void* mapped = nullptr;
        VkResult result = vkMapMemory(device, stagingMemory, 0, byteCount, 0, &mapped);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkMapMemory(texture staging)", result);
            cleanupStaging();
            return false;
        }
        std::memcpy(mapped, pixels, byteCount);
        vkUnmapMemory(device, stagingMemory);

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        result = vkCreateImage(device, &imageInfo, nullptr, &texture.image);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateImage(UI)", result);
            cleanupStaging();
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, texture.image, &requirements);
        const std::uint32_t memoryType = FindMemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX)
        {
            cleanupStaging();
            cleanupTexture();
            return false;
        }
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        result = vkAllocateMemory(device, &allocation, nullptr, &texture.memory);
        if (result == VK_SUCCESS)
            result = vkBindImageMemory(device, texture.image, texture.memory, 0);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("UI image memory", result);
            cleanupStaging();
            cleanupTexture();
            return false;
        }

        VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandAllocation.commandPool = commandPool;
        commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandAllocation.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(device, &commandAllocation, &uploadCommand);
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (result == VK_SUCCESS)
            result = vkBeginCommandBuffer(uploadCommand, &beginInfo);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("begin UI texture upload", result);
            cleanupStaging();
            cleanupTexture();
            return false;
        }

        VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = texture.image;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(uploadCommand, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(uploadCommand, stagingBuffer, texture.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        VkImageMemoryBarrier toShader{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.image = texture.image;
        toShader.subresourceRange = toTransfer.subresourceRange;
        vkCmdPipelineBarrier(uploadCommand, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShader);

        result = vkEndCommandBuffer(uploadCommand);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &uploadCommand;
        if (result == VK_SUCCESS)
            result = vkQueueSubmit(graphicsQueue, 1, &submit, VK_NULL_HANDLE);
        if (result == VK_SUCCESS)
            result = vkQueueWaitIdle(graphicsQueue);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("submit UI texture upload", result);
            cleanupStaging();
            cleanupTexture();
            return false;
        }
        cleanupStaging();

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = texture.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        result = vkCreateImageView(device, &viewInfo, nullptr, &texture.view);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateImageView(UI)", result);
            cleanupTexture();
            return false;
        }

        VkDescriptorSetAllocateInfo descriptorAllocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        descriptorAllocation.descriptorPool = uiDescriptorPool;
        descriptorAllocation.descriptorSetCount = 1;
        descriptorAllocation.pSetLayouts = &uiDescriptorSetLayout;
        result = vkAllocateDescriptorSets(device, &descriptorAllocation, &texture.descriptorSet);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkAllocateDescriptorSets(UI)", result);
            cleanupTexture();
            return false;
        }
        VkDescriptorImageInfo descriptorImage{};
        descriptorImage.sampler = uiSampler;
        descriptorImage.imageView = texture.view;
        descriptorImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = texture.descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &descriptorImage;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        texture.width = width;
        texture.height = height;
        return true;
    }

    bool CreateFallbackTexture()
    {
        const std::uint8_t white[] = {255, 255, 255, 255};
        fallbackTexture = CreateUiTexture("__openxray_white__", 1, 1, white, sizeof(white));
        return fallbackTexture != 0;
    }

    VkDescriptorSet GetUiDescriptor(AndroidVulkanUiTexture handle) const
    {
        return handle == 0 || handle > uiTextures.size()
            ? VK_NULL_HANDLE : uiTextures[static_cast<std::size_t>(handle - 1)].descriptorSet;
    }

    bool CreateDepthResources()
    {
        constexpr VkFormat candidates[] = {
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D24_UNORM_S8_UINT,
            VK_FORMAT_D16_UNORM
        };
        depthFormat = VK_FORMAT_UNDEFINED;
        for (const VkFormat candidate : candidates)
        {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice, candidate, &properties);
            if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
            {
                depthFormat = candidate;
                break;
            }
        }
        if (depthFormat == VK_FORMAT_UNDEFINED)
        {
            __android_log_write(ANDROID_LOG_ERROR, LogTag, "No Vulkan depth attachment format available");
            return false;
        }

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = depthFormat;
        imageInfo.extent = {extent.width, extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = vkCreateImage(device, &imageInfo, nullptr, &depthImage);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateImage(depth)", result);
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, depthImage, &requirements);
        const std::uint32_t memoryType = FindMemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX)
            return false;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType;
        result = vkAllocateMemory(device, &allocation, nullptr, &depthMemory);
        if (result == VK_SUCCESS)
            result = vkBindImageMemory(device, depthImage, depthMemory, 0);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("depth image memory", result);
            return false;
        }

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = depthFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (depthFormat == VK_FORMAT_D24_UNORM_S8_UINT)
            viewInfo.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        result = vkCreateImageView(device, &viewInfo, nullptr, &depthImageView);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateImageView(depth)", result);
            return false;
        }
        return true;
    }

    bool CreateGraphicsPipeline()
    {
        if (!CreateUiDescriptors())
            return false;
        swapchainImageViews.reserve(swapchainImages.size());
        for (const VkImage image : swapchainImages)
        {
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = surfaceFormat.format;
            viewInfo.components = {
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
            };
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;

            VkImageView imageView = VK_NULL_HANDLE;
            const VkResult result = vkCreateImageView(device, &viewInfo, nullptr, &imageView);
            if (result != VK_SUCCESS)
            {
                LogVulkanError("vkCreateImageView", result);
                return false;
            }
            swapchainImageViews.push_back(imageView);
        }

        if (!CreateDepthResources())
            return false;

        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = surfaceFormat.format;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorReference{};
        colorReference.attachment = 0;
        colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = depthFormat;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference depthReference{};
        depthReference.attachment = 1;
        depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        subpass.pDepthStencilAttachment = &depthReference;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = dependency.srcStageMask;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        const VkAttachmentDescription attachments[] = {colorAttachment, depthAttachment};
        VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassInfo.attachmentCount = 2;
        renderPassInfo.pAttachments = attachments;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;
        VkResult result = vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateRenderPass", result);
            return false;
        }

        swapchainFramebuffers.reserve(swapchainImageViews.size());
        for (const VkImageView imageView : swapchainImageViews)
        {
            const VkImageView attachments[] = {imageView, depthImageView};
            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.attachmentCount = 2;
            framebufferInfo.pAttachments = attachments;
            framebufferInfo.width = extent.width;
            framebufferInfo.height = extent.height;
            framebufferInfo.layers = 1;

            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            result = vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer);
            if (result != VK_SUCCESS)
            {
                LogVulkanError("vkCreateFramebuffer", result);
                return false;
            }
            swapchainFramebuffers.push_back(framebuffer);
        }

        VkShaderModule vertexShader = VK_NULL_HANDLE;
        VkShaderModule fragmentShader = VK_NULL_HANDLE;
        if (!CreateShaderModule(OpenXRayBootstrapVertSpv, sizeof(OpenXRayBootstrapVertSpv), vertexShader) ||
            !CreateShaderModule(OpenXRayBootstrapFragSpv, sizeof(OpenXRayBootstrapFragSpv), fragmentShader))
        {
            if (fragmentShader != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, fragmentShader, nullptr);
            if (vertexShader != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, vertexShader, nullptr);
            return false;
        }

        VkPipelineShaderStageCreateInfo shaderStages[2]{};
        shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shaderStages[0].module = vertexShader;
        shaderStages[0].pName = "main";
        shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shaderStages[1].module = fragmentShader;
        shaderStages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(AndroidVulkanUiVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attributes[3]{};
        attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
            static_cast<std::uint32_t>(offsetof(AndroidVulkanUiVertex, x))};
        attributes[1] = {1, 0, VK_FORMAT_R32_UINT,
            static_cast<std::uint32_t>(offsetof(AndroidVulkanUiVertex, color))};
        attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,
            static_cast<std::uint32_t>(offsetof(AndroidVulkanUiVertex, u))};
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 3;
        vertexInput.pVertexAttributeDescriptions = attributes;
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};

        VkViewport viewport{};
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{{0, 0}, extent};
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo rasterizer{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisampling{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo colorBlending{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &blendAttachment;

        VkPushConstantRange pushConstant{};
        pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstant.size = sizeof(UiPushConstants);
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &uiDescriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstant;
        result = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);
        if (result != VK_SUCCESS)
        {
            vkDestroyShaderModule(device, fragmentShader, nullptr);
            vkDestroyShaderModule(device, vertexShader, nullptr);
            LogVulkanError("vkCreatePipelineLayout", result);
            return false;
        }

        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
        VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicState.dynamicStateCount = 1;
        dynamicState.pDynamicStates = dynamicStates;
        pipelineInfo.pDynamicState = &dynamicState;

        const VkPrimitiveTopology topologies[] = {
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
            VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
        };
        for (std::size_t index = 0; index < 4; ++index)
        {
            inputAssembly.topology = topologies[index];
            result = vkCreateGraphicsPipelines(
                device, pipelineCache, 1, &pipelineInfo, nullptr, &graphicsPipelines[index]);
            if (result != VK_SUCCESS)
                break;
        }
        vkDestroyShaderModule(device, fragmentShader, nullptr);
        vkDestroyShaderModule(device, vertexShader, nullptr);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateGraphicsPipelines", result);
            return false;
        }

        VkShaderModule worldVertexShader = VK_NULL_HANDLE;
        VkShaderModule worldFragmentShader = VK_NULL_HANDLE;
        if (!CreateShaderModule(OpenXRayWorldVertSpv, sizeof(OpenXRayWorldVertSpv), worldVertexShader) ||
            !CreateShaderModule(OpenXRayWorldFragSpv, sizeof(OpenXRayWorldFragSpv), worldFragmentShader))
        {
            if (worldFragmentShader != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, worldFragmentShader, nullptr);
            if (worldVertexShader != VK_NULL_HANDLE)
                vkDestroyShaderModule(device, worldVertexShader, nullptr);
            return false;
        }

        VkPipelineShaderStageCreateInfo worldStages[2]{};
        worldStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        worldStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        worldStages[0].module = worldVertexShader;
        worldStages[0].pName = "main";
        worldStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        worldStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        worldStages[1].module = worldFragmentShader;
        worldStages[1].pName = "main";

        VkVertexInputBindingDescription worldBinding{};
        worldBinding.binding = 0;
        worldBinding.stride = sizeof(AndroidVulkanWorldVertex);
        worldBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription worldAttributes[3]{};
        worldAttributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
            static_cast<std::uint32_t>(offsetof(AndroidVulkanWorldVertex, x))};
        worldAttributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
            static_cast<std::uint32_t>(offsetof(AndroidVulkanWorldVertex, nx))};
        worldAttributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,
            static_cast<std::uint32_t>(offsetof(AndroidVulkanWorldVertex, u))};
        VkPipelineVertexInputStateCreateInfo worldVertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        worldVertexInput.vertexBindingDescriptionCount = 1;
        worldVertexInput.pVertexBindingDescriptions = &worldBinding;
        worldVertexInput.vertexAttributeDescriptionCount = 3;
        worldVertexInput.pVertexAttributeDescriptions = worldAttributes;

        VkPipelineInputAssemblyStateCreateInfo worldAssembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        worldAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo worldRasterizer = rasterizer;
        worldRasterizer.cullMode = VK_CULL_MODE_NONE;
        VkPipelineDepthStencilStateCreateInfo depthState{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthState.depthTestEnable = VK_TRUE;
        depthState.depthWriteEnable = VK_TRUE;
        depthState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState opaqueAttachment = blendAttachment;
        opaqueAttachment.blendEnable = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo opaqueBlending = colorBlending;
        opaqueBlending.pAttachments = &opaqueAttachment;

        VkPushConstantRange worldPushConstant{};
        worldPushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        worldPushConstant.size = sizeof(WorldPushConstants);
        VkPipelineLayoutCreateInfo worldLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        worldLayoutInfo.setLayoutCount = 1;
        worldLayoutInfo.pSetLayouts = &uiDescriptorSetLayout;
        worldLayoutInfo.pushConstantRangeCount = 1;
        worldLayoutInfo.pPushConstantRanges = &worldPushConstant;
        result = vkCreatePipelineLayout(device, &worldLayoutInfo, nullptr, &worldPipelineLayout);
        if (result == VK_SUCCESS)
        {
            VkGraphicsPipelineCreateInfo worldPipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            worldPipelineInfo.stageCount = 2;
            worldPipelineInfo.pStages = worldStages;
            worldPipelineInfo.pVertexInputState = &worldVertexInput;
            worldPipelineInfo.pInputAssemblyState = &worldAssembly;
            worldPipelineInfo.pViewportState = &viewportState;
            worldPipelineInfo.pRasterizationState = &worldRasterizer;
            worldPipelineInfo.pMultisampleState = &multisampling;
            worldPipelineInfo.pDepthStencilState = &depthState;
            worldPipelineInfo.pColorBlendState = &opaqueBlending;
            worldPipelineInfo.pDynamicState = &dynamicState;
            worldPipelineInfo.layout = worldPipelineLayout;
            worldPipelineInfo.renderPass = renderPass;
            result = vkCreateGraphicsPipelines(
                device, pipelineCache, 1, &worldPipelineInfo, nullptr, &worldPipeline);
        }
        vkDestroyShaderModule(device, worldFragmentShader, nullptr);
        vkDestroyShaderModule(device, worldVertexShader, nullptr);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("create world graphics pipeline", result);
            return false;
        }
        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "Vulkan world pipeline ready (depth format %d)", static_cast<int>(depthFormat));
        return true;
    }

    bool CreateCommands()
    {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = queueFamilies.graphics;
        VkResult result = vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateCommandPool", result);
            return false;
        }

        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = commandPool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(device, &allocation, &commandBuffer);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkAllocateCommandBuffers", result);
            return false;
        }

        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        result = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailable);
        if (result == VK_SUCCESS)
            result = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinished);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateSemaphore", result);
            return false;
        }
        return true;
    }

    bool PresentFrame(std::uint64_t frameIndex, float elapsedSeconds)
    {
        const std::size_t uiBytes = uiVertices.size() * sizeof(AndroidVulkanUiVertex);
        if (uiBytes != 0)
        {
            if (!EnsureUiVertexBuffer(uiBytes))
                return false;
            void* mapped = nullptr;
            VkResult mapResult = vkMapMemory(device, uiVertexMemory, 0, uiBytes, 0, &mapped);
            if (mapResult != VK_SUCCESS)
            {
                LogVulkanError("vkMapMemory(UI)", mapResult);
                return false;
            }
            std::memcpy(mapped, uiVertices.data(), uiBytes);
            vkUnmapMemory(device, uiVertexMemory);
        }

        VkResult result = vkResetCommandPool(device, commandPool, 0);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkResetCommandPool", result);
            return false;
        }

        uint32_t imageIndex = 0;
        result = vkAcquireNextImageKHR(
            device, swapchain, UINT64_MAX, imageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            LogVulkanError("vkAcquireNextImageKHR", result);
            return false;
        }

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkBeginCommandBuffer", result);
            return false;
        }

        VkClearValue clears[2]{};
        clears[0].color = {{0.015f, 0.02f, 0.018f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo renderPassBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        renderPassBegin.renderPass = renderPass;
        renderPassBegin.framebuffer = swapchainFramebuffers[imageIndex];
        renderPassBegin.renderArea.extent = extent;
        renderPassBegin.clearValueCount = 2;
        renderPassBegin.pClearValues = clears;
        vkCmdBeginRenderPass(commandBuffer, &renderPassBegin, VK_SUBPASS_CONTENTS_INLINE);
        if (!worldDraws.empty())
        {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, worldPipeline);
            const VkRect2D worldScissor{{0, 0}, extent};
            vkCmdSetScissor(commandBuffer, 0, 1, &worldScissor);
            AndroidVulkanWorldMesh boundMesh = 0;
            for (const WorldDraw& draw : worldDraws)
            {
                if (draw.mesh == 0 || draw.mesh > worldMeshes.size())
                    continue;
                const WorldGpuMesh& mesh = worldMeshes[static_cast<std::size_t>(draw.mesh - 1)];
                if (!mesh.alive)
                    continue;
                if (boundMesh != draw.mesh)
                {
                    const VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &mesh.vertexBuffer, &offset);
                    vkCmdBindIndexBuffer(commandBuffer, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT16);
                    boundMesh = draw.mesh;
                }
                WorldPushConstants constants{};
                std::memcpy(constants.worldViewProjection, draw.worldViewProjection,
                    sizeof(constants.worldViewProjection));
                vkCmdPushConstants(commandBuffer, worldPipelineLayout,
                    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(constants), &constants);
                const VkDescriptorSet descriptor = GetUiDescriptor(draw.texture);
                if (descriptor == VK_NULL_HANDLE)
                    continue;
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    worldPipelineLayout, 0, 1, &descriptor, 0, nullptr);
                vkCmdDrawIndexed(commandBuffer, draw.indexCount, 1,
                    draw.firstIndex, draw.vertexOffset, 0);
            }
        }
        if (!uiVertices.empty())
        {
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &uiVertexBuffer, &offset);
            for (const UiBatch& batch : uiBatches)
            {
                const std::size_t pipelineIndex = static_cast<std::size_t>(batch.primitive);
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelines[pipelineIndex]);
                const VkDescriptorSet descriptor = GetUiDescriptor(batch.texture);
                if (descriptor == VK_NULL_HANDLE)
                    continue;
                const UiPushConstants constants{
                    static_cast<float>(extent.width), static_cast<float>(extent.height),
                    static_cast<std::uint32_t>(batch.textureMode), 0};
                vkCmdPushConstants(commandBuffer, pipelineLayout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(constants), &constants);
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipelineLayout, 0, 1, &descriptor, 0, nullptr);
                VkRect2D batchScissor{{0, 0}, extent};
                if (batch.scissor.enabled)
                {
                    const std::int32_t left = std::clamp(batch.scissor.x, 0, static_cast<std::int32_t>(extent.width));
                    const std::int32_t top = std::clamp(batch.scissor.y, 0, static_cast<std::int32_t>(extent.height));
                    const std::uint32_t right = std::min(extent.width,
                        static_cast<std::uint32_t>(left) + batch.scissor.width);
                    const std::uint32_t bottom = std::min(extent.height,
                        static_cast<std::uint32_t>(top) + batch.scissor.height);
                    batchScissor.offset = {left, top};
                    batchScissor.extent = {right - static_cast<std::uint32_t>(left),
                        bottom - static_cast<std::uint32_t>(top)};
                }
                if (batchScissor.extent.width == 0 || batchScissor.extent.height == 0)
                    continue;
                vkCmdSetScissor(commandBuffer, 0, 1, &batchScissor);
                vkCmdDraw(commandBuffer, batch.vertexCount, 1, batch.firstVertex, 0);
            }
        }
        vkCmdEndRenderPass(commandBuffer);

        result = vkEndCommandBuffer(commandBuffer);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkEndCommandBuffer", result);
            return false;
        }

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &imageAvailable;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderFinished;
        result = vkQueueSubmit(graphicsQueue, 1, &submit, VK_NULL_HANDLE);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkQueueSubmit", result);
            return false;
        }

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderFinished;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &imageIndex;
        result = vkQueuePresentKHR(presentQueue, &present);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            LogVulkanError("vkQueuePresentKHR", result);
            return false;
        }

        result = vkDeviceWaitIdle(device);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkDeviceWaitIdle", result);
            return false;
        }

        if (!loggedComplexUi && uiBatches.size() > 10)
        {
            loggedComplexUi = true;
            __android_log_print(ANDROID_LOG_INFO, LogTag,
                "Complex UI frame geometry: frame=%llu surface=%ux%u vertices=%zu batches=%zu",
                static_cast<unsigned long long>(frameIndex), extent.width, extent.height,
                uiVertices.size(), uiBatches.size());
            for (std::size_t index = 0; index < uiBatches.size(); ++index)
            {
                const UiBatch& batch = uiBatches[index];
                const std::size_t first = batch.firstVertex;
                const std::size_t end = std::min(uiVertices.size(), first + batch.vertexCount);
                if (first >= end)
                    continue;

                float minX = std::numeric_limits<float>::max();
                float minY = std::numeric_limits<float>::max();
                float maxX = std::numeric_limits<float>::lowest();
                float maxY = std::numeric_limits<float>::lowest();
                float minU = std::numeric_limits<float>::max();
                float minV = std::numeric_limits<float>::max();
                float maxU = std::numeric_limits<float>::lowest();
                float maxV = std::numeric_limits<float>::lowest();
                for (std::size_t vertexIndex = first; vertexIndex < end; ++vertexIndex)
                {
                    const AndroidVulkanUiVertex& vertex = uiVertices[vertexIndex];
                    minX = std::min(minX, vertex.x);
                    minY = std::min(minY, vertex.y);
                    maxX = std::max(maxX, vertex.x);
                    maxY = std::max(maxY, vertex.y);
                    minU = std::min(minU, vertex.u);
                    minV = std::min(minV, vertex.v);
                    maxU = std::max(maxU, vertex.u);
                    maxV = std::max(maxV, vertex.v);
                }

                const char* textureName = "<fallback>";
                if (batch.texture > 0 && batch.texture <= uiTextures.size())
                    textureName = uiTextures[static_cast<std::size_t>(batch.texture - 1)].name.c_str();
                __android_log_print(ANDROID_LOG_INFO, LogTag,
                    "Complex UI batch %zu: texture=%llu ''%s'' mode=%u vertices=%u "
                    "bounds=(%.1f,%.1f)-(%.1f,%.1f) uv=(%.3f,%.3f)-(%.3f,%.3f) "
                    "scissor=%d,(%d,%d %ux%u)",
                    index, static_cast<unsigned long long>(batch.texture), textureName,
                    static_cast<unsigned>(batch.textureMode), batch.vertexCount,
                    minX, minY, maxX, maxY, minU, minV, maxU, maxV,
                    batch.scissor.enabled ? 1 : 0, batch.scissor.x, batch.scissor.y,
                    batch.scissor.width, batch.scissor.height);
            }
        }

        if (frameIndex == 1)
        {
            __android_log_print(ANDROID_LOG_INFO, LogTag,
                "Presented first OpenXRay Vulkan frame (%ux%u, format %d, %zu UI vertices in %zu batches)",
                extent.width, extent.height, surfaceFormat.format, uiVertices.size(), uiBatches.size());
            for (std::size_t index = 0; index < uiBatches.size(); ++index)
            {
                const UiBatch& batch = uiBatches[index];
                if (batch.firstVertex >= uiVertices.size())
                    continue;
                const AndroidVulkanUiVertex& vertex = uiVertices[batch.firstVertex];
                __android_log_print(ANDROID_LOG_INFO, LogTag,
                    "First-frame UI batch %zu: texture=%llu mode=%u vertices=%u first=(%.1f,%.1f uv=%.3f,%.3f color=0x%08x)",
                    index, static_cast<unsigned long long>(batch.texture),
                    static_cast<unsigned>(batch.textureMode), batch.vertexCount,
                    vertex.x, vertex.y, vertex.u, vertex.v, vertex.color);
            }
        }
        else if ((frameIndex % 300) == 0)
        {
            __android_log_print(ANDROID_LOG_DEBUG, LogTag,
                "Vulkan frame loop alive (frame %llu, %zu UI vertices in %zu batches)",
                static_cast<unsigned long long>(frameIndex), uiVertices.size(), uiBatches.size());
        }
        return true;
    }

    bool SavePipelineCache()
    {
        size_t size = 0;
        VkResult result = vkGetPipelineCacheData(device, pipelineCache, &size, nullptr);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkGetPipelineCacheData", result);
            return false;
        }

        std::vector<unsigned char> data(size);
        result = vkGetPipelineCacheData(device, pipelineCache, &size, data.data());
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkGetPipelineCacheData", result);
            return false;
        }
        data.resize(size);
        if (!WriteFile(cachePath, data))
        {
            __android_log_write(ANDROID_LOG_ERROR, LogTag, "Could not write the private Vulkan pipeline cache");
            return false;
        }

        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "Vulkan pipeline cache stored in private app data (%zu bytes)", data.size());
        return true;
    }

    void Destroy()
    {
        if (device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(device);
        if (device != VK_NULL_HANDLE && renderFinished != VK_NULL_HANDLE)
            vkDestroySemaphore(device, renderFinished, nullptr);
        if (device != VK_NULL_HANDLE && imageAvailable != VK_NULL_HANDLE)
            vkDestroySemaphore(device, imageAvailable, nullptr);
        if (device != VK_NULL_HANDLE && commandPool != VK_NULL_HANDLE)
            vkDestroyCommandPool(device, commandPool, nullptr);
        if (device != VK_NULL_HANDLE)
        {
            for (VkPipeline pipeline : graphicsPipelines)
                if (pipeline != VK_NULL_HANDLE)
                    vkDestroyPipeline(device, pipeline, nullptr);
            if (worldPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, worldPipeline, nullptr);
        }
        if (device != VK_NULL_HANDLE && uiVertexBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, uiVertexBuffer, nullptr);
        if (device != VK_NULL_HANDLE && uiVertexMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, uiVertexMemory, nullptr);
        if (device != VK_NULL_HANDLE)
            for (WorldGpuMesh& mesh : worldMeshes)
                if (mesh.alive)
                    DestroyWorldMeshResources(mesh);
        if (device != VK_NULL_HANDLE)
        {
            for (const UiTexture& texture : uiTextures)
            {
                if (texture.view != VK_NULL_HANDLE)
                    vkDestroyImageView(device, texture.view, nullptr);
                if (texture.image != VK_NULL_HANDLE)
                    vkDestroyImage(device, texture.image, nullptr);
                if (texture.memory != VK_NULL_HANDLE)
                    vkFreeMemory(device, texture.memory, nullptr);
            }
        }
        if (device != VK_NULL_HANDLE && pipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (device != VK_NULL_HANDLE && worldPipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, worldPipelineLayout, nullptr);
        if (device != VK_NULL_HANDLE && uiSampler != VK_NULL_HANDLE)
            vkDestroySampler(device, uiSampler, nullptr);
        if (device != VK_NULL_HANDLE && uiDescriptorPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device, uiDescriptorPool, nullptr);
        if (device != VK_NULL_HANDLE && uiDescriptorSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, uiDescriptorSetLayout, nullptr);
        if (device != VK_NULL_HANDLE)
        {
            for (const VkFramebuffer framebuffer : swapchainFramebuffers)
                vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
        if (device != VK_NULL_HANDLE && depthImageView != VK_NULL_HANDLE)
            vkDestroyImageView(device, depthImageView, nullptr);
        if (device != VK_NULL_HANDLE && depthImage != VK_NULL_HANDLE)
            vkDestroyImage(device, depthImage, nullptr);
        if (device != VK_NULL_HANDLE && depthMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, depthMemory, nullptr);
        if (device != VK_NULL_HANDLE && renderPass != VK_NULL_HANDLE)
            vkDestroyRenderPass(device, renderPass, nullptr);
        if (device != VK_NULL_HANDLE)
        {
            for (const VkImageView imageView : swapchainImageViews)
                vkDestroyImageView(device, imageView, nullptr);
        }
        if (device != VK_NULL_HANDLE && swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        if (device != VK_NULL_HANDLE && pipelineCache != VK_NULL_HANDLE)
            vkDestroyPipelineCache(device, pipelineCache, nullptr);
        if (device != VK_NULL_HANDLE)
            vkDestroyDevice(device, nullptr);
        if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance != VK_NULL_HANDLE)
            vkDestroyInstance(instance, nullptr);

        renderFinished = VK_NULL_HANDLE;
        imageAvailable = VK_NULL_HANDLE;
        commandBuffer = VK_NULL_HANDLE;
        commandPool = VK_NULL_HANDLE;
        for (VkPipeline& pipeline : graphicsPipelines)
            pipeline = VK_NULL_HANDLE;
        worldPipeline = VK_NULL_HANDLE;
        worldPipelineLayout = VK_NULL_HANDLE;
        worldMeshes.clear();
        worldDraws.clear();
        uiVertexBuffer = VK_NULL_HANDLE;
        uiVertexMemory = VK_NULL_HANDLE;
        uiVertexCapacity = 0;
        uiTextures.clear();
        uiTextureByName.clear();
        fallbackTexture = 0;
        pipelineLayout = VK_NULL_HANDLE;
        uiSampler = VK_NULL_HANDLE;
        uiDescriptorPool = VK_NULL_HANDLE;
        uiDescriptorSetLayout = VK_NULL_HANDLE;
        swapchainFramebuffers.clear();
        depthImageView = VK_NULL_HANDLE;
        depthImage = VK_NULL_HANDLE;
        depthMemory = VK_NULL_HANDLE;
        depthFormat = VK_FORMAT_UNDEFINED;
        renderPass = VK_NULL_HANDLE;
        swapchainImageViews.clear();
        swapchainImages.clear();
        swapchain = VK_NULL_HANDLE;
        pipelineCache = VK_NULL_HANDLE;
        graphicsQueue = VK_NULL_HANDLE;
        presentQueue = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        physicalDevice = VK_NULL_HANDLE;
        surface = VK_NULL_HANDLE;
        instance = VK_NULL_HANDLE;
    }

    SDL_Window* window = nullptr;
    std::string cacheDirectory;
    std::string cachePath;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties physicalProperties{};
    QueueFamilies queueFamilies;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surfaceFormat{};
    VkExtent2D extent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<VkFramebuffer> swapchainFramebuffers;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout uiDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool uiDescriptorPool = VK_NULL_HANDLE;
    VkSampler uiSampler = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline graphicsPipelines[4]{};
    VkPipelineLayout worldPipelineLayout = VK_NULL_HANDLE;
    VkPipeline worldPipeline = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthImageView = VK_NULL_HANDLE;
    std::vector<WorldGpuMesh> worldMeshes;
    std::vector<WorldDraw> worldDraws;
    VkBuffer uiVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uiVertexMemory = VK_NULL_HANDLE;
    std::size_t uiVertexCapacity{};
    std::vector<AndroidVulkanUiVertex> uiVertices;
    std::vector<UiBatch> uiBatches;
    std::vector<UiTexture> uiTextures;
    std::unordered_map<std::string, AndroidVulkanUiTexture> uiTextureByName;
    AndroidVulkanUiTexture fallbackTexture{};
    bool loggedComplexUi{};
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    bool initialized = false;
};
} // namespace

struct AndroidVulkanRenderer::Implementation
{
    VulkanBootstrap renderer;
};

AndroidVulkanRenderer::AndroidVulkanRenderer()
    : implementation(std::make_unique<Implementation>())
{}

AndroidVulkanRenderer::~AndroidVulkanRenderer() = default;

bool AndroidVulkanRenderer::Initialize(SDL_Window* window, const char* appFilesPath)
{
    return implementation->renderer.Initialize(window, appFilesPath);
}

AndroidVulkanUiTexture AndroidVulkanRenderer::CreateUiTexture(const char* name, std::uint32_t width,
    std::uint32_t height, const std::uint8_t* rgbaPixels, std::size_t byteCount)
{
    return implementation->renderer.CreateUiTexture(name, width, height, rgbaPixels, byteCount);
}

bool AndroidVulkanRenderer::UpdateUiTexture(AndroidVulkanUiTexture texture,
    const std::uint8_t* rgbaPixels, std::size_t byteCount)
{
    return implementation->renderer.UpdateUiTexture(texture, rgbaPixels, byteCount);
}

AndroidVulkanWorldMesh AndroidVulkanRenderer::CreateWorldMesh(
    const AndroidVulkanWorldVertex* vertices, std::size_t vertexCount,
    const std::uint16_t* indices, std::size_t indexCount)
{
    return implementation->renderer.CreateWorldMesh(vertices, vertexCount, indices, indexCount);
}

bool AndroidVulkanRenderer::UpdateWorldMesh(AndroidVulkanWorldMesh mesh,
    const AndroidVulkanWorldVertex* vertices, std::size_t vertexCount)
{
    return implementation->renderer.UpdateWorldMesh(mesh, vertices, vertexCount);
}

void AndroidVulkanRenderer::DestroyWorldMesh(AndroidVulkanWorldMesh mesh)
{
    implementation->renderer.DestroyWorldMesh(mesh);
}

void AndroidVulkanRenderer::BeginUiFrame()
{
    implementation->renderer.BeginUiFrame();
}

void AndroidVulkanRenderer::BeginWorldFrame()
{
    implementation->renderer.BeginWorldFrame();
}

void AndroidVulkanRenderer::SubmitUiBatch(const AndroidVulkanUiVertex* vertices, std::size_t vertexCount,
    AndroidVulkanUiPrimitive primitive, const AndroidVulkanUiScissor& scissor,
    AndroidVulkanUiTexture texture, AndroidVulkanUiTextureMode textureMode)
{
    implementation->renderer.SubmitUiBatch(vertices, vertexCount, primitive, scissor, texture, textureMode);
}

void AndroidVulkanRenderer::SubmitWorldMesh(AndroidVulkanWorldMesh mesh,
    std::uint32_t firstIndex, std::uint32_t indexCount, std::int32_t vertexOffset,
    const float* worldViewProjection, AndroidVulkanUiTexture texture)
{
    implementation->renderer.SubmitWorldMesh(
        mesh, firstIndex, indexCount, vertexOffset, worldViewProjection, texture);
}

bool AndroidVulkanRenderer::DrawFrame(std::uint64_t frameIndex, float elapsedSeconds)
{
    return implementation->renderer.DrawFrame(frameIndex, elapsedSeconds);
}

void AndroidVulkanRenderer::GetSurfaceSize(std::uint32_t& width, std::uint32_t& height) const
{
    implementation->renderer.GetSurfaceSize(width, height);
}

void AndroidVulkanRenderer::Shutdown()
{
    implementation->renderer.Shutdown();
}

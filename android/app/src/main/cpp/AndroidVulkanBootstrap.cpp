#include "AndroidVulkanBootstrap.hpp"
#include "AndroidBootstrapFragSpv.hpp"
#include "AndroidBootstrapVertSpv.hpp"

#include <SDL.h>
#include <vulkan/vulkan.h>
#include <SDL_vulkan.h>

#include <algorithm>
#include <android/log.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{
constexpr const char* LogTag = "OpenXRay";

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
            SavePipelineCache();
        return initialized;
    }

    bool DrawFrame(std::uint64_t frameIndex, float elapsedSeconds)
    {
        return initialized && PresentFrame(frameIndex, elapsedSeconds);
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
        createInfo.preTransform = capabilities.currentTransform;
        createInfo.compositeAlpha = compositeAlpha;
        createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        createInfo.clipped = VK_TRUE;

        result = vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateSwapchainKHR", result);
            return false;
        }

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

    bool CreateGraphicsPipeline()
    {
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

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
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
            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &imageView;
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

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

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
        VkPipelineColorBlendStateCreateInfo colorBlending{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &blendAttachment;

        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
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
        result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &graphicsPipeline);
        vkDestroyShaderModule(device, fragmentShader, nullptr);
        vkDestroyShaderModule(device, vertexShader, nullptr);
        if (result != VK_SUCCESS)
        {
            LogVulkanError("vkCreateGraphicsPipelines", result);
            return false;
        }
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

        // A small pulse makes it visible that the renderer remains active after
        // the first presentation instead of leaving one stale swapchain image.
        const float pulse = 0.5f + 0.5f * std::sin(elapsedSeconds * 1.25f);
        const VkClearValue clear{{{0.025f + pulse * 0.02f, 0.07f + pulse * 0.035f, 0.045f, 1.0f}}};
        VkRenderPassBeginInfo renderPassBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        renderPassBegin.renderPass = renderPass;
        renderPassBegin.framebuffer = swapchainFramebuffers[imageIndex];
        renderPassBegin.renderArea.extent = extent;
        renderPassBegin.clearValueCount = 1;
        renderPassBegin.pClearValues = &clear;
        vkCmdBeginRenderPass(commandBuffer, &renderPassBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
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

        if (frameIndex == 1)
        {
            __android_log_print(ANDROID_LOG_INFO, LogTag,
                "Presented first persistent Vulkan host frame (%ux%u, format %d, 3 vertices)",
                extent.width, extent.height, surfaceFormat.format);
        }
        else if ((frameIndex % 300) == 0)
        {
            __android_log_print(ANDROID_LOG_DEBUG, LogTag,
                "Vulkan host frame loop alive (frame %llu)",
                static_cast<unsigned long long>(frameIndex));
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
        if (device != VK_NULL_HANDLE && graphicsPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device, graphicsPipeline, nullptr);
        if (device != VK_NULL_HANDLE && pipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (device != VK_NULL_HANDLE)
        {
            for (const VkFramebuffer framebuffer : swapchainFramebuffers)
                vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
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
        graphicsPipeline = VK_NULL_HANDLE;
        pipelineLayout = VK_NULL_HANDLE;
        swapchainFramebuffers.clear();
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
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline = VK_NULL_HANDLE;
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

bool AndroidVulkanRenderer::DrawFrame(std::uint64_t frameIndex, float elapsedSeconds)
{
    return implementation->renderer.DrawFrame(frameIndex, elapsedSeconds);
}

void AndroidVulkanRenderer::Shutdown()
{
    implementation->renderer.Shutdown();
}

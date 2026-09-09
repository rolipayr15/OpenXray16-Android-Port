#include "AndroidVulkanRenderDevice.hpp"

#include <SDL.h>
#include <SDL_vulkan.h>
#include <android/log.h>

namespace
{
constexpr const char* LogTag = "OpenXRay";
}

AndroidVulkanRenderDevice::AndroidVulkanRenderDevice(const char* path) : appFilesPath(path ? path : "")
{
    m_hq_skinning = false;
    m_skinning = 0;
    m_MSAASample = 1;
    m_SMAPSize = 0;
}

AndroidVulkanRenderDevice::~AndroidVulkanRenderDevice()
{
    Destroy();
}

IRender::GenerationLevel AndroidVulkanRenderDevice::GetGeneration() const { return GENERATION_R2; }
IRender::BackendAPI AndroidVulkanRenderDevice::GetBackendAPI() const { return BackendAPI::Vulkan; }
bool AndroidVulkanRenderDevice::is_sun_static() { return false; }
u32 AndroidVulkanRenderDevice::get_dx_level() { return 11; }
void AndroidVulkanRenderDevice::create() {}
void AndroidVulkanRenderDevice::destroy() {}
void AndroidVulkanRenderDevice::reset_begin() {}
void AndroidVulkanRenderDevice::reset_end() {}
void AndroidVulkanRenderDevice::level_Load(IReader*) {}
void AndroidVulkanRenderDevice::level_Unload() {}

HRESULT AndroidVulkanRenderDevice::shader_compile(pcstr, IReader*, pcstr, pcstr, u32, void*& result)
{
    result = nullptr;
    return static_cast<HRESULT>(-1);
}

void AndroidVulkanRenderDevice::DumpStatistics(IGameFont&, IPerformanceAlert*) {}
pcstr AndroidVulkanRenderDevice::getShaderPath() { return "vulkan"; }
IRenderVisual* AndroidVulkanRenderDevice::getVisual(int) { return nullptr; }
xrImTextureData AndroidVulkanRenderDevice::GetImGuiTextureId(pcstr) { return {}; }
void AndroidVulkanRenderDevice::add_Visual(u32, IRenderable*, IRenderVisual*, Fmatrix&) {}
void AndroidVulkanRenderDevice::add_StaticWallmark(const wm_shader&, const Fvector&, float, CDB::TRI*, Fvector*) {}
void AndroidVulkanRenderDevice::add_StaticWallmark(IWallMarkArray*, const Fvector&, float, CDB::TRI*, Fvector*) {}
void AndroidVulkanRenderDevice::clear_static_wallmarks() {}
void AndroidVulkanRenderDevice::add_SkeletonWallmark(
    const Fmatrix*, IKinematics*, IWallMarkArray*, const Fvector&, const Fvector&, float)
{}
IRender_ObjectSpecific* AndroidVulkanRenderDevice::ros_create(IRenderable*) { return nullptr; }
void AndroidVulkanRenderDevice::ros_destroy(IRender_ObjectSpecific*& object) { object = nullptr; }
IRender_Light* AndroidVulkanRenderDevice::light_create() { return nullptr; }
IRender_Glow* AndroidVulkanRenderDevice::glow_create() { return nullptr; }
IRenderVisual* AndroidVulkanRenderDevice::model_CreateParticles(pcstr) { return nullptr; }
IRenderVisual* AndroidVulkanRenderDevice::model_Create(pcstr, IReader*) { return nullptr; }
IRenderVisual* AndroidVulkanRenderDevice::model_CreateChild(pcstr, IReader*) { return nullptr; }
IRenderVisual* AndroidVulkanRenderDevice::model_Duplicate(IRenderVisual*) { return nullptr; }
void AndroidVulkanRenderDevice::model_Delete(IRenderVisual*& visual, bool) { visual = nullptr; }
void AndroidVulkanRenderDevice::model_Logging(bool) {}
void AndroidVulkanRenderDevice::models_Prefetch() {}
void AndroidVulkanRenderDevice::models_Clear(bool) {}
bool AndroidVulkanRenderDevice::occ_visible(vis_data&) { return true; }
bool AndroidVulkanRenderDevice::occ_visible(Fbox&) { return true; }
bool AndroidVulkanRenderDevice::occ_visible(sPoly&) { return true; }
void AndroidVulkanRenderDevice::Calculate() {}
void AndroidVulkanRenderDevice::Render() {}
void AndroidVulkanRenderDevice::RenderMenu() {}
void AndroidVulkanRenderDevice::BeforeWorldRender() {}
void AndroidVulkanRenderDevice::AfterWorldRender() {}
void AndroidVulkanRenderDevice::Screenshot(ScreenshotMode, pcstr) {}
void AndroidVulkanRenderDevice::SetPostProcessParams(const SPPInfo&) {}
void AndroidVulkanRenderDevice::setGamma(float) {}
void AndroidVulkanRenderDevice::setBrightness(float) {}
void AndroidVulkanRenderDevice::setContrast(float) {}
void AndroidVulkanRenderDevice::updateGamma() {}
void AndroidVulkanRenderDevice::OnDeviceDestroy(bool) {}

void AndroidVulkanRenderDevice::Destroy()
{
    if (!initialized)
        return;

    renderer.Shutdown();
    initialized = false;
    window = nullptr;
}

void AndroidVulkanRenderDevice::UpdateDimensions(
    SDL_Window* targetWindow, u32& width, u32& height, float& halfWidth, float& halfHeight)
{
    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_Vulkan_GetDrawableSize(targetWindow, &drawableWidth, &drawableHeight);
    if (drawableWidth > 0 && drawableHeight > 0)
    {
        width = static_cast<u32>(drawableWidth);
        height = static_cast<u32>(drawableHeight);
    }
    halfWidth = static_cast<float>(width) * 0.5f;
    halfHeight = static_cast<float>(height) * 0.5f;
}

void AndroidVulkanRenderDevice::Create(
    SDL_Window* targetWindow, u32& width, u32& height, float& halfWidth, float& halfHeight)
{
    Destroy();
    window = targetWindow;
    UpdateDimensions(window, width, height, halfWidth, halfHeight);
    failed = !renderer.Initialize(window, appFilesPath.c_str());
    initialized = !failed;
    frameIndex = 0;
    frameLoopStart = SDL_GetPerformanceCounter();

    if (failed)
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Vulkan IRender device creation failed");
    else
        __android_log_write(ANDROID_LOG_INFO, LogTag, "Vulkan bootstrap is connected through GEnv.Render");
}

void AndroidVulkanRenderDevice::Reset(
    SDL_Window* targetWindow, u32& width, u32& height, float& halfWidth, float& halfHeight)
{
    Create(targetWindow, width, height, halfWidth, halfHeight);
}

void AndroidVulkanRenderDevice::ObtainRequiredWindowFlags(u32& windowFlags) { windowFlags |= SDL_WINDOW_VULKAN; }
void AndroidVulkanRenderDevice::SetupStates() {}
void AndroidVulkanRenderDevice::OnDeviceCreate(pcstr) {}
void AndroidVulkanRenderDevice::overdrawBegin() {}
void AndroidVulkanRenderDevice::overdrawEnd() {}
void AndroidVulkanRenderDevice::DeferredLoad(bool) {}
void AndroidVulkanRenderDevice::ResourcesDeferredUpload() {}
void AndroidVulkanRenderDevice::ResourcesDeferredUnload() {}

void AndroidVulkanRenderDevice::ResourcesGetMemoryUsage(
    u32& baseMemory, u32& baseCount, u32& lightmapMemory, u32& lightmapCount)
{
    baseMemory = 0;
    baseCount = 0;
    lightmapMemory = 0;
    lightmapCount = 0;
}

void AndroidVulkanRenderDevice::ResourcesDestroyNecessaryTextures() {}
void AndroidVulkanRenderDevice::ResourcesStoreNecessaryTextures() {}
void AndroidVulkanRenderDevice::ResourcesDumpMemoryUsage() {}
bool AndroidVulkanRenderDevice::HWSupportsShaderYUV2RGB() { return false; }
DeviceState AndroidVulkanRenderDevice::GetDeviceState() { return failed ? DeviceState::Lost : DeviceState::Normal; }
bool AndroidVulkanRenderDevice::GetForceGPU_REF() { return false; }
u32 AndroidVulkanRenderDevice::GetCacheStatPolys() { return 1; }
void AndroidVulkanRenderDevice::OnCameraUpdated() {}
void AndroidVulkanRenderDevice::Begin() {}
void AndroidVulkanRenderDevice::Clear() {}

void AndroidVulkanRenderDevice::End()
{
    if (!initialized || failed)
        return;

    const std::uint64_t frequency = SDL_GetPerformanceFrequency();
    const float elapsedSeconds = frequency == 0 ? 0.0f : static_cast<float>(
        static_cast<double>(SDL_GetPerformanceCounter() - frameLoopStart) / static_cast<double>(frequency));
    ++frameIndex;
    failed = !renderer.DrawFrame(frameIndex, elapsedSeconds);
}

void AndroidVulkanRenderDevice::ClearTarget() {}
void AndroidVulkanRenderDevice::SetCacheXform(Fmatrix&, Fmatrix&) {}
void AndroidVulkanRenderDevice::OnAssetsChanged() {}
IRender::RenderContext AndroidVulkanRenderDevice::GetCurrentContext() const { return PrimaryContext; }
void AndroidVulkanRenderDevice::MakeContextCurrent(RenderContext) {}

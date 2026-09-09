#pragma once

#include "Common/Platform.hpp"
#include "AndroidVulkanBootstrap.hpp"
#include "Include/xrAPI/xrAPI.h"
#include "xrCore/xrCore.h"
#include <imgui.h>
#include "xrEngine/Render.h"

#include <cstdint>
#include <string>

// First Stage-6 bridge between the native Vulkan bootstrap and the render
// lifecycle expected by CRenderDevice. Resource, model and UI services are
// intentionally not advertised here; they are implemented by subsequent
// renderer layers instead of being hidden behind fake success paths.
class AndroidVulkanRenderDevice final : public IRender
{
public:
    explicit AndroidVulkanRenderDevice(const char* appFilesPath);
    ~AndroidVulkanRenderDevice() override;

    bool HasFailed() const { return failed; }

    GenerationLevel GetGeneration() const override;
    BackendAPI GetBackendAPI() const override;
    bool is_sun_static() override;
    u32 get_dx_level() override;

    void create() override;
    void destroy() override;
    void reset_begin() override;
    void reset_end() override;
    void level_Load(IReader* fs) override;
    void level_Unload() override;
    HRESULT shader_compile(pcstr name, IReader* fs, pcstr functionName, pcstr target, u32 flags,
        void*& result) override;
    void DumpStatistics(IGameFont& font, IPerformanceAlert* alert) override;
    pcstr getShaderPath() override;
    IRenderVisual* getVisual(int id) override;
    xrImTextureData GetImGuiTextureId(pcstr textureName) override;
    void add_Visual(u32 contextId, IRenderable* root, IRenderVisual* visual, Fmatrix& transform) override;
    void add_StaticWallmark(const wm_shader& shader, const Fvector& position, float size, CDB::TRI* triangle,
        Fvector* vertices) override;
    void add_StaticWallmark(IWallMarkArray* array, const Fvector& position, float size, CDB::TRI* triangle,
        Fvector* vertices) override;
    void clear_static_wallmarks() override;
    void add_SkeletonWallmark(const Fmatrix* transform, IKinematics* object, IWallMarkArray* array,
        const Fvector& start, const Fvector& direction, float size) override;
    IRender_ObjectSpecific* ros_create(IRenderable* parent) override;
    void ros_destroy(IRender_ObjectSpecific*& object) override;
    IRender_Light* light_create() override;
    IRender_Glow* glow_create() override;
    IRenderVisual* model_CreateParticles(pcstr name) override;
    IRenderVisual* model_Create(pcstr name, IReader* data) override;
    IRenderVisual* model_CreateChild(pcstr name, IReader* data) override;
    IRenderVisual* model_Duplicate(IRenderVisual* visual) override;
    void model_Delete(IRenderVisual*& visual, bool discard) override;
    void model_Logging(bool enable) override;
    void models_Prefetch() override;
    void models_Clear(bool complete) override;
    bool occ_visible(vis_data& visibility) override;
    bool occ_visible(Fbox& box) override;
    bool occ_visible(sPoly& polygon) override;
    void Calculate() override;
    void Render() override;
    void RenderMenu() override;
    void BeforeWorldRender() override;
    void AfterWorldRender() override;
    void Screenshot(ScreenshotMode mode, pcstr name) override;
    void SetPostProcessParams(const SPPInfo& parameters) override;
    void setGamma(float value) override;
    void setBrightness(float value) override;
    void setContrast(float value) override;
    void updateGamma() override;
    void OnDeviceDestroy(bool keepTextures) override;
    void Destroy() override;
    void Reset(SDL_Window* window, u32& width, u32& height, float& halfWidth, float& halfHeight) override;
    void ObtainRequiredWindowFlags(u32& windowFlags) override;
    void SetupStates() override;
    void OnDeviceCreate(pcstr shaderName) override;
    void Create(SDL_Window* window, u32& width, u32& height, float& halfWidth, float& halfHeight) override;
    void overdrawBegin() override;
    void overdrawEnd() override;
    void DeferredLoad(bool enable) override;
    void ResourcesDeferredUpload() override;
    void ResourcesDeferredUnload() override;
    void ResourcesGetMemoryUsage(u32& baseMemory, u32& baseCount, u32& lightmapMemory, u32& lightmapCount) override;
    void ResourcesDestroyNecessaryTextures() override;
    void ResourcesStoreNecessaryTextures() override;
    void ResourcesDumpMemoryUsage() override;
    bool HWSupportsShaderYUV2RGB() override;
    DeviceState GetDeviceState() override;
    bool GetForceGPU_REF() override;
    u32 GetCacheStatPolys() override;
    void OnCameraUpdated() override;
    void Begin() override;
    void Clear() override;
    void End() override;
    void ClearTarget() override;
    void SetCacheXform(Fmatrix& view, Fmatrix& projection) override;
    void OnAssetsChanged() override;
    RenderContext GetCurrentContext() const override;
    void MakeContextCurrent(RenderContext context) override;

private:
    void UpdateDimensions(SDL_Window* window, u32& width, u32& height, float& halfWidth, float& halfHeight);

    AndroidVulkanRenderer renderer;
    std::string appFilesPath;
    SDL_Window* window{};
    std::uint64_t frameIndex{};
    std::uint64_t frameLoopStart{};
    bool initialized{};
    bool failed{};
};

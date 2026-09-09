#include "AndroidVulkanRenderServices.hpp"

#include "AndroidVulkanRenderDevice.hpp"

#include "Include/xrRender/DebugRender.h"
#include "Include/xrRender/DrawUtils.h"
#include "Include/xrRender/EnvironmentRender.h"
#include "Include/xrRender/FontRender.h"
#include "Include/xrRender/ImGuiRender.h"
#include "Include/xrRender/LensFlareRender.h"
#include "Include/xrRender/ObjectSpaceRender.h"
#include "Include/xrRender/RainRender.h"
#include "Include/xrRender/RenderFactory.h"
#include "Include/xrRender/StatGraphRender.h"
#include "Include/xrRender/ThunderboltDescRender.h"
#include "Include/xrRender/ThunderboltRender.h"
#include "Include/xrRender/UIRender.h"
#include "Include/xrRender/UISequenceVideoItem.h"
#include "Include/xrRender/UIShader.h"
#include "Include/xrRender/WallMarkArray.h"
#include "Include/xrRender/particles_systems_library_interface.hpp"

#include <android/log.h>

namespace
{
constexpr const char* LogTag = "OpenXRay";

class BootstrapUISequenceVideoItem final : public IUISequenceVideoItem
{
public:
    void Copy(IUISequenceVideoItem&) override {}
    bool HasTexture() override { return false; }
    void CaptureTexture() override {}
    void ResetTexture() override {}
    BOOL video_IsPlaying() override { return FALSE; }
    void video_Sync(u32) override {}
    void video_Play(BOOL, u32) override {}
    void video_Stop() override {}
};

class BootstrapUIShader final : public IUIShader
{
public:
    void Copy(IUIShader&) override { initialized = true; }
    void create(LPCSTR, LPCSTR) override { initialized = true; }
    bool inited() override { return initialized; }
    void destroy() override { initialized = false; }
    bool operator==(const IUIShader& other) const override { return this == &other; }
    bool GetBaseTextureResolution(Fvector2& resolution) override
    {
        resolution.set(1.0f, 1.0f);
        return false;
    }
    xrImTextureData GetImGuiTextureId() override { return {}; }

private:
    bool initialized{};
};

class BootstrapStatGraphRender final : public IStatGraphRender
{
public:
    void Copy(IStatGraphRender&) override {}
    void OnDeviceCreate() override {}
    void OnDeviceDestroy() override {}
    void OnRender(CStatGraph&) override {}
};

#ifdef DEBUG
class BootstrapObjectSpaceRender final : public IObjectSpaceRender
{
public:
    void Copy(IObjectSpaceRender&) override {}
    void dbgRender() override {}
    void dbgAddSphere(const Fsphere&, u32) override {}
    void dbgReserveSphere(size_t) override {}
    void SetShader() override {}
};
#endif

class BootstrapWallMarkArray final : public IWallMarkArray
{
public:
    void Copy(IWallMarkArray&) override {}
    void AppendMark(LPCSTR) override { hasMarks = true; }
    void clear() override { hasMarks = false; }
    bool empty() override { return !hasMarks; }
    wm_shader GenerateWallmark() override { return {}; }

private:
    bool hasMarks{};
};

class EmptyParticlesLibrary final : public particles_systems::library_interface
{
public:
    particles_systems::PS::CPGDef const* const* particles_group_begin() const override { return nullptr; }
    particles_systems::PS::CPGDef const* const* particles_group_end() const override { return nullptr; }
    void particles_group_next(particles_systems::PS::CPGDef const* const*& iterator) const override
    {
        iterator = nullptr;
    }
    const shared_str& particles_group_id(const particles_systems::PS::CPGDef&) const override { return emptyId; }

private:
    shared_str emptyId;
};

class BootstrapEnvironmentRender final : public IEnvironmentRender
{
public:
    void Copy(IEnvironmentRender&) override {}
    void RenderSky(CEnvironment&) override {}
    void RenderClouds(CEnvironment&) override {}
    void OnDeviceCreate() override {}
    void OnDeviceDestroy() override {}
    void Clear() override {}
    void lerp(CEnvDescriptorMixer&, IEnvDescriptorRender*, IEnvDescriptorRender*) override {}
    const particles_systems::library_interface& particles_systems_library() override { return particles; }

private:
    EmptyParticlesLibrary particles;
};

class BootstrapEnvDescriptorRender final : public IEnvDescriptorRender
{
public:
    void Copy(IEnvDescriptorRender&) override {}
    void OnDeviceCreate(CEnvDescriptor&) override {}
    void OnDeviceDestroy() override {}
};

class BootstrapRainRender final : public IRainRender
{
public:
    void Copy(IRainRender&) override {}
    void Render(CEffect_Rain&) override {}
    const Fsphere& GetDropBounds() const override { return bounds; }

private:
    Fsphere bounds{};
};

class BootstrapLensFlareRender final : public ILensFlareRender
{
public:
    void Copy(ILensFlareRender&) override {}
    void Render(CLensFlare&, BOOL, BOOL, BOOL) override {}
    void OnDeviceCreate() override {}
    void OnDeviceDestroy() override {}
};

class BootstrapImGuiRender final : public IImGuiRender
{
public:
    void Copy(IImGuiRender&) override {}
    void Frame() override {}
    void Render(ImDrawData*) override {}
    void OnDeviceCreate(ImGuiContext*) override {}
    void OnDeviceDestroy() override {}
    void OnDeviceResetBegin() override {}
    void OnDeviceResetEnd() override {}
};

class BootstrapThunderboltRender final : public IThunderboltRender
{
public:
    void Copy(IThunderboltRender&) override {}
    void Render(CEffect_Thunderbolt&) override {}
};

class BootstrapThunderboltDescRender final : public IThunderboltDescRender
{
public:
    void Copy(IThunderboltDescRender&) override {}
    void CreateModel(LPCSTR) override {}
    void DestroyModel() override {}
};

class BootstrapFlareRender final : public IFlareRender
{
public:
    void Copy(IFlareRender&) override {}
    void CreateShader(LPCSTR, LPCSTR) override {}
    void DestroyShader() override {}
};

class BootstrapFontRender final : public IFontRender
{
public:
    void Initialize(cpcstr, cpcstr) override {}
    void OnRender(CGameFont&) override {}
};

#define IMPLEMENT_FACTORY_OBJECT(Class, Implementation) \
    I##Class* Create##Class() override { return xr_new<Implementation>(); } \
    void Destroy##Class(I##Class* object) override { xr_delete(object); }

class BootstrapRenderFactory final : public IRenderFactory
{
public:
    IMPLEMENT_FACTORY_OBJECT(UISequenceVideoItem, BootstrapUISequenceVideoItem)
    IMPLEMENT_FACTORY_OBJECT(UIShader, BootstrapUIShader)
    IMPLEMENT_FACTORY_OBJECT(StatGraphRender, BootstrapStatGraphRender)
#ifdef DEBUG
    IMPLEMENT_FACTORY_OBJECT(ObjectSpaceRender, BootstrapObjectSpaceRender)
#endif
    IMPLEMENT_FACTORY_OBJECT(WallMarkArray, BootstrapWallMarkArray)
    IMPLEMENT_FACTORY_OBJECT(EnvironmentRender, BootstrapEnvironmentRender)
    IMPLEMENT_FACTORY_OBJECT(EnvDescriptorRender, BootstrapEnvDescriptorRender)
    IMPLEMENT_FACTORY_OBJECT(RainRender, BootstrapRainRender)
    IMPLEMENT_FACTORY_OBJECT(LensFlareRender, BootstrapLensFlareRender)
    IMPLEMENT_FACTORY_OBJECT(ImGuiRender, BootstrapImGuiRender)
    IMPLEMENT_FACTORY_OBJECT(ThunderboltRender, BootstrapThunderboltRender)
    IMPLEMENT_FACTORY_OBJECT(ThunderboltDescRender, BootstrapThunderboltDescRender)
    IMPLEMENT_FACTORY_OBJECT(FlareRender, BootstrapFlareRender)
    IMPLEMENT_FACTORY_OBJECT(FontRender, BootstrapFontRender)
};

#undef IMPLEMENT_FACTORY_OBJECT

class BootstrapUIRender final : public IUIRender
{
public:
    void CreateUIGeom() override {}
    void DestroyUIGeom() override {}
    void SetShader(IUIShader&) override {}
    void SetAlphaRef(int) override {}
    void SetScissor(Irect*) override {}
    void PushPoint(float, float, float, u32, float, float) override {}
    void StartPrimitive(u32, ePrimitiveType, ePointType) override {}
    void FlushPrimitive() override {}
    LPCSTR UpdateShaderName(LPCSTR, LPCSTR shaderName) override { return shaderName; }
    void CacheSetXformWorld(const Fmatrix&) override {}
    void CacheSetCullMode(CullMode) override {}
};

class BootstrapDrawUtils final : public CDUInterface
{
public:
    void DrawCross(const Fvector&, float, float, float, float, float, float, u32, BOOL) override {}
    void DrawCross(const Fvector&, float, u32, BOOL) override {}
    void DrawFlag(const Fvector&, float, float, float, float, u32, BOOL) override {}
    void DrawRomboid(const Fvector&, float, u32) override {}
    void DrawJoint(const Fvector&, float, u32) override {}
    void DrawSpotLight(const Fvector&, const Fvector&, float, float, u32) override {}
    void DrawDirectionalLight(const Fvector&, const Fvector&, float, float, u32) override {}
    void DrawPointLight(const Fvector&, float, u32) override {}
    void DrawSound(const Fvector&, float, u32) override {}
    void DrawLineSphere(const Fvector&, float, u32, BOOL) override {}
    void dbgDrawPlacement(const Fvector&, int, u32, LPCSTR, u32) override {}
    void dbgDrawVert(const Fvector&, u32, LPCSTR) override {}
    void dbgDrawEdge(const Fvector&, const Fvector&, u32, LPCSTR) override {}
    void dbgDrawFace(const Fvector&, const Fvector&, const Fvector&, u32, LPCSTR) override {}
    void DrawFace(const Fvector&, const Fvector&, const Fvector&, u32, u32, BOOL, BOOL) override {}
    void DrawLine(const Fvector&, const Fvector&, u32) override {}
    void DrawLink(const Fvector&, const Fvector&, float, u32) override {}
    void DrawFaceNormal(const Fvector&, const Fvector&, const Fvector&, float, u32) override {}
    void DrawFaceNormal(const Fvector*, float, u32) override {}
    void DrawFaceNormal(const Fvector&, const Fvector&, float, u32) override {}
    void DrawSelectionBox(const Fvector&, const Fvector&, u32*) override {}
    void DrawSelectionBoxB(const Fbox&, u32*) override {}
    void DrawIdentSphere(BOOL, BOOL, u32, u32) override {}
    void DrawIdentSpherePart(BOOL, BOOL, u32, u32) override {}
    void DrawIdentCone(BOOL, BOOL, u32, u32) override {}
    void DrawIdentCylinder(BOOL, BOOL, u32, u32) override {}
    void DrawIdentBox(BOOL, BOOL, u32, u32) override {}
    void DrawBox(const Fvector&, const Fvector&, BOOL, BOOL, u32, u32) override {}
    void DrawAABB(const Fvector&, const Fvector&, u32, u32, BOOL, BOOL) override {}
    void DrawAABB(const Fmatrix&, const Fvector&, const Fvector&, u32, u32, BOOL, BOOL) override {}
    void DrawOBB(const Fmatrix&, const Fobb&, u32, u32) override {}
    void DrawSphere(const Fmatrix&, const Fvector&, float, u32, u32, BOOL, BOOL) override {}
    void DrawSphere(const Fmatrix&, const Fsphere&, u32, u32, BOOL, BOOL) override {}
    void DrawCylinder(const Fmatrix&, const Fvector&, const Fvector&, float, float, u32, u32, BOOL, BOOL) override {}
    void DrawCone(const Fmatrix&, const Fvector&, const Fvector&, float, float, u32, u32, BOOL, BOOL) override {}
    void DrawPlane(const Fvector&, const Fvector2&, const Fvector&, u32, u32, BOOL, BOOL, BOOL) override {}
    void DrawPlane(const Fvector&, const Fvector&, const Fvector2&, u32, u32, BOOL, BOOL, BOOL) override {}
    void DrawRectangle(const Fvector&, const Fvector&, const Fvector&, u32, u32, BOOL, BOOL) override {}
    void DrawGrid() override {}
    void DrawPivot(const Fvector&, float) override {}
    void DrawAxis(const Fmatrix&) override {}
    void DrawObjectAxis(const Fmatrix&, float, BOOL) override {}
    void DrawSelectionRect(const Ivector2&, const Ivector2&) override {}
    void DrawIndexedPrimitive(int, u32, const Fvector&, const Fvector*, const u32&, const u32*, const u32&,
        const u32&, float) override
    {}
    void OutText(const Fvector&, LPCSTR, u32, u32) override {}
    void OnDeviceDestroy() override {}
};

#ifdef DEBUG
class BootstrapDebugRender final : public IDebugRender
{
public:
    void Render() override {}
    void add_lines(const Fvector*, const u32&, const u16*, const u32&, const u32&) override {}
    void NextSceneMode() override {}
    void ZEnable(bool) override {}
    void OnFrameEnd() override {}
    void SetShader(const debug_shader&) override {}
    void CacheSetXformWorld(const Fmatrix&) override {}
    void CacheSetCullMode(CullMode) override {}
    void SetAmbient(u32) override {}
    void SetDebugShader(dbgShaderHandle) override {}
    void DestroyDebugShader(dbgShaderHandle) override {}
    void dbg_DrawTRI(Fmatrix&, Fvector&, Fvector&, Fvector&, u32) override {}
};
#endif
} // namespace

struct AndroidVulkanRenderServices::Implementation
{
    BootstrapRenderFactory factory;
    BootstrapUIRender ui;
    BootstrapDrawUtils drawUtils;
#ifdef DEBUG
    BootstrapDebugRender debug;
#endif
    bool attached{};
};

AndroidVulkanRenderServices::AndroidVulkanRenderServices() : implementation(std::make_unique<Implementation>()) {}
AndroidVulkanRenderServices::~AndroidVulkanRenderServices() { Detach(); }

void AndroidVulkanRenderServices::Attach(AndroidVulkanRenderDevice& renderDevice)
{
    Detach();
    GEnv.Render = &renderDevice;
    GEnv.RenderFactory = &implementation->factory;
    GEnv.UIRender = &implementation->ui;
    GEnv.DU = &implementation->drawUtils;
#ifdef DEBUG
    GEnv.DRender = &implementation->debug;
#endif
    implementation->attached = true;
    __android_log_write(ANDROID_LOG_INFO, LogTag, "Android Vulkan bootstrap render services attached to GEnv");
}

void AndroidVulkanRenderServices::Detach()
{
    if (!implementation || !implementation->attached)
        return;

    GEnv.Render = nullptr;
    GEnv.RenderFactory = nullptr;
    GEnv.UIRender = nullptr;
    GEnv.DU = nullptr;
    GEnv.DRender = nullptr;
    implementation->attached = false;
}

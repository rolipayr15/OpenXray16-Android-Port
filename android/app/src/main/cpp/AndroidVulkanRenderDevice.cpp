#include "AndroidVulkanRenderDevice.hpp"
#include "AndroidVulkanRenderServices.hpp"
#include "Layers/xrRenderPC_GL/AndroidModelBridge.h"

#include "Include/xrRender/ParticleCustom.h"
#include "Include/xrRender/RenderVisual.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/IRenderable.h"
#include "xrEngine/device.h"
#include "xrEngine/vis_common.h"
#include "xrCore/FMesh.hpp"
#include "xrCore/LocatorAPI.h"
#include "xrCore/stream_reader.h"
#include "xrCDB/Frustum.h"
#include "xrCDB/ISpatial.h"
#include "Common/LevelStructure.hpp"
#include "Common/d3d9compat.hpp"

#include <SDL.h>
#include <SDL_vulkan.h>
#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace
{
constexpr const char* LogTag = "OpenXRay";

struct AndroidLevelVertexBuffer
{
    std::vector<AndroidVulkanWorldVertex> vertices;
    std::uint32_t combinedBase{};
};

struct AndroidLevelIndexBuffer
{
    std::vector<std::uint16_t> indices;
    std::uint32_t combinedBase{};
};

std::size_t DeclarationTypeSize(std::uint8_t type)
{
    switch (type)
    {
    case D3DDECLTYPE_FLOAT1: return sizeof(float);
    case D3DDECLTYPE_FLOAT2: return sizeof(float) * 2;
    case D3DDECLTYPE_FLOAT3: return sizeof(float) * 3;
    case D3DDECLTYPE_FLOAT4: return sizeof(float) * 4;
    case D3DDECLTYPE_D3DCOLOR:
    case D3DDECLTYPE_UBYTE4:
    case D3DDECLTYPE_UBYTE4N:
    case D3DDECLTYPE_UDEC3:
    case D3DDECLTYPE_DEC3N: return sizeof(std::uint32_t);
    case D3DDECLTYPE_SHORT2:
    case D3DDECLTYPE_SHORT2N:
    case D3DDECLTYPE_USHORT2N:
    case D3DDECLTYPE_FLOAT16_2: return sizeof(std::uint16_t) * 2;
    case D3DDECLTYPE_SHORT4:
    case D3DDECLTYPE_SHORT4N:
    case D3DDECLTYPE_USHORT4N:
    case D3DDECLTYPE_FLOAT16_4: return sizeof(std::uint16_t) * 4;
    default: return 0;
    }
}

const D3DVERTEXELEMENT9* FindDeclarationElement(
    const std::vector<D3DVERTEXELEMENT9>& declaration, std::uint8_t usage, std::uint8_t usageIndex = 0)
{
    const auto it = std::find_if(declaration.begin(), declaration.end(), [usage, usageIndex](const auto& element)
    {
        return element.Stream == 0 && element.Usage == usage && element.UsageIndex == usageIndex;
    });
    return it == declaration.end() ? nullptr : &*it;
}

float UnpackSignedTenBit(std::uint32_t value)
{
    std::int32_t signedValue = static_cast<std::int32_t>(value & 0x3ffu);
    if ((signedValue & 0x200) != 0)
        signedValue -= 0x400;
    return std::max(-1.0f, static_cast<float>(signedValue) / 511.0f);
}

void DecodeNormal(const std::uint8_t* source, const D3DVERTEXELEMENT9* element, AndroidVulkanWorldVertex& vertex)
{
    if (!element)
        return;

    source += element->Offset;
    if (element->Type == D3DDECLTYPE_FLOAT3 || element->Type == D3DDECLTYPE_FLOAT4)
    {
        std::memcpy(&vertex.nx, source, sizeof(float) * 3);
        return;
    }

    std::uint32_t packed{};
    std::memcpy(&packed, source, sizeof(packed));
    if (element->Type == D3DDECLTYPE_D3DCOLOR || element->Type == D3DDECLTYPE_UBYTE4N)
    {
        constexpr float scale = 2.0f / 255.0f;
        vertex.nx = static_cast<float>((packed >> 16) & 0xffu) * scale - 1.0f;
        vertex.ny = static_cast<float>((packed >> 8) & 0xffu) * scale - 1.0f;
        vertex.nz = static_cast<float>(packed & 0xffu) * scale - 1.0f;
    }
    else if (element->Type == D3DDECLTYPE_DEC3N)
    {
        vertex.nx = UnpackSignedTenBit(packed);
        vertex.ny = UnpackSignedTenBit(packed >> 10);
        vertex.nz = UnpackSignedTenBit(packed >> 20);
    }
}

std::uint8_t PackedAlpha(const std::uint8_t* source, const D3DVERTEXELEMENT9* element)
{
    if (!element || element->Type != D3DDECLTYPE_D3DCOLOR)
        return 0;
    std::uint32_t packed{};
    std::memcpy(&packed, source + element->Offset, sizeof(packed));
    return static_cast<std::uint8_t>(packed >> 24);
}

void DecodeTexcoord(const std::uint8_t* source, const std::vector<D3DVERTEXELEMENT9>& declaration,
    AndroidVulkanWorldVertex& vertex)
{
    const auto* texcoord = FindDeclarationElement(declaration, D3DDECLUSAGE_TEXCOORD);
    if (!texcoord)
        return;

    const std::uint8_t* value = source + texcoord->Offset;
    if (texcoord->Type == D3DDECLTYPE_FLOAT2 || texcoord->Type == D3DDECLTYPE_FLOAT3 ||
        texcoord->Type == D3DDECLTYPE_FLOAT4)
    {
        std::memcpy(&vertex.u, value, sizeof(float) * 2);
        return;
    }

    if (texcoord->Type == D3DDECLTYPE_SHORT2 || texcoord->Type == D3DDECLTYPE_SHORT4)
    {
        std::int16_t packed[2]{};
        std::memcpy(packed, value, sizeof(packed));
        const auto* tangent = FindDeclarationElement(declaration, D3DDECLUSAGE_TANGENT);
        const auto* binormal = FindDeclarationElement(declaration, D3DDECLUSAGE_BINORMAL);
        constexpr float scale = 32.0f / 32768.0f;
        vertex.u = static_cast<float>(packed[0] + PackedAlpha(source, tangent)) * scale;
        vertex.v = static_cast<float>(packed[1] + PackedAlpha(source, binormal)) * scale;
    }
}

bool ReadLevelVertexBuffers(CStreamReader& geometry, std::vector<AndroidLevelVertexBuffer>& buffers,
    std::vector<AndroidVulkanWorldVertex>& combinedVertices)
{
    CStreamReader* chunk = geometry.open_chunk(fsL_VB);
    if (!chunk)
        return false;

    const std::uint32_t bufferCount = chunk->r_u32();
    buffers.resize(bufferCount);
    for (std::uint32_t bufferIndex = 0; bufferIndex < bufferCount; ++bufferIndex)
    {
        std::vector<D3DVERTEXELEMENT9> declaration;
        bool declarationEnded = false;
        for (std::size_t elementIndex = 0; elementIndex <= MAXD3DDECLLENGTH; ++elementIndex)
        {
            D3DVERTEXELEMENT9 element{};
            chunk->r(&element, sizeof(element));
            if (element.Stream == 0xff)
            {
                declarationEnded = true;
                break;
            }
            declaration.push_back(element);
        }
        if (!declarationEnded || declaration.empty())
        {
            chunk->close();
            return false;
        }

        std::size_t stride = 0;
        for (const auto& element : declaration)
            stride = std::max(stride, static_cast<std::size_t>(element.Offset) + DeclarationTypeSize(element.Type));

        const auto* position = FindDeclarationElement(declaration, D3DDECLUSAGE_POSITION);
        const std::uint32_t vertexCount = chunk->r_u32();
        if (!position || position->Type != D3DDECLTYPE_FLOAT3 || stride == 0 ||
            vertexCount > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
        {
            chunk->close();
            return false;
        }

        auto& buffer = buffers[bufferIndex];
        buffer.combinedBase = static_cast<std::uint32_t>(combinedVertices.size());
        buffer.vertices.resize(vertexCount);
        std::vector<std::uint8_t> source(stride);
        const auto* normal = FindDeclarationElement(declaration, D3DDECLUSAGE_NORMAL);
        for (auto& vertex : buffer.vertices)
        {
            chunk->r(source.data(), source.size());
            vertex.ny = 1.0f;
            std::memcpy(&vertex.x, source.data() + position->Offset, sizeof(float) * 3);
            DecodeNormal(source.data(), normal, vertex);
            DecodeTexcoord(source.data(), declaration, vertex);
            combinedVertices.push_back(vertex);
        }
    }
    chunk->close();
    return true;
}

bool ReadLevelIndexBuffers(CStreamReader& geometry, std::vector<AndroidLevelIndexBuffer>& buffers,
    std::vector<std::uint16_t>& combinedIndices)
{
    CStreamReader* chunk = geometry.open_chunk(fsL_IB);
    if (!chunk)
        return false;

    const std::uint32_t bufferCount = chunk->r_u32();
    buffers.resize(bufferCount);
    for (std::uint32_t bufferIndex = 0; bufferIndex < bufferCount; ++bufferIndex)
    {
        auto& buffer = buffers[bufferIndex];
        const std::uint32_t indexCount = chunk->r_u32();
        buffer.combinedBase = static_cast<std::uint32_t>(combinedIndices.size());
        buffer.indices.resize(indexCount);
        chunk->r(buffer.indices.data(), buffer.indices.size() * sizeof(std::uint16_t));
        combinedIndices.insert(combinedIndices.end(), buffer.indices.begin(), buffer.indices.end());
    }
    chunk->close();
    return true;
}

// Particle GPU geometry is the next renderer subsystem; retain the real
// engine-visible lifecycle while that dedicated path is connected.
class AndroidBootstrapParticleVisual final : public IRenderVisual, public IParticleCustom
{
public:
    explicit AndroidBootstrapParticleVisual(pcstr particleName)
        : name(particleName ? particleName : "")
    {
        visibility.clear();
    }

    vis_data& getVisData() override { return visibility; }
    u32 getType() const override { return 0; }
#ifdef DEBUG
    shared_str getDebugName() override { return name; }
#endif
    IParticleCustom* dcast_ParticleCustom() override { return this; }

    void OnDeviceCreate() override {}
    void OnDeviceDestroy() override {}
    void UpdateParent(const Fmatrix& transform, const Fvector&, BOOL) override
    {
        visibility.sphere.P.set(transform.c);
    }
    void OnFrame(u32) override {}
    void Play() override { playing = true; }
    void Stop(BOOL) override { playing = false; }
    BOOL IsPlaying() override { return playing ? TRUE : FALSE; }
    u32 ParticlesCount() override { return 0; }
    float GetTimeLimit() override { return 1.0f; }
    const shared_str Name() override { return name; }
    void SetHudMode(BOOL value) override { hudMode = value != FALSE; }
    BOOL GetHudMode() override { return hudMode ? TRUE : FALSE; }

private:
    vis_data visibility;
    shared_str name;
    bool playing{};
    bool hudMode{};
};
}

AndroidVulkanRenderDevice::AndroidVulkanRenderDevice(const char* path) : appFilesPath(path ? path : "")
{
    modelPool = CreateAndroidRenderModelPool();
    m_hq_skinning = false;
    m_skinning = 0;
    m_MSAASample = 1;
    m_SMAPSize = 0;
}

AndroidVulkanRenderDevice::~AndroidVulkanRenderDevice()
{
    Destroy();
    DestroyAndroidRenderModelPool(modelPool);
}

IRender::GenerationLevel AndroidVulkanRenderDevice::GetGeneration() const { return GENERATION_R2; }
IRender::BackendAPI AndroidVulkanRenderDevice::GetBackendAPI() const { return BackendAPI::Vulkan; }
bool AndroidVulkanRenderDevice::is_sun_static() { return false; }
u32 AndroidVulkanRenderDevice::get_dx_level() { return 11; }
void AndroidVulkanRenderDevice::create() {}
void AndroidVulkanRenderDevice::destroy() {}
void AndroidVulkanRenderDevice::reset_begin() {}
void AndroidVulkanRenderDevice::reset_end() {}
void AndroidVulkanRenderDevice::level_Load(IReader* level)
{
    level_Unload();
    if (!level || !initialized)
        return;

    CStreamReader* geometry = FS.rs_open("$level$", "level.geom");
    if (!geometry)
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Android Vulkan: level.geom is missing");
        return;
    }

    std::vector<AndroidLevelVertexBuffer> vertexBuffers;
    std::vector<AndroidLevelIndexBuffer> indexBuffers;
    std::vector<AndroidVulkanWorldVertex> vertices;
    std::vector<std::uint16_t> indices;
    const bool geometryLoaded = ReadLevelVertexBuffers(*geometry, vertexBuffers, vertices) &&
        ReadLevelIndexBuffers(*geometry, indexBuffers, indices);
    FS.r_close(geometry);
    if (!geometryLoaded)
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag,
            "Android Vulkan: failed to decode level.geom vertex/index buffers");
        return;
    }

    IReader* visuals = level->open_chunk(fsL_VISUALS);
    if (!visuals)
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Android Vulkan: level has no visual table");
        return;
    }

    std::vector<std::string> materialTextures;
    if (IReader* shaders = level->open_chunk(fsL_SHADERS))
    {
        const std::uint32_t shaderCount = shaders->r_u32();
        materialTextures.resize(shaderCount);
        for (std::uint32_t shaderIndex = 0; shaderIndex < shaderCount; ++shaderIndex)
        {
            string512 material{};
            shaders->r_stringZ(material, sizeof(material));
            const char* textureList = std::strchr(material, '/');
            if (!textureList || !textureList[1])
                continue;
            ++textureList;
            const char* comma = std::strchr(textureList, ',');
            materialTextures[shaderIndex].assign(textureList,
                comma ? static_cast<std::size_t>(comma - textureList) : std::strlen(textureList));
        }
        shaders->close();
    }

    for (std::uint32_t visualIndex = 0;; ++visualIndex)
    {
        IReader* visual = visuals->open_chunk(visualIndex);
        if (!visual)
            break;

        ogf_header header{};
        const bool hasHeader = visual->r_chunk_safe(OGF_HEADER, &header, sizeof(header));

        if (visual->find_chunk(OGF_GCONTAINER) != 0)
        {
            const std::uint32_t vertexBuffer = visual->r_u32();
            const std::uint32_t vertexBase = visual->r_u32();
            const std::uint32_t vertexCount = visual->r_u32();
            const std::uint32_t indexBuffer = visual->r_u32();
            const std::uint32_t indexBase = visual->r_u32();
            const std::uint32_t indexCount = visual->r_u32();

            if (vertexBuffer < vertexBuffers.size() && indexBuffer < indexBuffers.size() &&
                vertexBase <= vertexBuffers[vertexBuffer].vertices.size() &&
                vertexCount <= vertexBuffers[vertexBuffer].vertices.size() - vertexBase &&
                indexBase <= indexBuffers[indexBuffer].indices.size() &&
                indexCount <= indexBuffers[indexBuffer].indices.size() - indexBase)
            {
                LevelDraw draw{};
                draw.firstIndex = indexBuffers[indexBuffer].combinedBase + indexBase;
                draw.indexCount = indexCount;
                draw.vertexOffset = static_cast<std::int32_t>(
                    vertexBuffers[vertexBuffer].combinedBase + vertexBase);
                if (hasHeader && header.shader_id < materialTextures.size() &&
                    !materialTextures[header.shader_id].empty())
                {
                    std::uint32_t textureWidth = 0;
                    std::uint32_t textureHeight = 0;
                    AndroidLoadVulkanTexture(*this, materialTextures[header.shader_id].c_str(),
                        draw.texture, textureWidth, textureHeight);
                }
                levelDraws.push_back(draw);
            }
            else
            {
                __android_log_print(ANDROID_LOG_WARN, LogTag,
                    "Android Vulkan: visual %u has an invalid GCONTAINER range", visualIndex);
            }
        }
        visual->close();
    }
    visuals->close();

    if (vertices.empty() || indices.empty() || levelDraws.empty())
    {
        levelDraws.clear();
        __android_log_write(ANDROID_LOG_ERROR, LogTag,
            "Android Vulkan: level contains no drawable static geometry");
        return;
    }

    levelMesh = renderer.CreateWorldMesh(vertices.data(), vertices.size(), indices.data(), indices.size());
    if (levelMesh == 0)
    {
        levelDraws.clear();
        __android_log_write(ANDROID_LOG_ERROR, LogTag,
            "Android Vulkan: failed to upload static level geometry");
        return;
    }

    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "Android Vulkan: uploaded level geometry: %zu vertices, %zu indices, %zu draws",
        vertices.size(), indices.size(), levelDraws.size());
}

void AndroidVulkanRenderDevice::level_Unload()
{
    if (levelMesh != 0)
        renderer.DestroyWorldMesh(levelMesh);
    levelMesh = 0;
    levelDraws.clear();
    ClearDynamicMeshes();
}

HRESULT AndroidVulkanRenderDevice::shader_compile(pcstr, IReader*, pcstr, pcstr, u32, void*& result)
{
    result = nullptr;
    return static_cast<HRESULT>(-1);
}

void AndroidVulkanRenderDevice::DumpStatistics(IGameFont&, IPerformanceAlert*) {}
pcstr AndroidVulkanRenderDevice::getShaderPath() { return "vulkan"; }
IRenderVisual* AndroidVulkanRenderDevice::getVisual(int) { return nullptr; }
xrImTextureData AndroidVulkanRenderDevice::GetImGuiTextureId(pcstr) { return {}; }
void AndroidVulkanRenderDevice::add_Visual(
    u32, IRenderable* root, IRenderVisual* visual, Fmatrix& transform)
{
    if (root)
        AndroidRenderObjectUpdate(root->renderable_ROS(), root);
    if (!visual || !initialized)
        return;

    std::vector<AndroidModelGeometry> geometry;
    if (!AndroidBuildModelGeometry(visual, geometry))
        return;

    static_assert(sizeof(AndroidModelVertex) == sizeof(AndroidVulkanWorldVertex));
    static_assert(std::is_trivially_copyable_v<AndroidModelVertex>);
    for (const AndroidModelGeometry& part : geometry)
    {
        if (!part.source || part.vertices.empty() || part.indices.empty())
            continue;

        DynamicMesh& cached = dynamicMeshes[part.source];
        if (cached.mesh != 0 &&
            (cached.vertexCount != part.vertices.size() || cached.indexCount != part.indices.size()))
        {
            renderer.DestroyWorldMesh(cached.mesh);
            cached = {};
        }
        if (cached.mesh == 0)
        {
            cached.mesh = renderer.CreateWorldMesh(
                reinterpret_cast<const AndroidVulkanWorldVertex*>(part.vertices.data()),
                part.vertices.size(), part.indices.data(), part.indices.size());
            if (cached.mesh == 0)
                continue;
            cached.vertexCount = static_cast<std::uint32_t>(part.vertices.size());
            cached.indexCount = static_cast<std::uint32_t>(part.indices.size());
            if (!part.texture.empty())
            {
                std::uint32_t textureWidth = 0;
                std::uint32_t textureHeight = 0;
                AndroidLoadVulkanTexture(*this, part.texture.c_str(), cached.texture,
                    textureWidth, textureHeight);
            }
            __android_log_print(ANDROID_LOG_INFO, LogTag,
                "Android Vulkan: uploaded dynamic model part: %u vertices, %u indices, texture=%s",
                cached.vertexCount, cached.indexCount,
                part.texture.empty() ? "<none>" : part.texture.c_str());
        }
        else if (!renderer.UpdateWorldMesh(cached.mesh,
            reinterpret_cast<const AndroidVulkanWorldVertex*>(part.vertices.data()), part.vertices.size()))
        {
            continue;
        }

        DynamicDraw draw{};
        draw.mesh = cached.mesh;
        draw.indexCount = cached.indexCount;
        draw.texture = cached.texture;
        Fmatrix worldViewProjection;
        worldViewProjection.mul(Device.mFullTransform, transform);
        std::memcpy(draw.worldViewProjection, &worldViewProjection._11,
            sizeof(draw.worldViewProjection));
        dynamicDraws.push_back(draw);
    }
}
void AndroidVulkanRenderDevice::add_StaticWallmark(const wm_shader&, const Fvector&, float, CDB::TRI*, Fvector*) {}
void AndroidVulkanRenderDevice::add_StaticWallmark(IWallMarkArray*, const Fvector&, float, CDB::TRI*, Fvector*) {}
void AndroidVulkanRenderDevice::clear_static_wallmarks() {}
void AndroidVulkanRenderDevice::add_SkeletonWallmark(
    const Fmatrix*, IKinematics*, IWallMarkArray*, const Fvector&, const Fvector&, float)
{}
IRender_ObjectSpecific* AndroidVulkanRenderDevice::ros_create(IRenderable*)
{
    return AndroidRenderObjectCreate();
}
void AndroidVulkanRenderDevice::ros_destroy(IRender_ObjectSpecific*& object)
{
    AndroidRenderObjectDestroy(object);
}
IRender_Light* AndroidVulkanRenderDevice::light_create()
{
    return AndroidRenderLightCreate();
}
void AndroidVulkanRenderDevice::light_destroy(IRender_Light* light)
{
    AndroidRenderLightDestroyed(light);
}
IRender_Glow* AndroidVulkanRenderDevice::glow_create()
{
    return AndroidRenderGlowCreate();
}
void AndroidVulkanRenderDevice::glow_destroy(IRender_Glow* glow)
{
    AndroidRenderGlowDestroyed(glow);
}
IRenderVisual* AndroidVulkanRenderDevice::model_CreateParticles(pcstr name)
{
    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "Android bootstrap particle visual created: %s", name ? name : "<null>");
    return xr_new<AndroidBootstrapParticleVisual>(name);
}
IRenderVisual* AndroidVulkanRenderDevice::model_Create(pcstr name, IReader* data)
{
    IRenderVisual* visual = AndroidModelCreate(modelPool, name, data);
    if (visual)
    {
        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "Android model loaded: %s (type=%u, kinematics=%s)", name ? name : "<null>",
            visual->getType(), visual->dcast_PKinematics() ? "yes" : "no");
    }
    return visual;
}
IRenderVisual* AndroidVulkanRenderDevice::model_CreateChild(pcstr name, IReader* data)
{
    return AndroidModelCreateChild(modelPool, name, data);
}
IRenderVisual* AndroidVulkanRenderDevice::model_Duplicate(IRenderVisual* visual)
{
    return AndroidModelDuplicate(modelPool, visual);
}
void AndroidVulkanRenderDevice::model_Delete(IRenderVisual*& visual, bool discard)
{
    if (visual && visual->dcast_ParticleCustom())
    {
        xr_delete(visual);
        return;
    }
    AndroidModelDelete(modelPool, visual, discard);
}
void AndroidVulkanRenderDevice::model_Logging(bool enable) { AndroidModelSetLogging(modelPool, enable); }
void AndroidVulkanRenderDevice::models_Prefetch() { AndroidModelsPrefetch(modelPool); }
void AndroidVulkanRenderDevice::models_Clear(bool complete)
{
    ClearDynamicMeshes();
    AndroidModelsClear(modelPool, complete);
}
bool AndroidVulkanRenderDevice::occ_visible(vis_data&) { return true; }
bool AndroidVulkanRenderDevice::occ_visible(Fbox&) { return true; }
bool AndroidVulkanRenderDevice::occ_visible(sPoly&) { return true; }
void AndroidVulkanRenderDevice::Calculate()
{
    dynamicDraws.clear();
    if (!g_pGamePersistent)
        return;

    CFrustum view;
    view.CreateFromMatrix(Device.mFullTransform, FRUSTUM_P_ALL);
    xr_vector<ISpatial*> renderables;
    g_pGamePersistent->SpatialSpace.q_frustum(
        renderables, ISpatial_DB::O_ORDERED, STYPE_RENDERABLE, view);
    for (ISpatial* spatial : renderables)
    {
        IRenderable* renderable = spatial ? spatial->dcast_Renderable() : nullptr;
        if (renderable)
            renderable->renderable_Render(PrimaryContext, renderable);
    }
}
void AndroidVulkanRenderDevice::Render()
{
    const float* worldViewProjection = &Device.mFullTransform._11;
    if (levelMesh != 0)
    {
        for (const auto& draw : levelDraws)
            renderer.SubmitWorldMesh(levelMesh, draw.firstIndex, draw.indexCount,
                draw.vertexOffset, worldViewProjection, draw.texture);
    }
    for (const DynamicDraw& draw : dynamicDraws)
        renderer.SubmitWorldMesh(draw.mesh, 0, draw.indexCount, 0,
            draw.worldViewProjection, draw.texture);
}
void AndroidVulkanRenderDevice::RenderMenu()
{
    // The ShoC main menu normally renders into the desktop renderer's
    // post-process UI target. There is no intermediate target in the Android
    // bootstrap yet, so submit that main pass directly to this frame.
    if (g_pGamePersistent)
        g_pGamePersistent->OnRenderPPUI_main();
}
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

    level_Unload();
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

void AndroidVulkanRenderDevice::ClearDynamicMeshes()
{
    for (auto& [visual, mesh] : dynamicMeshes)
    {
        (void)visual;
        if (mesh.mesh != 0)
            renderer.DestroyWorldMesh(mesh.mesh);
    }
    dynamicMeshes.clear();
    dynamicDraws.clear();
}

void AndroidVulkanRenderDevice::Create(
    SDL_Window* targetWindow, u32& width, u32& height, float& halfWidth, float& halfHeight)
{
    Destroy();
    window = targetWindow;
    UpdateDimensions(window, width, height, halfWidth, halfHeight);
    failed = !renderer.Initialize(window, appFilesPath.c_str());
    initialized = !failed;
    if (initialized)
    {
        // SDL_SetWindowSize may briefly report the portrait vid_mode restored
        // from user.ltx while Android is enforcing sensorLandscape. The
        // swapchain extent is authoritative once its surface is created.
        renderer.GetSurfaceSize(width, height);
        halfWidth = static_cast<float>(width) * 0.5f;
        halfHeight = static_cast<float>(height) * 0.5f;
        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "Android render dimensions synchronized to swapchain: %ux%u", width, height);
    }
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
DeviceState AndroidVulkanRenderDevice::GetDeviceState()
{
    // A surface destroyed during Android pause invalidates the swapchain. A
    // failed frame on an otherwise initialized renderer is recoverable through
    // CRenderDevice::Reset once SDL has supplied the replacement surface.
    if (failed)
        return initialized ? DeviceState::NeedReset : DeviceState::Lost;
    return DeviceState::Normal;
}
bool AndroidVulkanRenderDevice::GetForceGPU_REF() { return false; }
u32 AndroidVulkanRenderDevice::GetCacheStatPolys() { return 1; }
void AndroidVulkanRenderDevice::OnCameraUpdated() {}
void AndroidVulkanRenderDevice::Begin()
{
    renderer.BeginWorldFrame();
    renderer.BeginUiFrame();
}
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

void AndroidVulkanRenderDevice::SubmitUiBatch(const AndroidVulkanUiVertex* vertices, std::size_t vertexCount,
    AndroidVulkanUiPrimitive primitive, const AndroidVulkanUiScissor& scissor,
    AndroidVulkanUiTexture texture, AndroidVulkanUiTextureMode textureMode)
{
    renderer.SubmitUiBatch(vertices, vertexCount, primitive, scissor, texture, textureMode);
}

AndroidVulkanUiTexture AndroidVulkanRenderDevice::CreateUiTexture(const char* name, std::uint32_t width,
    std::uint32_t height, const std::uint8_t* rgbaPixels, std::size_t byteCount)
{
    return renderer.CreateUiTexture(name, width, height, rgbaPixels, byteCount);
}

bool AndroidVulkanRenderDevice::UpdateUiTexture(AndroidVulkanUiTexture texture,
    const std::uint8_t* rgbaPixels, std::size_t byteCount)
{
    return renderer.UpdateUiTexture(texture, rgbaPixels, byteCount);
}

#include "AndroidVulkanRenderDevice.hpp"
#include "AndroidVulkanRenderServices.hpp"
#include "Layers/xrRenderPC_GL/AndroidModelBridge.h"

#include "Include/xrRender/ParticleCustom.h"
#include "Include/xrRender/RenderVisual.h"
#include "xrEngine/device.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/IGame_Level.h"
#include "xrEngine/Environment.h"
#include "xrEngine/CustomHUD.h"
#include "xrEngine/IRenderable.h"
#include "xrEngine/AndroidTouchControls.hpp"
#include "xrEngine/vis_common.h"
#include "xrCore/FMesh.hpp"
#include "xrCore/LocatorAPI.h"
#include "xrCore/stream_reader.h"
#include "xrCDB/Frustum.h"
#include "xrCDB/ISpatial.h"
#include "xrCDB/xrCDB.h"
#include "Common/LevelStructure.hpp"
#include "Common/d3d9compat.hpp"

#include <SDL.h>
#include <SDL_vulkan.h>
#include <android/log.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <vector>

extern ENGINE_API float psHUD_FOV;

namespace
{
constexpr const char* LogTag = "OpenXRay";
float androidViewDistanceScale = 1.f;

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

struct AndroidLevelSlidingWindow
{
    std::vector<FSlideWindow> windows;
};

struct AndroidLevelMaterial
{
    std::string shaderName;
    std::string baseTexture;
    std::string lightmapTexture;
    std::string hemiTexture;
    std::string detailTexture;
    float detailScale{1.f};
};

#pragma pack(push, 1)
struct AndroidDetailHeader
{
    std::uint32_t version;
    std::uint32_t objectCount;
    std::int32_t offsetX;
    std::int32_t offsetZ;
    std::uint32_t sizeX;
    std::uint32_t sizeZ;
};

struct AndroidDetailPalette
{
    std::uint16_t a0 : 4;
    std::uint16_t a1 : 4;
    std::uint16_t a2 : 4;
    std::uint16_t a3 : 4;
};

struct AndroidDetailSlotRaw
{
    std::uint32_t yBase : 12;
    std::uint32_t yHeight : 8;
    std::uint32_t id0 : 6;
    std::uint32_t id1 : 6;
    std::uint32_t id2 : 6;
    std::uint32_t id3 : 6;
    std::uint32_t directional : 4;
    std::uint32_t hemi : 4;
    std::uint32_t red : 4;
    std::uint32_t green : 4;
    std::uint32_t blue : 4;
    AndroidDetailPalette palette[4];
};
#pragma pack(pop)

static_assert(sizeof(AndroidDetailHeader) == 24);
static_assert(sizeof(AndroidDetailSlotRaw) == 16);

std::uint32_t DetailHash(std::int32_t x, std::int32_t z, std::uint32_t sample)
{
    std::uint32_t value = static_cast<std::uint32_t>(x) * 0x9e3779b9u;
    value ^= static_cast<std::uint32_t>(z) * 0x85ebca6bu;
    value ^= sample * 0xc2b2ae35u;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float InterpolateDetailPalette(const std::array<std::uint8_t, 4>& palette, float x, float z)
{
    const float top = static_cast<float>(palette[0]) * (1.f - x) +
        static_cast<float>(palette[1]) * x;
    const float bottom = static_cast<float>(palette[2]) * (1.f - x) +
        static_cast<float>(palette[3]) * x;
    return top * (1.f - z) + bottom * z;
}

float MaximumTransformScale(const Fmatrix& transform)
{
    const float x = _sqrt(transform._11 * transform._11 + transform._12 * transform._12 +
        transform._13 * transform._13);
    const float y = _sqrt(transform._21 * transform._21 + transform._22 * transform._22 +
        transform._23 * transform._23);
    const float z = _sqrt(transform._31 * transform._31 + transform._32 * transform._32 +
        transform._33 * transform._33);
    return std::max({x, y, z});
}

bool PassesDistanceLimit(const Fvector& center, float radius, float maximumDistance)
{
    if (maximumDistance <= 0.f)
        return true;
    const float expandedDistance = maximumDistance + std::max(radius, 0.f);
    return Device.vCameraPosition.distance_to_sqr(center) <= _sqr(expandedDistance);
}

std::string LowercaseTextureName(const std::string& textureName)
{
    std::string lowered = textureName;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return lowered;
}

bool TextureUsesAlphaTest(const std::string& textureName)
{
    const std::string lowered = LowercaseTextureName(textureName);
    if (lowered.empty())
        return false;

    // Do not infer opacity from the DDS alpha channel: Shadow of Chernobyl
    // also stores gloss and other material masks there. These families are
    // authored as cutout geometry and really do use alpha for empty texels.
    static constexpr std::array<const char*, 12> cutoutTokens = {
        "barbwire", "barbed", "\\fence", "_fence", "\\resh", "_resh",
        "setka", "vetka", "listva", "foliage", "forestwall", "_fur"};
    return std::any_of(cutoutTokens.begin(), cutoutTokens.end(), [&](const char* token)
    {
        return lowered.find(token) != std::string::npos;
    });
}

float ConservativeVisibilityRadius(float radius)
{
    // OGF bounds are authored for the legacy render graph and can be very
    // tight for transformed trees/large compound level visuals.  A modest
    // guard band keeps objects stable at frustum edges while retaining the
    // bulk of the static frustum/distance rejection.
    return radius > EPS_S ? radius * 1.5f + 0.5f : radius;
}

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

float PackedAlphaFraction(const std::uint8_t* source, const D3DVERTEXELEMENT9* element)
{
    if (!element || element->Type != D3DDECLTYPE_D3DCOLOR)
        return 0.f;
    std::uint32_t packed{};
    std::memcpy(&packed, source + element->Offset, sizeof(packed));
    // D3DDECLTYPE_D3DCOLOR reaches the shader as a normalized color. X-Ray
    // stores the fractional part of its compressed base UV in tangent/binormal
    // alpha, so unpack_tc_base adds alpha in the 0..1 range, not the raw byte.
    return static_cast<float>(static_cast<std::uint8_t>(packed >> 24)) / 255.f;
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
        vertex.u = (static_cast<float>(packed[0]) + PackedAlphaFraction(source, tangent)) * scale;
        vertex.v = (static_cast<float>(packed[1]) + PackedAlphaFraction(source, binormal)) * scale;
    }
}

void DecodeLightmapTexcoord(const std::uint8_t* source,
    const std::vector<D3DVERTEXELEMENT9>& declaration, AndroidVulkanWorldVertex& vertex)
{
    const auto* texcoord = FindDeclarationElement(
        declaration, D3DDECLUSAGE_TEXCOORD, 1);
    if (!texcoord)
        return;

    const std::uint8_t* value = source + texcoord->Offset;
    if (texcoord->Type == D3DDECLTYPE_FLOAT2 || texcoord->Type == D3DDECLTYPE_FLOAT3 ||
        texcoord->Type == D3DDECLTYPE_FLOAT4)
    {
        std::memcpy(&vertex.u2, value, sizeof(float) * 2);
        return;
    }

    if (texcoord->Type == D3DDECLTYPE_SHORT2 || texcoord->Type == D3DDECLTYPE_SHORT4)
    {
        std::int16_t packed[2]{};
        std::memcpy(packed, value, sizeof(packed));
        constexpr float scale = 1.0f / 32768.0f;
        vertex.u2 = static_cast<float>(packed[0]) * scale;
        vertex.v2 = static_cast<float>(packed[1]) * scale;
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
            DecodeLightmapTexcoord(source.data(), declaration, vertex);
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

bool ReadLevelSlidingWindows(CStreamReader& geometry,
    std::vector<AndroidLevelSlidingWindow>& slidingWindows)
{
    CStreamReader* chunk = geometry.open_chunk(fsL_SWIS);
    if (!chunk)
        return true;

    const std::uint32_t itemCount = chunk->r_u32();
    slidingWindows.resize(itemCount);
    for (AndroidLevelSlidingWindow& item : slidingWindows)
    {
        // The four reserved values are part of the on-disk format.
        chunk->r_u32();
        chunk->r_u32();
        chunk->r_u32();
        chunk->r_u32();
        const std::uint32_t windowCount = chunk->r_u32();
        if (windowCount > chunk->elapsed() / sizeof(FSlideWindow))
        {
            chunk->close();
            slidingWindows.clear();
            return false;
        }
        item.windows.resize(windowCount);
        if (windowCount != 0)
            chunk->r(item.windows.data(), item.windows.size() * sizeof(FSlideWindow));
    }
    chunk->close();
    return true;
}

bool ReadVisualSlidingWindow(IReader& visual,
    const std::vector<AndroidLevelSlidingWindow>& levelWindows, FSlideWindow& window)
{
    if (visual.find_chunk(OGF_SWIDATA) != 0)
    {
        // Progressive visuals keep their own SWI array. Window zero is the
        // full-resolution range used when LOD is maximal.
        visual.r_u32();
        visual.r_u32();
        visual.r_u32();
        visual.r_u32();
        const std::uint32_t count = visual.r_u32();
        if (count != 0 && visual.elapsed() >= sizeof(FSlideWindow))
        {
            visual.r(&window, sizeof(window));
            return true;
        }
    }

    if (visual.find_chunk(OGF_SWICONTAINER) != 0)
    {
        const std::uint32_t id = visual.r_u32();
        if (id < levelWindows.size() && !levelWindows[id].windows.empty())
        {
            window = levelWindows[id].windows.front();
            return true;
        }
    }
    return false;
}

void AppendCircle(std::vector<AndroidVulkanUiVertex>& vertices, float x, float y,
    float radius, std::uint32_t color, bool filled)
{
    constexpr int segments = 32;
    if (filled)
    {
        for (int segment = 0; segment < segments; ++segment)
        {
            const float a0 = static_cast<float>(segment) * PI_MUL_2 / segments;
            const float a1 = static_cast<float>(segment + 1) * PI_MUL_2 / segments;
            vertices.push_back({x, y, 0.f, color, 0.f, 0.f});
            vertices.push_back({x + std::cos(a0) * radius, y + std::sin(a0) * radius,
                0.f, color, 0.f, 0.f});
            vertices.push_back({x + std::cos(a1) * radius, y + std::sin(a1) * radius,
                0.f, color, 0.f, 0.f});
        }
    }
    else
    {
        for (int segment = 0; segment < segments; ++segment)
        {
            const float a0 = static_cast<float>(segment) * PI_MUL_2 / segments;
            const float a1 = static_cast<float>(segment + 1) * PI_MUL_2 / segments;
            vertices.push_back({x + std::cos(a0) * radius, y + std::sin(a0) * radius,
                0.f, color, 0.f, 0.f});
            vertices.push_back({x + std::cos(a1) * radius, y + std::sin(a1) * radius,
                0.f, color, 0.f, 0.f});
        }
    }
}

void AppendGlyphLine(std::vector<AndroidVulkanUiVertex>& vertices, float cx, float cy,
    float radius, float x0, float y0, float x1, float y1, std::uint32_t color)
{
    vertices.push_back({cx + x0 * radius, cy + y0 * radius, 0.f, color, 0.f, 0.f});
    vertices.push_back({cx + x1 * radius, cy + y1 * radius, 0.f, color, 0.f, 0.f});
}

void AppendTouchGlyph(std::vector<AndroidVulkanUiVertex>& vertices, AndroidTouchAction action,
    float x, float y, float radius, std::uint32_t color)
{
    auto line = [&](float x0, float y0, float x1, float y1)
    {
        AppendGlyphLine(vertices, x, y, radius, x0, y0, x1, y1, color);
    };
    auto letter = [&](char glyph)
    {
        switch (glyph)
        {
        case 'F':
            line(-.30f, .45f, -.30f, -.45f); line(-.30f, -.45f, .34f, -.45f);
            line(-.30f, -.05f, .22f, -.05f); break;
        case 'R':
            line(-.30f, .45f, -.30f, -.45f); line(-.30f, -.45f, .20f, -.45f);
            line(.20f, -.45f, .34f, -.25f); line(.34f, -.25f, .20f, -.05f);
            line(.20f, -.05f, -.30f, -.05f); line(0.f, -.05f, .38f, .45f); break;
        case 'I':
            line(-.28f, -.45f, .28f, -.45f); line(0.f, -.45f, 0.f, .45f);
            line(-.28f, .45f, .28f, .45f); break;
        case 'P':
            line(-.30f, .45f, -.30f, -.45f); line(-.30f, -.45f, .18f, -.45f);
            line(.18f, -.45f, .36f, -.25f); line(.36f, -.25f, .18f, -.05f);
            line(.18f, -.05f, -.30f, -.05f); break;
        case 'L':
            line(-.28f, -.45f, -.28f, .45f); line(-.28f, .45f, .32f, .45f); break;
        case 'X':
            line(-.32f, -.45f, .32f, .45f); line(.32f, -.45f, -.32f, .45f); break;
        case 'Y':
            line(-.34f, -.45f, 0.f, -.04f); line(.34f, -.45f, 0.f, -.04f);
            line(0.f, -.04f, 0.f, .45f); break;
        case 'G':
            line(.30f, -.30f, .08f, -.45f); line(.08f, -.45f, -.28f, -.32f);
            line(-.28f, -.32f, -.34f, .30f); line(-.34f, .30f, .08f, .45f);
            line(.08f, .45f, .32f, .24f); line(.32f, .24f, .32f, .02f);
            line(.32f, .02f, .04f, .02f); break;
        case 'N':
            line(-.30f, .45f, -.30f, -.45f); line(-.30f, -.45f, .30f, .45f);
            line(.30f, .45f, .30f, -.45f); break;
        case 'O':
            line(-.26f, -.45f, .26f, -.45f); line(.26f, -.45f, .36f, .32f);
            line(.36f, .32f, -.26f, .45f); line(-.26f, .45f, -.36f, -.32f);
            line(-.36f, -.32f, -.26f, -.45f); break;
        default: break;
        }
    };
    auto digit = [&](int value)
    {
        const bool a = value != 1 && value != 4;
        const bool b = value != 5 && value != 6;
        const bool c = value != 2;
        const bool d = value != 1 && value != 4 && value != 7;
        const bool e = value == 2 || value == 6 || value == 8;
        const bool f = value == 4 || value == 5 || value == 6 || value == 8 || value == 9;
        const bool g = value != 0 && value != 1 && value != 7;
        if (a) line(-.25f, -.42f, .25f, -.42f);
        if (b) line(.25f, -.42f, .25f, 0.f);
        if (c) line(.25f, 0.f, .25f, .42f);
        if (d) line(-.25f, .42f, .25f, .42f);
        if (e) line(-.25f, 0.f, -.25f, .42f);
        if (f) line(-.25f, -.42f, -.25f, 0.f);
        if (g) line(-.25f, 0.f, .25f, 0.f);
    };
    switch (action)
    {
    case AndroidTouchAction::Fire:
        line(-.45f, 0.f, .45f, 0.f); line(0.f, -.45f, 0.f, .45f);
        break;
    case AndroidTouchAction::Aim:
        line(-.45f, 0.f, -.16f, 0.f); line(.16f, 0.f, .45f, 0.f);
        line(0.f, -.45f, 0.f, -.16f); line(0.f, .16f, 0.f, .45f);
        break;
    case AndroidTouchAction::Jump:
        line(0.f, .45f, 0.f, -.45f); line(0.f, -.45f, -.28f, -.12f);
        line(0.f, -.45f, .28f, -.12f);
        break;
    case AndroidTouchAction::Sprint:
        letter('X');
        break;
    case AndroidTouchAction::Use:
        letter('F');
        break;
    case AndroidTouchAction::Crouch:
        line(.35f, -.4f, -.15f, -.4f); line(-.15f, -.4f, -.4f, 0.f);
        line(-.4f, 0.f, -.15f, .4f); line(-.15f, .4f, .35f, .4f);
        break;
    case AndroidTouchAction::Reload:
        letter('R');
        break;
    case AndroidTouchAction::NextAmmo:
        letter('Y');
        break;
    case AndroidTouchAction::Drop:
        letter('G');
        break;
    case AndroidTouchAction::Inventory:
        letter('I');
        break;
    case AndroidTouchAction::Pda:
        letter('P');
        break;
    case AndroidTouchAction::Flashlight:
        letter('L');
        break;
    case AndroidTouchAction::NightVision:
        letter('N');
        break;
    case AndroidTouchAction::Detector:
        letter('O');
        break;
    case AndroidTouchAction::Slot1: digit(1); break;
    case AndroidTouchAction::Slot2: digit(2); break;
    case AndroidTouchAction::Slot3: digit(3); break;
    case AndroidTouchAction::Slot4: digit(4); break;
    case AndroidTouchAction::Slot5: digit(5); break;
    case AndroidTouchAction::Slot6: digit(6); break;
        break;
    case AndroidTouchAction::Pause:
        line(-.18f, -.45f, -.18f, .45f); line(.18f, -.45f, .18f, .45f);
        break;
    default: break;
    }
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

void AndroidSetViewDistanceScale(float scale)
{
    androidViewDistanceScale = std::clamp(scale, .4f, 1.f);
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
    DestroyAndroidTextureDetailManager(textureDetails);
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

void AndroidVulkanRenderDevice::LoadLevelDetails()
{
    ClearLevelDetails();
    IReader* details = FS.r_open("$level$", "level.details");
    if (!details)
        return;

    AndroidDetailHeader header{};
    if (!details->r_chunk_safe(0, &header, sizeof(header)) || header.version != 3 ||
        header.objectCount == 0 || header.objectCount > 63 || header.sizeX == 0 || header.sizeZ == 0)
    {
        __android_log_write(ANDROID_LOG_WARN, LogTag,
            "Android Vulkan: level.details has an unsupported header");
        FS.r_close(details);
        return;
    }

    IReader* objects = details->open_chunk(1);
    IReader* slots = details->open_chunk(2);
    const std::uint64_t slotCount = static_cast<std::uint64_t>(header.sizeX) * header.sizeZ;
    if (!objects || !slots || slotCount > std::numeric_limits<std::uint32_t>::max() ||
        slots->length() < slotCount * sizeof(AndroidDetailSlotRaw))
    {
        if (objects)
            objects->close();
        if (slots)
            slots->close();
        FS.r_close(details);
        __android_log_write(ANDROID_LOG_WARN, LogTag,
            "Android Vulkan: level.details is incomplete");
        return;
    }

    detailModels.reserve(header.objectCount);
    bool modelsValid = true;
    for (std::uint32_t modelIndex = 0; modelIndex < header.objectCount; ++modelIndex)
    {
        IReader* source = objects->open_chunk(modelIndex);
        if (!source)
        {
            modelsValid = false;
            break;
        }

        string256 shaderName{};
        string256 textureName{};
        source->r_stringZ(shaderName, sizeof(shaderName));
        source->r_stringZ(textureName, sizeof(textureName));
        source->r_u32(); // detail flags (waving is added after static details)

        DetailModel model{};
        model.textureName = textureName;
        model.minimumScale = source->r_float();
        model.maximumScale = source->r_float();
        const std::uint32_t vertexCount = source->r_u32();
        const std::uint32_t indexCount = source->r_u32();
        const std::uint64_t geometryBytes = static_cast<std::uint64_t>(vertexCount) *
            sizeof(DetailModel::Vertex) + static_cast<std::uint64_t>(indexCount) *
            sizeof(std::uint16_t);
        if (vertexCount == 0 || indexCount == 0 || vertexCount > 65535 ||
            indexCount > 1024 * 1024 || geometryBytes > static_cast<std::uint64_t>(source->elapsed()))
        {
            source->close();
            modelsValid = false;
            break;
        }
        static_assert(sizeof(DetailModel::Vertex) == sizeof(Fvector) + sizeof(float) * 2);
        model.vertices.resize(vertexCount);
        model.indices.resize(indexCount);
        source->r(model.vertices.data(), model.vertices.size() * sizeof(DetailModel::Vertex));
        source->r(model.indices.data(), model.indices.size() * sizeof(std::uint16_t));
        source->close();
        detailModels.push_back(std::move(model));
    }
    objects->close();

    if (modelsValid && detailModels.size() == header.objectCount)
    {
        detailSlots.resize(static_cast<std::size_t>(slotCount));
        for (DetailSlot& destination : detailSlots)
        {
            AndroidDetailSlotRaw source{};
            slots->r(&source, sizeof(source));
            destination.minimumY = static_cast<float>(source.yBase) * .2f - 200.f;
            destination.height = static_cast<float>(source.yHeight) * .1f;
            destination.modelIds = {static_cast<std::uint8_t>(source.id0),
                static_cast<std::uint8_t>(source.id1), static_cast<std::uint8_t>(source.id2),
                static_cast<std::uint8_t>(source.id3)};
            for (std::size_t paletteIndex = 0; paletteIndex < destination.palette.size(); ++paletteIndex)
            {
                const AndroidDetailPalette& palette = source.palette[paletteIndex];
                destination.palette[paletteIndex] = {static_cast<std::uint8_t>(palette.a0),
                    static_cast<std::uint8_t>(palette.a1), static_cast<std::uint8_t>(palette.a2),
                    static_cast<std::uint8_t>(palette.a3)};
            }
        }
        detailSlotsX = header.sizeX;
        detailSlotsZ = header.sizeZ;
        detailOffsetX = header.offsetX;
        detailOffsetZ = header.offsetZ;
        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "Android Vulkan: loaded level details: %zu models, %zu slots",
            detailModels.size(), detailSlots.size());
    }
    else
    {
        detailModels.clear();
        __android_log_write(ANDROID_LOG_WARN, LogTag,
            "Android Vulkan: failed to decode detail models");
    }

    slots->close();
    FS.r_close(details);
}

void AndroidVulkanRenderDevice::UpdateLevelDetails()
{
    if (detailModels.empty() || detailSlots.empty() || !g_pGameLevel)
        return;

    constexpr float slotSize = 2.f;
    constexpr std::int32_t slotRadius = 12;
    const std::int32_t centerX = iFloor(Device.vCameraPosition.x / slotSize + .5f) + detailOffsetX;
    const std::int32_t centerZ = iFloor(Device.vCameraPosition.z / slotSize + .5f) + detailOffsetZ;
    // Keep a generous 24 m detail ring and rebuild it only after the camera
    // moves 10 m. Rebuilding performs collision rays, CPU mesh generation and
    // GPU resource replacement, so doing it every 4-6 m caused visible frame
    // spikes while simply walking through a level.
    if (detailCenterValid && std::abs(centerX - detailCenterX) < 5 &&
        std::abs(centerZ - detailCenterZ) < 5)
    {
        return;
    }
    detailCenterX = centerX;
    detailCenterZ = centerZ;
    detailCenterValid = true;

    std::vector<AndroidVulkanWorldMesh> obsoleteMeshes;
    obsoleteMeshes.reserve(detailModels.size());
    for (DetailModel& model : detailModels)
    {
        if (model.mesh != 0)
            obsoleteMeshes.push_back(model.mesh);
        model.mesh = 0;
        model.drawIndexCount = 0;
    }
    renderer.DestroyWorldMeshes(obsoleteMeshes.data(), obsoleteMeshes.size());

    std::vector<std::vector<AndroidVulkanWorldVertex>> generatedVertices(detailModels.size());
    std::vector<std::vector<std::uint16_t>> generatedIndices(detailModels.size());
    CDB::COLLIDER groundCollider;
    const Fvector down = Fvector().set(0.f, -1.f, 0.f);
    const CDB::MODEL* collision = g_pGameLevel->ObjectSpace.GetStaticModel();
    std::size_t instanceCount = 0;

    const std::int32_t minimumSlotX = std::max(0, centerX - slotRadius);
    const std::int32_t maximumSlotX = std::min(static_cast<std::int32_t>(detailSlotsX) - 1,
        centerX + slotRadius);
    const std::int32_t minimumSlotZ = std::max(0, centerZ - slotRadius);
    const std::int32_t maximumSlotZ = std::min(static_cast<std::int32_t>(detailSlotsZ) - 1,
        centerZ + slotRadius);
    constexpr std::array<float, 4> sampleX{.22f, .72f, .28f, .78f};
    constexpr std::array<float, 4> sampleZ{.25f, .30f, .75f, .70f};

    for (std::int32_t slotZ = minimumSlotZ; slotZ <= maximumSlotZ; ++slotZ)
    {
        for (std::int32_t slotX = minimumSlotX; slotX <= maximumSlotX; ++slotX)
        {
            const DetailSlot& slot = detailSlots[static_cast<std::size_t>(slotZ) * detailSlotsX + slotX];
            const float slotMinimumX = static_cast<float>(slotX - detailOffsetX) * slotSize;
            const float slotMinimumZ = static_cast<float>(slotZ - detailOffsetZ) * slotSize;
            for (std::uint32_t sample = 0; sample < sampleX.size(); ++sample)
            {
                std::size_t selectedLayer = 4;
                float selectedWeight = 0.f;
                for (std::size_t layer = 0; layer < 4; ++layer)
                {
                    if (slot.modelIds[layer] >= detailModels.size())
                        continue;
                    const float weight = InterpolateDetailPalette(
                        slot.palette[layer], sampleX[sample], sampleZ[sample]);
                    if (weight > selectedWeight)
                    {
                        selectedWeight = weight;
                        selectedLayer = layer;
                    }
                }
                if (selectedLayer == 4 || selectedWeight <= 0.f)
                    continue;

                const std::uint32_t hash = DetailHash(slotX, slotZ, sample);
                const float selection = static_cast<float>(hash & 0xffffu) / 65535.f;
                if (selection > selectedWeight / 15.f)
                    continue;

                const std::size_t modelIndex = slot.modelIds[selectedLayer];
                const DetailModel& model = detailModels[modelIndex];
                auto& vertices = generatedVertices[modelIndex];
                auto& indices = generatedIndices[modelIndex];
                if (vertices.size() + model.vertices.size() > 65535)
                    continue;

                const float jitterX = (static_cast<float>((hash >> 16) & 0xffu) / 255.f - .5f) * .34f;
                const float jitterZ = (static_cast<float>((hash >> 24) & 0xffu) / 255.f - .5f) * .34f;
                const float worldX = slotMinimumX + sampleX[sample] * slotSize + jitterX;
                const float worldZ = slotMinimumZ + sampleZ[sample] * slotSize + jitterZ;
                Fvector rayStart;
                rayStart.set(worldX, slot.minimumY + slot.height + 2.f, worldZ);
                groundCollider.ray_query(CDB::OPT_ONLYNEAREST, collision, rayStart, down,
                    slot.height + 4.f);
                if (groundCollider.r_count() == 0)
                    continue;
                const float worldY = rayStart.y - groundCollider.r_begin()->range;

                const float scaleWeight = static_cast<float>((hash >> 8) & 0xffffu) / 65535.f;
                const float scale = model.minimumScale +
                    (model.maximumScale - model.minimumScale) * scaleWeight;
                const float angle = static_cast<float>(hash) / static_cast<float>(
                    std::numeric_limits<std::uint32_t>::max()) * PI_MUL_2;
                const float sine = std::sin(angle);
                const float cosine = std::cos(angle);
                const std::uint16_t vertexBase = static_cast<std::uint16_t>(vertices.size());
                for (const DetailModel::Vertex& source : model.vertices)
                {
                    AndroidVulkanWorldVertex vertex{};
                    vertex.x = worldX + (source.position.x * cosine + source.position.z * sine) * scale;
                    vertex.y = worldY + source.position.y * scale;
                    vertex.z = worldZ + (-source.position.x * sine + source.position.z * cosine) * scale;
                    vertex.ny = 1.f;
                    vertex.u = source.u;
                    vertex.v = source.v;
                    vertices.push_back(vertex);
                }
                for (std::uint16_t index : model.indices)
                    indices.push_back(static_cast<std::uint16_t>(vertexBase + index));
                ++instanceCount;
            }
        }
    }

    for (std::size_t modelIndex = 0; modelIndex < detailModels.size(); ++modelIndex)
    {
        DetailModel& model = detailModels[modelIndex];
        auto& vertices = generatedVertices[modelIndex];
        auto& indices = generatedIndices[modelIndex];
        if (vertices.empty() || indices.empty())
            continue;
        if (model.texture == 0 && !model.textureName.empty())
        {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            AndroidLoadVulkanTexture(*this, model.textureName.c_str(), model.texture, width, height);
        }
        model.mesh = renderer.CreateWorldMesh(
            vertices.data(), vertices.size(), indices.data(), indices.size());
        if (model.mesh != 0)
            model.drawIndexCount = static_cast<std::uint32_t>(indices.size());
    }

    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "Android Vulkan: rebuilt nearby detail geometry: %zu instances around slot %d,%d",
        instanceCount, centerX, centerZ);
}

void AndroidVulkanRenderDevice::ClearLevelDetails()
{
    std::vector<AndroidVulkanWorldMesh> obsoleteMeshes;
    obsoleteMeshes.reserve(detailModels.size());
    for (DetailModel& model : detailModels)
    {
        if (model.mesh != 0)
            obsoleteMeshes.push_back(model.mesh);
    }
    renderer.DestroyWorldMeshes(obsoleteMeshes.data(), obsoleteMeshes.size());
    detailModels.clear();
    detailSlots.clear();
    detailSlotsX = 0;
    detailSlotsZ = 0;
    detailOffsetX = 0;
    detailOffsetZ = 0;
    detailCenterValid = false;
}

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
    std::vector<AndroidLevelSlidingWindow> slidingWindows;
    std::vector<AndroidVulkanWorldVertex> vertices;
    std::vector<std::uint16_t> indices;
    const bool geometryLoaded = ReadLevelVertexBuffers(*geometry, vertexBuffers, vertices) &&
        ReadLevelIndexBuffers(*geometry, indexBuffers, indices) &&
        ReadLevelSlidingWindows(*geometry, slidingWindows);
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

    std::vector<AndroidLevelMaterial> materials;
    if (IReader* shaders = level->open_chunk(fsL_SHADERS))
    {
        const std::uint32_t shaderCount = shaders->r_u32();
        materials.resize(shaderCount);
        for (std::uint32_t shaderIndex = 0; shaderIndex < shaderCount; ++shaderIndex)
        {
            string512 material{};
            shaders->r_stringZ(material, sizeof(material));
            const char* textureList = std::strchr(material, '/');
            if (!textureList || !textureList[1])
                continue;
            materials[shaderIndex].shaderName.assign(material,
                static_cast<std::size_t>(textureList - material));
            ++textureList;
            const char* comma = std::strchr(textureList, ',');
            materials[shaderIndex].baseTexture.assign(textureList,
                comma ? static_cast<std::size_t>(comma - textureList) : std::strlen(textureList));
            if (comma && comma[1])
            {
                const char* lightmap = comma + 1;
                const char* nextComma = std::strchr(lightmap, ',');
                materials[shaderIndex].lightmapTexture.assign(lightmap,
                    nextComma ? static_cast<std::size_t>(nextComma - lightmap) :
                                std::strlen(lightmap));
                if (nextComma && nextComma[1])
                {
                    const char* hemi = nextComma + 1;
                    const char* hemiEnd = std::strchr(hemi, ',');
                    materials[shaderIndex].hemiTexture.assign(hemi,
                        hemiEnd ? static_cast<std::size_t>(hemiEnd - hemi) :
                                  std::strlen(hemi));
                }
            }
            AndroidFindTextureDetail(textureDetails, materials[shaderIndex].baseTexture.c_str(),
                materials[shaderIndex].detailTexture, materials[shaderIndex].detailScale);
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
                draw.transform.identity();
                // Mark immutable level geometry for the static pipeline. Some
                // compiled visuals reverse their effective winding after
                // transforms, so that pipeline is deliberately two-sided.
                draw.materialFlags = 32u;
                if (hasHeader)
                {
                    draw.boundsCenter.set(header.bs.c);
                    draw.boundsRadius = header.bs.r;
                }
                draw.firstIndex = indexBuffers[indexBuffer].combinedBase + indexBase;
                draw.indexCount = indexCount;
                draw.vertexOffset = static_cast<std::int32_t>(
                    vertexBuffers[vertexBuffer].combinedBase + vertexBase);
                if (hasHeader && (header.type == MT_PROGRESSIVE || header.type == MT_TREE_PM))
                {
                    FSlideWindow window{};
                    if (ReadVisualSlidingWindow(*visual, slidingWindows, window) &&
                        window.offset <= indexCount &&
                        static_cast<std::uint32_t>(window.num_tris) * 3u <= indexCount - window.offset)
                    {
                        draw.firstIndex += window.offset;
                        draw.indexCount = static_cast<std::uint32_t>(window.num_tris) * 3u;
                    }
                    else
                    {
                        __android_log_print(ANDROID_LOG_WARN, LogTag,
                            "Android Vulkan: visual %u has no valid full-resolution sliding window",
                            visualIndex);
                        visual->close();
                        continue;
                    }
                }
                if (hasHeader && (header.type == MT_TREE_ST || header.type == MT_TREE_PM) &&
                    visual->find_chunk(OGF_TREEDEF2) != 0)
                {
                    visual->r(&draw.transform, sizeof(draw.transform));
                    draw.materialFlags |= 8u | 16u;
                    const Fvector localCenter = draw.boundsCenter;
                    draw.transform.transform_tiny(draw.boundsCenter, localCenter);
                    draw.boundsRadius *= MaximumTransformScale(draw.transform);
                }
                if (hasHeader && header.shader_id < materials.size())
                {
                    const AndroidLevelMaterial& material = materials[header.shader_id];
                    if (material.shaderName.find("aref") != std::string::npos ||
                        material.shaderName.find("alpha") != std::string::npos ||
                        TextureUsesAlphaTest(material.baseTexture))
                    {
                        draw.materialFlags |= 8u | 16u;
                    }
                    if (!material.baseTexture.empty())
                    {
                        std::uint32_t textureWidth = 0;
                        std::uint32_t textureHeight = 0;
                        AndroidLoadVulkanTexture(*this, material.baseTexture.c_str(),
                            draw.texture, textureWidth, textureHeight);
                    }
                    if (!material.lightmapTexture.empty())
                    {
                        std::uint32_t textureWidth = 0;
                        std::uint32_t textureHeight = 0;
                        if (AndroidLoadVulkanTexture(*this, material.lightmapTexture.c_str(),
                                draw.lightmapTexture, textureWidth, textureHeight))
                        {
                            draw.materialFlags |= 1u;
                        }
                    }
                    if (!material.hemiTexture.empty())
                    {
                        std::uint32_t textureWidth = 0;
                        std::uint32_t textureHeight = 0;
                        if (AndroidLoadVulkanTexture(*this, material.hemiTexture.c_str(),
                                draw.hemiTexture, textureWidth, textureHeight))
                        {
                            draw.materialFlags |= 2u;
                        }
                    }
                    if (!material.detailTexture.empty())
                    {
                        std::uint32_t textureWidth = 0;
                        std::uint32_t textureHeight = 0;
                        if (AndroidLoadVulkanTexture(*this, material.detailTexture.c_str(),
                                draw.detailTexture, textureWidth, textureHeight))
                        {
                            draw.detailScale = material.detailScale;
                            draw.materialFlags |= 64u;
                        }
                    }
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

    // The level file is organized by visual hierarchy rather than render
    // state. Group immutable static draws by pipeline and material once at
    // load time. Alpha-tested foliage is depth-writing (not blended), so this
    // reordering is safe and substantially reduces descriptor/pipeline churn
    // on tile-based mobile GPUs.
    std::stable_sort(levelDraws.begin(), levelDraws.end(), [](const LevelDraw& left, const LevelDraw& right)
    {
        if (left.materialFlags != right.materialFlags)
            return left.materialFlags < right.materialFlags;
        if (left.texture != right.texture)
            return left.texture < right.texture;
        if (left.lightmapTexture != right.lightmapTexture)
            return left.lightmapTexture < right.lightmapTexture;
        if (left.hemiTexture != right.hemiTexture)
            return left.hemiTexture < right.hemiTexture;
        if (left.detailTexture != right.detailTexture)
            return left.detailTexture < right.detailTexture;
        if (left.detailScale != right.detailScale)
            return left.detailScale < right.detailScale;
        if (left.vertexOffset != right.vertexOffset)
            return left.vertexOffset < right.vertexOffset;
        return left.firstIndex < right.firstIndex;
    });

    levelMesh = renderer.CreateWorldMesh(vertices.data(), vertices.size(), indices.data(), indices.size());
    if (levelMesh == 0)
    {
        levelDraws.clear();
        __android_log_write(ANDROID_LOG_ERROR, LogTag,
            "Android Vulkan: failed to upload static level geometry");
        return;
    }

    const std::size_t detailedDraws = std::count_if(levelDraws.begin(), levelDraws.end(),
        [](const LevelDraw& draw) { return (draw.materialFlags & 64u) != 0u; });
    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "Android Vulkan: uploaded level geometry: %zu vertices, %zu indices, %zu draws (%zu detailed)",
        vertices.size(), indices.size(), levelDraws.size(), detailedDraws);
    LoadLevelDetails();
    AndroidTouchControlsSetGameplayActive(true);
}

void AndroidVulkanRenderDevice::level_Unload()
{
    AndroidTouchControlsSetGameplayActive(false);
    if (levelMesh != 0)
        renderer.DestroyWorldMesh(levelMesh);
    levelMesh = 0;
    levelDraws.clear();
    ClearLevelDetails();
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
    if (!visual || !initialized)
        return;

    const bool hud = root && root->renderable_HUD();
    if (root)
        AndroidRenderObjectUpdate(root->renderable_ROS(), root);

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
            cached.uploadedVertices.assign(
                reinterpret_cast<const AndroidVulkanWorldVertex*>(part.vertices.data()),
                reinterpret_cast<const AndroidVulkanWorldVertex*>(part.vertices.data()) + part.vertices.size());
            if (!part.texture.empty())
            {
                std::uint32_t textureWidth = 0;
                std::uint32_t textureHeight = 0;
                AndroidLoadVulkanTexture(*this, part.texture.c_str(), cached.texture,
                    textureWidth, textureHeight);
                if (TextureUsesAlphaTest(part.texture))
                    cached.materialFlags |= 8u | 16u;
            }
            __android_log_print(ANDROID_LOG_INFO, LogTag,
                "Android Vulkan: uploaded dynamic model part: %u vertices, %u indices, texture=%s",
                cached.vertexCount, cached.indexCount,
                part.texture.empty() ? "<none>" : part.texture.c_str());
        }
        else
        {
            const auto* vertices = reinterpret_cast<const AndroidVulkanWorldVertex*>(part.vertices.data());
            const std::size_t byteCount = part.vertices.size() * sizeof(AndroidVulkanWorldVertex);
            const bool changed = cached.uploadedVertices.size() != part.vertices.size() ||
                std::memcmp(cached.uploadedVertices.data(), vertices, byteCount) != 0;
            if (changed)
            {
                if (!renderer.UpdateWorldMesh(cached.mesh, vertices, part.vertices.size()))
                    continue;
                cached.uploadedVertices.assign(vertices, vertices + part.vertices.size());
            }
        }

        DynamicDraw draw{};
        draw.mesh = cached.mesh;
        draw.indexCount = cached.indexCount;
        draw.texture = cached.texture;
        draw.materialFlags = cached.materialFlags;
        draw.hud = hud;
        Fmatrix fullTransform;
        if (draw.hud)
        {
            const float farPlane = g_pGamePersistent ?
                g_pGamePersistent->Environment().CurrentEnv.far_plane : VIEWPORT_NEAR + 500.f;
            Fmatrix hudProjection;
            hudProjection.build_projection(deg2rad(psHUD_FOV * Device.fFOV),
                Device.fASPECT, HUD_VIEWPORT_NEAR, farPlane);
            fullTransform.mul(hudProjection, Device.mView);
        }
        else
        {
            fullTransform.set(Device.mFullTransform);
        }
        Fmatrix worldViewProjection;
        worldViewProjection.mul(fullTransform, transform);
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
    if (visual)
    {
        // Release the cached GPU mesh before the visual is destroyed so the map
        // never holds a dangling IRenderVisual* key, and the buffers are freed
        // when the object despawns instead of accumulating until level unload.
        auto mesh = dynamicMeshes.find(visual);
        if (mesh != dynamicMeshes.end())
        {
            if (mesh->second.mesh != 0)
                renderer.DestroyWorldMesh(mesh->second.mesh);
            dynamicMeshes.erase(mesh);
        }
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

    maximumWorldDistance = g_pGamePersistent->Environment().CurrentEnv.far_plane *
        androidViewDistanceScale;

    UpdateLevelDetails();

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

    // The regular render graph invokes the HUD after world renderables. The
    // Android renderer owns its graph, so submit the real hands/weapon visuals
    // explicitly instead of leaving the weapon slot as UI-only state.
    if (g_pGameLevel && g_pGameLevel->pHUD)
        g_pGameLevel->pHUD->Render_Last(PrimaryContext);
}
void AndroidVulkanRenderDevice::Render()
{
    CFrustum view;
    view.CreateFromMatrix(Device.mFullTransform, FRUSTUM_P_ALL);
    std::size_t visibleLevelDraws = 0;
    if (levelMesh != 0)
    {
        for (const auto& draw : levelDraws)
        {
            const float visibilityRadius = ConservativeVisibilityRadius(draw.boundsRadius);
            if (draw.boundsRadius > EPS_S &&
                !view.testSphere_dirty(draw.boundsCenter, visibilityRadius))
            {
                continue;
            }
            if (!PassesDistanceLimit(draw.boundsCenter, visibilityRadius, maximumWorldDistance))
                continue;
            Fmatrix worldViewProjection;
            worldViewProjection.mul(Device.mFullTransform, draw.transform);
            renderer.SubmitWorldMesh(levelMesh, draw.firstIndex, draw.indexCount,
                draw.vertexOffset, &worldViewProjection._11, draw.texture,
                draw.lightmapTexture, draw.hemiTexture, draw.detailTexture,
                draw.detailScale, draw.materialFlags);
            ++visibleLevelDraws;
        }
    }
    for (const DetailModel& model : detailModels)
    {
        if (model.mesh != 0 && model.drawIndexCount != 0)
            renderer.SubmitWorldMesh(model.mesh, 0, model.drawIndexCount, 0,
                &Device.mFullTransform._11, model.texture, 0, 0, 0, 1.f, 8u | 16u);
    }
    for (const DynamicDraw& draw : dynamicDraws)
        renderer.SubmitWorldMesh(draw.mesh, 0, draw.indexCount, 0,
            draw.worldViewProjection, draw.texture, 0, 0, 0, 1.f,
            draw.materialFlags | (draw.hud ? 4u : 0u));

    if (frameIndex != 0 && frameIndex % 300 == 0)
    {
        __android_log_print(ANDROID_LOG_DEBUG, LogTag,
            "Android visibility: %zu/%zu static level draws submitted",
            visibleLevelDraws, levelDraws.size());
    }
}
void AndroidVulkanRenderDevice::RenderMenu()
{
    // The ShoC main menu normally renders into the desktop renderer's
    // post-process UI target. There is no intermediate target in the Android
    // bootstrap yet, so submit that main pass directly to this frame. Android
    // can expose the physical View size through Device while Vulkan renders
    // into a lower-resolution fixed-size Surface. pp_start() uses Device's
    // dimensions, so temporarily give it the actual raster extent; otherwise
    // only this menu pass is scaled twice and is clipped at the lower-right.
    if (!g_pGamePersistent)
        return;

    std::uint32_t rasterWidth = 0;
    std::uint32_t rasterHeight = 0;
    renderer.GetSurfaceSize(rasterWidth, rasterHeight);
    if (rasterWidth == 0 || rasterHeight == 0 ||
        (Device.dwWidth == rasterWidth && Device.dwHeight == rasterHeight))
    {
        g_pGamePersistent->OnRenderPPUI_main();
        return;
    }

    const u32 savedWidth = Device.dwWidth;
    const u32 savedHeight = Device.dwHeight;
    const float savedHalfWidth = Device.fWidth_2;
    const float savedHalfHeight = Device.fHeight_2;
    Device.dwWidth = rasterWidth;
    Device.dwHeight = rasterHeight;
    Device.fWidth_2 = static_cast<float>(rasterWidth) * 0.5f;
    Device.fHeight_2 = static_cast<float>(rasterHeight) * 0.5f;
    g_pGamePersistent->OnRenderPPUI_main();
    Device.dwWidth = savedWidth;
    Device.dwHeight = savedHeight;
    Device.fWidth_2 = savedHalfWidth;
    Device.fHeight_2 = savedHalfHeight;
}
void AndroidVulkanRenderDevice::BeforeWorldRender()
{
    if (!g_pGamePersistent)
        return;
    const Fvector3& sky = g_pGamePersistent->Environment().CurrentEnv.sky_color;
    renderer.SetWorldClearColor(sky.x, sky.y, sky.z);
    const CEnvDescriptor& environment = g_pGamePersistent->Environment().CurrentEnv;
    renderer.SetWorldLighting(&environment.ambient.x, &environment.hemi_color.x,
        &environment.sun_color.x, &environment.sun_dir.x);
}
void AndroidVulkanRenderDevice::AfterWorldRender() {}
void AndroidVulkanRenderDevice::Screenshot(ScreenshotMode, pcstr) {}
void AndroidVulkanRenderDevice::SetPostProcessParams(const SPPInfo&) {}
void AndroidVulkanRenderDevice::setGamma(float) {}
void AndroidVulkanRenderDevice::setBrightness(float) {}
void AndroidVulkanRenderDevice::setContrast(float) {}
void AndroidVulkanRenderDevice::updateGamma() {}
void AndroidVulkanRenderDevice::OnDeviceDestroy(bool)
{
    DestroyAndroidTextureDetailManager(textureDetails);
}

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
    // Batch every handle so DestroyWorldMeshes performs a single device idle
    // instead of one full stall per dynamic mesh.
    std::vector<AndroidVulkanWorldMesh> handles;
    handles.reserve(dynamicMeshes.size());
    for (const auto& [visual, mesh] : dynamicMeshes)
    {
        (void)visual;
        if (mesh.mesh != 0)
            handles.push_back(mesh.mesh);
    }
    if (!handles.empty())
        renderer.DestroyWorldMeshes(handles.data(), handles.size());
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
    if (!initialized)
    {
        Create(targetWindow, width, height, halfWidth, halfHeight);
        return;
    }

    window = targetWindow;
    UpdateDimensions(window, width, height, halfWidth, halfHeight);
    failed = !renderer.RecreateSurface(window);
    initialized = !failed;
    if (initialized)
    {
        renderer.GetSurfaceSize(width, height);
        halfWidth = static_cast<float>(width) * 0.5f;
        halfHeight = static_cast<float>(height) * 0.5f;
        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "Android Vulkan surface and GPU resources restored: %ux%u", width, height);
    }
    else
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag,
            "Android Vulkan surface restoration failed");
    }
    frameIndex = 0;
    frameLoopStart = SDL_GetPerformanceCounter();
}

void AndroidVulkanRenderDevice::ObtainRequiredWindowFlags(u32& windowFlags) { windowFlags |= SDL_WINDOW_VULKAN; }
void AndroidVulkanRenderDevice::SetupStates() {}
void AndroidVulkanRenderDevice::OnDeviceCreate(pcstr)
{
    DestroyAndroidTextureDetailManager(textureDetails);
    textureDetails = CreateAndroidTextureDetailManager();
}
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
    SubmitTouchControls();
    failed = !renderer.DrawFrame(frameIndex, elapsedSeconds);
}

void AndroidVulkanRenderDevice::SubmitTouchControls()
{
    AndroidTouchControlsSnapshot controls{};
    if (!AndroidTouchControlsGetSnapshot(controls))
        return;

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    renderer.GetSurfaceSize(width, height);
    if (width == 0 || height == 0)
        return;
    const float scale = static_cast<float>(std::min(width, height));

    std::vector<AndroidVulkanUiVertex> fill;
    std::vector<AndroidVulkanUiVertex> lines;
    const float stickX = controls.stickX * width;
    const float stickY = controls.stickY * height;
    const float stickRadius = controls.stickRadius * scale;
    AppendCircle(fill, stickX, stickY, stickRadius, 0x30283838u, true);
    AppendCircle(lines, stickX, stickY, stickRadius, 0xA0D8E0E0u, false);
    const float knobX = controls.knobX * width;
    const float knobY = controls.knobY * height;
    AppendCircle(fill, knobX, knobY, stickRadius * .42f, 0x7048A8B8u, true);
    AppendCircle(lines, knobX, knobY, stickRadius * .42f, 0xD0E8FFFFu, false);

    for (const auto& button : controls.buttons)
    {
        const float x = button.x * width;
        const float y = button.y * height;
        const float radius = button.radius * scale;
        const std::uint32_t fillColor = button.pressed ? 0xA078B8C8u : 0x40303838u;
        AppendCircle(fill, x, y, radius, fillColor, true);
        AppendCircle(lines, x, y, radius, button.pressed ? 0xFFF4FFFFu : 0xB0D8E0E0u, false);
        AppendTouchGlyph(lines, button.action, x, y, radius,
            button.pressed ? 0xFFFFFFFFu : 0xD0E8F0F0u);
    }

    if (!fill.empty())
        SubmitUiBatch(fill.data(), fill.size(), AndroidVulkanUiPrimitive::TriangleList, {}, 0);
    if (!lines.empty())
        SubmitUiBatch(lines.data(), lines.size(), AndroidVulkanUiPrimitive::LineList, {}, 0);
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

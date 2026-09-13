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
#include "xrCore/FS.h"
#include "xrCore/Text/StringConversion.hpp"
#include "xrEngine/GameFont.h"
#include "xrEngine/device.h"
#include "xrEngine/xrTheora_Surface.h"
#include "xrEngine/xr_level_controller.h"

#include <algorithm>
#include <android/log.h>
#include <gli/convert.hpp>
#include <gli/load.hpp>
#include <gli/texture2d.hpp>
#include <imgui.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern ENGINE_API Fvector2 g_current_font_scale;

class AndroidVulkanFontRenderAccess
{
public:
    static void Render(AndroidVulkanRenderDevice& device, AndroidVulkanUiTexture texture,
        AndroidVulkanUiTextureMode textureMode, std::uint32_t textureWidth,
        std::uint32_t textureHeight, CGameFont& owner);
};

namespace
{
constexpr const char* LogTag = "OpenXRay";

bool LoadUiTexture(AndroidVulkanRenderDevice& device, const char* requestedName,
    AndroidVulkanUiTexture& texture, std::uint32_t& width, std::uint32_t& height)
{
    texture = 0;
    width = 0;
    height = 0;
    if (!requestedName || !requestedName[0] || xr_strcmp(requestedName, "$null") == 0)
        return false;

    string_path textureName;
    xr_strcpy(textureName, requestedName);
    if (char* extension = strext(textureName))
        *extension = 0;

    string_path path;
    string_path resolvedTextureName;
    xr_strcpy(resolvedTextureName, textureName);
    bool found = false;
    for (cpcstr folder : {"$level$", "$game_saves$", "$game_textures$"})
    {
        if (FS.exist(path, folder, resolvedTextureName, ".dds"))
        {
            found = true;
            break;
        }
    }
    if (!found && FS.exist(path, "$game_textures$", textureName, ".seq"))
    {
        IReader* sequence = FS.r_open(path);
        string256 line{};
        if (sequence)
        {
            // Sequence files contain an optional "cycled" marker, an FPS
            // line and then the texture names. A static first frame is enough
            // for the Android cursor until animated texture state is added.
            sequence->r_string(line, sizeof(line));
            _Trim(line);
            if (xr_stricmp(line, "cycled") == 0 && !sequence->eof())
                sequence->r_string(line, sizeof(line));
            while (!sequence->eof())
            {
                sequence->r_string(line, sizeof(line));
                _Trim(line);
                if (line[0])
                {
                    xr_strcpy(resolvedTextureName, line);
                    break;
                }
            }
            FS.r_close(sequence);
        }
        if (resolvedTextureName[0] && xr_strcmp(resolvedTextureName, textureName) != 0)
        {
            for (cpcstr folder : {"$level$", "$game_saves$", "$game_textures$"})
            {
                if (FS.exist(path, folder, resolvedTextureName, ".dds"))
                {
                    found = true;
                    __android_log_print(ANDROID_LOG_INFO, LogTag,
                        "Android UI sequence uses static first frame: %s -> %s",
                        requestedName, resolvedTextureName);
                    break;
                }
            }
        }
    }
    if (!found)
    {
        __android_log_print(ANDROID_LOG_WARN, LogTag,
            "Android UI texture is missing from VFS: %s", requestedName);
        return false;
    }

    IReader* reader = FS.r_open(path);
    if (!reader)
    {
        __android_log_print(ANDROID_LOG_WARN, LogTag,
            "Android UI texture could not be opened: %s", path);
        return false;
    }

    const gli::texture source = gli::load(
        static_cast<const char*>(reader->pointer()), reader->length());
    FS.r_close(reader);
    if (source.empty() || source.target() != gli::TARGET_2D ||
        (gli::is_compressed(source.format()) && !gli::has_decoder(source.format())))
    {
        __android_log_print(ANDROID_LOG_WARN, LogTag,
            "Android UI DDS format is unsupported: %s (target=%d, format=%d)",
            requestedName, source.empty() ? -1 : static_cast<int>(source.target()),
            source.empty() ? -1 : static_cast<int>(source.format()));
        return false;
    }

    const gli::texture2d decoded = gli::convert(
        gli::texture2d(source), gli::FORMAT_RGBA8_UNORM_PACK8);
    if (decoded.empty())
        return false;

    const gli::texture2d::extent_type dimensions = decoded.extent(0);
    if (dimensions.x <= 0 || dimensions.y <= 0)
        return false;
    width = static_cast<std::uint32_t>(dimensions.x);
    height = static_cast<std::uint32_t>(dimensions.y);
    const std::size_t byteCount = static_cast<std::size_t>(width) * height * 4;
    if (strstr(requestedName, "font"))
    {
        const std::uint8_t* pixels = decoded.data<std::uint8_t>(0, 0, 0);
        std::uint8_t minimum[4] = {255, 255, 255, 255};
        std::uint8_t maximum[4] = {0, 0, 0, 0};
        std::uint64_t sum[4] = {};
        std::uint64_t differentRedAlpha = 0;
        const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
        for (std::size_t index = 0; index < pixelCount; ++index)
        {
            for (std::size_t channel = 0; channel < 4; ++channel)
            {
                const std::uint8_t value = pixels[index * 4 + channel];
                minimum[channel] = std::min(minimum[channel], value);
                maximum[channel] = std::max(maximum[channel], value);
                sum[channel] += value;
            }
            differentRedAlpha += pixels[index * 4] != pixels[index * 4 + 3];
        }
        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "Android font DDS channels: %s min=(%u,%u,%u,%u) max=(%u,%u,%u,%u) "
            "mean=(%.1f,%.1f,%.1f,%.1f) red-alpha-different=%llu/%zu",
            requestedName, minimum[0], minimum[1], minimum[2], minimum[3],
            maximum[0], maximum[1], maximum[2], maximum[3],
            static_cast<double>(sum[0]) / pixelCount, static_cast<double>(sum[1]) / pixelCount,
            static_cast<double>(sum[2]) / pixelCount, static_cast<double>(sum[3]) / pixelCount,
            static_cast<unsigned long long>(differentRedAlpha), pixelCount);
    }
    texture = device.CreateUiTexture(requestedName, width, height,
        decoded.data<std::uint8_t>(0, 0, 0), byteCount);
    if (texture == 0)
    {
        width = 0;
        height = 0;
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "Android UI texture ready: %s (%ux%u, DDS format %d)",
        requestedName, width, height, static_cast<int>(source.format()));
    return true;
}

bool LoadUiSequence(AndroidVulkanRenderDevice& device, const char* requestedName,
    std::vector<AndroidVulkanUiTexture>& frames, bool& cycles,
    u32& millisecondsPerFrame, std::uint32_t& width, std::uint32_t& height)
{
    frames.clear();
    cycles = false;
    millisecondsPerFrame = 0;
    if (!requestedName || !requestedName[0])
        return false;

    string_path textureName;
    xr_strcpy(textureName, requestedName);
    if (char* extension = strext(textureName))
        *extension = 0;

    string_path path;
    if (!FS.exist(path, "$game_textures$", textureName, ".seq"))
        return false;

    IReader* sequence = FS.r_open(path);
    if (!sequence)
        return false;

    string256 line{};
    sequence->r_string(line, sizeof(line));
    _Trim(line);
    if (xr_stricmp(line, "cycled") == 0)
    {
        cycles = true;
        if (!sequence->eof())
        {
            sequence->r_string(line, sizeof(line));
            _Trim(line);
        }
    }
    const u32 fps = static_cast<u32>(std::max(atoi(line), 1));
    millisecondsPerFrame = std::max(1000u / fps, 1u);

    while (!sequence->eof())
    {
        sequence->r_string(line, sizeof(line));
        _Trim(line);
        if (!line[0])
            continue;

        AndroidVulkanUiTexture frame{};
        std::uint32_t frameWidth{};
        std::uint32_t frameHeight{};
        if (LoadUiTexture(device, line, frame, frameWidth, frameHeight))
        {
            if (frames.empty())
            {
                width = frameWidth;
                height = frameHeight;
            }
            frames.push_back(frame);
        }
    }
    FS.r_close(sequence);

    if (frames.empty())
        return false;
    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "Android UI sequence ready: %s (%zu frames, %u ms/frame, ping-pong=%d)",
        requestedName, frames.size(), millisecondsPerFrame, cycles ? 1 : 0);
    return true;
}

class BootstrapVideoTexture final
{
public:
    bool Load(AndroidVulkanRenderDevice& target, const char* requestedName)
    {
        if (!requestedName || !requestedName[0])
            return false;

        string_path textureName;
        xr_strcpy(textureName, requestedName);
        if (char* extension = strext(textureName))
            *extension = 0;

        string_path path;
        if (!FS.exist(path, "$game_textures$", textureName, ".ogm"))
            return false;
        if (!surface.Load(path))
        {
            __android_log_print(ANDROID_LOG_ERROR, LogTag,
                "Android UI video could not be decoded: %s", requestedName);
            return false;
        }

        device = &target;
        name = requestedName;
        width = surface.Width(false);
        height = surface.Height(false);
        realWidth = surface.Width(true);
        realHeight = surface.Height(true);
        pixels.assign(static_cast<std::size_t>(width) * height * 4, 0);
        for (std::size_t offset = 3; offset < pixels.size(); offset += 4)
            pixels[offset] = 255;

        texture = device->CreateUiTexture(
            name.c_str(), width, height, pixels.data(), pixels.size());
        if (texture == 0)
            return false;

        const bool stopAtEnd = strstr(textureName, "intro\\") || strstr(textureName, "intro/") ||
            strstr(textureName, "outro\\") || strstr(textureName, "outro/");
        surface.Play(!stopAtEnd, Device.dwTimeContinual);
        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "Android UI video ready: %s (%ux%u content in %ux%u texture)",
            requestedName, realWidth, realHeight, width, height);
        return true;
    }

    void Update()
    {
        if (!device || texture == 0 || !surface.Valid())
            return;

        const u32 time = syncTime != 0xffffffff ? syncTime : Device.dwTimeContinual;
        if (!surface.Update(time))
            return;

        int decodedPixels = 0;
        surface.DecompressFrame(reinterpret_cast<u32*>(pixels.data()),
            width - realWidth, decodedPixels);
        if (decodedPixels != static_cast<int>(realHeight * width))
        {
            __android_log_print(ANDROID_LOG_WARN, LogTag,
                "Android UI video produced an incomplete frame: %s (%d/%u pixels)",
                name.c_str(), decodedPixels, realHeight * width);
        }

        // color_rgba() stores D3D's AARRGGBB integer. On little-endian CPUs
        // that is BGRA byte order, while the bootstrap image is RGBA8.
        for (u32 y = 0; y < realHeight; ++y)
        {
            std::uint8_t* row = pixels.data() + static_cast<std::size_t>(y) * width * 4;
            for (u32 x = 0; x < realWidth; ++x)
                std::swap(row[x * 4], row[x * 4 + 2]);
        }

        if (!device->UpdateUiTexture(texture, pixels.data(), pixels.size()) && !uploadFailureLogged)
        {
            __android_log_print(ANDROID_LOG_ERROR, LogTag,
                "Android UI video frame upload failed: %s", name.c_str());
            uploadFailureLogged = true;
        }
    }

    AndroidVulkanUiTexture GetTexture() const { return texture; }
    u32 GetWidth() const { return width; }
    u32 GetHeight() const { return height; }
    BOOL IsPlaying() { return surface.IsPlaying() ? TRUE : FALSE; }
    void Sync(u32 time) { syncTime = time; }
    void Play(BOOL looped, u32 time)
    {
        surface.Play(looped != FALSE,
            time != 0xffffffff ? (syncTime = time) : Device.dwTimeContinual);
    }
    void Stop() { surface.Stop(); }

private:
    AndroidVulkanRenderDevice* device{};
    CTheoraSurface surface;
    AndroidVulkanUiTexture texture{};
    std::vector<std::uint8_t> pixels;
    std::string name;
    u32 width{};
    u32 height{};
    u32 realWidth{};
    u32 realHeight{};
    u32 syncTime{0xffffffff};
    bool uploadFailureLogged{};
};

std::weak_ptr<BootstrapVideoTexture> ActiveVideoTexture;

std::shared_ptr<BootstrapVideoTexture> LoadUiVideo(
    AndroidVulkanRenderDevice& device, const char* requestedName)
{
    auto video = std::make_shared<BootstrapVideoTexture>();
    return video->Load(device, requestedName) ? video : nullptr;
}

class BootstrapUISequenceVideoItem final : public IUISequenceVideoItem
{
public:
    void Copy(IUISequenceVideoItem& source) override
    {
        video = static_cast<BootstrapUISequenceVideoItem&>(source).video;
    }
    bool HasTexture() override { return video != nullptr; }
    void CaptureTexture() override { video = ActiveVideoTexture.lock(); }
    void ResetTexture() override { video.reset(); }
    BOOL video_IsPlaying() override { return video ? video->IsPlaying() : FALSE; }
    void video_Sync(u32 time) override { if (video) video->Sync(time); }
    void video_Play(BOOL looped, u32 time) override { if (video) video->Play(looped, time); }
    void video_Stop() override { if (video) video->Stop(); }

private:
    std::shared_ptr<BootstrapVideoTexture> video;
};

class BootstrapUIShader final : public IUIShader
{
public:
    explicit BootstrapUIShader(AndroidVulkanRenderDevice* target) : device(target) {}

    void Copy(IUIShader& source) override
    {
        const BootstrapUIShader& other = static_cast<const BootstrapUIShader&>(source);
        initialized = other.initialized;
        texture = other.texture;
        width = other.width;
        height = other.height;
        textureName = other.textureName;
        textureMode = other.textureMode;
        sequenceFrames = other.sequenceFrames;
        sequenceCycles = other.sequenceCycles;
        sequenceMillisecondsPerFrame = other.sequenceMillisecondsPerFrame;
        video = other.video;
    }
    void create(LPCSTR requestedShader, LPCSTR requestedTexture) override
    {
        destroy();
        initialized = true;
        textureName = requestedTexture ? requestedTexture : "";
        textureMode = requestedShader && xr_strcmp(requestedShader, "font2") == 0
            ? AndroidVulkanUiTextureMode::Font2
            : requestedShader && strstr(requestedShader, "font")
            ? AndroidVulkanUiTextureMode::HudFont
            : AndroidVulkanUiTextureMode::Normal;
        if (device)
        {
            if (LoadUiSequence(*device, requestedTexture, sequenceFrames,
                sequenceCycles, sequenceMillisecondsPerFrame, width, height))
            {
                texture = sequenceFrames.front();
            }
            else if ((video = LoadUiVideo(*device, requestedTexture)))
            {
                texture = video->GetTexture();
                width = video->GetWidth();
                height = video->GetHeight();
            }
            else
            {
                LoadUiTexture(*device, requestedTexture, texture, width, height);
            }
        }
    }
    bool inited() override { return initialized; }
    void destroy() override
    {
        initialized = false;
        texture = 0;
        width = 0;
        height = 0;
        textureName.clear();
        textureMode = AndroidVulkanUiTextureMode::Normal;
        sequenceFrames.clear();
        sequenceCycles = false;
        sequenceMillisecondsPerFrame = 0;
        video.reset();
    }
    bool operator==(const IUIShader& source) const override
    {
        const BootstrapUIShader& other = static_cast<const BootstrapUIShader&>(source);
        return initialized == other.initialized && textureName == other.textureName;
    }
    bool GetBaseTextureResolution(Fvector2& resolution) override
    {
        resolution.set(static_cast<float>(width), static_cast<float>(height));
        return texture != 0;
    }
    xrImTextureData GetImGuiTextureId() override { return {}; }
    AndroidVulkanUiTexture GetTexture()
    {
        if (video)
        {
            video->Update();
            return video->GetTexture();
        }
        if (sequenceFrames.empty() || sequenceMillisecondsPerFrame == 0)
            return texture;

        const u32 logicalFrame = Device.dwTimeContinual / sequenceMillisecondsPerFrame;
        const u32 frameCount = static_cast<u32>(sequenceFrames.size());
        u32 frame = logicalFrame % frameCount;
        if (sequenceCycles && frameCount > 1)
        {
            const u32 pingPongFrame = logicalFrame % (frameCount * 2);
            frame = pingPongFrame < frameCount
                ? pingPongFrame : frameCount - 1 - (pingPongFrame % frameCount);
        }
        return sequenceFrames[frame];
    }
    AndroidVulkanUiTextureMode GetTextureMode() const { return textureMode; }
    const std::shared_ptr<BootstrapVideoTexture>& GetVideo() const { return video; }

private:
    AndroidVulkanRenderDevice* device{};
    bool initialized{};
    AndroidVulkanUiTexture texture{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::string textureName;
    AndroidVulkanUiTextureMode textureMode{AndroidVulkanUiTextureMode::Normal};
    std::vector<AndroidVulkanUiTexture> sequenceFrames;
    bool sequenceCycles{};
    u32 sequenceMillisecondsPerFrame{};
    std::shared_ptr<BootstrapVideoTexture> video;
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
    void Frame() override
    {
        ProcessTextureRequests(ImGui::GetPlatformIO().Textures);
    }
    void Render(ImDrawData* drawData) override
    {
        if (drawData && drawData->Textures)
            ProcessTextureRequests(*drawData->Textures);
    }
    void OnDeviceCreate(ImGuiContext* context) override
    {
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = "OpenXRay Android Vulkan bootstrap";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    }
    void OnDeviceDestroy() override
    {
        ImGuiIO& io = ImGui::GetIO();
        for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
        {
            if (texture->GetTexID() == BootstrapTextureId)
                texture->SetStatus(ImTextureStatus_WantDestroy);
        }
        ProcessTextureRequests(ImGui::GetPlatformIO().Textures);
        io.BackendRendererName = nullptr;
        io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
    }
    void OnDeviceResetBegin() override {}
    void OnDeviceResetEnd() override {}

private:
    static constexpr ImTextureID BootstrapTextureId = static_cast<ImTextureID>(1);

    static void ProcessTextureRequests(ImVector<ImTextureData*>& textures)
    {
        // Stage 6 deliberately discards ImGui draw data. Keep the ImGui 1.92
        // dynamic-atlas lifecycle valid without creating GPU resources; the
        // dummy ID is never dereferenced by this bootstrap renderer.
        for (ImTextureData* texture : textures)
        {
            if (texture->Status == ImTextureStatus_WantCreate)
            {
                texture->SetTexID(BootstrapTextureId);
                texture->SetStatus(ImTextureStatus_OK);
            }
            else if (texture->Status == ImTextureStatus_WantUpdates)
            {
                texture->SetStatus(ImTextureStatus_OK);
            }
            else if (texture->Status == ImTextureStatus_WantDestroy)
            {
                texture->SetTexID(ImTextureID_Invalid);
                texture->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }
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
    explicit BootstrapFontRender(AndroidVulkanRenderDevice* target) : device(target) {}

    void Initialize(cpcstr shader, cpcstr textureName) override
    {
        textureMode = shader && xr_strcmp(shader, "font2") == 0
            ? AndroidVulkanUiTextureMode::Font2
            : AndroidVulkanUiTextureMode::HudFont;
        texture = 0;
        width = 0;
        height = 0;
        if (device)
            LoadUiTexture(*device, textureName, texture, width, height);
    }

    void OnRender(CGameFont& owner) override
    {
        if (device && texture != 0)
            AndroidVulkanFontRenderAccess::Render(
                *device, texture, textureMode, width, height, owner);
    }

private:
    AndroidVulkanRenderDevice* device{};
    AndroidVulkanUiTexture texture{};
    AndroidVulkanUiTextureMode textureMode{AndroidVulkanUiTextureMode::HudFont};
    std::uint32_t width{};
    std::uint32_t height{};
};

#define IMPLEMENT_FACTORY_OBJECT(Class, Implementation) \
    I##Class* Create##Class() override { return xr_new<Implementation>(); } \
    void Destroy##Class(I##Class* object) override { xr_delete(object); }

class BootstrapRenderFactory final : public IRenderFactory
{
public:
    void Attach(AndroidVulkanRenderDevice* target) { device = target; }

    IMPLEMENT_FACTORY_OBJECT(UISequenceVideoItem, BootstrapUISequenceVideoItem)
    IUIShader* CreateUIShader() override { return xr_new<BootstrapUIShader>(device); }
    void DestroyUIShader(IUIShader* object) override { xr_delete(object); }
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
    IFontRender* CreateFontRender() override { return xr_new<BootstrapFontRender>(device); }
    void DestroyFontRender(IFontRender* object) override { xr_delete(object); }

private:
    AndroidVulkanRenderDevice* device{};
};

#undef IMPLEMENT_FACTORY_OBJECT

class BootstrapUIRender final : public IUIRender
{
public:
    void Attach(AndroidVulkanRenderDevice* target) { device = target; }
    void CreateUIGeom() override {}
    void DestroyUIGeom() override { vertices.clear(); }
    void SetShader(IUIShader& shader) override
    {
        BootstrapUIShader& bootstrapShader = static_cast<BootstrapUIShader&>(shader);
        texture = bootstrapShader.GetTexture();
        textureMode = bootstrapShader.GetTextureMode();
        ActiveVideoTexture = bootstrapShader.GetVideo();
    }
    void SetAlphaRef(int) override {}
    void SetScissor(Irect* rect) override
    {
        scissor = {};
        if (rect)
        {
            scissor.enabled = true;
            scissor.x = rect->x1;
            scissor.y = rect->y1;
            scissor.width = static_cast<u32>(std::max(rect->x2 - rect->x1, 0));
            scissor.height = static_cast<u32>(std::max(rect->y2 - rect->y1, 0));
        }
    }
    void PushPoint(float x, float y, float z, u32 color, float u, float v) override
    {
        if (vertices.size() < maximumVertices)
            vertices.push_back({x, y, z, color, u, v});
    }
    void StartPrimitive(u32 maxVertices, ePrimitiveType type, ePointType) override
    {
        vertices.clear();
        vertices.reserve(maxVertices);
        maximumVertices = maxVertices;
        primitive = type;
    }
    void FlushPrimitive() override
    {
        if (!device || vertices.empty())
            return;

        AndroidVulkanUiPrimitive targetPrimitive;
        switch (primitive)
        {
        case ptTriList: targetPrimitive = AndroidVulkanUiPrimitive::TriangleList; break;
        case ptTriStrip: targetPrimitive = AndroidVulkanUiPrimitive::TriangleStrip; break;
        case ptLineList: targetPrimitive = AndroidVulkanUiPrimitive::LineList; break;
        case ptLineStrip: targetPrimitive = AndroidVulkanUiPrimitive::LineStrip; break;
        default: return;
        }
        device->SubmitUiBatch(
            vertices.data(), vertices.size(), targetPrimitive, scissor, texture, textureMode);
        vertices.clear();
    }
    LPCSTR UpdateShaderName(LPCSTR, LPCSTR shaderName) override { return shaderName; }
    void CacheSetXformWorld(const Fmatrix&) override {}
    void CacheSetCullMode(CullMode) override {}

private:
    AndroidVulkanRenderDevice* device{};
    std::vector<AndroidVulkanUiVertex> vertices;
    AndroidVulkanUiScissor scissor;
    u32 maximumVertices{};
    ePrimitiveType primitive{ptNone};
    AndroidVulkanUiTexture texture{};
    AndroidVulkanUiTextureMode textureMode{AndroidVulkanUiTextureMode::Normal};
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

void AndroidVulkanFontRenderAccess::Render(AndroidVulkanRenderDevice& device,
    AndroidVulkanUiTexture texture, AndroidVulkanUiTextureMode textureMode,
    std::uint32_t textureWidth, std::uint32_t textureHeight, CGameFont& owner)
{
    if (texture == 0 || textureWidth == 0 || textureHeight == 0)
        return;

    if (!(owner.uFlags & CGameFont::fsValid))
    {
        owner.vTS.set(static_cast<int>(textureWidth), static_cast<int>(textureHeight));
        owner.fTCHeight = owner.fHeight / static_cast<float>(textureHeight);
        owner.uFlags |= CGameFont::fsValid;
    }

    std::vector<AndroidVulkanUiVertex> vertices;
    for (CGameFont::String& text : owner.strings)
    {
        xr_wide_char wideText[MAX_MB_CHARS];
        const u16 length = owner.IsMultibyte()
            ? mbhMulti2Wide(wideText, nullptr, MAX_MB_CHARS, text.string)
            : static_cast<u16>(xr_strlen(text.string));
        if (length == 0)
            continue;

        float x = static_cast<float>(iFloor(text.x));
        float y = static_cast<float>(iFloor(text.y));
        const float y2 = y + text.height * g_current_font_scale.y;
        float lineWidth = 0.f;
        if (text.align)
            lineWidth = owner.IsMultibyte() ? owner.SizeOf_(wideText) : owner.SizeOf_(text.string);
        if (text.align == CGameFont::alCenter)
            x -= iFloor(lineWidth * .5f) * g_current_font_scale.x;
        else if (text.align == CGameFont::alRight)
            x -= iFloor(lineWidth);

        const u32 topColor = text.c;
        u32 bottomColor = topColor;
        if (owner.uFlags & CGameFont::fsGradient)
        {
            bottomColor = color_rgba(color_get_R(topColor) / 2,
                color_get_G(topColor) / 2, color_get_B(topColor) / 2,
                color_get_A(topColor));
        }
        x -= .5f;
        y -= .5f;
        const float adjustedY2 = y2 - .5f;

        auto imprint = [&](u16 character, u16 sourceCharacter)
        {
            const Fvector glyph = owner.GetCharTC(character);
            const float scaledWidth = glyph.z * g_current_font_scale.x;
            if (!fis_zero(glyph.z))
            {
                const float u0 = glyph.x / static_cast<float>(owner.vTS.x);
                const float v0 = glyph.y / static_cast<float>(owner.vTS.y);
                const float u1 = u0 + glyph.z / static_cast<float>(owner.vTS.x);
                const float v1 = v0 + owner.fTCHeight;
                const float right = x + scaledWidth;
                vertices.push_back({x, adjustedY2, 0.f, bottomColor, u0, v1});
                vertices.push_back({x, y, 0.f, topColor, u0, v0});
                vertices.push_back({right, adjustedY2, 0.f, bottomColor, u1, v1});
                vertices.push_back({right, adjustedY2, 0.f, bottomColor, u1, v1});
                vertices.push_back({x, y, 0.f, topColor, u0, v0});
                vertices.push_back({right, y, 0.f, topColor, u1, v0});
            }
            x += scaledWidth * owner.vInterval.x;
            if (owner.IsMultibyte())
            {
                x -= 2.f;
                if (IsNeedSpaceCharacter(sourceCharacter))
                    x += owner.fXStep;
            }
        };

        for (u16 index = 0; index < length; ++index)
        {
            const u16 sourceCharacter = owner.IsMultibyte()
                ? wideText[1 + index]
                : static_cast<u16>(static_cast<u8>(text.string[index]));
            if (sourceCharacter == GAME_ACTION_MARK && index + 1 < length)
            {
                ++index;
                const EGameActions action = static_cast<EGameActions>(owner.IsMultibyte()
                    ? wideText[1 + index]
                    : static_cast<u8>(text.string[index]));
                pcstr binding = GetActionBinding(action);
                if (owner.IsMultibyte())
                {
                    xr_wide_char wideBinding[MAX_MB_CHARS];
                    const u16 bindingLength = mbhMulti2Wide(
                        wideBinding, nullptr, MAX_MB_CHARS, binding);
                    for (u16 bindingIndex = 0; bindingIndex < bindingLength; ++bindingIndex)
                        imprint(wideBinding[1 + bindingIndex], wideBinding[1 + bindingIndex]);
                }
                else
                {
                    while (binding && *binding)
                    {
                        const u16 character = static_cast<u8>(*binding++);
                        imprint(character, character);
                    }
                }
            }
            else
            {
                imprint(sourceCharacter, sourceCharacter);
            }
        }
    }

    if (!vertices.empty())
    {
        device.SubmitUiBatch(vertices.data(), vertices.size(),
            AndroidVulkanUiPrimitive::TriangleList, {}, texture, textureMode);
        static bool logged{};
        if (!logged)
        {
            __android_log_print(ANDROID_LOG_INFO, LogTag,
                "Android font batch submitted: %zu vertices, atlas %ux%u",
                vertices.size(), textureWidth, textureHeight);
            logged = true;
        }
    }
}

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
    implementation->factory.Attach(&renderDevice);
    implementation->ui.Attach(&renderDevice);
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
    implementation->factory.Attach(nullptr);
    implementation->ui.Attach(nullptr);
    implementation->attached = false;
}

#include <SDL.h>
#include <AL/alc.h>

#include "Common/Platform.hpp"
#include "xrCore/Android/AndroidArchiveDescriptors.hpp"
#include "xrCore/Android/AndroidCoreTest.hpp"
#include "xrCore/xrCore.h"
#include "xrCore/xr_ini.h"
#include "xrScriptEngine/xrScriptEngine.hpp"
#include "xrScriptEngine/script_space.hpp"
#include "AndroidGameData.hpp"
#include "AndroidGameModuleProbe.hpp"
#include "AndroidSafBridge.hpp"
#include "AndroidVulkanRenderDevice.hpp"
#include "AndroidVulkanRenderServices.hpp"
#include "AndroidVulkanRendererModule.hpp"
#include "Include/xrAPI/xrAPI.h"
#include "Include/xrRender/xrRender.h"
#include "xrEngine/x_ray.h"
#include "xrGame/xrGame.h"
#include "xrGameSpy/GameSpy_MasterServer.h"
#include "Opcode.h"
#include "md5.h"
#include <imgui.h>
#include <xrLuaFix.h>

#include <android/log.h>
#include <cerrno>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sstream>
#include <unordered_set>
#include <vector>
#include <jpeglib.h>
#include <memory>
#include <luabind/memory.hpp>
#include <luajit.h>
#include <ode/ode.h>
#include <ogg/ogg.h>
#include <string>
#include <sys/stat.h>
#include <theora/theora.h>
#include <unistd.h>
#include <vorbis/codec.h>

namespace
{
constexpr const char* LogTag = "OpenXRay";

const char* FindArgument(int argc, char** argv, const char* name)
{
    for (int index = 1; index + 1 < argc; ++index)
    {
        if (std::strcmp(argv[index], name) == 0)
            return argv[index + 1];
    }
    return "";
}

bool WriteFile(const std::string& path, const char* contents)
{
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;

    const size_t size = std::strlen(contents);
    const bool written = std::fwrite(contents, 1, size, file) == size;
    return std::fclose(file) == 0 && written;
}

// The marker lives in writable app storage, which every mount mode shares;
// $game_data$ may point into the user's (read-only) installation.
bool VerifyFilesystemFixture()
{
    static constexpr char Marker[] = "openxray-android-vfs-smoke-v1\n";
    IReader* reader = FS.r_open("$app_data_root$", "android_smoke.txt");
    if (!reader)
        return false;

    const bool valid = reader->length() == sizeof(Marker) - 1 &&
        std::memcmp(reader->pointer(), Marker, sizeof(Marker) - 1) == 0;
    FS.r_close(reader);
    return valid;
}

// SDL blocks the main thread while the activity is paused, but OpenAL Soft
// mixes on its own thread and would keep playing the last buffers in the
// background. The watch runs on the Android UI thread; ALC_SOFT_pause_device
// is thread-safe, so it pauses mixing without touching engine state.
int AndroidLifecycleWatch(void*, SDL_Event* event)
{
    using DevicePauseProc = void (*)(ALCdevice*);
    const bool background = event->type == SDL_APP_WILLENTERBACKGROUND;
    const bool foreground = event->type == SDL_APP_DIDENTERFOREGROUND;
    if (!background && !foreground)
        return 0;

    ALCcontext* context = alcGetCurrentContext();
    ALCdevice* device = context ? alcGetContextsDevice(context) : nullptr;
    if (device && alcIsExtensionPresent(device, "ALC_SOFT_pause_device"))
    {
        const auto proc = reinterpret_cast<DevicePauseProc>(
            alcGetProcAddress(device, background ? "alcDevicePauseSOFT" : "alcDeviceResumeSOFT"));
        if (proc)
            proc(device);
    }
    __android_log_print(ANDROID_LOG_INFO, LogTag, "Activity %s; audio %s",
        background ? "entering background" : "back in foreground", background ? "paused" : "resumed");
    return 0;
}

bool VerifyOpenALSoft()
{
    ALCdevice* device = alcOpenDevice(nullptr);
    if (!device)
        return false;

    ALCcontext* context = alcCreateContext(device, nullptr);
    const bool current = context && alcMakeContextCurrent(context) == ALC_TRUE;
    if (current)
        alcMakeContextCurrent(nullptr);
    if (context)
        alcDestroyContext(context);
    const bool closed = alcCloseDevice(device) == ALC_TRUE;
    return current && closed;
}

bool VerifyMediaCodecs()
{
    ogg_sync_state oggState{};
    if (ogg_sync_init(&oggState) != 0)
        return false;
    const bool oggReady = ogg_sync_buffer(&oggState, 4096) != nullptr;
    ogg_sync_clear(&oggState);

    vorbis_info vorbisInfo{};
    vorbis_info_init(&vorbisInfo);
    vorbis_info_clear(&vorbisInfo);

    theora_info theoraInfo{};
    theora_info_init(&theoraInfo);
    theora_info_clear(&theoraInfo);

    jpeg_decompress_struct jpegInfo{};
    jpeg_error_mgr jpegError{};
    jpegInfo.err = jpeg_std_error(&jpegError);
    jpeg_create_decompress(&jpegInfo);
    jpeg_destroy_decompress(&jpegInfo);

    const char* vorbisVersion = vorbis_version_string();
    const char* theoraVersion = theora_version_string();
    if (!oggReady || !vorbisVersion || !theoraVersion)
        return false;

    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "Media codec smoke test passed (Vorbis %s; Theora %s; JPEG API %d)",
        vorbisVersion, theoraVersion, JPEG_LIB_VERSION);
    return true;
}

void* LuabindSmokeAllocator(void*, const void* pointer, size_t size)
{
    if (size == 0)
    {
        std::free(const_cast<void*>(pointer));
        return nullptr;
    }
    return std::realloc(const_cast<void*>(pointer), size);
}

bool VerifyScriptRuntime()
{
    lua_State* state = luaL_newstate();
    if (!state)
        return false;

    luaL_openlibs(state);
    luabind::allocator = &LuabindSmokeAllocator;
    luabind::allocator_context = nullptr;
    luabind::open(state);
    luaopen_xrluafix(state);
    lua_settop(state, 0);

    static constexpr char Script[] =
        "local ffi_module = require('ffi'); "
        "return (type(jit) == 'table' and type(bit) == 'table' and type(ffi_module) == 'table' "
        "and type(lua_extensions) == 'table' and type(lfs) == 'table' "
        "and type(string.pack) == 'function') and 42 or -1";
    const int result = luaL_dostring(state, Script);
    const bool valueMatches = result == 0 && lua_isnumber(state, -1) && lua_tointeger(state, -1) == 42;

    // This exported helper lives in xrScriptEngine rather than luabind. Calling
    // it guarantees the engine scripting archive participates in the runtime
    // link, in addition to compiling all of its translation units.
    const luabind::iterator end;
    const bool engineSymbolWorks = luabind_it_distance(luabind::iterator{}, end) == 0;
    lua_close(state);

    if (valueMatches && engineSymbolWorks)
        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "%s, bit, FFI, luabind, xrLuaFix, and xrScriptEngine smoke test passed",
            LUAJIT_VERSION);
    return valueMatches && engineSymbolWorks;
}

bool VerifyCollisionAndPhysics()
{
    // Exercise exported OPCODE object and virtual-method ABI without feeding
    // its old tree builder a synthetic degenerate level model.
    Opcode::OPCODE_Model collisionModel;
    Opcode::RayCollider rayCollider;
    if (collisionModel.GetTree() || rayCollider.GetNbRayBVTests() != 0 || !rayCollider.ValidateSettings())
        return false;

    dWorldID world = dWorldCreate();
    if (!world)
        return false;

    dWorldSetGravity(world, 0.0, -9.81, 0.0);
    dBodyID body = dBodyCreate(world);
    if (!body)
    {
        dWorldDestroy(world);
        return false;
    }

    dMass mass;
    dMassSetSphere(&mass, 1.0, 0.5);
    dBodySetMass(body, &mass);
    dBodySetPosition(body, 0.0, 2.0, 0.0);
    dWorldQuickStep(world, 0.1);
    const dReal* position = dBodyGetPosition(body);
    const bool stepped = position && std::isfinite(position[1]) && position[1] < 2.0;

    dBodyDestroy(body);
    dWorldDestroy(world);
    if (stepped)
        __android_log_write(ANDROID_LOG_INFO, LogTag,
            "OPCODE collider ABI and ODE quick-step smoke test passed");
    return stepped;
}

bool VerifyRemainingSupportLibraries()
{
    unsigned char input[] = {'a', 'b', 'c'};
    char digest[33]{};
    MD5Digest(input, sizeof(input), digest);

    const bool gameSpyWorks = std::strcmp(digest, "900150983cd24fb0d6963f7d28e17f72") == 0;
    const char* imguiVersion = ImGui::GetVersion();
    const bool imguiWorks = imguiVersion && std::strcmp(imguiVersion, IMGUI_VERSION) == 0;
    if (gameSpyWorks && imguiWorks)
    {
        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "GameSpy MD5 and Dear ImGui %s runtime smoke test passed", imguiVersion);
    }
    return gameSpyWorks && imguiWorks;
}

bool VerifyOptionalArchiveFixture()
{
    static constexpr char Marker[] =
        "; Freely redistributable OpenXRay Android archive fixture.\n"
        "[android_smoke]\n"
        "marker = openxray-android-xrarchive-v1\n";

    IReader* reader = FS.r_open("$game_data$", "configs\\android_smoke.ltx");
    if (!reader)
        return true;

    // xrCompress preserves the fixture bytes exactly.
    const bool valid = reader->length() == sizeof(Marker) - 1 &&
        std::memcmp(reader->pointer(), Marker, sizeof(Marker) - 1) == 0;
    FS.r_close(reader);
    return valid;
}

bool VerifyGameArchiveResource()
{
    // Opening this small, normally compressed file exercises the archive data
    // path (mmap plus decompression), not only the archive-index parser. Do not
    // inspect or log its commercial contents.
    IReader* reader = FS.r_open("$game_config$", "system.ltx");
    if (!reader)
        return false;

    const size_t size = reader->length();
    FS.r_close(reader);
    if (size == 0)
        return false;

    __android_log_print(
        ANDROID_LOG_INFO, LogTag, "Read system.ltx through archive VFS (%zu bytes)", size);
    return true;
}

bool VerifyGameExecutionResources()
{
    struct Resource
    {
        const char* alias;
        const char* path;
        const char* purpose;
        bool required;
    };
    static constexpr Resource Resources[] = {
        {"$game_config$", "system.ltx", "engine configuration", true},
        {"$game_data$", "scripts\\_g.script", "Lua bootstrap", true},
        {"$game_config$", "ui\\ui_mm_main.xml", "main-menu layout", false},
    };

    for (const Resource& resource : Resources)
    {
        IReader* reader = FS.r_open(resource.alias, resource.path);
        if (!reader)
        {
            if (resource.required)
            {
                __android_log_print(ANDROID_LOG_ERROR, LogTag,
                    "Execution preflight missing %s: %s", resource.purpose, resource.path);
                return false;
            }
            __android_log_print(ANDROID_LOG_WARN, LogTag,
                "Optional resource not found: %s", resource.path);
            continue;
        }

        const size_t size = reader->length();
        FS.r_close(reader);
        if (size == 0 && resource.required)
        {
            __android_log_print(ANDROID_LOG_ERROR, LogTag,
                "Execution preflight found empty %s: %s", resource.purpose, resource.path);
            return false;
        }
    }

    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "Game execution resource preflight passed (%zu required files)",
        std::size(Resources));
    return true;
}

bool VerifyGameConfigurationGraph()
{
    string_path configRoot;
    string_path systemPath;
    string_path gamePath;
    FS.update_path(configRoot, "$game_config$", "");
    FS.update_path(systemPath, "$game_config$", "system.ltx");
    FS.update_path(gamePath, "$game_config$", "game.ltx");

    IReader* systemReader = FS.r_open(systemPath);
    if (!systemReader)
        return false;

    // This invokes the real engine INI parser, including all recursive
    // #include directives resolved through the descriptor-backed archive VFS.
    // No configuration contents are copied out of the user's installation.
    CInifile systemConfig(systemReader, configRoot);
    FS.r_close(systemReader);
    if (systemConfig.section_count() == 0 ||
        !systemConfig.section_exist("actor") ||
        !systemConfig.section_exist("alife") ||
        !systemConfig.line_exist("alife", "time_factor"))
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag,
            "System configuration graph is missing required gameplay sections");
        return false;
    }

    IReader* gameReader = FS.r_open(gamePath);
    if (!gameReader)
        return false;
    CInifile gameConfig(gameReader, configRoot);
    FS.r_close(gameReader);
    if (gameConfig.section_count() == 0)
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "game.ltx contains no sections");
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "Configuration graph parsed by CInifile (system=%u sections, game=%u sections)",
        systemConfig.section_count(), gameConfig.section_count());
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    const char* gameDataUri = FindArgument(argc, argv, "--game-data-uri");
    const char* gameDataPath = FindArgument(argc, argv, "--game-data-path");
    const char* gameMode = FindArgument(argc, argv, "--game-mode");
    const char* rendererName = FindArgument(argc, argv, "--renderer");
    const char* engineResources = FindArgument(argc, argv, "--engine-resources");
    const bool useGles = std::strcmp(rendererName, "gles") == 0;
    const char* appFilesPath = FindArgument(argc, argv, "--app-files-path");
    const char* profileId = FindArgument(argc, argv, "--profile-id");
    const char* renderScale = FindArgument(argc, argv, "--render-scale");
    const char* viewDistance = FindArgument(argc, argv, "--view-distance");
    const char* masterHost = FindArgument(argc, argv, "--master-host");
    const char* masterPortText = FindArgument(argc, argv, "--master-port");
    const char* engineArguments = FindArgument(argc, argv, "--engine-args");
    const float viewDistancePercent = std::clamp(
        viewDistance && viewDistance[0] ? std::strtof(viewDistance, nullptr) : 100.f,
        40.f, 100.f);
    AndroidSetViewDistanceScale(viewDistancePercent / 100.f);
    const long parsedMasterPort = masterPortText && masterPortText[0] ? std::strtol(masterPortText, nullptr, 10) : 28910;
    const unsigned short masterPort = static_cast<unsigned short>(
        std::clamp(parsedMasterPort, 1L, 65535L));
    ConfigureGameSpyMasterServer(masterHost, masterPort);
    OutputDebugString("Starting OpenXRay Android SDL host");
    __android_log_print(ANDROID_LOG_INFO, LogTag, "Selected game-data tree: %s", gameDataUri);
    if (gameDataPath[0])
        __android_log_print(ANDROID_LOG_INFO, LogTag, "Direct game-data path: %s", gameDataPath);
    __android_log_print(ANDROID_LOG_INFO, LogTag, "Private engine files: %s", appFilesPath);
    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "Launch profile: %s; render scale: %s%%; view distance: %.0f%%",
        profileId, renderScale, viewDistancePercent);
    if (IsGameSpyMasterServerConfigured())
        __android_log_print(ANDROID_LOG_INFO, LogTag, "Custom GameSpy master: %s:%hu", masterHost, masterPort);
    else
        __android_log_write(ANDROID_LOG_INFO, LogTag, "Custom GameSpy master disabled; using compatibility backend");

    // Prefer ordinary paths (all-files access): the stock locator then sees
    // loose files, mods and the game's own layout at native speed. Otherwise
    // read the selected Storage Access Framework tree through the bridge.
    std::unique_ptr<GameDataSource> dataSource = gameDataPath && gameDataPath[0]
        ? CreateDirectGameDataSource(gameDataPath) : CreateSafGameDataSource();
    std::vector<GameDataFile> treeFiles;
    if (!dataSource->Walk(treeFiles))
    {
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "Could not walk the selected game-data tree (%s)",
            dataSource->Name());
        return 20;
    }
    __android_log_print(ANDROID_LOG_INFO, LogTag, "Walked selected tree via %s: %zu entries",
        dataSource->Name(), treeFiles.size());

    GameLayout layout;
    if (DetectGameLayout(*dataSource, treeFiles, ParseGameMode(gameMode), layout))
    {
        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "Detected installation: root=\"%s\", fsgame=\"%s\", archives=%zu, loose files=%zu, mode=%s%s",
            layout.gameRoot.c_str(), layout.fsgameRelativePath.c_str(), layout.archiveCount,
            layout.looseFileCount, GameFlavorName(layout.flavor), layout.flavorForced ? " (forced)" : "");
    }
    else
    {
        __android_log_write(ANDROID_LOG_WARN, LogTag,
            "No X-Ray installation found in the selected tree; continuing in resource-free host mode");
    }

    const int coreTestResult = xrCoreAndroidSmokeTest();
    if (coreTestResult != 0)
    {
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "xrCore Android smoke test failed: %d", coreTestResult);
        return 10 + coreTestResult;
    }
    __android_log_write(ANDROID_LOG_INFO, LogTag, "xrCore Android smoke test passed");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0)
    {
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    if (!VerifyOpenALSoft())
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "OpenAL Soft device/context smoke test failed");
        SDL_Quit();
        return 9;
    }
    __android_log_write(ANDROID_LOG_INFO, LogTag, "OpenAL Soft device/context smoke test passed");
    if (!VerifyMediaCodecs())
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Media codec smoke test failed");
        SDL_Quit();
        return 11;
    }
    if (!VerifyScriptRuntime())
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Script runtime smoke test failed");
        SDL_Quit();
        return 12;
    }
    if (!VerifyCollisionAndPhysics())
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Collision/physics smoke test failed");
        SDL_Quit();
        return 13;
    }
    if (!VerifyRemainingSupportLibraries())
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "GameSpy/ImGui runtime smoke test failed");
        SDL_Quit();
        return 14;
    }

    std::uint32_t playerStateSize = 0;
    if (!OpenXRayAndroidProbeGameModule(1, &playerStateSize) || playerStateSize == 0)
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "xrGame Android runtime link probe failed");
        SDL_Quit();
        return 17;
    }
    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "xrGame Android runtime link probe passed (game_PlayerState=%u bytes)", playerStateSize);

    // All engine state lives below this app-private root; the installation
    // itself is only ever read.
    const std::string privateRoot = std::string(appFilesPath) + "/xr-core-smoke";
    GameDataMount gameData;
    std::string fixtureConfig;
    if (!gameData.Mount(*dataSource, treeFiles, layout, privateRoot, engineResources, fixtureConfig) ||
        !WriteFile(privateRoot + "/_appdata_/android_smoke.txt", "openxray-android-vfs-smoke-v1\n"))
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Could not prepare the private engine root");
        SDL_Quit();
        return 3;
    }

    // Exercise the real locator on an engine-owned fixture. Archives from the
    // selected read-only SAF tree are exposed through borrowed descriptors;
    // writable state stays in the app-private directory.
    const std::string coreCommandLine = std::string(GameFlavorFlag(layout.flavor)) + " -nolog";
    Core.Initialize("OpenXRay Android", coreCommandLine.c_str(), true, fixtureConfig.c_str());
    if (layout.detected && !layout.flavorForced)
    {
        const GameFlavor refined = RefineGameFlavorFromVfs(layout.flavor);
        if (refined != layout.flavor)
        {
            __android_log_print(ANDROID_LOG_INFO, LogTag, "Game mode refined from shipped levels: %s",
                GameFlavorName(refined));
            layout.flavor = refined;
        }
    }
    if (!VerifyFilesystemFixture())
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "xrCore private filesystem smoke test failed");
        Core._destroy();
        SDL_Quit();
        return 4;
    }
    if (!VerifyOptionalArchiveFixture())
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "xrCore archive fixture contents did not match");
        Core._destroy();
        SDL_Quit();
        return 7;
    }

    // Unpacked installations have no archives at all, so the decision to run
    // the game rests on the engine configuration being reachable.
    const bool hasArchives = layout.detected && FS.exist("$game_config$", "system.ltx");
    if (layout.detected && !hasArchives)
    {
        __android_log_write(ANDROID_LOG_WARN, LogTag,
            "Installation has no reachable system.ltx; treating it as incomplete");
    }
    if (hasArchives && !VerifyGameArchiveResource())
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Could not read system.ltx from archive VFS");
        Core._destroy();
        SDL_Quit();
        return 8;
    }
    if (hasArchives && !VerifyGameExecutionResources())
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Game execution resource preflight failed");
        Core._destroy();
        SDL_Quit();
        return 18;
    }
    if (hasArchives && !VerifyGameConfigurationGraph())
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Game configuration compatibility preflight failed");
        Core._destroy();
        SDL_Quit();
        return 19;
    }
    __android_log_write(ANDROID_LOG_INFO, LogTag, "xrCore private filesystem smoke test passed");

    if (hasArchives)
    {
        // The preflight deliberately owns a short xrCore/SDL lifecycle so a
        // bad installation fails before any game state is created. Restart
        // through CApplication now, keeping the descriptor-backed archive
        // mirror alive for the complete engine/game lifecycle below.
        Core._destroy();
        SDL_Quit();

        std::string commandLine = std::string(GameFlavorFlag(layout.flavor));
        commandLine += " -nosplash -fsltx ";
        commandLine += fixtureConfig;
        if (engineArguments && engineArguments[0])
        {
            commandLine += ' ';
            commandLine += engineArguments;
        }
        // Vulkan is the default renderer. "gles" selects the upstream GL
        // renderer (R3-class deferred pipeline, real shaders and materials)
        // running on OpenGL ES 3.2.
        AndroidVulkanRendererModule rendererModule(appFilesPath);
        const std::array<RendererModule*, 2> renderModules = {
            useGles ? xray::render::render_gl::GetRendererModule() : &rendererModule, nullptr };

        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "Starting full xrEngine/xrGame lifecycle with the %s renderer (%s mode)",
            useGles ? "OpenGL ES" : "Vulkan", GameFlavorName(layout.flavor));
        CApplication application(commandLine.c_str(), &xrGame, renderModules);
        SDL_AddEventWatch(&AndroidLifecycleWatch, nullptr);
        const int result = application.Run();
        SDL_DelEventWatch(&AndroidLifecycleWatch, nullptr);
        return result;
    }

    SDL_Window* window = SDL_CreateWindow(
        "OpenXRay Android Host",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        1280,
        720,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN
    );
    if (!window)
    {
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "SDL_CreateWindow failed: %s", SDL_GetError());
        Core._destroy();
        SDL_Quit();
        return 2;
    }
    AndroidVulkanRenderDevice renderer(appFilesPath);
    AndroidVulkanRenderServices renderServices;
    renderServices.Attach(renderer);
    u32 renderWidth = 1280;
    u32 renderHeight = 720;
    float renderHalfWidth = 640.0f;
    float renderHalfHeight = 360.0f;
    renderer.Create(window, renderWidth, renderHeight, renderHalfWidth, renderHalfHeight);
    if (renderer.HasFailed())
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Vulkan GEnv.Render bootstrap failed");
        renderServices.Detach();
        SDL_DestroyWindow(window);
        Core._destroy();
        SDL_Quit();
        return 15;
    }

    std::uint64_t frameIndex = 0;
    bool rendererFailed = false;
    bool running = true;
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT)
                running = false;
        }
        if (!running)
            break;

        ++frameIndex;
        Core.dwFrame = static_cast<u32>(frameIndex);
        GEnv.Render->Begin();
        GEnv.Render->End();
        if (renderer.HasFailed())
        {
            __android_log_write(ANDROID_LOG_ERROR, LogTag, "Persistent Vulkan GEnv.Render frame failed");
            rendererFailed = true;
            running = false;
        }
        SDL_Delay(1);
    }

    GEnv.Render->Destroy();
    renderServices.Detach();
    SDL_DestroyWindow(window);
    Core._destroy();
    __android_log_write(ANDROID_LOG_INFO, LogTag, "xrCore lifecycle destroyed cleanly");
    SDL_Quit();
    return rendererFailed ? 16 : 0;
}

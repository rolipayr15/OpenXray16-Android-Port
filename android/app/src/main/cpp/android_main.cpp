#include <SDL.h>
#include <AL/alc.h>

#include "Common/Platform.hpp"
#include "xrCore/Android/AndroidArchiveDescriptors.hpp"
#include "xrCore/Android/AndroidCoreTest.hpp"
#include "xrCore/xrCore.h"
#include "xrCore/xr_ini.h"
#include "xrScriptEngine/xrScriptEngine.hpp"
#include "xrScriptEngine/script_space.hpp"
#include "AndroidGameModuleProbe.hpp"
#include "AndroidSafBridge.hpp"
#include "AndroidVulkanBootstrap.hpp"
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <jpeglib.h>
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

bool EnsureDirectory(const std::string& path)
{
    return mkdir(path.c_str(), 0700) == 0 || errno == EEXIST;
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

bool PrepareFilesystemFixture(const char* appFilesPath, std::string& fsConfigPath)
{
    if (!appFilesPath || !appFilesPath[0])
        return false;

    const std::string root = std::string(appFilesPath) + "/xr-core-smoke";
    if (!EnsureDirectory(root) ||
        !EnsureDirectory(root + "/_appdata_") ||
        !EnsureDirectory(root + "/_appdata_/logs") ||
        !EnsureDirectory(root + "/gamedata"))
    {
        return false;
    }

    static constexpr char FsConfig[] =
        "; OpenXRay-owned fixture; contains no game data.\n"
        "$app_data_root$ = true|false|$fs_root$|_appdata_\\\n"
        "$arch_dir$ = false|false|$fs_root$\n"
        "$game_data$ = true|true|$fs_root$|gamedata\\\n"
        "$logs$ = true|false|$app_data_root$|logs\\\n";
    static constexpr char Marker[] = "openxray-android-vfs-smoke-v1\n";

    fsConfigPath = root + "/fsgame.ltx";
    return WriteFile(fsConfigPath, FsConfig) && WriteFile(root + "/gamedata/android_smoke.txt", Marker);
}

bool VerifyFilesystemFixture()
{
    static constexpr char Marker[] = "openxray-android-vfs-smoke-v1\n";
    IReader* reader = FS.r_open("$game_data$", "android_smoke.txt");
    if (!reader)
        return false;

    const bool valid = reader->length() == sizeof(Marker) - 1 &&
        std::memcmp(reader->pointer(), Marker, sizeof(Marker) - 1) == 0;
    FS.r_close(reader);
    return valid;
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

bool IsGameArchive(const AndroidSafEntry& entry)
{
    if (entry.directory)
        return false;

    std::string name = entry.name;
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return name.rfind("gamedata.db", 0) == 0;
}

bool IsGameArchiveName(const char* value)
{
    if (!value)
        return false;

    AndroidSafEntry entry;
    entry.name = value;
    return IsGameArchive(entry);
}

class SafArchiveMirror
{
public:
    SafArchiveMirror() = default;
    SafArchiveMirror(const SafArchiveMirror&) = delete;
    SafArchiveMirror& operator=(const SafArchiveMirror&) = delete;

    ~SafArchiveMirror()
    {
        xrClearAndroidArchiveDescriptors();
        for (const int descriptor : descriptors)
            close(descriptor);
    }

    bool Initialize(const std::string& root, const std::vector<AndroidSafEntry>& entries)
    {
        xrClearAndroidArchiveDescriptors();

        // The directory is fully app-owned. Remove only stale archive symlinks
        // created by earlier runs; private fixture files are left untouched.
        if (DIR* directory = opendir(root.c_str()))
        {
            while (dirent* entry = readdir(directory))
            {
                if (!IsGameArchiveName(entry->d_name))
                    continue;

                const std::string stalePath = root + "/" + entry->d_name;
                struct stat status{};
                if (lstat(stalePath.c_str(), &status) == 0 && S_ISLNK(status.st_mode))
                    unlink(stalePath.c_str());
            }
            closedir(directory);
        }

        for (const AndroidSafEntry& entry : entries)
        {
            if (!IsGameArchive(entry))
                continue;
            if (entry.name.find('/') != std::string::npos || entry.name.find('\\') != std::string::npos)
                return false;

            const int descriptor = AndroidSafOpenFile(entry.name.c_str());
            if (descriptor < 0)
                return false;

            const off_t size = lseek(descriptor, 0, SEEK_END);
            std::array<unsigned char, 16> header{};
            const ssize_t bytesRead = size >= static_cast<off_t>(header.size())
                ? pread(descriptor, header.data(), header.size(), 0)
                : -1;
            if (size < static_cast<off_t>(header.size()) || bytesRead != static_cast<ssize_t>(header.size()))
            {
                close(descriptor);
                return false;
            }

            const std::string target = "/proc/self/fd/" + std::to_string(descriptor);
            const std::string link = root + "/" + entry.name;
            if (symlink(target.c_str(), link.c_str()) != 0)
            {
                close(descriptor);
                return false;
            }
            if (!xrRegisterAndroidArchiveDescriptor(link.c_str(), descriptor))
            {
                unlink(link.c_str());
                close(descriptor);
                return false;
            }
            descriptors.push_back(descriptor);
        }
        return true;
    }

    size_t size() const { return descriptors.size(); }

private:
    std::vector<int> descriptors;
};

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

bool VerifyKnownShoCArchiveResource()
{
    // Opening this small, normally compressed file exercises the archive data
    // path (mmap plus decompression), not only the archive-index parser. Do not
    // inspect or log its commercial contents.
    IReader* reader = FS.r_open("$game_data$", "config\\system.ltx");
    if (!reader)
        return false;

    const size_t size = reader->length();
    FS.r_close(reader);
    if (size == 0)
        return false;

    __android_log_print(
        ANDROID_LOG_INFO, LogTag, "Read known ShoC resource through archive VFS (%zu bytes)", size);
    return true;
}

bool VerifyShoCExecutionResources()
{
    struct Resource
    {
        const char* path;
        const char* purpose;
    };
    static constexpr Resource RequiredResources[] = {
        {"config\\system.ltx", "engine configuration"},
        {"scripts\\_g.script", "Lua bootstrap"},
        {"config\\ui\\ui_mm_main.xml", "main-menu layout"},
    };

    for (const Resource& resource : RequiredResources)
    {
        IReader* reader = FS.r_open("$game_data$", resource.path);
        if (!reader)
        {
            __android_log_print(ANDROID_LOG_ERROR, LogTag,
                "ShoC execution preflight missing %s: %s", resource.purpose, resource.path);
            return false;
        }

        const size_t size = reader->length();
        FS.r_close(reader);
        if (size == 0)
        {
            __android_log_print(ANDROID_LOG_ERROR, LogTag,
                "ShoC execution preflight found empty %s: %s", resource.purpose, resource.path);
            return false;
        }
    }

    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "ShoC execution resource preflight passed (%zu required files)",
        std::size(RequiredResources));
    return true;
}

bool VerifyShoCConfigurationGraph()
{
    string_path configRoot;
    string_path systemPath;
    string_path gamePath;
    FS.update_path(configRoot, "$game_data$", "config\\");
    FS.update_path(systemPath, "$game_data$", "config\\system.ltx");
    FS.update_path(gamePath, "$game_data$", "config\\game.ltx");

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
            "ShoC system configuration graph is missing required gameplay sections");
        return false;
    }

    IReader* gameReader = FS.r_open(gamePath);
    if (!gameReader)
        return false;
    CInifile gameConfig(gameReader, configRoot);
    FS.r_close(gameReader);
    if (gameConfig.section_count() == 0)
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "ShoC game.ltx contains no sections");
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "ShoC configuration graph parsed by CInifile (system=%u sections, game=%u sections)",
        systemConfig.section_count(), gameConfig.section_count());
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    const char* gameDataUri = FindArgument(argc, argv, "--game-data-uri");
    const char* appFilesPath = FindArgument(argc, argv, "--app-files-path");
    OutputDebugString("Starting OpenXRay Android SDL host");
    __android_log_print(ANDROID_LOG_INFO, LogTag, "Selected game-data tree: %s", gameDataUri);
    __android_log_print(ANDROID_LOG_INFO, LogTag, "Private engine files: %s", appFilesPath);

    std::vector<AndroidSafEntry> gameDataRootEntries;
    if (!AndroidSafListDirectory("", gameDataRootEntries))
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Selected SAF game-data tree is not readable");
        return 5;
    }
    __android_log_print(
        ANDROID_LOG_INFO, LogTag, "Selected SAF game-data tree is readable (%zu root entries)",
        gameDataRootEntries.size());

    const auto archive = std::find_if(gameDataRootEntries.begin(), gameDataRootEntries.end(), IsGameArchive);
    if (archive == gameDataRootEntries.end())
    {
        __android_log_write(
            ANDROID_LOG_WARN, LogTag, "No gamedata.db* archive found; continuing in resource-free host mode");
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

    std::string fixtureConfig;
    if (!PrepareFilesystemFixture(appFilesPath, fixtureConfig))
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Could not prepare private xrCore filesystem fixture");
        SDL_Quit();
        return 3;
    }

    const std::string fixtureRoot = std::string(appFilesPath) + "/xr-core-smoke";
    SafArchiveMirror archiveMirror;
    if (!archiveMirror.Initialize(fixtureRoot, gameDataRootEntries))
    {
        __android_log_write(
            ANDROID_LOG_ERROR, LogTag, "Could not mirror seekable SAF archives into the private xrCore view");
        SDL_Quit();
        return 6;
    }
    __android_log_print(
        ANDROID_LOG_INFO, LogTag, "Prepared %zu descriptor-backed SAF archives", archiveMirror.size());

    // Exercise the real locator on an engine-owned fixture. Root gamedata.db*
    // archives from the selected read-only SAF tree are exposed through borrowed
    // descriptors; writable state stays in the app-private directory.
    Core.Initialize("OpenXRay Android", "-shoc -nolog", true, fixtureConfig.c_str());
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
    if (archiveMirror.size() > 1 && !VerifyKnownShoCArchiveResource())
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Could not read known ShoC resource from archive VFS");
        Core._destroy();
        SDL_Quit();
        return 8;
    }
    if (archiveMirror.size() > 1 && !VerifyShoCExecutionResources())
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "ShoC execution resource preflight failed");
        Core._destroy();
        SDL_Quit();
        return 18;
    }
    if (archiveMirror.size() > 1 && !VerifyShoCConfigurationGraph())
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "ShoC configuration compatibility preflight failed");
        Core._destroy();
        SDL_Quit();
        return 19;
    }
    __android_log_write(ANDROID_LOG_INFO, LogTag, "xrCore private filesystem smoke test passed");

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
    AndroidVulkanRenderer renderer;
    if (!renderer.Initialize(window, appFilesPath))
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Vulkan renderer bootstrap failed");
        SDL_DestroyWindow(window);
        Core._destroy();
        SDL_Quit();
        return 15;
    }

    const Uint64 performanceFrequency = SDL_GetPerformanceFrequency();
    const Uint64 frameLoopStart = SDL_GetPerformanceCounter();
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
        const float elapsedSeconds = static_cast<float>(
            static_cast<double>(SDL_GetPerformanceCounter() - frameLoopStart) /
            static_cast<double>(performanceFrequency));
        if (!renderer.DrawFrame(frameIndex, elapsedSeconds))
        {
            __android_log_write(ANDROID_LOG_ERROR, LogTag, "Persistent Vulkan host frame failed");
            rendererFailed = true;
            running = false;
        }
        SDL_Delay(1);
    }

    renderer.Shutdown();
    SDL_DestroyWindow(window);
    Core._destroy();
    __android_log_write(ANDROID_LOG_INFO, LogTag, "xrCore lifecycle destroyed cleanly");
    SDL_Quit();
    return rendererFailed ? 16 : 0;
}

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
#include "AndroidVulkanRenderDevice.hpp"
#include "AndroidVulkanRenderServices.hpp"
#include "AndroidVulkanRendererModule.hpp"
#include "Include/xrAPI/xrAPI.h"
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

// mkdir -p, safe for both absolute and relative paths. Returns false only when
// a component cannot be created and does not already exist.
bool EnsureDirectoryRecursive(const std::string& path)
{
    if (path.empty())
        return true;

    std::string current;
    size_t start = 0;
    if (path[0] == '/')
    {
        current = "/";
        start = 1;
    }

    while (start <= path.size())
    {
        const size_t slash = path.find('/', start);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string component = path.substr(start, end - start);
        if (!component.empty())
        {
            current += component;
            if (mkdir(current.c_str(), 0700) != 0 && errno != EEXIST)
                return false;
        }
        if (slash == std::string::npos)
            break;
        current += '/';
        start = slash + 1;
    }
    return true;
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

bool PrepareFilesystemFixture(const char* appFilesPath, const std::string& fsgameContent,
    std::string& fsConfigPath)
{
    if (!appFilesPath || !appFilesPath[0])
        return false;

    const std::string root = std::string(appFilesPath) + "/xr-core-smoke";
    if (!EnsureDirectoryRecursive(root) ||
        !EnsureDirectoryRecursive(root + "/_appdata_") ||
        !EnsureDirectoryRecursive(root + "/_appdata_/logs") ||
        !EnsureDirectoryRecursive(root + "/_appdata_/savedgames") ||
        !EnsureDirectoryRecursive(root + "/_appdata_/screenshots") ||
        !EnsureDirectoryRecursive(root + "/gamedata"))
    {
        return false;
    }

    static constexpr char Marker[] = "openxray-android-vfs-smoke-v1\n";

    fsConfigPath = root + "/fsgame.ltx";
    return WriteFile(fsConfigPath, fsgameContent.c_str()) &&
        WriteFile(root + "/gamedata/android_smoke.txt", Marker);
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

// ---------------------------------------------------------------------------
// Universal game-resource discovery.
//
// The launcher grants a read-only Storage Access Framework tree. The engine VFS
// (CLocatorAPI) already mounts archives from the fsgame.ltx layout for all three
// games and for mods that add their own $arch_dir* entries. The host's only job
// is to walk that tree, find every archive and the game's own fsgame.ltx, mirror
// them into app-private storage with descriptor-backed symlinks, and emit an
// fsgame.ltx that matches the discovered layout.
// ---------------------------------------------------------------------------

enum class GameFlavor
{
    Unknown,
    ShadowOfChernobyl,
    ClearSky,
    CallOfPripyat,
};

pcstr GameFlavorFlag(GameFlavor flavor)
{
    switch (flavor)
    {
    case GameFlavor::ClearSky: return "-cs";
    case GameFlavor::CallOfPripyat: return "-cop";
    case GameFlavor::ShadowOfChernobyl:
    default: return "-shoc";
    }
}

const char* GameFlavorName(GameFlavor flavor)
{
    switch (flavor)
    {
    case GameFlavor::ShadowOfChernobyl: return "SoC";
    case GameFlavor::ClearSky: return "CS";
    case GameFlavor::CallOfPripyat: return "CoP";
    default: return "unknown";
    }
}

struct SafDiscoveredFile
{
    std::string path;               // relative to the selected tree root, '/' separated
    std::int64_t size = -1;
    bool archive = false;
    bool archiveNeedsRename = false; // ".db.ntp"/".xdb.ntp" suffix (usually ransomware-encrypted)
    std::string archiveName;         // engine-visible basename (after optional rename)
};

std::string LowerName(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::string BaseName(const std::string& path)
{
    const std::string::size_type slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string ParentDir(const std::string& path)
{
    const std::string::size_type slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string PathRelativeToRoot(const std::string& path, const std::string& root)
{
    if (root.empty())
        return path;
    if (path == root)
        return std::string();
    if (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/')
        return path.substr(root.size() + 1);
    return std::string();
}

bool IsArchiveByExtension(const std::string& baseLower)
{
    const std::string::size_type dot = baseLower.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    const std::string extension = baseLower.substr(dot);
    return extension.compare(0, 3, ".db") == 0 || extension.compare(0, 4, ".xdb") == 0;
}

// Files with a ".db.ntp"/".xdb.ntp" suffix are normally the originals left
// behind by ransomware ("винлокер") after encryption — irrecoverable, and only
// replaceable with clean copies. The engine only discovers *.db / *.xdb, so the
// host must never feed these to it blindly. They are still detected here so a
// genuinely intact archive that happens to carry the suffix can be mirrored
// under its real name after the CFS header is verified.
bool IsRepackArchiveName(const std::string& baseLower)
{
    static constexpr char DbNtp[] = ".db.ntp";
    static constexpr char XdbNtp[] = ".xdb.ntp";
    constexpr size_t DbNtpLength = sizeof(DbNtp) - 1;
    constexpr size_t XdbNtpLength = sizeof(XdbNtp) - 1;
    if (baseLower.size() > DbNtpLength &&
        baseLower.compare(baseLower.size() - DbNtpLength, DbNtpLength, DbNtp) == 0)
        return true;
    return baseLower.size() > XdbNtpLength &&
        baseLower.compare(baseLower.size() - XdbNtpLength, XdbNtpLength, XdbNtp) == 0;
}

bool IsFsgameName(const std::string& baseLower)
{
    return baseLower == "fsgame.ltx";
}

bool ShouldSkipDirectory(const std::string& baseLower)
{
    static const std::unordered_set<std::string> skip = {
        ".git", ".svn", ".hg", "$recycle.bin", "system volume information",
        "android", "dcim", "download", "pictures", "movies", "music",
        "alarms", "ringtones", "notifications", "podcasts",
    };
    return skip.count(baseLower) != 0;
}

// Depth-first walk of the SAF tree. Returns false only when a directory in the
// walk cannot be enumerated (i.e. the retained permission was revoked).
bool AndroidSafWalkTree(std::vector<SafDiscoveredFile>& files)
{
    files.clear();
    struct Pending
    {
        std::string path;
        int depth;
    };
    std::vector<Pending> stack;
    stack.push_back(Pending{"", 0});
    constexpr int MaxDepth = 14;

    while (!stack.empty())
    {
        Pending current = std::move(stack.back());
        stack.pop_back();

        std::vector<AndroidSafEntry> entries;
        if (!AndroidSafListDirectory(current.path.c_str(), entries))
            return false;

        for (const AndroidSafEntry& entry : entries)
        {
            if (entry.name.empty())
                continue;

            const std::string baseLower = LowerName(entry.name);
            if (entry.directory)
            {
                if (current.depth >= MaxDepth || ShouldSkipDirectory(baseLower))
                    continue;
                std::string child = current.path.empty() ? entry.name : current.path + "/" + entry.name;
                stack.push_back(Pending{std::move(child), current.depth + 1});
                continue;
            }

            SafDiscoveredFile file;
            file.path = current.path.empty() ? entry.name : current.path + "/" + entry.name;
            file.size = entry.size;

            if (IsArchiveByExtension(baseLower))
            {
                file.archive = true;
                file.archiveName = entry.name;
            }
            else if (IsRepackArchiveName(baseLower))
            {
                file.archive = true;
                file.archiveNeedsRename = true;
                std::string renamed = entry.name;
                renamed.erase(renamed.size() - 4); // drop the trailing ".ntp"
                file.archiveName = std::move(renamed);
            }
            files.push_back(std::move(file));
        }
    }
    return true;
}

bool HasArchiveMagic(const std::array<unsigned char, 16>& header)
{
    // CFS_HeaderChunkID == 666 == 0x29A, little-endian.
    return header[0] == 0x9A && header[1] == 0x02 && header[2] == 0x00 && header[3] == 0x00;
}

bool ReadSafFileText(const std::string& relativePath, std::string& output)
{
    output.clear();
    const int descriptor = AndroidSafOpenFile(relativePath.c_str());
    if (descriptor < 0)
        return false;

    const off_t size = lseek(descriptor, 0, SEEK_END);
    if (size <= 0 || size > 4 * 1024 * 1024)
    {
        close(descriptor);
        return false;
    }
    output.resize(static_cast<size_t>(size));
    const ssize_t read = pread(descriptor, &output[0], static_cast<size_t>(size), 0);
    close(descriptor);
    return read == size;
}

// Extract the "add" field (4th item, 0-based index 3) from an fsgame line like
// "$game_config$ = true|false|$game_data$|configs\". Returns "" when absent.
std::string FsgameAddField(const std::string& content, const std::string& variable)
{
    std::istringstream stream(content);
    std::string line;
    const std::string needle = LowerName(variable);
    while (std::getline(stream, line))
    {
        const std::string::size_type eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string id = line.substr(0, eq);
        id.erase(id.begin(), std::find_if(id.begin(), id.end(), [](unsigned char c) { return !std::isspace(c); }));
        id.erase(std::find_if(id.rbegin(), id.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), id.end());
        if (LowerName(id) != needle)
            continue;

        const std::string value = line.substr(eq + 1);
        std::vector<std::string> fields;
        size_t start = 0;
        while (start <= value.size())
        {
            const size_t bar = value.find('|', start);
            fields.push_back(value.substr(start, (bar == std::string::npos ? value.size() : bar) - start));
            if (bar == std::string::npos)
                break;
            start = bar + 1;
        }
        return fields.size() >= 4 ? fields[3] : std::string();
    }
    return std::string();
}

// Rewrite the writable-path lines so logs/saves/screenshots live in the
// app-private mirror (the SAF tree is read-only). Every other line is kept
// verbatim so mods that add custom $arch_dir* / $game_data$ entries keep working.
bool RewriteFsgame(const std::string& source, std::string& output)
{
    std::istringstream stream(source);
    std::string line;
    output.clear();
    bool rewritten = false;
    while (std::getline(stream, line))
    {
        const std::string::size_type eq = line.find('=');
        bool handled = false;
        if (eq != std::string::npos)
        {
            std::string id = line.substr(0, eq);
            id.erase(id.begin(), std::find_if(id.begin(), id.end(), [](unsigned char c) { return !std::isspace(c); }));
            id.erase(std::find_if(id.rbegin(), id.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), id.end());
            const std::string lower = LowerName(id);
            if (lower == "$app_data_root$")
            {
                output += "$app_data_root$ = true|false|$fs_root$|_appdata_\\\n";
                rewritten = handled = true;
            }
            else if (lower == "$logs$")
            {
                output += "$logs$ = true|false|$app_data_root$|logs\\\n";
                rewritten = handled = true;
            }
            else if (lower == "$screenshots$")
            {
                output += "$screenshots$ = true|false|$app_data_root$|screenshots\\\n";
                rewritten = handled = true;
            }
            else if (lower == "$game_saves$")
            {
                output += "$game_saves$ = true|false|$app_data_root$|savedgames\\\n";
                rewritten = handled = true;
            }
            else if (lower == "$downloads$")
            {
                output += "$downloads$ = false|false|$app_data_root$\n";
                rewritten = handled = true;
            }
        }
        if (!handled)
        {
            output += line;
            output += '\n';
        }
    }
    return rewritten;
}

std::string SynthesizeFsgame(GameFlavor flavor)
{
    const bool soc = flavor == GameFlavor::ShadowOfChernobyl;
    const std::string config = soc ? "config" : "configs";
    std::string result;
    result += "; OpenXRay Android synthesized fsgame (no game fsgame.ltx found).\n";
    result += "$app_data_root$ = true|false|$fs_root$|_appdata_\\\n";
    result += "$arch_dir$ = false|false|$fs_root$\n";
    result += "$game_arch_mp$ = false|false|$fs_root$|mp\\\n";
    result += "$arch_dir_levels$ = false|false|$fs_root$|levels\\\n";
    result += "$arch_dir_resources$ = false|false|$fs_root$|resources\\\n";
    result += "$arch_dir_localization$ = false|false|$fs_root$|localization\\\n";
    result += "$arch_dir_patches$ = false|true|$fs_root$|patches\\\n";
    result += "$game_data$ = true|true|$fs_root$|gamedata\\\n";
    result += "$game_ai$ = true|false|$game_data$|ai\\\n";
    result += "$game_spawn$ = true|false|$game_data$|spawns\\\n";
    result += "$game_levels$ = true|false|$game_data$|levels\\\n";
    result += "$game_meshes$ = true|true|$game_data$|meshes\\|*.ogf;*.omf|Game Object files\n";
    result += "$game_anims$ = true|true|$game_data$|anims\\|*.anm;*.anms|Animation files\n";
    result += "$game_dm$ = true|true|$game_data$|meshes\\|*.dm|Detail Model files\n";
    result += "$game_shaders$ = true|true|$game_data$|shaders\\\n";
    result += "$game_sounds$ = true|true|$game_data$|sounds\\\n";
    result += "$game_textures$ = true|true|$game_data$|textures\\\n";
    result += "$game_config$ = true|false|$game_data$|" + config + "\\\n";
    result += "$game_weathers$ = true|false|$game_config$|environment\\weathers\n";
    result += "$game_weather_effects$ = true|false|$game_config$|environment\\weather_effects\n";
    result += "$textures$ = true|true|$game_data$|textures\\\n";
    result += "$level$ = false|false|$game_levels$\n";
    result += "$game_scripts$ = true|false|$game_data$|scripts\\|*.script|Game script files\n";
    result += "$logs$ = true|false|$app_data_root$|logs\\\n";
    result += "$screenshots$ = true|false|$app_data_root$|screenshots\\\n";
    result += "$game_saves$ = true|false|$app_data_root$|savedgames\\\n";
    result += "$downloads$ = false|false|$app_data_root$\n";
    return result;
}

struct GameLayout
{
    GameFlavor flavor = GameFlavor::Unknown;
    std::string gameRoot;                        // relative dir under the selected tree
    std::string fsgameRelativePath;              // game's own fsgame.ltx ("" if none)
    std::string fsgameContent;                   // rewritten / synthesized fsgame text
    std::vector<const SafDiscoveredFile*> archives; // archives under gameRoot
};

bool DetectGameLayout(const std::vector<SafDiscoveredFile>& files, GameLayout& layout)
{
    layout = GameLayout{};

    // Find the game's own fsgame.ltx (shallowest wins on ambiguity).
    std::string bestFsgame;
    size_t bestDepth = static_cast<size_t>(-1);
    for (const SafDiscoveredFile& file : files)
    {
        if (IsFsgameName(LowerName(BaseName(file.path))))
        {
            const size_t depth = static_cast<size_t>(std::count(file.path.begin(), file.path.end(), '/'));
            if (depth < bestDepth)
            {
                bestDepth = depth;
                bestFsgame = file.path;
            }
        }
    }
    if (!bestFsgame.empty())
    {
        layout.fsgameRelativePath = bestFsgame;
        layout.gameRoot = ParentDir(bestFsgame);
    }

    // Infer the game root for the "no fsgame.ltx" case from the shallowest
    // archive that implies a game root. This must happen before flavor
    // classification so archive paths can be resolved relative to the root.
    if (layout.gameRoot.empty())
    {
        for (const SafDiscoveredFile& file : files)
        {
            if (!file.archive)
                continue;
            const std::string baseLower = LowerName(BaseName(file.path));
            if (baseLower.rfind("gamedata.db", 0) == 0)
            {
                layout.gameRoot = ParentDir(file.path);
                break;
            }
            const std::string dirBase = LowerName(BaseName(ParentDir(file.path)));
            if (dirBase == "resources" || dirBase == "levels")
            {
                layout.gameRoot = ParentDir(ParentDir(file.path));
                break;
            }
        }
    }

    // Classify archives under the game root and infer the flavor.
    bool hasRootGamedataDb = false;
    bool hasResourcesArchives = false;
    std::string copMarker; // "xefis_movies.db" / "resources.db3" / "resources.db4"
    for (const SafDiscoveredFile& file : files)
    {
        if (!file.archive)
            continue;
        const std::string rel = PathRelativeToRoot(file.path, layout.gameRoot);
        if (rel.empty())
            continue;

        const std::string baseLower = LowerName(BaseName(rel));
        const std::string dirLower = LowerName(ParentDir(rel));

        if (baseLower.rfind("gamedata.db", 0) == 0)
            hasRootGamedataDb = true;
        if (dirLower == "resources")
            hasResourcesArchives = true;
        if (baseLower == "xefis_movies.db" || baseLower == "resources.db3" || baseLower == "resources.db4")
            copMarker = baseLower;
    }

    GameFlavor flavor = GameFlavor::Unknown;
    if (!bestFsgame.empty())
    {
        std::string content;
        if (ReadSafFileText(layout.fsgameRelativePath, content))
        {
            std::string add = LowerName(FsgameAddField(content, "$game_config$"));
            add.erase(std::remove_if(add.begin(), add.end(),
                [](unsigned char c) { return std::isspace(c) || c == '\\' || c == '/'; }), add.end());
            if (add == "config")
                flavor = GameFlavor::ShadowOfChernobyl;
            else if (add == "configs")
                flavor = GameFlavor::ClearSky; // refined below
        }
    }
    if (flavor == GameFlavor::Unknown)
    {
        if (hasResourcesArchives || !copMarker.empty())
            flavor = GameFlavor::ClearSky;
        else if (hasRootGamedataDb)
            flavor = GameFlavor::ShadowOfChernobyl;
    }
    if (flavor == GameFlavor::ClearSky && !copMarker.empty())
        flavor = GameFlavor::CallOfPripyat;
    layout.flavor = flavor;

    // Collect archives under the game root.
    for (const SafDiscoveredFile& file : files)
    {
        if (!file.archive)
            continue;
        if (PathRelativeToRoot(file.path, layout.gameRoot).empty())
            continue;
        layout.archives.push_back(&file);
    }

    // Build fsgame.ltx: reuse the game's own (rewritten) or synthesize.
    if (!bestFsgame.empty() && ReadSafFileText(layout.fsgameRelativePath, layout.fsgameContent))
    {
        std::string rewritten;
        if (RewriteFsgame(layout.fsgameContent, rewritten))
            layout.fsgameContent = std::move(rewritten);
    }
    else
    {
        layout.fsgameContent = SynthesizeFsgame(layout.flavor);
    }

    return !layout.archives.empty() || layout.flavor != GameFlavor::Unknown;
}

void RemoveStaleMirrorSymlinks(const std::string& root)
{
    DIR* directory = opendir(root.c_str());
    if (!directory)
        return;

    while (dirent* entry = readdir(directory))
    {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
            continue;
        const std::string path = root + "/" + entry->d_name;
        struct stat status{};
        if (lstat(path.c_str(), &status) != 0)
            continue;
        if (S_ISLNK(status.st_mode))
            unlink(path.c_str());
        else if (S_ISDIR(status.st_mode))
            RemoveStaleMirrorSymlinks(path);
    }
    closedir(directory);
}

// Mirrors the discovered archives into the private mirror root, preserving
// their layout relative to the game root, so CLocatorAPI can mount them exactly
// as the game's own fsgame.ltx expects.
class SafGameMirror
{
public:
    SafGameMirror() = default;
    SafGameMirror(const SafGameMirror&) = delete;
    SafGameMirror& operator=(const SafGameMirror&) = delete;

    ~SafGameMirror()
    {
        xrClearAndroidArchiveDescriptors();
        for (const int descriptor : descriptors)
            close(descriptor);
    }

    bool Initialize(const std::string& mirrorRoot, const GameLayout& layout)
    {
        xrClearAndroidArchiveDescriptors();
        RemoveStaleMirrorSymlinks(mirrorRoot);

        size_t mirrored = 0;
        for (const SafDiscoveredFile* file : layout.archives)
        {
            const std::string relative = PathRelativeToRoot(file->path, layout.gameRoot);
            if (relative.empty())
                continue;

            // Mirror under the engine-visible basename. A ".db.ntp" archive is
            // mirrored as ".db" so CLocatorAPI can discover it after the CFS
            // header is verified.
            const std::string parent = ParentDir(relative);
            const std::string mirrorRelative = parent.empty()
                ? file->archiveName : parent + "/" + file->archiveName;

            const int descriptor = AndroidSafOpenFile(file->path.c_str());
            if (descriptor < 0)
            {
                __android_log_print(ANDROID_LOG_WARN, LogTag,
                    "Cannot open resource: %s", file->path.c_str());
                continue;
            }

            // Validate seekability and, for renamed files, the CFS header.
            const off_t size = lseek(descriptor, 0, SEEK_END);
            std::array<unsigned char, 16> header{};
            const ssize_t bytesRead = size >= static_cast<off_t>(header.size())
                ? pread(descriptor, header.data(), header.size(), 0)
                : -1;
            if (size < static_cast<off_t>(header.size()) || bytesRead != static_cast<ssize_t>(header.size()))
            {
                close(descriptor);
                __android_log_print(ANDROID_LOG_WARN, LogTag,
                    "Resource is not seekable: %s", file->path.c_str());
                continue;
            }
            if (file->archiveNeedsRename && !HasArchiveMagic(header))
            {
                close(descriptor);
                __android_log_print(ANDROID_LOG_WARN, LogTag,
                    "Skipping encrypted/corrupted archive (no CFS header; likely ransomware-encrypted, replace with a clean copy): %s",
                    file->path.c_str());
                continue;
            }

            const std::string mirrorPath = mirrorRoot + "/" + mirrorRelative;
            if (!EnsureDirectoryRecursive(ParentDir(mirrorPath)))
            {
                close(descriptor);
                __android_log_print(ANDROID_LOG_WARN, LogTag,
                    "Cannot create mirror directory for %s", mirrorRelative.c_str());
                continue;
            }

            const std::string target = "/proc/self/fd/" + std::to_string(descriptor);
            if (symlink(target.c_str(), mirrorPath.c_str()) != 0)
            {
                close(descriptor);
                __android_log_print(ANDROID_LOG_WARN, LogTag,
                    "Cannot symlink %s", relative.c_str());
                continue;
            }
            if (!xrRegisterAndroidArchiveDescriptor(mirrorPath.c_str(), descriptor))
            {
                unlink(mirrorPath.c_str());
                close(descriptor);
                __android_log_print(ANDROID_LOG_WARN, LogTag,
                    "Cannot register descriptor for %s", relative.c_str());
                continue;
            }
            descriptors.push_back(descriptor);
            ++mirrored;
        }

        __android_log_print(ANDROID_LOG_INFO, LogTag, "Mirrored %zu archives into private storage", mirrored);
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
    __android_log_print(ANDROID_LOG_INFO, LogTag, "Private engine files: %s", appFilesPath);
    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "Launch profile: %s; render scale: %s%%; view distance: %.0f%%",
        profileId, renderScale, viewDistancePercent);
    if (IsGameSpyMasterServerConfigured())
        __android_log_print(ANDROID_LOG_INFO, LogTag, "Custom SoC master: %s:%hu", masterHost, masterPort);
    else
        __android_log_write(ANDROID_LOG_INFO, LogTag, "Custom SoC master disabled; using compatibility backend");

    std::vector<AndroidSafEntry> gameDataRootEntries;
    if (!AndroidSafListDirectory("", gameDataRootEntries))
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Selected SAF game-data tree is not readable");
        return 5;
    }
    __android_log_print(
        ANDROID_LOG_INFO, LogTag, "Selected SAF game-data tree is readable (%zu root entries)",
        gameDataRootEntries.size());

    // Recursively discover archives and the game's own fsgame.ltx across the
    // whole selected tree, so licensed SoC/CS/CoP installs, repacks and mods
    // with custom layouts all work without a hardcoded path.
    std::vector<SafDiscoveredFile> treeFiles;
    if (!AndroidSafWalkTree(treeFiles))
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Could not walk the selected game-data tree");
        return 20;
    }
    __android_log_print(ANDROID_LOG_INFO, LogTag, "Walked selected tree: %zu files", treeFiles.size());

    GameLayout layout;
    const bool gameDetected = DetectGameLayout(treeFiles, layout);
    if (gameDetected)
    {
        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "Detected game: %s, root=\"%s\", archives=%zu, fsgame=\"%s\"",
            GameFlavorName(layout.flavor), layout.gameRoot.c_str(),
            layout.archives.size(), layout.fsgameRelativePath.c_str());
    }
    else
    {
        __android_log_write(ANDROID_LOG_WARN, LogTag,
            "No recognizable game layout found; continuing in resource-free host mode");
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
    const std::string& fsgame = layout.fsgameContent.empty()
        ? SynthesizeFsgame(GameFlavor::ShadowOfChernobyl) : layout.fsgameContent;
    if (!PrepareFilesystemFixture(appFilesPath, fsgame, fixtureConfig))
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Could not prepare private xrCore filesystem fixture");
        SDL_Quit();
        return 3;
    }

    const std::string fixtureRoot = std::string(appFilesPath) + "/xr-core-smoke";
    SafGameMirror archiveMirror;
    archiveMirror.Initialize(fixtureRoot, layout);
    __android_log_print(
        ANDROID_LOG_INFO, LogTag, "Prepared %zu descriptor-backed SAF archives", archiveMirror.size());

    // Exercise the real locator on an engine-owned fixture. Archives from the
    // selected read-only SAF tree are exposed through borrowed descriptors;
    // writable state stays in the app-private directory.
    const std::string coreCommandLine = std::string(GameFlavorFlag(layout.flavor)) + " -nolog";
    Core.Initialize("OpenXRay Android", coreCommandLine.c_str(), true, fixtureConfig.c_str());
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

    // A single archive is the redistributable fixture, not a full install.
    const bool hasArchives = archiveMirror.size() > 1;
    if (archiveMirror.size() == 1)
    {
        __android_log_write(ANDROID_LOG_WARN, LogTag,
            "Only one archive found; treating it as an incomplete installation");
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
        commandLine += " -nolog -nosplash -fsltx ";
        commandLine += fixtureConfig;
        if (engineArguments && engineArguments[0])
        {
            commandLine += ' ';
            commandLine += engineArguments;
        }
        AndroidVulkanRendererModule rendererModule(appFilesPath);
        const std::array<RendererModule*, 2> renderModules = { &rendererModule, nullptr };

        __android_log_print(ANDROID_LOG_INFO, LogTag,
            "Starting full xrEngine/xrGame lifecycle with Android Vulkan renderer module (%s mode)",
            GameFlavorName(layout.flavor));
        CApplication application(commandLine.c_str(), &xrGame, renderModules);
        return application.Run();
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

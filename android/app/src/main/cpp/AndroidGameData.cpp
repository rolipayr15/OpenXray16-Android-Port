#include "Common/Platform.hpp"
#include "xrCore/xrCore.h"
#include "xrCore/Android/AndroidArchiveDescriptors.hpp"

#include "AndroidGameData.hpp"
#include "AndroidSafBridge.hpp"

#include <android/log.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <unordered_set>

namespace
{
constexpr const char* LogTag = "OpenXRay";
constexpr int MaxWalkDepth = 14;

std::string LowerName(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::string Trim(const std::string& value)
{
    const auto begin = std::find_if(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); });
    const auto end = std::find_if(value.rbegin(), value.rend(), [](unsigned char c) { return !std::isspace(c); }).base();
    return begin < end ? std::string(begin, end) : std::string();
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

std::string JoinPath(const std::string& left, const std::string& right)
{
    if (left.empty())
        return right;
    if (right.empty())
        return left;
    return left + "/" + right;
}

// Path of `path` below `root`; empty when it is outside (or equal to) root.
std::string PathRelativeToRoot(const std::string& path, const std::string& root)
{
    if (root.empty())
        return path;
    if (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/')
        return path.substr(root.size() + 1);
    return std::string();
}

bool EnsureDirectoryRecursive(const std::string& path)
{
    if (path.empty())
        return true;

    std::string current = path[0] == '/' ? "/" : "";
    size_t start = path[0] == '/' ? 1 : 0;
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

bool WriteTextFile(const std::string& path, const std::string& contents)
{
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;
    const bool written = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    return std::fclose(file) == 0 && written;
}

bool ReadDescriptorText(int descriptor, std::string& output)
{
    output.clear();
    if (descriptor < 0)
        return false;
    const off_t size = lseek(descriptor, 0, SEEK_END);
    bool ok = size > 0 && size <= 4 * 1024 * 1024;
    if (ok)
    {
        output.resize(static_cast<size_t>(size));
        ok = pread(descriptor, &output[0], output.size(), 0) == size;
    }
    close(descriptor);
    return ok;
}

bool IsArchiveName(const std::string& baseLower)
{
    // Matches *.db, *.db0..*.dbf, *.xdb, *.xdb0.. (the extensions the locator
    // mounts). Windows thumbnail caches are not archives.
    if (baseLower == "thumbs.db" || baseLower == "desktop.db")
        return false;
    const std::string::size_type dot = baseLower.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    const std::string extension = baseLower.substr(dot);
    return extension.compare(0, 3, ".db") == 0 || extension.compare(0, 4, ".xdb") == 0;
}

// ".db.ntp"/".xdb.ntp" are usually originals left behind by ransomware. They
// are only mounted (under their real name) when the CFS header is intact.
bool IsSuffixedArchiveName(const std::string& baseLower)
{
    for (const char* suffix : { ".db.ntp", ".xdb.ntp" })
    {
        const size_t length = std::strlen(suffix);
        if (baseLower.size() > length && baseLower.compare(baseLower.size() - length, length, suffix) == 0)
            return true;
    }
    return false;
}

bool HasArchiveMagic(const std::array<unsigned char, 16>& header)
{
    // CFS_HeaderChunkID == 666 == 0x29A, little-endian.
    return header[0] == 0x9A && header[1] == 0x02 && header[2] == 0x00 && header[3] == 0x00;
}

// Seekable descriptor with at least a header; optionally a CFS header.
bool ValidateArchiveDescriptor(int descriptor, bool requireMagic)
{
    std::array<unsigned char, 16> header{};
    const off_t size = lseek(descriptor, 0, SEEK_END);
    const bool readable = size >= static_cast<off_t>(header.size()) &&
        pread(descriptor, header.data(), header.size(), 0) == static_cast<ssize_t>(header.size());
    return readable && (!requireMagic || HasArchiveMagic(header));
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

// --- fsgame.ltx handling ----------------------------------------------------

struct FsgameLine
{
    std::string id;                  // lower-case alias, e.g. "$game_data$"
    std::vector<std::string> fields; // '|'-separated value, untrimmed
};

bool ParseFsgameLine(const std::string& line, FsgameLine& parsed)
{
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == ';')
        return false;
    const std::string::size_type eq = line.find('=');
    if (eq == std::string::npos)
        return false;

    parsed.id = LowerName(Trim(line.substr(0, eq)));
    parsed.fields.clear();
    const std::string value = line.substr(eq + 1);
    size_t start = 0;
    while (true)
    {
        const size_t bar = value.find('|', start);
        parsed.fields.push_back(value.substr(start, (bar == std::string::npos ? value.size() : bar) - start));
        if (bar == std::string::npos)
            break;
        start = bar + 1;
    }
    return true;
}

std::string FsgameField(const std::string& content, const char* alias, size_t index)
{
    std::istringstream stream(content);
    std::string line;
    FsgameLine parsed;
    while (std::getline(stream, line))
    {
        if (ParseFsgameLine(line, parsed) && parsed.id == alias && parsed.fields.size() > index)
            return Trim(parsed.fields[index]);
    }
    return std::string();
}

bool IsWritableAlias(const std::string& id)
{
    return id == "$app_data_root$" || id == "$logs$" || id == "$screenshots$" ||
        id == "$game_saves$" || id == "$downloads$";
}

// Rewrites a game's fsgame.ltx for the private engine root: writable aliases
// move to app-private storage and, when gameRootAlias is set, every other
// alias rooted at $fs_root$ is rebased onto it. All other lines are kept
// verbatim, so mods with custom $arch_dir*$ / $game_data$ layouts keep working.
std::string RewriteFsgame(const std::string& source, const char* gameRootAlias)
{
    std::string output;
    if (gameRootAlias)
    {
        output += "; OpenXRay Android: the installation is reached through this alias.\n";
        output += std::string(gameRootAlias) + " = false|false|$fs_root$|gameroot\\\n";
    }

    bool hasAppData = false;
    std::istringstream stream(source);
    std::string line;
    FsgameLine parsed;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!ParseFsgameLine(line, parsed))
        {
            output += line;
            output += '\n';
            continue;
        }

        if (parsed.id == "$app_data_root$")
        {
            output += "$app_data_root$ = true|false|$fs_root$|_appdata_\\\n";
            hasAppData = true;
            continue;
        }
        if (parsed.id == "$logs$")
        {
            output += "$logs$ = true|false|$app_data_root$|logs\\\n";
            continue;
        }
        if (parsed.id == "$screenshots$")
        {
            output += "$screenshots$ = true|false|$app_data_root$|screenshots\\\n";
            continue;
        }
        if (parsed.id == "$game_saves$")
        {
            output += "$game_saves$ = true|false|$app_data_root$|savedgames\\\n";
            continue;
        }
        if (parsed.id == "$downloads$")
        {
            output += "$downloads$ = false|false|$app_data_root$\n";
            continue;
        }

        if (gameRootAlias && parsed.fields.size() >= 3 && !IsWritableAlias(parsed.id) &&
            LowerName(Trim(parsed.fields[2])) == "$fs_root$")
        {
            std::string& root = parsed.fields[2];
            root.replace(root.find_first_not_of(" \t"), std::strlen("$fs_root$"), gameRootAlias);
            std::string rebuilt = line.substr(0, line.find('=') + 1);
            for (size_t index = 0; index < parsed.fields.size(); ++index)
            {
                if (index)
                    rebuilt += '|';
                rebuilt += parsed.fields[index];
            }
            output += rebuilt;
            output += '\n';
            continue;
        }

        output += line;
        output += '\n';
    }

    if (!hasAppData)
    {
        // The locator asserts that $app_data_root$ exists; keep it first so
        // later aliases can refer to it.
        output = "$app_data_root$ = true|false|$fs_root$|_appdata_\\\n" + output;
        output += "$logs$ = true|false|$app_data_root$|logs\\\n";
        output += "$screenshots$ = true|false|$app_data_root$|screenshots\\\n";
        output += "$game_saves$ = true|false|$app_data_root$|savedgames\\\n";
    }
    return output;
}

// Equivalent of the stock fsgame.ltx files for installs that ship none.
std::string SynthesizeFsgame(GameFlavor flavor)
{
    const std::string config = flavor == GameFlavor::ShadowOfChernobyl ? "config" : "configs";
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

// --- sources ------------------------------------------------------------------

class DirectGameDataSource final : public GameDataSource
{
public:
    explicit DirectGameDataSource(std::string rootPath) : root(std::move(rootPath))
    {
        while (root.size() > 1 && root.back() == '/')
            root.pop_back();
    }

    const char* Name() const override { return "direct filesystem"; }

    bool Walk(std::vector<GameDataFile>& files) override
    {
        files.clear();
        struct Pending
        {
            std::string path;
            int depth;
        };
        std::vector<Pending> stack{ { "", 0 } };
        while (!stack.empty())
        {
            const Pending current = std::move(stack.back());
            stack.pop_back();

            DIR* directory = opendir(AbsolutePath(current.path).c_str());
            if (!directory)
            {
                if (current.path.empty())
                    return false;
                continue;
            }
            while (dirent* entry = readdir(directory))
            {
                if (entry->d_name[0] == '.')
                    continue;
                const std::string relative = JoinPath(current.path, entry->d_name);
                struct stat status{};
                if (stat(AbsolutePath(relative).c_str(), &status) != 0)
                    continue;

                GameDataFile file;
                file.path = relative;
                file.directory = S_ISDIR(status.st_mode);
                file.size = file.directory ? 0 : static_cast<std::int64_t>(status.st_size);
                file.modified = static_cast<std::int64_t>(status.st_mtime);
                if (file.directory)
                {
                    if (current.depth >= MaxWalkDepth || ShouldSkipDirectory(LowerName(entry->d_name)))
                        continue;
                    stack.push_back(Pending{ relative, current.depth + 1 });
                }
                else if (!S_ISREG(status.st_mode))
                {
                    continue;
                }
                files.push_back(std::move(file));
            }
            closedir(directory);
        }
        return true;
    }

    int Open(const std::string& relativePath) override
    {
        return open(AbsolutePath(relativePath).c_str(), O_RDONLY | O_CLOEXEC);
    }

    std::string AbsolutePath(const std::string& relativePath) const override
    {
        return relativePath.empty() ? root : root + "/" + relativePath;
    }

private:
    std::string root;
};

class SafGameDataSource final : public GameDataSource
{
public:
    const char* Name() const override { return "Storage Access Framework"; }

    bool Walk(std::vector<GameDataFile>& files) override
    {
        files.clear();
        std::vector<AndroidSafTreeEntry> entries;
        if (!AndroidSafWalkTree(entries))
            return false;
        files.reserve(entries.size());
        for (AndroidSafTreeEntry& entry : entries)
        {
            GameDataFile file;
            file.path = std::move(entry.path);
            file.size = entry.size;
            file.modified = entry.modified;
            file.directory = entry.directory;
            files.push_back(std::move(file));
        }
        return true;
    }

    int Open(const std::string& relativePath) override { return AndroidSafOpenFile(relativePath.c_str()); }
    std::string AbsolutePath(const std::string&) const override { return std::string(); }
};

int OpenSafVirtualFile(pcstr sourceId) { return AndroidSafOpenFile(sourceId); }

void RemoveStaleLinks(const std::string& root)
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
            RemoveStaleLinks(path);
    }
    closedir(directory);
}
} // namespace

GameFlavor ParseGameMode(const char* mode)
{
    const std::string value = LowerName(mode ? mode : "");
    if (value == "shoc" || value == "soc")
        return GameFlavor::ShadowOfChernobyl;
    if (value == "cs")
        return GameFlavor::ClearSky;
    if (value == "cop")
        return GameFlavor::CallOfPripyat;
    if (value == "unlock")
        return GameFlavor::Unlocked;
    return GameFlavor::Unknown;
}

const char* GameFlavorFlag(GameFlavor flavor)
{
    switch (flavor)
    {
    case GameFlavor::ShadowOfChernobyl: return "-shoc";
    case GameFlavor::ClearSky: return "-cs";
    case GameFlavor::CallOfPripyat: return "-cop";
    case GameFlavor::Unlocked: return "-unlock_game_mode";
    default: return "";
    }
}

const char* GameFlavorName(GameFlavor flavor)
{
    switch (flavor)
    {
    case GameFlavor::ShadowOfChernobyl: return "Shadow of Chernobyl";
    case GameFlavor::ClearSky: return "Clear Sky";
    case GameFlavor::CallOfPripyat: return "Call of Pripyat";
    case GameFlavor::Unlocked: return "unlocked";
    default: return "auto";
    }
}

std::unique_ptr<GameDataSource> CreateDirectGameDataSource(const std::string& root)
{
    return std::make_unique<DirectGameDataSource>(root);
}

std::unique_ptr<GameDataSource> CreateSafGameDataSource() { return std::make_unique<SafGameDataSource>(); }

bool DetectGameLayout(GameDataSource& source, const std::vector<GameDataFile>& files,
    GameFlavor requested, GameLayout& layout)
{
    layout = GameLayout{};
    layout.flavor = requested;
    layout.flavorForced = requested != GameFlavor::Unknown;

    // 1. The game's own fsgame.ltx defines the root (shallowest wins).
    size_t bestDepth = static_cast<size_t>(-1);
    for (const GameDataFile& file : files)
    {
        if (file.directory || LowerName(BaseName(file.path)) != "fsgame.ltx")
            continue;
        const size_t depth = static_cast<size_t>(std::count(file.path.begin(), file.path.end(), '/'));
        if (depth < bestDepth)
        {
            bestDepth = depth;
            layout.fsgameRelativePath = file.path;
        }
    }
    bool rootKnown = !layout.fsgameRelativePath.empty();
    if (rootKnown)
        layout.gameRoot = ParentDir(layout.fsgameRelativePath);

    // 2. Otherwise infer it from the shallowest marker: a gamedata folder, a
    //    root gamedata.db* archive, or the resources/levels archive folders.
    if (!rootKnown)
    {
        size_t markerDepth = static_cast<size_t>(-1);
        for (const GameDataFile& file : files)
        {
            const std::string baseLower = LowerName(BaseName(file.path));
            const std::string parentLower = LowerName(BaseName(ParentDir(file.path)));
            std::string candidate;
            bool matched = false;
            if (file.directory && baseLower == "gamedata")
            {
                candidate = ParentDir(file.path);
                matched = true;
            }
            else if (!file.directory && baseLower.rfind("gamedata.db", 0) == 0)
            {
                candidate = ParentDir(file.path);
                matched = true;
            }
            else if (!file.directory && IsArchiveName(baseLower) &&
                (parentLower == "resources" || parentLower == "levels"))
            {
                candidate = ParentDir(ParentDir(file.path));
                matched = true;
            }
            if (!matched)
                continue;
            const size_t depth = static_cast<size_t>(std::count(candidate.begin(), candidate.end(), '/')) +
                (candidate.empty() ? 0 : 1);
            if (depth < markerDepth)
            {
                markerDepth = depth;
                layout.gameRoot = candidate;
                rootKnown = true;
            }
        }
    }
    if (!rootKnown)
        return false;

    const auto underRoot = [&layout](const std::string& path) {
        return layout.gameRoot.empty() || !PathRelativeToRoot(path, layout.gameRoot).empty();
    };

    bool hasGamedataDir = false;
    bool hasConfigDir = false;
    bool hasConfigsDir = false;
    for (const GameDataFile& file : files)
    {
        if (!underRoot(file.path))
            continue;
        const std::string relativeLower = LowerName(PathRelativeToRoot(file.path, layout.gameRoot));
        if (file.directory)
        {
            hasGamedataDir |= relativeLower == "gamedata";
            hasConfigDir |= relativeLower == "gamedata/config";
            hasConfigsDir |= relativeLower == "gamedata/configs";
            continue;
        }
        const std::string baseLower = LowerName(BaseName(file.path));
        if (IsArchiveName(baseLower) || IsSuffixedArchiveName(baseLower))
            ++layout.archiveCount;
        else
            ++layout.looseFileCount;
    }

    if (!layout.fsgameRelativePath.empty() &&
        !ReadDescriptorText(source.Open(layout.fsgameRelativePath), layout.fsgameContent))
    {
        __android_log_print(ANDROID_LOG_WARN, LogTag, "Cannot read %s; synthesizing fsgame.ltx",
            layout.fsgameRelativePath.c_str());
        layout.fsgameRelativePath.clear();
    }

    // 3. A first guess at the flavor. Only the ShoC config folder name is
    //    decisive here; CS and CoP share "configs" and are told apart after
    //    mounting by RefineGameFlavorFromVfs.
    if (!layout.flavorForced)
    {
        std::string config = LowerName(FsgameField(layout.fsgameContent, "$game_config$", 3));
        config.erase(std::remove_if(config.begin(), config.end(),
            [](unsigned char c) { return std::isspace(c) || c == '\\' || c == '/'; }), config.end());
        if (config == "config" || (config.empty() && hasConfigDir && !hasConfigsDir))
            layout.flavor = GameFlavor::ShadowOfChernobyl;
    }

    layout.detected = layout.archiveCount > 0 || hasGamedataDir || !layout.fsgameRelativePath.empty();
    return layout.detected;
}

GameDataMount::~GameDataMount()
{
    if (!mounted)
        return;
    xrClearAndroidVirtualFiles();
    xrSetAndroidVirtualFileOpener(nullptr);
}

bool GameDataMount::Mount(GameDataSource& source, const std::vector<GameDataFile>& files,
    const GameLayout& layout, const std::string& privateRoot, std::string& fsgamePath)
{
    archives = 0;
    looseFiles = 0;
    xrClearAndroidVirtualFiles();
    xrClearAndroidArchiveDescriptors();

    for (const char* directory : { "", "/_appdata_", "/_appdata_/logs", "/_appdata_/savedgames",
             "/_appdata_/screenshots", "/gamedata" })
    {
        if (!EnsureDirectoryRecursive(privateRoot + directory))
            return false;
    }
    // Earlier host versions mirrored archives through symlinks; the
    // installation is now reached through "gameroot" or virtual files only.
    RemoveStaleLinks(privateRoot);

    const std::string base = layout.fsgameContent.empty()
        ? SynthesizeFsgame(layout.flavor) : layout.fsgameContent;
    const std::string gameRootPath = source.AbsolutePath(layout.gameRoot);
    direct = !gameRootPath.empty();

    std::string fsgame;
    if (direct)
    {
        // Ordinary paths: the locator scans the real installation through a
        // lower-case symlink, which keeps the private root case-stable.
        const std::string link = privateRoot + "/gameroot";
        if (symlink(gameRootPath.c_str(), link.c_str()) != 0)
        {
            __android_log_print(ANDROID_LOG_ERROR, LogTag, "Cannot link %s: %s",
                gameRootPath.c_str(), std::strerror(errno));
            return false;
        }
        fsgame = RewriteFsgame(base, "$game_root$");
        for (const GameDataFile& file : files)
        {
            if (file.directory || PathRelativeToRoot(file.path, layout.gameRoot).empty())
                continue;
            const std::string baseLower = LowerName(BaseName(file.path));
            if (IsArchiveName(baseLower))
                ++archives;
            else
                ++looseFiles;
        }
    }
    else
    {
        // SAF: register every file of the installation under the private root
        // as a virtual file. The locator scans and opens them like real files;
        // each open asks the provider for a fresh descriptor.
        xrAddAndroidVirtualRoot(privateRoot.c_str());
        xrSetAndroidVirtualFileOpener(&OpenSafVirtualFile);
        fsgame = RewriteFsgame(base, nullptr);
        for (const GameDataFile& file : files)
        {
            if (file.directory)
                continue;
            const std::string relative = PathRelativeToRoot(file.path, layout.gameRoot);
            if (relative.empty())
                continue;
            const std::string relativeLower = LowerName(relative);
            if (relativeLower == "fsgame.ltx")
                continue; // replaced by the generated one

            const std::string baseLower = LowerName(BaseName(relative));
            std::string mounted = relativeLower;
            const bool suffixed = IsSuffixedArchiveName(baseLower);
            const bool archive = suffixed || IsArchiveName(baseLower);
            if (archive)
            {
                // Archives need random access; reject pipes and corrupt
                // (e.g. ransomware-encrypted) copies before the locator asserts.
                const int descriptor = source.Open(file.path);
                const bool valid = descriptor >= 0 && ValidateArchiveDescriptor(descriptor, suffixed);
                if (descriptor >= 0)
                    close(descriptor);
                if (!valid)
                {
                    __android_log_print(ANDROID_LOG_WARN, LogTag,
                        "Skipping archive that is not seekable or has no CFS header: %s", file.path.c_str());
                    continue;
                }
                if (suffixed)
                    mounted.erase(mounted.size() - 4); // drop ".ntp"
            }

            if (xrRegisterAndroidVirtualFile((privateRoot + "/" + mounted).c_str(), file.path.c_str(),
                    file.size, file.modified))
            {
                if (archive)
                    ++archives;
                else
                    ++looseFiles;
            }
        }
    }

    fsgamePath = privateRoot + "/fsgame.ltx";
    if (!WriteTextFile(fsgamePath, fsgame))
        return false;
    mounted = true;
    __android_log_print(ANDROID_LOG_INFO, LogTag,
        "Mounted installation via %s: %zu archives, %zu loose files", source.Name(), archives, looseFiles);
    return true;
}

GameFlavor RefineGameFlavorFromVfs(GameFlavor detected)
{
    if (!FS.path_exist("$game_levels$"))
        return detected;

    const auto hasLevel = [](pcstr name) {
        string_path path;
        strconcat(path, name, "\\level.ltx");
        return static_cast<bool>(FS.exist("$game_levels$", path));
    };

    // Mods such as Call of Chernobyl ship CS and SoC locations on the CoP
    // engine, so the CoP locations take precedence.
    if (hasLevel("zaton") || hasLevel("jupiter") || hasLevel("pripyat"))
        return GameFlavor::CallOfPripyat;
    if (hasLevel("marsh") || hasLevel("red_forest") || hasLevel("limansk"))
        return GameFlavor::ClearSky;
    if (hasLevel("l01_escape") || hasLevel("l02_garbage"))
        return GameFlavor::ShadowOfChernobyl;
    return detected;
}

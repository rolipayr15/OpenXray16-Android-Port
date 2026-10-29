#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Game-data discovery for the Android host. The user selects any directory;
// the host finds the X-Ray installation inside it, reuses that game's own
// fsgame.ltx and exposes the installation to the engine locator, either
// through ordinary paths (all-files access) or through the read-only Storage
// Access Framework bridge. Nothing here is specific to one game: SoC, CS, CoP
// and mods built on them go through the same path.

enum class GameFlavor
{
    Unknown, // let the engine decide (openxray.ltx, CoP by default)
    ShadowOfChernobyl,
    ClearSky,
    CallOfPripyat,
    Unlocked, // -unlock_game_mode for mods that mix resources
};

// "auto", "shoc", "cs", "cop" or "unlock"; anything else means auto.
GameFlavor ParseGameMode(const char* mode);
// Engine command-line flag for the flavor; empty for Unknown.
const char* GameFlavorFlag(GameFlavor flavor);
const char* GameFlavorName(GameFlavor flavor);

struct GameDataFile
{
    std::string path; // relative to the selected root, '/'-separated
    std::int64_t size = -1;
    std::int64_t modified = 0;
    bool directory = false;
};

class GameDataSource
{
public:
    virtual ~GameDataSource() = default;

    virtual const char* Name() const = 0;
    virtual bool Walk(std::vector<GameDataFile>& files) = 0;
    // Returns a read-only descriptor owned by the caller, or -1.
    virtual int Open(const std::string& relativePath) = 0;
    // Absolute filesystem path for direct sources; empty for SAF.
    virtual std::string AbsolutePath(const std::string& relativePath) const = 0;
};

std::unique_ptr<GameDataSource> CreateDirectGameDataSource(const std::string& root);
std::unique_ptr<GameDataSource> CreateSafGameDataSource();

struct GameLayout
{
    GameFlavor flavor = GameFlavor::Unknown;
    bool flavorForced = false;      // chosen by the user, never refined
    bool detected = false;          // an installation was recognized
    std::string gameRoot;           // relative to the selected root
    std::string fsgameRelativePath; // the game's own fsgame.ltx, if any
    std::string fsgameContent;      // its text (empty when synthesized)
    size_t archiveCount = 0;
    size_t looseFileCount = 0;
};

bool DetectGameLayout(GameDataSource& source, const std::vector<GameDataFile>& files,
    GameFlavor requested, GameLayout& layout);

// Prepares the private engine root (writable state only) and exposes the
// installation to the locator. Keep the object alive for the whole engine run.
class GameDataMount
{
public:
    GameDataMount() = default;
    GameDataMount(const GameDataMount&) = delete;
    GameDataMount& operator=(const GameDataMount&) = delete;
    ~GameDataMount();

    // Writes <privateRoot>/fsgame.ltx and returns its path in fsgamePath.
    bool Mount(GameDataSource& source, const std::vector<GameDataFile>& files, const GameLayout& layout,
        const std::string& privateRoot, std::string& fsgamePath);

    size_t ArchiveCount() const { return archives; }
    size_t LooseFileCount() const { return looseFiles; }
    bool Direct() const { return direct; }

private:
    size_t archives = 0;
    size_t looseFiles = 0;
    bool direct = false;
    bool mounted = false;
};

// After xrCore mounted the data, tells CS from CoP (and catches mods built on
// either) by the levels the installation actually ships.
GameFlavor RefineGameFlavorFromVfs(GameFlavor detected);

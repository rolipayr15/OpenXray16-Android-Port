#include "AndroidGameModuleProbe.hpp"

#include "xrGame/game_base.h"

extern "C" bool OpenXRayAndroidProbeGameModule(
    std::uint32_t apiVersion, std::uint32_t* playerStateSize)
{
    constexpr std::uint32_t ProbeApiVersion = 1;
    if (apiVersion != ProbeApiVersion || !playerStateSize)
        return false;

    *playerStateSize = static_cast<std::uint32_t>(sizeof(game_PlayerState));
    return GAME_PHASE_NONE == 0 && GAME_PHASE_INPROGRESS == 1 &&
        GAME_PLAYER_FLAG_LOCAL == (1u << 0);
}


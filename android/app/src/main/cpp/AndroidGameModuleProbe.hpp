#pragma once

#include <cstdint>

// Implemented inside the xrGame static target. Calling this from libmain.so
// proves that the Android package is linked to the game module rather than
// merely building an otherwise-unused archive as a side effect.
extern "C" bool OpenXRayAndroidProbeGameModule(
    std::uint32_t apiVersion, std::uint32_t* playerStateSize);


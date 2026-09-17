#pragma once

// Returns zero when the platform, CRC, path normalization, LZO round-trip and
// xrMiscMath checks pass. This is intentionally independent of game data.
extern "C" int xrCoreAndroidSmokeTest();

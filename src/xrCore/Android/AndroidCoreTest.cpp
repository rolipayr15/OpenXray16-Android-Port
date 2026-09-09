#include "stdafx.h"

#include "AndroidCoreTest.hpp"

#include <array>
#include <cstring>

extern "C" int xrCoreAndroidSmokeTest()
{
    static constexpr char CrcPayload[] = "123456789";
    if (crc32(CrcPayload, sizeof(CrcPayload) - 1) != 0xcbf43926)
        return 1;

    static constexpr char ForwardPath[] = "gamedata/configs/system.ltx";
    static constexpr char BackwardPath[] = "gamedata\\configs\\system.ltx";
    if (path_crc32(ForwardPath, sizeof(ForwardPath) - 1) != path_crc32(BackwardPath, sizeof(BackwardPath) - 1))
        return 2;

    std::array<u8, 1024> input{};
    for (size_t index = 0; index < input.size(); ++index)
        input[index] = static_cast<u8>((index * 17 + index / 7) & 0xff);

    std::vector<u8> compressed(rtc_csize(input.size()));
    std::array<u8, 1024> restored{};
    rtc_initialize();
    const size_t compressedSize = rtc_compress(compressed.data(), compressed.size(), input.data(), input.size());
    if (compressedSize == 0 || compressedSize > compressed.size())
        return 3;

    const size_t restoredSize =
        rtc_decompress(restored.data(), restored.size(), compressed.data(), compressedSize);
    if (restoredSize != input.size())
        return 4;
    if (std::memcmp(input.data(), restored.data(), input.size()) != 0)
        return 5;

    Fmatrix identity;
    identity.identity();
    if (identity._11 != 1.f || identity._22 != 1.f || identity._33 != 1.f || identity._44 != 1.f)
        return 6;

    return 0;
}

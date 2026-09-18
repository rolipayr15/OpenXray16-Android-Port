#pragma once

#include <cstdint>

enum class AndroidTouchAction : std::uint8_t
{
    Fire,
    Jump,
    Use,
    Crouch,
    Reload,
    Inventory,
    Pda,
    Flashlight,
    Slot1,
    Slot2,
    Slot3,
    Slot4,
    Slot5,
    Slot6,
    Pause,
    Count
};

struct AndroidTouchButtonSnapshot
{
    float x{};
    float y{};
    float radius{};
    AndroidTouchAction action{};
    bool pressed{};
};

struct AndroidTouchControlsSnapshot
{
    bool visible{};
    float stickX{};
    float stickY{};
    float stickRadius{};
    float knobX{};
    float knobY{};
    AndroidTouchButtonSnapshot buttons[static_cast<std::uint8_t>(AndroidTouchAction::Count)]{};
};

void AndroidTouchControlsSetGameplayActive(bool active);
bool AndroidTouchControlsGetSnapshot(AndroidTouchControlsSnapshot& snapshot);

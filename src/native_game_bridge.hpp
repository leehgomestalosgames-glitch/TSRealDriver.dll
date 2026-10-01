#pragma once

#include <cstdint>
#include <string>

struct NativeGameBridgeStatus {
    bool initialized = false;
    bool supportedGame = false;
    bool textSectionFound = false;
    bool d3d11Loaded = false;
    std::wstring executableName;
    std::uintptr_t moduleBase = 0;
    std::uintptr_t textBase = 0;
    std::size_t textSize = 0;
    std::uint64_t textFingerprint = 0;
};

bool native_game_bridge_initialize();
void native_game_bridge_shutdown();
const NativeGameBridgeStatus& native_game_bridge_status();
std::wstring native_game_bridge_report();

// Generic scanner used by TSRealDriver's own compatibility research.
// Pattern bytes use -1 as a wildcard.
std::uintptr_t native_game_find_pattern(const int* pattern, std::size_t length);

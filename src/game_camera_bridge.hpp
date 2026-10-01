#pragma once

#include <cstdint>
#include <string>

struct GameCameraBridgeStatus {
    bool resolved = false;
    std::uintptr_t managerSingletonAddress = 0;
    std::uintptr_t managerObject = 0;
    std::uintptr_t debugCameraObject = 0;
    int debugCameraSlot = -1;
    int currentCameraSlot = -1;
    std::string debugCameraClass;
    std::wstring error;
};

bool game_camera_bridge_resolve();
bool game_camera_bridge_refresh();
bool game_camera_bridge_request_debug(unsigned timeoutMs = 1200);
bool game_camera_bridge_request_slot(int slot, unsigned timeoutMs = 1200);
bool game_camera_bridge_set_debug_position(double worldX, double worldY, double worldZ);
int game_camera_bridge_current_slot();
const GameCameraBridgeStatus& game_camera_bridge_status();
std::wstring game_camera_bridge_report();
std::wstring game_camera_bridge_census();

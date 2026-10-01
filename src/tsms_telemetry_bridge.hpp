#pragma once

#include <string>

struct TsmsGameState {
    bool available = false;
    bool sdkActive = false;
    bool paused = false;
    unsigned game = 0; // 1 ETS2, 2 ATS

    double worldX = 0.0;
    double worldY = 0.0;
    double worldZ = 0.0;
    double heading = 0.0; // SCS normalized heading [0,1)
    double pitch = 0.0;
    double roll = 0.0;

    float speedMps = 0.0f;
    float fuel = 0.0f;
    float fuelCapacity = 0.0f;
    float adblue = 0.0f;
    float adblueCapacity = 0.0f;

    bool parkingBrake = false;
    bool engineEnabled = false;
    bool electricEnabled = false;
    bool refuel = false;

    float cabinLocalX = 0.0f;
    float cabinLocalY = 0.0f;
    float cabinLocalZ = 0.0f;
    float headLocalX = 0.0f;
    float headLocalY = 0.0f;
    float headLocalZ = 0.0f;
    float hookLocalX = 0.0f;
    float hookLocalY = 0.0f;
    float hookLocalZ = 0.0f;

    std::string truckId;
    std::string truckName;

    bool trailerAttached = false;
    double trailerX = 0.0;
    double trailerY = 0.0;
    double trailerZ = 0.0;
    double trailerHeading = 0.0;
};

bool tsms_bridge_open();
void tsms_bridge_close();
bool tsms_bridge_read(TsmsGameState& out);

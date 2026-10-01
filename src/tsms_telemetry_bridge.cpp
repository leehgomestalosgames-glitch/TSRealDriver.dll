#include "tsms_telemetry_bridge.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace {

constexpr wchar_t MapName[] = L"Local\\SCSTelemetry";
constexpr std::size_t MapSize = 32 * 1024;
constexpr std::size_t TrailerBase = 6000;

HANDLE g_mapping = nullptr;
const unsigned char* g_view = nullptr;

template <typename T>
T read_value(std::size_t offset) {
    T value{};
    if (!g_view || offset + sizeof(T) > MapSize) return value;
    std::memcpy(&value, g_view + offset, sizeof(T));
    return value;
}

bool read_bool(std::size_t offset) {
    return read_value<std::uint8_t>(offset) != 0;
}

std::string read_string(std::size_t offset, std::size_t length) {
    if (!g_view || offset >= MapSize) return {};
    const std::size_t capped = std::min(length, MapSize - offset);
    std::size_t actual = 0;
    while (actual < capped && g_view[offset + actual] != 0) ++actual;
    return std::string(reinterpret_cast<const char*>(g_view + offset), actual);
}

}

bool tsms_bridge_open() {
    if (g_view) return true;

    g_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, MapName);
    if (!g_mapping) return false;

    g_view = static_cast<const unsigned char*>(
        MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, MapSize));

    if (!g_view) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return false;
    }

    return true;
}

void tsms_bridge_close() {
    if (g_view) {
        UnmapViewOfFile(g_view);
        g_view = nullptr;
    }
    if (g_mapping) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
}

bool tsms_bridge_read(TsmsGameState& out) {
    out = {};

    if (!g_view && !tsms_bridge_open()) return false;

    out.available = true;
    out.sdkActive = read_bool(0);
    out.paused = read_bool(4);
    out.game = read_value<std::uint32_t>(52);

    // Float zone from the public rev12 Local\\SCSTelemetry layout.
    out.fuelCapacity = read_value<float>(700);
    out.adblueCapacity = read_value<float>(708);
    out.speedMps = read_value<float>(948);
    out.fuel = read_value<float>(1000);
    out.adblue = read_value<float>(1012);

    // Bool zone. config_b ends at 1565, truck_b starts at 1566.
    out.parkingBrake = read_bool(1566);
    out.electricEnabled = read_bool(1575);
    out.engineEnabled = read_bool(1576);
    out.refuel = read_bool(4308);

    // Config local positions.
    out.cabinLocalX = read_value<float>(1640);
    out.cabinLocalY = read_value<float>(1644);
    out.cabinLocalZ = read_value<float>(1648);
    out.headLocalX = read_value<float>(1652);
    out.headLocalY = read_value<float>(1656);
    out.headLocalZ = read_value<float>(1660);
    out.hookLocalX = read_value<float>(1664);
    out.hookLocalY = read_value<float>(1668);
    out.hookLocalZ = read_value<float>(1672);

    // Truck world placement.
    out.worldX = read_value<double>(2200);
    out.worldY = read_value<double>(2208);
    out.worldZ = read_value<double>(2216);
    out.heading = read_value<double>(2224);
    out.pitch = read_value<double>(2232);
    out.roll = read_value<double>(2240);

    // Strings from config_s.
    out.truckId = read_string(2428, 64);
    out.truckName = read_string(2492, 64);

    // First trailer.
    out.trailerAttached = read_bool(TrailerBase + 80);
    out.trailerX = read_value<double>(TrailerBase + 872);
    out.trailerY = read_value<double>(TrailerBase + 880);
    out.trailerZ = read_value<double>(TrailerBase + 888);
    out.trailerHeading = read_value<double>(TrailerBase + 896);

    return true;
}

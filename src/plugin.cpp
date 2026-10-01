#include <windows.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <mmsystem.h>

#include "native_game_bridge.hpp"
#include "game_camera_bridge.hpp"
#include "tsms_telemetry_bridge.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

HMODULE g_module = nullptr;
std::atomic_bool g_stop{false};
std::thread g_worker;

struct Settings {
    int toggleKey = VK_F10;
    int interactKey = 'F';
    int forwardKey = 'W';
    int backwardKey = 'S';
    int leftKey = 'A';
    int rightKey = 'D';
    int sprintKey = VK_SHIFT;
    int crouchKey = VK_CONTROL;
    int jumpKey = VK_SPACE;
    int eyeDownKey = 'Q';
    int eyeUpKey = 'E';
    int eyeResetKey = 'R';
    int buildingKey = VK_F8;
    int flashlightKey = VK_RBUTTON;
    int flashlightSizeKey = 'G';
    int speedResetKey = VK_MBUTTON;
    int zoomKey = VK_LBUTTON;
    int fuelModeKey = VK_F7;
    int consoleKey = VK_OEM_3;

    double walkSpeed = 1.75;
    double sprintMultiplier = 2.60;
    double backwardFactor = 0.70;
    double strafeFactor = 0.90;
    double crouchDropSeconds = 0.16;
    double jumpUpSeconds = 0.12;
    double jumpHangSeconds = 0.12;
    double jumpDownSeconds = 0.16;

    bool prompts = true;
    bool flashlightEnabled = true;
    bool fuelEnabled = true;
    bool debugCameraBridge = true;
    bool autoDoorOffset = true;
    bool blockGameKeys = true;
    bool consoleSpeedControl = true;
    double spawnFlySpeed = 5.0;
    double walkFlySpeed = 1.75;
    double restoreFlySpeed = 100.0;
    int sprintWheelNotches = 3;

    bool headBob = true;
    bool breathingMotion = true;
    bool fadeEnabled = true;
    bool soundEnabled = true;
    bool footstepSounds = false;
    bool breathingSound = true;
    bool shadowEnabled = true;

    double bobAmount = 0.004;
    double swayAmount = 0.003;
    double stepLength = 0.93;
    double runStride = 0.75;
    double runHeadDip = 0.60;
    double runSway = 0.30;
    double landingDip = 0.035;
    double tiredAfterSeconds = 18.0;
    double masterVolume = 0.70;

    double exitSpeedThreshold = 0.30;
    double doorOutward = 2.0;
    double interactRange = 1.2;
    double mouseLookScale = 0.0025;

    bool fuelCardStep = true;
    bool rememberTank = true;
    double pumpOut = 4.5;
    double pumpBack = 2.5;
    double pumpRadius = 4.0;
    double tankOut = 1.4;
    double tankBack = 1.5;
    double tankRadius = 1.3;

    bool trailerEnabled = true;
    bool trailerSteps = true;
    bool trailerCoupleSteps = true;
    double fifthWheelRadius = 2.5;
    int trailerAttachKey = 'T';
};

struct UiState {
    bool walking = false;
    bool paused = false;
    bool flashlight = false;
    int flashlightSize = 1;
    int fuelStage = 0;
    bool fueling = false;
    bool buildingMode = false;
    int trailerStage = 0;
    std::wstring status = L"READY";
    std::wstring context = L"";
};

Settings g_settings;
std::mutex g_stateMutex;
UiState g_ui;

std::filesystem::path g_moduleDir;
std::filesystem::path g_iniPath;
std::filesystem::path g_logPath;
std::filesystem::path g_configExePath;
std::filesystem::path g_audioDir;
std::filesystem::path g_tanksPath;

HWND g_promptWindow = nullptr;
HWND g_flashlightWindow = nullptr;
HWND g_fadeWindow = nullptr;
FILETIME g_lastIniWrite{};

TsmsGameState g_gameState{};
bool g_haveTelemetry = false;

double g_walkerX = 0.0;
double g_walkerY = 0.0;
double g_walkerZ = 0.0;
double g_walkerYaw = 0.0;
bool g_walkerPositionValid = false;
int g_previousCameraSlot = -1;

HHOOK g_mouseHook = nullptr;
HHOOK g_keyboardHook = nullptr;
std::array<std::atomic_bool, 256> g_hookKeyState{};
POINT g_lastMousePoint{};
bool g_haveMousePoint = false;
std::atomic<long> g_mouseDx{0};
std::atomic<long> g_mouseDy{0};
std::atomic<int> g_mouseWheel{0};


bool key_down(int vk) {
    if (vk <= 0) return false;
    if (vk < static_cast<int>(g_hookKeyState.size()) && g_hookKeyState[static_cast<std::size_t>(vk)].load()) {
        return true;
    }
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

int parse_ini_int(const wchar_t* section, const wchar_t* key, int fallback) {
    wchar_t buffer[64]{};
    wchar_t fallbackText[64]{};
    _snwprintf_s(fallbackText, _countof(fallbackText), _TRUNCATE, L"%d", fallback);
    GetPrivateProfileStringW(section, key, fallbackText, buffer, _countof(buffer), g_iniPath.c_str());

    wchar_t* end = nullptr;
    const long value = wcstol(buffer, &end, 0);
    return (end && end != buffer) ? static_cast<int>(value) : fallback;
}

double parse_ini_double(const wchar_t* section, const wchar_t* key, double fallback) {
    wchar_t buffer[64]{};
    wchar_t fallbackText[64]{};
    _snwprintf_s(fallbackText, _countof(fallbackText), _TRUNCATE, L"%.4f", fallback);
    GetPrivateProfileStringW(section, key, fallbackText, buffer, _countof(buffer), g_iniPath.c_str());

    wchar_t* end = nullptr;
    const double value = wcstod(buffer, &end);
    return (end && end != buffer) ? value : fallback;
}

bool parse_ini_bool(const wchar_t* section, const wchar_t* key, bool fallback) {
    return parse_ini_int(section, key, fallback ? 1 : 0) != 0;
}

void log_line(const std::wstring& text) {
    if (g_logPath.empty()) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);

    std::wofstream out(g_logPath, std::ios::app);
    if (!out) return;

    out << L"["
        << st.wYear << L"-"
        << (st.wMonth < 10 ? L"0" : L"") << st.wMonth << L"-"
        << (st.wDay < 10 ? L"0" : L"") << st.wDay << L" "
        << (st.wHour < 10 ? L"0" : L"") << st.wHour << L":"
        << (st.wMinute < 10 ? L"0" : L"") << st.wMinute << L":"
        << (st.wSecond < 10 ? L"0" : L"") << st.wSecond
        << L"] " << text << L"\n";
}

std::filesystem::path module_directory() {
    wchar_t buffer[32768]{};
    const DWORD size = GetModuleFileNameW(g_module, buffer, static_cast<DWORD>(_countof(buffer)));
    if (size == 0 || size >= _countof(buffer)) return {};
    return std::filesystem::path(std::wstring(buffer, size)).parent_path();
}

void set_ui_status(const std::wstring& status) {
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_ui.status = status;
    }
    if (g_promptWindow) InvalidateRect(g_promptWindow, nullptr, TRUE);
}

bool query_write_time(const std::filesystem::path& path, FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    out = data.ftLastWriteTime;
    return true;
}

void load_settings() {
    if (g_iniPath.empty()) return;

    g_settings.toggleKey = parse_ini_int(L"keys", L"toggle", VK_F10);
    g_settings.interactKey = parse_ini_int(L"keys", L"interact", 'F');
    g_settings.forwardKey = parse_ini_int(L"keys", L"forward", 'W');
    g_settings.backwardKey = parse_ini_int(L"keys", L"back", 'S');
    g_settings.leftKey = parse_ini_int(L"keys", L"left", 'A');
    g_settings.rightKey = parse_ini_int(L"keys", L"right", 'D');
    g_settings.sprintKey = parse_ini_int(L"keys", L"sprint", VK_SHIFT);
    g_settings.crouchKey = parse_ini_int(L"keys", L"crouch", VK_CONTROL);
    g_settings.jumpKey = parse_ini_int(L"keys", L"jump", VK_SPACE);
    g_settings.eyeDownKey = parse_ini_int(L"keys", L"eye_down", 'Q');
    g_settings.eyeUpKey = parse_ini_int(L"keys", L"eye_up", 'E');
    g_settings.eyeResetKey = parse_ini_int(L"keys", L"eye_reset", 'R');
    g_settings.buildingKey = parse_ini_int(L"keys", L"building", VK_F8);
    g_settings.flashlightKey = parse_ini_int(L"keys", L"flashlight", VK_RBUTTON);
    g_settings.flashlightSizeKey = parse_ini_int(L"keys", L"flashlight_size", 'G');
    g_settings.speedResetKey = parse_ini_int(L"keys", L"speed_reset", VK_MBUTTON);
    g_settings.zoomKey = parse_ini_int(L"keys", L"zoom", VK_LBUTTON);
    g_settings.fuelModeKey = parse_ini_int(L"keys", L"fuel_mode", VK_F7);
    g_settings.consoleKey = parse_ini_int(L"keys", L"console", VK_OEM_3);

    g_settings.walkSpeed = parse_ini_double(L"movement", L"walk_speed", 1.75);
    g_settings.sprintMultiplier = parse_ini_double(L"movement", L"sprint_multiplier", 2.60);
    g_settings.backwardFactor = parse_ini_double(L"movement", L"backward_factor", 0.70);
    g_settings.strafeFactor = parse_ini_double(L"movement", L"strafe_factor", 0.90);
    g_settings.crouchDropSeconds = parse_ini_double(L"movement", L"crouch_drop_seconds", 0.16);
    g_settings.jumpUpSeconds = parse_ini_double(L"movement", L"jump_up_seconds", 0.12);
    g_settings.jumpHangSeconds = parse_ini_double(L"movement", L"jump_hang_seconds", 0.12);
    g_settings.jumpDownSeconds = parse_ini_double(L"movement", L"jump_down_seconds", 0.16);

    g_settings.prompts = parse_ini_bool(L"movement", L"show_prompts", true);
    g_settings.flashlightEnabled = parse_ini_bool(L"flashlight", L"enabled", true);
    g_settings.fuelEnabled = parse_ini_bool(L"fuel", L"enabled", true);
    g_settings.debugCameraBridge = parse_ini_bool(L"camera", L"debug_camera_bridge", true);
    g_settings.autoDoorOffset = parse_ini_bool(L"camera", L"auto_door_offset", true);
    g_settings.blockGameKeys = parse_ini_bool(L"camera", L"block_game_keys", true);
    g_settings.consoleSpeedControl = parse_ini_bool(L"camera", L"console_speed_control", true);
    g_settings.spawnFlySpeed = parse_ini_double(L"camera", L"spawn_fly_speed", 5.0);
    g_settings.walkFlySpeed = parse_ini_double(L"camera", L"walk_fly_speed", 1.75);
    g_settings.restoreFlySpeed = parse_ini_double(L"camera", L"restore_fly_speed", 100.0);
    g_settings.sprintWheelNotches = parse_ini_int(L"camera", L"sprint_wheel_notches", 3);

    g_settings.headBob = parse_ini_bool(L"movement", L"head_bob", true);
    g_settings.breathingMotion = parse_ini_bool(L"movement", L"breathing_motion", true);
    g_settings.fadeEnabled = parse_ini_bool(L"movement", L"fade_enabled", true);
    g_settings.bobAmount = parse_ini_double(L"movement", L"bob_amount", 0.004);
    g_settings.swayAmount = parse_ini_double(L"movement", L"sway_amount", 0.003);
    g_settings.stepLength = parse_ini_double(L"movement", L"step_length", 0.93);
    g_settings.runStride = parse_ini_double(L"movement", L"run_stride", 0.75);
    g_settings.runHeadDip = parse_ini_double(L"movement", L"run_head_dip", 0.60);
    g_settings.runSway = parse_ini_double(L"movement", L"run_sway", 0.30);
    g_settings.landingDip = parse_ini_double(L"movement", L"landing_dip", 0.035);

    g_settings.soundEnabled = parse_ini_bool(L"sound", L"enabled", true);
    g_settings.footstepSounds = parse_ini_bool(L"sound", L"footsteps", false);
    g_settings.breathingSound = parse_ini_bool(L"sound", L"breathing", true);
    g_settings.tiredAfterSeconds = parse_ini_double(L"sound", L"tired_after_s", 18.0);
    g_settings.masterVolume = parse_ini_double(L"sound", L"master_volume", 0.70);

    g_settings.shadowEnabled = parse_ini_bool(L"shadow", L"enabled", true);

    g_settings.exitSpeedThreshold = parse_ini_double(L"camera", L"cabin_exit_speed_threshold", 0.30);
    g_settings.doorOutward = parse_ini_double(L"camera", L"door_outward", 2.0);
    g_settings.interactRange = parse_ini_double(L"camera", L"enter_range_m", 1.2);
    g_settings.mouseLookScale = parse_ini_double(L"camera", L"mouse_look_scale", 0.0025);

    g_settings.fuelCardStep = parse_ini_bool(L"fuel", L"card_step", true);
    g_settings.rememberTank = parse_ini_bool(L"fuel", L"remember_tank", true);
    g_settings.pumpOut = parse_ini_double(L"fuel", L"pump_out", 4.5);
    g_settings.pumpBack = parse_ini_double(L"fuel", L"pump_back", 2.5);
    g_settings.pumpRadius = parse_ini_double(L"fuel", L"pump_radius_m", 4.0);
    g_settings.tankOut = parse_ini_double(L"fuel", L"tank_out", 1.4);
    g_settings.tankBack = parse_ini_double(L"fuel", L"tank_back", 1.5);
    g_settings.tankRadius = parse_ini_double(L"fuel", L"tank_radius_m", 1.3);

    g_settings.trailerEnabled = parse_ini_bool(L"trailer", L"enabled", true);
    g_settings.trailerSteps = parse_ini_bool(L"trailer", L"steps", true);
    g_settings.trailerCoupleSteps = parse_ini_bool(L"trailer", L"couple_steps", true);
    g_settings.fifthWheelRadius = parse_ini_double(L"trailer", L"radius", 2.5);
    g_settings.trailerAttachKey = parse_ini_int(L"trailer", L"attach_key", 'T');

    query_write_time(g_iniPath, g_lastIniWrite);
    log_line(L"Configuration loaded.");
}

void reload_settings_if_changed() {
    FILETIME current{};
    if (!query_write_time(g_iniPath, current)) return;

    if (CompareFileTime(&current, &g_lastIniWrite) != 0) {
        load_settings();
        g_lastIniWrite = current;
        set_ui_status(L"CONFIG RELOADED");
    }
}

void send_key(int vk, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

// The SCS free-camera controls are read through the game's low-level keyboard
// path. Virtual-key SendInput events can be visible to Windows while still being
// ignored by that path. Emit hardware-style scan codes for controls that must be
// seen by the game (numpad camera movement and Numpad 0).
void send_game_key(int vk, bool down) {
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX);
    if (scan == 0) {
        send_key(vk, down);
        return;
    }

    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 0;
    input.ki.wScan = static_cast<WORD>(scan & 0xff);
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    if ((scan & 0xff00u) == 0xe000u || (scan & 0xff00u) == 0xe100u) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    SendInput(1, &input, sizeof(INPUT));
}

void tap_key(int vk) {
    send_key(vk, true);
    std::this_thread::sleep_for(8ms);
    send_key(vk, false);
}

void tap_game_key(int vk) {
    send_game_key(vk, true);
    std::this_thread::sleep_for(24ms);
    send_game_key(vk, false);
}

void hold_key_for(int vk, std::chrono::milliseconds duration) {
    send_key(vk, true);
    std::this_thread::sleep_for(duration);
    send_key(vk, false);
}

void hold_game_key_for(int vk, std::chrono::milliseconds duration) {
    send_game_key(vk, true);
    std::this_thread::sleep_for(duration);
    send_game_key(vk, false);
}

void send_wheel(int notches) {
    if (notches == 0) return;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(notches * WHEEL_DELTA);
    SendInput(1, &input, sizeof(INPUT));
}


void write_wave_file(const std::filesystem::path& path, const std::vector<short>& samples, int sampleRate = 44100) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;

    const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(short));
    const std::uint32_t riffSize = 36u + dataSize;
    const std::uint16_t audioFormat = 1;
    const std::uint16_t channels = 1;
    const std::uint16_t bits = 16;
    const std::uint32_t byteRate = sampleRate * channels * (bits / 8);
    const std::uint16_t blockAlign = channels * (bits / 8);

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), 4);
    out.write("WAVEfmt ", 8);

    const std::uint32_t fmtSize = 16;
    out.write(reinterpret_cast<const char*>(&fmtSize), 4);
    out.write(reinterpret_cast<const char*>(&audioFormat), 2);
    out.write(reinterpret_cast<const char*>(&channels), 2);
    out.write(reinterpret_cast<const char*>(&sampleRate), 4);
    out.write(reinterpret_cast<const char*>(&byteRate), 4);
    out.write(reinterpret_cast<const char*>(&blockAlign), 2);
    out.write(reinterpret_cast<const char*>(&bits), 2);

    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataSize), 4);
    out.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(dataSize));
}

std::vector<short> synth_footstep(bool running, int variant) {
    constexpr int sampleRate = 44100;
    const double seconds = running ? 0.16 : 0.20;
    const int count = std::max(1, static_cast<int>(seconds * sampleRate));
    std::vector<short> data(static_cast<std::size_t>(count));

    std::mt19937 rng(0x54535244u + static_cast<unsigned>(variant * 97 + (running ? 1000 : 0)));
    std::uniform_real_distribution<double> noise(-1.0, 1.0);

    double low = 0.0;
    double mid = 0.0;

    for (int i = 0; i < count; ++i) {
        const double t = i / static_cast<double>(sampleRate);

        const double heelEnv = std::exp(-t * (running ? 34.0 : 28.0));
        const double toeT = std::max(0.0, t - (running ? 0.045 : 0.060));
        const double toeEnv = toeT > 0.0 ? std::exp(-toeT * 38.0) : 0.0;

        const double n = noise(rng);
        low += 0.045 * (n - low);
        mid += 0.18 * (n - mid);

        const double thumpHz = running ? (82.0 + variant * 3.0) : (68.0 + variant * 2.0);
        const double thump =
            std::sin(6.283185307179586 * thumpHz * t) * heelEnv * (running ? 0.10 : 0.08);

        const double sole =
            (0.72 * low + 0.28 * mid) * heelEnv * (running ? 0.11 : 0.085);

        const double toe =
            (mid - low) * toeEnv * (running ? 0.07 : 0.055);

        const double sample = std::clamp(thump + sole + toe, -0.55, 0.55);
        data[static_cast<std::size_t>(i)] = static_cast<short>(sample * 32767.0);
    }

    return data;
}

std::vector<short> synth_effect(double seconds, double lowHz, double highHz, double noiseMix, double amplitude, int seed) {
    constexpr int sampleRate = 44100;
    const int count = std::max(1, static_cast<int>(seconds * sampleRate));
    std::vector<short> data(static_cast<std::size_t>(count));

    std::mt19937 rng(0x53524400u + static_cast<unsigned>(seed));
    std::uniform_real_distribution<double> noise(-1.0, 1.0);

    double filtered = 0.0;
    for (int i = 0; i < count; ++i) {
        const double t = i / static_cast<double>(sampleRate);
        const double phase = t / seconds;
        const double env = std::pow(std::max(0.0, 1.0 - phase), 2.0);
        const double freq = lowHz + (highHz - lowHz) * phase;

        const double n = noise(rng);
        filtered += 0.12 * (n - filtered);

        const double tone = std::sin(6.283185307179586 * freq * t);
        const double sample =
            (tone * (1.0 - noiseMix) + filtered * noiseMix) * env * amplitude;

        data[static_cast<std::size_t>(i)] =
            static_cast<short>(std::clamp(sample, -0.8, 0.8) * 32767.0);
    }

    return data;
}

std::vector<short> synth_breath() {
    constexpr int sampleRate = 44100;
    constexpr double seconds = 0.70;
    const int count = static_cast<int>(seconds * sampleRate);
    std::vector<short> data(static_cast<std::size_t>(count));

    std::mt19937 rng(0xBEEA7u);
    std::uniform_real_distribution<double> noise(-1.0, 1.0);
    double filtered = 0.0;

    for (int i = 0; i < count; ++i) {
        const double t = i / static_cast<double>(sampleRate);
        const double x = t / seconds;
        const double env = std::sin(3.14159265358979323846 * std::clamp(x, 0.0, 1.0));
        filtered += 0.035 * (noise(rng) - filtered);
        const double sample = filtered * env * 0.20;
        data[static_cast<std::size_t>(i)] = static_cast<short>(sample * 32767.0);
    }

    return data;
}

void ensure_audio_assets() {
    if (g_audioDir.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(g_audioDir, ec);

    // These files are regenerated by this build so older 8 kHz prototype
    // sounds are automatically replaced on the next game start.
    write_wave_file(g_audioDir / L"footstep1.wav", synth_footstep(false, 1));
    write_wave_file(g_audioDir / L"footstep2.wav", synth_footstep(false, 2));
    write_wave_file(g_audioDir / L"runstep1.wav", synth_footstep(true, 1));
    write_wave_file(g_audioDir / L"runstep2.wav", synth_footstep(true, 2));

    write_wave_file(g_audioDir / L"door.wav", synth_effect(0.32, 72.0, 48.0, 0.70, 0.34, 11));
    write_wave_file(g_audioDir / L"flashlight.wav", synth_effect(0.045, 1800.0, 900.0, 0.10, 0.28, 12));
    write_wave_file(g_audioDir / L"card.wav", synth_effect(0.10, 820.0, 1180.0, 0.05, 0.22, 13));
    write_wave_file(g_audioDir / L"nozzle.wav", synth_effect(0.12, 310.0, 160.0, 0.45, 0.28, 14));
    write_wave_file(g_audioDir / L"receipt.wav", synth_effect(0.24, 520.0, 340.0, 0.58, 0.18, 15));
    write_wave_file(g_audioDir / L"breath.wav", synth_breath());
}

void play_audio(const wchar_t* fileName) {
    if (!g_settings.soundEnabled || g_audioDir.empty()) return;
    const auto path = g_audioDir / fileName;
    if (!std::filesystem::exists(path)) return;
    PlaySoundW(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}


void type_text(const std::wstring& text) {
    for (const wchar_t ch : text) {
        const SHORT code = VkKeyScanW(ch);
        if (code == -1) continue;

        const int vk = LOBYTE(code);
        const int modifiers = HIBYTE(code);

        if (modifiers & 1) send_key(VK_SHIFT, true);
        if (modifiers & 2) send_key(VK_CONTROL, true);
        if (modifiers & 4) send_key(VK_MENU, true);

        tap_key(vk);

        if (modifiers & 4) send_key(VK_MENU, false);
        if (modifiers & 2) send_key(VK_CONTROL, false);
        if (modifiers & 1) send_key(VK_SHIFT, false);

        std::this_thread::sleep_for(2ms);
    }
}

void set_game_flyspeed(double speed) {
    if (!g_settings.consoleSpeedControl || g_settings.consoleKey <= 0) return;

    wchar_t command[64]{};
    _snwprintf_s(command, _countof(command), _TRUNCATE, L"g_flyspeed %.2f", speed);

    // Run while the screen is faded so the console is not visible to the player.
    tap_key(g_settings.consoleKey);
    std::this_thread::sleep_for(80ms);
    type_text(command);
    tap_key(VK_RETURN);
    std::this_thread::sleep_for(80ms);
    tap_key(g_settings.consoleKey);
    std::this_thread::sleep_for(80ms);

    log_line(std::wstring(L"Developer camera speed requested: ") + command);
}

void mouse_nudge(LONG dx, LONG dy) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}



bool should_block_game_key(DWORD vk) {
    if (!g_settings.blockGameKeys) return false;

    bool walking = false;
    bool paused = false;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        walking = g_ui.walking;
        paused = g_ui.paused;
    }
    if (!walking || paused) return false;

    return vk == static_cast<DWORD>(g_settings.forwardKey) ||
           vk == static_cast<DWORD>(g_settings.backwardKey) ||
           vk == static_cast<DWORD>(g_settings.leftKey) ||
           vk == static_cast<DWORD>(g_settings.rightKey) ||
           vk == static_cast<DWORD>(g_settings.jumpKey) ||
           vk == static_cast<DWORD>(g_settings.eyeDownKey) ||
           vk == static_cast<DWORD>(g_settings.eyeUpKey) ||
           vk == static_cast<DWORD>(g_settings.eyeResetKey);
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (info && (info->flags & LLKHF_INJECTED) == 0) {
            const DWORD vk = info->vkCode;
            const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;

            if (vk < g_hookKeyState.size() && (down || up)) {
                g_hookKeyState[vk].store(down);
            }

            if (should_block_game_key(vk)) {
                return 1;
            }
        }
    }

    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

void install_keyboard_hook() {
    if (g_keyboardHook) return;
    for (auto& key : g_hookKeyState) key.store(false);
    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, g_module, 0);
    log_line(g_keyboardHook ? L"Walking keyboard isolation online." : L"Walking keyboard isolation unavailable.");
}

void uninstall_keyboard_hook() {
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    for (auto& key : g_hookKeyState) key.store(false);
}

LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (info) {
            if ((info->flags & LLMHF_INJECTED) != 0) {
                return CallNextHookEx(g_mouseHook, code, wParam, lParam);
            }

            if (!g_haveMousePoint) {
                g_lastMousePoint = info->pt;
                g_haveMousePoint = true;
            } else if (wParam == WM_MOUSEMOVE) {
                g_mouseDx.fetch_add(info->pt.x - g_lastMousePoint.x);
                g_mouseDy.fetch_add(info->pt.y - g_lastMousePoint.y);
                g_lastMousePoint = info->pt;
            }

            if (wParam == WM_MOUSEWHEEL) {
                const short delta = static_cast<short>(HIWORD(info->mouseData));
                if (delta != 0) g_mouseWheel.fetch_add(delta / WHEEL_DELTA);
            }
        }
    }
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
}

void install_mouse_hook() {
    if (g_mouseHook) return;
    g_haveMousePoint = false;
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, g_module, 0);
    log_line(g_mouseHook ? L"Low-level mouse bridge online." : L"Low-level mouse bridge unavailable.");
}

void uninstall_mouse_hook() {
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
    g_haveMousePoint = false;
}

std::wstring widen_ascii(const std::string& input) {
    if (input.empty()) return L"unknown";
    int needed = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (needed <= 0) return L"unknown";
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), out.data(), needed);

    for (auto& ch : out) {
        if (!(std::iswalnum(ch) || ch == L'_' || ch == L'-')) ch = L'_';
    }
    return out;
}

struct WorldPoint {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

double game_heading_radians() {
    return g_gameState.heading * 6.28318530717958647692;
}

WorldPoint truck_local_to_world(double localX, double localY, double localZ) {
    const double a = game_heading_radians();
    const double ca = std::cos(a);
    const double sa = std::sin(a);

    // Horizontal transform. SCS world heading is normalized turns.
    WorldPoint p;
    p.x = g_gameState.worldX + localX * ca + localZ * sa;
    p.y = g_gameState.worldY + localY;
    p.z = g_gameState.worldZ - localX * sa + localZ * ca;
    return p;
}

void world_to_truck_local(double worldX, double worldZ, double& localX, double& localZ) {
    const double dx = worldX - g_gameState.worldX;
    const double dz = worldZ - g_gameState.worldZ;
    const double a = game_heading_radians();
    const double ca = std::cos(a);
    const double sa = std::sin(a);

    localX = dx * ca - dz * sa;
    localZ = dx * sa + dz * ca;
}

double distance_xz(double ax, double az, double bx, double bz) {
    const double dx = ax - bx;
    const double dz = az - bz;
    return std::sqrt(dx * dx + dz * dz);
}

WorldPoint door_point() {
    // Driver side default: negative local X from the cab/head reference.
    const double seatX = static_cast<double>(g_gameState.cabinLocalX + g_gameState.headLocalX);
    const double seatY = static_cast<double>(g_gameState.cabinLocalY + g_gameState.headLocalY);
    const double seatZ = static_cast<double>(g_gameState.cabinLocalZ + g_gameState.headLocalZ);
    return truck_local_to_world(seatX - g_settings.doorOutward, seatY, seatZ);
}

WorldPoint pump_point() {
    return truck_local_to_world(-g_settings.pumpOut, 0.0, -g_settings.pumpBack);
}

bool load_learned_tank(double& localX, double& localZ) {
    if (!g_settings.rememberTank || g_tanksPath.empty() || g_gameState.truckId.empty()) return false;

    const std::wstring section = widen_ascii(g_gameState.truckId);
    wchar_t bx[64]{};
    wchar_t bz[64]{};

    GetPrivateProfileStringW(section.c_str(), L"local_x", L"", bx, _countof(bx), g_tanksPath.c_str());
    GetPrivateProfileStringW(section.c_str(), L"local_z", L"", bz, _countof(bz), g_tanksPath.c_str());

    if (bx[0] == 0 || bz[0] == 0) return false;

    wchar_t* endX = nullptr;
    wchar_t* endZ = nullptr;
    const double x = wcstod(bx, &endX);
    const double z = wcstod(bz, &endZ);
    if (!endX || endX == bx || !endZ || endZ == bz) return false;

    localX = x;
    localZ = z;
    return true;
}

void save_learned_tank(double localX, double localZ) {
    if (!g_settings.rememberTank || g_tanksPath.empty() || g_gameState.truckId.empty()) return;

    const std::wstring section = widen_ascii(g_gameState.truckId);
    wchar_t bx[64]{};
    wchar_t bz[64]{};
    _snwprintf_s(bx, _countof(bx), _TRUNCATE, L"%.4f", localX);
    _snwprintf_s(bz, _countof(bz), _TRUNCATE, L"%.4f", localZ);

    WritePrivateProfileStringW(section.c_str(), L"local_x", bx, g_tanksPath.c_str());
    WritePrivateProfileStringW(section.c_str(), L"local_z", bz, g_tanksPath.c_str());
}

WorldPoint tank_point() {
    double localX = -g_settings.tankOut;
    double localZ = -g_settings.tankBack;
    load_learned_tank(localX, localZ);
    return truck_local_to_world(localX, 0.0, localZ);
}

WorldPoint fifth_wheel_point() {
    return truck_local_to_world(
        static_cast<double>(g_gameState.hookLocalX),
        static_cast<double>(g_gameState.hookLocalY),
        static_cast<double>(g_gameState.hookLocalZ));
}

bool refresh_game_state() {
    TsmsGameState next{};
    const bool ok = tsms_bridge_read(next);
    if (ok) {
        g_gameState = next;
        g_haveTelemetry = next.sdkActive;
    } else {
        g_haveTelemetry = false;
    }
    return g_haveTelemetry;
}

bool can_leave_cab(std::wstring& reason) {
    if (!g_haveTelemetry) return true;

    if (std::abs(g_gameState.speedMps) > g_settings.exitSpeedThreshold) {
        reason = L"STOP THE TRUCK FIRST";
        return false;
    }
    if (!g_gameState.parkingBrake) {
        reason = L"SET THE PARKING BRAKE FIRST";
        return false;
    }
    return true;
}

void initialize_walker_world_position() {
    if (!g_haveTelemetry) {
        g_walkerPositionValid = false;
        return;
    }

    const WorldPoint d = door_point();
    g_walkerX = d.x;
    g_walkerY = std::max(d.y, g_gameState.worldY + 0.2);
    g_walkerZ = d.z;
    g_walkerYaw = game_heading_radians();
    g_walkerPositionValid = true;
}

void update_walker_world_estimate(double dt) {
    if (!g_walkerPositionValid || dt <= 0.0) return;

    const long mdx = g_mouseDx.exchange(0);
    g_mouseDy.exchange(0);
    g_walkerYaw += static_cast<double>(mdx) * g_settings.mouseLookScale;

    while (g_walkerYaw > 3.14159265358979323846) g_walkerYaw -= 6.28318530717958647692;
    while (g_walkerYaw < -3.14159265358979323846) g_walkerYaw += 6.28318530717958647692;

    double forward = 0.0;
    double right = 0.0;

    if (key_down(g_settings.forwardKey)) forward += 1.0;
    if (key_down(g_settings.backwardKey)) forward -= g_settings.backwardFactor;
    if (key_down(g_settings.rightKey)) right += g_settings.strafeFactor;
    if (key_down(g_settings.leftKey)) right -= g_settings.strafeFactor;

    const double magnitude = std::sqrt(forward * forward + right * right);
    if (magnitude > 1.0) {
        forward /= magnitude;
        right /= magnitude;
    }

    const double speed =
        g_settings.walkSpeed *
        (key_down(g_settings.sprintKey) ? std::max(1.0, g_settings.sprintMultiplier) : 1.0);

    const double fx = std::sin(g_walkerYaw);
    const double fz = std::cos(g_walkerYaw);
    const double rx = std::cos(g_walkerYaw);
    const double rz = -std::sin(g_walkerYaw);

    g_walkerX += (fx * forward + rx * right) * speed * dt;
    g_walkerZ += (fz * forward + rz * right) * speed * dt;
}

enum class InteractionKind {
    None,
    EnterCab,
    FuelCard,
    FuelNozzle,
    FuelReceipt,
    Trailer
};

InteractionKind current_interaction(std::wstring& label) {
    label.clear();
    if (!g_walkerPositionValid || !g_haveTelemetry) return InteractionKind::None;

    const WorldPoint door = door_point();
    if (distance_xz(g_walkerX, g_walkerZ, door.x, door.z) <= g_settings.interactRange) {
        label = L"[F] ENTER CAB";
        return InteractionKind::EnterCab;
    }

    if (g_settings.fuelEnabled &&
        std::abs(g_gameState.speedMps) <= g_settings.exitSpeedThreshold &&
        g_gameState.parkingBrake &&
        !g_gameState.engineEnabled) {

        int fuelStage = 0;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            fuelStage = g_ui.fuelStage;
        }

        if (fuelStage == 0 || fuelStage == 1) {
            const WorldPoint pump = pump_point();
            if (distance_xz(g_walkerX, g_walkerZ, pump.x, pump.z) <= g_settings.pumpRadius) {
                label = g_settings.fuelCardStep ? L"[F] PAY WITH TS FLEET CARD" : L"[F] TAKE NOZZLE";
                return InteractionKind::FuelCard;
            }
        } else if (fuelStage == 2 || fuelStage == 3) {
            const WorldPoint tank = tank_point();
            if (distance_xz(g_walkerX, g_walkerZ, tank.x, tank.z) <= g_settings.tankRadius) {
                label = fuelStage == 3 ? L"FUELING... HOLD [F]" : L"[F] FUEL";
                return InteractionKind::FuelNozzle;
            }
        } else if (fuelStage == 4) {
            const WorldPoint pump = pump_point();
            if (distance_xz(g_walkerX, g_walkerZ, pump.x, pump.z) <= g_settings.pumpRadius) {
                label = L"[F] TAKE RECEIPT";
                return InteractionKind::FuelReceipt;
            }
        }
    }

    if (g_settings.trailerEnabled) {
        const WorldPoint fifth = fifth_wheel_point();
        if (distance_xz(g_walkerX, g_walkerZ, fifth.x, fifth.z) <= g_settings.fifthWheelRadius) {
            int stage = 0;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                stage = g_ui.trailerStage;
            }

            if (g_gameState.trailerAttached) {
                if (!g_settings.trailerSteps) label = L"[F] UNCOUPLE TRAILER";
                else if (stage == 0) label = L"[F] LOWER LANDING GEAR";
                else if (stage == 1) label = L"[F] DISCONNECT AIR / ELECTRIC";
                else label = L"[F] RELEASE FIFTH WHEEL";
            } else {
                if (!g_settings.trailerCoupleSteps) label = L"[F] COUPLE TRAILER";
                else if (stage == 0) label = L"[F] LOCK FIFTH WHEEL";
                else if (stage == 1) label = L"[F] CONNECT AIR / ELECTRIC";
                else label = L"[F] RAISE LANDING GEAR";
            }
            return InteractionKind::Trailer;
        }
    }

    return InteractionKind::None;
}

void update_context_prompt() {
    if (!g_haveTelemetry) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_ui.context = L"TSMS TELEMETRY OFFLINE - CAMERA FALLBACK";
        if (g_promptWindow) InvalidateRect(g_promptWindow, nullptr, FALSE);
        return;
    }

    std::wstring label;
    current_interaction(label);

    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (!label.empty()) {
        g_ui.context = label;
    } else if (g_gameState.engineEnabled && g_settings.fuelEnabled) {
        g_ui.context = L"Fuel roleplay: stop, parking brake, engine off";
    } else {
        g_ui.context = L"";
    }
    if (g_promptWindow) InvalidateRect(g_promptWindow, nullptr, FALSE);
}

void handle_trailer_interaction() {
    if (!g_settings.trailerEnabled) return;

    int stage = 0;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        stage = g_ui.trailerStage;
    }

    const bool attached = g_gameState.trailerAttached;
    const bool staged = attached ? g_settings.trailerSteps : g_settings.trailerCoupleSteps;

    if (!staged) {
        tap_key(g_settings.trailerAttachKey);
        play_audio(L"nozzle.wav");
        set_ui_status(attached ? L"TRAILER UNCOUPLE REQUESTED" : L"TRAILER COUPLE REQUESTED");
        return;
    }

    if (stage == 0) {
        play_audio(L"nozzle.wav");
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_ui.trailerStage = 1;
        g_ui.status = attached ? L"LANDING GEAR LOWERED" : L"FIFTH WHEEL LOCKED";
    } else if (stage == 1) {
        play_audio(L"nozzle.wav");
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_ui.trailerStage = 2;
        g_ui.status = attached ? L"AIR / ELECTRIC DISCONNECTED" : L"AIR / ELECTRIC CONNECTED";
    } else {
        if (attached) {
            tap_key(g_settings.trailerAttachKey);
            set_ui_status(L"FIFTH WHEEL RELEASE REQUESTED");
        } else {
            // For coupling the actual game attach key is issued at the lock step completion.
            tap_key(g_settings.trailerAttachKey);
            set_ui_status(L"TRAILER COUPLE REQUESTED");
        }
        play_audio(L"door.wav");
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_ui.trailerStage = 0;
    }
}

void launch_config_editor() {
    if (!std::filesystem::exists(g_configExePath)) {
        set_ui_status(L"CONFIG EXE NOT FOUND");
        log_line(L"TSRealDriverConfig.exe not found next to the DLL.");
        return;
    }

    std::wstring command = L"\"" + g_configExePath.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::wstring mutableCommand = command;
    if (CreateProcessW(
            nullptr,
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            g_moduleDir.c_str(),
            &si,
            &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        set_ui_status(L"CONFIG OPENED");
    } else {
        set_ui_status(L"CONFIG LAUNCH FAILED");
        log_line(L"Could not launch TSRealDriverConfig.exe.");
    }
}

void draw_text(HDC dc, int x, int y, const wchar_t* text, int size, COLORREF color, bool bold = false) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(size, GetDeviceCaps(dc, LOGPIXELSY), 72);
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");

    HFONT font = CreateFontIndirectW(&lf);
    HFONT old = static_cast<HFONT>(SelectObject(dc, font));
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, x, y, text, static_cast<int>(wcslen(text)));
    SelectObject(dc, old);
    DeleteObject(font);
}

LRESULT CALLBACK PromptWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);

        HBRUSH clearBrush = CreateSolidBrush(RGB(1, 2, 3));
        FillRect(dc, &ps.rcPaint, clearBrush);
        DeleteObject(clearBrush);

        UiState state;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            state = g_ui;
        }

        const COLORREF gold = RGB(225, 184, 86);
        const COLORREF white = RGB(242, 242, 244);
        const COLORREF gray = RGB(175, 177, 185);

        draw_text(dc, 18, 12, L"TS REAL DRIVE", 16, gold, true);
        draw_text(dc, 18, 42, state.status.c_str(), 11, white, true);

        if (state.walking) {
            std::wstring line = L"WASD walk  |  Shift run  |  Space jump  |  F interact  |  Ctrl+F10 settings";
            draw_text(dc, 18, 68, line.c_str(), 9, gray, false);

            std::wstring line2 = L"Right click flashlight  |  G beam size  |  mouse wheel speed";
            draw_text(dc, 18, 92, line2.c_str(), 9, gray, false);

            if (!state.context.empty()) {
                draw_text(dc, 18, 118, state.context.c_str(), 10, gold, true);
            }

            if (state.paused) {
                draw_text(dc, 18, 142, L"WALK INPUT PAUSED (console mode)", 9, gold, true);
            }

            if (state.fuelStage > 0) {
                RECT card{360, 116, 590, 195};
                HBRUSH cardBrush = CreateSolidBrush(RGB(31, 34, 42));
                FillRect(dc, &card, cardBrush);
                DeleteObject(cardBrush);

                HPEN pen = CreatePen(PS_SOLID, 2, gold);
                HGDIOBJ oldPen = SelectObject(dc, pen);
                HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, card.left, card.top, card.right, card.bottom);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(pen);

                draw_text(dc, 374, 126, L"TS FLEET", 13, gold, true);

                const wchar_t* fuelText = L"CARD READY - F";
                if (state.fuelStage == 2) fuelText = L"NOZZLE READY - HOLD F";
                if (state.fuelStage == 3) fuelText = L"FUELING...";
                if (state.fuelStage == 4) fuelText = L"RECEIPT READY - F";
                draw_text(dc, 374, 154, fuelText, 9, white, false);
            }
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

LRESULT CALLBACK FadeWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

LRESULT CALLBACK FlashlightWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);

        RECT client{};
        GetClientRect(hwnd, &client);

        HBRUSH clearBrush = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(dc, &client, clearBrush);
        DeleteObject(clearBrush);

        UiState state;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            state = g_ui;
        }

        const int cx = (client.right - client.left) / 2;
        const int cy = (client.bottom - client.top) / 2;

        if (state.walking && g_settings.shadowEnabled) {
            const int shadowW = 210;
            const int shadowH = 58;
            const int shadowY = client.bottom - 130;

            HBRUSH shadow = CreateSolidBrush(RGB(12, 12, 14));
            HPEN none = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
            HGDIOBJ oldBrush = SelectObject(dc, shadow);
            HGDIOBJ oldPen = SelectObject(dc, none);
            Ellipse(dc, cx - shadowW / 2, shadowY - shadowH / 2, cx + shadowW / 2, shadowY + shadowH / 2);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(none);
            DeleteObject(shadow);
        }

        if (state.flashlight) {
            const int scale = state.flashlightSize;
            const int width = 360 + scale * 130;
            const int height = 240 + scale * 90;

            HBRUSH outer = CreateSolidBrush(RGB(255, 245, 205));
            HPEN none = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
            HGDIOBJ oldBrush = SelectObject(dc, outer);
            HGDIOBJ oldPen = SelectObject(dc, none);
            Ellipse(dc, cx - width / 2, cy - height / 2, cx + width / 2, cy + height / 2);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(none);
            DeleteObject(outer);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void create_overlay_windows() {
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSW promptClass{};
    promptClass.lpfnWndProc = PromptWindowProc;
    promptClass.hInstance = instance;
    promptClass.lpszClassName = L"TSRealDriverPromptOverlay";
    promptClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&promptClass);

    WNDCLASSW flashClass{};
    flashClass.lpfnWndProc = FlashlightWindowProc;
    flashClass.hInstance = instance;
    flashClass.lpszClassName = L"TSRealDriverFlashOverlay";
    flashClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&flashClass);

    WNDCLASSW fadeClass{};
    fadeClass.lpfnWndProc = FadeWindowProc;
    fadeClass.hInstance = instance;
    fadeClass.lpszClassName = L"TSRealDriverFadeOverlay";
    fadeClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&fadeClass);

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);

    g_flashlightWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"TSRealDriverFlashOverlay",
        L"",
        WS_POPUP,
        0, 0, screenW, screenH,
        nullptr, nullptr, instance, nullptr);

    if (g_flashlightWindow) {
        SetLayeredWindowAttributes(g_flashlightWindow, RGB(255, 0, 255), 52, LWA_COLORKEY | LWA_ALPHA);
        ShowWindow(g_flashlightWindow, SW_HIDE);
    }

    g_fadeWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"TSRealDriverFadeOverlay",
        L"",
        WS_POPUP,
        0, 0, screenW, screenH,
        nullptr, nullptr, instance, nullptr);

    if (g_fadeWindow) {
        SetLayeredWindowAttributes(g_fadeWindow, 0, 0, LWA_ALPHA);
        ShowWindow(g_fadeWindow, SW_HIDE);
    }

    g_promptWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"TSRealDriverPromptOverlay",
        L"",
        WS_POPUP,
        24, screenH - 245, 620, 205,
        nullptr, nullptr, instance, nullptr);

    if (g_promptWindow) {
        SetLayeredWindowAttributes(g_promptWindow, RGB(1, 2, 3), 255, LWA_COLORKEY | LWA_ALPHA);
        ShowWindow(g_promptWindow, g_settings.prompts ? SW_SHOWNOACTIVATE : SW_HIDE);
        InvalidateRect(g_promptWindow, nullptr, TRUE);
    }
}

void destroy_overlay_windows() {
    if (g_fadeWindow) {
        DestroyWindow(g_fadeWindow);
        g_fadeWindow = nullptr;
    }
    if (g_flashlightWindow) {
        DestroyWindow(g_flashlightWindow);
        g_flashlightWindow = nullptr;
    }
    if (g_promptWindow) {
        DestroyWindow(g_promptWindow);
        g_promptWindow = nullptr;
    }
}

void pump_messages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

struct Edge {
    bool previous = false;

    bool pressed(bool now) {
        const bool result = now && !previous;
        previous = now;
        return result;
    }
};

bool g_sentForward = false;
bool g_sentBackward = false;
bool g_sentLeft = false;
bool g_sentRight = false;
bool g_sentEyeUp = false;
bool g_sentEyeDown = false;

void mirror_key(int source, int target, bool& state) {
    const bool down = key_down(source);
    if (down == state) return;
    send_game_key(target, down);
    state = down;
}

void release_walk_keys() {
    if (g_sentForward) send_game_key(VK_NUMPAD8, false);
    if (g_sentBackward) send_game_key(VK_NUMPAD2, false);
    if (g_sentLeft) send_game_key(VK_NUMPAD4, false);
    if (g_sentRight) send_game_key(VK_NUMPAD6, false);
    if (g_sentEyeUp) send_game_key(VK_NUMPAD9, false);
    if (g_sentEyeDown) send_game_key(VK_NUMPAD3, false);

    g_sentForward = false;
    g_sentBackward = false;
    g_sentLeft = false;
    g_sentRight = false;
    g_sentEyeUp = false;
    g_sentEyeDown = false;
}

void update_overlay_visibility() {
    UiState state;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        state = g_ui;
    }

    if (g_promptWindow) {
        ShowWindow(g_promptWindow, g_settings.prompts ? SW_SHOWNOACTIVATE : SW_HIDE);
        InvalidateRect(g_promptWindow, nullptr, TRUE);
    }

    if (g_flashlightWindow) {
        const bool show = state.walking &&
            ((state.flashlight && g_settings.flashlightEnabled) || g_settings.shadowEnabled);
        ShowWindow(g_flashlightWindow, show ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (show) InvalidateRect(g_flashlightWindow, nullptr, TRUE);
    }
}

void transition_fade(bool toBlack) {
    if (!g_settings.fadeEnabled || !g_fadeWindow) return;

    ShowWindow(g_fadeWindow, SW_SHOWNOACTIVATE);
    constexpr int steps = 8;
    for (int i = 0; i <= steps; ++i) {
        const int phase = toBlack ? i : (steps - i);
        const BYTE alpha = static_cast<BYTE>(std::clamp(phase * 28, 0, 224));
        SetLayeredWindowAttributes(g_fadeWindow, 0, alpha, LWA_ALPHA);
        std::this_thread::sleep_for(12ms);
    }

    if (!toBlack) ShowWindow(g_fadeWindow, SW_HIDE);
}

void enter_walk_mode() {
    if (!g_settings.debugCameraBridge) {
        set_ui_status(L"DEBUG CAMERA BRIDGE DISABLED");
        return;
    }

    refresh_game_state();
    std::wstring blockedReason;
    if (!can_leave_cab(blockedReason)) {
        set_ui_status(blockedReason);
        return;
    }

    transition_fade(true);

    // First let the game itself activate Numpad-0 free camera. Going through
    // the game's normal camera command initializes/synchronizes the debug camera
    // better than writing only the requested slot, which can resurrect an old
    // free-camera position from another place on the map.
    game_camera_bridge_refresh();
    g_previousCameraSlot = game_camera_bridge_current_slot();

    log_line(game_camera_bridge_report());

    bool debugReady = false;
    const auto debugStatus = game_camera_bridge_status();
    if (debugStatus.debugCameraSlot >= 0) {
        tap_game_key(VK_NUMPAD0);

        const ULONGLONG start = GetTickCount64();
        while (GetTickCount64() - start <= 1200) {
            const int current = game_camera_bridge_current_slot();
            if (current == debugStatus.debugCameraSlot) {
                debugReady = true;
                break;
            }
            Sleep(20);
        }
    }

    if (debugReady) {
        log_line(L"Walk enter: debug camera activated through the game's Numpad-0 path.");
    } else {
        log_line(L"Walk enter: Numpad-0 path did not switch cameras; falling back to native slot request.");
        if (!game_camera_bridge_request_debug(1200)) {
            log_line(std::wstring(L"Walk enter failed: ") + game_camera_bridge_status().error);
            log_line(game_camera_bridge_census());
            transition_fade(false);
            set_ui_status(L"NATIVE WALK CAMERA NOT READY");
            return;
        }
        log_line(L"Walk enter: native debug camera fallback requested successfully.");
    }

    // Step sideways out of the driver's side without forcing the camera down
    // under the truck. Vertical placement will stay at the game's camera height.
    if (g_settings.autoDoorOffset) {
        set_game_flyspeed(g_settings.spawnFlySpeed);
        hold_game_key_for(VK_NUMPAD4, 260ms);
        set_game_flyspeed(g_settings.walkFlySpeed);
    }

    initialize_walker_world_position();

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_ui.walking = true;
        g_ui.paused = false;
        g_ui.fuelStage = 0;
        g_ui.fueling = false;
        g_ui.trailerStage = 0;
        g_ui.status = L"WALK MODE";
        g_ui.context.clear();
    }
    play_audio(L"door.wav");
    update_overlay_visibility();
    transition_fade(false);
    log_line(L"Walk mode enabled.");
}

void leave_walk_mode() {
    release_walk_keys();
    send_key(VK_RETURN, false);

    bool flashlightWasOn = false;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        flashlightWasOn = g_ui.flashlight;
        g_ui.walking = false;
        g_ui.paused = false;
        g_ui.fueling = false;
        g_ui.fuelStage = 0;
        g_ui.status = L"IN CAB";
    }

    transition_fade(true);
    set_game_flyspeed(g_settings.restoreFlySpeed);

    if (g_previousCameraSlot >= 0) {
        bool restored = false;

        // Mirror the normal game behaviour first: Numpad 0 leaves the developer
        // camera and normally restores the camera that was active before it.
        tap_game_key(VK_NUMPAD0);
        const ULONGLONG restoreStart = GetTickCount64();
        while (GetTickCount64() - restoreStart <= 900) {
            if (game_camera_bridge_current_slot() == g_previousCameraSlot) {
                restored = true;
                break;
            }
            Sleep(20);
        }

        if (restored) {
            log_line(L"Walk leave: previous camera restored through the game's Numpad-0 path.");
        } else if (game_camera_bridge_request_slot(g_previousCameraSlot, 1200)) {
            log_line(L"Walk leave: restored the previous game camera slot through native fallback.");
        } else {
            log_line(std::wstring(L"Walk leave: could not restore previous camera slot: ") +
                     game_camera_bridge_status().error);
        }
    } else {
        log_line(L"Walk leave: previous camera slot was not known.");
    }

    if (flashlightWasOn) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_ui.flashlight = false;
    }

    g_walkerPositionValid = false;
    play_audio(L"door.wav");
    update_overlay_visibility();
    transition_fade(false);
    log_line(L"Walk mode disabled.");
}

void do_jump() {
    set_ui_status(L"JUMP");
    send_key(VK_NUMPAD9, true);
    std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double>(g_settings.jumpUpSeconds)));
    send_key(VK_NUMPAD9, false);

    std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double>(g_settings.jumpHangSeconds)));

    send_key(VK_NUMPAD3, true);
    std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double>(g_settings.jumpDownSeconds)));
    send_key(VK_NUMPAD3, false);

    set_ui_status(L"WALK MODE");
}

void begin_fuel_roleplay() {
    if (!g_settings.fuelEnabled) {
        set_ui_status(L"FUEL ROLEPLAY DISABLED");
        return;
    }

    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (g_ui.fuelStage == 0) {
        g_ui.fuelStage = 1;
        g_ui.status = L"FUEL: PRESENT TS FLEET CARD";
    } else {
        g_ui.fuelStage = 0;
        g_ui.fueling = false;
        g_ui.status = L"WALK MODE";
        send_key(VK_RETURN, false);
    }
    if (g_promptWindow) InvalidateRect(g_promptWindow, nullptr, TRUE);
}

void handle_fuel_interaction(bool interactNow, bool interactPressed) {
    int stage = 0;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        stage = g_ui.fuelStage;
    }

    if (stage == 0) return;

    if (stage == 1 && interactPressed) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_ui.fuelStage = 2;
        g_ui.status = L"FUEL: TAKE NOZZLE / HOLD F AT TANK";
        play_audio(L"card.wav");
        InvalidateRect(g_promptWindow, nullptr, TRUE);
        return;
    }

    if (stage == 2 && interactNow) {
        send_key(VK_RETURN, true);
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_ui.fuelStage = 3;
        g_ui.fueling = true;
        play_audio(L"nozzle.wav");
        g_ui.status = L"FUELING - F IS HOLDING GAME ENTER";
        InvalidateRect(g_promptWindow, nullptr, TRUE);
        return;
    }

    if (stage == 3 && !interactNow) {
        send_key(VK_RETURN, false);
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_ui.fuelStage = 4;
        g_ui.fueling = false;
        g_ui.status = L"FUEL: HANG NOZZLE / TAKE RECEIPT";
        InvalidateRect(g_promptWindow, nullptr, TRUE);
        return;
    }

    if (stage == 4 && interactPressed) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_ui.fuelStage = 0;
        g_ui.status = L"WALK MODE";
        play_audio(L"receipt.wav");
        InvalidateRect(g_promptWindow, nullptr, TRUE);
    }
}

void worker_main() {
    load_settings();
    tsms_bridge_open();
    refresh_game_state();
    install_mouse_hook();
    install_keyboard_hook();
    create_overlay_windows();

    Edge f10Edge;
    Edge interactEdge;
    Edge sprintEdge;
    Edge jumpEdge;
    Edge crouchEdge;
    Edge flashEdge;
    Edge flashSizeEdge;
    Edge fuelEdge;
    Edge buildingEdge;
    Edge consoleEdge;
    Edge resetEyeEdge;
    Edge speedResetEdge;

    auto lastReloadCheck = Clock::now();
    auto nextTelemetry = Clock::now();
    auto nextContext = Clock::now();
    auto nextStep = Clock::now();
    auto nextBreath = Clock::now();
    auto lastFrame = Clock::now();
    Clock::time_point sprintStarted{};
    int bobPhase = 1;
    int manualSpeedOffset = 0;
    bool lastTrailerAttached = g_gameState.trailerAttached;

    while (!g_stop.load()) {
        pump_messages();

        const auto frameNow = Clock::now();
        const double dt = std::clamp(
            std::chrono::duration<double>(frameNow - lastFrame).count(),
            0.0, 0.05);
        lastFrame = frameNow;

        if (frameNow >= nextTelemetry) {
            refresh_game_state();

            if (g_haveTelemetry && g_gameState.trailerAttached != lastTrailerAttached) {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_ui.trailerStage = 0;
                lastTrailerAttached = g_gameState.trailerAttached;
            }

            nextTelemetry = frameNow + 50ms;
        }

        const bool walking = [&] {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            return g_ui.walking;
        }();

        const bool f10Now = key_down(g_settings.toggleKey);
        const bool ctrlNow = key_down(VK_CONTROL);

        if (f10Edge.pressed(f10Now)) {
            if (ctrlNow) {
                launch_config_editor();
            } else if (walking) {
                leave_walk_mode();
            } else {
                enter_walk_mode();
            }
        }

        const bool consoleNow = key_down(g_settings.consoleKey);
        if (consoleEdge.pressed(consoleNow) && walking) {
            bool paused = false;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_ui.paused = !g_ui.paused;
                paused = g_ui.paused;
                g_ui.status = paused ? L"INPUT PAUSED FOR CONSOLE" : L"WALK MODE";
            }
            if (paused) release_walk_keys();
            update_overlay_visibility();
        }

        bool paused = false;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            paused = g_ui.paused;
        }

        if (walking && !paused) {
            update_walker_world_estimate(dt);

            // The game's developer/free-camera movement axes.
            mirror_key(g_settings.forwardKey, VK_NUMPAD8, g_sentForward);
            mirror_key(g_settings.backwardKey, VK_NUMPAD2, g_sentBackward);
            mirror_key(g_settings.leftKey, VK_NUMPAD4, g_sentLeft);
            mirror_key(g_settings.rightKey, VK_NUMPAD6, g_sentRight);
            mirror_key(g_settings.eyeUpKey, VK_NUMPAD9, g_sentEyeUp);
            mirror_key(g_settings.eyeDownKey, VK_NUMPAD3, g_sentEyeDown);

            const int manualWheel = g_mouseWheel.exchange(0);
            if (manualWheel != 0) {
                manualSpeedOffset += manualWheel;
                set_ui_status(manualWheel > 0 ? L"WALK SPEED +" : L"WALK SPEED -");
            }

            const bool speedResetNow = key_down(g_settings.speedResetKey);
            if (speedResetEdge.pressed(speedResetNow) && manualSpeedOffset != 0) {
                send_wheel(-manualSpeedOffset);
                manualSpeedOffset = 0;
                set_ui_status(L"WALK SPEED RESET");
            }

            const bool sprintNow = key_down(g_settings.sprintKey);
            if (sprintEdge.pressed(sprintNow)) {
                send_wheel(g_settings.sprintWheelNotches);
                set_ui_status(L"RUN");
            }

            static bool sprintHeld = false;
            if (sprintNow != sprintHeld) {
                if (!sprintNow && sprintHeld) {
                    send_wheel(-g_settings.sprintWheelNotches);
                    set_ui_status(L"WALK MODE");
                }
                sprintHeld = sprintNow;
            }

            const bool movingNow =
                key_down(g_settings.forwardKey) ||
                key_down(g_settings.backwardKey) ||
                key_down(g_settings.leftKey) ||
                key_down(g_settings.rightKey);

            const auto motionNow = Clock::now();
            if (movingNow) {
                const double speed = std::max(0.25,
                    g_settings.walkSpeed * (sprintNow ? std::max(1.0, g_settings.sprintMultiplier) : 1.0));
                const double stride = std::max(0.25,
                    g_settings.stepLength * (sprintNow ? std::max(0.40, g_settings.runStride) : 1.0));
                const double intervalSeconds = std::clamp(stride / speed, 0.12, 1.20);

                if (motionNow >= nextStep) {
                    const wchar_t* stepSound = nullptr;
                    if (sprintNow) stepSound = (bobPhase > 0) ? L"runstep1.wav" : L"runstep2.wav";
                    else stepSound = (bobPhase > 0) ? L"footstep1.wav" : L"footstep2.wav";
                    if (g_settings.footstepSounds) play_audio(stepSound);

                    if (g_settings.headBob) {
                        const int bobPixels = std::max(1, static_cast<int>(std::round(g_settings.bobAmount * 500.0)));
                        const int swayPixels = std::max(0, static_cast<int>(std::round(g_settings.swayAmount * 500.0)));
                        mouse_nudge(bobPhase * swayPixels, bobPhase * bobPixels);
                        bobPhase = -bobPhase;
                    }

                    nextStep = motionNow + std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(intervalSeconds));
                }

                if (sprintNow) {
                    if (sprintStarted == Clock::time_point{}) sprintStarted = motionNow;
                    const double runningFor = std::chrono::duration<double>(motionNow - sprintStarted).count();
                    if (g_settings.breathingSound &&
                        runningFor >= g_settings.tiredAfterSeconds &&
                        motionNow >= nextBreath) {
                        play_audio(L"breath.wav");
                        nextBreath = motionNow + 3s;
                    }
                } else {
                    sprintStarted = Clock::time_point{};
                }
            } else {
                nextStep = motionNow;
                sprintStarted = Clock::time_point{};
            }

            const bool crouchNow = key_down(g_settings.crouchKey);
            if (crouchEdge.pressed(crouchNow)) {
                send_key(VK_NUMPAD3, true);
                std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::duration<double>(g_settings.crouchDropSeconds)));
                send_key(VK_NUMPAD3, false);
                set_ui_status(L"CROUCH");
            }

            static bool crouchHeld = false;
            if (!crouchNow && crouchHeld) {
                send_key(VK_NUMPAD9, true);
                std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::duration<double>(g_settings.crouchDropSeconds)));
                send_key(VK_NUMPAD9, false);
                set_ui_status(L"WALK MODE");
            }
            crouchHeld = crouchNow;

            const bool jumpNow = key_down(g_settings.jumpKey);
            if (jumpEdge.pressed(jumpNow)) {
                do_jump();
            }

            const bool resetEyeNow = key_down(g_settings.eyeResetKey);
            if (resetEyeEdge.pressed(resetEyeNow)) {
                hold_key_for(VK_NUMPAD9, 120ms);
                set_ui_status(L"EYE HEIGHT RESET");
            }

            const bool flashNow = key_down(g_settings.flashlightKey);
            if (flashEdge.pressed(flashNow) && g_settings.flashlightEnabled) {
                {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    g_ui.flashlight = !g_ui.flashlight;
                    g_ui.status = g_ui.flashlight ? L"FLASHLIGHT ON" : L"FLASHLIGHT OFF";
                }
                play_audio(L"flashlight.wav");
                update_overlay_visibility();
            }

            const bool flashSizeNow = key_down(g_settings.flashlightSizeKey);
            if (flashSizeEdge.pressed(flashSizeNow)) {
                {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    g_ui.flashlightSize = (g_ui.flashlightSize % 3) + 1;
                    g_ui.status = L"FLASHLIGHT BEAM SIZE";
                }
                update_overlay_visibility();
            }

            const bool buildingNow = key_down(g_settings.buildingKey);
            if (buildingEdge.pressed(buildingNow)) {
                {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    g_ui.buildingMode = !g_ui.buildingMode;
                    g_ui.status = g_ui.buildingMode ? L"BUILDING / GHOST WALK" : L"WALK MODE";
                }
                update_overlay_visibility();
            }

            // F7 remains a manual fallback. Normal fuel use is contextual and does not require it.
            const bool fuelNow = key_down(g_settings.fuelModeKey);
            if (fuelEdge.pressed(fuelNow)) {
                begin_fuel_roleplay();
            }

            const bool interactNow = key_down(g_settings.interactKey);
            const bool interactPressed = interactEdge.pressed(interactNow);

            int fuelStage = 0;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                fuelStage = g_ui.fuelStage;
            }

            // Releasing F always stops an active game fueling hold.
            if (fuelStage == 3 && !interactNow) {
                send_key(VK_RETURN, false);
                {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    g_ui.fuelStage = 4;
                    g_ui.fueling = false;
                    g_ui.status = L"FUEL: RETURN NOZZLE / TAKE RECEIPT";
                }
                play_audio(L"nozzle.wav");
            }

            std::wstring interactionLabel;
            InteractionKind interaction = current_interaction(interactionLabel);

            if (interactPressed) {
                switch (interaction) {
                case InteractionKind::EnterCab:
                    leave_walk_mode();
                    break;

                case InteractionKind::FuelCard: {
                    {
                        std::lock_guard<std::mutex> lock(g_stateMutex);
                        g_ui.fuelStage = 2;
                        g_ui.status = g_settings.fuelCardStep
                            ? L"TS FLEET CARD ACCEPTED - TAKE NOZZLE"
                            : L"TAKE NOZZLE";
                    }
                    play_audio(L"card.wav");
                    break;
                }

                case InteractionKind::FuelNozzle: {
                    int stageNow = 0;
                    {
                        std::lock_guard<std::mutex> lock(g_stateMutex);
                        stageNow = g_ui.fuelStage;
                    }

                    if (stageNow == 2) {
                        if (g_walkerPositionValid) {
                            double localX = 0.0;
                            double localZ = 0.0;
                            world_to_truck_local(g_walkerX, g_walkerZ, localX, localZ);
                            save_learned_tank(localX, localZ);
                        }

                        send_key(VK_RETURN, true);
                        {
                            std::lock_guard<std::mutex> lock(g_stateMutex);
                            g_ui.fuelStage = 3;
                            g_ui.fueling = true;
                            g_ui.status = L"FUELING - HOLD F";
                        }
                        play_audio(L"nozzle.wav");
                    }
                    break;
                }

                case InteractionKind::FuelReceipt:
                    {
                        std::lock_guard<std::mutex> lock(g_stateMutex);
                        g_ui.fuelStage = 0;
                        g_ui.fueling = false;
                        g_ui.status = L"WALK MODE";
                    }
                    play_audio(L"receipt.wav");
                    break;

                case InteractionKind::Trailer:
                    handle_trailer_interaction();
                    break;

                case InteractionKind::None:
                default:
                    break;
                }
            }

            // If the user manually armed fuel roleplay with F7, keep its original fallback state machine.
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                fuelStage = g_ui.fuelStage;
            }
            if (!g_haveTelemetry && fuelStage > 0) {
                handle_fuel_interaction(interactNow, interactPressed);
            }

            if (frameNow >= nextContext) {
                update_context_prompt();
                nextContext = frameNow + 100ms;
            }
        } else {
            release_walk_keys();
            g_mouseDx.exchange(0);
            g_mouseDy.exchange(0);
            g_mouseWheel.exchange(0);
        }

        if (Clock::now() - lastReloadCheck > 1s) {
            reload_settings_if_changed();
            lastReloadCheck = Clock::now();
        }

        std::this_thread::sleep_for(8ms);
    }

    release_walk_keys();
    send_key(VK_RETURN, false);
    uninstall_mouse_hook();
    uninstall_keyboard_hook();
    tsms_bridge_close();
    destroy_overlay_windows();
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) int scs_telemetry_init(unsigned int, const void*) {
    if (g_worker.joinable()) return 0;

    g_moduleDir = module_directory();
    if (g_moduleDir.empty()) return 1;

    g_iniPath = g_moduleDir / L"TSRealDriver.ini";
    g_logPath = g_moduleDir / L"TSRealDriver.log";
    g_configExePath = g_moduleDir / L"TSRealDriverConfig.exe";
    g_audioDir = g_moduleDir / L"TSRealDriver.audio";
    g_tanksPath = g_moduleDir / L"TSRealDriver.tanks.ini";
    ensure_audio_assets();

    native_game_bridge_initialize();
    log_line(native_game_bridge_report());

    if (game_camera_bridge_resolve()) {
        log_line(game_camera_bridge_report());
        log_line(game_camera_bridge_census());
    } else {
        log_line(std::wstring(L"GameCameraBridge resolve failed: ") +
                 game_camera_bridge_status().error);
        log_line(game_camera_bridge_census());
    }

    g_stop = false;
    g_worker = std::thread(worker_main);
    log_line(L"TSRealDriver 0.5.3 initialized.");
    return 0;
}

extern "C" __declspec(dllexport) void scs_telemetry_shutdown() {
    g_stop = true;
    if (g_worker.joinable()) {
        g_worker.join();
    }
    native_game_bridge_shutdown();
    log_line(L"TSRealDriver shutdown.");
}

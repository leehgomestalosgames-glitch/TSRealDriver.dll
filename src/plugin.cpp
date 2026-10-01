#include <windows.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <mmsystem.h>

#include "native_game_bridge.hpp"
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
    int fuelModeKey = VK_F7;
    int consoleKey = VK_OEM_3;

    double walkSpeed = 1.75;
    double sprintMultiplier = 2.60;
    double crouchDropSeconds = 0.16;
    double jumpUpSeconds = 0.12;
    double jumpHangSeconds = 0.12;
    double jumpDownSeconds = 0.16;

    bool prompts = true;
    bool flashlightEnabled = true;
    bool fuelEnabled = true;
    bool debugCameraBridge = true;
    bool autoDoorOffset = true;
    int sprintWheelNotches = 3;

    bool headBob = true;
    bool breathingMotion = true;
    bool fadeEnabled = true;
    bool soundEnabled = true;
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
};

struct UiState {
    bool walking = false;
    bool paused = false;
    bool flashlight = false;
    int flashlightSize = 1;
    int fuelStage = 0;
    bool fueling = false;
    bool buildingMode = false;
    std::wstring status = L"READY";
};

Settings g_settings;
std::mutex g_stateMutex;
UiState g_ui;

std::filesystem::path g_moduleDir;
std::filesystem::path g_iniPath;
std::filesystem::path g_logPath;
std::filesystem::path g_configExePath;
std::filesystem::path g_audioDir;

HWND g_promptWindow = nullptr;
HWND g_flashlightWindow = nullptr;
HWND g_fadeWindow = nullptr;
FILETIME g_lastIniWrite{};

bool key_down(int vk) {
    return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
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
    g_settings.fuelModeKey = parse_ini_int(L"keys", L"fuel_mode", VK_F7);
    g_settings.consoleKey = parse_ini_int(L"keys", L"console", VK_OEM_3);

    g_settings.walkSpeed = parse_ini_double(L"movement", L"walk_speed", 1.75);
    g_settings.sprintMultiplier = parse_ini_double(L"movement", L"sprint_multiplier", 2.60);
    g_settings.crouchDropSeconds = parse_ini_double(L"movement", L"crouch_drop_seconds", 0.16);
    g_settings.jumpUpSeconds = parse_ini_double(L"movement", L"jump_up_seconds", 0.12);
    g_settings.jumpHangSeconds = parse_ini_double(L"movement", L"jump_hang_seconds", 0.12);
    g_settings.jumpDownSeconds = parse_ini_double(L"movement", L"jump_down_seconds", 0.16);

    g_settings.prompts = parse_ini_bool(L"movement", L"show_prompts", true);
    g_settings.flashlightEnabled = parse_ini_bool(L"flashlight", L"enabled", true);
    g_settings.fuelEnabled = parse_ini_bool(L"fuel", L"enabled", true);
    g_settings.debugCameraBridge = parse_ini_bool(L"camera", L"debug_camera_bridge", true);
    g_settings.autoDoorOffset = parse_ini_bool(L"camera", L"auto_door_offset", true);
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
    g_settings.breathingSound = parse_ini_bool(L"sound", L"breathing", true);
    g_settings.tiredAfterSeconds = parse_ini_double(L"sound", L"tired_after_s", 18.0);
    g_settings.masterVolume = parse_ini_double(L"sound", L"master_volume", 0.70);

    g_settings.shadowEnabled = parse_ini_bool(L"shadow", L"enabled", true);

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

void tap_key(int vk) {
    send_key(vk, true);
    std::this_thread::sleep_for(8ms);
    send_key(vk, false);
}

void hold_key_for(int vk, std::chrono::milliseconds duration) {
    send_key(vk, true);
    std::this_thread::sleep_for(duration);
    send_key(vk, false);
}

void send_wheel(int notches) {
    if (notches == 0) return;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(notches * WHEEL_DELTA);
    SendInput(1, &input, sizeof(INPUT));
}


void write_wave_file(const std::filesystem::path& path, const std::vector<short>& samples, int sampleRate = 8000) {
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

std::vector<short> synth_sound(double seconds, double frequency, double noiseAmount, double amplitude) {
    constexpr int sampleRate = 8000;
    const int count = std::max(1, static_cast<int>(seconds * sampleRate));
    std::vector<short> data(static_cast<size_t>(count));

    std::mt19937 rng(0x54535244u + static_cast<unsigned>(frequency));
    std::uniform_real_distribution<double> noise(-1.0, 1.0);

    for (int i = 0; i < count; ++i) {
        const double t = i / static_cast<double>(sampleRate);
        const double envelope = std::max(0.0, 1.0 - i / static_cast<double>(count));
        const double tone = std::sin(6.283185307179586 * frequency * t);
        const double sample = (tone * (1.0 - noiseAmount) + noise(rng) * noiseAmount) * envelope * amplitude;
        data[static_cast<size_t>(i)] = static_cast<short>(std::clamp(sample, -1.0, 1.0) * 32767.0);
    }
    return data;
}

void ensure_audio_assets() {
    if (g_audioDir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(g_audioDir, ec);

    struct Asset {
        const wchar_t* name;
        double seconds;
        double frequency;
        double noise;
        double amplitude;
    };

    const Asset assets[] = {
        {L"footstep.wav", 0.10, 86.0, 0.55, 0.55},
        {L"runstep.wav", 0.08, 112.0, 0.48, 0.60},
        {L"door.wav", 0.24, 72.0, 0.68, 0.52},
        {L"flashlight.wav", 0.05, 1450.0, 0.12, 0.46},
        {L"card.wav", 0.12, 980.0, 0.05, 0.40},
        {L"nozzle.wav", 0.07, 420.0, 0.25, 0.44},
        {L"receipt.wav", 0.30, 165.0, 0.35, 0.32},
        {L"breath.wav", 0.45, 145.0, 0.80, 0.18},
    };

    for (const auto& asset : assets) {
        const auto path = g_audioDir / asset.name;
        if (!std::filesystem::exists(path)) {
            write_wave_file(path, synth_sound(asset.seconds, asset.frequency, asset.noise, asset.amplitude));
        }
    }
}

void play_audio(const wchar_t* fileName) {
    if (!g_settings.soundEnabled || g_audioDir.empty()) return;
    const auto path = g_audioDir / fileName;
    if (!std::filesystem::exists(path)) return;
    PlaySoundW(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

void mouse_nudge(LONG dx, LONG dy) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
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

        draw_text(dc, 18, 12, L"TS REAL DRIVER", 16, gold, true);
        draw_text(dc, 18, 42, state.status.c_str(), 11, white, true);

        if (state.walking) {
            std::wstring line = L"WASD walk  |  Shift run  |  Space jump  |  F enter cab  |  Ctrl+F10 settings";
            draw_text(dc, 18, 68, line.c_str(), 9, gray, false);

            std::wstring line2 = L"Right click flashlight  |  G beam size  |  F7 fuel roleplay";
            draw_text(dc, 18, 92, line2.c_str(), 9, gray, false);

            if (state.paused) {
                draw_text(dc, 18, 118, L"WALK INPUT PAUSED (console mode)", 9, gold, true);
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
    send_key(target, down);
    state = down;
}

void release_walk_keys() {
    if (g_sentForward) send_key(VK_NUMPAD8, false);
    if (g_sentBackward) send_key(VK_NUMPAD5, false);
    if (g_sentLeft) send_key(VK_NUMPAD4, false);
    if (g_sentRight) send_key(VK_NUMPAD6, false);
    if (g_sentEyeUp) send_key(VK_NUMPAD9, false);
    if (g_sentEyeDown) send_key(VK_NUMPAD3, false);

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

    transition_fade(true);

    // ETS2/ATS developer camera is normally opened with the top-row 0 key.
    tap_key('0');
    std::this_thread::sleep_for(220ms);

    if (g_settings.autoDoorOffset) {
        // Small camera-zero offset from the driver's eye toward the door.
        // It is intentionally configurable and does not patch game memory.
        hold_key_for(VK_NUMPAD4, 180ms);
        hold_key_for(VK_NUMPAD5, 70ms);
        hold_key_for(VK_NUMPAD3, 80ms);
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_ui.walking = true;
        g_ui.paused = false;
        g_ui.fuelStage = 0;
        g_ui.fueling = false;
        g_ui.status = L"WALK MODE";
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
    tap_key('0');

    if (flashlightWasOn) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_ui.flashlight = false;
    }

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

    auto lastReloadCheck = Clock::now();
    auto nextStep = Clock::now();
    auto nextBreath = Clock::now();
    Clock::time_point sprintStarted{};
    int bobPhase = 1;

    while (!g_stop.load()) {
        pump_messages();

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
            mirror_key(g_settings.forwardKey, VK_NUMPAD8, g_sentForward);
            mirror_key(g_settings.backwardKey, VK_NUMPAD5, g_sentBackward);
            mirror_key(g_settings.leftKey, VK_NUMPAD4, g_sentLeft);
            mirror_key(g_settings.rightKey, VK_NUMPAD6, g_sentRight);
            mirror_key(g_settings.eyeUpKey, VK_NUMPAD9, g_sentEyeUp);
            mirror_key(g_settings.eyeDownKey, VK_NUMPAD3, g_sentEyeDown);

            const bool sprintNow = key_down(g_settings.sprintKey);
            if (sprintEdge.pressed(sprintNow)) {
                send_wheel(g_settings.sprintWheelNotches);
                set_ui_status(L"RUN");
            }
            if (!sprintNow && sprintEdge.previous) {
                // This branch is intentionally unreachable because Edge::pressed updates previous.
                // Sprint release is handled below by a dedicated static state.
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
                    play_audio(sprintNow ? L"runstep.wav" : L"footstep.wav");

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
                    if (g_settings.breathingSound && runningFor >= g_settings.tiredAfterSeconds && motionNow >= nextBreath) {
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
                // Conservative reset: a short raise after crouch/eye adjustments.
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

            if (fuelStage > 0) {
                handle_fuel_interaction(interactNow, interactPressed);
            } else if (interactPressed) {
                leave_walk_mode();
            }
        } else {
            release_walk_keys();
        }

        if (Clock::now() - lastReloadCheck > 1s) {
            reload_settings_if_changed();
            lastReloadCheck = Clock::now();
        }

        std::this_thread::sleep_for(8ms);
    }

    release_walk_keys();
    send_key(VK_RETURN, false);
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
    ensure_audio_assets();

    native_game_bridge_initialize();
    log_line(native_game_bridge_report());

    g_stop = false;
    g_worker = std::thread(worker_main);
    log_line(L"TSRealDriver 0.3 initialized.");
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

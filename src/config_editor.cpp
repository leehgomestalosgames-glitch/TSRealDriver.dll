#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr COLORREF BG = RGB(12, 13, 17);
constexpr COLORREF PANEL = RGB(18, 19, 25);
constexpr COLORREF PANEL2 = RGB(27, 29, 38);
constexpr COLORREF TEXT = RGB(224, 225, 231);
constexpr COLORREF MUTED = RGB(139, 142, 154);
constexpr COLORREF ACCENT = RGB(78, 214, 224);
constexpr COLORREF ACCENT2 = RGB(224, 83, 189);
constexpr COLORREF TRACK = RGB(55, 58, 70);
constexpr COLORREF WHITE = RGB(248, 248, 245);

enum class ItemType { Slider, Toggle, Key };

struct Item {
    std::wstring label;
    std::wstring section;
    std::wstring key;
    ItemType type = ItemType::Slider;
    double minValue = 0.0;
    double maxValue = 1.0;
    double step = 0.1;
    double value = 0.0;
};

struct Tab {
    std::wstring name;
    std::vector<Item> items;
};

std::filesystem::path g_ini;
std::vector<Tab> g_tabs;
int g_tab = 0;
int g_dragItem = -1;
int g_waitingKeyItem = -1;
HWND g_hwnd = nullptr;

HFONT make_font(HDC dc, int pt, int weight = FW_NORMAL) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(pt, GetDeviceCaps(dc, LOGPIXELSY), 72);
    lf.lfWeight = weight;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

void text(HDC dc, int x, int y, const std::wstring& s, int pt, COLORREF c, int weight = FW_NORMAL) {
    HFONT font = make_font(dc, pt, weight);
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    TextOutW(dc, x, y, s.c_str(), static_cast<int>(s.size()));
    SelectObject(dc, old);
    DeleteObject(font);
}

std::filesystem::path executable_directory() {
    wchar_t path[32768]{};
    const DWORD size = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(_countof(path)));
    if (size == 0 || size >= _countof(path)) return {};
    return std::filesystem::path(std::wstring(path, size)).parent_path();
}

std::wstring read_ini(const std::wstring& section, const std::wstring& key, const std::wstring& fallback) {
    wchar_t buffer[256]{};
    GetPrivateProfileStringW(section.c_str(), key.c_str(), fallback.c_str(), buffer, _countof(buffer), g_ini.c_str());
    return buffer;
}

double read_number(const Item& item) {
    const std::wstring raw = read_ini(item.section, item.key, L"");
    if (raw.empty()) return item.value;
    wchar_t* end = nullptr;
    const double v = wcstod(raw.c_str(), &end);
    return (end && end != raw.c_str()) ? v : item.value;
}

std::wstring number_text(const Item& item) {
    if (item.type == ItemType::Key) {
        const int vk = static_cast<int>(std::llround(item.value));
        if (vk >= 'A' && vk <= 'Z') return std::wstring(1, static_cast<wchar_t>(vk));
        if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
        switch (vk) {
        case VK_SHIFT: return L"Shift";
        case VK_CONTROL: return L"Ctrl";
        case VK_SPACE: return L"Space";
        case VK_RBUTTON: return L"Right click";
        case VK_LBUTTON: return L"Left click";
        case VK_MBUTTON: return L"Middle click";
        case VK_OEM_3: return L"~";
        case VK_RETURN: return L"Enter";
        default: {
            wchar_t b[32]{};
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"0x%02X", vk);
            return b;
        }
        }
    }

    if (item.type == ItemType::Toggle) return item.value >= 0.5 ? L"ON" : L"OFF";

    const double rounded = std::round(item.value);
    wchar_t b[64]{};
    if (std::abs(item.value - rounded) < 0.0001) {
        _snwprintf_s(b, _countof(b), _TRUNCATE, L"%.0f", item.value);
    } else if (std::abs(item.step) < 0.01) {
        _snwprintf_s(b, _countof(b), _TRUNCATE, L"%.3f", item.value);
    } else {
        _snwprintf_s(b, _countof(b), _TRUNCATE, L"%.2f", item.value);
    }
    return b;
}

Item slider(const wchar_t* label, const wchar_t* section, const wchar_t* key,
            double value, double minValue, double maxValue, double step) {
    Item i;
    i.label = label; i.section = section; i.key = key; i.type = ItemType::Slider;
    i.value = value; i.minValue = minValue; i.maxValue = maxValue; i.step = step;
    return i;
}

Item toggle(const wchar_t* label, const wchar_t* section, const wchar_t* key, bool value) {
    Item i;
    i.label = label; i.section = section; i.key = key; i.type = ItemType::Toggle;
    i.value = value ? 1.0 : 0.0; i.minValue = 0.0; i.maxValue = 1.0; i.step = 1.0;
    return i;
}

Item key_item(const wchar_t* label, const wchar_t* key, int vk) {
    Item i;
    i.label = label; i.section = L"keys"; i.key = key; i.type = ItemType::Key;
    i.value = static_cast<double>(vk); i.minValue = 0; i.maxValue = 255; i.step = 1;
    return i;
}

void build_tabs() {
    g_tabs.clear();

    g_tabs.push_back({L"MOVEMENT", {
        slider(L"Walking speed (m/s)", L"movement", L"walk_speed", 1.75, 0.4, 6.0, 0.05),
        slider(L"Sprint speed (x walking)", L"movement", L"sprint_multiplier", 2.60, 1.0, 5.0, 0.05),
        slider(L"Backward speed factor", L"movement", L"backward_factor", 0.70, 0.2, 1.2, 0.05),
        slider(L"Strafe speed factor", L"movement", L"strafe_factor", 0.90, 0.2, 1.4, 0.05),
        slider(L"Start-up time (s)", L"movement", L"acceleration_time", 0.15, 0.0, 1.0, 0.01),
        slider(L"Stopping time (s)", L"movement", L"deceleration_time", 0.10, 0.0, 1.0, 0.01),
        slider(L"Standing eye height (m)", L"movement", L"eye_height", 1.70, 1.0, 2.2, 0.02),
        slider(L"Crouch eye height (m)", L"movement", L"crouch_eye_height", 1.00, 0.5, 1.6, 0.02),
        slider(L"Jump strength", L"movement", L"jump_strength", 4.80, 1.0, 8.0, 0.1),
        slider(L"Climb height (m)", L"movement", L"max_climb_height", 1.30, 0.2, 2.0, 0.05),
        slider(L"Air control", L"movement", L"air_control", 0.25, 0.0, 1.0, 0.05),
        toggle(L"Show on-screen [F] prompts", L"movement", L"show_prompts", true),
        toggle(L"Only exit when looking toward door", L"movement", L"exit_requires_look", false),
    }});

    g_tabs.push_back({L"WALK STYLE", {
        toggle(L"Head bob", L"movement", L"head_bob", true),
        slider(L"Head bob amount", L"movement", L"bob_amount", 0.004, 0.0, 0.03, 0.001),
        slider(L"Side sway amount", L"movement", L"sway_amount", 0.003, 0.0, 0.03, 0.001),
        slider(L"Step length (m)", L"movement", L"step_length", 0.93, 0.3, 1.8, 0.01),
        slider(L"Running stride multiplier", L"movement", L"run_stride", 0.75, 0.2, 1.5, 0.05),
        slider(L"Run head dip", L"movement", L"run_head_dip", 0.60, 0.0, 1.5, 0.05),
        slider(L"Run extra sway", L"movement", L"run_sway", 0.30, 0.0, 1.0, 0.05),
        slider(L"Running surge (m)", L"movement", L"run_surge", 0.014, 0.0, 0.05, 0.001),
        toggle(L"Breathing movement", L"movement", L"breathing_motion", true),
        slider(L"Landing dip", L"movement", L"landing_dip", 0.035, 0.0, 0.12, 0.005),
        toggle(L"Hide driver model while walking", L"movement", L"hide_driver", true),
        toggle(L"Fade on enter / exit", L"movement", L"fade_enabled", true),
    }});

    g_tabs.push_back({L"CAMERA", {
        slider(L"Zoom", L"camera", L"zoom_factor", 0.62, 0.25, 1.2, 0.01),
        slider(L"Mouse sensitivity", L"camera", L"mouse_sensitivity", 0.0018, 0.0005, 0.0060, 0.0001),
        slider(L"Exit angle", L"camera", L"exit_angle", -22, -90, 90, 1),
        slider(L"Enter-cabin prompt range (m)", L"camera", L"enter_range_m", 1.2, 0.4, 3.0, 0.1),
        slider(L"Building hop distance (m)", L"camera", L"building_hop_m", 1.6, 0.3, 4.0, 0.1),
        slider(L"Black screen minimum (ms)", L"camera", L"fade_min_ms", 900, 0, 4000, 50),
        slider(L"Black screen maximum (ms)", L"camera", L"fade_max_ms", 3500, 0, 6000, 50),
        toggle(L"Automatic driver-door offset", L"camera", L"auto_door_offset", true),
        toggle(L"Use native mouse look", L"camera", L"native_mouse_look", true),
        toggle(L"Follow ground away from truck", L"collision", L"far_ground_estimate", true),
        toggle(L"Buildings and fences block movement", L"collision", L"world_collision", false),
        toggle(L"Invert left-right", L"camera", L"invert_x", false),
        toggle(L"Invert up-down", L"camera", L"invert_y", false),
    }});

    g_tabs.push_back({L"FLASHLIGHT", {
        toggle(L"Flashlight enabled", L"flashlight", L"enabled", true),
        slider(L"Beam size (degrees)", L"flashlight", L"beam_angle", 28, 8, 90, 1),
        slider(L"Beam range (m)", L"flashlight", L"range_m", 40, 5, 120, 1),
        slider(L"Brightness", L"flashlight", L"brightness", 1.0, 0.1, 2.5, 0.05),
        slider(L"Edge softness", L"flashlight", L"softness", 0.75, 0.0, 1.0, 0.05),
        slider(L"Beam fade time (s)", L"flashlight", L"fade_time", 0.18, 0.0, 1.0, 0.01),
        toggle(L"Show hand holding flashlight", L"flashlight", L"hands", true),
        toggle(L"Use TSRealDriver light overlay", L"flashlight", L"own_light", true),
    }});

    g_tabs.push_back({L"SOUND", {
        toggle(L"Footstep / door / flashlight sounds", L"sound", L"enabled", true),
        toggle(L"Breathing sounds", L"sound", L"breathing", true),
        slider(L"Master volume", L"sound", L"master_volume", 0.70, 0.0, 1.0, 0.05),
        slider(L"Footstep volume", L"sound", L"footstep_volume", 0.75, 0.0, 1.0, 0.05),
        slider(L"Breathing volume", L"sound", L"breathing_volume", 0.45, 0.0, 1.0, 0.05),
        slider(L"Seconds running before tired breath", L"sound", L"tired_after_s", 18, 3, 60, 1),
    }});

    g_tabs.push_back({L"SHADOW", {
        toggle(L"Driver ground shadow", L"shadow", L"enabled", true),
        slider(L"Darkness", L"shadow", L"strength", 0.80, 0.0, 1.0, 0.05),
        slider(L"Edge softness", L"shadow", L"softness", 4.0, 0.0, 8.0, 0.25),
        slider(L"Size", L"shadow", L"size", 1.40, 0.4, 3.0, 0.05),
        slider(L"Length", L"shadow", L"length", 2.0, 0.5, 5.0, 0.1),
        slider(L"Fade near truck (m)", L"shadow", L"truck_fade_m", 2.0, 0.0, 8.0, 0.1),
    }});

    g_tabs.push_back({L"FUEL", {
        toggle(L"Fuel interaction enabled", L"fuel", L"enabled", true),
        toggle(L"Pay with TS Fleet card first", L"fuel", L"card_step", true),
        toggle(L"Receipt after fuelling", L"fuel", L"receipt", true),
        toggle(L"Show hand animations", L"fuel", L"hands", true),
        toggle(L"Remember tank position per truck", L"fuel", L"remember_tank", true),
        slider(L"Pump interaction radius (m)", L"fuel", L"pump_radius_m", 4.0, 1.0, 12.0, 0.1),
        slider(L"Tank interaction radius (m)", L"fuel", L"tank_radius_m", 1.3, 0.4, 4.0, 0.1),
        slider(L"Tank length (m)", L"fuel", L"tank_length_m", 1.8, 0.5, 4.0, 0.1),
        slider(L"Learned-spot tolerance (m)", L"fuel", L"learned_spot_m", 1.1, 0.3, 3.0, 0.1),
    }});

    g_tabs.push_back({L"KEYS", {
        key_item(L"Leave / enter truck", L"toggle", VK_F10),
        key_item(L"Move forward", L"forward", 'W'),
        key_item(L"Move backward", L"back", 'S'),
        key_item(L"Move left", L"left", 'A'),
        key_item(L"Move right", L"right", 'D'),
        key_item(L"Sprint", L"sprint", VK_SHIFT),
        key_item(L"Crouch", L"crouch", VK_CONTROL),
        key_item(L"Jump / climb", L"jump", VK_SPACE),
        key_item(L"Lower eye", L"eye_down", 'Q'),
        key_item(L"Raise eye", L"eye_up", 'E'),
        key_item(L"Reset eye height", L"eye_reset", 'R'),
        key_item(L"Interact / enter cabin", L"interact", 'F'),
        key_item(L"Building / ghost walk", L"building", VK_F8),
        key_item(L"Flashlight on / off", L"flashlight", VK_RBUTTON),
        key_item(L"Change beam size", L"flashlight_size", 'G'),
        key_item(L"Fuel roleplay", L"fuel_mode", VK_F7),
        key_item(L"Console pause", L"console", VK_OEM_3),
    }});

    for (auto& tab : g_tabs) {
        for (auto& item : tab.items) {
            item.value = read_number(item);
            item.value = std::clamp(item.value, item.minValue, item.maxValue);
        }
    }
}

void save_all() {
    for (const auto& tab : g_tabs) {
        for (const auto& item : tab.items) {
            wchar_t value[64]{};
            if (item.type == ItemType::Toggle || item.type == ItemType::Key) {
                _snwprintf_s(value, _countof(value), _TRUNCATE, L"%d", static_cast<int>(std::llround(item.value)));
            } else {
                _snwprintf_s(value, _countof(value), _TRUNCATE, L"%.6f", item.value);
            }
            WritePrivateProfileStringW(item.section.c_str(), item.key.c_str(), value, g_ini.c_str());
        }
    }
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

void apply_preset(int preset) {
    auto set = [](const wchar_t* tabName, const wchar_t* key, double value) {
        for (auto& tab : g_tabs) {
            if (tab.name != tabName) continue;
            for (auto& item : tab.items) {
                if (item.key == key) {
                    item.value = std::clamp(value, item.minValue, item.maxValue);
                    return;
                }
            }
        }
    };

    if (preset == 0) { // Realistic
        set(L"MOVEMENT", L"walk_speed", 1.55);
        set(L"MOVEMENT", L"sprint_multiplier", 2.2);
        set(L"WALK STYLE", L"bob_amount", 0.004);
        set(L"WALK STYLE", L"sway_amount", 0.003);
        set(L"CAMERA", L"zoom_factor", 0.62);
        set(L"FLASHLIGHT", L"brightness", 1.0);
    } else if (preset == 1) { // Light
        set(L"MOVEMENT", L"walk_speed", 1.75);
        set(L"MOVEMENT", L"sprint_multiplier", 2.4);
        set(L"WALK STYLE", L"bob_amount", 0.002);
        set(L"WALK STYLE", L"sway_amount", 0.001);
        set(L"FLASHLIGHT", L"brightness", 0.85);
    } else if (preset == 2) { // Dramatic
        set(L"MOVEMENT", L"walk_speed", 1.85);
        set(L"MOVEMENT", L"sprint_multiplier", 2.8);
        set(L"WALK STYLE", L"bob_amount", 0.010);
        set(L"WALK STYLE", L"sway_amount", 0.008);
        set(L"FLASHLIGHT", L"brightness", 1.35);
    } else { // Relaxed
        set(L"MOVEMENT", L"walk_speed", 1.25);
        set(L"MOVEMENT", L"sprint_multiplier", 1.8);
        set(L"WALK STYLE", L"bob_amount", 0.001);
        set(L"WALK STYLE", L"sway_amount", 0.001);
        set(L"FLASHLIGHT", L"brightness", 0.75);
    }

    save_all();
}

RECT tab_rect(int index) {
    const int x0 = 42;
    const int top = 98;
    const int width = 142;
    return RECT{x0 + index * width, top, x0 + (index + 1) * width - 6, top + 48};
}

RECT row_rect(int index) {
    const int top = 166;
    const int height = 43;
    return RECT{32, top + index * height, 1202, top + (index + 1) * height - 2};
}

RECT slider_rect(int index) {
    RECT r = row_rect(index);
    return RECT{880, r.top + 19, 1178, r.top + 24};
}

RECT toggle_rect(int index) {
    RECT r = row_rect(index);
    return RECT{1128, r.top + 8, 1186, r.top + 34};
}

RECT key_rect(int index) {
    RECT r = row_rect(index);
    return RECT{1015, r.top + 6, 1186, r.top + 36};
}

void fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void rounded(HDC dc, const RECT& r, COLORREF fillColor, COLORREF borderColor, int radius = 12) {
    HBRUSH b = CreateSolidBrush(fillColor);
    HPEN p = CreatePen(PS_SOLID, 1, borderColor);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
    DeleteObject(b);
}

void paint(HDC dc, const RECT& client) {
    fill(dc, client, BG);

    RECT header{0, 0, client.right, 94};
    fill(dc, header, PANEL);
    text(dc, 38, 18, L"TS REAL DRIVER", 20, ACCENT, FW_BOLD);
    text(dc, 38, 54, L"Walking, camera, interaction and simulation settings", 10, MUTED);
    text(dc, client.right - 208, 25, L"v0.3 BETA", 9, MUTED, FW_BOLD);

    for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i) {
        const RECT tr = tab_rect(i);
        if (i == g_tab) {
            text(dc, tr.left + 8, tr.top + 13, g_tabs[i].name, 9, WHITE, FW_BOLD);
            RECT underline{tr.left + 4, tr.bottom - 3, tr.right - 4, tr.bottom};
            fill(dc, underline, ACCENT);
        } else {
            text(dc, tr.left + 8, tr.top + 13, g_tabs[i].name, 9, MUTED);
        }
    }

    const auto& tab = g_tabs[g_tab];
    for (int i = 0; i < static_cast<int>(tab.items.size()); ++i) {
        const auto& item = tab.items[i];
        RECT rr = row_rect(i);
        if ((i % 2) == 1) fill(dc, rr, RGB(14, 15, 20));

        text(dc, 44, rr.top + 12, item.label, 10, TEXT);

        if (item.type == ItemType::Slider) {
            const RECT sr = slider_rect(i);
            fill(dc, sr, TRACK);

            const double norm = (item.value - item.minValue) / (item.maxValue - item.minValue);
            RECT active{sr.left, sr.top, sr.left + static_cast<int>((sr.right - sr.left) * norm), sr.bottom};
            fill(dc, active, ACCENT);

            const int knobX = active.right;
            HBRUSH knob = CreateSolidBrush(WHITE);
            HGDIOBJ old = SelectObject(dc, knob);
            Ellipse(dc, knobX - 8, sr.top - 6, knobX + 8, sr.bottom + 6);
            SelectObject(dc, old);
            DeleteObject(knob);

            const std::wstring v = number_text(item);
            text(dc, 806, rr.top + 10, v, 10, ACCENT, FW_BOLD);
        } else if (item.type == ItemType::Toggle) {
            const RECT tr = toggle_rect(i);
            rounded(dc, tr, item.value >= 0.5 ? ACCENT : PANEL2, item.value >= 0.5 ? ACCENT : TRACK, 24);

            const int cy = (tr.top + tr.bottom) / 2;
            const int cx = item.value >= 0.5 ? tr.right - 14 : tr.left + 14;
            HBRUSH kb = CreateSolidBrush(item.value >= 0.5 ? RGB(8, 24, 28) : MUTED);
            HGDIOBJ old = SelectObject(dc, kb);
            Ellipse(dc, cx - 9, cy - 9, cx + 9, cy + 9);
            SelectObject(dc, old);
            DeleteObject(kb);
        } else {
            const RECT kr = key_rect(i);
            rounded(dc, kr,
                (g_waitingKeyItem == i ? RGB(33, 67, 72) : PANEL2),
                (g_waitingKeyItem == i ? ACCENT : TRACK), 10);
            const std::wstring v = g_waitingKeyItem == i ? L"press a key..." : number_text(item);
            text(dc, kr.left + 18, kr.top + 8, v, 9, g_waitingKeyItem == i ? ACCENT : WHITE, FW_BOLD);
        }
    }

    const int bottom = client.bottom - 88;
    text(dc, 38, bottom, L"PRESETS", 9, MUTED, FW_BOLD);

    const wchar_t* presetNames[] = {L"Realistic", L"Light", L"Dramatic", L"Relaxed"};
    for (int i = 0; i < 4; ++i) {
        RECT pr{38 + i * 138, bottom + 28, 162 + i * 138, bottom + 66};
        rounded(dc, pr, PANEL, TRACK, 18);
        text(dc, pr.left + 24, pr.top + 10, presetNames[i], 9, TEXT);
    }

    RECT save{client.right - 304, bottom + 28, client.right - 174, bottom + 66};
    RECT reload{client.right - 164, bottom + 28, client.right - 34, bottom + 66};
    rounded(dc, save, RGB(22, 67, 72), ACCENT, 18);
    rounded(dc, reload, PANEL, TRACK, 18);
    text(dc, save.left + 43, save.top + 10, L"SAVE", 9, WHITE, FW_BOLD);
    text(dc, reload.left + 33, reload.top + 10, L"RELOAD", 9, TEXT, FW_BOLD);

    text(dc, 38, client.bottom - 18, L"Changes are hot-reloaded by TSRealDriver.dll after Save.", 8, MUTED);
}

int hit_tab(POINT p) {
    for (int i = 0; i < static_cast<int>(g_tabs.size()); ++i) {
        RECT r = tab_rect(i);
        if (PtInRect(&r, p)) return i;
    }
    return -1;
}

int hit_item(POINT p, ItemType type) {
    const auto& items = g_tabs[g_tab].items;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        if (items[i].type != type) continue;
        RECT r = type == ItemType::Slider ? slider_rect(i) :
                 type == ItemType::Toggle ? toggle_rect(i) : key_rect(i);
        if (PtInRect(&r, p)) return i;
    }
    return -1;
}

void update_slider_from_x(int index, int x) {
    auto& item = g_tabs[g_tab].items[index];
    RECT sr = slider_rect(index);
    const double norm = std::clamp((x - sr.left) / static_cast<double>(sr.right - sr.left), 0.0, 1.0);
    double v = item.minValue + norm * (item.maxValue - item.minValue);
    if (item.step > 0.0) v = std::round(v / item.step) * item.step;
    item.value = std::clamp(v, item.minValue, item.maxValue);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void resize_to_work_area(HWND hwnd) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = std::min(1240, static_cast<int>(work.right - work.left - 40));
    const int h = std::min(880, static_cast<int>(work.bottom - work.top - 40));
    const int x = work.left + (work.right - work.left - w) / 2;
    const int y = work.top + (work.bottom - work.top - h) / 2;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_hwnd = hwnd;
        build_tabs();
        resize_to_work_area(hwnd);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        paint(dc, client);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        if (const int t = hit_tab(p); t >= 0) {
            g_tab = t;
            g_dragItem = -1;
            g_waitingKeyItem = -1;
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        if (const int i = hit_item(p, ItemType::Slider); i >= 0) {
            g_dragItem = i;
            SetCapture(hwnd);
            update_slider_from_x(i, p.x);
            return 0;
        }

        if (const int i = hit_item(p, ItemType::Toggle); i >= 0) {
            auto& item = g_tabs[g_tab].items[i];
            item.value = item.value >= 0.5 ? 0.0 : 1.0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (const int i = hit_item(p, ItemType::Key); i >= 0) {
            g_waitingKeyItem = i;
            SetFocus(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        RECT client{};
        GetClientRect(hwnd, &client);
        const int bottom = client.bottom - 88;

        for (int i = 0; i < 4; ++i) {
            RECT pr{38 + i * 138, bottom + 28, 162 + i * 138, bottom + 66};
            if (PtInRect(&pr, p)) {
                apply_preset(i);
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
        }

        RECT save{client.right - 304, bottom + 28, client.right - 174, bottom + 66};
        RECT reload{client.right - 164, bottom + 28, client.right - 34, bottom + 66};

        if (PtInRect(&save, p)) {
            save_all();
            MessageBeep(MB_OK);
            return 0;
        }

        if (PtInRect(&reload, p)) {
            build_tabs();
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
        if (g_dragItem >= 0 && (wParam & MK_LBUTTON)) {
            update_slider_from_x(g_dragItem, GET_X_LPARAM(lParam));
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (g_dragItem >= 0) {
            g_dragItem = -1;
            ReleaseCapture();
            return 0;
        }
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_waitingKeyItem >= 0 && g_tab >= 0 && g_tab < static_cast<int>(g_tabs.size())) {
            auto& items = g_tabs[g_tab].items;
            if (g_waitingKeyItem < static_cast<int>(items.size()) &&
                items[g_waitingKeyItem].type == ItemType::Key) {
                if (wParam == VK_ESCAPE) {
                    g_waitingKeyItem = -1;
                } else {
                    items[g_waitingKeyItem].value = static_cast<double>(wParam);
                    g_waitingKeyItem = -1;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g_ini = executable_directory() / L"TSRealDriver.ini";

    const wchar_t* className = L"TSRealDriverSettingsPanel";

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(BG);

    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        className,
        L"TSRealDriver Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 1240, 880,
        nullptr, nullptr, instance, nullptr);

    if (!hwnd) return 2;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

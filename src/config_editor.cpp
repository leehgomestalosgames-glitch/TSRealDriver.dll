#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {
constexpr int ID_EDITOR = 1001;
constexpr int ID_SAVE = 1002;
constexpr int ID_RELOAD = 1003;
constexpr int ID_FOLDER = 1004;

HWND g_editor = nullptr;
std::filesystem::path g_ini;

std::wstring utf8_to_wide(const std::string& input) {
    if (input.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), out.data(), count);
    return out;
}

std::string wide_to_utf8(const std::wstring& input) {
    if (input.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string out(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), out.data(), count, nullptr, nullptr);
    return out;
}

std::filesystem::path executable_directory() {
    wchar_t path[32768]{};
    const DWORD size = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (size == 0 || size >= std::size(path)) return {};
    return std::filesystem::path(std::wstring(path, size)).parent_path();
}

void load_ini(HWND owner) {
    std::ifstream in(g_ini, std::ios::binary);
    if (!in) {
        SetWindowTextW(g_editor, L"; TSRealDriver.ini not found next to this editor.\r\n");
        MessageBoxW(owner, L"TSRealDriver.ini was not found.", L"TSRealDriver", MB_ICONWARNING);
        return;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    std::wstring text = utf8_to_wide(ss.str());

    std::wstring crlf;
    crlf.reserve(text.size() + 64);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) crlf += L'\r';
        crlf += text[i];
    }

    SetWindowTextW(g_editor, crlf.c_str());
}

void save_ini(HWND owner) {
    const int length = GetWindowTextLengthW(g_editor);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(g_editor, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));

    std::ofstream out(g_ini, std::ios::binary | std::ios::trunc);
    if (!out) {
        MessageBoxW(owner, L"Could not save TSRealDriver.ini.", L"TSRealDriver", MB_ICONERROR);
        return;
    }

    const std::string utf8 = wide_to_utf8(text);
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    out.flush();

    MessageBoxW(owner, L"Configuration saved.", L"TSRealDriver", MB_ICONINFORMATION);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_editor = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN,
            12, 12, 900, 575,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDITOR)),
            nullptr,
            nullptr);

        CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            12, 600, 110, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SAVE)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Reload", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            132, 600, 110, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RELOAD)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            252, 600, 130, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_FOLDER)), nullptr, nullptr);

        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SendMessageW(g_editor, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

        load_ini(hwnd);
        return 0;
    }
    case WM_SIZE: {
        const int width = LOWORD(lParam);
        const int height = HIWORD(lParam);
        if (g_editor) {
            MoveWindow(g_editor, 12, 12, width - 24, height - 82, TRUE);
            MoveWindow(GetDlgItem(hwnd, ID_SAVE), 12, height - 58, 110, 32, TRUE);
            MoveWindow(GetDlgItem(hwnd, ID_RELOAD), 132, height - 58, 110, 32, TRUE);
            MoveWindow(GetDlgItem(hwnd, ID_FOLDER), 252, height - 58, 130, 32, TRUE);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_SAVE:
            save_ini(hwnd);
            return 0;
        case ID_RELOAD:
            load_ini(hwnd);
            return 0;
        case ID_FOLDER:
            ShellExecuteW(hwnd, L"open", g_ini.parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        default:
            break;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g_ini = executable_directory() / L"TSRealDriver.ini";

    const wchar_t* class_name = L"TSRealDriverConfigEditor";

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc)) {
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        class_name,
        L"TSRealDriver - Configuration Editor",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        950, 700,
        nullptr, nullptr, instance, nullptr);

    if (!hwnd) {
        return 2;
    }

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

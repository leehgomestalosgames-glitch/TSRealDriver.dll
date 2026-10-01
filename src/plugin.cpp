#include <windows.h>
#include <filesystem>
#include <fstream>

namespace {
HMODULE g_module = nullptr;

std::filesystem::path module_directory() {
    wchar_t buffer[32768]{};
    const DWORD size = GetModuleFileNameW(g_module, buffer, static_cast<DWORD>(std::size(buffer)));
    if (size == 0 || size >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer, size)).parent_path();
}

void log_line(const wchar_t* text) {
    const auto dir = module_directory();
    if (dir.empty()) return;

    std::wofstream out(dir / L"TSRealDriver.log", std::ios::app);
    if (out) {
        out << text << L"\n";
    }
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) int scs_telemetry_init(unsigned int, const void*) {
    log_line(L"[TSRealDriver] plugin initialized");
    return 0;
}

extern "C" __declspec(dllexport) void scs_telemetry_shutdown() {
    log_line(L"[TSRealDriver] plugin shutdown");
}

#include "native_game_bridge.hpp"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <sstream>

namespace {

NativeGameBridgeStatus g_status{};

std::uint64_t fnv1a64(const unsigned char* data, std::size_t size) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint64_t>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::wstring lower_copy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

bool locate_text_section(HMODULE module, std::uintptr_t& base, std::size_t& size) {
    if (!module) return false;

    auto* image = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        char name[9]{};
        memcpy(name, section->Name, 8);
        if (std::string(name) == ".text") {
            base = reinterpret_cast<std::uintptr_t>(image) + section->VirtualAddress;
            size = static_cast<std::size_t>(section->Misc.VirtualSize);
            return size > 0;
        }
    }
    return false;
}

}

bool native_game_bridge_initialize() {
    g_status = {};

    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) return false;

    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(_countof(path)));
    if (length == 0 || length >= _countof(path)) return false;

    g_status.executableName = std::filesystem::path(std::wstring(path, length)).filename().wstring();
    const auto lower = lower_copy(g_status.executableName);
    g_status.supportedGame =
        lower == L"eurotrucks2.exe" ||
        lower == L"amtrucks.exe";

    g_status.moduleBase = reinterpret_cast<std::uintptr_t>(exe);
    g_status.textSectionFound = locate_text_section(exe, g_status.textBase, g_status.textSize);

    if (g_status.textSectionFound) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(g_status.textBase);
        g_status.textFingerprint = fnv1a64(bytes, g_status.textSize);
    }

    g_status.d3d11Loaded = GetModuleHandleW(L"d3d11.dll") != nullptr;
    g_status.initialized = true;
    return true;
}

void native_game_bridge_shutdown() {
    g_status = {};
}

const NativeGameBridgeStatus& native_game_bridge_status() {
    return g_status;
}

std::uintptr_t native_game_find_pattern(const int* pattern, std::size_t length) {
    if (!g_status.textSectionFound || !pattern || length == 0 || length > g_status.textSize) {
        return 0;
    }

    const auto* data = reinterpret_cast<const unsigned char*>(g_status.textBase);
    const std::size_t end = g_status.textSize - length;

    for (std::size_t i = 0; i <= end; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < length; ++j) {
            if (pattern[j] >= 0 && data[i + j] != static_cast<unsigned char>(pattern[j])) {
                match = false;
                break;
            }
        }
        if (match) return g_status.textBase + i;
    }

    return 0;
}

std::wstring native_game_bridge_report() {
    std::wostringstream out;
    out << L"Native bridge: " << (g_status.initialized ? L"initialized" : L"not initialized")
        << L", exe=" << (g_status.executableName.empty() ? L"<unknown>" : g_status.executableName)
        << L", game=" << (g_status.supportedGame ? L"ETS2/ATS" : L"unsupported")
        << L", .text=" << (g_status.textSectionFound ? L"yes" : L"no")
        << L", d3d11=" << (g_status.d3d11Loaded ? L"loaded" : L"not loaded");

    if (g_status.textSectionFound) {
        out << L", fingerprint=0x" << std::hex << g_status.textFingerprint
            << L", text_size=0x" << g_status.textSize;
    }

    return out.str();
}

#include "game_camera_bridge.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

GameCameraBridgeStatus g_status{};

struct SectionView {
    std::string name;
    std::uintptr_t base = 0;
    std::size_t size = 0;
    DWORD characteristics = 0;
};

bool safe_read(std::uintptr_t address, void* out, std::size_t size) {
    if (!address || !out || size == 0) return false;
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<LPCVOID>(address),
                             out,
                             size,
                             &read) &&
           read == size;
}

template <typename T>
bool safe_read_value(std::uintptr_t address, T& out) {
    return safe_read(address, &out, sizeof(T));
}

bool safe_write(std::uintptr_t address, const void* data, std::size_t size) {
    if (!address || !data || size == 0) return false;
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(),
                              reinterpret_cast<LPVOID>(address),
                              data,
                              size,
                              &written) &&
           written == size;
}

bool is_readable(std::uintptr_t address, std::size_t size = 1) {
    if (!address || size == 0) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }

    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;

    const std::uintptr_t regionStart = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const std::uintptr_t regionEnd = regionStart + mbi.RegionSize;
    return address >= regionStart && address + size <= regionEnd;
}

bool is_executable(std::uintptr_t address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }

    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;

    const DWORD p = mbi.Protect & 0xff;
    return p == PAGE_EXECUTE ||
           p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE ||
           p == PAGE_EXECUTE_WRITECOPY;
}

bool module_sections(std::uintptr_t& moduleBase,
                     std::vector<SectionView>& sections,
                     std::size_t& imageSize) {
    moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!moduleBase) return false;

    IMAGE_DOS_HEADER dos{};
    if (!safe_read_value(moduleBase, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    IMAGE_NT_HEADERS64 nt{};
    if (!safe_read(moduleBase + static_cast<std::uintptr_t>(dos.e_lfanew),
                   &nt,
                   sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    imageSize = nt.OptionalHeader.SizeOfImage;

    const std::uintptr_t sectionTable =
        moduleBase + static_cast<std::uintptr_t>(dos.e_lfanew) +
        offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
        nt.FileHeader.SizeOfOptionalHeader;

    sections.clear();
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER sh{};
        if (!safe_read(sectionTable + i * sizeof(sh), &sh, sizeof(sh))) return false;

        char name[9]{};
        std::memcpy(name, sh.Name, 8);

        SectionView s;
        s.name = name;
        s.base = moduleBase + sh.VirtualAddress;
        s.size = static_cast<std::size_t>(std::max(sh.Misc.VirtualSize, sh.SizeOfRawData));
        s.characteristics = sh.Characteristics;
        sections.push_back(std::move(s));
    }

    return true;
}

const SectionView* find_section(const std::vector<SectionView>& sections, const char* name) {
    for (const auto& s : sections) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

std::uintptr_t find_bounded_ascii(const SectionView& section, const char* needle) {
    const std::size_t len = std::strlen(needle);
    if (len == 0 || section.size < len + 2) return 0;

    std::vector<unsigned char> bytes(section.size);
    if (!safe_read(section.base, bytes.data(), bytes.size())) return 0;

    for (std::size_t i = 1; i + len < bytes.size(); ++i) {
        if (bytes[i - 1] != 0 || bytes[i + len] != 0) continue;
        if (std::memcmp(bytes.data() + i, needle, len) == 0) {
            return section.base + i;
        }
    }
    return 0;
}

std::string normalize_rtti_name(std::string name) {
    if (name.rfind(".?AV", 0) == 0 || name.rfind(".?AU", 0) == 0) {
        name.erase(0, 4);
    }

    const auto end = name.find("@@");
    if (end != std::string::npos) name.resize(end);

    // Namespace-decorated MSVC names keep '@'. The first component is the class.
    const auto at = name.find('@');
    if (at != std::string::npos) name.resize(at);

    return name;
}

bool read_ascii_string(std::uintptr_t address, std::string& out, std::size_t maxLen = 128) {
    out.clear();
    if (!address || !is_readable(address, 1)) return false;

    std::vector<char> buffer(maxLen, '\0');
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<LPCVOID>(address),
                           buffer.data(),
                           buffer.size() - 1,
                           &got) ||
        got == 0) {
        return false;
    }

    buffer.back() = '\0';
    const std::size_t len = strnlen(buffer.data(), buffer.size());
    if (len == 0 || len >= buffer.size()) return false;

    for (std::size_t i = 0; i < len; ++i) {
        const unsigned char ch = static_cast<unsigned char>(buffer[i]);
        if (ch < 0x20 || ch > 0x7e) return false;
    }

    out.assign(buffer.data(), len);
    return true;
}

bool plausible_camera_class_name(const std::string& raw, std::string& out) {
    if (raw.empty()) return false;

    std::string normalized = normalize_rtti_name(raw);
    if (normalized.empty()) return false;

    std::string lower = normalized;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (lower.find("camera") == std::string::npos) return false;

    out = std::move(normalized);
    return true;
}

bool class_name_from_candidate(std::uintptr_t candidate, std::string& out) {
    // SCS has used several tiny type-name thunks over time. Some return the
    // string directly, others return one or more descriptor/pointer hops.
    // Follow a short, bounded chain and also understand an MSVC TypeDescriptor.
    std::uintptr_t cursor = candidate;

    for (int depth = 0; depth < 4 && cursor; ++depth) {
        std::string raw;
        if (read_ascii_string(cursor, raw, 128) &&
            plausible_camera_class_name(raw, out)) {
            return true;
        }

        // MSVC TypeDescriptor on x64: two pointers followed by the decorated name.
        if (is_readable(cursor + 2 * sizeof(std::uintptr_t), 1) &&
            read_ascii_string(cursor + 2 * sizeof(std::uintptr_t), raw, 128) &&
            plausible_camera_class_name(raw, out)) {
            return true;
        }

        std::uintptr_t next = 0;
        if (!safe_read_value(cursor, next) ||
            !next ||
            next == cursor ||
            !is_readable(next, 1)) {
            break;
        }
        cursor = next;
    }

    return false;
}

bool class_name_from_msvc_rtti(std::uintptr_t vtable,
                               std::uintptr_t imageBase,
                               std::string& out) {
    if (!vtable || !imageBase || vtable < sizeof(std::uintptr_t)) return false;

    std::uintptr_t locatorAddress = 0;
    if (!safe_read_value(vtable - sizeof(std::uintptr_t), locatorAddress) ||
        !locatorAddress ||
        !is_readable(locatorAddress, 24)) {
        return false;
    }

    struct CompleteObjectLocator64 {
        std::uint32_t signature;
        std::uint32_t offset;
        std::uint32_t cdOffset;
        std::int32_t typeDescriptorRva;
        std::int32_t classDescriptorRva;
        std::int32_t selfRva;
    } locator{};

    if (!safe_read(locatorAddress, &locator, sizeof(locator))) return false;

    // MSVC x64 uses signature 1 with image-relative RTTI references.
    if (locator.signature != 1 || locator.typeDescriptorRva == 0) return false;

    const std::uintptr_t typeDescriptor =
        imageBase + static_cast<std::intptr_t>(locator.typeDescriptorRva);

    std::string raw;
    if (!read_ascii_string(typeDescriptor + 2 * sizeof(std::uintptr_t), raw, 128)) {
        return false;
    }

    return plausible_camera_class_name(raw, out);
}

bool object_class_name(std::uintptr_t object,
                       std::uintptr_t fallbackImageBase,
                       std::string& out) {
    out.clear();
    if (!object || !is_readable(object, sizeof(std::uintptr_t))) return false;

    std::uintptr_t vtable = 0;
    if (!safe_read_value(object, vtable) ||
        !vtable ||
        !is_readable(vtable + 0x28, sizeof(std::uintptr_t))) {
        return false;
    }

    std::uintptr_t typeNameRoutine = 0;
    if (safe_read_value(vtable + 0x28, typeNameRoutine) &&
        typeNameRoutine &&
        is_executable(typeNameRoutine)) {
        std::array<unsigned char, 32> code{};
        if (safe_read(typeNameRoutine, code.data(), code.size())) {
            // Accept the common RIP-relative LEA/MOV forms instead of requiring
            // one exact 8-byte thunk. Game updates can change instruction
            // scheduling without changing the underlying type-name object.
            for (std::size_t i = 0; i + 7 <= code.size(); ++i) {
                if ((code[i] == 0x48 || code[i] == 0x4c) &&
                    (code[i + 1] == 0x8d || code[i + 1] == 0x8b) &&
                    (code[i + 2] & 0xc7) == 0x05) {
                    std::int32_t rel = 0;
                    std::memcpy(&rel, code.data() + i + 3, sizeof(rel));

                    const std::uintptr_t target =
                        typeNameRoutine + i + 7 + static_cast<std::intptr_t>(rel);

                    if (class_name_from_candidate(target, out)) {
                        return true;
                    }
                }

                if (i + 10 <= code.size() &&
                    code[i] == 0x48 &&
                    code[i + 1] == 0xb8) {
                    std::uintptr_t immediate = 0;
                    std::memcpy(&immediate, code.data() + i + 2, sizeof(immediate));
                    if (class_name_from_candidate(immediate, out)) {
                        return true;
                    }
                }
            }
        }
    }

    // Fallback: read the compiler RTTI attached to the object's vtable. This is
    // independent of the exact implementation of the game's type-name vfunc.
    return class_name_from_msvc_rtti(vtable, fallbackImageBase, out);
}

bool refresh_manager_and_debug(std::uintptr_t moduleBase) {
    if (!g_status.managerSingletonAddress) return false;

    std::uintptr_t manager = 0;
    if (!safe_read_value(g_status.managerSingletonAddress, manager) ||
        !manager ||
        !is_readable(manager + 0x40, 8)) {
        g_status.error = L"camera manager singleton exists but manager object is not ready";
        g_status.managerObject = 0;
        return false;
    }

    g_status.managerObject = manager;

    std::uint32_t current = 0;
    safe_read_value(manager + 0x10, current);
    g_status.currentCameraSlot = static_cast<int>(current);

    std::uintptr_t slots = 0;
    std::uint64_t count = 0;
    if (!safe_read_value(manager + 0x38, slots) ||
        !safe_read_value(manager + 0x40, count) ||
        !slots ||
        count == 0 ||
        count > 64) {
        g_status.error = L"camera manager slot array is not readable";
        return false;
    }

    int foundSlot = -1;
    std::uintptr_t foundObject = 0;
    std::string foundClass;

    for (std::uint64_t i = 0; i < count; ++i) {
        std::uintptr_t object = 0;
        if (!safe_read_value(slots + i * sizeof(std::uintptr_t), object) || !object) continue;

        std::string className;
        if (!object_class_name(object, moduleBase, className)) continue;

        if (className == "debug_camera" ||
            className.find("debug_camera") != std::string::npos) {
            foundSlot = static_cast<int>(i);
            foundObject = object;
            foundClass = className;
            break;
        }
    }

    if (foundSlot < 0) {
        g_status.error = L"debug_camera slot was not found";
        return false;
    }

    g_status.debugCameraSlot = foundSlot;
    g_status.debugCameraObject = foundObject;
    g_status.debugCameraClass = std::move(foundClass);
    g_status.error.clear();
    return true;
}

}

bool game_camera_bridge_resolve() {
    g_status = {};

    std::uintptr_t moduleBase = 0;
    std::size_t imageSize = 0;
    std::vector<SectionView> sections;

    if (!module_sections(moduleBase, sections, imageSize)) {
        g_status.error = L"could not parse game executable sections";
        return false;
    }

    const SectionView* rdata = find_section(sections, ".rdata");
    const SectionView* dataSection = find_section(sections, ".data");
    if (!rdata || !dataSection) {
        g_status.error = L"game executable is missing .rdata or .data";
        return false;
    }

    const std::uintptr_t cameraManagerName =
        find_bounded_ascii(*rdata, "camera_manager");
    if (!cameraManagerName) {
        g_status.error = L"camera_manager type name was not found";
        return false;
    }

    std::vector<unsigned char> dataBytes(dataSection->size);
    if (!safe_read(dataSection->base, dataBytes.data(), dataBytes.size())) {
        g_status.error = L"could not scan the game's .data section";
        return false;
    }

    std::uintptr_t singletonStore = 0;
    int usableCreators = 0;

    for (std::size_t i = 0; i + 16 <= dataBytes.size(); i += sizeof(std::uintptr_t)) {
        std::uintptr_t namePtr = 0;
        std::memcpy(&namePtr, dataBytes.data() + i, sizeof(namePtr));
        if (namePtr != cameraManagerName) continue;

        const std::uintptr_t registration = dataSection->base + i;

        std::uintptr_t creatorHolder = 0;
        if (!safe_read_value(registration + 8, creatorHolder) || !creatorHolder) continue;

        std::uintptr_t creatorFunction = 0;
        if (!safe_read_value(creatorHolder + 8, creatorFunction) ||
            !creatorFunction ||
            !is_executable(creatorFunction)) {
            continue;
        }

        std::array<unsigned char, 512> code{};
        if (!safe_read(creatorFunction, code.data(), code.size())) continue;

        std::vector<std::uintptr_t> stores;
        for (std::size_t p = 0; p + 7 <= code.size(); ++p) {
            const unsigned char rex = code[p];
            if (rex != 0x48 && rex != 0x4c) continue;
            if (code[p + 1] != 0x89) continue;

            const unsigned char modrm = code[p + 2];
            if ((modrm & 0xc7) != 0x05) continue;

            std::int32_t rel = 0;
            std::memcpy(&rel, code.data() + p + 3, sizeof(rel));

            const std::uintptr_t target =
                creatorFunction + p + 7 + static_cast<std::intptr_t>(rel);

            if (target < moduleBase || target >= moduleBase + imageSize) continue;

            if (std::find(stores.begin(), stores.end(), target) == stores.end()) {
                stores.push_back(target);
            }
        }

        if (stores.size() == 1) {
            singletonStore = stores.front();
            ++usableCreators;
        }
    }

    if (usableCreators != 1 || !singletonStore) {
        std::wostringstream out;
        out << L"camera manager creator resolution produced " << usableCreators
            << L" usable singleton store(s)";
        g_status.error = out.str();
        return false;
    }

    g_status.managerSingletonAddress = singletonStore;

    if (!refresh_manager_and_debug(moduleBase)) {
        return false;
    }

    g_status.resolved = true;
    return true;
}

bool game_camera_bridge_refresh() {
    std::uintptr_t moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_status.managerSingletonAddress) {
        return game_camera_bridge_resolve();
    }

    const bool ok = refresh_manager_and_debug(moduleBase);
    g_status.resolved = ok;
    return ok;
}

bool game_camera_bridge_request_slot(int slot, unsigned timeoutMs) {
    if (slot < 0) return false;
    if (!game_camera_bridge_refresh()) return false;

    const std::uint32_t requested = static_cast<std::uint32_t>(slot);
    if (!safe_write(g_status.managerObject + 0x14, &requested, sizeof(requested))) {
        g_status.error = L"camera request write failed";
        return false;
    }

    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start <= timeoutMs) {
        std::uint32_t current = 0;
        if (safe_read_value(g_status.managerObject + 0x10, current)) {
            g_status.currentCameraSlot = static_cast<int>(current);
            if (current == requested) {
                g_status.error.clear();
                return true;
            }
        }
        Sleep(10);
    }

    g_status.error = L"camera did not switch to the requested slot in time";
    return false;
}

bool game_camera_bridge_request_debug(unsigned timeoutMs) {
    if (!game_camera_bridge_refresh()) return false;
    return game_camera_bridge_request_slot(g_status.debugCameraSlot, timeoutMs);
}

bool game_camera_bridge_set_debug_position(double worldX, double worldY, double worldZ) {
    if (!std::isfinite(worldX) || !std::isfinite(worldY) || !std::isfinite(worldZ)) {
        g_status.error = L"debug camera position is not finite";
        return false;
    }

    if (!game_camera_bridge_refresh() || !g_status.debugCameraObject) {
        if (g_status.error.empty()) g_status.error = L"debug camera object is not ready";
        return false;
    }

    // SCS stores camera world coordinates as local float X/Y/Z plus two
    // signed 16-bit 512 m sector coordinates. The 16-byte position block is
    // the first half of the camera pose at debug_camera + 0x40. Keep the
    // following quaternion untouched so native mouse look remains owned by
    // the game.
    constexpr double sectorSize = 512.0;
    const double sectorXd = std::floor(worldX / sectorSize);
    const double sectorZd = std::floor(worldZ / sectorSize);

    if (sectorXd < -32768.0 || sectorXd > 32767.0 ||
        sectorZd < -32768.0 || sectorZd > 32767.0) {
        g_status.error = L"debug camera world position is outside sector range";
        return false;
    }

#pragma pack(push, 1)
    struct CameraPositionBlock {
        float localX;
        float y;
        float localZ;
        std::int16_t sectorX;
        std::int16_t sectorZ;
    };
#pragma pack(pop)
    static_assert(sizeof(CameraPositionBlock) == 16);

    CameraPositionBlock pose{};
    pose.localX = static_cast<float>(worldX - sectorXd * sectorSize);
    pose.y = static_cast<float>(worldY);
    pose.localZ = static_cast<float>(worldZ - sectorZd * sectorSize);
    pose.sectorX = static_cast<std::int16_t>(sectorXd);
    pose.sectorZ = static_cast<std::int16_t>(sectorZd);

    if (!safe_write(g_status.debugCameraObject + 0x40, &pose, sizeof(pose))) {
        g_status.error = L"debug camera position write failed";
        return false;
    }

    g_status.error.clear();
    return true;
}

int game_camera_bridge_current_slot() {
    if (!game_camera_bridge_refresh()) return -1;
    return g_status.currentCameraSlot;
}

const GameCameraBridgeStatus& game_camera_bridge_status() {
    return g_status;
}

std::wstring game_camera_bridge_report() {
    std::wostringstream out;
    out << L"GameCameraBridge: " << (g_status.resolved ? L"resolved" : L"not resolved");

    if (g_status.managerSingletonAddress) {
        out << L", singleton=0x" << std::hex << g_status.managerSingletonAddress;
    }
    if (g_status.managerObject) {
        out << L", manager=0x" << std::hex << g_status.managerObject;
    }
    if (g_status.debugCameraSlot >= 0) {
        out << L", debug_slot=" << std::dec << g_status.debugCameraSlot;
    }
    if (g_status.currentCameraSlot >= 0) {
        out << L", current_slot=" << g_status.currentCameraSlot;
    }
    if (!g_status.error.empty()) {
        out << L", error=" << g_status.error;
    }

    return out.str();
}


std::wstring game_camera_bridge_census() {
    std::wostringstream out;

    // Census is diagnostic: it must still enumerate the manager when debug-camera
    // recognition failed. Do not make the report depend on refresh() succeeding.
    if (!g_status.managerSingletonAddress) {
        game_camera_bridge_resolve();
    }

    if (!g_status.managerSingletonAddress) {
        out << L"[camera] census unavailable: " << g_status.error;
        return out.str();
    }

    std::uintptr_t manager = 0;
    if (!safe_read_value(g_status.managerSingletonAddress, manager) ||
        !manager ||
        !is_readable(manager + 0x40, 8)) {
        out << L"[camera] census manager is not ready";
        return out.str();
    }

    g_status.managerObject = manager;

    std::uint32_t current = 0;
    safe_read_value(manager + 0x10, current);
    g_status.currentCameraSlot = static_cast<int>(current);

    std::uintptr_t slots = 0;
    std::uint64_t count = 0;
    if (!safe_read_value(manager + 0x38, slots) ||
        !safe_read_value(manager + 0x40, count) ||
        !slots ||
        count == 0 ||
        count > 64) {
        out << L"[camera] census invalid slot array";
        return out.str();
    }

    out << L"[camera] census: manager=0x" << std::hex << manager
        << L" current=" << std::dec << g_status.currentCameraSlot
        << L" requested_field=";

    std::uint32_t requested = 0;
    safe_read_value(manager + 0x14, requested);
    out << requested
        << L" slots=" << count;

    if (!g_status.error.empty()) {
        out << L" resolver_error=" << g_status.error;
    }

    const std::uintptr_t moduleBase =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

    for (std::uint64_t i = 0; i < count; ++i) {
        std::uintptr_t object = 0;
        safe_read_value(slots + i * sizeof(std::uintptr_t), object);

        out << L"\n[camera] slot " << std::dec << i << L": ";
        if (!object) {
            out << L"(empty)";
            continue;
        }

        std::string className;
        if (object_class_name(object, moduleBase, className)) {
            std::wstring wide(className.begin(), className.end());
            out << L"object=0x" << std::hex << object
                << L" class=" << wide;
        } else {
            out << L"object=0x" << std::hex << object
                << L" class=<unresolved>";
        }
    }

    return out.str();
}

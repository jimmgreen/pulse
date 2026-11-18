#include "crash_guard.h"
#include "../runtime.h"
#include "../../../common/crash_reporter.h"
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>
#include <string>
#include <set>
#include <utility>

namespace pulse::extensions::native::crash_guard {
namespace {

using Entry = std::pair<std::string, std::string>; // id, lowercase sha256

constexpr size_t kSlots = 32;
constexpr size_t kIdBytes = 128;
constexpr size_t kRecordBytes = kIdBytes + 72;

// Read by the crash path without locks: begin/end are published last on
// track and cleared first on forget; id/record are written only while the
// slot is unpublished.
struct Slot {
    std::atomic<uintptr_t> begin{0}, end{0};
    size_t package = 0;
    uint32_t refs = 0;
    char id[kIdBytes]{};
    char record[kRecordBytes]{};
    DWORD record_length = 0;
};
Slot g_slots[kSlots];
wchar_t g_crashed_path[MAX_PATH + 64]{};
std::wstring g_root;
std::set<Entry> g_disabled;

std::wstring File(const wchar_t* name) { return g_root + L"\\" + name; }

std::string Lower(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::optional<Entry> Identity(size_t package) {
    const Manifest* manifest = PackageManifest(package);
    if (!manifest || !manifest->native || manifest->id.empty() || manifest->native->sha256.empty()) return std::nullopt;
    if (manifest->id.find_first_of("\t\r\n") != std::string::npos || manifest->id.size() >= kIdBytes) return std::nullopt;
    return Entry{manifest->id, Lower(manifest->native->sha256)};
}

std::string ReadSmall(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string text;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 256 * 1024) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) read = 0;
        text.resize(read);
    }
    CloseHandle(file);
    return text;
}

bool WriteSmall(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = text.empty() || (WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                                     written == text.size());
    CloseHandle(file);
    return ok;
}

void ParseInto(const std::string& text, std::set<Entry>& out) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t tab = line.find('\t');
        if (tab != std::string::npos && tab > 0 && tab + 1 < line.size())
            out.emplace(line.substr(0, tab), Lower(line.substr(tab + 1)));
        start = end + 1;
    }
}

bool SaveDisabled() {
    std::string text;
    for (const auto& [id, sha] : g_disabled) text += id + "\t" + sha + "\n";
    const std::wstring path = File(L"crash-disabled.txt");
    if (text.empty()) return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    const std::wstring temp = path + L".tmp";
    if (!WriteSmall(temp, text) || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        OutputDebugStringA("Pulse extension crash guard: cannot_save_disabled_list\n");
        return false;
    }
    return true;
}

std::wstring Wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>((std::max)(length, 0)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    return out;
}

// Runs inside the exception filter: fixed buffers, one file write, no locks.
const char* Attribute(const void* address) noexcept {
    const auto at = reinterpret_cast<uintptr_t>(address);
    for (Slot& slot : g_slots) {
        const uintptr_t begin = slot.begin.load(std::memory_order_acquire);
        const uintptr_t end = slot.end.load(std::memory_order_acquire);
        if (!begin || at < begin || at >= end) continue;
        HANDLE file = CreateFileW(g_crashed_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, slot.record, slot.record_length, &written, nullptr);
            CloseHandle(file);
        }
        return slot.id;
    }
    return nullptr;
}

} // namespace

std::vector<std::wstring> ApplyAtStartup(bool isolated) {
    std::vector<std::wstring> newly_disabled;
    // Development hosts reload the author's build on every save; disabling it
    // after a crash would only get in the way of fixing it.
    if (DevelopmentMode() || PackageCount() == 0) return newly_disabled;
    wchar_t base[32768]{};
    const DWORD length = GetEnvironmentVariableW(isolated ? L"PULSE_TEST_DATA_DIR" : L"LOCALAPPDATA", base, 32768);
    if (!length || length >= 32768) return newly_disabled;
    std::wstring root(base, length);
    root += isolated ? L"\\extensions" : L"\\Pulse\\extensions";
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return newly_disabled;
    const std::wstring crashed = root + L"\\native-crashed.txt";
    if (crashed.size() >= std::size(g_crashed_path)) return newly_disabled;
    g_root = std::move(root);
    wcscpy_s(g_crashed_path, crashed.c_str());

    std::set<Entry> fresh;
    ParseInto(ReadSmall(File(L"native-activating.txt")), fresh);
    ParseInto(ReadSmall(crashed), fresh);
    g_disabled.clear();
    ParseInto(ReadSmall(File(L"crash-disabled.txt")), g_disabled);
    const size_t before = g_disabled.size();
    g_disabled.insert(fresh.begin(), fresh.end());
    bool changed = g_disabled.size() != before;

    for (size_t package = 0; package < PackageCount(); ++package) {
        const auto identity = Identity(package);
        if (!identity) continue;
        // A different build of a crashed package gets a fresh start.
        for (auto it = g_disabled.begin(); it != g_disabled.end();) {
            if (it->first == identity->first && it->second != identity->second) { it = g_disabled.erase(it); changed = true; }
            else ++it;
        }
        if (!g_disabled.count(*identity)) continue;
        SetPackageEnabled(package, false);
        if (fresh.count(*identity)) {
            const Manifest* manifest = PackageManifest(package);
            const std::string& name = manifest->name.empty() ? manifest->id : manifest->name;
            newly_disabled.push_back(Wide(name));
        }
    }
    const bool saved = !changed || SaveDisabled();
    // Only now that the list holds them: losing the markers first could let
    // the same crash repeat on the next start.
    if (saved) {
        DeleteFileW(File(L"native-activating.txt").c_str());
        DeleteFileW(crashed.c_str());
    }
    crash::SetFaultAttribution(&Attribute);
    return newly_disabled;
}

ActivationScope::ActivationScope(size_t package) {
    if (g_root.empty()) return;
    const auto identity = Identity(package);
    if (!identity) return;
    armed_ = WriteSmall(File(L"native-activating.txt"), identity->first + "\t" + identity->second + "\n");
}

ActivationScope::~ActivationScope() {
    if (armed_) DeleteFileW(File(L"native-activating.txt").c_str());
}

bool TrackModule(size_t package, HMODULE image) {
    if (g_root.empty() || !image) return false;
    for (Slot& slot : g_slots) {
        if (slot.refs && slot.package == package) { ++slot.refs; return true; }
    }
    const auto identity = Identity(package);
    if (!identity) return false;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), image, &info, sizeof(info)) || !info.SizeOfImage) return false;
    for (Slot& slot : g_slots) {
        if (slot.refs) continue;
        const std::string record = identity->first + "\t" + identity->second + "\n";
        if (record.size() >= kRecordBytes) return false;
        slot.package = package;
        slot.refs = 1;
        memcpy(slot.id, identity->first.c_str(), identity->first.size() + 1);
        memcpy(slot.record, record.c_str(), record.size() + 1);
        slot.record_length = static_cast<DWORD>(record.size());
        const auto begin = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
        slot.end.store(begin + info.SizeOfImage, std::memory_order_release);
        slot.begin.store(begin, std::memory_order_release);
        return true;
    }
    OutputDebugStringA("Pulse extension crash guard: no_free_slot\n");
    return false;
}

void ForgetModule(size_t package) {
    for (Slot& slot : g_slots) {
        if (!slot.refs || slot.package != package) continue;
        if (--slot.refs == 0) {
            slot.begin.store(0, std::memory_order_release);
            slot.end.store(0, std::memory_order_release);
        }
        return;
    }
}

void ForgetPackageCrash(size_t package) {
    if (g_root.empty()) return;
    const auto identity = Identity(package);
    if (identity && g_disabled.erase(*identity)) SaveDisabled();
}

} // namespace pulse::extensions::native::crash_guard

#include "crash_guard.h"
#include "../runtime.h"
#include "../../../common/crash_reporter.h"
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace pulse::extensions::native::crash_guard {
namespace {

using Entry = std::pair<std::string, std::string>; // id, lowercase sha256

constexpr size_t kSlots = 256;
constexpr size_t kIdBytes = 128;
constexpr size_t kRecordBytes = kIdBytes + 72;

struct ScopedTrackLock {
    SRWLOCK& lock;
    explicit ScopedTrackLock(SRWLOCK& value) noexcept : lock(value) { AcquireSRWLockExclusive(&lock); }
    ~ScopedTrackLock() { ReleaseSRWLockExclusive(&lock); }
};

struct PackageTrack {
    bool active = false;
    size_t package = 0;
    uint32_t refs = 0;
    char id[kIdBytes]{};
    char record[kRecordBytes]{};
    DWORD record_length = 0;
};

// Read by the crash path without locks: begin/end are published last on
// track and cleared first on forget; id/record are written only while the
// slot is unpublished.
struct Slot {
    std::atomic<uintptr_t> begin{0}, end{0};
    size_t package = 0;
    bool occupied = false;
    char id[kIdBytes]{};
    char record[kRecordBytes]{};
    DWORD record_length = 0;
};

SRWLOCK g_track_lock = SRWLOCK_INIT;
PackageTrack g_packages[kSlots];
Slot g_slots[kSlots];
size_t g_next_slot = 0;
wchar_t g_crashed_path[MAX_PATH + 64]{};
std::wstring g_root;
std::set<Entry> g_disabled;

std::wstring File(const wchar_t* name) { return g_root + L"\\" + name; }

std::string Lower(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::wstring LowerWide(std::wstring text) {
    for (wchar_t& c : text) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
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

bool ModuleRange(HMODULE image, uintptr_t& begin, uintptr_t& end) noexcept {
    MODULEINFO info{};
    if (!image || !GetModuleInformation(GetCurrentProcess(), image, &info, sizeof(info)) || !info.SizeOfImage)
        return false;
    begin = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    end = begin + info.SizeOfImage;
    return begin != 0 && end > begin;
}

PackageTrack* FindPackageLocked(size_t package) noexcept {
    for (PackageTrack& item : g_packages) {
        if (item.active && item.package == package) return &item;
    }
    return nullptr;
}

PackageTrack* EnsurePackageLocked(size_t package, bool& created) {
    created = false;
    if (PackageTrack* existing = FindPackageLocked(package)) return existing;
    const auto identity = Identity(package);
    if (!identity) return nullptr;
    const std::string record = identity->first + "\t" + identity->second + "\n";
    if (record.size() >= kRecordBytes) return nullptr;
    for (PackageTrack& item : g_packages) {
        if (item.active) continue;
        item.active = true;
        item.package = package;
        item.refs = 0;
        std::memcpy(item.id, identity->first.c_str(), identity->first.size() + 1);
        std::memcpy(item.record, record.c_str(), record.size() + 1);
        item.record_length = static_cast<DWORD>(record.size());
        created = true;
        return &item;
    }
    OutputDebugStringA("Pulse extension crash guard: no_free_package_slot\n");
    return nullptr;
}

bool PublishRangeLocked(const PackageTrack& pkg, uintptr_t begin, uintptr_t end) noexcept {
    if (!begin || end <= begin) return false;
    for (const Slot& slot : g_slots) {
        if (slot.occupied && slot.package == pkg.package &&
            slot.begin.load(std::memory_order_relaxed) == begin &&
            slot.end.load(std::memory_order_relaxed) == end) {
            return true;
        }
    }
    for (size_t offset = 0; offset < kSlots; ++offset) {
        const size_t index = (g_next_slot + offset) % kSlots;
        Slot& slot = g_slots[index];
        if (slot.occupied) continue;
        slot.package = pkg.package;
        slot.occupied = true;
        std::memcpy(slot.id, pkg.id, sizeof(slot.id));
        std::memcpy(slot.record, pkg.record, sizeof(slot.record));
        slot.record_length = pkg.record_length;
        slot.end.store(end, std::memory_order_release);
        slot.begin.store(begin, std::memory_order_release);
        g_next_slot = (index + 1) % kSlots;
        return true;
    }
    OutputDebugStringA("Pulse extension crash guard: no_free_slot\n");
    return false;
}

bool SafeRelativeDllPath(const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
        return false;
    for (const auto& part : relative) {
        if (part == L"..") return false;
    }
    return LowerWide(relative.extension().wstring()) == L".dll";
}

void PublishSidecarsLocked(const PackageTrack& pkg, const std::wstring& package_dir,
                           const NativeRuntime* runtime) {
    if (package_dir.empty() || !runtime || runtime->files.empty()) return;
    const std::filesystem::path base = std::filesystem::path(package_dir).lexically_normal();
    if (base.empty()) return;
    for (const auto& file : runtime->files) {
        const std::filesystem::path relative(Wide(file.path));
        if (!SafeRelativeDllPath(relative)) continue;
        const std::filesystem::path full = (base / relative).lexically_normal();
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, full.wstring().c_str(), &module) || !module)
            continue;
        uintptr_t begin = 0, end = 0;
        if (ModuleRange(module, begin, end)) PublishRangeLocked(pkg, begin, end);
    }
}

const Slot* FindSlot(const void* address) noexcept {
    const auto at = reinterpret_cast<uintptr_t>(address);
    if (!at) return nullptr;
    for (const Slot& slot : g_slots) {
        const uintptr_t begin = slot.begin.load(std::memory_order_acquire);
        const uintptr_t end = slot.end.load(std::memory_order_acquire);
        if (!begin || at < begin || at >= end) continue;
        return &slot;
    }
    return nullptr;
}

// Runs inside the exception filter: fixed buffers, one file write, no locks.
const char* Attribute(const void* address) noexcept {
    const Slot* slot = FindSlot(address);
    if (!slot) return nullptr;
    if (g_crashed_path[0] != L'\0') {
        HANDLE file = CreateFileW(g_crashed_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, slot->record, slot->record_length, &written, nullptr);
            CloseHandle(file);
        }
    }
    return slot->id;
}

} // namespace

std::vector<std::wstring> ApplyAtStartup(bool isolated) {
    crash::SetActiveCallClue(&ActiveCallPackage);
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

bool TrackPackageModules(size_t package, HMODULE entry_image,
                         const std::wstring& package_dir,
                         const NativeRuntime* runtime) {
    if (g_root.empty() || !entry_image) return false;
    uintptr_t begin = 0, end = 0;
    if (!ModuleRange(entry_image, begin, end)) return false;
    ScopedTrackLock lock(g_track_lock);
    bool created = false;
    PackageTrack* pkg = EnsurePackageLocked(package, created);
    if (!pkg) return false;
    if (!PublishRangeLocked(*pkg, begin, end)) {
        if (created) pkg->active = false;
        return false;
    }
    ++pkg->refs;
    PublishSidecarsLocked(*pkg, package_dir, runtime);
    crash::SetFaultAttribution(&Attribute);
    crash::SetActiveCallClue(&ActiveCallPackage);
    return true;
}

bool TrackModule(size_t package, HMODULE image) {
    return TrackPackageModules(package, image, {}, nullptr);
}

void RefreshPackageModules(size_t package, const std::wstring& package_dir,
                           const NativeRuntime* runtime) {
    if (g_root.empty() || package_dir.empty() || !runtime || runtime->files.empty()) return;
    ScopedTrackLock lock(g_track_lock);
    const PackageTrack* pkg = FindPackageLocked(package);
    if (!pkg || pkg->refs == 0) return;
    PublishSidecarsLocked(*pkg, package_dir, runtime);
}

bool TrackModuleRange(size_t package, uintptr_t begin, uintptr_t end, bool add_ref) {
    if (!begin || end <= begin) return false;
    ScopedTrackLock lock(g_track_lock);
    bool created = false;
    PackageTrack* pkg = EnsurePackageLocked(package, created);
    if (!pkg) return false;
    if (!created && !add_ref && pkg->refs == 0) return false;
    if (created && !add_ref) {
        pkg->active = false;
        return false;
    }
    if (!PublishRangeLocked(*pkg, begin, end)) {
        if (created) pkg->active = false;
        return false;
    }
    if (add_ref) ++pkg->refs;
    crash::SetFaultAttribution(&Attribute);
    crash::SetActiveCallClue(&ActiveCallPackage);
    return true;
}

void ForgetModule(size_t package) {
    ScopedTrackLock lock(g_track_lock);
    PackageTrack* pkg = FindPackageLocked(package);
    if (!pkg || pkg->refs == 0) return;
    if (--pkg->refs > 0) return;
    for (Slot& slot : g_slots) {
        if (!slot.occupied || slot.package != package) continue;
        slot.begin.store(0, std::memory_order_release);
        slot.end.store(0, std::memory_order_release);
        slot.occupied = false;
    }
    pkg->active = false;
}

const char* FindAttributedPackage(const void* address) noexcept {
    const Slot* slot = FindSlot(address);
    return slot ? slot->id : nullptr;
}

void ForgetPackageCrash(size_t package) {
    if (g_root.empty()) return;
    const auto identity = Identity(package);
    if (identity && g_disabled.erase(*identity)) SaveDisabled();
}

} // namespace pulse::extensions::native::crash_guard

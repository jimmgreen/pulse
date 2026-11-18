#include "setup_dialog_migration.h"
#include "setup_common.h"
#include <algorithm>
namespace pulse::setup {
namespace {
bool Same(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool OwnedDll(const std::wstring& app, const std::wstring& path) {
    if (Same(path, app + L"\\" + dialog_auto::DllName) || Same(path, app + L"\\DialogBridge\\" + dialog_auto::DllName)) return true;
    const auto prefix = app + L"\\DialogBridge\\";
    if (path.size() != prefix.size() + 65 + wcslen(dialog_auto::DllName) || !Same(path.substr(0, prefix.size()), prefix)) return false;
    const auto digest = path.substr(prefix.size(), 64);
    return std::all_of(digest.begin(), digest.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }) &&
        Same(path.substr(prefix.size() + 64), L"\\" + std::wstring(dialog_auto::DllName));
}
}
DialogMigrationApi SystemDialogMigrationApi() { return {dialog_auto::Inspect, dialog_auto::Enable}; }
bool IsOwnedDialogDll(const std::wstring& app, const std::wstring& path) { return OwnedDll(app, path); }
DWORD DisableOwnedDialogRegistration(const std::wstring& app, const dialog_auto::Status& status,
    const std::function<DWORD(const std::wstring&)>& disable) {
    if (status.error) return status.error;
    if (!OwnedDll(app, status.dll_path)) return ERROR_SUCCESS;
    return disable(status.dll_path);
}
std::vector<std::wstring> RetiredRuntimeFiles(const std::vector<std::wstring>& shipped) {
    std::vector<std::wstring> result;
    for (const wchar_t* stale : {L"msvcp140.dll", L"msvcp140_atomic_wait.dll", L"vcruntime140.dll", L"vcruntime140_1.dll"})
        if (std::none_of(shipped.begin(), shipped.end(), [&](const auto& path) { return Same(path, stale); })) result.emplace_back(stale);
    return result;
}
std::vector<std::wstring> LegacyDialogFiles(const std::wstring& app, const std::vector<std::wstring>& shipped) {
    std::string manifest;
    std::vector<std::wstring> result;
    if (!ReadWholeFile(app + L"\\.pulse-files", manifest)) return result;
    for (size_t pos = 0; pos < manifest.size();) {
        size_t end = manifest.find('\n', pos); if (end == std::string::npos) end = manifest.size();
        auto line = manifest.substr(pos, end - pos); pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto rel = Utf8ToWide(line);
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        // The previous flat schema-2 package owned these root bridge artifacts.
        // An installed-file entry alone must never authorize deleting arbitrary
        // root files, and files still shipped by the new package remain in place.
        constexpr const wchar_t* flat_bridge[] = {
            L"PulseDialogLauncher.exe", L"PulsePickerUiProof.dll", L"PulseDialogAutoControl.exe",
            L"PulseDialogAuto64.dll", L"PulseDialogAuto32.dll",
            L"PulseFileDialogRuntimeUiProof64R8.dll", L"PulseFileDialogRuntimeUiProof32R8.dll",
            L"manifest.json", L"VERIFIED.json"
        };
        const bool flat = std::any_of(std::begin(flat_bridge), std::end(flat_bridge), [&](const auto* name) { return Same(rel, name); });
        constexpr const wchar_t* old_runtime[] = {L"Pulse.AI.exe",L"Pulse.Content.exe",L"Pulse.Document.exe",L"Pulse.Index.exe",
            L"Pulse.Preview.exe",L"pulse_elevated.exe",L"pulse_shell.exe",L"pulse_start_pin.dll",L"pulse_integration.exe",L"lumatext.dll",L"pdfium.dll"};
        const bool full_version = std::any_of(shipped.begin(), shipped.end(), [](const auto& path) {
            return path.starts_with(L"DialogBridge\\") && path.ends_with(L"\\pulse_integration.exe");
        });
        const bool runtime = full_version && std::any_of(std::begin(old_runtime), std::end(old_runtime), [&](const auto* name) { return Same(rel,name); });
        if (flat || runtime) {
            if (std::any_of(shipped.begin(), shipped.end(), [&](const auto& file) { return Same(rel, file); })) continue;
            const DWORD attrs = GetFileAttributesW((app + L"\\" + rel).c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                std::none_of(result.begin(), result.end(), [&](const auto& file) { return Same(rel, file); })) result.push_back(rel);
            continue;
        }
        if (rel.size() <= 13 || !Same(rel.substr(0, 13), L"DialogBridge\\") || rel.find(L"..") != std::wstring::npos || rel.find(L':') != std::wstring::npos) continue;
        const DWORD file_attrs = GetFileAttributesW((app + L"\\" + rel).c_str());
        if (file_attrs != INVALID_FILE_ATTRIBUTES && (file_attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
        // Never follow an installed manifest through a user-created reparse directory.
        bool reparse = false;
        for (size_t slash = rel.find(L'\\'); slash != std::wstring::npos; slash = rel.find(L'\\', slash + 1)) {
            const DWORD attrs = GetFileAttributesW((app + L"\\" + rel.substr(0, slash)).c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) reparse = true;
        }
        if (!reparse && std::none_of(result.begin(), result.end(), [&](const auto& entry) { return Same(entry, rel); })) result.push_back(std::move(rel));
    }
    return result;
}
DialogLayoutMigration::DialogLayoutMigration(std::wstring app, bool real, DialogMigrationApi api)
    : app_(app), old_(app + L"\\DialogBridge\\" + dialog_auto::DllName), next_(app + L"\\" + dialog_auto::DllName),
      marker_(app + L"\\.pulse-setup\\dialog-migration"), real_(real), api_(std::move(api)) {}
DialogLayoutMigration::~DialogLayoutMigration() { if (mutex_) { ReleaseMutex(mutex_); CloseHandle(mutex_); } }
void DialogLayoutMigration::SetVersionDirectory(const std::wstring& relative) {
    next_ = app_ + L"\\" + relative + L"\\" + dialog_auto::DllName;
    versioned_ = true;
}
bool DialogLayoutMigration::Lock(std::wstring& error) {
    if (mutex_ || !real_) return true;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Global\\Pulse.DialogAuto.Registration.v1");
    const DWORD wait = mutex ? WaitForSingleObject(mutex, 30000) : WAIT_FAILED;
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        error = L"Cannot lock dialog registration: " + Win32Error(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        if (mutex) CloseHandle(mutex); return false;
    }
    mutex_ = mutex; return true;
}
bool DialogLayoutMigration::Prepare(std::wstring& error) {
    if (!real_) return true;
    if (!Lock(error)) return false;
    const auto status = api_.inspect();
    if (status.error) { error = L"Cannot inspect dialog registration: " + Win32Error(status.error); return false; }
    if (versioned_) {
        if (!OwnedDll(app_, next_)) { error = L"Invalid dialog migration destination."; return false; }
        if (!OwnedDll(app_, status.dll_path)) return true;
        old_ = status.dll_path;
        if (Same(old_, next_)) return true;
    }
    if (status.conflict && Same(status.dll_path, old_)) { error = L"Legacy dialog registration is inconsistent; repair it before upgrading."; return false; }
    if (!status.enabled || status.conflict || !Same(status.dll_path, old_)) return true;
    // Flush intent before any file or registry mutation. Only canonical paths derived from app are used.
    HANDLE file = CreateFileW(marker_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = L"Cannot journal dialog migration: " + Win32Error(GetLastError()); return false; }
    const std::string version = versioned_ ? "3\n1\n" + WideToUtf8(old_.substr(app_.size()+1)) + "\n" + WideToUtf8(next_.substr(app_.size()+1)) + "\n" : "1\n";
    DWORD written = 0;
    const bool ok = WriteFile(file, version.data(), static_cast<DWORD>(version.size()), &written, nullptr) && written == version.size() && FlushFileBuffers(file);
    const DWORD failure = GetLastError(); CloseHandle(file);
    if (!ok) { error = L"Cannot flush dialog migration: " + Win32Error(failure); return false; }
    prepared_ = true; return true;
}
bool DialogLayoutMigration::Apply(std::wstring& error) {
    if (!prepared_) return true;
    const auto status = api_.inspect();
    if (status.error || status.conflict || !status.enabled || !Same(status.dll_path, old_)) {
        error = L"Dialog registration changed during installation."; return false;
    }
    HANDLE marker = CreateFileW(marker_.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker == INVALID_HANDLE_VALUE) { error = L"Cannot advance dialog migration journal: " + Win32Error(GetLastError()); return false; }
    DWORD written = 0;
    if (versioned_ && SetFilePointer(marker, 2, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        const DWORD failure = GetLastError(); CloseHandle(marker);
        error = L"Cannot seek dialog migration journal: " + Win32Error(failure); return false;
    }
    const bool flushed = WriteFile(marker, "2\n", 2, &written, nullptr) && written == 2 && FlushFileBuffers(marker);
    const DWORD failure = GetLastError(); CloseHandle(marker);
    if (!flushed) { error = L"Cannot flush dialog migration journal: " + Win32Error(failure); return false; }
    const DWORD result = api_.enable(next_);
    if (result) { error = L"Cannot migrate dialog registration: " + Win32Error(result); return false; }
    return true;
}
bool DialogLayoutMigration::Recover(std::wstring& error) {
    if (!real_ || GetFileAttributesW(marker_.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    if (!Lock(error)) return false;
    std::string version;
    if (!ReadWholeFile(marker_, version) || version.size() > 1024) { error = L"Invalid dialog migration recovery marker."; return false; }
    if (version.starts_with("3\n")) {
        const auto split = version.find('\n', 4), end = split == std::string::npos ? split : version.find('\n', split + 1);
        if (version.size() < 5 || (version.substr(2, 2) != "1\n" && version.substr(2, 2) != "2\n") ||
            split == std::string::npos || end != version.size() - 1) { error = L"Invalid versioned dialog recovery marker."; return false; }
        old_ = app_ + L"\\" + Utf8ToWide(version.substr(4, split-4));
        next_ = app_ + L"\\" + Utf8ToWide(version.substr(split+1, end-split-1));
        if (!OwnedDll(app_, old_) || !OwnedDll(app_, next_)) { error = L"Dialog recovery path is outside this installation."; return false; }
        version = version.substr(2,2);
    }
    if (version != "1\n" && version != "2\n") { error = L"Invalid dialog migration recovery marker."; return false; }
    if (version == "1\n") return true;  // No registry operation began; preserve any concurrent user change.
    const auto status = api_.inspect();
    if (status.error) { error = L"Cannot inspect dialog recovery: " + Win32Error(status.error); return false; }
    if (!Same(status.dll_path, old_) && !Same(status.dll_path, next_)) {
        error = L"Dialog registration changed; automatic recovery was not applied."; return false;
    }
    if (status.conflict) { error = L"Dialog registration conflict; recovery remains pending."; return false; }
    if (status.enabled && Same(status.dll_path, old_)) return true;
    const DWORD result = api_.enable(old_);
    if (result) { error = L"Cannot restore dialog registration: " + Win32Error(result); return false; }
    return true;
}
}

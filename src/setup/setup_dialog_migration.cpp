#include "setup_dialog_migration.h"
#include "setup_common.h"
#include <algorithm>
namespace pulse::setup {
namespace {
bool Same(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool OwnedDll(const std::wstring& app, const std::wstring& path, dialog_auto::Architecture architecture) {
    const wchar_t* name = dialog_auto::DllNameFor(architecture);
    if (!name) return false;
    if (Same(path, app + L"\\" + name) || Same(path, app + L"\\DialogBridge\\" + name)) return true;
    const auto prefix = app + L"\\DialogBridge\\";
    if (path.size() != prefix.size() + 65 + wcslen(name) || !Same(path.substr(0, prefix.size()), prefix)) return false;
    const auto digest = path.substr(prefix.size(), 64);
    return std::all_of(digest.begin(), digest.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }) &&
        Same(path.substr(prefix.size() + 64), L"\\" + std::wstring(name));
}
}
DialogMigrationApi SystemDialogMigrationApi() {
    return {[] { return dialog_auto::Inspect(); }, [](const std::wstring& path) { return dialog_auto::Enable(path); },
        [](dialog_auto::Architecture architecture) { return dialog_auto::Inspect(architecture); },
        [](dialog_auto::Architecture architecture, const std::wstring& path) { return dialog_auto::Enable(architecture, path); }};
}
bool IsOwnedDialogDll(const std::wstring& app, const std::wstring& path) { return OwnedDll(app, path, dialog_auto::CurrentArchitecture); }
bool IsOwnedDialogDll(const std::wstring& app, const std::wstring& path, dialog_auto::Architecture architecture) { return OwnedDll(app, path, architecture); }
DWORD DisableOwnedDialogRegistration(const std::wstring& app, const dialog_auto::Status& status,
    const std::function<DWORD(const std::wstring&)>& disable) {
    return DisableOwnedDialogRegistration(app, status, disable, dialog_auto::CurrentArchitecture);
}
DWORD DisableOwnedDialogRegistration(const std::wstring& app, const dialog_auto::Status& status,
    const std::function<DWORD(const std::wstring&)>& disable, dialog_auto::Architecture architecture) {
    if (status.error) return status.error;
    if (!OwnedDll(app, status.dll_path, architecture)) return ERROR_SUCCESS;
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
    : app_(std::move(app)), marker_(app_ + L"\\.pulse-setup\\dialog-migration"), real_(real), api_(std::move(api)) {
    for (size_t i = 0; i < registrations_.size(); ++i) {
        auto& registration = registrations_[i];
        registration.architecture = i == 0 ? dialog_auto::Architecture::X64 : dialog_auto::Architecture::X86;
        const auto name = dialog_auto::DllNameFor(registration.architecture);
        registration.old_path = app_ + L"\\DialogBridge\\" + name;
        registration.next_path = app_ + L"\\" + name;
        registration.configured = registration.architecture == dialog_auto::CurrentArchitecture;
    }
}
DialogLayoutMigration::~DialogLayoutMigration() { ReleaseLock(); }
void DialogLayoutMigration::ReleaseLock() {
    if (!mutex_) return;
    ReleaseMutex(mutex_);
    CloseHandle(mutex_);
    mutex_ = nullptr;
}
void DialogLayoutMigration::SetVersionDirectory(const std::wstring& relative) {
    SetCompanionVersionDirectory(dialog_auto::CurrentArchitecture, relative);
}
void DialogLayoutMigration::SetCompanionVersionDirectory(dialog_auto::Architecture architecture, const std::wstring& relative) {
    if (!dialog_auto::DllNameFor(architecture)) { valid_plan_ = false; return; }
    auto& registration = registrations_[architecture == dialog_auto::Architecture::X64 ? 0 : 1];
    registration.next_path = app_ + L"\\" + relative + L"\\" + dialog_auto::DllNameFor(architecture);
    registration.configured = true; registration.versioned = true;
}
dialog_auto::Status DialogLayoutMigration::Inspect(dialog_auto::Architecture architecture) const {
    if (api_.inspect_arch) return api_.inspect_arch(architecture);
    if (architecture == dialog_auto::CurrentArchitecture && api_.inspect) return api_.inspect();
    return {false, false, ERROR_NOT_SUPPORTED, {}};
}
DWORD DialogLayoutMigration::Enable(dialog_auto::Architecture architecture, const std::wstring& path) const {
    if (api_.enable_arch) return api_.enable_arch(architecture, path);
    if (architecture == dialog_auto::CurrentArchitecture && api_.enable) return api_.enable(path);
    return ERROR_NOT_SUPPORTED;
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
    if (!valid_plan_) { error = L"Invalid dialog migration architecture."; return false; }
    prepared_ = false;
    for (auto& registration : registrations_) {
        registration.stage = 0;
        if (!registration.configured) continue;
        if (!OwnedDll(app_, registration.next_path, registration.architecture)) { error = L"Invalid dialog migration destination."; return false; }
        const auto status = Inspect(registration.architecture);
        if (status.error) { error = L"Cannot inspect dialog registration: " + Win32Error(status.error); return false; }
        if (registration.versioned) {
            if (!OwnedDll(app_, status.dll_path, registration.architecture)) continue;
            registration.old_path = status.dll_path;
            if (Same(registration.old_path, registration.next_path)) continue;
        }
        if (status.conflict && Same(status.dll_path, registration.old_path)) {
            error = L"Legacy dialog registration is inconsistent; repair it before upgrading."; return false;
        }
        if (!status.enabled || status.conflict || !Same(status.dll_path, registration.old_path)) continue;
        registration.stage = 1;
    }
    std::string intent = "4\n";
    for (auto& registration : registrations_) {
        if (!registration.stage) continue;
        registration.phase_offset = static_cast<LONGLONG>(intent.size() + 3);
        intent += registration.architecture == dialog_auto::Architecture::X64 ? "64\t1\t" : "32\t1\t";
        intent += WideToUtf8(registration.old_path.substr(app_.size() + 1)) + "\t" +
            WideToUtf8(registration.next_path.substr(app_.size() + 1)) + "\n";
    }
    if (intent == "4\n") return true;
    HANDLE file = CreateFileW(marker_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = L"Cannot journal dialog migration: " + Win32Error(GetLastError()); return false; }
    DWORD written = 0;
    const bool ok = WriteFile(file, intent.data(), static_cast<DWORD>(intent.size()), &written, nullptr) && written == intent.size() && FlushFileBuffers(file);
    const DWORD failure = GetLastError(); CloseHandle(file);
    if (!ok) { error = L"Cannot flush dialog migration: " + Win32Error(failure); return false; }
    prepared_ = true; return true;
}
bool DialogLayoutMigration::SetPhase(const Registration& registration, unsigned phase, std::wstring& error) const {
    HANDLE file = CreateFileW(marker_.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = L"Cannot advance dialog migration journal: " + Win32Error(GetLastError()); return false; }
    LARGE_INTEGER position{}; position.QuadPart = registration.phase_offset;
    const char value = static_cast<char>('0' + phase); DWORD written = 0;
    // Only a pre-existing phase byte changes. A torn rewrite must not destroy
    // either architecture's already-flushed source/destination intent.
    const bool ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) && WriteFile(file, &value, 1, &written, nullptr) && written == 1 && FlushFileBuffers(file);
    const DWORD failure = GetLastError(); CloseHandle(file);
    if (!ok) { error = L"Cannot flush dialog migration phase: " + Win32Error(failure); return false; }
    return true;
}
bool DialogLayoutMigration::Apply(std::wstring& error) {
    if (!prepared_) return true;
    const auto unchanged = [&](const Registration& registration) {
        const auto status = Inspect(registration.architecture);
        return !status.error && !status.conflict && status.enabled && Same(status.dll_path, registration.old_path);
    };
    for (const auto& registration : registrations_) if (registration.stage && !unchanged(registration)) {
        error = L"Dialog registration changed during installation."; return false;
    }
    for (auto& registration : registrations_) {
        if (!registration.stage) continue;
        if (!unchanged(registration)) { error = L"Dialog registration changed during installation."; return false; }
        if (!SetPhase(registration, 2, error)) return false;
        registration.stage = 2;
        const DWORD result = Enable(registration.architecture, registration.next_path);
        if (result) { error = L"Cannot migrate dialog registration: " + Win32Error(result); return false; }
        const auto applied = Inspect(registration.architecture);
        if (applied.error || applied.conflict || !applied.enabled || !Same(applied.dll_path, registration.next_path)) {
            error = L"Dialog migration did not publish the expected registration."; return false;
        }
        if (!SetPhase(registration, 3, error)) return false;
        registration.stage = 3;
    }
    return true;
}
bool DialogLayoutMigration::ReadJournal(std::vector<Registration>& registrations, bool& legacy, std::wstring& error) const {
    std::string text;
    const auto invalid = [&] { error = L"Invalid dialog migration recovery marker."; return false; };
    if (!ReadWholeFile(marker_, text) || text.size() > 4096) return invalid();
    legacy = !text.starts_with("4\n");
    if (!legacy) {
        bool seen[2]{};
        for (size_t position = 2; position < text.size();) {
            const size_t end = text.find('\n', position);
            if (end == std::string::npos) return invalid();
            const auto line = text.substr(position, end - position);
            const auto first = line.find('\t'), second = line.find('\t', first == std::string::npos ? 0 : first + 1);
            const auto third = line.find('\t', second == std::string::npos ? 0 : second + 1);
            if (first != 2 || second != 4 || third == std::string::npos || line.find('\t', third + 1) != std::string::npos ||
                (line.substr(0, 2) != "64" && line.substr(0, 2) != "32") || line[3] < '1' || line[3] > '3') return invalid();
            const size_t index = line.starts_with("64") ? 0 : 1;
            if (seen[index]) return invalid(); seen[index] = true;
            Registration registration;
            registration.architecture = index == 0 ? dialog_auto::Architecture::X64 : dialog_auto::Architecture::X86;
            registration.stage = static_cast<unsigned>(line[3] - '0');
            registration.phase_offset = static_cast<LONGLONG>(position + 3);
            registration.old_path = app_ + L"\\" + Utf8ToWide(line.substr(5, third - 5));
            registration.next_path = app_ + L"\\" + Utf8ToWide(line.substr(third + 1));
            if (!OwnedDll(app_, registration.old_path, registration.architecture) || !OwnedDll(app_, registration.next_path, registration.architecture)) return invalid();
            registrations.push_back(std::move(registration)); position = end + 1;
        }
        return registrations.empty() ? invalid() : true;
    }
    Registration registration;
    registration.architecture = dialog_auto::CurrentArchitecture;
    registration.old_path = app_ + L"\\DialogBridge\\" + dialog_auto::DllName;
    registration.next_path = app_ + L"\\" + dialog_auto::DllName;
    if (text.starts_with("3\n")) {
        const auto split = text.find('\n', 4), end = split == std::string::npos ? split : text.find('\n', split + 1);
        if (text.size() < 5 || (text.substr(2, 2) != "1\n" && text.substr(2, 2) != "2\n") ||
            split == std::string::npos || end != text.size() - 1) return invalid();
        registration.old_path = app_ + L"\\" + Utf8ToWide(text.substr(4, split - 4));
        registration.next_path = app_ + L"\\" + Utf8ToWide(text.substr(split + 1, end - split - 1));
        if (!OwnedDll(app_, registration.old_path, registration.architecture) || !OwnedDll(app_, registration.next_path, registration.architecture)) return invalid();
        text = text.substr(2, 2);
    }
    if (text != "1\n" && text != "2\n") return invalid();
    registration.stage = static_cast<unsigned>(text[0] - '0'); registrations.push_back(std::move(registration));
    return true;
}
bool DialogLayoutMigration::Recover(std::wstring& error) {
    if (!real_) return true;
    const DWORD attributes = GetFileAttributesW(marker_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD missing = GetLastError();
        if (missing == ERROR_FILE_NOT_FOUND || missing == ERROR_PATH_NOT_FOUND) return true;
        error = L"Cannot inspect dialog migration recovery: " + Win32Error(missing); return false;
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !Lock(error)) {
        if (error.empty()) error = L"Invalid dialog migration recovery marker."; return false;
    }
    std::vector<Registration> registrations; bool legacy = false;
    if (!ReadJournal(registrations, legacy, error)) return false;
    bool complete = true;
    for (auto it = registrations.rbegin(); it != registrations.rend(); ++it) {
        auto& registration = *it;
        if (registration.stage == 1) continue;
        const auto status = Inspect(registration.architecture);
        std::wstring failure;
        if (status.error) failure = L"Cannot inspect dialog recovery: " + Win32Error(status.error);
        else if ((!Same(status.dll_path, registration.old_path) && !Same(status.dll_path, registration.next_path)) || status.conflict)
            failure = L"Dialog registration changed; automatic recovery was not applied.";
        else if (registration.stage == 3 && !status.enabled) {
            // A completed enable followed by a user disable is not ours to undo.
        } else if (!(status.enabled && Same(status.dll_path, registration.old_path))) {
            const DWORD result = Enable(registration.architecture, registration.old_path);
            if (result) failure = L"Cannot restore dialog registration: " + Win32Error(result);
            else {
                const auto restored = Inspect(registration.architecture);
                if (restored.error || restored.conflict || !restored.enabled || !Same(restored.dll_path, registration.old_path))
                    failure = L"Dialog recovery did not restore the expected registration.";
            }
        }
        if (failure.empty() && !legacy && !SetPhase(registration, 1, failure) && failure.empty())
            failure = L"Cannot record dialog migration recovery.";
        if (!failure.empty()) { if (complete) error = failure; complete = false; }
    }
    return complete;
}
}

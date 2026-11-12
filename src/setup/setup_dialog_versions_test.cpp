#include "setup_dialog_versions.h"
#include "setup_dialog_migration.h"
#include "setup_transaction.h"
#include "setup_runtime_transaction.h"
#include "setup_common.h"
#include "../common/dialog_bundle.h"
#include <filesystem>
#include <cstdio>
#include <algorithm>
#include <thread>
#include <fstream>

namespace {
namespace fs = std::filesystem;
using namespace pulse::setup;
int failures = 0;
void Check(bool ok, const char* text) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text); std::fflush(stdout); if (!ok) ++failures; }
std::string Read(const fs::path& file) { std::string text; ReadWholeFile(file.wstring(), text); return text; }
bool Write(const fs::path& file, const std::string& text) { return WriteWholeFile(file.wstring(), text.data(), text.size()); }
std::wstring Self() { std::wstring path(32768, L'\0'); const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())); path.resize(n); return path; }
std::wstring Dll(const fs::path& app) {
    pulse::dialog_bundle::BundleLease lease;
    if (pulse::dialog_bundle::ResolveInstalledBundle(app.wstring(), lease)) return {};
    return lease.Directory() + L"\\" + pulse::dialog_auto::DllName;
}
struct Holder {
    HANDLE process = nullptr, ready = nullptr, stop = nullptr;
    bool Start(const fs::path& app) {
        const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
        const auto ready_name = L"Local\\Pulse.VersionTest.Ready." + suffix;
        const auto stop_name = L"Local\\Pulse.VersionTest.Stop." + suffix;
        ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
        stop = CreateEventW(nullptr, TRUE, FALSE, stop_name.c_str());
        auto command = Quote(Self()) + L" --hold " + Quote(app.wstring()) + L" " + Quote(ready_name) + L" " + Quote(stop_name);
        STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
        CloseHandle(pi.hThread); process = pi.hProcess;
        HANDLE wait[] = {ready, process};
        return WaitForMultipleObjects(2, wait, FALSE, 10000) == WAIT_OBJECT_0;
    }
    bool Alive() const { return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }
    void Stop() { if (stop) SetEvent(stop); if (process) { WaitForSingleObject(process, 10000); CloseHandle(process); process = nullptr; } if (ready) { CloseHandle(ready); ready = nullptr; } if (stop) { CloseHandle(stop); stop = nullptr; } }
    ~Holder() { Stop(); }
};
bool Apply(const fs::path& app, const fs::path& package, bool fail_registration = false, bool lock_new = false, bool interrupt = false) {
    fs::create_directories(app);
    auto registered = Utf8ToWide(Read(app / L".test-registration"));
    if (registered.empty()) registered = Dll(app);
    pulse::dialog_auto::Status status{!registered.empty(), false, 0, registered};
    HANDLE new_lock = INVALID_HANDLE_VALUE;
    unsigned writes = 0;
    DialogMigrationApi api{[&] { return status; }, [&](const std::wstring& next) -> DWORD {
        ++writes;
        status.enabled = true; status.dll_path = next;
        Write(app / L".test-registration", WideToUtf8(next));
        if (interrupt && writes == 1) ExitProcess(77);
        if (writes == 1 && lock_new) new_lock = CreateFileW(next.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        return writes == 1 && fail_registration ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
    }};
    DialogLayoutMigration migration(app.wstring(), true, api);
    FileTransaction tx(app.wstring(), [&](std::wstring& e) { return migration.Recover(e); });
    std::wstring error;
    bool ok = tx.Begin(error);
    std::vector<std::wstring> files;
    if (ok) {
        for (const auto& e : fs::recursive_directory_iterator(package)) {
            if (!e.is_regular_file()) continue;
            const auto relative = fs::relative(e.path(), package);
            files.push_back(relative.wstring());
            const auto target = fs::path(tx.StagingDir()) / relative;
            fs::create_directories(target.parent_path());
            fs::copy_file(e.path(), target, fs::copy_options::overwrite_existing);
        }
        DialogVersionPlan plan;
        ok = PrepareDialogVersion(app.wstring(), tx.StagingDir(), files, plan, error) &&
            PublishDialogVersion(app.wstring(), tx.StagingDir(), plan, error);
        if (ok) {
            std::erase(files, L".pulse-files");
            std::string installed_manifest;
            for (const auto& file : files) installed_manifest += WideToUtf8(file) + "\r\n";
            ok = Write(fs::path(tx.StagingDir()) / L".pulse-files", installed_manifest);
            std::erase(plan.replacements, L".pulse-files");
            plan.replacements.insert(plan.versioned ? plan.replacements.end()-1 : plan.replacements.end(), L".pulse-files");
            auto obsolete = LegacyDialogFiles(app.wstring(), files);
            std::erase_if(obsolete, IsDialogVersionFile);
            const auto retired = RetiredRuntimeFiles(files);
            obsolete.insert(obsolete.end(), retired.begin(), retired.end());
            migration.SetVersionDirectory(plan.relative_directory);
            ok = ok && migration.Prepare(error) && tx.Commit(plan.replacements, error, obsolete, [&](std::wstring& e) { return migration.Apply(e); });
        }
        if (ok) {
            tx.Finish();
            if (!status.enabled) { status.enabled = true; status.dll_path = Dll(app); Write(app / L".test-registration", WideToUtf8(status.dll_path)); }
        }
    }
    if (new_lock != INVALID_HANDLE_VALUE) CloseHandle(new_lock);
    if (!ok) std::fwprintf(stderr, L"apply: %s\n", error.c_str());
    return ok;
}
int Hold(const fs::path& app, const wchar_t* ready_name, const wchar_t* stop_name) {
    const auto dll = Dll(app);
    if (dll.empty()) return 2;
    const auto launcher = fs::path(dll).parent_path() / L"pulse.exe";
    HANDLE a = CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE b = CreateFileW(launcher.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name), stop = OpenEventW(SYNCHRONIZE, FALSE, stop_name);
    if (a == INVALID_HANDLE_VALUE || b == INVALID_HANDLE_VALUE || !ready || !stop) return 3;
    SetEvent(ready); WaitForSingleObject(stop, 120000);
    CloseHandle(a); CloseHandle(b); CloseHandle(ready); CloseHandle(stop); return 0;
}
int CleanupReports(const fs::path& a, const fs::path& b, const fs::path& c) {
    const auto root = fs::absolute(L"bench_data") / (L"dialog-cleanup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root / L"DialogBridge");
    std::vector<std::string> digests;
    for (const auto& package : {a, b, c}) {
        pulse::dialog_bundle::BundleLease bundle;
        const DWORD error = pulse::dialog_bundle::ResolveInstalledBundle(package.wstring(), bundle);
        Check(error == ERROR_SUCCESS, "resolve source fixture bundle");
        if (error) return 1;
        digests.push_back(bundle.ManifestDigest());
        fs::copy(bundle.Directory(), root / L"DialogBridge" / Utf8ToWide(bundle.ManifestDigest()), fs::copy_options::recursive);
    }
    Check(digests[0] != digests[1] && digests[1] != digests[2] && digests[0] != digests[2], "fixture contains three independent versions");
    Check(Write(root / L"dialog-current.json", Read(a / L"dialog-current.json")), "select A only inside fixture");
    const auto retired = fs::path(Dll(root)).parent_path();
    const auto retired_dll = Dll(root);
    const auto manifest = Read(retired / L"manifest.json");
    Holder holder;
    Check(holder.Start(root), "external process acquires retired bundle before version switch");
    Check(Write(root / L"dialog-current.json", Read(c / L"dialog-current.json")) &&
        Write(root / L".pulse-dialog-previous", digests[1]), "protect current C and previous B");
    for (unsigned i = 0; i < 20; ++i) fs::create_directory(root / L"DialogBridge" / (L"000-unknown-" + std::to_wstring(i)));
    const auto locked = CleanupDialogVersionsReport(root.wstring());
    Check(locked.scan_complete && locked.protected_directories.size() == 2 && locked.retained_directories.size() == 20 &&
        locked.blocked_directories.size() == 1 && locked.deleted_directories.empty() && locked.Retryable(),
        "complete scan exceeds sixteen entries and reports protected, unknown and blocked versions");
    Check(holder.Alive() && Read(retired / L"manifest.json") == manifest && fs::exists(retired_dll) &&
        fs::exists(retired / L"pulse.exe"), "busy cleanup preserves complete bundle and leaves holder running");
    Check(!locked.locks.files.empty(), "cleanup reports locked file paths");
    if (!locked.locks.programs.empty()) Check(true, "cleanup identifies external owner");
    else {
        Check(locked.locks.identification_error != ERROR_SUCCESS, "unidentified owner preserves identification error");
        std::printf("[SKIP] Restart Manager owner identification unavailable (Win32 %lu)\n", locked.locks.identification_error);
    }
    holder.Stop();
    HANDLE directory_lock = CreateFileW(retired.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Check(directory_lock != INVALID_HANDLE_VALUE, "hold directory with no delete sharing");
    HANDLE delete_probe = CreateFileW(retired.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    const DWORD probe_error = GetLastError();
    Check(delete_probe == INVALID_HANDLE_VALUE && probe_error == ERROR_SHARING_VIOLATION,
        "directory fixture actually denies DELETE access before cleanup");
    if (delete_probe != INVALID_HANDLE_VALUE) CloseHandle(delete_probe);
    const auto unidentified = CleanupDialogVersionsReport(root.wstring());
    Check(unidentified.Retryable() && unidentified.blocked_directories.size() == 1 && unidentified.locks.programs.empty() &&
        unidentified.locks.error != ERROR_SUCCESS && Read(retired / L"manifest.json") == manifest,
        "directory-only blocker remains retryable when Restart Manager identifies no program");
    if (directory_lock != INVALID_HANDLE_VALUE) CloseHandle(directory_lock);
    Check(Write(retired / L"personal.txt", "unmanaged"), "insert unknown file into isolated retired fixture");
    const auto unknown = CleanupDialogVersionsReport(root.wstring());
    Check(!unknown.Retryable() && unknown.retained_directories.size() == 21 &&
        Read(retired / L"manifest.json") == manifest && Read(retired / L"personal.txt") == "unmanaged",
        "unknown bundle member preserves entire tree and reports nonretryable reason");
    DeleteFileW((retired / L"personal.txt").c_str());
    Check(Write(root / L".pulse-dialog-previous", "damaged-marker"), "damage only isolated previous marker");
    const auto invalid_marker = CleanupDialogVersionsReport(root.wstring());
    Check(!invalid_marker.scan_complete && invalid_marker.deleted_directories.empty() && fs::exists(retired_dll),
        "invalid previous marker prevents every deletion");
    Write(root / L".pulse-dialog-previous", digests[1]);
    const auto cleaned = CleanupDialogVersionsReport(root.wstring());
    Check(cleaned.deleted_directories.size() == 1 && cleaned.deleted_directories[0] == retired.wstring() &&
        cleaned.protected_directories.size() == 2 && cleaned.retained_directories.size() == 20 && !cleaned.Retryable() &&
        !fs::exists(retired) && fs::exists(root / L"DialogBridge" / Utf8ToWide(digests[1])) &&
        fs::exists(root / L"DialogBridge" / Utf8ToWide(digests[2])),
        "release and retry removes retired version despite twenty retained entries and protects current plus previous");
    const auto corrupted = root / L"DialogBridge" / std::wstring(64, L'0');
    fs::create_directory(corrupted); Write(corrupted / L"manifest.json", "not a valid manifest");
    const auto damaged = CleanupDialogVersionsReport(root.wstring());
    Check(damaged.retained_directories.size() == 21 && Read(corrupted / L"manifest.json") == "not a valid manifest",
        "damaged digest-shaped directory stays untouched with a retention reason");
    const auto again = CleanupDialogVersionsReport(root.wstring());
    Check(again.deleted_directories.empty() && again.protected_directories.size() == 2, "cleanup is idempotent after successful deletion");
    const auto link = root / L"DialogBridge" / std::wstring(64, L'f');
    if (CreateSymbolicLinkW(link.c_str(), retired.parent_path().c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) {
        const auto reparse = CleanupDialogVersionsReport(root.wstring());
        Check(reparse.retained_directories.size() == 22 && reparse.deleted_directories.empty() && fs::exists(link),
            "reparse candidate is retained without traversing its target");
        RemoveDirectoryW(link.c_str());
    } else {
        std::printf("[SKIP] symlink fixture creation unavailable (Win32 %lu)\n", GetLastError());
    }
    const auto oversized = root / L"oversized";
    fs::create_directories(oversized / L"DialogBridge");
    for (unsigned i = 0; i < 4097; ++i) fs::create_directory(oversized / L"DialogBridge" / std::to_wstring(i));
    const auto limited = CleanupDialogVersionsReport(oversized.wstring(), true);
    Check(!limited.scan_complete && limited.deleted_directories.empty() && limited.retained_directories.size() == 1 &&
        limited.retained_directories[0].error == ERROR_FILE_TOO_LARGE && fs::exists(oversized / L"DialogBridge" / L"4096"),
        "oversized candidate set reports a limit before any deletion");
    std::wprintf(L"fixture=%s\n", root.c_str());
    return failures ? 1 : 0;
}
int PublicationLocks() {
    const auto root = fs::absolute(L"bench_data") / (L"dialog-publication-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto app = root / L"app", staging = root / L"staging";
    DialogVersionPlan plan;
    plan.versioned = true;
    plan.digest = std::string(64, 'a');
    plan.relative_directory = L"DialogBridge\\" + Utf8ToWide(plan.digest);
    const auto source = staging / plan.relative_directory;
    const auto target = app / plan.relative_directory;
    fs::create_directories(source);
    fs::create_directories(target.parent_path());
    Check(Write(source / L"payload.dll", "verified staged payload"), "prepare isolated publication fixture");
    HANDLE lock = CreateFileW((source / L"payload.dll").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "hold staged file without delete sharing as a scanner can");
    const bool moved = MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    const DWORD failure = GetLastError();
    Check(!moved && failure == ERROR_ACCESS_DENIED, "staged file lock reproduces directory publication error 5 without an ACL denial");
    std::thread scanner([lock] { Sleep(1500); if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock); });
    std::wstring error;
    const bool published = PublishDialogVersion(app.wstring(), staging.wstring(), plan, error);
    scanner.join();
    Check(published && Read(target / L"payload.dll") == "verified staged payload" && !fs::exists(source),
        "publication survives a transient scan longer than the old 785ms retry window");
    if (!published) std::fwprintf(stderr, L"publish: %s\n", error.c_str());
    const auto blocked_staging = root / L"blocked-staging";
    const auto blocked_app = root / L"blocked-app";
    const auto blocked_source = blocked_staging / plan.relative_directory;
    fs::create_directories(blocked_source);
    fs::create_directories(blocked_app);
    Check(Write(blocked_source / L"payload.dll", "retained staged payload"), "prepare persistent publication lock");
    HANDLE persistent = CreateFileW((blocked_source / L"payload.dll").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(persistent != INVALID_HANDLE_VALUE, "hold persistent publication lock");
    const auto started = GetTickCount64();
    error.clear();
    const bool blocked = !PublishDialogVersion(blocked_app.wstring(), blocked_staging.wstring(), plan, error);
    const auto elapsed = GetTickCount64() - started;
    if (persistent != INVALID_HANDLE_VALUE) CloseHandle(persistent);
    Check(blocked && elapsed >= 10000 && elapsed < 15000 &&
        Read(blocked_source / L"payload.dll") == "retained staged payload" && !fs::exists(blocked_app / plan.relative_directory),
        "persistent lock fails within a bounded wait without moving or deleting payload");
    Check(error.find(blocked_source.wstring()) != error.npos && error.find((blocked_app / plan.relative_directory).wstring()) != error.npos,
        "publication failure identifies source and destination for diagnosis");
    std::wprintf(L"fixture=%s\n", root.c_str());
    return failures ? 1 : 0;
}
}

std::vector<std::wstring> CopyPackage(const fs::path& package, const fs::path& staging) {
    std::vector<std::wstring> files;
    for (const auto& entry : fs::recursive_directory_iterator(package)) {
        if (!entry.is_regular_file()) continue;
        const auto relative = fs::relative(entry.path(), package);
        files.push_back(relative.wstring());
        const auto target = staging / relative;
        fs::create_directories(target.parent_path());
        fs::copy_file(entry.path(), target);
    }
    return files;
}
std::string VersionDigest(const fs::path& app, unsigned machine) {
    pulse::dialog_bundle::BundleLease lease;
    return pulse::dialog_bundle::ResolveInstalledBundleForMachine(app.wstring(), machine, lease) ? std::string() : lease.ManifestDigest();
}
bool ApplyVersionFiles(const fs::path& app, const fs::path& package, bool fail_commit = false, HANDLE* held = nullptr, bool* commit_reached = nullptr) {
    fs::create_directories(app);
    FileTransaction transaction(app.wstring());
    std::wstring error;
    if (!transaction.Begin(error)) return false;
    auto files = CopyPackage(package, transaction.StagingDir());
    DialogVersionPlan plan;
    if (!PrepareDialogVersion(app.wstring(), transaction.StagingDir(), files, plan, error)) { std::fwprintf(stderr, L"prepare: %s\n", error.c_str()); return false; }
    Check(plan.machine == IMAGE_FILE_MACHINE_AMD64 && plan.companions.size() == 1 &&
        plan.companions[0].machine == IMAGE_FILE_MACHINE_I386 && plan.companions[0].companions.empty() &&
        plan.replacements.back() == L"dialog-current.json" &&
        std::find(plan.replacements.begin(), plan.replacements.end(), L"dialog-x86-current.json") != plan.replacements.end(),
        "dual plan has typed one-level companion and primary pointer is final root commit");
    if (!PublishDialogVersion(app.wstring(), transaction.StagingDir(), plan, error)) { std::fwprintf(stderr, L"publish: %s\n", error.c_str()); return false; }
    std::erase(files, L".pulse-files");
    std::string installed;
    for (const auto& file : files) installed += WideToUtf8(file) + "\r\n";
    if (!Write(fs::path(transaction.StagingDir()) / L".pulse-files", installed)) return false;
    std::erase(plan.replacements, L".pulse-files");
    plan.replacements.insert(plan.replacements.end() - 1, L".pulse-files");
    const bool committed = transaction.Commit(plan.replacements, error, {}, [&](std::wstring& reason) {
        if (commit_reached) *commit_reached = true;
        if (!fail_commit) return true;
        if (held) {
            const auto child = app / L"DialogBridge" / Utf8ToWide(plan.companions[0].digest) / L"PulseDialogAuto32.dll";
            *held = CreateFileW(child.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        reason = L"injected dual root commit failure";
        return false;
    });
    if (committed) transaction.Finish();
    return committed;
}
int DualVersions(const fs::path& a, const fs::path& b, const fs::path& c) {
    const auto app = fs::absolute(L"bench_data") /
        (L"dialog-dual-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Check(ApplyVersionFiles(app, a), "install two verified immutable architecture versions");
    if (failures) return 1;
    const auto a64 = VersionDigest(app, IMAGE_FILE_MACHINE_AMD64), a32 = VersionDigest(app, IMAGE_FILE_MACHINE_I386);
    const auto a_directory = app / L"DialogBridge" / Utf8ToWide(a32);
    const auto original_manifest = Read(a_directory / L"manifest.json");
    HANDLE old_reader = CreateFileW((a_directory / L"PulseDialogAuto32.dll").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(!a64.empty() && !a32.empty() && a64 != a32 && old_reader != INVALID_HANDLE_VALUE, "hold actual companion member without loading synthetic image");
    Check(ApplyVersionFiles(app, b), "upgrade both architectures while old companion remains held");
    const auto b64 = VersionDigest(app, IMAGE_FILE_MACHINE_AMD64), b32 = VersionDigest(app, IMAGE_FILE_MACHINE_I386);
    Check(Read(app / L".pulse-dialog-previous") == a64 && Read(app / L".pulse-dialog-x86-previous") == a32,
        "upgrade journals independent previous markers");
    Check(ApplyVersionFiles(app, b) && Read(app / L".pulse-dialog-x86-previous") == a32,
        "same digest reinstall preserves companion previous marker");
    const auto before_main = Read(app / L"dialog-current.json"), before_peer = Read(app / L"dialog-x86-current.json");
    const auto before_marker = Read(app / L".pulse-dialog-x86-previous");
    HANDLE new_reader = INVALID_HANDLE_VALUE;
    Check(!ApplyVersionFiles(app, c, true, &new_reader) && new_reader != INVALID_HANDLE_VALUE,
        "root transaction failure can hold newly published companion");
    Check(Read(app / L"dialog-current.json") == before_main && Read(app / L"dialog-x86-current.json") == before_peer &&
        Read(app / L".pulse-dialog-previous") == a64 && Read(app / L".pulse-dialog-x86-previous") == before_marker,
        "rollback restores both current pointers and both previous markers");
    if (new_reader != INVALID_HANDLE_VALUE) CloseHandle(new_reader);
    const auto single_stage = app / L"single-stage";
    fs::create_directories(single_stage);
    pulse::dialog_bundle::BundleLease primary;
    Check(pulse::dialog_bundle::ResolveInstalledBundle(b.wstring(), primary) == ERROR_SUCCESS, "resolve valid single-update source");
    fs::copy_file(b / L"pulse.exe", single_stage / L"pulse.exe");
    fs::copy_file(b / L"dialog-current.json", single_stage / L"dialog-current.json");
    fs::create_directories(single_stage / L"DialogBridge");
    fs::copy(primary.Directory(), single_stage / L"DialogBridge" / Utf8ToWide(primary.ManifestDigest()), fs::copy_options::recursive);
    primary = {};
    std::vector<std::wstring> single_files;
    for (const auto& entry : fs::recursive_directory_iterator(single_stage))
        if (entry.is_regular_file()) single_files.push_back(fs::relative(entry.path(), single_stage).wstring());
    DialogVersionPlan single_plan; std::wstring error;
    Check(!PrepareDialogVersion(app.wstring(), single_stage.wstring(), single_files, single_plan, error) &&
        !fs::exists(single_stage / L".pulse-dialog-previous") && Read(app / L"dialog-x86-current.json") == before_peer,
        "dual to single update fails before markers or installed pointers change");
    const auto bad_stage = app / L"bad-stage";
    auto bad_files = CopyPackage(c, bad_stage);
    Write(bad_stage / L"dialog-x86-current.json", Read(bad_stage / L"dialog-current.json"));
    DialogVersionPlan bad_plan;
    Check(!PrepareDialogVersion(app.wstring(), bad_stage.wstring(), bad_files, bad_plan, error),
        "companion pointer cannot select primary architecture");
    const auto tampered_stage = app / L"tampered-stage";
    auto tampered_files = CopyPackage(c, tampered_stage);
    DialogVersionPlan tampered_plan;
    Check(PrepareDialogVersion(app.wstring(), tampered_stage.wstring(), tampered_files, tampered_plan, error), "prepare two verified source closures for tamper check");
    const auto tampered_file = tampered_stage / tampered_plan.companions[0].relative_directory / L"pulse.exe";
    { std::ofstream output(tampered_file, std::ios::binary | std::ios::app); output << "tampered"; }
    Check(!PublishDialogVersion(app.wstring(), tampered_stage.wstring(), tampered_plan, error),
        "publication rechecks staged companion hash even when destination can be reused");
    Check(ApplyVersionFiles(app, c), "successful dual update after complete root rollback");
    const auto protected_peer = Read(app / L"dialog-x86-current.json");
    Write(app / L"dialog-x86-current.json", "{}");
    auto damaged = CleanupDialogVersionsReport(app.wstring());
    Check(!damaged.scan_complete && damaged.deleted_directories.empty() && fs::exists(a_directory),
        "damaged companion pointer protects all versions before cleanup");
    Write(app / L"dialog-x86-current.json", protected_peer);
    Write(app / L".pulse-dialog-x86-previous", "bad-marker");
    damaged = CleanupDialogVersionsReport(app.wstring());
    Check(!damaged.scan_complete && damaged.deleted_directories.empty(), "damaged companion previous marker protects all versions");
    Write(app / L".pulse-dialog-x86-previous", b32);
    auto busy = CleanupDialogVersionsReport(app.wstring());
    Check(busy.protected_directories.size() == 4 && busy.blocked_directories.size() == 1 &&
        Read(a_directory / L"manifest.json") == original_manifest && fs::exists(a_directory / L"PulseDialogAuto32.dll"),
        "cleanup protects two current plus two previous and retains complete busy x86 tree");
    if (old_reader != INVALID_HANDLE_VALUE) CloseHandle(old_reader);
    Write(a_directory / L"personal.txt", "unknown member");
    auto unknown = CleanupDialogVersionsReport(app.wstring());
    Check(unknown.deleted_directories.empty() && !unknown.retained_directories.empty() && fs::exists(a_directory / L"personal.txt"),
        "unknown companion member is never removed");
    DeleteFileW((a_directory / L"personal.txt").c_str());
    auto cleaned = CleanupDialogVersionsReport(app.wstring());
    Check(cleaned.deleted_directories.size() == 1 && cleaned.protected_directories.size() == 4 && !fs::exists(a_directory) &&
        VersionDigest(app, IMAGE_FILE_MACHINE_AMD64) == VersionDigest(c, IMAGE_FILE_MACHINE_AMD64) &&
        VersionDigest(app, IMAGE_FILE_MACHINE_I386) == VersionDigest(c, IMAGE_FILE_MACHINE_I386),
        "released retired companion alone is cleaned while both current architectures remain bound");
    std::printf("dual-version failures=%d\n", failures);
    return failures ? 1 : 0;
}

int ThinVersions(const fs::path& a, const fs::path& b, const fs::path& c) {
    const auto app = fs::absolute(L"bench_data") /
        (L"dialog-thin-cleanup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Check(ApplyVersionFiles(app, a), "install verified thin pair");
    if (failures) return 1;
    const auto a64 = VersionDigest(app, IMAGE_FILE_MACHINE_AMD64), a32 = VersionDigest(app, IMAGE_FILE_MACHINE_I386);
    const auto host = app / L"DialogBridge" / Utf8ToWide(a64);
    const auto thin = app / L"DialogBridge" / Utf8ToWide(a32);
    HANDLE reader = CreateFileW((thin / L"PulseDialogAuto32.dll").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(reader != INVALID_HANDLE_VALUE && ApplyVersionFiles(app, b), "upgrade thin pair while old companion is held");
    const auto primary = Read(app / L"dialog-current.json"), companion = Read(app / L"dialog-x86-current.json");
    Check(!ApplyVersionFiles(app, c, true) && Read(app / L"dialog-current.json") == primary &&
        Read(app / L"dialog-x86-current.json") == companion &&
        Read(app / L".pulse-dialog-previous") == a64 && Read(app / L".pulse-dialog-x86-previous") == a32,
        "failed thin upgrade restores both pointers and previous pair");
    Check(ApplyVersionFiles(app, c), "publish thin pair after rollback");
    const auto busy = CleanupDialogVersionsReport(app.wstring());
    Check(busy.blocked_directories.size() == 1 && busy.deleted_directories.empty() &&
        fs::exists(host / L"pulse.exe") && fs::exists(thin / L"manifest.json"),
        "busy thin retains its complete host dependency");
    if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
    Write(thin / L"personal.txt", "unowned");
    const auto unknown = CleanupDialogVersionsReport(app.wstring());
    Check(unknown.deleted_directories.empty() && fs::exists(host / L"pulse.exe") && fs::exists(thin / L"personal.txt"),
        "unknown companion file retains companion and host");
    DeleteFileW((thin / L"personal.txt").c_str());
    const auto valid_manifest = Read(thin / L"manifest.json");
    Check(Write(thin / L"manifest.json", "{}"), "prepare valid JSON with missing dependency schema");
    const auto missing_schema = CleanupDialogVersionsReport(app.wstring());
    Check(!missing_schema.scan_complete && missing_schema.deleted_directories.empty() &&
        fs::exists(host / L"pulse.exe") && fs::exists(thin / L"manifest.json"),
        "missing dependency schema fails closed before deleting any host or companion");
    Check(Write(thin / L"manifest.json", valid_manifest), "restore original manifest after missing-schema check");
    const auto cleaned = CleanupDialogVersionsReport(app.wstring());
    Check(cleaned.deleted_directories.size() == 2 && cleaned.deleted_directories.front() == thin.wstring() &&
        !fs::exists(thin) && !fs::exists(host) && cleaned.protected_directories.size() == 4,
        "retired thin deletes before host with current and previous pairs protected");
    const auto removed = CleanupDialogVersionsReport(app.wstring(), true);
    Check(removed.deleted_directories.size() == 4 && removed.Complete(), "uninstall removes both remaining pairs without orphaning thin bundles");
    std::printf("thin-version failures=%d\n", failures);
    return failures ? 1 : 0;
}

int OrphanRepair(const fs::path& old_package, const fs::path& new_package) {
    const auto root = fs::absolute(L"bench_data") /
        (L"orphan-repair-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto app = root / L"app";
    Check(ApplyVersionFiles(app, old_package), "install old verified dual architecture fixture");
    if (failures) return 1;
    const auto old64 = VersionDigest(app, IMAGE_FILE_MACHINE_AMD64), old32 = VersionDigest(app, IMAGE_FILE_MACHINE_I386);
    const auto missing = app / L"DialogBridge" / Utf8ToWide(old32);
    Check(IsDialogVersionDigest(old32) && missing.lexically_normal().parent_path() == (app / L"DialogBridge").lexically_normal() &&
        app.parent_path() == root, "orphan simulation stays inside owned fixture canonical directory");
    if (failures) return 1;
    fs::remove_all(missing);
    Write(app / L".pulse-dialog-previous", old64);
    Write(app / L".pulse-dialog-x86-previous", old32);
    const std::vector<std::wstring> names{L"dialog-current.json", L"dialog-x86-current.json", L"pulse.exe", L".pulse-files",
        L".pulse-dialog-previous", L".pulse-dialog-x86-previous"};
    std::vector<std::string> original;
    for (const auto& name : names) original.push_back(Read(app / name));
    const auto sentinel = app / L"DialogBridge" / L"unowned" / L"personal.txt";
    fs::create_directories(sentinel.parent_path()); Write(sentinel, "unowned must stay");
    bool reached = false;
    Check(!ApplyVersionFiles(app, new_package, true, nullptr, &reached) && reached,
        "orphan repair reaches transaction commit then injected failure rolls back");
    bool restored = true;
    for (size_t i = 0; i < names.size(); ++i) restored &= Read(app / names[i]) == original[i];
    Check(restored && !fs::exists(missing), "failed repair restores raw pointers main file manifest and both markers");
    Check(ApplyVersionFiles(app, new_package), "verified new dual package repairs missing old x86 version");
    if (failures) { std::printf("orphan-repair failures=%d\n", failures); return 1; }
    const auto new64 = VersionDigest(new_package, IMAGE_FILE_MACHINE_AMD64), new32 = VersionDigest(new_package, IMAGE_FILE_MACHINE_I386);
    Check(VersionDigest(app, IMAGE_FILE_MACHINE_AMD64) == new64 && VersionDigest(app, IMAGE_FILE_MACHINE_I386) == new32,
        "successful repair resolves both newly verified architectures");
    CleanupDialogVersionsReport(app.wstring());
    Check(VersionDigest(app, IMAGE_FILE_MACHINE_AMD64) == new64 && VersionDigest(app, IMAGE_FILE_MACHINE_I386) == new32 &&
        fs::exists(app / L"DialogBridge" / Utf8ToWide(new64) / L"pulse.exe") && Read(sentinel) == "unowned must stay",
        "cleanup retains both current architectures thin host and unowned files");
    const auto damaged = root / L"damaged";
    Check(ApplyVersionFiles(damaged, old_package), "install separate existing-directory corruption fixture");
    const auto damaged_dir = damaged / L"DialogBridge" / Utf8ToWide(old32);
    const auto saved = Read(damaged_dir / L"manifest.json");
    Write(damaged_dir / L"manifest.json", saved + " ");
    Check(!ApplyVersionFiles(damaged, new_package), "existing version with damaged manifest hash never qualifies as orphan");
    Write(damaged_dir / L"manifest.json", saved);
    Check(DeleteFileW((damaged_dir / L"manifest.json").c_str()) != FALSE, "prepare missing manifest inside existing version directory");
    Check(!ApplyVersionFiles(damaged, new_package), "missing manifest inside existing directory is not an orphan pointer");
    Write(damaged_dir / L"manifest.json", saved);
    const auto raw_pointer = Read(damaged / L"dialog-x86-current.json");
    Write(damaged / L"dialog-x86-current.json", "{\"schema\":1,\"directory\":\"../outside\",\"manifest_sha256\":\"" + old32 + "\"}");
    Check(!ApplyVersionFiles(damaged, new_package), "malicious old pointer path remains rejected");
    Write(damaged / L"dialog-x86-current.json", raw_pointer);
    HANDLE locked = CreateFileW((damaged / L"dialog-x86-current.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(locked != INVALID_HANDLE_VALUE && !ApplyVersionFiles(damaged, new_package), "inaccessible old pointer does not qualify as orphan");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    {
        FileTransaction transaction(damaged.wstring());
        std::wstring error;
        Check(transaction.Begin(error), "begin invalid staging verification fixture");
        auto files = CopyPackage(new_package, transaction.StagingDir());
        const auto manifest = fs::path(transaction.StagingDir()) / L"DialogBridge" / Utf8ToWide(new32) / L"manifest.json";
        Write(manifest, Read(manifest) + " ");
        DialogVersionPlan plan;
        Check(!PrepareDialogVersion(damaged.wstring(), transaction.StagingDir(), files, plan, error),
            "orphan repair never weakens complete new staging hash verification");
    }
    std::wprintf(L"orphan-fixture=%s\n", root.c_str());
    std::printf("orphan-repair failures=%d\n", failures);
    return failures ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 4 && std::wstring(argv[1]) == L"--orphan-repair") return OrphanRepair(fs::absolute(argv[2]), fs::absolute(argv[3]));
        if (argc == 5 && std::wstring(argv[1]) == L"--thin-suite") return ThinVersions(fs::absolute(argv[2]), fs::absolute(argv[3]), fs::absolute(argv[4]));
        if (argc == 5 && std::wstring(argv[1]) == L"--dual-suite") return DualVersions(fs::absolute(argv[2]), fs::absolute(argv[3]), fs::absolute(argv[4]));
        if (argc == 5 && std::wstring(argv[1]) == L"--cleanup-report") return CleanupReports(fs::absolute(argv[2]), fs::absolute(argv[3]), fs::absolute(argv[4]));
        if (argc == 2 && std::wstring(argv[1]) == L"--publication-locks") return PublicationLocks();
        if (argc == 5 && std::wstring(argv[1]) == L"--hold") return Hold(argv[2], argv[3], argv[4]);
        if (argc == 4 && std::wstring(argv[1]) == L"--apply-staged") return Apply(fs::absolute(argv[2]), fs::absolute(argv[3])) ? 0 : 1;
        if (argc == 4 && std::wstring(argv[1]) == L"--interrupt") return Apply(fs::absolute(argv[2]), fs::absolute(argv[3]), false, false, true) ? 0 : 1;
        if (argc != 5 || std::wstring(argv[1]) != L"--suite") {
            std::puts("--suite <package-A> <package-B> <package-C> | --apply-staged <isolated-app> <package>"); return 2;
        }
        const fs::path root = fs::absolute(L"bench_data") / (L"dialog-versions-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(root);
        {
            const auto runtime_fixture = root / L"runtime-rollback";
            fs::create_directories(runtime_fixture);
            Write(runtime_fixture / L"pulse.exe", "old-launcher");
            std::string registration = "PRT2old-service-path|old-pin-path";
            RuntimeRegistrationApi api{
                [&](std::string& bytes, std::wstring&) { bytes = registration; return true; },
                [&](const std::string& bytes, std::wstring&) { registration = bytes; return true; }};
            RuntimeRegistrationTransaction reg(runtime_fixture.wstring(), true, api);
            FileTransaction tx(runtime_fixture.wstring(), [&](std::wstring& e) { return reg.Recover(e); });
            std::wstring error;
            Check(tx.Begin(error) && reg.Prepare(error), "persist service and StartPin snapshot before runtime mutation");
            Write(fs::path(tx.StagingDir()) / L"pulse.exe", "new-launcher");
            Check(!tx.Commit({L"pulse.exe"}, error, {}, [&](std::wstring& e) {
                registration = "PRT2new-service-path|new-pin-path"; e = L"injected service registration failure"; return false;
            }) && registration == "PRT2old-service-path|old-pin-path" && Read(runtime_fixture / L"pulse.exe") == "old-launcher",
                "registration failure restores persisted service and pin snapshot with old root executable");
            Check(tx.Begin(error) && reg.Prepare(error), "begin preparation-only interruption with durable empty journal");
            registration = "PRT2temporarily-disabled-custom-account-service";
            Check(FileTransaction::RecoverInterrupted(runtime_fixture.wstring(), error, [&](std::wstring& e) { return reg.Recover(e); }) &&
                registration == "PRT2old-service-path|old-pin-path", "interrupted preparation restores registration before any file commit");
            Check(tx.Begin(error) && reg.Prepare(error), "begin successful commit without cleanup fixture");
            Write(fs::path(tx.StagingDir()) / L"pulse.exe", "committed-launcher");
            Check(tx.Commit({L"pulse.exe"}, error, {}, [&](std::wstring&) {
                registration = "PRT2committed-service-path|committed-pin-path"; return true;
            }), "commit removes journal while retaining snapshot until Finish");
            Check(fs::exists(runtime_fixture / L".pulse-setup" / L"runtime-registration") &&
                !fs::exists(runtime_fixture / L".pulse-setup" / L"journal.txt"), "simulate crash between commit point and cleanup");
            Check(tx.Begin(error) && registration == "PRT2committed-service-path|committed-pin-path" &&
                Read(runtime_fixture / L"pulse.exe") == "committed-launcher",
                "next Begin never restores old registration after committed journal removal");
            tx.Abort();
        }
        const fs::path a = fs::absolute(argv[2]), b = fs::absolute(argv[3]), c = fs::absolute(argv[4]);
        Check(Apply(root, a), "install A through verified immutable publication and root transaction");
        const auto a_dll = Dll(root);
        Holder holder;
        Check(holder.Start(root), "external process holds A DLL and private pulse.exe without delete sharing");
        const auto original_pointer = Read(root / L"dialog-current.json");
        const auto original_main = Read(root / L"pulse.exe");
        const auto original_manifest = Read(root / L".pulse-files");
        const auto old_luma = Read(root / L"lumatext.dll");
        const auto old_pdf = Read(root / L"pdfium.dll");
        Check(!old_luma.empty() && !old_pdf.empty(), "old A fixture actually contains duplicate managed root runtime DLLs");
        Check(Write(root / L"user-owned.dll", "unmanaged-user-file"), "prepare unlisted root file preservation fixture");
        Check(!Apply(root, b, true, true), "registration failure after B is locked reports failure");
        Check(Read(root / L".pulse-files") == original_manifest && Read(root / L"lumatext.dll") == old_luma &&
            Read(root / L"pdfium.dll") == old_pdf, "failed migration restores original installed manifest and retired root runtime DLLs");
        Check(Read(root / L"dialog-current.json") == original_pointer && Read(root / L"pulse.exe") == original_main &&
            Utf8ToWide(Read(root / L".test-registration")) == a_dll, "rollback restores pointer root executable and A registration while new B was locked");
        Check(Apply(root, b) && holder.Alive(), "A to B succeeds without terminating external holder");
        Check(!fs::exists(root / L"lumatext.dll") && !fs::exists(root / L"pdfium.dll") &&
            Read(root / L"user-owned.dll") == "unmanaged-user-file", "shared runtime upgrade retires owned root duplicates and preserves unknown root file");
        const auto previous_b = Read(root / L".pulse-dialog-previous");
        Check(Apply(root, b) && Read(root / L".pulse-dialog-previous") == previous_b && holder.Alive(), "same digest reinstall preserves actual previous version and locked A");
        {
            Holder b_holder;
            Check(b_holder.Start(root), "external process also locks current B");
            Check(Apply(root, b) && b_holder.Alive(), "same digest locked-current reinstall verifies and reuses files");
            Check(Apply(root, c) && holder.Alive() && b_holder.Alive(), "B to C preserves continuously running A and B holders");
        }
        Check(!CleanupDialogVersions(root.wstring()) && fs::exists(a_dll) && holder.Alive(), "bounded cleanup retains busy A and current previous versions");
        const auto extra = fs::path(a_dll).parent_path() / L"personal.txt";
        Check(Write(extra, "not managed"), "add unknown file only in isolated retired fixture");
        holder.Stop();
        Check(!CleanupDialogVersions(root.wstring()) && fs::exists(extra) && fs::exists(a_dll), "cleanup never removes a bundle with additional unknown files");
        DeleteFileW(extra.c_str());
        const auto c_dll = Dll(root);
        Check(CleanupDialogVersions(root.wstring()) && !fs::exists(a_dll) && fs::exists(c_dll) &&
            fs::exists(root / L"DialogBridge" / Utf8ToWide(Read(root / L".pulse-dialog-previous"))), "after hosts exit cleanup removes only retired A and keeps current C plus previous B");
        const auto interrupted = root / L"interrupted";
        Check(Apply(interrupted, a), "prepare interrupted-transaction fixture");
        const auto before = Read(interrupted / L"dialog-current.json");
        const auto crash = RunProcess(Self(), L"--interrupt " + Quote(interrupted.wstring()) + L" " + Quote(b.wstring()), true);
        Check(crash.started && crash.exit_code == 77 && fs::exists(interrupted / L".pulse-setup" / L"journal.txt"), "interrupt after registry publication retains root transaction journal");
        Check(Apply(interrupted, a) && Read(interrupted / L"dialog-current.json") == before &&
            Utf8ToWide(Read(interrupted / L".test-registration")) == Dll(interrupted), "next run recovers interrupted pointer and registration before applying A");
        std::wprintf(L"fixture=%s\n", root.c_str());
        return failures ? 1 : 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 2; }
}

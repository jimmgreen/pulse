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
int wmain(int argc, wchar_t** argv) {
    try {
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

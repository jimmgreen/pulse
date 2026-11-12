#include "setup_dialog_migration.h"
#include "setup_transaction.h"
#include "setup_file_locks.h"
#include "setup_common.h"
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <array>
#include <string_view>
using namespace pulse::setup;
namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* label) { ++checks; if (!ok) ++failures; std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; }
void Write(const std::wstring& path, const std::string& text) { CreateDirs(ParentDir(path)); Check(WriteWholeFile(path, text.data(), text.size()), "write fixture"); }
std::string Read(const std::wstring& path) { std::string text; ReadWholeFile(path, text); return text; }
struct Fixture {
    std::wstring root;
    explicit Fixture(unsigned n) {
        root = std::filesystem::absolute(L"bench_data").wstring() + L"\\dialog-migration-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(n);
        if (!CreateDirectoryW(root.c_str(), nullptr)) throw std::runtime_error("unique fixture directory failed");
        Write(root + L"\\DialogBridge\\pulse.exe", "old-dialog");
        Write(root + L"\\DialogBridge\\personal.txt", "user");
        CreateDirectoryW((root + L"\\DialogBridge\\personal-dir").c_str(), nullptr);
        Write(root + L"\\pulse.exe", "old-main");
        Write(root + L"\\vcruntime140.dll", "old-runtime");
        Write(root + L"\\.pulse-files", "pulse.exe\r\nDialogBridge\\pulse.exe\r\nDialogBridge\\..\\outside\r\nDialogBridge\\x:ads\r\nDialogBridge\\personal-dir\r\nOther\\unlisted.dll\r\n");
    }
    ~Fixture() {
        for (const auto* rel : {L"DialogBridge\\pulse.exe", L"DialogBridge\\personal.txt", L"pulse.exe", L".pulse-files", L"vcruntime140.dll"}) DeleteFileW((root + L"\\" + rel).c_str());
        RemoveDirectoryW((root + L"\\DialogBridge\\personal-dir").c_str());
        RemoveDirectoryW((root + L"\\DialogBridge").c_str());
        RemoveDirectoryW(root.c_str());
    }
};
}

namespace {
using Architecture = pulse::dialog_auto::Architecture;
size_t Index(Architecture architecture) { return architecture == Architecture::X64 ? 0u : 1u; }
void DualArchitectureTests() {
    for (unsigned scenario = 0; scenario != 14; ++scenario) {
        Fixture f(200 + scenario);
        const std::array<std::wstring, 2> old_dir{L"DialogBridge\\" + std::wstring(64, L'a'), L"DialogBridge\\" + std::wstring(64, L'b')};
        const std::array<std::wstring, 2> next_dir{L"DialogBridge\\" + std::wstring(64, L'c'), L"DialogBridge\\" + std::wstring(64, L'd')};
        const std::array<Architecture, 2> architectures{Architecture::X64, Architecture::X86};
        std::array<std::wstring, 2> old_path, next_path, old_rel, next_rel;
        std::array<pulse::dialog_auto::Status, 2> status;
        std::array<unsigned, 2> upgrades{}, restores{};
        bool files_restored_first = true, no_op_restore = scenario == 8;
        std::vector<std::pair<size_t, bool>> operations;
        for (size_t i = 0; i < 2; ++i) {
            old_rel[i] = old_dir[i] + L"\\" + pulse::dialog_auto::DllNameFor(architectures[i]);
            next_rel[i] = next_dir[i] + L"\\" + pulse::dialog_auto::DllNameFor(architectures[i]);
            old_path[i] = f.root + L"\\" + old_rel[i]; next_path[i] = f.root + L"\\" + next_rel[i];
            Write(old_path[i], i == 0 ? "old64" : "old32");
            status[i] = {true, false, ERROR_SUCCESS, old_path[i]};
        }
        if (scenario == 4) status[1].dll_path = L"C:\\OtherInstall\\PulseDialogAuto32.dll";
        if (scenario == 5) status[1].enabled = false;
        if (scenario == 12) { status[0].enabled = false; status[1].enabled = false; }
        const auto initial_status = status;
        DialogMigrationApi api;
        api.inspect_arch = [&](Architecture architecture) { return status[Index(architecture)]; };
        api.enable_arch = [&](Architecture architecture, const std::wstring& path) -> DWORD {
            const size_t i = Index(architecture); const bool upgrading = path == next_path[i];
            operations.emplace_back(i, upgrading);
            if (upgrading) {
                ++upgrades[i];
                Check(Read(path) == (i == 0 ? "new64" : "new32"), "dual migration sees newly installed architecture files");
                if ((scenario == 7 && i == 0) || (scenario == 13 && i == 1)) return ERROR_SUCCESS;
                if (i == 1 && (scenario == 1 || scenario == 2 || scenario == 3 || scenario == 8 || scenario == 9 || scenario == 11)) {
                    status[i].dll_path = scenario == 2 ? L"C:\\Concurrent\\PulseDialogAuto32.dll" : path;
                    status[i].enabled = false;
                    if (scenario == 3) status[0].enabled = false;
                    if (scenario == 9) status[i].error = ERROR_ACCESS_DENIED;
                    return ERROR_ACCESS_DENIED;
                }
            } else {
                ++restores[i];
                files_restored_first &= Read(f.root + L"\\pulse.exe") == "old-main" && Read(old_path[0]) == "old64" && Read(old_path[1]) == "old32";
                if (i == 1 && no_op_restore) return ERROR_SUCCESS;
                if (i == 0 && scenario == 11 && restores[i] == 1) {
                    status[i] = {true, false, 0, L"C:\\Concurrent\\PulseDialogAuto64.dll"}; return ERROR_SUCCESS;
                }
            }
            status[i] = {true, false, 0, path}; return ERROR_SUCCESS;
        };
        DialogLayoutMigration migration(f.root, true, api);
        migration.SetVersionDirectory(next_dir[Index(pulse::dialog_auto::CurrentArchitecture)]);
        migration.SetCompanionVersionDirectory(pulse::dialog_auto::CurrentArchitecture == Architecture::X64 ? Architecture::X86 : Architecture::X64,
            next_dir[Index(pulse::dialog_auto::CurrentArchitecture == Architecture::X64 ? Architecture::X86 : Architecture::X64)]);
        FileTransaction tx(f.root, [&](std::wstring& error) { return migration.Recover(error); });
        std::wstring error;
        Check(tx.Begin(error), "begin dual architecture file transaction");
        Write(tx.StagingDir() + L"\\pulse.exe", "new-main");
        Write(tx.StagingDir() + L"\\" + next_rel[0], "new64");
        Write(tx.StagingDir() + L"\\" + next_rel[1], "new32");
        Check(migration.Prepare(error), "prepare both architecture intents before mutations");
        const auto marker = f.root + L"\\.pulse-setup\\dialog-migration";
        const auto intent = Read(marker);
        Check(scenario == 12 ? intent.empty() : intent.starts_with("4\n"), "dual journal is durable version four or absent when nothing is enabled");
        const bool committed = tx.Commit({L"pulse.exe", next_rel[0], next_rel[1]}, error, {old_rel[0], old_rel[1]}, [&](std::wstring& failure) {
            if (scenario == 6) status[1] = {true, false, 0, L"C:\\Concurrent\\PulseDialogAuto32.dll"};
            if (scenario == 10) status[1].enabled = false;
            return migration.Apply(failure);
        });
        const bool success = scenario == 0 || scenario == 4 || scenario == 5 || scenario == 12;
        Check(committed == success, "dual migration commit result matches the injected architecture matrix");
        if (success) {
            Check(upgrades[0] == (scenario == 12 ? 0u : 1u) && upgrades[1] == (scenario == 0 ? 1u : 0u),
                "disabled or foreign companion is never migrated or enabled");
            if (scenario == 4 || scenario == 5 || scenario == 12)
                Check(status[1].dll_path == initial_status[1].dll_path && status[1].enabled == initial_status[1].enabled, "skipped companion state remains byte-for-byte intended");
            tx.Finish();
        } else {
            Check(Read(f.root + L"\\pulse.exe") == "old-main" && Read(old_path[0]) == "old64" && Read(old_path[1]) == "old32",
                "dual failure restores shared and both old architecture files");
            Check(files_restored_first, "registry recovery occurs only after all original files have restored");
            if (scenario == 1) {
                Check(restores[0] == 1 && restores[1] == 1 && operations.size() == 4 && operations[2] == std::pair<size_t, bool>{1, false} && operations[3] == std::pair<size_t, bool>{0, false},
                    "second architecture failure restores companion then first architecture in reverse order");
            }
            if (scenario == 3) Check(!status[0].enabled && status[0].dll_path == next_path[0] && restores[0] == 0, "completed migration followed by user disable is not re-enabled");
            else if (scenario == 11) Check(status[0].dll_path == L"C:\\Concurrent\\PulseDialogAuto64.dll", "post-restore concurrent replacement is preserved and not falsely verified");
            else Check(status[0].enabled && status[0].dll_path == old_path[0], "first architecture is restored even if the second remains blocked");
            if (scenario == 6) Check(operations.empty() && status[1].dll_path == L"C:\\Concurrent\\PulseDialogAuto32.dll", "pre-apply foreign change prevents all registry mutations");
            if (scenario == 10) Check(operations.empty() && !status[1].enabled, "pre-apply user disable remains disabled");
            if (scenario == 7 || scenario == 13) Check(upgrades[0] == 1 && upgrades[1] == (scenario == 13 ? 1u : 0u), "successful no-op enable cannot falsely commit migration");
            if (scenario == 2 || scenario == 8 || scenario == 9 || scenario == 11) {
                Check(GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES, "blocked or unverified recovery retains durable evidence");
                if (scenario == 2) Check(status[1].dll_path == L"C:\\Concurrent\\PulseDialogAuto32.dll", "failed enable does not overwrite a concurrent foreign registration");
                if (scenario == 8) Check(restores[1] == 1 && !status[1].enabled, "successful no-op restore remains an explicit recovery failure");
                no_op_restore = false;
                if (scenario == 2 || scenario == 9) status[1] = {true, false, 0, old_path[1]};
                if (scenario == 11) status[0] = {true, false, 0, old_path[0]};
                DialogLayoutMigration restarted(f.root, true, api);
                Check(FileTransaction::RecoverInterrupted(f.root, error, [&](std::wstring& failure) { return restarted.Recover(failure); }), "fresh migration object retries the persisted dual journal after files restore");
            }
        }
        Check(GetFileAttributesW((f.root + L"\\.pulse-setup").c_str()) == INVALID_FILE_ATTRIBUTES, "dual transaction evidence clears only after successful recovery or commit");
        for (size_t i = 0; i < 2; ++i) {
            DeleteFileW(old_path[i].c_str()); DeleteFileW(next_path[i].c_str());
            RemoveDirectoryW((f.root + L"\\" + old_dir[i]).c_str()); RemoveDirectoryW((f.root + L"\\" + next_dir[i]).c_str());
        }
    }
    for (unsigned version = 0; version < 4; ++version) {
        Fixture f(300 + version);
        const auto name = std::wstring(pulse::dialog_auto::DllName);
        const auto old_rel = version < 2 ? L"DialogBridge\\" + name : L"DialogBridge\\" + std::wstring(64, L'a') + L"\\" + name;
        const auto next_rel = version < 2 ? name : L"DialogBridge\\" + std::wstring(64, L'b') + L"\\" + name;
        const auto old_path = f.root + L"\\" + old_rel, next_path = f.root + L"\\" + next_rel;
        pulse::dialog_auto::Status status{true, false, 0, version % 2 ? next_path : L"C:\\UserChanged\\different.dll"};
        unsigned enables = 0;
        DialogMigrationApi api{[&] { return status; }, [&](const std::wstring& path) { ++enables; status = {true, false, 0, path}; return ERROR_SUCCESS; }};
        const std::string marker = version < 2 ? (version ? "2\n" : "1\n") : "3\n" + std::string(version % 2 ? "2\n" : "1\n") + WideToUtf8(old_rel) + "\n" + WideToUtf8(next_rel) + "\n";
        Write(f.root + L"\\.pulse-setup\\dialog-migration", marker);
        DialogLayoutMigration migration(f.root, true, api); std::wstring error;
        Check(migration.Recover(error) && enables == (version % 2 ? 1u : 0u), "legacy journal formats one two and three remain recoverable");
        Check(version % 2 ? status.dll_path == old_path && status.enabled : status.dll_path == L"C:\\UserChanged\\different.dll", "legacy prepared journal preserves changes and attempted journal restores native architecture");
        DeleteFileW((f.root + L"\\.pulse-setup\\dialog-migration").c_str()); RemoveDirectoryW((f.root + L"\\.pulse-setup").c_str());
    }
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--dual-architecture") {
        DualArchitectureTests();
        std::cout << checks << " checks, " << failures << " failures\n"; return failures ? 1 : 0;
    }
    if (argc != 1) return 2;
    {
        const std::wstring app = L"C:\\IsolatedPulse";
        const auto old_version = app + L"\\DialogBridge\\" + std::wstring(64,L'a') + L"\\" + pulse::dialog_auto::DllName;
        pulse::dialog_auto::Status status{true,false,0,old_version};
        unsigned calls = 0; std::wstring disabled;
        auto disable = [&](const std::wstring& path)->DWORD { ++calls; disabled=path; return ERROR_SUCCESS; };
        Check(DisableOwnedDialogRegistration(app,status,disable)==ERROR_SUCCESS && calls==1 && disabled==old_version,
            "uninstall disables registered old hash version even when current pointer targets another version");
        status.dll_path=L"C:\\OtherInstall\\DialogBridge\\"+std::wstring(64,L'a')+L"\\"+pulse::dialog_auto::DllName;
        Check(DisableOwnedDialogRegistration(app,status,disable)==ERROR_SUCCESS && calls==1,
            "uninstall preserves foreign dialog registration");
        status.dll_path=app+L"\\DialogBridge\\..\\"+pulse::dialog_auto::DllName;
        Check(DisableOwnedDialogRegistration(app,status,disable)==ERROR_SUCCESS && calls==1,
            "uninstall rejects noncanonical traversal as an owned registration");
        status.error=ERROR_ACCESS_DENIED;
        Check(DisableOwnedDialogRegistration(app,status,disable)==ERROR_ACCESS_DENIED && calls==1,
            "uninstall inspection error blocks deletion instead of assuming registration is absent");
    }
    {
        Fixture f(99);
        for (const auto* name : {L"PulseDialogAuto64.dll", L"PulsePickerUiProof.dll", L"PulseDialogLauncher.exe", L"manifest.json", L"unknown.dll"})
            Write(f.root + L"\\" + name, "old-owned-or-user");
        CreateDirectoryW((f.root + L"\\VERIFIED.json").c_str(), nullptr);
        Write(f.root + L"\\.pulse-files", "pulse.exe\r\nDialogBridge\\pulse.exe\r\nPulseDialogAuto64.dll\r\nmanifest.json\r\nPulseDialogLauncher.exe\r\nVERIFIED.json\r\nunknown.dll\r\n");
        const auto obsolete = LegacyDialogFiles(f.root, {L"pulse.exe", L"PulseDialogLauncher.exe"});
        Check(obsolete.size() == 3 && std::find(obsolete.begin(), obsolete.end(), L"PulseDialogAuto64.dll") != obsolete.end() &&
            std::find(obsolete.begin(), obsolete.end(), L"manifest.json") != obsolete.end(),
            "flat migration selects only retired known bridge names owned by old file manifest");
        Check(std::find(obsolete.begin(), obsolete.end(), L"PulsePickerUiProof.dll") == obsolete.end() &&
            std::find(obsolete.begin(), obsolete.end(), L"unknown.dll") == obsolete.end() &&
            std::find(obsolete.begin(), obsolete.end(), L"PulseDialogLauncher.exe") == obsolete.end() &&
            std::find(obsolete.begin(), obsolete.end(), L"VERIFIED.json") == obsolete.end(),
            "unlisted same-name user file unknown manifest entry retained shipped file and directory stay untouched");
        HANDLE held = CreateFileW((f.root + L"\\PulseDialogAuto64.dll").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(held != INVALID_HANDLE_VALUE && !InspectReplacementLocks(f.root, obsolete).Ready(),
            "first flat migration still reports a real locked root dialog DLL");
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        FileTransaction tx(f.root); std::wstring error;
        Check(tx.Begin(error), "begin flat retirement rollback test");
        Write(tx.StagingDir() + L"\\pulse.exe", "new-main");
        Check(!tx.Commit({L"pulse.exe"}, error, obsolete, [](std::wstring& e) { e = L"injected registration failure"; return false; }) &&
            Read(f.root + L"\\PulseDialogAuto64.dll") == "old-owned-or-user" && Read(f.root + L"\\manifest.json") == "old-owned-or-user" &&
            Read(f.root + L"\\pulse.exe") == "old-main", "registration failure restores retired flat bridge artifacts and root main");
        for (const auto* name : {L"PulseDialogAuto64.dll", L"PulsePickerUiProof.dll", L"PulseDialogLauncher.exe", L"manifest.json", L"unknown.dll"})
            DeleteFileW((f.root + L"\\" + name).c_str());
        RemoveDirectoryW((f.root + L"\\VERIFIED.json").c_str());
    }
    const auto retired = RetiredRuntimeFiles({L"VCRUNTIME140.DLL", L"msvcp140.dll", L"user.dll"});
    Check(retired.size() == 2 && retired[0] == L"msvcp140_atomic_wait.dll" && retired[1] == L"vcruntime140_1.dll", "only unshipped fixed runtime names are retired, case-insensitively");
    for (unsigned scenario = 0; scenario != 9; ++scenario) {
        Fixture f(scenario);
        const auto old = f.root + L"\\DialogBridge\\" + pulse::dialog_auto::DllName;
        const auto next = f.root + L"\\" + pulse::dialog_auto::DllName;
        pulse::dialog_auto::Status status{true, false, 0, old};
        if (scenario == 3) status.dll_path = L"C:\\OtherInstall\\PulseDialogAuto64.dll";
        if (scenario == 4) status.enabled = false;
        unsigned calls = 0;
        bool restore_fails = scenario == 2;
        DialogMigrationApi api{[&] { return status; }, [&](const std::wstring& path) -> DWORD {
            ++calls;
            if (path == next) Check(GetFileAttributesW((f.root + L"\\vcruntime140.dll").c_str()) == INVALID_FILE_ATTRIBUTES, "obsolete runtime removed before registration validates new bundle");
            if (path == next && scenario == 7) { status.dll_path = L"C:\\Changed\\different.dll"; status.enabled = false; return ERROR_ACCESS_DENIED; }
            if (path == next && (scenario == 1 || scenario == 2)) {
                status.dll_path = next; status.enabled = false; return ERROR_ACCESS_DENIED;
            }
            if (path == old && restore_fails) return ERROR_WRITE_FAULT;
            status.dll_path = path; status.enabled = true; return ERROR_SUCCESS;
        }};
        DialogLayoutMigration migration(f.root, true, api);
        FileTransaction tx(f.root, [&](std::wstring& error) { return migration.Recover(error); });
        std::wstring error;
        Check(tx.Begin(error), "begin isolated transaction");
        Write(tx.StagingDir() + L"\\pulse.exe", "new-main");
        const auto old_manifest = Read(f.root + L"\\.pulse-files");
        Write(tx.StagingDir() + L"\\.pulse-files", "pulse.exe\r\n");
        auto obsolete = LegacyDialogFiles(f.root);
        Check(obsolete.size() == 1 && obsolete[0] == L"DialogBridge\\pulse.exe", "only safe manifest-owned legacy file selected");
        obsolete.push_back(L"vcruntime140.dll");
        HANDLE held = INVALID_HANDLE_VALUE;
        if (scenario == 5) {
            held = CreateFileW((f.root + L"\\DialogBridge\\pulse.exe").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            Check(!InspectReplacementLocks(f.root, obsolete).Ready(), "obsolete component participates in preflight");
        }
        Check(migration.Prepare(error), "prepare ownership-checked migration");
        const bool committed = tx.Commit({L"pulse.exe", L".pulse-files"}, error, obsolete, [&](std::wstring& failure) {
            if (scenario == 8) { status.enabled = false; return migration.Apply(failure); }
            if (scenario == 6) { status.dll_path = L"C:\\Changed\\different.dll"; return migration.Apply(failure); }
            return migration.Apply(failure);
        });
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        if (scenario == 0 || scenario == 3 || scenario == 4) {
            Check(committed && Read(f.root + L"\\pulse.exe") == "new-main", "successful migration installs main file");
            Check(GetFileAttributesW((f.root + L"\\DialogBridge\\pulse.exe").c_str()) == INVALID_FILE_ATTRIBUTES, "registered old duplicate removed");
            Check(calls == (scenario == 0 ? 1u : 0u), "only enabled matching registration migrates");
            Check(Read(f.root + L"\\.pulse-files") == "pulse.exe\r\n", "installed manifest excludes removed duplicate");
            tx.Finish();
        } else {
            Check(!committed && Read(f.root + L"\\pulse.exe") == "old-main" && Read(f.root + L"\\DialogBridge\\pulse.exe") == "old-dialog", "failure restores old and removed files");
            if (scenario == 2 || scenario == 7) {
                Check(GetFileAttributesW((f.root + L"\\.pulse-setup\\journal.txt").c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW((f.root + L"\\.pulse-setup\\dialog-migration").c_str()) != INVALID_FILE_ATTRIBUTES, "failed recovery retains journal and marker");
                if (scenario == 7) Check(calls == 1, "foreign registration after failed enable never overwritten");
                restore_fails = false;
                if (scenario == 7) { status.dll_path = old; status.enabled = true; }
                DialogLayoutMigration restarted(f.root, true, api);
                Check(FileTransaction::RecoverInterrupted(f.root, error, [&](std::wstring& failure) { return restarted.Recover(failure); }), "fresh migration object recovers from preserved evidence");
            }
            Check(Read(f.root + L"\\vcruntime140.dll") == "old-runtime", "rollback restores obsolete runtime");
            Check(Read(f.root + L"\\.pulse-files") == old_manifest, "rollback restores old installed manifest");
            if (scenario == 6) Check(calls == 0 && status.dll_path == L"C:\\Changed\\different.dll", "pre-apply foreign registration preserved without recovery mutation");
            else if (scenario == 8) Check(calls == 0 && !status.enabled && status.dll_path == old, "pre-apply user disable is not re-enabled by rollback");
            else Check(status.dll_path == old && status.enabled, "rollback restores enabled original registration");
        }
        Check(Read(f.root + L"\\DialogBridge\\personal.txt") == "user", "unregistered user file preserved");
        Check(GetFileAttributesW((f.root + L"\\.pulse-setup").c_str()) == INVALID_FILE_ATTRIBUTES, "transaction evidence cleaned only after completion");
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}

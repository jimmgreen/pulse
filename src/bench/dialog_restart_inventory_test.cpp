#ifdef PULSE_RESTART_FIXTURE
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
#else
#include "../common/dialog_restart_inventory.h"
#include <filesystem>
#include <cstdio>
using namespace pulse;
namespace fs = std::filesystem;
namespace {
int checks = 0, failures = 0;
void Check(bool value, const char* text) { ++checks; if (!value) ++failures; std::printf("[%s] %s\n", value ? "PASS" : "FAIL", text); }
dialog_diagnostics::RestartProcessSnapshot Model(const std::wstring& image, const std::wstring& module, DWORD pid = 100) {
    dialog_diagnostics::RestartProcessSnapshot s; s.image_path = image; s.module_paths = {module}; s.same_user_session = true;
    s.process.pid = pid; s.process.creation_time = 1234; s.process.identity_verified = true; s.process.process_arch = 64;
    s.process.module_status = dialog_diagnostics::ModuleStatus::Complete; return s;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const auto root = fs::absolute(argv[2]); if (fs::exists(root)) return 3; fs::create_directories(root);
    dialog_restart::CurrentComponents current;
    current.install_root = root.wstring(); current.x64_directory = (root / L"DialogBridge" / std::wstring(64, L'a')).wstring();
    current.x86_directory = (root / L"DialogBridge" / std::wstring(64, L'b')).wstring(); current.x64_error = current.x86_error = 0;
    const auto old = root / L"DialogBridge" / std::wstring(64, L'c');
    const auto image = (root / L"owned-app.exe").wstring(); const auto dll = L"PulseDialogAuto64.dll";
    Check(dialog_restart::ProcessOrder({30,10,20,20},20) == std::vector<DWORD>({30,10,20}), "bounded scan cursor starts after prior PID and wraps once");
    Check(dialog_restart::ProcessOrder({30,10,20},30) == std::vector<DWORD>({10,20,30}), "tail cursor wraps fairly to beginning");
    Check(dialog_restart::ProcessOrder({30,10,20},25) == std::vector<DWORD>({30,10,20}), "vanished cursor PID does not reset and starve tail");
    auto s = Model(image, (old / dll).wstring());
    auto result = dialog_restart::Classify(current, {s});
    Check(result.scan_complete && result.records.size() == 1 && result.records[0].old_component_count == 1, "model old version actual module path requires restart");
    auto now = s; now.module_paths = {(fs::path(current.x64_directory) / dll).wstring()};
    Check(dialog_restart::Classify(current, {now}).records.empty(), "same current path never guesses stale in-memory bytes");
    auto foreign = s; foreign.module_paths = {(root / L"other-install" / L"DialogBridge" / std::wstring(64,L'c') / dll).wstring()};
    Check(dialog_restart::Classify(current, {foreign}).records.empty(), "same DLL name outside installed root ignored");
    auto error = s; error.process.module_status = dialog_diagnostics::ModuleStatus::AccessDenied; error.process.module_error = ERROR_ACCESS_DENIED;
    result = dialog_restart::Classify(current, {error});
    Check(result.records.empty() && !result.scan_complete && result.query_errors == 1, "access failure is unknown not restart candidate");
    error = s; error.process.identity_verified = false;
    Check(dialog_restart::Classify(current, {error}).records.empty(), "PID identity changed or exited never attributed");
    error = s; error.same_user_session = false;
    Check(dialog_restart::Classify(current, {error}).records.empty(), "different user or session ignored");
    error = s; error.process.module_status = dialog_diagnostics::ModuleStatus::Partial;
    Check(dialog_restart::Classify(current, {error}).records.empty(), "partial architecture/module observation not certified");
    result = dialog_restart::Classify(current, {s,s});
    Check(result.records.size() == 1 && result.records[0].processes.size() == 1 && result.records[0].old_component_count == 1, "duplicate PID plus creation is counted once");
    auto peer = s; peer.process.pid = 101;
    result = dialog_restart::Classify(current, {s,peer});
    Check(result.records.size() == 1 && result.records[0].processes.size() == 2, "same full app identity aggregates multiple processes");
    peer.image_path = (root / L"different-path" / L"owned-app.exe").wstring();
    result = dialog_restart::Classify(current, {s,peer});
    Check(result.records.size() == 2 && result.records[0].app_image_id != result.records[1].app_image_id, "same basename at different application paths stays distinct");
    auto unavailable = current; unavailable.x64_error = ERROR_FILE_NOT_FOUND;
    result = dialog_restart::Classify(unavailable, {s});
    Check(result.records.empty() && !result.scan_complete, "unverified current bundle cannot produce restart claim");
    auto hidden = s; hidden.image_path = (root / L"owned\u202eapp.exe").wstring();
    result = dialog_restart::Classify(current, {hidden});
    Check(result.records.empty() && !result.scan_complete, "bidi application display name cannot spoof restart list");
    unavailable.x86_error = ERROR_FILE_NOT_FOUND;
    result = dialog_restart::Scan(unavailable);
    Check(!result.scan_complete && result.scanned_processes == 0, "no verified current architecture skips process scan");
    auto x86 = s; x86.process.process_arch = 32; x86.module_paths = {(old / L"PulseDialogAuto32.dll").wstring()};
    Check(dialog_restart::Classify(current, {x86}).records.size() == 1, "x86 modules use verified x86 current directory");
    result = dialog_restart::Scan(current, [] { return true; });
    Check(result.cancelled && result.records.empty(), "cancelled production scan returns no partial candidates");
    fs::create_directories(old); fs::create_directories(current.x64_directory); fs::create_directories(current.x86_directory);
    const wchar_t* native_dll = sizeof(void*) == 8 ? L"PulseDialogAuto64.dll" : L"PulseDialogAuto32.dll";
    const auto loaded_old = old / native_dll; fs::copy_file(argv[1], loaded_old);
    HMODULE module = LoadLibraryW(loaded_old.c_str());
    auto observed = dialog_diagnostics::CaptureRestartProcess(GetCurrentProcessId());
    result = dialog_restart::Classify(current, {observed});
    Check(module && observed.same_user_session && observed.process.identity_verified && result.records.size() == 1 && result.records[0].old_component_count == 1, "native owned inert LoadLibrary old-version module observed");
    const auto exported = dialog_diagnostics::Serialize(observed.process);
    Check(!observed.image_path.empty() && exported.find(fs::path(observed.image_path).filename().string()) == exported.npos &&
        exported.find(fs::path(observed.image_path).string()) == exported.npos && exported.find(loaded_old.string()) == exported.npos &&
        exported.find("PulseDialogAuto64.dll") == exported.npos && exported.find("PulseDialogAuto32.dll") == exported.npos,
        "default serialization excludes local image and module paths and executable basename");
    if (module) FreeLibrary(module);
    const auto current_path = fs::path(sizeof(void*) == 8 ? current.x64_directory : current.x86_directory) / native_dll;
    fs::copy_file(argv[1], current_path); module = LoadLibraryW(current_path.c_str());
    observed = dialog_diagnostics::CaptureRestartProcess(GetCurrentProcessId());
    Check(module && dialog_restart::Classify(current, {observed}).records.empty(), "native owned current-version module not restart candidate");
    if (module) FreeLibrary(module);
    const auto foreign_path = root / L"other-install" / native_dll; fs::create_directories(foreign_path.parent_path()); fs::copy_file(argv[1], foreign_path); module = LoadLibraryW(foreign_path.c_str());
    observed = dialog_diagnostics::CaptureRestartProcess(GetCurrentProcessId());
    Check(module && dialog_restart::Classify(current, {observed}).records.empty(), "native same-name foreign DLL ignored");
    if (module) FreeLibrary(module);
    std::printf("Checks: %d, failures: %d\n", checks, failures); return failures ? 1 : 0;
}
#endif

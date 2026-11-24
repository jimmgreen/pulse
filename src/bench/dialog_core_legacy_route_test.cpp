// Real Core DLL, stub-managed: GetOpen/SaveFileName A/W and SHBrowseForFolder A/W
// sink hooks. This executable is also the picker host the Core launches
// (--picker-host): it answers without UI, so every Pulse takeover is checked by
// its result. Dialogs run on a private desktop; native fallbacks are cancelled.
//
//   pulse_dialog_core_legacy_route_test.exe <PulseDialogCore64.dll>
#include "../shell_bridge/dialog_core_abi.h"
#include "../common/dialog_auto_registration.h"
#include "../ipc/picker_session.h"

#include <windows.h>
#include <commdlg.h>
#include <cderr.h>
#include <shlobj.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>

namespace fs = std::filesystem;
namespace picker = pulse::ipc::picker;

namespace {
unsigned failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

// The test process publishes the fixture folder; the host it spawns inherits it.
constexpr wchar_t kFixtureVariable[] = L"PULSE_CORE_LEGACY_FIXTURE";
fs::path FixtureDir(DWORD) {
    wchar_t value[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(kFixtureVariable, value, MAX_PATH);
    return length && length < MAX_PATH ? fs::path(value) : fs::path();
}

// ---- fake picker host ----
int Host(const wchar_t* pipe, const wchar_t* parent_pid) {
    const DWORD owner = wcstoul(parent_pid, nullptr, 10);
    picker::PipeHandle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, owner));
    picker::Channel channel;
    if (!parent || !picker::Connect(pipe, parent.Get(), channel)) return 2;
    picker::Message request;
    if (!picker::ReceivePickerSessionRequest(channel, request, {nullptr, 5000})) return 3;
    const auto& body = std::get<picker::Request>(request.body);
    picker::Response answer;
    answer.operation = body.operation;
    const bool cancel = GetEnvironmentVariableW(L"PULSE_CORE_LEGACY_CANCEL", nullptr, 0) != 0;
    answer.status = cancel ? picker::Status::Cancelled : picker::Status::Accepted;
    const auto dir = FixtureDir(owner);
    if (answer.status == picker::Status::Accepted) {
        const fs::path chosen = body.operation == picker::Operation::PickFolder ? dir :
            body.operation == picker::Operation::SaveFile ? dir / L"saved.txt" : dir / L"pick.txt";
        answer.paths = {chosen.u16string()};
    }
    return channel.Send(picker::Message{request.request_id, answer}, {nullptr, 5000}) ? 0 : 4;
}

// ---- test process ----
HDESK g_desktop = nullptr;
std::atomic<bool> g_stop{false};
std::atomic<unsigned> g_native{0};
std::set<HWND> g_seen_native;

BOOL CALLBACK CancelDialogs(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t cls[64]{};
    if (pid == GetCurrentProcessId() && GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"#32770") == 0 && IsWindowVisible(hwnd)) {
        PostMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
        if (g_seen_native.insert(hwnd).second) ++g_native;
    }
    return TRUE;
}
void Watchdog() {
    while (!g_stop.load()) {
        EnumDesktopWindows(g_desktop, CancelDialogs, 0);
        Sleep(150);
    }
}

std::atomic<unsigned> g_hook_calls{0};
UINT_PTR CALLBACK OfnHook(HWND, UINT message, WPARAM, LPARAM) {
    if (message == WM_INITDIALOG) ++g_hook_calls;
    return 0;
}
std::atomic<unsigned> g_browse_cb_calls{0};
int CALLBACK BrowseCallback(HWND, UINT message, LPARAM, LPARAM) {
    if (message == BFFM_INITIALIZED) ++g_browse_cb_calls;
    return 0;
}
std::wstring PidlPath(PCIDLIST_ABSOLUTE pidl) {
    wchar_t path[MAX_PATH]{};
    return pidl && SHGetPathFromIDListW(pidl, path) ? path : L"";
}
bool SamePath(const std::wstring& a, const fs::path& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

void RunCases(const std::wstring& core_path, const std::wstring& self) {
    const auto dir = FixtureDir(GetCurrentProcessId());
    std::error_code created;
    fs::create_directories(dir, created);
    { std::ofstream(dir / L"pick.txt") << "fixture"; }
    Check(!created && fs::exists(dir / L"pick.txt"), "isolated fixture folder");

    g_desktop = CreateDesktopW((L"PulseCoreLegacy" + std::to_wstring(GetCurrentProcessId())).c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    Check(g_desktop && SetThreadDesktop(g_desktop), "private desktop for every dialog");
    if (!g_desktop) return;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::thread watchdog(Watchdog);

    const auto bridge_root = dir / L"bridge_root";
    fs::create_directories(bridge_root, created);
    { std::ofstream(bridge_root / L"active-bridge.json") << "{\"schema\":1,\"generation\":1,\"disabled\":false}"; }

    HMODULE core = LoadLibraryW(core_path.c_str());
    const auto query = core ? reinterpret_cast<PulseDialogCoreQueryV1Fn>(GetProcAddress(core, "PulseDialogCoreQueryV1")) : nullptr;
    PulseDialogCoreV1 table{};
    table.size = sizeof(table);
    Check(query && SUCCEEDED(query(&table)) && table.initialize && table.get_active_hook_mask, "core loads and exposes its table");
    if (table.initialize) {
        PulseStubConfigV1 config{sizeof(config), PULSE_DIALOG_CORE_ABI_VERSION, PULSE_STUB_FLAG_INSTALL_SINK_HOOKS, 0, 1,
            bridge_root.c_str(), L"", self.c_str()};
        Check(SUCCEEDED(table.initialize(&config)), "stub-managed initialize installs sink hooks");
        const uint32_t mask = table.get_active_hook_mask();
        Check((mask & 0x1fu) == 0x1fu, "ClassObject, OpenSave W/A, extended error and SHBrowse sinks are active");
    }

    // Basic requests use the pulse.exe route this host answers. OFN_NOCHANGEDIR,
    // OFN_HIDEREADONLY and OFN_FILEMUSTEXIST avoid the working-directory events, the
    // read-only checkbox and missing-file Open, which need the remote V2 UI host
    // (pulse.exe --picker-remote-host).
    // Plain W Open is taken over: the host's file comes back, no native window.
    {
        wchar_t file[MAX_PATH]{};
        OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        const unsigned native = g_native.load();
        const BOOL ok = GetOpenFileNameW(&ofn);
        Check(ok && SamePath(file, dir / L"pick.txt") && g_native.load() == native,
            "GetOpenFileNameW returns the Pulse host's file without a native window");
        Check(ok && ofn.nFileOffset > 0 && SamePath(file + ofn.nFileOffset, L"pick.txt"), "GetOpenFileNameW sets nFileOffset");
        Check(CommDlgExtendedError() == 0, "CommDlgExtendedError is 0 after a Pulse acceptance");
    }
    // Pulse cancel stays a cancel: FALSE, no extended error, no native retry.
    {
        wchar_t file[MAX_PATH]{};
        OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        SetEnvironmentVariableW(L"PULSE_CORE_LEGACY_CANCEL", L"1");  // inherited by the host
        const unsigned native = g_native.load();
        const BOOL ok = GetOpenFileNameW(&ofn);
        SetEnvironmentVariableW(L"PULSE_CORE_LEGACY_CANCEL", nullptr);
        Check(!ok && CommDlgExtendedError() == 0 && g_native.load() == native, "Pulse cancel returns FALSE without a native retry");
    }
    // A Save.
    {
        char file[MAX_PATH]{};
        OPENFILENAMEA ofn{}; ofn.lStructSize = sizeof(ofn); ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        const unsigned native = g_native.load();
        const BOOL ok = GetSaveFileNameA(&ofn);
        wchar_t wide[MAX_PATH]{};
        MultiByteToWideChar(CP_ACP, 0, file, -1, wide, MAX_PATH);
        Check(ok && SamePath(wide, dir / L"saved.txt") && g_native.load() == native,
            "GetSaveFileNameA returns the Pulse host's save path without a native window");
    }
    // SHBrowseForFolderW.
    {
        wchar_t name[MAX_PATH]{};
        BROWSEINFOW bi{}; bi.pszDisplayName = name; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        const unsigned native = g_native.load();
        PIDLIST_ABSOLUTE result = SHBrowseForFolderW(&bi);
        Check(result && SamePath(PidlPath(result), dir) && g_native.load() == native,
            "SHBrowseForFolderW returns the Pulse host's folder without a native window");
        if (result) CoTaskMemFree(result);
    }
    // SHBrowseForFolderW with BIF_NONEWFOLDERBUTTON + callback requires V2; must NOT
    // silently drop callback/policy onto the basic pulse.exe bridge route.
    {
        wchar_t name[MAX_PATH]{};
        BROWSEINFOW bi{}; bi.pszDisplayName = name;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
        bi.lpfn = BrowseCallback;
        g_browse_cb_calls = 0;
        const unsigned native = g_native.load();
        PIDLIST_ABSOLUTE result = SHBrowseForFolderW(&bi);
        Check(!result && g_browse_cb_calls.load() >= 1 && g_native.load() > native,
            "SHBrowseForFolderW with callback/hide-new-folder preserves V2 policy and falls back to native callback");
        if (result) CoTaskMemFree(result);
    }
    // Live disabled:true in active-bridge.json revokes legacy sink takeover without unloading Core.
    {
        { std::ofstream(bridge_root / L"active-bridge.json", std::ios::trunc) << "{\"schema\":1,\"generation\":2,\"disabled\":true}"; }
        Sleep(300);  // past 250 ms pointer cache TTL
        wchar_t file[MAX_PATH]{};
        OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        const unsigned native = g_native.load();
        const BOOL ok = GetOpenFileNameW(&ofn);
        Check(!ok && g_native.load() == native + 1,
            "active-bridge.json disabled:true revokes GetOpenFileNameW takeover live");
        { std::ofstream(bridge_root / L"active-bridge.json", std::ios::trunc) << "{\"schema\":1,\"generation\":3,\"disabled\":false}"; }
        Sleep(300);
    }
    // Per-user HKCU Enabled=0 opt-out revokes Stub+Core takeover immediately without UAC.
    {
        const std::wstring sandbox = L"Software\\PulseCoreLegacyTest_" + std::to_wstring(GetCurrentProcessId());
        HKEY isolated = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, sandbox.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &isolated, nullptr) == ERROR_SUCCESS &&
            RegOverridePredefKey(HKEY_CURRENT_USER, isolated) == ERROR_SUCCESS) {
            HKEY cfg = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, pulse::dialog_auto::ConfigKey, 0, nullptr, 0,
                                KEY_ALL_ACCESS | pulse::dialog_auto::RegistryView, nullptr, &cfg, nullptr) == ERROR_SUCCESS) {
                const DWORD zero = 0;
                RegSetValueExW(cfg, L"Enabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
                wchar_t file[MAX_PATH]{};
                OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
                const unsigned native = g_native.load();
                const BOOL ok = GetOpenFileNameW(&ofn);
                Check(!ok && g_native.load() == native + 1,
                    "per-user HKCU Enabled=0 opts out of Stub+Core takeover live");
                const DWORD one = 1;
                RegSetValueExW(cfg, L"Enabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
                wchar_t file2[MAX_PATH]{};
                OPENFILENAMEW ofn2{}; ofn2.lStructSize = sizeof(ofn2); ofn2.lpstrFile = file2; ofn2.nMaxFile = MAX_PATH;
                ofn2.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
                const unsigned native2 = g_native.load();
                const BOOL ok2 = GetOpenFileNameW(&ofn2);
                Check(ok2 && SamePath(file2, dir / L"pick.txt") && g_native.load() == native2,
                    "per-user HKCU Enabled=1 restores Stub+Core takeover live");
                RegCloseKey(cfg);
            } else {
                Check(false, "create isolated DialogAuto config key");
            }
            RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
            RegCloseKey(isolated);
            RegDeleteTreeW(HKEY_CURRENT_USER, sandbox.c_str());
        } else {
            if (isolated) RegCloseKey(isolated);
            Check(false, "isolate HKCU for per-user opt-out check");
        }
    }
    // Default flags (working-directory events, read-only checkbox) need the remote
    // V2 UI host, which this test host does not implement: the original API must run.
    {
        wchar_t file[MAX_PATH]{};
        OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER;
        const unsigned native = g_native.load();
        const BOOL ok = GetOpenFileNameW(&ofn);
        Check(!ok && CommDlgExtendedError() == 0 && g_native.load() == native + 1,
            "custom-control Open without a V2 host falls back to exactly one native dialog");
    }
    // Unsupported structure: declined before UI, the original API keeps its own error.
    {
        wchar_t file[MAX_PATH]{};
        OPENFILENAMEW ofn{}; ofn.lStructSize = 0; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
        const unsigned native = g_native.load();
        Check(!GetOpenFileNameW(&ofn) && CommDlgExtendedError() == CDERR_STRUCTSIZE && g_native.load() == native,
            "bad structure size returns native CDERR_STRUCTSIZE without a window");
    }
    // Hook customization: no verified hook host, so the untouched OPENFILENAME
    // reaches the original API, whose hook procedure runs.
    {
        wchar_t file[MAX_PATH]{};
        OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_ENABLEHOOK; ofn.lpfnHook = OfnHook;
        g_hook_calls = 0;
        const unsigned native = g_native.load();
        const BOOL ok = GetOpenFileNameW(&ofn);
        Check(!ok && CommDlgExtendedError() == 0 && g_hook_calls.load() == 1 && g_native.load() > native,
            "hook dialog falls back to the original API and its hook runs once");
    }

    g_stop = true;
    watchdog.join();
    if (table.shutdown) table.shutdown();
    const auto core_name = fs::path(core_path).filename().wstring();
    if (core) FreeLibrary(core);
    // Stub-managed Core must not pin itself or leave legacy DirectReveal detours into freed pages.
    Check(GetModuleHandleW(core_name.c_str()) == nullptr, "stub-managed Core unloads cleanly via FreeLibrary");
    (void)SHOpenFolderAndSelectItems(nullptr, 0, nullptr, 0);
    Check(true, "shell32 DirectReveal entry remains safe after Core FreeLibrary");
    CoUninitialize();
    std::error_code ignored;
    fs::remove_all(dir, ignored);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && std::wstring(argv[1]) == L"--picker-host") return Host(argv[2], argv[3]);
    if (argc != 2) { std::puts("usage: pulse_dialog_core_legacy_route_test <PulseDialogCore.dll>"); return 2; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const auto fixture = fs::absolute(L"bench_data") / (L"core-legacy-route-" + std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(kFixtureVariable, fixture.c_str());
    // Window creation must start on a thread that owns no windows yet.
    std::thread worker(RunCases, std::wstring(argv[1]), std::wstring(self));
    worker.join();
    // The Core writes this process's diagnostics file; keep it only as failure evidence.
    wchar_t local[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) {
        const auto log = fs::path(local) / L"Pulse\\Diagnostics\\Runtime" / (L"dialog_auto-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
        if (failures) std::printf("log=%ls\n", log.c_str());
        else { std::error_code ignored; fs::remove(log, ignored); }
    }
    std::printf("CORE_LEGACY_ROUTE failures=%u\n", failures);
    return failures ? 1 : 0;
}

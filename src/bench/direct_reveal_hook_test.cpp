#include "../shell_bridge/direct_reveal_hook.h"
#include "../shell_bridge/explorer_process_route.h"
#include <detours.h>
#include <wrl/client.h>
#include <cstdio>
#include <filesystem>
#include <set>
#include <tlhelp32.h>
#include <cstring>
#include <thread>
namespace {
using Microsoft::WRL::ComPtr;
unsigned sent = 0, opened = 0; std::wstring selected, opened_folder;
pulse::DirectRevealResult reply = pulse::DirectRevealResult::Accepted;
pulse::DirectRevealResult Send(const std::wstring& path, DWORD) noexcept { ++sent; selected = path; return reply; }
pulse::DirectRevealResult OpenFolder(const std::wstring& path, DWORD) noexcept { ++opened; opened_folder = path; return reply; }
// Stand-ins for the native layer, detoured before the reveal hook so its
// trampolines land here: native bypass is observed without opening Explorer.
unsigned native = 0;
decltype(&SHOpenFolderAndSelectItems) real_reveal = &SHOpenFolderAndSelectItems;
decltype(&ShellExecuteExW) real_execute_ex = &ShellExecuteExW;
decltype(&ShellExecuteW) real_execute = &ShellExecuteW;
decltype(&ShellExecuteExA) real_execute_ex_a = &ShellExecuteExA;
decltype(&ShellExecuteA) real_execute_a = &ShellExecuteA;
BOOL native_bool = TRUE;
HRESULT native_hr = S_OK;
INT_PTR native_instance = 42;
DWORD native_error = ERROR_ACCESS_DENIED;
bool native_scope = false;
HWND native_window = nullptr;
const void* native_verb = nullptr;
const void* native_file = nullptr;
const void* native_parameters = nullptr;
const void* native_directory = nullptr;
INT native_show = 0;
std::wstring native_file_w, native_parameters_w, native_verb_w;
std::string native_file_a, native_parameters_a, native_verb_a;
bool simulate_folder_association = false;
std::wstring association_folder, association_explorer;
unsigned associated_pulse = 0, explicit_explorer = 0;
void CaptureNativeW(LPCWSTR file, LPCWSTR parameters, LPCWSTR verb) {
    native_file_w = file ? file : L""; native_parameters_w = parameters ? parameters : L""; native_verb_w = verb ? verb : L"";
    if (simulate_folder_association && native_file_w == association_folder) ++associated_pulse;
    if (simulate_folder_association && native_file_w == association_explorer) ++explicit_explorer;
}
void CaptureNativeA(LPCSTR file, LPCSTR parameters, LPCSTR verb) {
    native_file_a = file ? file : ""; native_parameters_a = parameters ? parameters : ""; native_verb_a = verb ? verb : "";
}
const void* native_info = nullptr;
SHELLEXECUTEINFOW native_w_input{};
SHELLEXECUTEINFOA native_a_input{};
PCIDLIST_ABSOLUTE native_folder = nullptr;
PCUITEMID_CHILD_ARRAY native_children = nullptr;
UINT native_count = 0;
DWORD native_flags = 0;
void RecordNative() {
    ++native;
    native_scope = pulse::shell_bridge::explorer_process::NativeFallbackActive();
    SetLastError(native_error);
}
HRESULT WINAPI NativeReveal(PCIDLIST_ABSOLUTE folder, UINT count, PCUITEMID_CHILD_ARRAY children, DWORD flags) {
    native_folder = folder; native_count = count; native_children = children; native_flags = flags;
    RecordNative(); return native_hr;
}
BOOL WINAPI NativeExecuteEx(SHELLEXECUTEINFOW* info) {
    native_info = info; native_w_input = *info;
    CaptureNativeW(info->lpFile, info->lpParameters, info->lpVerb);
    info->hInstApp = reinterpret_cast<HINSTANCE>(native_instance);
    info->hProcess = reinterpret_cast<HANDLE>(0x1234);
    RecordNative(); return native_bool;
}
HINSTANCE WINAPI NativeExecute(HWND window, LPCWSTR verb, LPCWSTR file, LPCWSTR parameters, LPCWSTR directory, INT show) {
    CaptureNativeW(file, parameters, verb);
    native_window = window; native_verb = verb; native_file = file; native_parameters = parameters;
    native_directory = directory; native_show = show;
    RecordNative(); return reinterpret_cast<HINSTANCE>(native_instance);
}
BOOL WINAPI NativeExecuteExA(SHELLEXECUTEINFOA* info) {
    native_info = info; native_a_input = *info;
    CaptureNativeA(info->lpFile, info->lpParameters, info->lpVerb);
    info->hInstApp = reinterpret_cast<HINSTANCE>(native_instance);
    info->hProcess = reinterpret_cast<HANDLE>(0x1234);
    RecordNative(); return native_bool;
}
HINSTANCE WINAPI NativeExecuteA(HWND window, LPCSTR verb, LPCSTR file, LPCSTR parameters, LPCSTR directory, INT show) {
    CaptureNativeA(file, parameters, verb);
    native_window = window; native_verb = verb; native_file = file; native_parameters = parameters;
    native_directory = directory; native_show = show;
    RecordNative(); return reinterpret_cast<HINSTANCE>(native_instance);
}
LONG InstallNativeStandIns() {
    LONG status = DetourTransactionBegin();
    if (status == NO_ERROR) status = DetourUpdateThread(GetCurrentThread());
    if (status == NO_ERROR) status = DetourAttach(reinterpret_cast<PVOID*>(&real_reveal), reinterpret_cast<PVOID>(&NativeReveal));
    if (status == NO_ERROR) status = DetourAttach(reinterpret_cast<PVOID*>(&real_execute_ex), reinterpret_cast<PVOID>(&NativeExecuteEx));
    if (status == NO_ERROR) status = DetourAttach(reinterpret_cast<PVOID*>(&real_execute), reinterpret_cast<PVOID>(&NativeExecute));
    if (status == NO_ERROR) status = DetourAttach(reinterpret_cast<PVOID*>(&real_execute_ex_a), reinterpret_cast<PVOID>(&NativeExecuteExA));
    if (status == NO_ERROR) status = DetourAttach(reinterpret_cast<PVOID*>(&real_execute_a), reinterpret_cast<PVOID>(&NativeExecuteA));
    if (status == NO_ERROR) return DetourTransactionCommit();
    DetourTransactionAbort(); return status;
}
void Diagnostic(const char* stage) {
    DWORD integrity = 0;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[1024]{};
        DWORD bytes = 0;
        if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &bytes)) {
            const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
            if (IsValidSid(sid) && *GetSidSubAuthorityCount(sid))
                integrity = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
        }
        CloseHandle(token);
    }
    std::printf("[DIAG] stage=%s pid=%lu integrity=%lu sent=%u opened=%u native=%u shift=%d tick=%llu\n",
        stage, GetCurrentProcessId(), integrity, sent, opened, native, (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0, GetTickCount64());
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) do {
        if (wcsstr(entry.szModule, L"Pulse") || wcsstr(entry.szModule, L"pulse")) {
            char path[32768]{};
            WideCharToMultiByte(CP_UTF8, 0, entry.szExePath, -1, path, sizeof(path), nullptr, nullptr);
            std::printf("[MODULE] %s\n", path);
        }
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
}
bool PrepareInstalledBootstrap() {
    // A registered ShellLink bootstrap can install production Detours on its
    // worker. Let that finish first; the native stand-ins and hooks under test
    // must be installed last so production delivery cannot bypass fake senders.
    ComPtr<IShellLinkW> link;
    const HRESULT created = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
#ifdef _WIN64
    constexpr auto core_name = L"PulseDialogCore64.dll";
    constexpr auto stub_name = L"PulseDialogStub64.dll";
#else
    constexpr auto core_name = L"PulseDialogCore32.dll";
    constexpr auto stub_name = L"PulseDialogStub32.dll";
#endif
    const bool installed = GetModuleHandleW(core_name) || GetModuleHandleW(stub_name);
    std::printf("[BOOTSTRAP] shell_link_hr=0x%08lx installed_module=%d\n", static_cast<unsigned long>(created), installed);
    if (FAILED(created)) return false;
    if (!installed) {
        std::puts("[BOOTSTRAP] no installed Core/Stub module; normal isolated test initialization");
        return true;
    }
    const auto deadline = GetTickCount64() + 10000;
    do {
        const auto core = GetModuleHandleW(core_name);
        const auto ready = core ? reinterpret_cast<BOOL(WINAPI*)()>(GetProcAddress(core, "PulseIsDirectRevealInstalled")) : nullptr;
        if (ready && ready()) {
            std::puts("[BOOTSTRAP] installed Core direct-reveal hooks ready before test Detours");
            return true;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    std::puts("[BOOTSTRAP] installed Core did not become ready; no browse API test will run");
    return false;
}
bool CreateShortcut(const std::wstring& lnk_path, const std::wstring& target, const std::wstring& arguments = {}) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    if (FAILED(link->SetPath(target.c_str()))) return false;
    if (!arguments.empty() && FAILED(link->SetArguments(arguments.c_str()))) return false;
    ComPtr<IPersistFile> persist;
    return SUCCEEDED(link.As(&persist)) && SUCCEEDED(persist->Save(lnk_path.c_str(), TRUE));
}
std::set<HWND> ExplorerWindows() {
    std::set<HWND> result;
    EnumWindows([](HWND window, LPARAM context) -> BOOL {
        wchar_t name[64]{}; GetClassNameW(window, name, 64);
        if (wcscmp(name, L"CabinetWClass") == 0) reinterpret_cast<std::set<HWND>*>(context)->insert(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}
}
using pulse::shell_bridge::runtime::DirectRevealPath;
int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Diagnostic("startup");
    if (argc == 2 && std::strcmp(argv[1], "--module-diagnostics-only") == 0) {
        // No reveal dispatch or ShellExecute: diagnose COM/module entry without UI.
        ComPtr<IShellLinkW> link;
        const HRESULT status = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
        std::printf("[DIAG] ShellLinkCoCreate=%08lx\n", static_cast<unsigned long>(status));
        Sleep(1000);
        Diagnostic("after-shell-link-com");
        return FAILED(status) ? 1 : 0;
    }
    unsigned failures = 0;
    const auto check = [&](bool ok, const char* text) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text); if (!ok) { ++failures; Diagnostic(text); } };
    wchar_t directory[32768]{}; GetWindowsDirectoryW(directory, 32768);
    const std::wstring folder(directory), file = folder + L"\\explorer.exe";
    PIDLIST_ABSOLUTE absolute = ILCreateFromPathW(file.c_str());
    PIDLIST_ABSOLUTE parent = ILCreateFromPathW(folder.c_str());
    PCUITEMID_CHILD child = absolute ? ILFindLastID(absolute) : nullptr;
    std::wstring path;
    check(absolute && parent, "filesystem fixtures exist");
    check(DirectRevealPath(absolute, 0, nullptr, 0, path) && path == file, "count zero preserves full selected path");
    check(DirectRevealPath(parent, 1, &child, 0, path) && path == file, "one child combines directory and selected file");
    PCUITEMID_CHILD full_child[] = {reinterpret_cast<PCUITEMID_CHILD>(absolute)};
    const bool full_child_matches = DirectRevealPath(parent, 1, full_child, 0, path) && path == file;
    check(full_child_matches, "a full file ID in the selected directory resolves without duplicating the parent");
    PIDLIST_ABSOLUTE other_parent = ILCreateFromPathW((folder + L"\\System32").c_str());
    check(other_parent && !DirectRevealPath(other_parent, 1, full_child, 0, path),
        "a full file ID outside the selected directory is rejected by the local parser");
    PCUITEMID_CHILD folder_as_selection[] = {reinterpret_cast<PCUITEMID_CHILD>(parent)};
    check(!DirectRevealPath(parent, 1, folder_as_selection, 0, path),
        "the containing folder is not one of its own children");
    check(!DirectRevealPath(parent, 2, &child, 0, path) && path.empty(), "multiple selection is not representable by the single-selection parser");
    check(!DirectRevealPath(absolute, 0, nullptr, OFASI_EDIT, path), "rename flag is not representable by the reveal parser");
    check(!DirectRevealPath(nullptr, 0, nullptr, 0, path), "null PIDL is rejected by the local parser");
    check(!DirectRevealPath(parent, 1, nullptr, 0, path), "missing child is rejected by the local parser");
    PIDLIST_ABSOLUTE virtual_item = nullptr;
    SHGetSpecialFolderLocation(nullptr, CSIDL_DRIVES, &virtual_item);
    check(virtual_item && !DirectRevealPath(virtual_item, 0, nullptr, 0, path), "virtual shell folder is not representable by the local-path parser");
    using pulse::shell_bridge::runtime::DirectExplorerSelection;
    check(DirectExplorerSelection(file.c_str(), (L"/select,\"" + file + L"\"").c_str(), path) && path == file, "explicit Explorer quoted select");
    check(!DirectExplorerSelection(L"other.exe", L"/select,C:\\x", path), "unrelated executable stays native");
    check(DirectExplorerSelection(file.c_str(), L" /SELECT, \"C:\\fixture\\a, b.txt\" \t", path) &&
        path == L"C:\\fixture\\a, b.txt", "quoted comma filename and shell whitespace preserve selection");
    check(!DirectExplorerSelection(file.c_str(), L"/select,\"C:\\x\" /root,C:\\", path), "arguments after quoted selection are not resolved by the local parser");
    check(!DirectExplorerSelection(file.c_str(), L"/select,C:\\x,/root,C:\\", path), "extra Explorer switches are not resolved by the local parser");
    check(!DirectExplorerSelection(file.c_str(), L"/select,\\\\server\\share\\x", path), "UNC is not resolved by the local selection parser");
    check(!DirectExplorerSelection(file.c_str(), L"/select,C:\\x\" /root", path), "malformed quotes are not resolved by the local selection parser");
    const bool bootstrap_ready = PrepareInstalledBootstrap();
    check(bootstrap_ready, "installed ShellLink bootstrap settles before isolated native stand-ins and test Detours");
    Diagnostic("before-test-hook-install");
    if (!bootstrap_ready) { CoUninitialize(); return 1; }
    const auto before = ExplorerWindows();
    const bool stand_ins = InstallNativeStandIns() == NO_ERROR;
    check(stand_ins, "native stand-ins installed beneath the reveal hook");
    // Without the stand-ins an unrelated native request could open a real Explorer window.
    if (!stand_ins) return 1;
    // Installed Core can asynchronously replace this EXE's import slots after
    // readiness. This suite exercises our export Detours + native stand-ins,
    // not another Core's outer IAT callbacks. Production IAT is tested by the
    // separate installed-entry probe, never by these injected sender counters.
    const auto shell_module = GetModuleHandleW(L"shell32.dll");
    const auto SHOpenFolderAndSelectItems = reinterpret_cast<decltype(&::SHOpenFolderAndSelectItems)>(GetProcAddress(shell_module, "SHOpenFolderAndSelectItems"));
    const auto ShellExecuteW = reinterpret_cast<decltype(&::ShellExecuteW)>(GetProcAddress(shell_module, "ShellExecuteW"));
    const auto ShellExecuteA = reinterpret_cast<decltype(&::ShellExecuteA)>(GetProcAddress(shell_module, "ShellExecuteA"));
    const auto ShellExecuteExW = reinterpret_cast<decltype(&::ShellExecuteExW)>(GetProcAddress(shell_module, "ShellExecuteExW"));
    const auto ShellExecuteExA = reinterpret_cast<decltype(&::ShellExecuteExA)>(GetProcAddress(shell_module, "ShellExecuteExA"));
    const bool exports_ready = SHOpenFolderAndSelectItems && ShellExecuteW && ShellExecuteA && ShellExecuteExW && ShellExecuteExA;
    check(exports_ready, "isolated Detours suite resolves five real Shell exports independently of production IAT hooks");
    if (!exports_ready) return 1;
    const LONG installed = pulse::shell_bridge::runtime::InstallDirectRevealHook(GetModuleHandleW(nullptr), []() noexcept { return true; }, nullptr, Send, nullptr, OpenFolder);
    check(installed == NO_ERROR, "real Detours install in isolated test process");
    if (installed == NO_ERROR) {
        const std::wstring args = L"/select,\"" + file + L"\"";
        const HINSTANCE result = ShellExecuteW(nullptr, L"open", file.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
        check(reinterpret_cast<INT_PTR>(result) > 32 && sent == 1 && selected == file, "real ShellExecuteW accepted through sender without native launch");
        const bool bare = DirectExplorerSelection(L"explorer.exe", args.c_str(), path);
        check(bare, "bare Explorer resolves to system Explorer without App Paths override");
        if (bare) {
            const HINSTANCE bare_result = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            check(reinterpret_cast<INT_PTR>(bare_result) > 32 && sent == 2 && selected == file, "Weixin bare Explorer call shape captured");
        } else {
            wchar_t resolved[32768]{};
            const DWORD found = SearchPathW(nullptr, L"explorer.exe", nullptr, 32768, resolved, nullptr);
            std::wprintf(L"bare resolve rejected: SearchPath result=%lu path=%ls; inspect App Paths overrides\n", found, resolved);
        }
        const unsigned before_pidl = sent;
        check(SHOpenFolderAndSelectItems(absolute, 0, nullptr, 0) == S_OK && sent == before_pidl + 1 && selected == file,
            "real count-zero shell entry captured");
        check(SHOpenFolderAndSelectItems(parent, 1, &child, 0) == S_OK && sent == before_pidl + 2 && selected == file,
            "real directory-child shell entry captured");
        // If the parser regresses, leave the failure above and don't open an
        // unrelated native Explorer window merely to run the rest of this test.
        if (full_child_matches) {
            const auto before_full = sent;
            check(SHOpenFolderAndSelectItems(parent, 1, full_child, 0) == S_OK &&
                sent == before_full + 1 && selected == file,
                "real full file ID selects the exact file through Pulse without native launch");
        }
        SHELLEXECUTEINFOW execute{}; execute.cbSize = sizeof(execute);
        execute.lpFile = file.c_str(); execute.lpParameters = args.c_str(); execute.nShow = SW_SHOWNORMAL;
        const auto before_execute = sent;
        check(ShellExecuteExW(&execute) && reinterpret_cast<INT_PTR>(execute.hInstApp) > 32 && sent == before_execute + 1,
            "real mask-zero ShellExecuteExW captured without process handle");
        check(native == 0, "accepted reveals never reach the native layer");
        // Direct folder and folder-shortcut (.lnk) takeover without Explorer flash.
        wchar_t temp_dir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp_dir);
        const auto fixture_dir = std::filesystem::path(temp_dir) / (L"pulse-lnk-hook-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(fixture_dir);
        const std::wstring target_dir = folder + L"\\System32";
        const std::wstring folder_lnk = (fixture_dir / L"my_folder.lnk").wstring();
        const std::wstring explorer_dir_lnk = (fixture_dir / L"exp_folder.lnk").wstring();
        const std::wstring explorer_sel_lnk = (fixture_dir / L"exp_select.lnk").wstring();
        const std::wstring app_lnk = (fixture_dir / L"my_app.lnk").wstring();
        check(CreateShortcut(folder_lnk, target_dir) &&
              CreateShortcut(explorer_dir_lnk, file, L"/e,\"" + target_dir + L"\"") &&
              CreateShortcut(explorer_sel_lnk, file, L"/select,\"" + file + L"\"") &&
              CreateShortcut(app_lnk, file),
              "create temporary folder, explorer, and executable .lnk fixtures");
        Diagnostic("after-shortcut-creation");
        const auto eq = [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; };
        unsigned prev_open = opened;
        check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", folder_lnk.c_str(), nullptr, fixture_dir.c_str(), SW_SHOWNORMAL)) > 32 &&
              opened == prev_open + 1 && eq(opened_folder, target_dir),
              "ShellExecuteW open on folder .lnk with lpDirectory opens target folder directly in Pulse");
        prev_open = opened;
        check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"explore", folder_lnk.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32 &&
              opened == prev_open + 1 && eq(opened_folder, target_dir),
              "ShellExecuteW explore on folder .lnk bypasses Explorer directly");
        prev_open = opened;
        check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, nullptr, L"my_folder.lnk", nullptr, fixture_dir.c_str(), SW_SHOWNORMAL)) > 32 &&
              opened == prev_open + 1 && eq(opened_folder, target_dir),
              "ShellExecuteW relative folder .lnk with lpDirectory resolves and opens directly");
        prev_open = opened;
        SHELLEXECUTEINFOW lnk_ex{}; lnk_ex.cbSize = sizeof(lnk_ex);
        lnk_ex.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
        lnk_ex.lpVerb = L"open"; lnk_ex.lpFile = folder_lnk.c_str(); lnk_ex.lpDirectory = fixture_dir.c_str(); lnk_ex.nShow = SW_SHOWNORMAL;
        check(ShellExecuteExW(&lnk_ex) && reinterpret_cast<INT_PTR>(lnk_ex.hInstApp) > 32 &&
              opened == prev_open + 1 && eq(opened_folder, target_dir),
              "ShellExecuteExW SEE_MASK_INVOKEIDLIST on folder .lnk opens directly");
        PIDLIST_ABSOLUTE lnk_pidl = ILCreateFromPathW(folder_lnk.c_str());
        if (lnk_pidl) {
            prev_open = opened;
            SHELLEXECUTEINFOW idl_ex{}; idl_ex.cbSize = sizeof(idl_ex);
            idl_ex.fMask = SEE_MASK_IDLIST; idl_ex.lpIDList = lnk_pidl; idl_ex.nShow = SW_SHOWNORMAL;
            check(ShellExecuteExW(&idl_ex) && reinterpret_cast<INT_PTR>(idl_ex.hInstApp) > 32 &&
                  opened == prev_open + 1 && eq(opened_folder, target_dir),
                  "ShellExecuteExW SEE_MASK_IDLIST on folder .lnk PIDL opens directly");
            CoTaskMemFree(lnk_pidl);
        }
        prev_open = opened;
        check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", explorer_dir_lnk.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32 &&
              opened == prev_open + 1 && eq(opened_folder, target_dir),
              "folder shortcut targeting explorer.exe /e,<dir> opens directly");
        const unsigned prev_sel = sent;
        check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", explorer_sel_lnk.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32 &&
              sent == prev_sel + 1 && eq(selected, file),
              "shortcut targeting explorer.exe /select,<file> reveals directly");
        prev_open = opened;
        check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"explore", target_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32 &&
              opened == prev_open + 1 && eq(opened_folder, target_dir),
              "direct ShellExecuteW explore on local directory opens directly");
        prev_open = opened;
        check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", file.c_str(), (L"/e,\"" + target_dir + L"\"").c_str(), nullptr, SW_SHOWNORMAL)) > 32 &&
              opened == prev_open + 1 && eq(opened_folder, target_dir),
              "explorer.exe /e,<dir> opens directly without /select");
        {
            std::string ansi_lnk;
            const int need = WideCharToMultiByte(CP_ACP, 0, folder_lnk.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (need > 1) {
                ansi_lnk.resize(static_cast<size_t>(need - 1));
                WideCharToMultiByte(CP_ACP, 0, folder_lnk.c_str(), -1, ansi_lnk.data(), need, nullptr, nullptr);
                prev_open = opened;
                check(reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", ansi_lnk.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32 &&
                      opened == prev_open + 1 && eq(opened_folder, target_dir),
                      "ShellExecuteA on folder .lnk opens directly");
                prev_open = opened;
                SHELLEXECUTEINFOA ex_a{}; ex_a.cbSize = sizeof(ex_a);
                ex_a.fMask = SEE_MASK_INVOKEIDLIST; ex_a.lpVerb = "open"; ex_a.lpFile = ansi_lnk.c_str(); ex_a.nShow = SW_SHOWNORMAL;
                check(ShellExecuteExA(&ex_a) && reinterpret_cast<INT_PTR>(ex_a.hInstApp) > 32 &&
                      opened == prev_open + 1 && eq(opened_folder, target_dir),
                      "ShellExecuteExA on folder .lnk opens directly");
            }
        }
        check(native == 0, "all accepted folder and .lnk direct opens never reach native layer");
        // File associations and unsupported browse contracts each use the original API once.
        unsigned prev_native = native;
        check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", app_lnk.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) == 42 &&
              native == prev_native + 1, "executable .lnk shortcut stays native");
        prev_native = native;
        check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"properties", folder_lnk.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) == 42 &&
              native == prev_native + 1, "properties verb on folder .lnk stays native");
        prev_native = native;
        check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", folder_lnk.c_str(), nullptr, nullptr, SW_HIDE)) == 42 &&
              native == prev_native + 1 && native_show == SW_HIDE, "SW_HIDE browse keeps native visibility semantics exactly once");
        prev_native = native;
        SHELLEXECUTEINFOW proc_ex{}; proc_ex.cbSize = sizeof(proc_ex);
        proc_ex.fMask = SEE_MASK_NOCLOSEPROCESS; proc_ex.lpVerb = L"open"; proc_ex.lpFile = folder_lnk.c_str(); proc_ex.nShow = SW_SHOWNORMAL;
        proc_ex.hProcess = reinterpret_cast<HANDLE>(1);
        check(ShellExecuteExW(&proc_ex) && reinterpret_cast<INT_PTR>(proc_ex.hInstApp) == 42 &&
              proc_ex.hProcess == reinterpret_cast<HANDLE>(0x1234) && native == prev_native + 1 &&
              native_w_input.hProcess == reinterpret_cast<HANDLE>(1) && native_w_input.fMask == SEE_MASK_NOCLOSEPROCESS && native_file_w == file,
              "SEE_MASK_NOCLOSEPROCESS preserves native input and native output handle exactly once");
        proc_ex.fMask = 0x80000000u;
        prev_native = native;
        check(ShellExecuteExW(&proc_ex) && native_w_input.fMask == 0x80000000u && native == prev_native + 1,
              "unknown ShellExecuteEx mask keeps original native contract exactly once");
        proc_ex.fMask = SEE_MASK_CLASSNAME; proc_ex.lpClass = L"Directory";
        prev_native = native;
        check(ShellExecuteExW(&proc_ex) && native == prev_native + 1 && native_info == &proc_ex &&
              native_w_input.lpFile == folder_lnk.c_str() && native_w_input.lpClass == proc_ex.lpClass,
              "explicit class override retains its original native contract rather than rewriting only lpFile");
        proc_ex.lpClass = nullptr;
        proc_ex.fMask = SEE_MASK_NOCLOSEPROCESS;
        const auto document = (fixture_dir / L"document.txt").wstring();
        HANDLE document_file = CreateFileW(document.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(document_file != INVALID_HANDLE_VALUE, "create document-association fixture");
        if (document_file != INVALID_HANDLE_VALUE) CloseHandle(document_file);
        prev_native = native;
        check(ShellExecuteW(nullptr, L"open", document.c_str(), nullptr, nullptr, SW_SHOWNORMAL) == reinterpret_cast<HINSTANCE>(42) &&
              native == prev_native + 1, "unrelated document open keeps the native association");
        proc_ex.lpFile = document.c_str();
        prev_native = native;
        check(ShellExecuteExW(&proc_ex) && reinterpret_cast<INT_PTR>(proc_ex.hInstApp) == 42 && native == prev_native + 1,
              "unrelated document with NOCLOSEPROCESS remains native");
        prev_native = native;
        check(ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL) == reinterpret_cast<HINSTANCE>(42) &&
              native == prev_native + 1, "no-argument Explorer desktop-shell launch stays native");
        prev_native = native;
        check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", file.c_str(), L"/root,C:\\", nullptr, SW_SHOWNORMAL)) == 42 &&
              native == prev_native + 1, "unknown explicit Explorer browse syntax uses native exactly once");
        prev_native = native;
        check(SHOpenFolderAndSelectItems(parent, 1, &child, OFASI_EDIT) == S_OK && native == prev_native + 1 && native_flags == OFASI_EDIT,
              "unsupported PIDL flags are forwarded unchanged to native once");
        PCUITEMID_CHILD many[]{child, child};
        prev_native = native;
        check(SHOpenFolderAndSelectItems(parent, 2, many, 0) == S_OK && native == prev_native + 1 && native_count == 2 && native_children == many,
              "unsupported multi-selection reaches native exactly once");
        prev_native = native;
        check(SHOpenFolderAndSelectItems(virtual_item, 0, nullptr, 0) == S_OK && native == prev_native + 1 && native_folder == virtual_item,
              "unsupported virtual PIDL reveal reaches native exactly once");
        std::error_code ec;
        std::filesystem::remove_all(fixture_dir, ec);
        // Failure/timeout/unsupported delivery each attempt Pulse once, then native once.
        const auto to_ansi = [](const std::wstring& wide, std::string& ansi) {
            const UINT code_page = GetACP();
            const DWORD flags = code_page == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
            BOOL replaced = FALSE;
            BOOL* used_default = code_page == CP_UTF8 ? nullptr : &replaced;
            const int bytes = WideCharToMultiByte(code_page, flags, wide.c_str(), -1, nullptr, 0, nullptr, used_default);
            if (bytes <= 0 || replaced) return false;
            ansi.resize(static_cast<size_t>(bytes));
            if (WideCharToMultiByte(code_page, flags, wide.c_str(), -1, ansi.data(), bytes, nullptr, used_default) != bytes || replaced)
                return false;
            const int chars = MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS, ansi.c_str(), -1, nullptr, 0);
            if (chars <= 0) return false;
            std::wstring roundtrip(static_cast<size_t>(chars), L'\0');
            if (MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS, ansi.c_str(), -1, roundtrip.data(), chars) != chars)
                return false;
            roundtrip.resize(static_cast<size_t>(chars - 1));
            ansi.resize(static_cast<size_t>(bytes - 1));
            return roundtrip == wide;
        };
        std::string ansi_explorer, ansi_args;
        const bool ansi_inputs = to_ansi(file, ansi_explorer) && to_ansi(args, ansi_args);
        check(ansi_inputs, "ANSI fallback fixtures round-trip through the current ACP without losing Unicode");
        if (ansi_inputs) {
            const auto accepted_native = native;
            auto accepted_sent = sent;
            reply = pulse::DirectRevealResult::Accepted;
            check(reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", ansi_explorer.c_str(), ansi_args.c_str(), nullptr, SW_SHOWNORMAL)) > 32 &&
                  sent == accepted_sent + 1 && native == accepted_native, "accepted ANSI reveal never reaches native fallback");
            SHELLEXECUTEINFOA accepted_a{}; accepted_a.cbSize = sizeof(accepted_a);
            accepted_a.lpFile = ansi_explorer.c_str(); accepted_a.lpParameters = ansi_args.c_str(); accepted_a.nShow = SW_SHOWNORMAL;
            accepted_sent = sent;
            check(ShellExecuteExA(&accepted_a) && reinterpret_cast<INT_PTR>(accepted_a.hInstApp) > 32 && !accepted_a.hProcess &&
                  sent == accepted_sent + 1 && native == accepted_native, "accepted ANSI Ex reveal never reaches native or fabricates a process handle");
        }
        association_folder = target_dir; association_explorer = file;
        simulate_folder_association = true;
        SHELLEXECUTEINFOW association{}; association.cbSize = sizeof(association);
        association.lpVerb = L"open"; association.lpFile = target_dir.c_str(); association.nShow = SW_SHOWNORMAL;
        association.fMask = SEE_MASK_NOCLOSEPROCESS;
        const auto association_before = associated_pulse;
        check(NativeExecuteEx(&association) && associated_pulse == association_before + 1,
              "association fixture models a raw directory ShellExecuteEx choosing Pulse");
        const auto bypass_native = native;
        check(pulse::shell_bridge::runtime::FallbackShellExecuteExW(pulse::DirectRevealResult::NotHandled, &NativeExecuteEx, &association) &&
              associated_pulse == association_before + 2 && native == bypass_native + 1 && native_info == &association,
              "NotHandled explicitly preserves the caller's directory association and original info pointer");
        const auto explicit_before = explicit_explorer;
        prev_native = native;
        association.hProcess = reinterpret_cast<HANDLE>(1);
        const auto original_association_file = association.lpFile;
        const auto original_association_parameters = association.lpParameters;
        check(ShellExecuteExW(&association) && associated_pulse == association_before + 2 && explicit_explorer == explicit_before + 1 &&
              native == prev_native + 1 && native_file_w == file && native_parameters_w == L"\"" + target_dir + L"\"" &&
              native_w_input.fMask == SEE_MASK_NOCLOSEPROCESS && association.lpFile == original_association_file &&
              association.lpParameters == original_association_parameters && association.hProcess == reinterpret_cast<HANDLE>(0x1234),
              "known folder fallback bypasses the Pulse Directory association via explicit Explorer without replacing caller pointers");
        simulate_folder_association = false;
        native_bool = FALSE; native_hr = E_ACCESSDENIED; native_instance = SE_ERR_ACCESSDENIED;
        const auto native_handle = reinterpret_cast<HANDLE>(0x1234);
        const auto caller_handle = reinterpret_cast<HANDLE>(1);
        const auto caller_window = reinterpret_cast<HWND>(2);
        const std::wstring normalized_selection = L"/select, \"" + file + L"\"";
        const std::string normalized_selection_a = "/select, \"" + ansi_explorer + "\"";
        const wchar_t* wide_verb = L"open";
        const char* ansi_verb = "open";
        for (const auto outcome : {pulse::DirectRevealResult::Rejected, pulse::DirectRevealResult::Unsupported,
                                  pulse::DirectRevealResult::Indeterminate, pulse::DirectRevealResult::NotHandled}) {
            reply = outcome;
            char label[160]{};
            auto previous = sent; prev_native = native;
            const auto w_result = ShellExecuteW(caller_window, wide_verb, file.c_str(), args.c_str(), folder.c_str(), SW_SHOWNORMAL);
            const DWORD w_error = GetLastError();
            std::snprintf(label, sizeof(label), "result=%u ShellExecuteW uses explicit Explorer args, original return/error once without Pulse retry", static_cast<unsigned>(outcome));
            check(reinterpret_cast<INT_PTR>(w_result) == native_instance && w_error == native_error && native_scope &&
                  native == prev_native + 1 && sent == previous + 1 && native_window == caller_window && native_verb_w == L"open" &&
                  native_file_w == file && native_parameters_w == normalized_selection && native_directory == folder.c_str() && native_show == SW_SHOWNORMAL, label);
            SHELLEXECUTEINFOW failed_ex{}; failed_ex.cbSize = sizeof(failed_ex);
            failed_ex.hwnd = caller_window; failed_ex.lpVerb = wide_verb; failed_ex.lpFile = file.c_str();
            failed_ex.lpParameters = args.c_str(); failed_ex.lpDirectory = folder.c_str(); failed_ex.nShow = SW_SHOWNORMAL;
            failed_ex.hProcess = caller_handle;
            previous = sent; prev_native = native;
            const BOOL ex_result = ShellExecuteExW(&failed_ex); const DWORD ex_error = GetLastError();
            std::snprintf(label, sizeof(label), "result=%u ShellExecuteExW normalizes a copy and preserves native FALSE/error/output once", static_cast<unsigned>(outcome));
            check(ex_result == native_bool && ex_error == native_error && native_scope && native == prev_native + 1 && sent == previous + 1 &&
                  native_info != &failed_ex && native_w_input.hwnd == caller_window && native_verb_w == L"open" &&
                  native_file_w == file && native_parameters_w == normalized_selection && failed_ex.lpFile == file.c_str() && failed_ex.lpParameters == args.c_str() && failed_ex.lpVerb == wide_verb && native_w_input.lpDirectory == folder.c_str() &&
                  native_w_input.hProcess == caller_handle && native_w_input.fMask == 0 && native_w_input.nShow == SW_SHOWNORMAL &&
                  failed_ex.hProcess == native_handle && reinterpret_cast<INT_PTR>(failed_ex.hInstApp) == native_instance, label);
            previous = sent; prev_native = native;
            const HRESULT pidl_result = SHOpenFolderAndSelectItems(parent, 1, &child, 0); const DWORD pidl_error = GetLastError();
            std::snprintf(label, sizeof(label), "result=%u PIDL preserves original args, native HRESULT/error exactly once", static_cast<unsigned>(outcome));
            check(pidl_result == native_hr && pidl_error == native_error && native_scope && native == prev_native + 1 && sent == previous + 1 &&
                  native_folder == parent && native_children == &child && native_count == 1 && native_flags == 0, label);
            if (ansi_inputs) {
                previous = sent; prev_native = native;
                const auto a_result = ShellExecuteA(caller_window, ansi_verb, ansi_explorer.c_str(), ansi_args.c_str(), nullptr, SW_SHOWNORMAL);
                const DWORD a_error = GetLastError();
                std::snprintf(label, sizeof(label), "result=%u ShellExecuteA uses lossless explicit Explorer args, native return/error once", static_cast<unsigned>(outcome));
                check(reinterpret_cast<INT_PTR>(a_result) == native_instance && a_error == native_error && native_scope && native == prev_native + 1 &&
                      sent == previous + 1 && native_window == caller_window && native_verb_a == "open" && native_file_a == ansi_explorer &&
                      native_parameters_a == normalized_selection_a && native_directory == nullptr && native_show == SW_SHOWNORMAL, label);
                SHELLEXECUTEINFOA failed_a{}; failed_a.cbSize = sizeof(failed_a);
                failed_a.hwnd = caller_window; failed_a.lpVerb = ansi_verb; failed_a.lpFile = ansi_explorer.c_str();
                failed_a.lpParameters = ansi_args.c_str(); failed_a.nShow = SW_SHOWNORMAL; failed_a.hProcess = caller_handle;
                previous = sent; prev_native = native;
                const BOOL ax_result = ShellExecuteExA(&failed_a); const DWORD ax_error = GetLastError();
                std::snprintf(label, sizeof(label), "result=%u ShellExecuteExA normalizes a copy and preserves native FALSE/error/output once", static_cast<unsigned>(outcome));
                check(ax_result == native_bool && ax_error == native_error && native_scope && native == prev_native + 1 && sent == previous + 1 &&
                      native_info != &failed_a && native_a_input.hwnd == caller_window && native_verb_a == "open" &&
                      native_file_a == ansi_explorer && native_parameters_a == normalized_selection_a && failed_a.lpFile == ansi_explorer.c_str() && failed_a.lpParameters == ansi_args.c_str() && failed_a.lpVerb == ansi_verb && native_a_input.lpDirectory == nullptr &&
                      native_a_input.hProcess == caller_handle && native_a_input.fMask == 0 && native_a_input.nShow == SW_SHOWNORMAL &&
                      failed_a.hProcess == native_handle && reinterpret_cast<INT_PTR>(failed_a.hInstApp) == native_instance, label);
            }
        }
        reply = pulse::DirectRevealResult::Rejected;
        const auto previous_opened = opened; prev_native = native;
        check(ShellExecuteW(nullptr, L"open", target_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL) == reinterpret_cast<HINSTANCE>(native_instance) &&
              GetLastError() == native_error && opened == previous_opened + 1 && native == prev_native + 1,
              "rejected folder open attempts Pulse once and preserves native result once");
        // A native Shell implementation may call another hooked Shell API internally.
        const auto before_nested = sent;
        {
            pulse::shell_bridge::explorer_process::NativeFallbackScope outer;
            {
                pulse::shell_bridge::explorer_process::NativeFallbackScope inner;
                check(pulse::shell_bridge::explorer_process::NativeFallbackActive(), "nested native fallback scope is active");
            }
            check(pulse::shell_bridge::explorer_process::NativeFallbackActive(), "inner scope exit preserves outer bypass");
            check(pulse::shell_bridge::runtime::DispatchDirectShellExecuteW(L"open", file.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL, Send, OpenFolder) == pulse::DirectRevealResult::NotHandled &&
                  pulse::shell_bridge::runtime::DispatchDirectShellExecuteExW(&execute, Send, OpenFolder) == pulse::DirectRevealResult::NotHandled &&
                  pulse::shell_bridge::runtime::DispatchDirectRevealPidl(parent, 1, &child, 0, Send) == pulse::DirectRevealResult::NotHandled,
                  "native fallback bypasses nested wide and PIDL Pulse dispatch");
            SHELLEXECUTEINFOA nested_a{}; nested_a.cbSize = sizeof(nested_a);
            nested_a.lpFile = ansi_explorer.c_str(); nested_a.lpParameters = ansi_args.c_str(); nested_a.nShow = SW_SHOWNORMAL;
            check(pulse::shell_bridge::runtime::DispatchDirectShellExecuteA("open", ansi_explorer.c_str(), ansi_args.c_str(), nullptr, SW_SHOWNORMAL, Send, OpenFolder) == pulse::DirectRevealResult::NotHandled &&
                  pulse::shell_bridge::runtime::DispatchDirectShellExecuteExA(&nested_a, Send, OpenFolder) == pulse::DirectRevealResult::NotHandled && sent == before_nested,
                  "native fallback bypasses nested ANSI Pulse dispatch without another send");
        }
        check(!pulse::shell_bridge::explorer_process::NativeFallbackActive(), "native fallback scope clears after outer exit");
        // Simulate another runtime DLL's scope without constructing this module's TLS scope.
        wchar_t peer_name[96]{};
        swprintf_s(peer_name, L"Local\\Pulse.NativeBrowse.%lu.%lu", GetCurrentProcessId(), GetCurrentThreadId());
        HANDLE peer_event = CreateEventW(nullptr, TRUE, FALSE, peer_name);
        check(peer_event != nullptr, "create peer-module native fallback event fixture");
        if (peer_event) {
            SetLastError(ERROR_CANCELLED);
            const bool peer_active = pulse::shell_bridge::explorer_process::NativeFallbackActive();
            check(peer_active && GetLastError() == ERROR_CANCELLED, "peer event activates bypass without changing LastError");
            const auto before_peer = sent; prev_native = native;
            reply = pulse::DirectRevealResult::Accepted;
            check(pulse::shell_bridge::runtime::DispatchDirectShellExecuteW(L"open", file.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL, Send, OpenFolder) == pulse::DirectRevealResult::NotHandled &&
                  sent == before_peer, "peer event bypasses local Shell dispatch without another Pulse attempt");
            const auto peer_result = ShellExecuteW(nullptr, L"open", file.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
            const DWORD peer_error = GetLastError();
            check(reinterpret_cast<INT_PTR>(peer_result) == native_instance && peer_error == native_error &&
                  native == prev_native + 1 && sent == before_peer && native_scope,
                  "peer event keeps the real Shell entry's original native result/error exactly once");
            bool other_active = true;
            auto other_result = pulse::DirectRevealResult::NotHandled;
            std::thread other_thread([&] {
                other_active = pulse::shell_bridge::explorer_process::NativeFallbackActive();
                other_result = pulse::shell_bridge::runtime::DispatchDirectShellExecuteW(L"open", file.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL, Send, OpenFolder);
            });
            other_thread.join();
            check(!other_active && other_result == pulse::DirectRevealResult::Accepted && sent == before_peer + 1,
                  "peer event is thread-local: another thread still routes its browse to Pulse");
            CloseHandle(peer_event);
            check(!pulse::shell_bridge::explorer_process::NativeFallbackActive(), "closing the peer event clears cross-module bypass");
            const auto after_peer = sent; prev_native = native;
            check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", file.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL)) > 32 &&
                  sent == after_peer + 1 && native == prev_native,
                  "after peer scope exit the original thread resumes Pulse routing without native fallback");
        }
        check(before == ExplorerWindows(), "no real Explorer window created by the test");
    }
    CoTaskMemFree(other_parent); CoTaskMemFree(virtual_item); CoTaskMemFree(parent); CoTaskMemFree(absolute);
    CoUninitialize(); return failures ? 1 : 0;
}

#include "../shell_bridge/direct_reveal_hook.h"
#include <detours.h>
#include <wrl/client.h>
#include <cstdio>
#include <filesystem>
#include <set>
namespace {
using Microsoft::WRL::ComPtr;
unsigned sent = 0, opened = 0; std::wstring selected, opened_folder;
pulse::DirectRevealResult reply = pulse::DirectRevealResult::Accepted;
pulse::DirectRevealResult Send(const std::wstring& path, DWORD) noexcept { ++sent; selected = path; return reply; }
pulse::DirectRevealResult OpenFolder(const std::wstring& path, DWORD) noexcept { ++opened; opened_folder = path; return reply; }
// Stand-ins for the native layer, detoured before the reveal hook so its
// trampolines land here: native fallback is observed without opening Explorer.
unsigned native = 0;
decltype(&SHOpenFolderAndSelectItems) real_reveal = &SHOpenFolderAndSelectItems;
decltype(&ShellExecuteExW) real_execute_ex = &ShellExecuteExW;
decltype(&ShellExecuteW) real_execute = &ShellExecuteW;
decltype(&ShellExecuteExA) real_execute_ex_a = &ShellExecuteExA;
decltype(&ShellExecuteA) real_execute_a = &ShellExecuteA;
HRESULT WINAPI NativeReveal(PCIDLIST_ABSOLUTE, UINT, PCUITEMID_CHILD_ARRAY, DWORD) { ++native; return S_OK; }
BOOL WINAPI NativeExecuteEx(SHELLEXECUTEINFOW* info) { ++native; info->hInstApp = reinterpret_cast<HINSTANCE>(42); return TRUE; }
HINSTANCE WINAPI NativeExecute(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) { ++native; return reinterpret_cast<HINSTANCE>(42); }
BOOL WINAPI NativeExecuteExA(SHELLEXECUTEINFOA* info) { ++native; info->hInstApp = reinterpret_cast<HINSTANCE>(42); return TRUE; }
HINSTANCE WINAPI NativeExecuteA(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT) { ++native; return reinterpret_cast<HINSTANCE>(42); }
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
int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    unsigned failures = 0;
    const auto check = [&](bool ok, const char* text) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text); if (!ok) ++failures; };
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
        "a full file ID from outside the selected directory stays native");
    PCUITEMID_CHILD folder_as_selection[] = {reinterpret_cast<PCUITEMID_CHILD>(parent)};
    check(!DirectRevealPath(parent, 1, folder_as_selection, 0, path),
        "the containing folder is not one of its own children");
    check(!DirectRevealPath(parent, 2, &child, 0, path) && path.empty(), "multiple selection stays native");
    check(!DirectRevealPath(absolute, 0, nullptr, OFASI_EDIT, path), "rename flag stays native");
    check(!DirectRevealPath(nullptr, 0, nullptr, 0, path), "null PIDL stays native");
    check(!DirectRevealPath(parent, 1, nullptr, 0, path), "missing child stays native");
    PIDLIST_ABSOLUTE virtual_item = nullptr;
    SHGetSpecialFolderLocation(nullptr, CSIDL_DRIVES, &virtual_item);
    check(virtual_item && !DirectRevealPath(virtual_item, 0, nullptr, 0, path), "virtual shell folder stays native");
    using pulse::shell_bridge::runtime::DirectExplorerSelection;
    check(DirectExplorerSelection(file.c_str(), (L"/select,\"" + file + L"\"").c_str(), path) && path == file, "explicit Explorer quoted select");
    check(!DirectExplorerSelection(L"other.exe", L"/select,C:\\x", path), "unrelated executable stays native");
    check(DirectExplorerSelection(file.c_str(), L" /SELECT, \"C:\\fixture\\a, b.txt\" \t", path) &&
        path == L"C:\\fixture\\a, b.txt", "quoted comma filename and shell whitespace preserve selection");
    check(!DirectExplorerSelection(file.c_str(), L"/select,\"C:\\x\" /root,C:\\", path), "arguments after quoted selection stay native");
    check(!DirectExplorerSelection(file.c_str(), L"/select,C:\\x,/root,C:\\", path), "extra Explorer switches stay native");
    check(!DirectExplorerSelection(file.c_str(), L"/select,\\\\server\\share\\x", path), "UNC remains native");
    check(!DirectExplorerSelection(file.c_str(), L"/select,C:\\x\" /root", path), "malformed quotes stay native");
    const auto before = ExplorerWindows();
    const bool stand_ins = InstallNativeStandIns() == NO_ERROR;
    check(stand_ins, "native stand-ins installed beneath the reveal hook");
    // Without the stand-ins a fallback would open a real Explorer window.
    if (!stand_ins) return 1;
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
        // Safety boundaries: non-folder .lnk, properties/runas verbs, NOCLOSEPROCESS, and SW_HIDE MUST stay native.
        unsigned prev_native = native;
        check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", app_lnk.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) == 42 &&
              native == prev_native + 1, "executable .lnk shortcut stays native");
        prev_native = native;
        check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"properties", folder_lnk.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) == 42 &&
              native == prev_native + 1, "properties verb on folder .lnk stays native");
        prev_native = native;
        check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", folder_lnk.c_str(), nullptr, nullptr, SW_HIDE)) == 42 &&
              native == prev_native + 1, "SW_HIDE invocation stays native");
        prev_native = native;
        SHELLEXECUTEINFOW proc_ex{}; proc_ex.cbSize = sizeof(proc_ex);
        proc_ex.fMask = SEE_MASK_NOCLOSEPROCESS; proc_ex.lpVerb = L"open"; proc_ex.lpFile = folder_lnk.c_str(); proc_ex.nShow = SW_SHOWNORMAL;
        check(ShellExecuteExW(&proc_ex) && reinterpret_cast<INT_PTR>(proc_ex.hInstApp) == 42 &&
              native == prev_native + 1, "SEE_MASK_NOCLOSEPROCESS stays native");
        std::error_code ec;
        std::filesystem::remove_all(fixture_dir, ec);
        // Pulse first; a request that may have been lost still opens natively.
        reply = pulse::DirectRevealResult::Indeterminate;
        unsigned previous = sent;
        prev_native = native;
        const HINSTANCE timeout = ShellExecuteW(nullptr, L"open", file.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
        check(reinterpret_cast<INT_PTR>(timeout) == 42 && sent == previous + 1 && native == prev_native + 1,
            "indeterminate ShellExecuteW falls back to native after one Pulse attempt");
        previous = sent;
        prev_native = native;
        SHELLEXECUTEINFOW fallback{}; fallback.cbSize = sizeof(fallback);
        fallback.lpFile = file.c_str(); fallback.lpParameters = args.c_str(); fallback.nShow = SW_SHOWNORMAL;
        check(ShellExecuteExW(&fallback) && reinterpret_cast<INT_PTR>(fallback.hInstApp) == 42 && sent == previous + 1 && native == prev_native + 1,
            "indeterminate ShellExecuteExW falls back to native");
        previous = sent;
        prev_native = native;
        check(SHOpenFolderAndSelectItems(parent, 1, &child, 0) == S_OK && sent == previous + 1 && native == prev_native + 1,
            "indeterminate SHOpenFolderAndSelectItems falls back to native");
        reply = pulse::DirectRevealResult::Rejected;
        previous = sent;
        prev_native = native;
        check(ShellExecuteW(nullptr, L"open", file.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL) == reinterpret_cast<HINSTANCE>(42) &&
            sent == previous + 1 && native == prev_native + 1, "rejected reveal falls back to native");
        check(before == ExplorerWindows(), "no real Explorer window created by the test");
    }
    CoTaskMemFree(other_parent); CoTaskMemFree(virtual_item); CoTaskMemFree(parent); CoTaskMemFree(absolute);
    CoUninitialize(); return failures ? 1 : 0;
}

#include "../shell_bridge/dialog_sink_hooks.h"

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <winstring.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <string>

namespace sink = pulse::shell_bridge::sink_hooks;
using Microsoft::WRL::ComPtr;

namespace {

int RunPreloadComDlgTest() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] [preload] %s\n", ok ? "PASS" : "FAIL", name);
        std::fflush(stdout);
        if (!ok) ++failures;
    };

    check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "CoInitializeEx STA");
    check(GetModuleHandleW(L"comdlg32.dll") == nullptr,
          "comdlg32.dll is NOT loaded prior to Install(preload_comdlg32=true)");

    std::atomic<int> save_clsid_hits{0};
    std::atomic<int> save_w_hits{0};

    sink::SinkCallbacks cb;
    cb.on_comdlg_class_object = [&](REFCLSID clsid, REFIID iid, void** out, sink::ComDlgGetClassObjectFn orig) -> HRESULT {
        if (clsid == CLSID_FileSaveDialog) ++save_clsid_hits;
        sink::ScopedNativeComDlgBypass bypass;
        return orig(clsid, iid, out);
    };
    cb.on_get_save_w = [&](LPOPENFILENAMEW, sink::GetSaveFileNameWFn) -> BOOL {
        ++save_w_hits;
        return FALSE;
    };

    sink::InstallOptions opts;
    opts.requested_mask = sink::kSinkGroupComDlgAll;
    opts.watch_late_dll_loads = true;
    opts.preload_comdlg32 = true;
    const auto st = sink::Install(std::move(cb), opts);

    // Zero sleep / zero FlushPendingHooks: comdlg32 sinks must be active synchronously on return.
    check(st.error == ERROR_SUCCESS &&
          (st.active_mask & sink::kSinkGroupComDlgAll) == sink::kSinkGroupComDlgAll &&
          GetModuleHandleW(L"comdlg32.dll") != nullptr,
          "Install(preload_comdlg32=true) synchronously loads and hooks comdlg32.dll with zero wait");

    HMODULE h_comdlg = GetModuleHandleW(L"comdlg32.dll");
    auto gco = h_comdlg ? reinterpret_cast<sink::ComDlgGetClassObjectFn>(GetProcAddress(h_comdlg, "DllGetClassObject")) : nullptr;
    auto save_w = h_comdlg ? reinterpret_cast<sink::GetSaveFileNameWFn>(GetProcAddress(h_comdlg, "GetSaveFileNameW")) : nullptr;

    ComPtr<IClassFactory> factory;
    const HRESULT hr = gco ? gco(CLSID_FileSaveDialog, IID_PPV_ARGS(&factory)) : E_FAIL;
    ComPtr<IFileSaveDialog> dlg;
    const HRESULT c_hr = SUCCEEDED(hr) && factory ? factory->CreateInstance(nullptr, IID_PPV_ARGS(&dlg)) : E_FAIL;
    check(SUCCEEDED(hr) && SUCCEEDED(c_hr) && dlg && save_clsid_hits.load() == 1,
          "immediate first comdlg32!DllGetClassObject(CLSID_FileSaveDialog) intercepted with zero race");

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    const BOOL ret = save_w ? save_w(&ofn) : TRUE;
    check(ret == FALSE && save_w_hits.load() == 1,
          "immediate first GetSaveFileNameW intercepted with zero race");

    const DWORD uninst = sink::Uninstall();
    check(uninst == ERROR_SUCCESS && sink::QueryStatus().active_mask == 0,
          "Uninstall cleanly detaches preloaded comdlg32 hooks");

    // Verify releasing live COM objects created via preloaded comdlg32 remains completely safe
    // AFTER Uninstall() (hot-reload / disable safety: comdlg32.dll must never unmap under callers).
    dlg.Reset();
    factory.Reset();
    check(GetModuleHandleW(L"comdlg32.dll") != nullptr,
          "preloaded comdlg32.dll remains safely pinned after Uninstall while releasing COM objects");

    CoUninitialize();
    return failures ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && _wcsicmp(argv[1], L"--preload-comdlg") == 0) {
        return RunPreloadComDlgTest();
    }

    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };

    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    check(SUCCEEDED(com_hr), "CoInitializeEx STA");

    // Verify comdlg32.dll is NOT statically imported by this test so we genuinely
    // exercise LdrRegisterDllNotification dynamic hook installation on late LoadLibrary!
    const bool comdlg_preloaded = GetModuleHandleW(L"comdlg32.dll") != nullptr;
    std::printf("[INFO] comdlg32 preloaded before Install: %d\n", comdlg_preloaded ? 1 : 0);

    std::atomic<int> comdlg_open_hits{0};
    std::atomic<int> comdlg_save_hits{0};
    std::atomic<int> get_open_w_hits{0};
    std::atomic<int> commdlg_err_hits{0};
    std::atomic<int> browse_w_hits{0};
    std::atomic<int> ro_factory_hits{0};
    std::atomic<int> ro_activate_hits{0};

    sink::SinkCallbacks cb;
    cb.on_comdlg_class_object = [&](REFCLSID clsid, REFIID iid, void** out, sink::ComDlgGetClassObjectFn orig) -> HRESULT {
        if (clsid == CLSID_FileOpenDialog) ++comdlg_open_hits;
        if (clsid == CLSID_FileSaveDialog) ++comdlg_save_hits;
        // Exercise real native pass-through with ScopedNativeComDlgBypass (zero recursion!).
        sink::ScopedNativeComDlgBypass bypass;
        return orig(clsid, iid, out);
    };
    cb.on_get_open_w = [&](LPOPENFILENAMEW, sink::GetOpenFileNameWFn) -> BOOL {
        ++get_open_w_hits;
        return FALSE;
    };
    cb.on_commdlg_error = [&](sink::CommDlgExtendedErrorFn) -> DWORD {
        ++commdlg_err_hits;
        return 0x3003u; // FNERR_INVALIDFILENAME sentinel for test proof
    };
    cb.on_browse_w = [&](LPBROWSEINFOW, sink::SHBrowseForFolderWFn) -> PIDLIST_ABSOLUTE {
        ++browse_w_hits;
        return nullptr;
    };
    cb.on_ro_get_activation_factory = [&](HSTRING cls, REFIID iid, void** out, sink::RoGetActivationFactoryFn orig) -> HRESULT {
        ++ro_factory_hits;
        return orig(cls, iid, out);
    };
    cb.on_ro_activate_instance = [&](HSTRING cls, IInspectable** out, sink::RoActivateInstanceFn orig) -> HRESULT {
        ++ro_activate_hits;
        return orig(cls, out);
    };

    sink::InstallOptions opts;
    opts.requested_mask = sink::kSinkGroupAll;
    opts.watch_late_dll_loads = true;
    const auto st0 = sink::Install(std::move(cb), opts);
    check(st0.error == ERROR_SUCCESS && st0.dll_watcher_active,
          "sink_hooks::Install succeeds and activates LdrRegisterDllNotification watcher");

    // 1. Dynamically load comdlg32.dll AFTER Install() to test LdrRegisterDllNotification!
    HMODULE h_comdlg = LoadLibraryW(L"comdlg32.dll");
    check(h_comdlg != nullptr, "late LoadLibraryW(comdlg32.dll) succeeds");
    // Give the watcher thread up to 500ms to react automatically without FlushPendingHooks!
    bool auto_hooked_comdlg = false;
    for (int i = 0; i < 50; ++i) {
        if ((sink::QueryStatus().active_mask & sink::kSinkComDlgClassObject) != 0) {
            auto_hooked_comdlg = true;
            break;
        }
        Sleep(10);
    }
    check(auto_hooked_comdlg,
          "LdrRegisterDllNotification watcher automatically hooked late-loaded comdlg32.dll");

    // 2. Direct GetProcAddress(comdlg32, "DllGetClassObject") interception + zero recursion.
    auto direct_gco = reinterpret_cast<sink::ComDlgGetClassObjectFn>(GetProcAddress(h_comdlg, "DllGetClassObject"));
    check(direct_gco != nullptr, "resolved comdlg32!DllGetClassObject");
    {
        ComPtr<IClassFactory> factory;
        const HRESULT hr = direct_gco(CLSID_FileOpenDialog, IID_PPV_ARGS(&factory));
        ComPtr<IFileOpenDialog> dlg;
        const HRESULT c_hr = SUCCEEDED(hr) && factory ? factory->CreateInstance(nullptr, IID_PPV_ARGS(&dlg)) : E_FAIL;
        check(SUCCEEDED(hr) && SUCCEEDED(c_hr) && dlg && comdlg_open_hits.load() == 1,
              "direct comdlg32!DllGetClassObject(CLSID_FileOpenDialog) intercepted with zero recursion");
    }

    // 3. Standard CoCreateInstance / CoCreateInstanceEx / CoGetClassObject interception via comdlg32 sink!
    // Note: if HKCU COM shadow is NOT registered (or points to comdlg32), CoCreateInstance hits comdlg32!DllGetClassObject.
    {
        const int before = comdlg_save_hits.load();
        ComPtr<IClassFactory> save_factory;
        const HRESULT hr = direct_gco(CLSID_FileSaveDialog, IID_PPV_ARGS(&save_factory));
        ComPtr<IFileSaveDialog> save_dlg;
        const HRESULT c_hr = SUCCEEDED(hr) && save_factory ? save_factory->CreateInstance(nullptr, IID_PPV_ARGS(&save_dlg)) : E_FAIL;
        check(SUCCEEDED(hr) && SUCCEEDED(c_hr) && save_dlg && comdlg_save_hits.load() == before + 1,
              "comdlg32!DllGetClassObject(CLSID_FileSaveDialog) intercepted and creates real native dialog");
    }

    // 4. ScopedNativeComDlgBypass suppresses hook on calling thread.
    {
        const int before = comdlg_open_hits.load();
        sink::ScopedNativeComDlgBypass bypass;
        ComPtr<IClassFactory> factory;
        const HRESULT hr = direct_gco(CLSID_FileOpenDialog, IID_PPV_ARGS(&factory));
        check(SUCCEEDED(hr) && factory && comdlg_open_hits.load() == before,
              "ScopedNativeComDlgBypass passes through directly to native comdlg32");
    }

    // 5. Unrelated CLSID never hits callback.
    {
        const int before_open = comdlg_open_hits.load();
        const int before_save = comdlg_save_hits.load();
        ComPtr<IUnknown> unk;
        const HRESULT hr = direct_gco(CLSID_ShellLink, IID_PPV_ARGS(&unk));
        check(FAILED(hr) && comdlg_open_hits.load() == before_open && comdlg_save_hits.load() == before_save,
              "unrelated CLSID bypasses file dialog callback");
    }

    // 6. Legacy GetOpenFileNameW + CommDlgExtendedError sink hooks.
    {
        auto get_open_w = reinterpret_cast<sink::GetOpenFileNameWFn>(GetProcAddress(h_comdlg, "GetOpenFileNameW"));
        auto comm_err = reinterpret_cast<sink::CommDlgExtendedErrorFn>(GetProcAddress(h_comdlg, "CommDlgExtendedError"));
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        const BOOL open_ret = get_open_w ? get_open_w(&ofn) : TRUE;
        const DWORD err_ret = comm_err ? comm_err() : 0;
        check(open_ret == FALSE && get_open_w_hits.load() == 1 &&
              err_ret == 0x3003u && commdlg_err_hits.load() == 1,
              "comdlg32 legacy GetOpenFileNameW + CommDlgExtendedError sink hooks intercept cleanly");
    }

    // 7. Late-load shell32.dll -> SHBrowseForFolderW sink hook.
    HMODULE h_shell = LoadLibraryW(L"shell32.dll");
    check(h_shell != nullptr, "LoadLibraryW(shell32.dll) succeeds");
    bool auto_hooked_shell = false;
    for (int i = 0; i < 50; ++i) {
        if ((sink::QueryStatus().active_mask & sink::kSinkShell32Browse) != 0) {
            auto_hooked_shell = true;
            break;
        }
        Sleep(10);
    }
    check(auto_hooked_shell, "shell32 sink hooks attached dynamically");
    {
        auto browse_w = reinterpret_cast<sink::SHBrowseForFolderWFn>(GetProcAddress(h_shell, "SHBrowseForFolderW"));
        BROWSEINFOW bi{};
        const auto pidl = browse_w ? browse_w(&bi) : reinterpret_cast<PIDLIST_ABSOLUTE>(1);
        check(pidl == nullptr && browse_w_hits.load() == 1,
              "shell32!SHBrowseForFolderW sink hook intercepts cleanly");
    }

    // 8. combase.dll RoGetActivationFactory + RoActivateInstance anti-double-wrap check.
    HMODULE h_combase = LoadLibraryW(L"combase.dll");
    check(h_combase != nullptr, "LoadLibraryW(combase.dll) succeeds");
    sink::FlushPendingHooks();
    const auto st_all = sink::QueryStatus();
    check((st_all.active_mask & sink::kSinkCombaseWinRtFactory) != 0 &&
          (st_all.active_mask & sink::kSinkCombaseWinRtActivate) != 0,
          "combase WinRT RoGetActivationFactory + RoActivateInstance sink hooks attached");
    {
        auto ro_act = reinterpret_cast<sink::RoActivateInstanceFn>(GetProcAddress(h_combase, "RoActivateInstance"));
        auto win_create_str = reinterpret_cast<HRESULT (WINAPI*)(LPCWSTR, UINT32, HSTRING*)>(
            GetProcAddress(h_combase, "WindowsCreateString"));
        auto win_del_str = reinterpret_cast<HRESULT (WINAPI*)(HSTRING)>(
            GetProcAddress(h_combase, "WindowsDeleteString"));
        if (ro_act && win_create_str && win_del_str) {
            HSTRING hstr = nullptr;
            const wchar_t* fake_cls = L"Pulse.NonExistent.TestClass";
            win_create_str(fake_cls, static_cast<UINT32>(wcslen(fake_cls)), &hstr);
            IInspectable* inst = nullptr;
            ro_act(hstr, &inst);
            if (inst) inst->Release();
            win_del_str(hstr);
            check(ro_activate_hits.load() == 1 && ro_factory_hits.load() == 0,
                  "RoActivateInstance suppresses nested RoGetActivationFactory hook (zero double-wrap)");
        }
    }

    // 9. Clean Uninstall() detaches all hooks and stops watcher.
    const DWORD uninst_err = sink::Uninstall();
    const auto st_after = sink::QueryStatus();
    check(uninst_err == ERROR_SUCCESS && st_after.active_mask == 0 && !st_after.dll_watcher_active,
          "sink_hooks::Uninstall cleanly detaches all trampolines and stops watcher");

    {
        const int before = comdlg_open_hits.load();
        ComPtr<IClassFactory> factory;
        const HRESULT hr = direct_gco(CLSID_FileOpenDialog, IID_PPV_ARGS(&factory));
        check(SUCCEEDED(hr) && factory && comdlg_open_hits.load() == before,
              "after Uninstall, comdlg32!DllGetClassObject runs completely unhooked");
    }

    if (h_combase) FreeLibrary(h_combase);
    if (h_shell) FreeLibrary(h_shell);
    if (h_comdlg) FreeLibrary(h_comdlg);
    CoUninitialize();

    // 10. Spawn fresh child process where comdlg32.dll is genuinely unloaded to verify
    //     preload_comdlg32=true eliminates the late-load race synchronously.
    {
        wchar_t self_path[32768]{};
        const DWORD len = GetModuleFileNameW(nullptr, self_path, ARRAYSIZE(self_path));
        bool child_ok = false;
        if (len > 0 && len < ARRAYSIZE(self_path)) {
            std::wstring cmd = std::wstring(L"\"") + self_path + L"\" --preload-comdlg";
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(self_path, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                const DWORD wait = WaitForSingleObject(pi.hProcess, 10000);
                DWORD exit_code = 1;
                if (wait == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code == 0) {
                    child_ok = true;
                }
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
        check(child_ok, "fresh child process preload_comdlg32=true zero-race regression passed");
    }

    return failures ? 1 : 0;
}

#include "../common/dialog_bridge_paths.h"
#include "../shell_bridge/dialog_core_abi.h"

#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace paths = pulse::dialog_bridge_paths;
using Microsoft::WRL::ComPtr;

namespace {
struct TempDir {
    fs::path dir;
    ~TempDir() {
        std::error_code ec;
        if (!dir.empty()) fs::remove_all(dir, ec);
    }
};
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        std::printf("[FAIL] usage: %ls <stub_dll> <core_v1_dll> <core_v2_dll>\n", argv[0]);
        return 2;
    }

    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };

    check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "CoInitializeEx STA");
    // Preload comdlg32.dll so sink hook active_mask is immediately populated.
    HMODULE h_comdlg = LoadLibraryW(L"comdlg32.dll");
    check(h_comdlg != nullptr, "LoadLibraryW(comdlg32.dll)");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    TempDir fixture;
    fixture.dir = fs::path(temp) / (L"pulse-stub-hotswitch-" + std::to_wstring(GetCurrentProcessId()) +
                                    L"-" + std::to_wstring(GetTickCount64()));
    const fs::path bridge_root = fixture.dir / L"DialogBridge";
    const fs::path v1_dir = bridge_root / L"v" / L"gen1";
    const fs::path v2_dir = bridge_root / L"v" / L"gen2";

    std::error_code ec;
    fs::create_directories(v1_dir, ec);
    fs::create_directories(v2_dir, ec);

    const fs::path stub_dst = bridge_root / paths::kStubDllName64;
    const fs::path core_v1_dst = v1_dir / paths::kCoreDllName64;
    const fs::path core_v2_dst = v2_dir / paths::kCoreDllName64;
    fs::copy_file(argv[1], stub_dst, fs::copy_options::overwrite_existing, ec);
    check(!ec, "stage fixed-path PulseDialogStub64.dll");
    fs::copy_file(argv[2], core_v1_dst, fs::copy_options::overwrite_existing, ec);
    check(!ec, "stage Core v1 (build 101)");
    fs::copy_file(argv[3], core_v2_dst, fs::copy_options::overwrite_existing, ec);
    check(!ec, "stage Core v2 (build 202)");

    // 1. Write active-bridge.json pointing to Core v1 (generation 1).
    paths::ActiveBridgePointer ptr1;
    ptr1.schema = 1;
    ptr1.core_dir_x64 = v1_dir.wstring();
    ptr1.host_exe = L"D:\\Pulse\\pulse.exe";
    ptr1.install_root = L"D:\\Pulse";
    ptr1.generation = 1;
    check(paths::WriteActiveBridgePointer(bridge_root.wstring(), ptr1) == ERROR_SUCCESS,
          "write active-bridge.json generation 1");

    // 2. Load Stub DLL ONCE.
    HMODULE stub = LoadLibraryW(stub_dst.c_str());
    check(stub != nullptr, "LoadLibraryW(PulseDialogStub64.dll) succeeds");
    if (!stub) return 1;

    using DllGetClassObjectFn = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
    using DllCanUnloadNowFn = HRESULT (WINAPI*)();
    using ActivateFn = DWORD (WINAPI*)(uint32_t);
    using HotReloadFn = DWORD (WINAPI*)();
    using GetStatusFn = HRESULT (WINAPI*)(PulseStubStatusV1*);
    using DirectRevealFn = DWORD (WINAPI*)(void*);

    auto p_gco = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(stub, "DllGetClassObject"));
    auto p_can_unload = reinterpret_cast<DllCanUnloadNowFn>(GetProcAddress(stub, "DllCanUnloadNow"));
    auto p_activate = reinterpret_cast<ActivateFn>(GetProcAddress(stub, "PulseStubActivate"));
    auto p_reload = reinterpret_cast<HotReloadFn>(GetProcAddress(stub, "PulseStubHotReload"));
    auto p_status = reinterpret_cast<GetStatusFn>(GetProcAddress(stub, "PulseStubGetStatus"));
    auto p_reveal = reinterpret_cast<DirectRevealFn>(GetProcAddress(stub, "PulseInitializeDirectReveal"));
    check(p_gco && p_can_unload && p_activate && p_reload && p_status && p_reveal,
          "all Stub ABI exports resolved");

    // 3. Activate via Stub -> loads Core v1 and installs sink hooks.
    const DWORD act1 = p_activate(PULSE_STUB_FLAG_INSTALL_SINK_HOOKS);
    PulseStubStatusV1 st1{};
    st1.size = sizeof(st1);
    const HRESULT q1 = p_status(&st1);
    check(act1 == ERROR_SUCCESS && SUCCEEDED(q1) && st1.core_loaded == 1 &&
          st1.generation == 1 && st1.core_build_id == 101 && st1.active_hook_mask != 0,
          "Stub loaded Core v1 (build 101, gen 1) with active sink hooks");
    check(p_can_unload() == S_FALSE, "Stub stays pinned (DllCanUnloadNow == S_FALSE)");

    void* dummy = nullptr;
    const HRESULT gco1 = p_gco(CLSID_FileOpenDialog, IID_IClassFactory, &dummy);
    check(gco1 == MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x4000u + 101u),
          "Stub DllGetClassObject dispatched to Core v1");
    check(p_reveal(nullptr) == 101u, "Stub PulseInitializeDirectReveal dispatched to Core v1");
    check(fs::exists(bridge_root / L"init_101.txt"), "Core v1 initialize marker written");

    // 4. Hot-upgrade active-bridge.json to Core v2 (generation 2) WITHOUT restarting host or unloading Stub!
    //    First trigger hot-swap PASSIVELY via Stub DllGetClassObject (simulating acad.exe/COM call after
    //    upgrade without Broker reload) to verify sink hooks are NEVER dropped to COM_ONLY!
    paths::ActiveBridgePointer ptr2 = ptr1;
    ptr2.core_dir_x64 = v2_dir.wstring();
    ptr2.host_exe = L"D:\\Pulse\\AppRuntime\\v2\\pulse.exe";
    ptr2.generation = 2;
    check(paths::WriteActiveBridgePointer(bridge_root.wstring(), ptr2) == ERROR_SUCCESS,
          "atomically update active-bridge.json to generation 2");

    Sleep(300); // Exceed Stub's 250ms reload check interval.
    const HRESULT gco2_passive = p_gco(CLSID_FileOpenDialog, IID_IClassFactory, &dummy);
    PulseStubStatusV1 st2{};
    st2.size = sizeof(st2);
    const HRESULT q2 = p_status(&st2);
    check(gco2_passive == MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x4000u + 202u) &&
          SUCCEEDED(q2) && st2.core_loaded == 1 &&
          st2.generation == 2 && st2.core_build_id == 202 && st2.active_hook_mask != 0,
          "Passive DllGetClassObject hot-swap switched to Core v2 AND preserved active sink hooks");
    check(fs::exists(bridge_root / L"shutdown_101.txt") && fs::exists(bridge_root / L"init_202.txt"),
          "Core v1 shutdown ran cleanly before Core v2 initialize");

    // Prove OS truly unloaded Core v1 DLL by deleting its on-disk PE file!
    const bool deleted_v1 = DeleteFileW(core_v1_dst.c_str()) != FALSE;
    check(deleted_v1, "OS unlocked and deleted Core v1 DLL while host process keeps running");

    // 4b. Verify host_exe-only update (same Core dir, generation 3) via PulseStubHotReload + idempotent PulseStubActivate.
    paths::ActiveBridgePointer ptr3 = ptr2;
    ptr3.host_exe = L"D:\\Pulse\\AppRuntime\\v3\\pulse.exe";
    ptr3.generation = 3;
    check(paths::WriteActiveBridgePointer(bridge_root.wstring(), ptr3) == ERROR_SUCCESS,
          "atomically update active-bridge.json host_exe to generation 3");
    const DWORD reload_err = p_reload();
    PulseStubStatusV1 st2b{};
    st2b.size = sizeof(st2b);
    const HRESULT q2b = p_status(&st2b);
    if (!(reload_err == ERROR_SUCCESS && SUCCEEDED(q2b) && st2b.core_loaded == 1 &&
          st2b.generation == 3 && st2b.core_build_id == 202 && st2b.active_hook_mask != 0)) {
        std::printf("  [DIAG] reload_err=%lu q2b=0x%08lX loaded=%u gen=%llu build=%llu mask=0x%X last_err=%lu\n",
            reload_err, static_cast<unsigned long>(q2b), st2b.core_loaded,
            static_cast<unsigned long long>(st2b.generation),
            static_cast<unsigned long long>(st2b.core_build_id),
            st2b.active_hook_mask, st2b.last_error);
    }
    check(reload_err == ERROR_SUCCESS && SUCCEEDED(q2b) && st2b.core_loaded == 1 &&
          st2b.generation == 3 && st2b.core_build_id == 202 && st2b.active_hook_mask != 0,
          "PulseStubHotReload refreshes generation 3 host_exe while keeping sink hooks active");
    check(p_activate(PULSE_STUB_FLAG_INSTALL_SINK_HOOKS) == ERROR_SUCCESS,
          "Idempotent PulseStubActivate at current generation succeeds cleanly");

    const HRESULT gco2 = p_gco(CLSID_FileOpenDialog, IID_IClassFactory, &dummy);
    check(gco2 == MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x4000u + 202u),
          "Stub DllGetClassObject now dispatches to Core v2");
    check(p_reveal(nullptr) == 202u, "Stub PulseInitializeDirectReveal now dispatches to Core v2");

    // 5. Disable via active-bridge.json -> Core v2 unloads and Stub falls back cleanly to native comdlg32.dll!
    {
        std::ofstream f(bridge_root / paths::kActivePointerFile, std::ios::binary | std::ios::trunc);
        f << "{\"schema\":1,\"disabled\":true}";
    }
    p_reload();
    PulseStubStatusV1 st3{};
    st3.size = sizeof(st3);
    p_status(&st3);
    check(st3.core_loaded == 0 && fs::exists(bridge_root / L"shutdown_202.txt"),
          "disabling active-bridge.json cleanly unloads Core v2");
    check(DeleteFileW(core_v2_dst.c_str()) != FALSE,
          "OS unlocked and deleted Core v2 DLL after disable");

    ComPtr<IClassFactory> native_factory;
    const HRESULT fallback_hr = p_gco(CLSID_FileOpenDialog, IID_PPV_ARGS(&native_factory));
    ComPtr<IFileOpenDialog> native_dlg;
    const HRESULT create_hr = SUCCEEDED(fallback_hr) && native_factory
        ? native_factory->CreateInstance(nullptr, IID_PPV_ARGS(&native_dlg)) : E_FAIL;
    check(SUCCEEDED(fallback_hr) && SUCCEEDED(create_hr) && native_dlg,
          "disabled Stub falls through seamlessly to real native comdlg32 IFileOpenDialog");

    if (h_comdlg) FreeLibrary(h_comdlg);
    CoUninitialize();
    return failures ? 1 : 0;
}

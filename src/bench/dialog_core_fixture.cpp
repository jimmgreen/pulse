#include "../shell_bridge/dialog_core_abi.h"
#include "../shell_bridge/dialog_sink_hooks.h"

#include <windows.h>
#include <shobjidl.h>
#include <atomic>
#include <string>

#ifndef PULSE_TEST_CORE_BUILD_ID
#define PULSE_TEST_CORE_BUILD_ID 101
#endif

namespace {

namespace sink = pulse::shell_bridge::sink_hooks;
std::wstring g_marker_dir;
std::atomic<bool> g_initialized{false};

void WriteMarker(const wchar_t* name) noexcept {
    if (g_marker_dir.empty()) return;
    const std::wstring path = g_marker_dir + L"\\" + name;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        const char body[] = "ok\n";
        DWORD written = 0;
        WriteFile(h, body, 3, &written, nullptr);
        CloseHandle(h);
    }
}

HRESULT WINAPI FixtureInitialize(const PulseStubConfigV1* cfg) noexcept {
    if (!cfg || cfg->size < sizeof(PulseStubConfigV1)) return E_INVALIDARG;
    g_marker_dir = cfg->bridge_root ? cfg->bridge_root : L"";
    g_initialized.store(true, std::memory_order_release);
    WriteMarker(L"init_" _CRT_WIDE(_CRT_STRINGIZE(PULSE_TEST_CORE_BUILD_ID)) L".txt");

    if (cfg->flags & PULSE_STUB_FLAG_INSTALL_SINK_HOOKS) {
        sink::SinkCallbacks cb;
        cb.on_comdlg_class_object = [](REFCLSID clsid, REFIID iid, void** out, sink::ComDlgGetClassObjectFn orig) -> HRESULT {
            sink::ScopedNativeComDlgBypass bypass;
            return orig(clsid, iid, out);
        };
        sink::InstallOptions opts;
        opts.requested_mask = sink::kSinkComDlgClassObject;
        opts.watch_late_dll_loads = true;
        const auto st = sink::Install(std::move(cb), opts);
        if (st.error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st.error);
    }
    return S_OK;
}

void WINAPI FixtureShutdown() noexcept {
    sink::Uninstall();
    if (g_initialized.exchange(false, std::memory_order_acq_rel)) {
        WriteMarker(L"shutdown_" _CRT_WIDE(_CRT_STRINGIZE(PULSE_TEST_CORE_BUILD_ID)) L".txt");
    }
}

HRESULT WINAPI FixtureDllGetClassObject(REFCLSID clsid, REFIID, void** out) noexcept {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (clsid != CLSID_FileOpenDialog && clsid != CLSID_FileSaveDialog) return CLASS_E_CLASSNOTAVAILABLE;
    // Return a distinct sentinel HRESULT carrying the Core build ID so the test
    // can prove which Core generation handled the Stub's DllGetClassObject call.
    return MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, static_cast<WORD>(0x4000u + PULSE_TEST_CORE_BUILD_ID));
}

DWORD WINAPI FixtureActivateProcess(const PulseStubConfigV1* cfg) noexcept {
    if (!cfg) return ERROR_INVALID_PARAMETER;
    if (sink::QueryStatus().active_mask == 0) {
        const HRESULT hr = FixtureInitialize(cfg);
        if (FAILED(hr)) return ERROR_GEN_FAILURE;
    }
    return ERROR_SUCCESS;
}

DWORD WINAPI FixtureDirectReveal(void*) noexcept {
    return static_cast<DWORD>(PULSE_TEST_CORE_BUILD_ID);
}

uint32_t WINAPI FixtureGetHookMask() noexcept {
    return sink::QueryStatus().active_mask;
}

} // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI PulseDialogCoreQueryV1(PulseDialogCoreV1* out) noexcept {
    if (!out || out->size < sizeof(PulseDialogCoreV1)) return E_INVALIDARG;
    PulseDialogCoreV1 t{};
    t.magic = PULSE_DIALOG_CORE_MAGIC;
    t.abi_version = PULSE_DIALOG_CORE_ABI_VERSION;
    t.size = sizeof(t);
    t.core_build_id = PULSE_TEST_CORE_BUILD_ID;
    t.initialize = &FixtureInitialize;
    t.shutdown = &FixtureShutdown;
    t.dll_get_class_object = &FixtureDllGetClassObject;
    t.activate_process = &FixtureActivateProcess;
    t.initialize_direct_reveal = &FixtureDirectReveal;
    t.get_active_hook_mask = &FixtureGetHookMask;
    *out = t;
    return S_OK;
}

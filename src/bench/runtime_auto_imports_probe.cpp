#include "../shell_bridge/runtime_auto_imports.h"
#include <objbase.h>
#include <array>
#include <atomic>

namespace {
HMODULE module = nullptr;
std::unique_ptr<pulse::shell_bridge::runtime::AutoImportPatch> patch;
pulse::shell_bridge::runtime::AutoImportResult result;
std::atomic<unsigned> calls[3];
HRESULT WINAPI Create(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID* output) { ++calls[0]; if (output) *output = nullptr; return E_ABORT; }
HRESULT WINAPI CreateEx(REFCLSID, IUnknown*, DWORD, COSERVERINFO*, DWORD count, MULTI_QI* output) {
    ++calls[1]; for (DWORD i = 0; output && i < count; ++i) { output[i].pItf = nullptr; output[i].hr = E_ABORT; } return E_ABORT;
}
HRESULT WINAPI Class(REFCLSID, DWORD, LPVOID, REFIID, LPVOID* output) { ++calls[2]; if (output) *output = nullptr; return E_ABORT; }
}
extern "C" __declspec(dllexport) HRESULT WINAPI ProbeInstall() {
    if (patch) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    patch = std::make_unique<pulse::shell_bridge::runtime::AutoImportPatch>();
    const std::array<pulse::shell_bridge::runtime::AutoImportHook, 3> hooks{{
        {reinterpret_cast<void*>(&CoCreateInstance), reinterpret_cast<void*>(&Create)},
        {reinterpret_cast<void*>(&CoCreateInstanceEx), reinterpret_cast<void*>(&CreateEx)},
        {reinterpret_cast<void*>(&CoGetClassObject), reinterpret_cast<void*>(&Class)}}};
    result = patch->Install(module, hooks);
    return result.status;
}
extern "C" __declspec(dllexport) HRESULT WINAPI ProbeRollback() {
    if (!patch) return S_FALSE;
    result = patch->Rollback(); patch.reset(); return result.status;
}
extern "C" __declspec(dllexport) unsigned WINAPI ProbeValue(unsigned index) {
    if (index < 3) return calls[index].load();
    if (index == 3) return result.api_mask;
    if (index == 4) return result.patched_slots;
    return result.protection_restored ? 1u : 0u;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { module = instance; DisableThreadLibraryCalls(instance); }
    return TRUE;
}

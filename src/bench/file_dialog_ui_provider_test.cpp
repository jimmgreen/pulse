#include "../shell_bridge/file_dialog_ui_provider.h"
#include <cstdio>
#include <memory>
#include <string>

namespace {
using pulse::shell_bridge::FileDialogInprocessProvider;
using pulse::shell_bridge::LoadFileDialogUiProvider;
constexpr wchar_t module_name[] = L"PulsePickerUiProof.dll";
unsigned checks = 0, failures = 0;
void Check(bool value, const char* label) {
    ++checks; if (!value) ++failures;
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
}
bool InModule(HMODULE module, const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    return module && address && VirtualQuery(address, &info, sizeof(info)) == sizeof(info) &&
        info.Type == MEM_IMAGE && info.State == MEM_COMMIT && info.AllocationBase == module;
}
void Rejected(const std::wstring& path, bool enabled, HRESULT expected, const char* label) {
    std::shared_ptr<const FileDialogInprocessProvider> output = std::make_shared<FileDialogInprocessProvider>();
    const HRESULT hr = LoadFileDialogUiProvider(path, output, enabled);
    std::printf("LOAD rejected-case hr=%08lX expected=%08lX output=%d\n",
        static_cast<unsigned long>(hr), static_cast<unsigned long>(expected), output != nullptr);
    Check(hr == expected && !output, label);
    Check(GetModuleHandleW(module_name) == nullptr, "rejection does not load proof module");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::wstring executable = argv[1];
    if (executable.size() < 3 || executable[1] != L':' || executable[2] != L'\\') {
        std::printf("absolute local pulse.exe argument required\n"); return 2;
    }
    Check(GetModuleHandleW(module_name) == nullptr, "fresh process has no proof module");
    if (failures) return 1;
    std::shared_ptr<const FileDialogInprocessProvider> output = std::make_shared<FileDialogInprocessProvider>();
    const HRESULT default_hr = LoadFileDialogUiProvider(executable, output);
    std::printf("DEFAULT hr=%08lX output=%d\n", static_cast<unsigned long>(default_hr), output != nullptr);
    Check(default_hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && !output, "omitted gate refuses and resets output");
    Check(GetModuleHandleW(module_name) == nullptr, "default gate leaves proof module unloaded");
    Rejected(executable, false, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "explicit disabled gate refuses actual executable");
    Rejected(L"pulse.exe", true, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "relative executable rejected");
    Rejected(L"\\\\localhost\\not-contacted\\pulse.exe", true, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "UNC rejected before remote access");
    const auto parent = executable.substr(0, executable.find_last_of(L'\\'));
    Rejected(parent + L"\\not-pulse.exe", true, E_INVALIDARG, "wrong executable leaf rejected before load");
    const HRESULT loaded = LoadFileDialogUiProvider(executable, output, true);
    std::printf("EXPLICIT hr=%08lX output=%d\n", static_cast<unsigned long>(loaded), output != nullptr);
    Check(loaded == S_OK && output, "explicit proof gate loads actual sibling UI module");
    if (loaded == S_OK && output) {
        const HMODULE module = GetModuleHandleW(module_name); // Observation never increments module refcount.
        Check(module != nullptr && output->module_lifetime, "provider holds loaded module lifetime");
        const auto& api = output->api;
        Check(api.size == sizeof(api) && api.version == PULSE_PICKER_UI_V2_VERSION && api.reserved == 0,
            "actual complete versioned API metadata");
        Check(api.capabilities == 0 && output->allow_unverified_proof,
            "cap-zero implementation is explicitly proof-only, not production certification");
        Check(InModule(module, reinterpret_cast<const void*>(api.Initialize)) &&
            InModule(module, reinterpret_cast<const void*>(api.ValidateConfig)) &&
            InModule(module, reinterpret_cast<const void*>(api.Show)) &&
            InModule(module, reinterpret_cast<const void*>(api.GetWindow)) &&
            InModule(module, reinterpret_cast<const void*>(api.Query)) &&
            InModule(module, reinterpret_cast<const void*>(api.Close)) &&
            InModule(module, reinterpret_cast<const void*>(api.Shutdown)) &&
            InModule(module, reinterpret_cast<const void*>(api.UpdateControl)), "all eight API functions belong to actual proof image");
        std::shared_ptr<const FileDialogInprocessProvider> duplicate = std::make_shared<FileDialogInprocessProvider>();
        const HRESULT repeated = LoadFileDialogUiProvider(executable, duplicate, true);
        std::printf("REPEAT hr=%08lX output=%d\n", static_cast<unsigned long>(repeated), duplicate != nullptr);
        Check(repeated == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) && !duplicate,
            "current loader policy rejects already mapped same-name module and clears output");
        Check(GetModuleHandleW(module_name) == module && output->module_lifetime,
            "rejected duplicate does not disturb original provider");
        auto retained = output;
        std::weak_ptr<void> lifetime = output->module_lifetime;
        output.reset();
        Check(!lifetime.expired() && GetModuleHandleW(module_name) == module,
            "another provider reference retains the actual image");
        retained.reset();
        Check(lifetime.expired(), "last provider reference releases guarded lifetime");
        // No Initialize/Show or old function pointer is ever called by this test.
        // This build does not PIN until Initialize; report unexpected residency.
        Check(GetModuleHandleW(module_name) == nullptr, "load-only provider releases uninitialized module mapping");
    }
    std::printf("SUMMARY checks=%u failures=%u (no Initialize, Show, or window operations)\n", checks, failures);
    return failures ? 1 : 0;
}

#include "module.h"
#include "../native_image_verify.h"
#include <filesystem>
namespace pulse::extensions::native {
Module::Module() noexcept : thread_(GetCurrentThreadId()) {}
Module::~Module() { Close(); }
PulseStatus Module::LoadVerified(const std::wstring& path, const std::string& expected_sha256,
    const PulseHostV1& host) {
    if (thread_ != GetCurrentThreadId() || invoking_ || library_ || verified_file_ != INVALID_HANDLE_VALUE)
        return PULSE_ERROR_STATE;
    if (host.size < sizeof(PulseHostV1) || host.version != PULSE_ABI_VERSION_1 || !host.query_interface)
        return PULSE_ERROR_ABI;
    std::string error;
    if (!OpenVerifiedNativeImage(path, expected_sha256, verified_file_, error, &ancestor_locks_)) {
        OutputDebugStringA(("Pulse extension native: " + error + "\n").c_str());
        return PULSE_ERROR_LOAD;
    }
    const auto status = Load(path, host);
    if (status != PULSE_OK) {
        CloseHandle(verified_file_);
        verified_file_ = INVALID_HANDLE_VALUE;
        for (HANDLE directory : ancestor_locks_) CloseHandle(directory);
        ancestor_locks_.clear();
    }
    return status;
}
PulseStatus Module::LoadVerified(const std::wstring& path, const NativeRuntime& native, const PulseHostV1& host) {
    if (native.files.empty()) return LoadVerified(path, native.sha256, host);
    if (thread_ != GetCurrentThreadId() || invoking_ || library_ || !package_locks_.empty()) return PULSE_ERROR_STATE;
    std::string error;
    if (!OpenVerifiedNativePackage(path, native, package_locks_, error)) {
        OutputDebugStringA(("Pulse extension native: " + error + "\n").c_str());
        return PULSE_ERROR_LOAD;
    }
    package_dependencies_ = true;
    const auto status = Load(path, host);
    if (status != PULSE_OK) {
        for (HANDLE handle : package_locks_) CloseHandle(handle);
        package_locks_.clear(); package_dependencies_ = false;
    }
    return status;
}
PulseStatus Module::Load(const std::wstring& path, const PulseHostV1& host) {
    if (thread_ != GetCurrentThreadId() || invoking_ || library_) return PULSE_ERROR_STATE;
    if (path.empty() || path.find(L'\0') != std::wstring::npos || !std::filesystem::path(path).is_absolute()) return PULSE_ERROR_ARGUMENT;
    if (host.size < sizeof(PulseHostV1) || host.version != PULSE_ABI_VERSION_1 || !host.query_interface) return PULSE_ERROR_ABI;
    // Private dependency search is enabled only for a fully verified package.
    const DWORD search = LOAD_LIBRARY_SEARCH_SYSTEM32 |
        ((package_dependencies_ || (verified_file_ == INVALID_HANDLE_VALUE && package_locks_.empty())) ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR : 0);
    library_ = LoadLibraryExW(path.c_str(), nullptr, search);
    if (!library_) return PULSE_ERROR_LOAD;
    // Before 1.0 the SDK changes between previews: refuse a plugin built
    // against another one before any of its entry code runs.
    const auto preview = reinterpret_cast<PulsePluginSdkPreviewFn>(GetProcAddress(library_, "PulsePluginSdkPreview"));
    const uint32_t built = preview ? preview() : 0;
    if (built != PULSE_SDK_PREVIEW) {
        const auto message = "Pulse extension native: built against SDK preview " + std::to_string(built) +
            (preview ? ", this Pulse requires preview " : " (export missing), this Pulse requires preview ") + std::to_string(PULSE_SDK_PREVIEW) +
            "; rebuild with the current SDK and PULSE_PLUGIN_SDK_PREVIEW()\n";
        OutputDebugStringA(message.c_str());
        FreeLibrary(library_); library_ = nullptr;
        return PULSE_ERROR_SDK_PREVIEW;
    }
    const auto entry = reinterpret_cast<PulsePluginEntryV1Fn>(GetProcAddress(library_, "PulsePluginEntryV1"));
    if (!entry) { FreeLibrary(library_); library_ = nullptr; return PULSE_ERROR_ENTRY; }
    host_ = host;
    plugin_ = {}; plugin_.size = sizeof(plugin_); plugin_.version = PULSE_ABI_VERSION_1;
    invoking_ = true;
    const auto status = entry(&host_, &plugin_);
    invoking_ = false;
    const bool valid = plugin_.size >= sizeof(PulsePluginV1) && plugin_.version == PULSE_ABI_VERSION_1 &&
        plugin_.instance && plugin_.activate && plugin_.deactivate && plugin_.destroy;
    if (status != PULSE_OK || !valid) {
        if (plugin_.instance && plugin_.destroy) { invoking_ = true; plugin_.destroy(plugin_.instance); invoking_ = false; }
        plugin_ = {}; host_ = {};
        FreeLibrary(library_); library_ = nullptr;
        return status == PULSE_OK ? PULSE_ERROR_ABI : status;
    }
    return PULSE_OK;
}
PulseStatus Module::Activate() {
    if (thread_ != GetCurrentThreadId() || invoking_ || !library_) return PULSE_ERROR_STATE;
    if (active_) return PULSE_OK;
    invoking_ = true;
    const auto status = plugin_.activate(plugin_.instance);
    invoking_ = false;
    if (status == PULSE_OK) active_ = true;
    else {
        // Failed activation must also release any partially registered work.
        invoking_ = true; plugin_.deactivate(plugin_.instance); invoking_ = false;
    }
    return status;
}
PulseStatus Module::Deactivate() {
    if (thread_ != GetCurrentThreadId() || invoking_) return PULSE_ERROR_STATE;
    if (!active_) return PULSE_OK;
    active_ = false;
    invoking_ = true; plugin_.deactivate(plugin_.instance); invoking_ = false;
    return PULSE_OK;
}
PulseStatus Module::Close() {
    if (thread_ != GetCurrentThreadId() || invoking_) return PULSE_ERROR_STATE;
    Deactivate();
    if (library_) {
        invoking_ = true; plugin_.destroy(plugin_.instance); invoking_ = false;
        plugin_ = {}; host_ = {};
        // Contract requires all plugin threads/callbacks stopped before return
        // from destroy. This is controlled shutdown, not arbitrary hot unload.
        FreeLibrary(library_); library_ = nullptr;
    }
    if (verified_file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(verified_file_);
        verified_file_ = INVALID_HANDLE_VALUE;
    }
    for (HANDLE directory : ancestor_locks_) CloseHandle(directory);
    ancestor_locks_.clear();
    for (HANDLE handle : package_locks_) CloseHandle(handle);
    package_locks_.clear(); package_dependencies_ = false;
    return PULSE_OK;
}
}

#include "user_dialog_auto_controller.h"
#include "../common/dialog_admin_auto_task.h"
#include "../common/dialog_admin_trust.h"
#include "../common/dialog_auto_repair.h"
#include "../common/dialog_bridge_paths.h"
#include "../common/dialog_bundle.h"
#include "../common/runtime_log.h"
#include "../dialog_launcher/user_auto_attach.h"
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace pulse::app {
namespace {

bool HasPointerFile(const std::filesystem::path& dir) {
    return std::filesystem::exists(dir / L"app-current.json") ||
           std::filesystem::exists(dir / L"dialog-current.json");
}

bool HasLegacyDialogPointer(const std::wstring& install_root) {
    return GetFileAttributesW((install_root + L"\\dialog-current.json").c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring NormalizeDir(std::wstring path) {
    if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

std::wstring ResolveInstallRootFromModule() {
    wchar_t path[32768]{};
    const DWORD len = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!len || len >= ARRAYSIZE(path)) return {};
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    // Installed layout: <InstallRoot>\AppRuntime\<sha256>\pulse.exe or <InstallRoot>\DialogBridge\<sha256>\pulse.exe.
    if (HasPointerFile(dir)) return dir.wstring();
    const auto grand = dir.parent_path().parent_path();
    if (!grand.empty() && HasPointerFile(grand)) return grand.wstring();
    const auto parent = dir.parent_path();
    if (!parent.empty() && HasPointerFile(parent)) return parent.wstring();
    return {};
}

void RepairRegistration(const std::wstring& install_root) {
    namespace auto_reg = pulse::dialog_auto;
    std::vector<auto_reg::InstalledDialogDll> installed;
    DWORD x64_error = ERROR_SUCCESS, x86_error = ERROR_SUCCESS;
    const bool legacy_layout = HasLegacyDialogPointer(install_root);
    if (!legacy_layout) {
        const auto common_root = pulse::dialog_bridge_paths::CommonDialogBridgeRoot();
        pulse::dialog_bridge_paths::ActiveBridgePointer ptr;
        if (!common_root.empty() &&
            pulse::dialog_bridge_paths::ReadActiveBridgePointer(common_root, ptr) == ERROR_SUCCESS &&
            !ptr.install_root.empty() &&
            CompareStringOrdinal(NormalizeDir(ptr.install_root).c_str(), -1,
                                 NormalizeDir(install_root).c_str(), -1, TRUE) == CSTR_EQUAL) {
            const auto stub64 = common_root + L"\\" + pulse::dialog_bridge_paths::kStubDllName64;
            if (GetFileAttributesW(stub64.c_str()) != INVALID_FILE_ATTRIBUTES) {
                installed.push_back({auto_reg::Architecture::X64, stub64});
            }
            const auto stub32 = common_root + L"\\" + pulse::dialog_bridge_paths::kStubDllName32;
            if (GetFileAttributesW(stub32.c_str()) != INVALID_FILE_ATTRIBUTES) {
                installed.push_back({auto_reg::Architecture::X86, stub32});
            }
        }
    } else {
        {
            pulse::dialog_bundle::BundleLease bundle;
            x64_error = pulse::dialog_bundle::ResolveInstalledBundleForMachine(install_root, IMAGE_FILE_MACHINE_AMD64, bundle);
            if (!x64_error) installed.push_back({auto_reg::Architecture::X64,
                bundle.Directory() + L"\\" + auto_reg::DllNameFor(auto_reg::Architecture::X64)});
        }
        if (GetFileAttributesW((install_root + L"\\dialog-x86-current.json").c_str()) != INVALID_FILE_ATTRIBUTES) {
            pulse::dialog_bundle::BundleLease bundle;
            x86_error = pulse::dialog_bundle::ResolveInstalledBundleForMachine(install_root, IMAGE_FILE_MACHINE_I386, bundle);
            if (!x86_error) installed.push_back({auto_reg::Architecture::X86,
                bundle.Directory() + L"\\" + auto_reg::DllNameFor(auto_reg::Architecture::X86)});
        }
    }
    const auto result = auto_reg::RebindStaleRegistration(install_root, installed);
    // The logon-started administrator helper exits while the registration names
    // another version; restart the existing task now that it names this one.
    HRESULT admin_restart = S_FALSE;
    if (legacy_layout && result.rebound && !result.error) admin_restart = pulse::dialog_admin_auto::StartExisting(install_root);
    if (result.rebound || result.error || x64_error || x86_error)
        pulse::diagnostics::runtime::Event("dialog_auto_rebind",
            {{"rebound", result.rebound}, {"error", result.error}, {"rollback_error", result.rollback_error},
             {"x64_bundle_error", x64_error}, {"x86_bundle_error", x86_error},
             {"admin_restart", static_cast<uint32_t>(admin_restart)}},
            result.error ? pulse::diagnostics::runtime::Level::Warning : pulse::diagnostics::runtime::Level::Info);
}

} // namespace

UserDialogAutoController::~UserDialogAutoController() {
    Stop();
    if (repair_.joinable()) repair_.join();
    if (repair_done_) CloseHandle(repair_done_);
}

void UserDialogAutoController::StartRepair(const std::wstring& install_root) {
    if (repair_done_ || install_root.empty()) return;
    repair_done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!repair_done_) return;
    HANDLE done = repair_done_;
    repair_ = std::thread([install_root, done]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        try { RepairRegistration(install_root); } catch (...) {}
        SetEvent(done);
    });
}

void UserDialogAutoController::Update(bool enabled, const std::wstring& data_dir) {
    if (dialog_admin::IsElevated()) {
        Stop();
        return;
    }
    const std::wstring install_root = ResolveInstallRootFromModule();
    StartRepair(install_root);
    // Migrated AppRuntime-only installs delegate process-wide injection to the
    // PulseDialogBroker service under Common Files\Pulse\DialogBridge.
    if (!enabled || data_dir.empty() || install_root.empty() || !HasLegacyDialogPointer(install_root)) {
        Stop();
        return;
    }
    if (running_.load(std::memory_order_acquire) &&
        current_root_ == install_root &&
        current_data_dir_ == data_dir) {
        return;
    }
    Stop();

    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) return;
    current_root_ = install_root;
    current_data_dir_ = data_dir;
    running_.store(true, std::memory_order_release);

    HANDLE stop = stop_event_;
    HANDLE repaired = repair_done_;
    worker_ = std::thread([this, install_root, data_dir, stop, repaired]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        if (repaired) {
            HANDLE waits[]{stop, repaired};
            if (WaitForMultipleObjects(2, waits, FALSE, 30000) == WAIT_OBJECT_0) {
                running_.store(false, std::memory_order_release);
                return;
            }
        }
        pulse::dialog_launcher::user_auto::Options opts;
        opts.installed_root = install_root;
        opts.data_dir = data_dir;
        opts.stop = stop;
        opts.log = [](const pulse::dialog_launcher::user_auto::Event& ev) {
            const std::wstring stage(ev.stage, ev.stage + std::strlen(ev.stage));
            pulse::diagnostics::runtime::TextEvent(
                "user_dialog_auto",
                {{"stage", stage}, {"app_basename", ev.app_basename}},
                {{"pid", ev.pid}, {"error", ev.error}, {"entries", ev.entries}, {"match_mask", ev.match_mask}},
                ev.error ? pulse::diagnostics::runtime::Level::Warning : pulse::diagnostics::runtime::Level::Info);
        };
        (void)pulse::dialog_launcher::user_auto::Run(opts);
        running_.store(false, std::memory_order_release);
    });
}

void UserDialogAutoController::Stop() {
    if (stop_event_) SetEvent(stop_event_);
    if (worker_.joinable()) worker_.join();
    if (stop_event_) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
    running_.store(false, std::memory_order_release);
    current_root_.clear();
    current_data_dir_.clear();
}

} // namespace pulse::app

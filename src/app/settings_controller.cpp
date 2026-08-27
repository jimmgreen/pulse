#include "settings_controller.h"
#include "../index/index_client.h"
#include "../index/network_agent_client.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <thread>

namespace pulse::app {

namespace {

template <size_t Size>
bool SelectValue(int index, const int (&values)[Size], int& target) {
    if (index < 0 || index >= static_cast<int>(Size) || target == values[index]) return false;
    target = values[index];
    return true;
}

} // namespace

SettingsController::~SettingsController() {
    Stop();
}

int SettingsController::PageFromName(std::wstring_view name) noexcept {
    if (name == L"index") return 1;
    if (name == L"context") return 2;
    if (name == L"about") return 3;
    return 0;
}

const wchar_t* SettingsController::PageName(int page) noexcept {
    if (page == 1) return L"index";
    if (page == 2) return L"context";
    if (page == 3) return L"about";
    return L"general";
}

void SettingsController::SelectPage(int page) noexcept {
    page_ = std::clamp(page, 0, 3);
    scroll_ = 0.0f;
}

void SettingsController::SetScroll(float value, float maximum) noexcept {
    scroll_ = std::clamp(value, 0.0f, (std::max)(0.0f, maximum));
}

void SettingsController::ScrollBy(float delta, float scale, float maximum) noexcept {
    SetScroll(scroll_ - delta / 120.0f * 48.0f * scale, maximum);
}

bool SettingsController::VolumePending(std::wstring_view id) const {
    std::lock_guard<std::mutex> lock(task_state_->mutex);
    return task_state_->pending_volume == id;
}

bool SettingsController::network_pending() const noexcept {
    std::lock_guard<std::mutex> lock(task_state_->mutex);
    return task_state_->network_pending;
}

bool SettingsController::diagnostics_pending() const noexcept {
    std::lock_guard<std::mutex> lock(task_state_->mutex);
    return task_state_->diagnostics_pending;
}

bool SettingsController::StartTask(SettingsTask task, SettingsTaskOperation operation,
                                   SettingsTaskCompletion completion) {
    if (!operation || !completion) return false;

    std::lock_guard<std::mutex> workers_lock(workers_mutex_);
    if (stopping_) return false;

    const bool network = IsNetworkTask(task.kind);
    {
        std::lock_guard<std::mutex> state_lock(task_state_->mutex);
        if (network) {
            if (task_state_->network_pending) return false;
            task_state_->network_pending = true;
        } else {
            if (task_state_->local_pending ||
                (task.kind == SettingsTaskKind::Volume && task.key.empty()) ||
                (task.kind == SettingsTaskKind::Exclude && task.path.empty())) return false;
            task_state_->local_pending = true;
            task_state_->diagnostics_pending =
                task.kind == SettingsTaskKind::DiagnosticsExport;
            if (task.kind == SettingsTaskKind::Volume)
                task_state_->pending_volume = task.key;
        }
    }

    // The operation and callback are deliberately moved into the worker. The
    // UI remains responsible only for posting the result back to its window.
    const auto state = task_state_;
    try {
        workers_.emplace_back([state, task = std::move(task), operation = std::move(operation),
                 completion = std::move(completion), network]() mutable {
        SettingsTaskResult result;
        result.task = task;
        try {
            result.ok = operation(task, result.error);
        } catch (...) {
            result.ok = false;
            if (result.error.empty()) result.error = L"设置操作异常终止。";
        }

        if (network) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->network_pending = false;
        } else {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->local_pending = false;
            state->diagnostics_pending = false;
            if (task.kind == SettingsTaskKind::Volume)
                state->pending_volume.clear();
        }
        completion(std::move(result));
        });
    } catch (...) {
        std::lock_guard<std::mutex> state_lock(state->mutex);
        if (network) state->network_pending = false;
        else {
            state->local_pending = false;
            state->diagnostics_pending = false;
            state->pending_volume.clear();
        }
        return false;
    }
    return true;
}

bool SettingsController::StartUiTask(SettingsTask task) {
    if (!ui_.task_completion) return false;
    auto* network = network_;
    const auto diagnostics_export = ui_.export_diagnostics;
    if (IsNetworkTask(task.kind) && !network) return false;
    return StartTask(std::move(task), [network, diagnostics_export](
                                      const SettingsTask& value, std::wstring& error) {
        switch (value.kind) {
        case SettingsTaskKind::Volume:
            return index::IndexClient::ConfigureVolumeElevated(value.key, value.enabled);
        case SettingsTaskKind::Exclude:
            return index::IndexClient::ConfigureExcludePathElevated(value.path, value.enabled);
        case SettingsTaskKind::InstallService:
            if (index::IndexClient::InstallServiceElevated()) return true;
            error = L"无法安装或启动 PulseIndex 服务。若刚覆盖安装，请查看 C:\\ProgramData\\Pulse\\index-service.log。";
            return false;
        case SettingsTaskKind::RebuildIndex:
            return index::IndexClient::RebuildElevated();
        case SettingsTaskKind::ConfigureIndexPath:
            return index::IndexClient::ConfigureIndexPathElevated(value.path);
        case SettingsTaskKind::NetworkAdd:
            return network->AddRoot(value.path, &error);
        case SettingsTaskKind::NetworkRemove:
            return network->RemoveRoot(value.path, &error);
        case SettingsTaskKind::NetworkRebuild:
            network->Rebuild();
            return true;
        case SettingsTaskKind::DiagnosticsExport:
            return diagnostics_export &&
                diagnostics_export(value.path, value.enabled, error);
        }
        return false;
    }, ui_.task_completion);
}

SettingsTaskEffect SettingsController::CompleteTask(const SettingsTaskResult& result,
                                                    bool service_installed) {
    SettingsTaskEffect effect;
    if (result.ok) {
        error_.clear();
        if (result.task.kind == SettingsTaskKind::NetworkAdd && result.task.pin)
            effect.pin_network = result.task.path;
        if (result.task.kind == SettingsTaskKind::DiagnosticsExport)
            effect.open_path = result.task.path;
        if (!IsNetworkTask(result.task.kind) &&
            result.task.kind != SettingsTaskKind::DiagnosticsExport) {
            service_installed_ = service_installed;
            effect.refresh_index = true;
        }
        return effect;
    }

    if (!result.error.empty()) {
        error_ = result.error;
    } else if (result.task.kind == SettingsTaskKind::InstallService) {
        error_ = result.error.empty()
            ? L"无法安装或启动索引服务。管理员授权可能已取消，或服务启动后异常退出。"
            : result.error;
    } else if (result.task.kind == SettingsTaskKind::NetworkRemove) {
        error_ = L"无法移除服务器文件夹。";
    } else if (IsNetworkTask(result.task.kind)) {
        error_ = L"无法添加服务器文件夹。";
    } else {
        error_ = L"操作未完成。管理员授权可能已取消，或索引服务无法更新配置。";
    }
    return effect;
}

void SettingsController::BindUi(AppPrefs& prefs, ContextMenuPrefs& context,
                                index::IndexClient& index,
                                index::NetworkAgentClient& network,
                                UiCallbacks callbacks) {
    prefs_ = &prefs;
    context_ = &context;
    index_ = &index;
    network_ = &network;
    ui_ = std::move(callbacks);
}

void SettingsController::ResetUi() noexcept {
    prefs_ = nullptr;
    context_ = nullptr;
    index_ = nullptr;
    network_ = nullptr;
    ui_ = {};
}

void SettingsController::Apply(SettingsEffect effect) const {
    if (effect != SettingsEffect::None && ui_.apply_effects) ui_.apply_effects(effect);
}

void SettingsController::SaveAndApply(SettingsEffect effect) const {
    prefs_->Save();
    Apply(effect);
}

void SettingsController::WindowEffect(std::wstring_view effect_id) {
    static constexpr std::wstring_view ids[] = {
        L"none", L"acrylic-material", L"mica", L"mica-alt"
    };
    if (!prefs_ || prefs_->window_effect == effect_id ||
        std::find(std::begin(ids), std::end(ids), effect_id) == std::end(ids)) return;
    prefs_->window_effect.assign(effect_id);
    SaveAndApply(SettingsEffect::WindowMaterial);
}

void SettingsController::AccentChoice(bool system_choice, uint32_t rgb) {
    if (!prefs_) return;
    uint32_t current = 0;
    const bool following = !ParseAccentRgb(prefs_->accent_rgb, current);
    if (system_choice && following) return;
    std::wstring next;
    if (!system_choice && (following || current != rgb)) {
        wchar_t hex[8]{};
        swprintf_s(hex, L"%06X", rgb & 0xFFFFFFu);
        next = hex;
    }
    if (prefs_->accent_rgb == next) return;
    prefs_->accent_rgb = std::move(next);
    SaveAndApply(SettingsEffect::Accent);
}

void SettingsController::RowHeight(int index) {
    static constexpr int values[] = {28, 34, 40};
    if (prefs_ && SelectValue(index, values, prefs_->row_height))
        SaveAndApply(SettingsEffect::RowHeight);
}

void SettingsController::TrayIconSize(int index) {
    static constexpr int values[] = {40, 48, 56};
    if (prefs_ && SelectValue(index, values, prefs_->tray_icon_size))
        SaveAndApply(SettingsEffect::TrayDeckIcon);
}

void SettingsController::Language(std::wstring_view language_id) {
    static constexpr std::wstring_view ids[] = {L"system", L"zh-CN", L"en-US"};
    if (!prefs_ || prefs_->language == language_id ||
        std::find(std::begin(ids), std::end(ids), language_id) == std::end(ids)) return;
    prefs_->language.assign(language_id);
    SaveAndApply(SettingsEffect::Language);
}

void SettingsController::Wallpaper(int action) {
    if (!prefs_ || !ui_.apply_effects) return;
    std::wstring path;
    if (action == 0 && ui_.pick_image && ui_.pick_image(path)) {
        if (!path.empty() && prefs_->StoreBackgroundImage(path))
            SaveAndApply(SettingsEffect::WindowMaterial);
    } else if (action == 1 && !prefs_->background_image.empty()) {
        prefs_->ClearBackgroundImage();
        SaveAndApply(SettingsEffect::WindowMaterial);
    }
}

void SettingsController::ToggleUi(int index) {
    if (!prefs_ || !context_) return;
    if (index == 1) {
        prefs_->ApplyLaunchOnStartup(!prefs_->launch_on_startup);
        SaveAndApply(SettingsEffect::None);
    } else if (index == 2) {
        prefs_->keep_running_on_close = !prefs_->keep_running_on_close;
        SaveAndApply(SettingsEffect::TrayVisibility);
    } else if (index == 3) {
        prefs_->ApplyFolderOpen(!prefs_->open_folders_in_pulse);
        SaveAndApply(SettingsEffect::None);
    } else if (index >= 10 && index < 15) {
        static constexpr ipc::CtxMenuGroup groups[] = {
            ipc::CtxMenuGroup::Software, ipc::CtxMenuGroup::OpenWith,
            ipc::CtxMenuGroup::Share, ipc::CtxMenuGroup::System, ipc::CtxMenuGroup::Print
        };
        const auto group = groups[index - 10];
        context_->SetGroupEnabled(group, !context_->GroupEnabled(group));
        context_->Save();
    } else if (index >= 100) {
        const size_t item = static_cast<size_t>(index - 100);
        if (item >= context_->seen.size()) return;
        const auto& seen = context_->seen[item];
        const bool enabled = context_->ItemEnabled(seen.key, seen.category, seen.from_com);
        context_->SetItemEnabled(seen.key, !enabled);
        context_->Save();
    }
}

void SettingsController::ToggleVolume(int position) {
    if (!index_ || !ui_.task_completion) return;
    const auto volumes = index_->Volumes();
    if (position < 0 || position >= static_cast<int>(volumes.size())) return;
    const auto& volume = volumes[static_cast<size_t>(position)];
    if (!index_->ServiceMode() || !volume.supported) return;
    ClearError();
    SettingsTask task{SettingsTaskKind::Volume};
    task.key = volume.id;
    task.enabled = !volume.enabled;
    StartUiTask(std::move(task));
}

void SettingsController::AddExclude() {
    if (!index_ || !index_->ServiceMode() || !ui_.pick_folder || !ui_.task_completion) return;
    std::wstring path;
    if (!ui_.pick_folder(path, L"选择要排除的本地文件夹")) return;
    ClearError();
    SettingsTask task{SettingsTaskKind::Exclude};
    task.path = std::move(path);
    task.enabled = true;
    StartUiTask(std::move(task));
}

void SettingsController::RemoveExclude(int position) {
    if (!index_ || !index_->ServiceMode() || !ui_.task_completion) return;
    const auto paths = index_->ExcludedPaths();
    if (position < 0 || position >= static_cast<int>(paths.size())) return;
    ClearError();
    SettingsTask task{SettingsTaskKind::Exclude};
    task.path = paths[static_cast<size_t>(position)];
    StartUiTask(std::move(task));
}

void SettingsController::IndexAction(int action) {
    if (!index_) return;
    if (action == 1) {
        const std::wstring path = index_->IndexPath();
        if (!path.empty() && ui_.open_path) ui_.open_path(path);
        return;
    }
    if ((action != 0 && action != 2) || !ui_.task_completion) return;
    std::wstring path;
    const bool set_path = action == 2 && index_->ServiceMode();
    if (set_path && (!ui_.pick_folder ||
        !ui_.pick_folder(path, L"选择索引存储位置"))) return;
    ClearError();
    SettingsTask task{action == 0 ? SettingsTaskKind::RebuildIndex
        : set_path ? SettingsTaskKind::ConfigureIndexPath
                   : SettingsTaskKind::InstallService};
    task.path = std::move(path);
    StartUiTask(std::move(task));
}

void SettingsController::NetworkAction(int action, bool pin_after_add) {
    if (!network_ || !ui_.task_completion) return;
    if (action == 1) {
        ClearError();
        StartUiTask(SettingsTask{SettingsTaskKind::NetworkRebuild});
        return;
    }
    if (action != 0 || !ui_.pick_folder) return;
    std::wstring path;
    if (!ui_.pick_folder(path, L"选择要索引的服务器文件夹")) return;
    ClearError();
    SettingsTask task{SettingsTaskKind::NetworkAdd};
    task.path = std::move(path);
    task.pin = pin_after_add;
    StartUiTask(std::move(task));
}

void SettingsController::RemoveNetwork(int position) {
    if (!network_ || !ui_.task_completion) return;
    const auto roots = network_->Roots();
    if (position < 0 || position >= static_cast<int>(roots.size())) return;
    ClearError();
    SettingsTask task{SettingsTaskKind::NetworkRemove};
    task.path = roots[static_cast<size_t>(position)].path;
    StartUiTask(std::move(task));
}

void SettingsController::DiagnosticsAction(int action) {
    if (action == 0) {
        if (ui_.open_diagnostics) ui_.open_diagnostics();
        return;
    }
    if (action == 1) {
        if (!ui_.clear_diagnostics) return;
        std::wstring error;
        if (ui_.clear_diagnostics(error)) ClearError();
        else SetError(std::move(error));
        return;
    }
    if (action != 2 || !ui_.prepare_diagnostics_export ||
        !ui_.export_diagnostics || !ui_.task_completion) return;
    std::wstring destination;
    bool include_service = false;
    if (!ui_.prepare_diagnostics_export(destination, include_service) ||
        destination.empty()) return;
    ClearError();
    SettingsTask task{SettingsTaskKind::DiagnosticsExport};
    task.path = std::move(destination);
    task.enabled = include_service;
    StartUiTask(std::move(task));
}

void SettingsController::Stop() {
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        stopping_ = true;
        workers.swap(workers_);
    }
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}

} // namespace pulse::app

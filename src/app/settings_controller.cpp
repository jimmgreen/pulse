#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "settings_controller.h"
#include "blank_pane_click.h"
#include "default_file_manager.h"
#include "../index/index_client.h"
#include "../index/network_agent_client.h"

#include <algorithm>
#include <array>
#include <cstdlib>
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
    if (name == L"duplicates") return 4;
    return 0;
}

const wchar_t* SettingsController::PageName(int page) noexcept {
    if (page == 1) return L"index";
    if (page == 2) return L"context";
    if (page == 3) return L"about";
    if (page == 4) return L"duplicates";
    return L"general";
}

void SettingsController::SelectPage(int page) noexcept {
    CancelGlobalSearchHotkeyCapture();
    page_ = std::clamp(page, 0, 4);
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

bool SettingsController::migration_pending() const noexcept {
    std::lock_guard<std::mutex> lock(task_state_->mutex);
    return task_state_->migration_pending;
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
            task_state_->migration_pending = task.kind == SettingsTaskKind::ConfigureIndexPath;
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
            if (result.error.empty()) result.error = l10n::Get(l10n::StringId::SettingsAborted).c_str();
        }

        if (network) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->network_pending = false;
        } else {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->local_pending = false;
            state->migration_pending = false;
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
            state->migration_pending = false;
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
        case SettingsTaskKind::InstallService: {
            DWORD code = 0;
            if (index::IndexClient::InstallServiceElevated(&code)) return true;
            wchar_t detail[512]{};
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, code, 0, detail, ARRAYSIZE(detail), nullptr);
            error = L"(" + std::to_wstring(code) + L") " + (detail[0]
                ? std::wstring(detail) : l10n::Get(l10n::StringId::SettingsServiceStartError));
            while (!error.empty() && (error.back() == L'\r' || error.back() == L'\n'))
                error.pop_back();
            return false;
        }
        case SettingsTaskKind::RebuildIndex:
            return index::IndexClient::RebuildElevated();
        case SettingsTaskKind::ConfigureIndexPath:
            return index::IndexClient::ConfigureIndexPathElevated(value.path, &error);
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
    if (result.task.kind == SettingsTaskKind::ConfigureIndexPath) effect.refresh_index = true;
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
            ? l10n::Get(l10n::StringId::SettingsServiceError).c_str()
            : result.error;
    } else if (result.task.kind == SettingsTaskKind::NetworkRemove) {
        error_ = l10n::Get(l10n::StringId::SettingsRemoveServerError).c_str();
    } else if (IsNetworkTask(result.task.kind)) {
        error_ = l10n::Get(l10n::StringId::SettingsAddServerError).c_str();
    } else {
        error_ = l10n::Get(l10n::StringId::SettingsOperationError).c_str();
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
    CancelGlobalSearchHotkeyCapture();
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
    if (!prefs_->Save() && ui_.show_error)
        ui_.show_error(l10n::HantText(prefs_->load_failed ?
            l10n::Pick(L"原设置未能读取，已阻止覆盖。请关闭后重试打开 Pulse。",
                L"The original settings could not be read. Saving is blocked to protect them. Restart Pulse to retry.") :
            l10n::Pick(L"设置未能保存，重启后可能恢复原值。",
                L"Settings could not be saved and may revert after a restart.")));
    Apply(effect);
}

void SettingsController::WindowEffect(std::wstring_view effect_id) {
    if (!compat::ModernWindows() && effect_id != L"none") return;
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
    std::wstring next;
    if (!system_choice) {
        wchar_t hex[8]{};
        swprintf_s(hex, L"%06X", rgb & 0xFFFFFFu);
        next = hex;
    }
    if (prefs_->accent_rgb == next && prefs_->accent_follow_system == system_choice) return;
    prefs_->accent_rgb = std::move(next);
    prefs_->accent_follow_system = system_choice;
    SaveAndApply(SettingsEffect::Accent);
}

void SettingsController::RowHeight(int index) {
    static constexpr int values[] = {28, 34, 40};
    if (prefs_ && SelectValue(index, values, prefs_->row_height))
        SaveAndApply(SettingsEffect::RowHeight);
}

void SettingsController::FolderSort(int index) {
    static constexpr int values[] = {0, 1, 2};
    if (prefs_ && SelectValue(index, values, prefs_->folder_sort_mode))
        SaveAndApply(SettingsEffect::FolderSort);
}

void SettingsController::DetailsColumns(uint32_t mask) {
    mask = ui::NormalizeDetailsColumns(mask);
    if (!prefs_ || prefs_->details_columns == mask) return;
    prefs_->details_columns = mask;
    SaveAndApply(SettingsEffect::ListStyle);
}

// Startup and new-tab locations are read when they are used; nothing to apply.
void SettingsController::StartupOpen(int index) {
    static constexpr int values[] = {0, 1};
    if (prefs_ && SelectValue(index, values, prefs_->startup_open))
        SaveAndApply(SettingsEffect::None);
}

void SettingsController::NotifyIcon(int index) {
    static constexpr int values[] = {0, 1, 2};
    if (prefs_ && SelectValue(index, values, prefs_->notify_icon_mode))
        SaveAndApply(SettingsEffect::TrayVisibility);
}

void SettingsController::NewTabOpen(int index) {
    static constexpr int values[] = {0, 1};
    if (prefs_ && SelectValue(index, values, prefs_->new_tab_open))
        SaveAndApply(SettingsEffect::None);
}

void SettingsController::BlankClick(int index) {
    static constexpr int values[] = {kBlankClickOff, kBlankClickBack, kBlankClickUp};
    if (prefs_ && SelectValue(index, values, prefs_->blank_click_action))
        SaveAndApply(SettingsEffect::None);
}

void SettingsController::HomeFolder(int action) {
    if (!prefs_) return;
    if (action == 0) {
        std::wstring path;
        if (!ui_.pick_folder ||
            !ui_.pick_folder(path, l10n::Get(l10n::StringId::SettingsHomeFolderTitle).c_str()) ||
            path.empty() || path == prefs_->home_folder) return;
        prefs_->home_folder = std::move(path);
    } else if (action == 1 && !prefs_->home_folder.empty()) {
        prefs_->home_folder.clear();
    } else {
        return;
    }
    SaveAndApply(SettingsEffect::None);
}

void SettingsController::TextRendering(int index) {
    static constexpr int values[] = {0, 1, 2};
    if (prefs_ && SelectValue(index, values, prefs_->text_render))
        SaveAndApply(SettingsEffect::TextRendering);
}

void SettingsController::UiFontSize(int index) {
    static constexpr int values[] = {90, 100, 112, 125};
    if (prefs_ && SelectValue(index, values, prefs_->ui_font_scale))
        SaveAndApply(SettingsEffect::UiFontSize);
}

void SettingsController::TrayIconSize(int index) {
    static constexpr int values[] = {40, 48, 56};
    if (prefs_ && SelectValue(index, values, prefs_->tray_icon_size))
        SaveAndApply(SettingsEffect::TrayDeckIcon);
}

// Both only change how the next frame is painted; the caller invalidates.
// Values snap to the former preset levels unless Shift is held.
bool SettingsController::SliderValue(int which, int value) {
    if (!prefs_ || which < 0 || which > 1) return false;
    const bool snap = GetKeyState(VK_SHIFT) >= 0;
    int& target = which == 0 ? prefs_->wallpaper_look : prefs_->wallpaper_blur;
    value = std::clamp(value, 0, which == 0 ? 90 : 40);
    if (snap) {
        static constexpr int kLook[] = {25, 50, 75};
        static constexpr int kBlur[] = {14, 28};
        if (which == 0) {
            for (int level : kLook) if (std::abs(value - level) <= 2) value = level;
        } else {
            for (int level : kBlur) if (std::abs(value - level) <= 1) value = level;
        }
    }
    if (target == value) return false;
    target = value;
    return true;
}

void SettingsController::EndSlider() {
    if (slider_drag_ < 0) return;
    slider_drag_ = -1;
    if (prefs_) prefs_->Save();
}

void SettingsController::Language(std::wstring_view language_id) {
    static constexpr std::wstring_view ids[] = {L"system", L"zh-CN", L"zh-TW", L"en-US"};
    if (!prefs_ || prefs_->language == language_id ||
        std::find(std::begin(ids), std::end(ids), language_id) == std::end(ids)) return;
    prefs_->language.assign(language_id);
    SaveAndApply(SettingsEffect::Language);
}

void SettingsController::Wallpaper(int action) {
    if (!prefs_ || !ui_.apply_effects) return;
    std::wstring path;
    if (action == 0 && ui_.pick_image && ui_.pick_image(path)) {
        if (!path.empty()) {
            if (prefs_->StoreBackgroundImage(path)) Apply(SettingsEffect::WindowMaterial);
            else if (ui_.show_error) ui_.show_error(l10n::HantText(l10n::Pick(
                L"新壁纸未能保存，已保留原壁纸。", L"The new wallpaper could not be saved. The previous wallpaper was kept.")));
        }
    } else if (action == 1 && !prefs_->background_image.empty()) {
        prefs_->ClearBackgroundImage();
        SaveAndApply(SettingsEffect::WindowMaterial);
    }
}

void SettingsController::ChangeTrackingDays(int days) {
    if (!prefs_ || (days != 1 && days != 3 && days != 7) ||
        prefs_->change_tracking_days == days) return;
    prefs_->change_tracking_days = days;
    SaveAndApply(SettingsEffect::ChangeTracking);
}

bool SettingsController::CaptureGlobalSearchHotkey(uint32_t key, uint32_t modifiers) {
    if (!global_search_capturing_) return false;
    if (key == VK_ESCAPE) { CancelGlobalSearchHotkeyCapture(); return true; }
    if (key == VK_CONTROL || key == VK_MENU || key == VK_SHIFT || key == VK_LWIN || key == VK_RWIN ||
        (key >= VK_LSHIFT && key <= VK_RMENU)) return true;
    if (!prefs_) return true;
    if (modifiers == 0 || (modifiers & ~15u) || key == 0 || key > 254) {
        global_search_error_ = l10n::Get(l10n::StringId::GlobalSearchInvalid);
        return true;
    }
    const auto previous_modifiers = prefs_->global_search_modifiers;
    const auto previous_key = prefs_->global_search_key;
    prefs_->global_search_modifiers = modifiers;
    prefs_->global_search_key = key;
    if (!prefs_->Save()) {
        prefs_->global_search_modifiers = previous_modifiers;
        prefs_->global_search_key = previous_key;
        global_search_error_ = l10n::Get(l10n::StringId::GlobalSearchSaveFailed);
        return true;
    }
    global_search_capturing_ = false;
    global_search_error_.clear();
    Apply(SettingsEffect::GlobalSearch);
    return true;
}

std::wstring SettingsController::GlobalSearchHotkeyText() const {
    if (!prefs_) return L"Alt + Space";
    std::wstring text;
    const auto modifiers = prefs_->global_search_modifiers;
    if (modifiers & MOD_CONTROL) text += L"Ctrl + ";
    if (modifiers & MOD_ALT) text += L"Alt + ";
    if (modifiers & MOD_SHIFT) text += L"Shift + ";
    if (modifiers & MOD_WIN) text += L"Win + ";
    const UINT key = prefs_->global_search_key;
    LONG scan = static_cast<LONG>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC) << 16);
    if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN ||
        key == VK_PRIOR || key == VK_NEXT || key == VK_END || key == VK_HOME ||
        key == VK_INSERT || key == VK_DELETE || key == VK_DIVIDE || key == VK_NUMLOCK) scan |= 1 << 24;
    wchar_t name[128]{};
    if (GetKeyNameTextW(scan, name, 128)) text += name;
    else text += L"VK " + std::to_wstring(key);
    return text;
}

bool SettingsController::IntegrationCanRestore() const noexcept {
    return prefs_ && (prefs_->integration_enabled || prefs_->open_folders_in_pulse ||
        prefs_->take_over_win_e || prefs_->take_over_this_pc || prefs_->integration_residual ||
        IntegrationCanRetry());
}

int SettingsController::IntegrationState() const noexcept {
    if (!prefs_) return 0;
    if (IntegrationCanRetry()) return 3;
    const auto& p = *prefs_;
    if (p.integration_incomplete) return 2;
    const bool active = p.open_folders_in_pulse || p.take_over_win_e || p.take_over_this_pc ||
        p.integration_residual;
    if (!p.integration_enabled) return active ? 2 : 0;
    if (p.open_folders_in_pulse != p.integration_folders ||
        p.take_over_win_e != p.integration_win_e || p.take_over_this_pc != p.integration_this_pc)
        return 2;
    return active || p.take_over_explorer_windows ? 1 : 0;
}

std::wstring SettingsController::IntegrationSummary() const {
    // Only problem details are surfaced; the status pill and checkboxes describe healthy states.
    if (!prefs_) return {};
    if (IntegrationCanRetry()) {
        std::wstring message;
        if (!integration_error_.empty())
            message = std::wstring(l10n::Pick(L"没能设置：", L"Could not apply: ")) + integration_error_ +
                l10n::Pick(L"。可能被安全软件拦截，可以重试。", L". Security software may have blocked it; try again.");
        if (integration_save_failed_) {
            if (!message.empty()) message += L" ";
            message += l10n::Pick(L"你的选择没能保存，重启后可能变回原来的设置。",
                L"Your choices could not be saved and may revert after a restart.");
        }
        return message;
    }
    if (IntegrationState() == 2) return l10n::Get(l10n::StringId::IntegrationDriftDesc);
    return {};
}

void SettingsController::IntegrationAction(int index) {
    if (!prefs_ || index < 0 || index > 6) return;
    if (ui_.integration_changing) ui_.integration_changing();
    auto& p = *prefs_;
    p.integration_configured = true;
    if (index == 0) p.integration_enabled = !p.integration_enabled;
    else if (index == 1) p.integration_folders = !p.integration_folders;
    else if (index == 2) p.integration_win_e = !p.integration_win_e;
    else if (index == 3) p.integration_this_pc = !p.integration_this_pc;
    else if (index == 4) p.take_over_explorer_windows = !p.take_over_explorer_windows;
    else if (index == 6) p.integration_enabled = false;
    // Editing a disabled integration only changes the saved selection.
    if (p.integration_enabled || index == 0 || index == 5 || index == 6) {
        integration_error_.clear();
        auto failed = [&](const wchar_t* label) {
            if (!integration_error_.empty()) integration_error_ += l10n::Pick(L"、", L", ");
            integration_error_ += label;
        };
        if (!p.ApplyFolderOpen(p.integration_enabled && p.integration_folders))
            failed(l10n::Pick(L"文件夹和磁盘", L"Folders and drives"));
        if (!p.ApplyWinE(p.integration_enabled && p.integration_win_e))
            failed(L"Win + E");
        if (!ApplyThisPcOpen(p, p.integration_enabled && p.integration_this_pc))
            failed(l10n::Pick(L"桌面上的「此电脑」", L"This PC on the desktop"));
    }
    integration_save_failed_ = !p.Save();
    if (ui_.integration_changed) ui_.integration_changed();
    Apply(SettingsEffect::None);
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
        IntegrationAction(1);
    } else if (index == 4) {
        prefs_->show_status_performance = !prefs_->show_status_performance;
        SaveAndApply(SettingsEffect::StatusBarPerformance);
    } else if (index == 5) {
        prefs_->show_hidden_files = !prefs_->show_hidden_files;
        SaveAndApply(SettingsEffect::FileVisibility);
    } else if (index == 16) {
        prefs_->show_protected_os_files = !prefs_->show_protected_os_files;
        SaveAndApply(SettingsEffect::FileVisibility);
    } else if (index == 6) {
        prefs_->show_pinned_tab_names = !prefs_->show_pinned_tab_names;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 34) {
        prefs_->multi_instance_mode = !prefs_->multi_instance_mode;
        SaveAndApply(SettingsEffect::MultiInstance);
    } else if (index == 20) {
        IntegrationAction(2);
    } else if (index == 28) {
        IntegrationAction(0);
    } else if (index == 29) {
        IntegrationAction(3);
    } else if (index == 30) {
        IntegrationAction(4);
    } else if (index == 31) {
        // app_updates.cpp reads it on every tick; turning it back on checks right away
        // because the skipped interval has already elapsed.
        prefs_->auto_check_updates = !prefs_->auto_check_updates;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 21) {
        // shell_tag_menu.cpp installs/removes the HKCU verbs on the next UI tick.
        prefs_->shell_tag_menu = !prefs_->shell_tag_menu;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 8) {
        prefs_->change_tracking_enabled = !prefs_->change_tracking_enabled;
        SaveAndApply(SettingsEffect::ChangeTracking);
    } else if (index == 9) {
        prefs_->search_pinyin = !prefs_->search_pinyin;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 21) {
        // The list-row switches from main's 1.0.39 rework. Ids 21-23, because 17-19 already
        // mean tooltips, file hashing and the title-bar mark on this branch.
        prefs_->list_smart_date = !prefs_->list_smart_date;
        SaveAndApply(SettingsEffect::ListStyle);
    } else if (index == 22) {
        prefs_->list_zebra_rows = !prefs_->list_zebra_rows;
        SaveAndApply(SettingsEffect::ListStyle);
    } else if (index == 23) {
        prefs_->list_size_bar = !prefs_->list_size_bar;
        SaveAndApply(SettingsEffect::ListStyle);
    } else if (index == 22) {
        prefs_->list_tag_name_color = !prefs_->list_tag_name_color;
        SaveAndApply(SettingsEffect::ListStyle);
    } else if (index == 33) {
        prefs_->list_selection_outline = !prefs_->list_selection_outline;
        SaveAndApply(SettingsEffect::ListStyle);
    } else if (index == 23) {
        prefs_->vertical_tabs = !prefs_->vertical_tabs;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 24) {
        prefs_->show_hints = !prefs_->show_hints;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 25) {
        prefs_->tips_seen = 0;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 26) {
        prefs_->close_window_with_last_tab = !prefs_->close_window_with_last_tab;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 32) {
        prefs_->confirm_recycle_delete = !prefs_->confirm_recycle_delete;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 27) {
        prefs_->start_in_tray = !prefs_->start_in_tray;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 15) {
        prefs_->global_search_enabled = !prefs_->global_search_enabled;
        if (!prefs_->Save()) {
            prefs_->global_search_enabled = !prefs_->global_search_enabled;
            global_search_error_ = l10n::Get(l10n::StringId::GlobalSearchSaveFailed);
            return;
        }
        global_search_error_.clear();
        Apply(SettingsEffect::GlobalSearch);
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
        if (item >= context_->seen.size()) {
            // Rows after the seen catalog are Pulse's own commands, in
            // BuiltinMenuItem order (app_runtime builds them the same way).
            const size_t builtin = item - context_->seen.size();
            if (builtin >= static_cast<size_t>(kBuiltinMenuItemCount)) return;
            const auto which = static_cast<BuiltinMenuItem>(builtin);
            context_->SetBuiltinVisible(which, !context_->BuiltinVisible(which));
            context_->Save();
            if (which == BuiltinMenuItem::RowNewTab || which == BuiltinMenuItem::RowStar ||
                which == BuiltinMenuItem::RowMore)
                Apply(SettingsEffect::ListStyle);
            return;
        }
        const auto& seen = context_->seen[item];
        const bool enabled = context_->ItemEnabled(seen.key, seen.category, seen.from_com);
        context_->SetItemEnabled(seen.key, !enabled);
        if (!enabled)
            context_->SetGroupEnabled(ipc::GroupOf(seen.category), true);
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
    if (!ui_.pick_folder(path, l10n::Get(l10n::StringId::SettingsPickExclude).c_str())) return;
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
    if (action != 1 && migration_pending()) return;
    if (action == 1) {
        const std::wstring path = index_->IndexPath();
        if (!path.empty() && ui_.open_path) ui_.open_path(path);
        return;
    }
    if ((action != 0 && action != 2) || !ui_.task_completion) return;
    std::wstring path;
    const bool set_path = action == 2 && index_->ServiceMode();
    if (set_path && (!ui_.pick_folder ||
        !ui_.pick_folder(path, l10n::Get(l10n::StringId::SettingsPickStorage).c_str()))) return;
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
    if (!ui_.pick_folder(path, l10n::Get(l10n::StringId::SettingsPickServer).c_str())) return;
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
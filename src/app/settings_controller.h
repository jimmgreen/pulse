#pragma once

#include "app_prefs.h"
#include "context_menu_prefs.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pulse::index { class IndexClient; class NetworkAgentClient; }

namespace pulse::app {

enum class SettingsEffect : uint32_t {
    None = 0,
    Accent = 1u << 0,
    WindowMaterial = 1u << 1,
    RowHeight = 1u << 2,
    TrayDeckIcon = 1u << 3,
    TrayVisibility = 1u << 4,
    Language = 1u << 5,
    StatusBarPerformance = 1u << 6,
    FileVisibility = 1u << 7,
    ChangeTracking = 1u << 8,
    GlobalSearch = 1u << 9,
    ListStyle = 1u << 10,
    FolderSort = 1u << 11,
    TextRendering = 1u << 12,
    UiFontSize = 1u << 13,
    MultiInstance = 1u << 14,
};

constexpr SettingsEffect operator|(SettingsEffect left, SettingsEffect right) noexcept {
    return static_cast<SettingsEffect>(static_cast<uint32_t>(left) |
                                       static_cast<uint32_t>(right));
}

constexpr bool HasEffect(SettingsEffect value, SettingsEffect effect) noexcept {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(effect)) != 0;
}

enum class SettingsTaskKind : uint8_t {
    Volume,
    Exclude,
    InstallService,
    RebuildIndex,
    ConfigureIndexPath,
    NetworkAdd,
    NetworkRebuild,
    NetworkRemove,
    DiagnosticsExport,
};

constexpr bool IsNetworkTask(SettingsTaskKind kind) noexcept {
    return kind == SettingsTaskKind::NetworkAdd ||
           kind == SettingsTaskKind::NetworkRebuild ||
           kind == SettingsTaskKind::NetworkRemove;
}

struct SettingsTask {
    SettingsTaskKind kind = SettingsTaskKind::Volume;
    std::wstring key;
    std::wstring path;
    bool enabled = false;
    bool pin = false;
};

struct SettingsTaskResult {
    SettingsTask task;
    bool ok = false;
    std::wstring error;
};

struct SettingsTaskEffect {
    bool refresh_index = false;
    std::wstring pin_network;
    std::wstring open_path;
};

using SettingsTaskOperation = std::function<bool(const SettingsTask&, std::wstring&)>;
using SettingsTaskCompletion = std::function<void(SettingsTaskResult)>;

class SettingsController {
public:
    struct UiCallbacks {
        std::function<bool(std::wstring&)> pick_image;
        std::function<bool(std::wstring&, std::wstring_view)> pick_folder;
        std::function<void(SettingsEffect)> apply_effects;
        std::function<void(const std::wstring&)> show_error;
        std::function<void()> integration_changing;
        std::function<void()> integration_changed;
        SettingsTaskCompletion task_completion;
        std::function<void(const std::wstring&)> open_path;
        std::function<void()> open_diagnostics;
        std::function<bool(std::wstring&)> clear_diagnostics;
        std::function<bool(std::wstring&, bool&)> prepare_diagnostics_export;
        std::function<bool(const std::wstring&, bool, std::wstring&)>
            export_diagnostics;
    };

    SettingsController() = default;
    ~SettingsController();
    SettingsController(const SettingsController&) = delete;
    SettingsController& operator=(const SettingsController&) = delete;

    static int PageFromName(std::wstring_view name) noexcept;
    static const wchar_t* PageName(int page) noexcept;

    void SelectPage(int page) noexcept;
    int page() const noexcept { return page_; }
    float scroll() const noexcept { return scroll_; }
    void SetScroll(float value, float maximum) noexcept;
    void ScrollBy(float delta, float scale, float maximum) noexcept;

    bool VolumePending(std::wstring_view id) const;
    bool network_pending() const noexcept;
    bool diagnostics_pending() const noexcept;
    bool migration_pending() const noexcept;

    SettingsTaskEffect CompleteTask(const SettingsTaskResult& result,
                                    bool service_installed);
    void Stop();

    const std::wstring& error() const noexcept { return error_; }
    void ClearError() { error_.clear(); }
    void SetError(std::wstring error) { error_ = std::move(error); }
    bool service_installed() const noexcept { return service_installed_; }
    void SetServiceInstalled(bool installed) noexcept { service_installed_ = installed; }

    void BindUi(AppPrefs& prefs, ContextMenuPrefs& context, index::IndexClient& index,
                index::NetworkAgentClient& network, UiCallbacks callbacks);
    void ResetUi() noexcept;
    bool ui_bound() const noexcept { return prefs_ != nullptr; }
    void WindowEffect(std::wstring_view effect_id);
    void AccentChoice(bool system_choice, uint32_t rgb);
    void RowHeight(int index);
    void FolderSort(int index);
    // Optional Details columns (ui/details_column_set.h bits), every folder.
    void DetailsColumns(uint32_t mask);
    void StartupOpen(int index);
    void NotifyIcon(int index);
    void NewTabOpen(int index);
    void BlankClick(int index);   // 0 nothing, 1 back, 2 up
    // 0 picks the default location's folder, 1 resets it to This PC.
    void HomeFolder(int action);
    void TextRendering(int index);
    void UiFontSize(int index);   // 0 small 90%, 1 default 100%, 2 large 112%, 3 larger 125%
    void TrayIconSize(int index);
    // Settings sliders: 0 interface transparency (0..90), 1 wallpaper blur (0..40).
    // Values apply live while dragging; EndSlider saves once.
    void BeginSlider(int which) noexcept { slider_drag_ = which; }
    int slider_drag() const noexcept { return slider_drag_; }
    bool SliderValue(int which, int value);
    void EndSlider();
    void Language(std::wstring_view language_id);
    void Wallpaper(int action);
    void ToggleUi(int index);
    void IntegrationAction(int index);
    int IntegrationState() const noexcept;
    std::wstring IntegrationSummary() const;
    bool IntegrationCanRestore() const noexcept;
    bool IntegrationCanRetry() const noexcept { return !integration_error_.empty() || integration_save_failed_; }
    void BeginGlobalSearchHotkeyCapture() noexcept { global_search_capturing_ = true; }
    void CancelGlobalSearchHotkeyCapture() noexcept { global_search_capturing_ = false; }
    bool CaptureGlobalSearchHotkey(uint32_t key, uint32_t modifiers);
    bool global_search_hotkey_capturing() const noexcept { return global_search_capturing_; }
    std::wstring GlobalSearchHotkeyText() const;
    void SetGlobalSearchError(std::wstring error) { global_search_error_ = std::move(error); }
    const std::wstring& global_search_error() const noexcept { return global_search_error_; }
    void ChangeTrackingDays(int days);
    void ToggleVolume(int index);
    void AddExclude();
    void RemoveExclude(int index);
    void IndexAction(int action);
    void NetworkAction(int action, bool pin_after_add = false);
    void RemoveNetwork(int index);
    void DiagnosticsAction(int action);

private:
    friend struct SettingsControllerTestPeer;

    struct TaskState {
        std::mutex mutex;
        std::wstring pending_volume;
        bool local_pending = false;
        bool network_pending = false;
        bool diagnostics_pending = false;
        bool migration_pending = false;
    };

    bool global_search_capturing_ = false;
    int slider_drag_ = -1;
    std::wstring global_search_error_;
    int page_ = 0;
    float scroll_ = 0.0f;
    std::shared_ptr<TaskState> task_state_ = std::make_shared<TaskState>();
    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    std::wstring error_;
    std::wstring integration_error_;
    bool integration_save_failed_ = false;
    bool service_installed_ = false;
    AppPrefs* prefs_ = nullptr;
    ContextMenuPrefs* context_ = nullptr;
    index::IndexClient* index_ = nullptr;
    index::NetworkAgentClient* network_ = nullptr;
    UiCallbacks ui_;

    bool StartTask(SettingsTask task, SettingsTaskOperation operation,
                   SettingsTaskCompletion completion);
    bool StartUiTask(SettingsTask task);
    void Apply(SettingsEffect effect) const;
    void SaveAndApply(SettingsEffect effect) const;
};

} // namespace pulse::app

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
    void TrayIconSize(int index);
    void Language(std::wstring_view language_id);
    void Wallpaper(int action);
    void ToggleUi(int index);
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
    };

    int page_ = 0;
    float scroll_ = 0.0f;
    std::shared_ptr<TaskState> task_state_ = std::make_shared<TaskState>();
    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    std::wstring error_;
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

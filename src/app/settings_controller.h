#pragma once

#include "app_prefs.h"
#include "pack_installer.h"
#include "context_menu_prefs.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pulse::ui { struct MediaPackOffer; }
namespace pulse::packs { enum class PackId : uint32_t; }

namespace pulse::index { class IndexClient; class NetworkAgentClient; }

namespace pulse::app {

class SystemCtxModel;
class ShortcutSettings;

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
    MenuMaterial = 1u << 14,
    SearchSwitch = 1u << 15,
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
    SystemExclusion,
    // 备份与迁移 (settings_transfer_ui.h): write a bundle (path, key = category
    // mask) / stage a bundle for the next start (pin: a backup to restore;
    // enabled: restart once staged).
    SettingsExport,
    SettingsImport,
};

constexpr bool IsSettingsTransferTask(SettingsTaskKind kind) noexcept {
    return kind == SettingsTaskKind::SettingsExport || kind == SettingsTaskKind::SettingsImport;
}

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
        std::function<bool(std::wstring&, bool&, bool&)> prepare_diagnostics_export;
        std::function<bool(const std::wstring&, bool, bool, std::wstring&)>
            export_diagnostics;
        // 备份与迁移: 0 export, 1 import, 2 restore / restart (UI thread), and
        // the worker side of SettingsExport / SettingsImport.
        std::function<void(int)> settings_transfer;
        SettingsTaskOperation settings_transfer_task;
        // 文件列表 > 缩略图缓存. Both run on a worker thread: the first gives
        // the bytes on disk (false: the cache has no folder), the second
        // empties it and gives the bytes freed.
        std::function<bool(uint64_t&)> thumb_cache_bytes;
        std::function<uint64_t()> clear_thumb_cache;
    };

    SettingsController() = default;
    ~SettingsController();
    SettingsController(const SettingsController&) = delete;
    SettingsController& operator=(const SettingsController&) = delete;

    static int PageFromName(std::wstring_view name) noexcept;
    static const wchar_t* PageName(int page) noexcept;

    void SelectPage(int page) noexcept;
    int page() const noexcept { return page_; }
    // The titles of the rows the 助手 changed last (「助手刚改的」); cleared
    // when it undoes them.
    void SetAssistantMarks(std::vector<std::wstring> titles) { assistant_marks_ = std::move(titles); }
    const std::vector<std::wstring>& AssistantMarks() const noexcept { return assistant_marks_; }
    // Settings lives in its title-bar button, not in the tab strip: the tab
    // to go back to when the button closes it (compared, never dereferenced).
    const void* return_tab() const noexcept { return return_tab_; }
    void set_return_tab(const void* tab) noexcept { return_tab_ = tab; }
    float scroll() const noexcept { return scroll_; }
    void SetScroll(float value, float maximum) noexcept;
    void ScrollBy(float delta, float scale, float maximum) noexcept;

    // Settings > 预览增强包 (page 5). The disk is read at most every 2 s while
    // the page is shown; actions write packs.json and re-read at once.
    struct PackState {
        uint32_t ffmpeg = 0;              // packs::ToolSource: 0 none, 1 pack, 2 own FFmpeg
        bool media_available = false, images_available = false;
        bool media_installed = false;     // the FFmpeg pack itself is on disk
        bool enabled = true, use_custom = false, remove_on_uninstall = true;
        bool auto_update = true;          // 自动更新预览增强包
        uint32_t installed = 0;
        uint64_t bytes = 0;
        std::wstring version, custom_path, detected_path, root, notice;
        // The catalog's newer release of an installed pack; empty when current.
        std::wstring update_version;
        bool installing = false;          // a download is running
        float progress = 0.0f;            // 0..1 while installing
        // The image pack (现代图像格式): its own card, download and message.
        bool images_installed = false, images_enabled = true;
        bool images_installing = false;
        float images_progress = 0.0f;
        std::wstring images_version, images_notice, images_update_version;
        bool raw_installed = false, raw_enabled = true, raw_installing = false;
        bool raw_available = false;
        float raw_progress = 0.0f;
        std::wstring raw_version, raw_notice, raw_update_version;
        bool archive_installed = false, archive_enabled = true, archive_installing = false;
        bool archive_available = false;
        float archive_progress = 0.0f;
        std::wstring archive_version, archive_notice, archive_update_version;
    };
    const PackState& Packs();

    // Settings > 文件列表 > 缩略图缓存. Counted on a worker when the page is
    // shown (again after 10 s); 清除缓存 runs there too. `notify` (an HWND)
    // is repainted when a result arrives.
    struct ThumbCacheState {
        enum Phase : uint8_t { Counting, Ready, Clearing, Cleared, Unavailable };
        Phase phase = Counting;
        uint64_t bytes = 0;
        uint64_t cleared = 0;  // freed by the last 清除缓存, shown while Cleared
    };
    ThumbCacheState ThumbCache(void* notify);
    void ClearThumbCache();
    // Each returns true when thumbnails have to be decoded again.
    // Starts the download, or cancels the running one. `notify` repaints.
    bool InstallMediaPack(HWND notify);
    // Once per finished download: true when a pack was installed.
    bool TakeMediaPackResult();
    // What Quick Look's codec cards offer. Cheap enough for every repaint:
    // the disk is read at most every 2 s and after each pack action.
    void FillMediaPackOffer(ui::MediaPackOffer& offer);
    bool RemoveMediaPack();
    bool ToggleMediaPack();
    // The same for the image pack.
    bool InstallImagePack(HWND notify);
    bool TakeImagePackResult();
    void FillImagePackOffer(ui::MediaPackOffer& offer);
    bool RemoveImagePack();
    bool ToggleImagePack();
    bool InstallRawPack(HWND notify);
    bool TakeRawPackResult();
    bool RemoveRawPack();
    bool ToggleRawPack();
    bool InstallArchivePack(HWND notify);
    bool TakeArchivePackResult();
    bool RemoveArchivePack();
    bool ToggleArchivePack();
    // Installs the catalog's newer release over an installed pack, copying
    // its unchanged files; the pack's switches stay as they are.
    bool UpdatePack(packs::PackId id, HWND notify);
    bool ToggleCustomFfmpeg();
    bool SetCustomFfmpeg(const std::wstring& path);
    bool UseDetectedFfmpeg();
    void ToggleRemovePacksOnUninstall();
    void ToggleAutoUpdatePacks();
    // Background updates (TickUpdates): 更新到 for every installed pack that
    // packs::PackAutoUpdateWanted picks. A failed one stays quiet - the
    // button remains and the next call tries again - and one the user
    // cancels is not retried until Pulse restarts. Returns how many started.
    int AutoUpdatePacks(HWND notify);

    bool VolumePending(std::wstring_view id) const;
    bool network_pending() const noexcept;
    bool diagnostics_pending() const noexcept;
    bool settings_transfer_pending() const noexcept;
    // Runs a SettingsExport / SettingsImport task; false while another runs.
    bool StartSettingsTransfer(SettingsTask task);
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
    void MtimeStyle(int index);     // 修改时间提示: off, dot, bar, text
    void MtimeRange(int index);     // today, 3, 7, 30 days
    void MtimePalette(int index);
    void ClickMode(int index);      // 鼠标与选择: 0/1 double/single click, 2/3 name band on hover/always
    // Optional Details columns (ui/details_column_set.h bits), every folder.
    void DetailsColumns(uint32_t mask);
    void StartupOpen(int index);
    void NotifyIcon(int index);
    void NewTabOpen(int index);
    void ExternalOpen(int index);  // plan §6 policy (multi-window M7b)
    void BlankClick(int index);   // 0 nothing, 1 back, 2 up
    void PreviewTrigger(int index);    // 0 hover card, 1 Space only
    void HoverPreviewDelay(int index); // 0.3 / 0.5 / 0.8 s
    void HoverCardSize(int index);     // 0 small, 1 medium, 2 large
    // 0 picks the default location's folder, 1 resets it to This PC.
    void HomeFolder(int action);
    void SetTerminalProgram(const std::wstring& path);
    void TextRendering(int index);
    void UiFontSize(int index);   // 0 small 90%, 1 default 100%, 2 large 112%, 3 larger 125%
    void TrayIconSize(int index);
    void DoubleCtrlGap(int index);   // 连按间隔: 200 / 300 / 400 ms
    // Settings sliders: 0 interface transparency (0..90), 1 wallpaper blur (0..40),
    // 2..4 custom accent hue (0..360), saturation and lightness (0..100).
    // Values apply live while dragging; EndSlider saves once.
    void BeginSlider(int which) noexcept { slider_drag_ = which; }
    int slider_drag() const noexcept { return slider_drag_; }
    bool SliderValue(int which, int value);
    void EndSlider();
    // 主题色 › 自定义: the sliders start from the accent in use.
    void SyncCustomAccent(uint32_t rgb) noexcept;
    const int* custom_accent() const noexcept { return custom_hsl_; }
    void Language(std::wstring_view language_id);
    void Wallpaper(int action);
    void ToggleUi(int index);
    // 本地右键菜单 › Pulse 菜单 tab: 0 on a file, 1 blank area, 2 row buttons.
    int ContextTab() const { return context_tab_; }
    // 系统右键菜单 (page 6): created on first use, scans on a worker thread.
    SystemCtxModel& SystemCtx();
    // 快捷键 (page 7): created on first use; probes key conflicts on a worker.
    ShortcutSettings& Shortcuts();
    // Pulse menu preview: a row pressed for dragging (BuiltinMenuItem), live
    // once the pointer moved 3 px along the tab's axis. EndMenuDrag drops
    // the drag; SetMenuOrder saves the order for the current tab.
    void BeginMenuDrag(int item, float pos, float grab) noexcept;
    bool MenuDragMove(float pos) noexcept;
    void EndMenuDrag() noexcept { menu_drag_item_ = -1; menu_drag_live_ = false; }
    int menu_drag_item() const noexcept { return menu_drag_item_; }
    bool menu_drag_live() const noexcept { return menu_drag_item_ >= 0 && menu_drag_live_; }
    float menu_drag_pos() const noexcept { return menu_drag_pos_; }
    float menu_drag_grab() const noexcept { return menu_drag_grab_; }
    void SetMenuOrder(const BuiltinMenuOrder& order);
    void IntegrationAction(int index);
    int IntegrationState() const noexcept;
    std::wstring IntegrationSummary() const;
    bool IntegrationCanRestore() const noexcept;
    bool IntegrationCanRetry() const noexcept { return !integration_error_.empty() || integration_save_failed_; }
    // A few seconds after the switch went on: the card shows a one-line confirmation.
    bool IntegrationJustEnabled() const noexcept;
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
    // 1 toggles the system folder switch, 2 expands its group list, and
    // 10 + i toggles index::kSystemExclusionGroups[i].
    void SystemExclusionAction(int action);
    bool system_exclusion_expanded() const noexcept { return system_exclusion_expanded_; }
    void IndexAction(int action);
    void NetworkAction(int action, bool pin_after_add = false);
    void RemoveNetwork(int index);
    // 0 open, 1 clear, 2 export diagnostics; 3..5 备份与迁移 (export, import,
    // restore or restart).
    void DiagnosticsAction(int action);

private:
    std::vector<std::wstring> assistant_marks_;
    const void* return_tab_ = nullptr;
    int context_tab_ = 0;
    std::shared_ptr<SystemCtxModel> sysctx_;  // shared_ptr: fine with the forward declaration
    std::shared_ptr<ShortcutSettings> shortcuts_;
    friend struct SettingsControllerTestPeer;

    struct TaskState {
        std::mutex mutex;
        std::wstring pending_volume;
        bool local_pending = false;
        bool network_pending = false;
        bool diagnostics_pending = false;
        bool transfer_pending = false;
        bool migration_pending = false;
    };

    bool global_search_capturing_ = false;
    bool system_exclusion_expanded_ = false;
    int slider_drag_ = -1;
    int custom_hsl_[3] = {220, 60, 50};
    int menu_drag_item_ = -1;
    bool menu_drag_live_ = false;
    float menu_drag_start_ = 0.0f, menu_drag_pos_ = 0.0f, menu_drag_grab_ = 0.0f;
    std::wstring global_search_error_;
    int page_ = 0;
    float scroll_ = 0.0f;
    PackState packs_;
    uint64_t packs_checked_ = 0;
    uint64_t offer_checked_ = 0;
    bool offer_missing_ = false;   // published, not installed, no own FFmpeg
    void RefreshPacks();
    PackInstaller media_installer_;
    uint64_t image_offer_checked_ = 0;
    bool image_offer_missing_ = false;   // published, not installed
    PackInstaller image_installer_;
    PackInstaller raw_installer_, archive_installer_;
    PackInstaller& InstallerFor(packs::PackId id);
    std::wstring& NoticeFor(packs::PackId id);
    bool InstallPack(packs::PackId id, HWND notify, bool automatic = false);
    bool TakePackResult(packs::PackId id);
    bool RemovePack(packs::PackId id);
    bool updating_[4] = {};   // per packs::PackId: the running job replaces an installed pack
    bool automatic_[4] = {};       // the running job was started by AutoUpdatePacks
    bool auto_declined_[4] = {};   // the user cancelled an automatic update: none again this run
    UpdateResponseReader pack_reader_ = ReadUpdateResponse;   // tests substitute a fake server
    std::shared_ptr<TaskState> task_state_ = std::make_shared<TaskState>();
    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
    // 缩略图缓存: one job at a time on its own thread, joined before the next.
    struct ThumbCacheJob {
        std::mutex mutex;
        ThumbCacheState state;
        bool busy = false, stopping = false;
        uint64_t checked = 0;
        void* notify = nullptr;
    };
    std::shared_ptr<ThumbCacheJob> thumb_cache_ = std::make_shared<ThumbCacheJob>();
    std::thread thumb_cache_worker_;
    void StartThumbCacheJob(bool clear);
    bool stopping_ = false;
    std::wstring error_;
    std::wstring integration_error_;
    bool integration_save_failed_ = false;
    unsigned long long integration_enabled_at_ = 0;
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

// sysctx_view.h — what Settings > 系统右键菜单 draws. Built by
// app::SystemCtxModel on the UI thread from its last scan; immutable once
// published, so view models share it instead of copying ~200 rows per frame.
#pragma once

#include <d2d1.h>

#include <array>
#include <string>
#include <vector>

namespace pulse::ui {

// HitTestResult::SettingsSysCtx indices.
enum SysCtxHit : int {
    kSysCtxRefresh = 0,
    kSysCtxApply = 1,        // write the queued changes (machine-wide ones: one UAC)
    kSysCtxDiscard = 2,      // drop the queue
    kSysCtxUndo = 3,         // reverse the last applied batch
    kSysCtxLog = 4,          // open the folder with ops.jsonl and the .reg backups
    kSysCtxRestoreAll = 5,   // reverse every batch applied in this session
    kSysCtxLocalLayer = 6,   // "Pulse 右键菜单" card: go to Settings page 2
    kSysCtxTabPreview = 7,   // side panel tabs
    kSysCtxTabDetail = 8,
    kSysCtxDetailSwitch = 9, // the switch inside the detail panel (selected row)
    kSysCtxDetailCopy = 10,  // copy the selected entry's registry path
    kSysCtxDetailFolder = 11,// open the selected entry's program folder
    kSysCtxFilterBase = 20,  // + SysCtxFilter
    kSysCtxSubBase = 40,     // + sub scene (extension / perceived type)
    kSysCtxSceneBase = 100,  // + scene index
    kSysCtxRowBase = 1000,   // + row index: the switch
    kSysCtxSelectBase = 3000,// + row index: the row itself (select)
    kSysCtxMoreBase = 5000,  // + row index: the "…" menu
    kSysCtxPreviewBase = 7000,// + preview item: press to drag (menu zones / 已隐藏 shelf)
    kSysCtxOverview = 12,     // scene list: the 概览 row
    kSysCtxDetailPulse = 13,  // detail panel: 在 Pulse 右键菜单中显示 (ContextMenuPrefs)
    kSysCtxDetailDelete = 14, // detail panel: 删除残留项 (queued)
    kSysCtxClean = 15,        // list head: 清理残留 (every leftover of the scene, queued)
    kSysCtxSlowSkipBase = 9000,    // + 概览 slow row: Pulse 跳过 (ContextMenuPrefs)
    kSysCtxSlowToggleBase = 9200,  // + 概览 slow row: 禁用 in Windows
    kSysCtxMarkRestoreBase = 9400, // + 概览 mark row: 还原 a ContextMenuManager mark
    kSysCtxRemoveBase = 11000,     // + row index: a leftover's own 删除 / 撤回 button (queued)
};

// Where a preview item can be dropped. Explorer orders the menu itself; a
// static verb can only be given Position = Top / Bottom.
enum SysCtxZone : int {
    kSysCtxZoneNone = -1,
    kSysCtxZoneTop = 0,      // Position = Top
    kSysCtxZoneDefault = 1,  // no Position value
    kSysCtxZoneBottom = 2,   // Position = Bottom
    kSysCtxZoneHidden = 3,   // 已隐藏 shelf: switched off
};

enum SysCtxFilter : int {
    kSysCtxFilterAll = 0,
    kSysCtxFilterOff,          // 已禁用
    kSysCtxFilterAdmin,        // 需要管理员 (HKLM)
    kSysCtxFilterThirdParty,   // 第三方
    kSysCtxFilterMissing,      // 残留项
    kSysCtxFilterDefaultHidden,// 系统默认隐藏
    kSysCtxFilterCount
};

struct SysCtxSceneView {
    std::wstring label;
    const wchar_t* glyph = L"\xE8A5";
    int group = 0;           // 0 基本对象, 1 特定类型, 2 专门菜单
    int count = 0;
    int disabled = 0;
    bool pending = false;    // has queued changes
};

struct SysCtxRowView {
    std::wstring title;      // menu text as Explorer shows it
    std::wstring subtitle;   // publisher · program, or why it is broken
    std::wstring icon_path;  // .exe/.ico for the shell icon cache; empty: glyph
    const wchar_t* glyph = L"\xE8A5";
    bool on = true;          // state after the queued change, if any
    bool pending = false;    // queued change, not written yet
    bool machine = false;    // HKLM (all users) rather than HKCU
    bool missing = false;    // its program/DLL is gone
    bool locked = false;     // protected (打开, 属性…) or owned by TrustedInstaller
    bool read_only = false;  // kinds whose switch comes in a later stage
    bool extended = false;   // only with Shift held
    bool system_owned = false; // TrustedInstaller
    bool default_hidden = false; // Windows ships it switched off
    bool third_party = false;
    bool selected = false;
    bool deletable = false;  // leftover whose key Pulse can delete
    bool deleting = false;   // its delete is queued
    int place = 0;           // effective SysCtxZone (Top / Default / Bottom), verbs only
    // Pulse's own menu (ContextMenuPrefs, Windows unchanged): 0 no Pulse row
    // for this kind, 1 shown in Pulse, 2 hidden in Pulse only.
    int pulse = 0;
    // Detail panel.
    std::wstring kind_label;  // 静态动词 / Shell 扩展 / …
    std::wstring registry;    // full key path
    bool registry_copyable = true;
    std::wstring file;        // program / DLL
    std::wstring publisher;
    std::wstring method;      // what switching it off writes
};

struct SysCtxPreviewItem {
    std::wstring title;
    std::wstring icon_path;
    const wchar_t* glyph = L"\xE8A5";
    int change = 0;           // 0 unchanged, 1 will be hidden, 2 will show
    bool extended = false;
    bool selected = false;
    bool hidden = false;      // off after the queued changes: drawn on the 已隐藏 shelf
    int zone = kSysCtxZoneDefault;  // effective placement (verbs; handlers stay Default)
    int rank = 1;             // menu order: 0 top verbs, 1 verbs, 2 handlers, 3 bottom verbs
    int moved = 0;            // queued placement change: 0 none, else 1 + target zone
    bool draggable = false;   // can be switched (shelf <-> menu)
    bool movable = false;     // static verb: can also change zone
};

// 概览 (the prototype's home scene): ContextMenuManager marks, the handlers
// Pulse measured while building its own menu, this session's changes.
struct SysCtxHandlerTiming {   // fed by the app: ContextMenuController + ContextMenuPrefs
    std::wstring clsid;
    uint32_t ms = 0;           // slowest query seen across the stored layouts
    bool pulse_skipped = false;// ContextMenuPrefs::ComDisabled
};
inline constexpr uint32_t kSysCtxSlowMs = 150;

struct SysCtxSlowRow {
    std::wstring title, publisher, icon_path, clsid;
    const wchar_t* glyph = L"\xE8A5";
    uint32_t ms = 0;
    bool on = true;            // after the queued change
    bool pending = false;
    bool cmm = false;          // off through a ContextMenuManager "-" marker
    bool pulse_skipped = false;
    bool switchable = false;   // 禁用 button: a scanned entry Pulse can switch
};
struct SysCtxMarkRow {         // 来自其他工具的禁用标记
    std::wstring title, registry, icon_path;
    const wchar_t* glyph = L"\xE8A5";
};
struct SysCtxChangeRow {       // 最近的改动
    std::wstring title, caption;
    bool undone = false;
};
struct SysCtxOverview {
    bool shown = false;        // the 概览 row is selected: cards replace the entry list
    int marks = 0;             // entries off through a ContextMenuManager marker
    int slow = 0;              // handlers measured at >= kSysCtxSlowMs
    int changes = 0;           // batches applied this session and not undone
    std::vector<SysCtxSlowRow> slow_rows;   // slowest first, at most 6
    std::vector<SysCtxMarkRow> mark_rows;
    std::vector<SysCtxChangeRow> recent;    // newest first, at most 4
};

struct SysCtxView {
    enum class State { Idle, Loading, Ready, Applying };
    State state = State::Idle;
    std::vector<SysCtxSceneView> scenes;
    int scene = 0;
    std::wstring scene_title;           // "文件夹"
    std::wstring scene_keys;            // "HKCR\Directory · HKCR\Folder"
    std::vector<std::wstring> subs;     // extensions / perceived types of the scene
    int sub = 0;
    int filter = kSysCtxFilterAll;
    std::array<int, kSysCtxFilterCount> filter_counts{};
    std::vector<SysCtxRowView> rows;    // the selected scene, after the filter
    int selected = -1;                  // index into rows
    std::vector<SysCtxPreviewItem> preview;  // the scene's entries, menu order; hidden ones go on the shelf
    int drag_item = -1;                 // preview item held by the pointer
    bool drag_live = false;             // moved past the threshold: the ghost follows
    float drag_y = 0.0f, drag_grab = 0.0f;  // pointer y and its offset inside the item
    int side_tab = 0;                   // 0 实时预览, 1 详情
    int total = 0, disabled = 0, missing = 0;  // across every scene
    int pending = 0;                    // queued changes
    int pending_admin = 0;              // of them: machine-wide (one UAC)
    int pending_pulse = 0;              // of them: Pulse's own menu only (ContextMenuPrefs)
    int leftovers = 0;                  // deletable leftovers in the scene, not queued yet
    int undo_steps = 0;
    std::wstring message;
    bool message_error = false;
    SysCtxOverview overview;
};

// Renderer geometry of the page (SettingsLayout::sysctx); filled by
// LayoutSettingsSysCtx in settings_sysctx_layout.h.
struct SysCtxDetailLayout {
    D2D1_RECT_F bounds{}, head{}, switch_row{}, toggle{}, pulse_row{}, pulse_toggle{}, method{}, copy{}, folder{};
    D2D1_RECT_F remove{};               // 删除残留项 / 取消删除
    std::array<D2D1_RECT_F, 4> kv{};    // 注册表, 文件, 发布者, 权限
};

// Live preview: menu surface with zones, the 已隐藏 shelf, and the drag
// ghost. LayoutSysCtxPreview in settings_sysctx_layout.h.
struct SysCtxPreviewLayout {
    D2D1_RECT_F head{}, menu{}, shelf_head{}, shelf{}, legend{};
    std::vector<D2D1_RECT_F> items;     // per preview item; empty rect when it does not fit
    std::array<D2D1_RECT_F, 3> zone_label{};  // while a verb is dragged
    D2D1_RECT_F more_menu{}, more_shelf{};
    int more_menu_count = 0, more_shelf_count = 0;
    bool dragging = false;
    int drop_zone = kSysCtxZoneNone;
    D2D1_RECT_F gap{}, ghost{};
};

// 概览 cards in the entry column. LayoutSysCtxOverview in settings_sysctx_layout.h.
struct SysCtxOverviewLayout {
    D2D1_RECT_F stats_card{}, slow_card{}, marks_card{}, recent_card{};
    std::array<D2D1_RECT_F, 4> stats{};
    std::vector<D2D1_RECT_F> slow_rows, slow_skip, slow_toggle;  // per slow row; empty rect: no button
    std::vector<D2D1_RECT_F> mark_rows, mark_restore, recent_rows;
    D2D1_RECT_F slow_empty{}, marks_empty{}, recent_empty{};
};

struct SysCtxLayout {
    float scale = 1.0f;
    bool wide = false;                  // three columns; otherwise details open under the row
    D2D1_RECT_F intro{}, undo{}, log{}, restore{};
    D2D1_RECT_F local_card{}, system_card{};
    D2D1_RECT_F scenes_card{};
    std::vector<D2D1_RECT_F> scenes;
    D2D1_RECT_F overview_row{};         // 概览, above the groups
    std::array<D2D1_RECT_F, 3> groups{};  // group captions
    D2D1_RECT_F list_card{}, list_head{}, refresh{}, clean{};
    std::vector<D2D1_RECT_F> filters, subs;
    D2D1_RECT_F rows_top{}, empty{};
    std::vector<D2D1_RECT_F> rows, switches, more;
    std::vector<D2D1_RECT_F> removes;   // per row: 删除 / 撤回 in the switch's place (leftovers only)
    SysCtxOverviewLayout overview;      // when SysCtxView::overview.shown (rows stay empty)
    D2D1_RECT_F side_card{}, tab_preview{}, tab_detail{}, side_body{};
    SysCtxDetailLayout detail;          // side panel (wide) or under the selected row
    D2D1_RECT_F notice{};               // result of the last batch
    D2D1_RECT_F pending_bar{}, apply{}, discard{};
};

} // namespace pulse::ui

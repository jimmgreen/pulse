// system_ctx_model.h — Settings > 系统右键菜单: scans Explorer's context menu
// registrations on a worker thread and switches them on/off.
//
// Per-user entries change at once; machine-wide ones queue until 应用, then go
// to pulse_integration.exe --system-ctx-apply in one elevated batch. Every
// write is preceded by a .reg export and logged, and each batch can be undone.
#pragma once

#include "system_ctx_registry.h"
#include "../ui/sysctx_view.h"

#include <windows.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::app {

class SystemCtxModel {
public:
    SystemCtxModel();
    ~SystemCtxModel();
    SystemCtxModel(const SystemCtxModel&) = delete;
    SystemCtxModel& operator=(const SystemCtxModel&) = delete;

    // Window repainted when a scan or batch finishes (any thread may call).
    void SetNotifyWindow(HWND hwnd);
    // First visit to the page: scan once in the background.
    void EnsureLoaded();
    void Refresh();

    // UI thread. Cheap when nothing changed since the last call.
    std::shared_ptr<const ui::SysCtxView> View();

    void SelectScene(int scene);
    void SelectSub(int sub);
    static std::wstring NormalizeExtension(std::wstring extension);
    bool SelectExtension(std::wstring extension); // one bounded custom scene; worker scan only
    void SetFilter(int filter);
    // Selects `row` of the current view (again: deselects) and shows 详情.
    void SelectRow(int row);
    void SetSideTab(int tab);
    // Flips the entry behind `row` of the current view; -1: the selected row.
    // Every registry change waits in the queue until ApplyPending (应用更改).
    void ToggleRow(int row);
    // 删除残留项: queues (again: withdraws) deleting the leftover behind `row`
    // (-1: the selected row). Applied with the rest; undo puts the key back.
    void DeleteRow(int row);
    // 清理残留: queues every leftover of the current scene; the count queued.
    int DeleteSceneLeftovers();
    bool RowDeletable(int row);   // a leftover Pulse can delete
    bool RowDeleting(int row);    // its delete is queued
    void ApplyPending();
    void DiscardPending();
    void Undo();
    void RestoreAll();  // reverses every batch applied this session

    // 概览, the first row of the scene list: cards instead of the entry list.
    void ShowOverview();
    // Pulse's own measurements of the handlers (fed while the page is open);
    // the 概览 ranks them. Cheap when nothing changed.
    void SetHandlerTimings(std::vector<ui::SysCtxHandlerTiming> timings);
    // The top-level rows each handler (CLSID) gave Pulse's own menu, from the
    // stored layouts: rows name a shell extension by what it adds.
    void SetHandlerLabels(std::map<std::wstring, std::vector<std::wstring>> labels);
    // 概览 buttons; `index` into the view's slow_rows / mark_rows.
    void ToggleSlowRow(int index);         // 禁用: like the row switch
    void RestoreMarkRow(int index);        // 还原: switches the marked entry on
    // ContextMenuPrefs catalog keys Pulse hides in its own menu (sorted); rows
    // show 在 Pulse 中隐藏. Cheap when nothing changed.
    void SetPulseHidden(std::vector<std::wstring> keys);
    // Pulse's own switches (只在 Pulse 隐藏, 概览 Pulse 跳过) wait in the
    // same queue as the registry ones: 应用更改 hands them to the caller
    // (TakePulseChanges, ContextMenuPrefs lives there), 放弃 drops them.
    void TogglePulseRow(int row);          // -1: the selected row
    bool RowPulseHidden(int row);          // after the queued change
    void QueueSlowSkip(int index);         // 概览 slow row: Pulse 跳过 (again: withdraws)
    struct PulseChanges {
        std::vector<std::pair<std::wstring, bool>> hidden;  // catalog key -> hide in Pulse
        std::vector<std::wstring> skipped;                  // CLSIDs Pulse stops loading
        bool empty() const { return hidden.empty() && skipped.empty(); }
    };
    PulseChanges TakePulseChanges();

    // Live preview drag (UI thread): a press arms it, moves past 3 px make the
    // ghost follow the pointer, the drop goes through DropPreview.
    void BeginPreviewDrag(int item, float y, float grab);
    bool PreviewDragMove(float y);  // true: repaint
    int PreviewDragItem() const;
    bool PreviewDragLive() const;
    void EndPreviewDrag();
    // Puts preview item `item` into a SysCtxZone: the 已隐藏 shelf switches it
    // off, a menu zone switches it on and (static verbs) sets Position. Queued
    // like the switches until 应用. Dropping an entry back where it was
    // cancels its queue.
    void DropPreview(int item, int zone);
    // A press without dragging selects the entry (list row and preview).
    void SelectPreview(int item);

    // "…" menu helpers for `row` (-1: the selected row); empty when there is none.
    std::wstring RowRegistryPath(int row);  // HKEY_…\Software\Classes\…
    std::wstring RowFile(int row);
    // Pulse's catalog key for the entry (ipc::HandlerCatalogKey / CatalogKey);
    // empty for kinds Pulse does not list as rows of its own.
    std::wstring RowPulseKey(int row);
    std::wstring RowPulseTreeRoot(int row); // ancestor root for the observed submenu editor
    std::wstring DataDirectory() const;     // holds ops.jsonl and the Backups folder
    std::wstring BackupDirectory() const;

    // Paths under %LOCALAPPDATA%\Pulse\SystemContextMenu (tests override).
    void SetDataDirectoryForTesting(std::wstring directory);

private:
    friend struct SystemCtxModelTestPeer;
    struct Entry {
        sysctx::Item item;
        std::wstring icon;         // resolved .exe/.ico, checked on the worker
        std::wstring publisher;    // CompanyName of the program/DLL
        std::wstring description;  // FileDescription (names bare handler keys)
        std::wstring product;      // ProductName, unless it is just "Windows"
    };
    struct Batch {
        std::vector<sysctx::Op> ops;
    };
    struct Change {            // 概览 · 最近的改动
        std::wstring title;    // 禁用「7-Zip」等 3 项
        std::wstring caption;  // HH:mm · backup.reg
        bool undone = false;
    };
    // A row of the scene list; extensions and perceived types pick a sub scene.
    struct Nav {
        const wchar_t* zh;
        const wchar_t* en;
        const wchar_t* glyph;
        int group;
        std::vector<int> scenes;   // indices into scene_ids_
        std::vector<std::wstring> subs;
    };

    void StartJob(ui::SysCtxView::State state, std::vector<sysctx::Op> ops, size_t pop_undo, bool record_undo, int only_scene = -1);
    void RunJob(std::vector<sysctx::Op> ops, size_t pop_undo, bool record_undo, int only_scene);
    struct ScanResult {
        std::vector<std::vector<Entry>> entries;
        std::vector<std::wstring> keys;  // per scene: where its entries live
    };
    ScanResult Scan(int only_scene = -1) const;
    // Machine ops through the elevated helper; fills `results`.
    bool RunElevated(const std::vector<sysctx::Op>& ops, std::vector<sysctx::OpResult>& results,
                     std::wstring& error) const;
    std::wstring WriteBackup(const std::vector<sysctx::Op>& ops) const;  // the .reg file name
    void AppendLog(const std::vector<sysctx::Op>& ops, const std::vector<sysctx::OpResult>& results) const;
    const Entry* RowEntryLocked(int row) const;
    int CurrentSceneLocked() const;
    // Flips `e` like the row switch (lock held): queues the change, or drops
    // it from the queue (a queued delete goes first).
    void ToggleEntryLocked(const Entry& e);
    void DeleteEntryLocked(const Entry& e);
    // The row title: what the entry adds to the menu, in words a user knows.
    std::wstring TitleLocked(const Entry& e) const;
    void ToggleMapped(const std::vector<std::pair<int, size_t>>& map, int index);
    std::wstring ChangeTitleLocked(const std::vector<sysctx::Op>& ops) const;

    static std::wstring SelectionKey(const sysctx::Item& item);
    static std::wstring OpKey(sysctx::Root root, sysctx::Kind kind, const std::wstring& key_path);
    // pending_ key: the switch and the placement of an entry queue separately.
    static std::wstring PendingKey(const sysctx::Op& op);

    std::vector<std::wstring> scene_ids_;
    std::vector<Nav> nav_;

    mutable std::mutex mutex_;
    HWND notify_ = nullptr;
    std::thread worker_;
    bool loaded_ = false;
    ui::SysCtxView::State state_ = ui::SysCtxView::State::Idle;
    std::vector<std::vector<Entry>> entries_;          // per scene id
    std::vector<std::wstring> scene_keys_;
    std::map<std::wstring, sysctx::Op> pending_;       // OpKey -> queued op
    std::vector<Batch> undo_;
    std::vector<Change> changes_;                      // applied this session, oldest first
    int custom_extension_scene_ = -1;
    int custom_extension_sub_ = -1;
    int nav_index_ = 2;                                // 文件夹: the busiest menu
    std::vector<int> sub_index_;                       // per nav row
    int filter_ = ui::kSysCtxFilterAll;
    std::wstring selected_;                            // OpKey of the selected row
    int side_tab_ = 0;
    std::wstring message_;
    bool message_error_ = false;
    std::wstring data_dir_;
    std::vector<std::pair<int, size_t>> row_map_;      // view row -> (scene, entry)
    int selected_row_ = -1;
    std::vector<std::pair<int, size_t>> preview_map_;  // preview item -> (scene, entry)
    bool overview_ = true;                             // 概览 shown; the first visit starts there
    std::vector<ui::SysCtxHandlerTiming> timings_;
    std::map<std::wstring, std::vector<std::wstring>> labels_;  // upper-case CLSID -> menu rows
    std::vector<std::wstring> pulse_hidden_;  // sorted
    std::map<std::wstring, bool> pending_pulse_;  // catalog key -> hide in Pulse (queued)
    std::vector<std::wstring> pending_skip_;      // upper-case CLSIDs: Pulse 跳过 (queued)
    std::vector<std::pair<int, size_t>> slow_map_, mark_map_;  // 概览 rows -> (scene, entry); scene -1: not scanned
    int drag_item_ = -1;
    bool drag_live_ = false;
    float drag_start_ = 0.0f, drag_y_ = 0.0f, drag_grab_ = 0.0f;
    std::shared_ptr<const ui::SysCtxView> view_;
    bool dirty_ = true;
};

} // namespace pulse::app

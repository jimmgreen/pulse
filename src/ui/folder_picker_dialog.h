#pragma once

// Pulse's own open dialog: a modal window that replaces IFileOpenDialog for
// folders, pictures and files. It hosts the main window's renderer in embedded
// mode, so the address row, sidebar and file list are the same code as the
// main UI; only the title strip and the footer are the picker's own.
// Folders are read off the UI thread.

#include "folder_picker_model.h"
#include "../common/picker_places_snapshot.h"
#include "../common/menu_icon.h"

#include <d2d1.h>
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse { class PickerOwnerGuard; }
namespace pulse::ui {

// A sidebar row: a known folder or a place the caller
// added (IFileDialog::AddPlace).
struct PickerPlace {
    std::wstring label;
    std::wstring path;
    std::wstring glyph;
};

enum class PickerDialogEvent { Snapshot, Shown, Ready, FolderChanging, FolderChanged, SelectionChanged, TypeChanged, FileOk, ButtonClicked, ItemSelected, ControlActivating, OpenDropDownSelected, CheckToggled, EditChanged, IncludeItem };
struct PickerDialogSnapshot {
    uint64_t generation = 0;
    std::wstring folder, filename, selection, candidate;
    std::vector<std::wstring> selections, candidates;
    size_t filter_index = 0;
    bool validating = false;
    uint32_t control_id = 0, item_id = 0;
    std::wstring control_text;
};
struct PickerCustomItem { uint32_t id = 0, state = 3; std::wstring label; };
// A jump panel result (HTML 输入跳转). group: 1 this program's recent, 2 favorites, 3 everywhere.
struct PickerJumpResult {
    std::wstring path;
    std::wstring name;   // empty: the leaf of path
    std::wstring note;   // quiet tag ("7 月 3 日", "网络位置")
    int group = 3;
    bool file = false;   // a file: going there opens its folder
};
// A folder this program used before (HTML 侧栏「微信 最近发送」), most used first.
struct PickerRecentPlace {
    std::wstring path;
    std::wstring label;   // empty: the folder's own name
    std::wstring detail;  // quiet tag on the right: "9 次", "今天"
};
// A recently used file of the accepted types, from any folder (open modes, HTML 打开).
struct PickerRecentFile {
    std::wstring path;
    std::wstring detail;          // "设计稿-v3\\导出 · 10 分钟前"
    HBITMAP thumbnail = nullptr;  // optional 32bpp premultiplied; borrowed until Show returns
    DWORD attributes = 0;         // Worker snapshot for the isolated thumbnail cache.
    uint64_t modified = 0, size = 0;
};
// A folder open elsewhere, for the jump strip (HTML 跳转条). The host fills these.
struct PickerOpenFolder {
    std::wstring path;
    std::wstring label;   // empty: the folder's own name
    bool explorer = false; // an Explorer window rather than a Pulse tab
};
struct PickerCustomControl {
    uint32_t kind = 0, id = 0, parent_id = 0, state = 0;
    bool has_parent = false;
    bool prominent = false;
    std::wstring label;
    std::vector<PickerCustomItem> items;
    bool has_selected = false;
    uint32_t selected_item_id = 0;
    uint32_t property_layout = 0; // Internal: 1 vector edit, 2 inline remove, 3 add, 4 scalar, 5 label, 6 readonly, 7 heading.
    bool checked = false;
    std::wstring text;
};
inline bool PickerPropertiesNeedPanel(const std::vector<PickerCustomControl>& controls) {
    for (const auto& control : controls)
        if ((control.state & 2) && (control.property_layout == 6 || control.property_layout == 7)) return true;
    return false;
}
// Internal same-STA update, never a cross-process message.
HRESULT UpdatePickerCustomControl(HWND window, const PickerCustomControl& control);
struct FilePickerResult;
struct FolderPickerSpec {
    // Optional shared-place provider for hosts with an existing catalog.
    // Reads run on the dialog thread and must be nonblocking; mutations run on a worker.
    std::function<picker_places::Snapshot()> read_shared_places;
    std::function<bool(const std::vector<picker_places::StarredItem>&, const std::wstring&)> add_shared_favorites;
    std::function<bool(const picker_places::ManagePlacesRequest&)> manage_shared_places;
    uint32_t native_options = 0; // Raw FILEOPENDIALOGOPTIONS; zero keeps standalone defaults.
    bool persist_layout = false; // In-process takeover remembers presentation per host and mode.
    PickerMode mode = PickerMode::Folder;
    std::wstring title;         // empty: "Choose a folder" / "Choose background image" / "Open"
    bool explicit_title = false, has_ok_button_label = false, has_file_name_label = false, has_cancel_button_label = false;
    std::wstring ok_button_label, file_name_label, cancel_button_label;
    std::wstring initial_path;  // empty: where the last pick of this mode was made
    std::vector<PickerFilter> filters;
    std::vector<PickerCustomControl> custom_controls;
    size_t filter_index = 0;
    std::wstring filename;
    std::wstring default_extension; // Without leading period
    std::wstring navigation_root;
    bool use_open_default_extension = false;
    bool no_test_file_create = false;
    bool require_existing_save_parent = false;
    bool native_save_defaults = false;
    bool prompt_overwrite = true;
    bool require_unencrypted_save = false;
    bool allow_multiselect = false;
    bool show_hidden = false;
    bool shell_item_filter = false; // Enumerated views only; never typed navigation/result validation.
    // The main window's list style, so rows look the same in both places.
    bool selection_outline = false;
    float row_height_dip = 0.0f;          // 0: the renderer's default
    bool list_smart_date = true;
    bool list_zebra_rows = true;
    bool list_size_bar = false;
    bool thumbnail_badges = true;
    int blank_click_action = 0;          // AppPrefs: off, back (parent fallback), or up.
    int preview_trigger = 0;
    int hover_preview_delay_ms = 500;
    int hover_card_size = 1;
    uint32_t details_columns = 0;         // Without explicit_details_columns, 0 uses defaults.
    bool explicit_details_columns = false;
    // ---- Jump strip (HTML 跳转条): all optional, filled by the host process. ----
    std::wstring host_app_name;            // the program that opened the dialog ("Microsoft Word")
    HICON host_app_icon = nullptr;         // its icon; borrowed until Show returns
    // Preferred over host_app_icon: the icon extracted at the title icon's pixel size
    // (20 DIP at the system DPI) on a worker, so nothing is stretched up.
    common::MenuIconRef host_app_icon_pixels;
    std::vector<PickerOpenFolder> open_folders; // Pulse tabs, then Explorer windows; Alt+1..9 in order
                                                // (empty: the folders the shared Pulse places mark open)
    std::vector<PickerRecentPlace> app_recent_places; // sidebar group "{host_app_name} 最近使用", first
    std::vector<PickerRecentFile> recent_files; // File / Image modes: a card row under the strip
    std::wstring recent_files_caption;          // empty: the default caption
    PickerOpenFolder last_location;        // where this program last saved / opened; path empty: none
    std::wstring last_location_source;     // "Word 上次"
    // initial_path was chosen by the host over the program's own folder (the automatic
    // jump to the folder open in Pulse); this is that original folder, offered as 回到 X.
    std::wstring auto_jump_from;
    // Typed names in the address row (not paths) open the jump panel. The panel itself
    // matches open_folders and last_location; this adds the rest. Called on the dialog
    // thread for every keystroke: answer from memory only (cache index results elsewhere).
    std::function<std::vector<PickerJumpResult>(const std::wstring& query)> jump_search;
    // Memory-only polling; producers own background I/O and retain icon resources.
    std::function<bool(FolderPickerSpec&)> poll_jump_context;
    bool allow_auto_jump = false; // Explicit SetFolder and unknown caller contracts stay put.
    int jump_index_status = 0; // 1 only after a connected per-user index is observed.
    std::wstring jump_search_query; // Completed background query; never applies to newer input.
    uint64_t jump_search_elapsed_ms = 0;
    bool jump_search_complete = false;
    std::vector<PickerPlace> bottom_places;
    std::vector<PickerPlace> places;      // extra quick-access rows, shown first
    pulse::PickerOwnerGuard* owner_guard = nullptr; // borrowed through Show; acquired once by caller or detailed entry
    // Without a guarded owner: a window (typically another process's, whose
    // own guard keeps the modality) that only owns and places the dialog: its
    // z-order, monitor/DPI and centring. Never enabled, disabled or observed.
    HWND placement_owner = nullptr;
    HANDLE cancel_event = nullptr; // borrowed until Show returns; bridge disconnect cancellation
    // File acceptance only; listing and Image format restrictions remain strict.
    bool advisory_file_filters = false;
    // Explicit Open contract only. The loader still checks existence; it does
    // not implement a read-access probe for either value of this field.
    bool skip_access_validation = false;
    bool allow_missing_open = false;
    bool require_existing_open_parent = false;
    bool validate_local_open_access_v1 = false;
    bool validate_local_save_access_v1 = false; // Worker probes may briefly create/delete a new target.
    bool no_dereference_links = false; // Treat .lnk as a file, including broken links.
    // Optional embedded-provider hooks, invoked on this window's Show thread.
    // Defaults preserve the ordinary standalone picker path.
    HINSTANCE module_instance = nullptr;
    bool pulse_painted_plain_child = false; // Strict opt-in presentation; preserves Pulse text rendering.
    // Internal C++ hook; the C ABI adapter owns copies and restores caller context.
    // Snapshot notifications update query state even before external events begin.
    std::function<HRESULT(HWND, PickerDialogEvent, const PickerDialogSnapshot&, const std::wstring&)> on_dialog_event;
    // Response: 0=Pulse prompt, 1=accept subject to revalidation, 2=refuse.
    std::function<HRESULT(HWND, const PickerDialogSnapshot&, const std::wstring&, uint32_t&)> on_overwrite;
    std::function<HRESULT(HWND, const PickerDialogSnapshot&, const std::wstring&, uint32_t&)> on_share_violation;
    std::function<void(HWND, const std::wstring&, HRESULT)> on_navigation_complete;
    std::function<HRESULT(HWND)> on_ready;
    std::function<HRESULT(HWND, const FilePickerResult&)> on_file_ok;
};

struct FilePickerResult {
    std::vector<std::wstring> paths;
    size_t filter_index = 0;
};

enum class FilePickerStatus { Accepted, Cancelled, Failed };
struct FilePickerOutcome {
    FilePickerStatus status = FilePickerStatus::Failed;
    DWORD error = ERROR_SUCCESS;
    FilePickerResult result;
};

HRESULT NavigatePickerFolder(HWND window, const std::wstring& path);
HRESULT UpdatePickerText(HWND window, uint32_t kind, const std::wstring& text);
HRESULT SetPickerPropertyValidation(HWND window, uint32_t index, bool invalid);
HRESULT SetPickerNavigationRoot(HWND window, const std::wstring& root);

// The bridge must not turn a window/message-loop failure into user cancellation.
FilePickerOutcome ShowFilePickerDetailed(HWND owner, const FolderPickerSpec& spec, bool dark,
                                         D2D1_COLOR_F accent);

bool ShowFilePicker(HWND owner, const FolderPickerSpec& spec, bool dark,
                    D2D1_COLOR_F accent, FilePickerResult& result);

// Returns true and sets `path` when the user picked something.
bool ShowFolderPicker(HWND owner, const FolderPickerSpec& spec, bool dark,
                      D2D1_COLOR_F accent, std::wstring& path);

} // namespace pulse::ui

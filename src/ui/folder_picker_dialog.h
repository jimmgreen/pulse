#pragma once

// Pulse's own open dialog: a modal window that replaces IFileOpenDialog for
// folders, pictures and files. It hosts the main window's renderer in embedded
// mode, so the address row, sidebar and file list are the same code as the
// main UI; only the title strip and the footer are the picker's own.
// Folders are read off the UI thread.

#include "folder_picker_model.h"
#include "../common/picker_places_snapshot.h"

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

enum class PickerDialogEvent { Snapshot, Shown, Ready, FolderChanging, FolderChanged, SelectionChanged, TypeChanged, FileOk, ButtonClicked, ItemSelected, ControlActivating, OpenDropDownSelected, CheckToggled, EditChanged };
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
struct PickerCustomControl {
    uint32_t kind = 0, id = 0, parent_id = 0, state = 0;
    bool has_parent = false;
    bool prominent = false;
    std::wstring label;
    std::vector<PickerCustomItem> items;
    bool has_selected = false;
    uint32_t selected_item_id = 0;
    uint32_t property_layout = 0; // Internal: 1 vector edit, 2 inline remove, 3 add, 4 scalar, 5 label.
    bool checked = false;
    std::wstring text;
};
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
    bool explicit_title = false, has_ok_button_label = false, has_file_name_label = false;
    std::wstring ok_button_label, file_name_label;
    std::wstring initial_path;  // empty: where the last pick of this mode was made
    std::vector<PickerFilter> filters;
    std::vector<PickerCustomControl> custom_controls;
    size_t filter_index = 0;
    std::wstring filename;
    std::wstring default_extension; // Without leading period
    bool use_open_default_extension = false;
    bool no_test_file_create = false;
    bool require_existing_save_parent = false;
    bool native_save_defaults = false;
    bool require_unencrypted_save = false;
    bool allow_multiselect = false;
    bool show_hidden = false;
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

// The bridge must not turn a window/message-loop failure into user cancellation.
FilePickerOutcome ShowFilePickerDetailed(HWND owner, const FolderPickerSpec& spec, bool dark,
                                         D2D1_COLOR_F accent);

bool ShowFilePicker(HWND owner, const FolderPickerSpec& spec, bool dark,
                    D2D1_COLOR_F accent, FilePickerResult& result);

// Returns true and sets `path` when the user picked something.
bool ShowFolderPicker(HWND owner, const FolderPickerSpec& spec, bool dark,
                      D2D1_COLOR_F accent, std::wstring& path);

} // namespace pulse::ui

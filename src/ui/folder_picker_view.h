#pragma once

// The picker's own chrome: the title strip above the address row and the
// footer below the panes. The address row, sidebar and file list are drawn by
// the main window's MainRenderer in embedded mode (see folder_picker_dialog),
// so only these two bands live here, separate from the window so tests can
// lay them out offscreen.

#include "FluentTokens.h"
#include "fluent_components.h"
#include "folder_picker_model.h"
#include "ui_compositor.h"

#include <string>
#include <functional>
#include <vector>

namespace pulse::ui {

enum PickerControl : int {
    kPickNone = 0,
    kPickOverwriteBack,
    kPickRename,
    kPickClose,
    kPickCancel,
    kPickPrimary,
    kPickPrimaryDropDown,
    kPickFilename,
    kPickFilter,
    kPickList,      // keyboard focus on the shared file list
    kPickAddress,   // keyboard focus on the address row (hosted EDIT)
    kPickSearch,    // keyboard focus on the search field (hosted EDIT)
    kPickUndoJump,  // the strip's "回到 X": undo the automatic jump
    kPickNewFolder, // the strip's labelled 新建文件夹 (same as Ctrl+Shift+N / F7)
    kPickMoreOptions,
    kPickJumpIndexInfo,
    kPickOverwriteReplace,
    kPickLastLocation, // the strip's "this program last time" chip
};

inline constexpr int kPickRemoveSelectionBase = 100;
inline constexpr int kPickOpenFolderBase = 200;  // + index into FolderPickerChrome::open_folders
inline constexpr int kPickerJumpIndexInfoHit = -3;
inline constexpr int kPickJumpPanel = 299;       // inside the jump panel, not on a row
inline constexpr int kPickJumpRowBase = 300;     // + jump panel row
inline constexpr int kPickRecentBase = 400;      // + recent file card (open modes)
inline constexpr int kPickContextFilterBase = 480;

inline constexpr float kPickerTitleDip = 40.0f;
// The strip of folders already open elsewhere, under the address row (HTML 已打开).
inline constexpr float kPickerStripDip = 36.0f;
// Open modes: recent files across folders, under the strip (HTML 打开 · 最近的图片).
inline constexpr float kPickerRecentDip = 172.0f;
inline constexpr float kPickerAutoJumpNoticeDip = 32.0f;
struct PickerRecentCard {
    std::wstring name;            // file name
    std::wstring detail;          // "设计稿-v3\导出 · 10 分钟前"
    ID2D1Bitmap* thumbnail = nullptr; // borrowed for the frame; null draws a type card
};

// A folder open elsewhere: a Pulse tab or an Explorer window (Alt+1..9 in order).
struct PickerOpenChip {
    std::wstring label;   // folder name
    std::wstring source;  // quiet suffix: empty for Pulse tabs, "资源管理器", "Word 上次"
    std::wstring path;    // full path, the tooltip and the navigation target
    bool active = false;  // the dialog is showing this folder
    bool window = false;  // an Explorer window (window mark) rather than a Pulse tab
};

// DIP height of the footer under the panes: the file name / type row, then one
// row with the target on the left and the buttons on the right (HTML 底部); an
// overwrite question adds its banner row between them.
float PickerFooterDip(PickerMode mode, bool overwrite_pending = false, bool has_options = false);
struct PickerOptionChip {
    int id = 0;
    std::wstring text;
    bool checkable = false;
    bool checked = false;
    bool enabled = true;
    bool drop_down = false;
    std::wstring label;
    bool property = false;  // a file property: joined label | value field (HTML 作者 | SS)
    bool add = false;       // the property has no value yet: text is "添加X", drawn in the accent
};

// Everything the chrome draws.
struct FolderPickerChrome {
    PickerMode mode = PickerMode::Folder;
    std::wstring title;
    // The program that opened the dialog, drawn left of `title` (HTML 标题栏).
    // Empty name: the title alone, as before.
    std::wstring app_name;
    ID2D1Bitmap* app_icon = nullptr;  // borrowed for the frame; null draws a neutral mark
    // The strip under the address row; shown when `strip_top` > 0.
    float strip_top = 0.0f;           // DIP from the window top
    float strip_left = 0.0f;          // DIP: the sidebar's right edge (the strip sits over the pane column)
    float strip_right = 0.0f;         // Optional right edge before an expanded property panel.
    float recent_left = 0.0f;         // Recent cards belong to the pane, not the full-width jump strip.
    float recent_top = 0.0f;
    float context_top = 0.0f, context_left = 0.0f, context_right = 0.0f;
    std::vector<PickerOptionChip> context_filters;
    std::vector<PickerOpenChip> open_folders;
    PickerOpenChip last_location;     // label empty: none
    std::wstring strip_caption;       // "已打开"
    std::wstring auto_jump_notice;
    std::wstring selection_summary, selection_hint;
    std::wstring undo_jump_text;      // "回到 文档"; empty: no automatic jump to undo
    std::wstring new_folder_text;     // "新建文件夹"; empty: no button
    std::wstring new_folder_key;      // "Ctrl+Shift+N"
    std::function<bool(size_t, const D2D1_RECT_F&)> draw_recent_thumbnail; // Host thumbnail drawing/async generation; no synchronous IO.
    std::vector<PickerRecentCard> recent_files;  // shown when non-empty (open modes)
    std::wstring recent_caption;      // "最近的图片 · 来自 Photoshop 和 Pulse，不限文件夹"
    int recent_selected = -1;
    // The summary in parts, when it names a target folder: lead, folder, leaf.
    std::wstring summary_lead, summary_folder, summary_leaf;
    std::wstring filename_label;   // "文件名(N):"
    std::wstring filter_text;      // current file type label
    std::wstring primary_text;
    std::wstring cancel_text;
    std::wstring summary;          // what the primary button would pick, or a hint
    std::wstring notice;           // validation message; drawn instead of the summary
    bool overwrite_pending = false;
    std::wstring overwrite_replace_text;
    std::wstring overwrite_back_text;
    std::wstring rename_text;
    bool primary_enabled = false;
    bool primary_drop_down = false;
    bool filename_focused = false;
    float filter_turn = 0.0f;
    bool hosted_edit = true;       // the file name text is a hosted EDIT, not drawn here
    std::vector<PickerOptionChip> options;
    PickerOptionChip footer_tool;
    std::wstring more_options_text;
    std::vector<std::wstring> selected_names;
    std::wstring filename_text;    // drawn only without a hosted EDIT (gallery, tests)
    int hover = kPickNone;
    int pressed = kPickNone;
    int focus = kPickNone;
    bool show_focus = false;
};

struct FolderPickerChromeLayout {
    float scale = 1.0f;
    float width = 0.0f;
    float height = 0.0f;
    D2D1_RECT_F title{};
    D2D1_RECT_F close{};
    D2D1_RECT_F strip{};
    std::vector<D2D1_RECT_F> open_chips;   // one per shown open_folders entry
    std::vector<D2D1_RECT_F> open_keycaps;
    std::vector<bool> open_chip_source;     // the chip had room for its source label
    D2D1_RECT_F last_location{};
    D2D1_RECT_F undo_jump{};
    D2D1_RECT_F auto_jump_notice{};
    D2D1_RECT_F new_folder{};
    bool new_folder_keycap = false;          // room for the Ctrl+Shift+N keycap
    std::vector<D2D1_RECT_F> context_filters;
    D2D1_RECT_F recent{};                   // the recent files band
    D2D1_RECT_F recent_caption{};
    std::vector<D2D1_RECT_F> recent_cards;
    std::vector<D2D1_RECT_F> recent_thumbnails, recent_names, recent_details;
    D2D1_RECT_F conflict{};                 // the overwrite banner
    D2D1_RECT_F conflict_text{};            // its message
    D2D1_RECT_F footer{};
    D2D1_RECT_F filename_label{};
    D2D1_RECT_F filename{};
    std::vector<D2D1_RECT_F> selection_tags;
    D2D1_RECT_F options{};
    std::vector<D2D1_RECT_F> option_chips;
    std::vector<D2D1_RECT_F> option_labels, option_buttons;
    D2D1_RECT_F more_options{};
    D2D1_RECT_F more_options_label{}, more_options_badge{};
    D2D1_RECT_F filter{};
    D2D1_RECT_F summary{};
    D2D1_RECT_F footer_tool{};
    D2D1_RECT_F overwrite_replace{};
    D2D1_RECT_F overwrite_back{};
    D2D1_RECT_F rename{};
    D2D1_RECT_F cancel{};
    D2D1_RECT_F primary{};
};

// The jump panel under the address row (HTML 输入跳转): typed names find folders.
enum PickerJumpGroup : int { kJumpOpen = 0, kJumpRecent, kJumpFavorite, kJumpEverywhere, kJumpCreate, kJumpGroupCount };
struct PickerJumpRow {
    int group = kJumpOpen;
    std::wstring name;     // shown name; the query is marked in it
    std::wstring path;     // second line and the target
    std::wstring note;     // quiet tag on the right ("Pulse 标签", "7 月 3 日")
    std::wstring key;      // keycap on the right ("Alt+1")
    bool window = false;   // an Explorer window
    bool file = false;     // a file: going there opens its folder with it selected
};
struct PickerJumpPanel {
    bool open = false;
    std::wstring query;
    std::vector<PickerJumpRow> rows;      // grouped: rows of one group are adjacent
    int selected = 0;
    int hover = -1;
    std::wstring group_details[kJumpGroupCount]; // Known formatted statistics only; empty hides.
    std::wstring index_notice, index_action_text;
    std::wstring titles[kJumpGroupCount]; // group captions
    std::wstring empty_text;              // no rows besides the create row
    std::wstring hints[5];                // 选择 / 进入 / 补全路径 / 直接保存到这里 / 返回
    std::wstring foot_note;               // "支持拼音首字母 · 路径片段"
};
struct PickerJumpLayout {
    D2D1_RECT_F panel{};
    D2D1_RECT_F index_notice{}, index_action{};
    std::vector<D2D1_RECT_F> rows; // Model indexed; empty rectangles are scrolled out.
    std::vector<std::pair<int, D2D1_RECT_F>> headers;  // group, caption rect
    D2D1_RECT_F empty{};
    D2D1_RECT_F foot{};
};
// `anchor`: the address row; the panel hangs under it, at most down to `bottom`.
PickerJumpLayout LayoutPickerJump(const PickerJumpPanel& panel, D2D1_RECT_F anchor, float width, float bottom,
                                  float scale);
int HitTestPickerJump(const PickerJumpLayout& layout, float x, float y);  // row index, -1 inside, -2 outside
void DrawPickerJump(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                    const PickerJumpPanel& panel, const PickerJumpLayout& layout, bool high_contrast);

FolderPickerChromeLayout LayoutPickerChrome(float width, float height,
                                            const FolderPickerChrome& chrome,
                                            const fluent::Painter& painter, float scale);
int HitTestPickerChrome(const FolderPickerChromeLayout& layout,
                        const FolderPickerChrome& chrome, float x, float y);
void DrawPickerChrome(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                      const FolderPickerChrome& chrome, const FolderPickerChromeLayout& layout,
                      bool high_contrast);

} // namespace pulse::ui

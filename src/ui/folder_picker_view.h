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
};

inline constexpr int kPickRemoveSelectionBase = 100;

inline constexpr float kPickerTitleDip = 40.0f;

// DIP height of the footer under the panes: one row of buttons for folders,
// plus the file name / file type row for files and pictures.
float PickerFooterDip(PickerMode mode);

// Everything the chrome draws.
struct FolderPickerChrome {
    PickerMode mode = PickerMode::Folder;
    std::wstring title;
    std::wstring filename_label;   // "文件名(N):"
    std::wstring filter_text;      // current file type label
    std::wstring primary_text;
    std::wstring cancel_text;
    std::wstring summary;          // what the primary button would pick, or a hint
    std::wstring notice;           // validation message; drawn instead of the summary
    bool overwrite_pending = false;
    std::wstring overwrite_back_text;
    std::wstring rename_text;
    bool primary_enabled = false;
    bool primary_drop_down = false;
    bool filename_focused = false;
    float filter_turn = 0.0f;
    bool hosted_edit = true;       // the file name text is a hosted EDIT, not drawn here
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
    D2D1_RECT_F footer{};
    D2D1_RECT_F filename_label{};
    D2D1_RECT_F filename{};
    std::vector<D2D1_RECT_F> selection_tags;
    D2D1_RECT_F filter{};
    D2D1_RECT_F summary{};
    D2D1_RECT_F overwrite_back{};
    D2D1_RECT_F rename{};
    D2D1_RECT_F cancel{};
    D2D1_RECT_F primary{};
};

FolderPickerChromeLayout LayoutPickerChrome(float width, float height,
                                            const FolderPickerChrome& chrome,
                                            const fluent::Painter& painter, float scale);
int HitTestPickerChrome(const FolderPickerChromeLayout& layout,
                        const FolderPickerChrome& chrome, float x, float y);
void DrawPickerChrome(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                      const FolderPickerChrome& chrome, const FolderPickerChromeLayout& layout,
                      bool high_contrast);

} // namespace pulse::ui

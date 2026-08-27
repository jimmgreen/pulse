// batch_rename_dialog.h — Modal preview UI for batch rename.
#pragma once
#include "../app/batch_rename.h"
#include <windows.h>
#include <d2d1.h>
#include <string>
#include <vector>

namespace pulse::ui {

struct BatchRenameDialogResult {
    bool accepted = false;
    app::BatchRenameRule rule;
    std::vector<app::BatchRenameItem> items;
};

BatchRenameDialogResult ShowBatchRenameDialog(HWND owner,
                                              const std::vector<std::wstring>& paths,
                                              bool dark,
                                              D2D1_COLOR_F accent);

} // namespace pulse::ui

#pragma once
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

// The rows of the dialog sidebar's 「{程序} 常用」 group: what is pinned to this
// program first, then the folders it uses most, capped. A program without any
// history of its own borrows the folders recently visited in Pulse. Pure: no
// file system access, so the dialog thread can call it on every layout.
struct PickerCommonFolder {
    std::wstring path, label;   // label empty: the folder name
};
struct PickerCommonInput {
    std::vector<std::wstring> pinned;              // pinned to this program, in its order
    std::vector<PickerCommonFolder> history;       // this program's folders, most used first
    std::vector<PickerCommonFolder> pulse_recent;  // Pulse's recent folders (used only without the two above)
    std::wstring last_location;                    // where this program last opened / saved
    std::vector<std::wstring> favorites;           // Pulse favorites: marked on the rows
    std::vector<std::wstring> forgotten;           // removed in this dialog (pending), left out
    size_t limit = 6;
};
struct PickerCommonRow {
    std::wstring path, label;
    std::wstring hint;   // a level of location, only when another row has the same name
    bool pinned = false, last = false, from_pulse = false, favorite = false;
};
using PickerPathTextFn = std::function<std::wstring(const std::wstring&)>;

// `leaf`: a folder's display name. `parent_label`: the name shown for a parent
// folder in a same-name hint (empty function: `leaf`).
std::vector<PickerCommonRow> BuildPickerCommonRows(const PickerCommonInput& input, const PickerPathTextFn& leaf,
                                                   const PickerPathTextFn& parent_label = {});

// Fills `hint` on rows whose names collide (case-insensitive): the parent's
// name, or the parent's full path when the parents share a name as well.
void AddPickerSameNameHints(std::vector<PickerCommonRow>& rows, const PickerPathTextFn& parent_label);

// "C:\a\b" -> "C:\a", "C:\a" -> "C:\", "\\srv\share\a" -> "\\srv\share"; empty at a root.
std::wstring PickerParentPath(const std::wstring& path);

} // namespace pulse::ui

// action_files.h - File operations as run_actions steps (全面打通 P4c).
//
// A file step never runs at once: the assistant shows the user what it would
// do (the first batch's plan card for moves, copies, renames; the second
// batch's Recycle Bin card; a red card that needs two clicks for a permanent
// delete) and only then queues the same requests the menus queue. Here is
// the plan behind those cards and the requests; the card's words are the
// assistant's. Nothing here asks or waits for the user.
#pragma once

#include "../../common/json_value.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse {
struct AppState;
}

namespace pulse::app::actions {

enum class FileOp : uint8_t { None, Copy, Move, Rename, Group, Dissolve, Recycle, Purge, Restore, EmptyBin, Organize };

// file.copy_to / move_to / paste / rename / group_into_folder / dissolve /
// recycle / delete_permanent / organize, recycle.restore / empty; None for
// the rest.
FileOp FileOpOf(std::string_view id) noexcept;
inline bool IsFileOp(std::string_view id) noexcept { return FileOpOf(id) != FileOp::None; }

// Purge and EmptyBin cannot be undone, nor can a restore (the ops undo stack
// keeps no restore).
bool FileOpUndoes(FileOp op) noexcept;

struct FileRow {
    std::wstring name;    // the item's name
    std::wstring to;      // Copy / Move / Group: the folder; Rename: the new name; Restore: where it goes back
    uint64_t bytes = 0;   // a file's size (folders 0)
    bool folder = false;
    bool skip = false;    // Restore: its place is taken, so it stays in the bin
};

// Organize: one folder the plan fills (the dialog lists these, in order).
struct OrganizeDest {
    std::wstring path;
    bool make = false;                  // not there yet: Pulse makes it first
    std::vector<std::wstring> sources;  // full paths, in the order given
};

struct FilePlan {
    FileOp op = FileOp::None;
    bool ok = false;
    bool skip = false;                  // nothing to do (the bin is empty, the name is the same)
    std::string error;                  // for the model (English)
    std::wstring why;                   // for the user
    bool paste = false;                 // file.paste: Copy or Move of the clipboard's files
    std::vector<std::wstring> sources;  // full paths (Restore: the bin's payloads)
    std::wstring dest;                  // Copy / Move / Group: the folder items go to; Dissolve: the parent
    bool dest_exists = true;            // Copy / Move: false = Pulse makes it first
    std::wstring new_name;              // Rename: the new name; Group: the new folder's
    std::vector<FileRow> rows;          // every item, in order
    size_t count = 0;                   // items (EmptyBin: in the bin)
    uint64_t bytes = 0;                 // bytes of the files (EmptyBin: the bin's)
    bool files_only = true;             // no folder among them (bytes is then the total)
    size_t clashes = 0;                 // Restore: its place is taken (left out); Copy / Move: the name is taken (kept both)
    size_t inner = 0;                   // Dissolve: items that move up a level
    uint64_t bin_max = 0, bin_used = 0; // Recycle: the drive's bin
    // Organize (file.organize moves[{item, dest}]): the folders in order, and
    // the ones Pulse makes first, parents before children. `dest` is the
    // folder the items are in when they share one.
    std::vector<OrganizeDest> dests;
    std::vector<std::wstring> make_folders;
    bool one_source_folder = true;
};

// dest arguments: "D:\财务\2026", "Documents\发票" (the assistant resolves
// known folders the way run_task does); an empty result is not a folder.
using DestResolver = std::wstring (*)(std::wstring);

// What `id` would do with `args` now. The items are args.items (absolute
// paths) or the focused pane's selection; never anything inside Windows,
// Program Files or ProgramData, and never a drive itself. A plan that is not
// ok says why in the user's words and the model's.
FilePlan PlanFileOp(AppState& s, std::string_view id, const json::Value& args, DestResolver resolve);

// Queues the plan's requests (one undo group, like 合并到文件夹); returns
// the tasks, the last one last.
std::vector<uint64_t> SubmitFilePlan(AppState& s, const FilePlan& plan, uint64_t undo_group,
                                     const std::wstring& undo_label);

} // namespace pulse::app::actions

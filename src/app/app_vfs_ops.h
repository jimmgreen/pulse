// app_vfs_ops.h - files inside an archive opened as a folder (pulse:vfs: tabs).
//
// Taking items out (解压所选到…, Ctrl+C, drag-out) extracts them into a staging
// folder under %TEMP%\Pulse\vfs_cache first; the normal transfer engine then
// moves them to the destination (progress, conflict dialog). Changing the archive
// (drop / paste in, delete, rename, an opened file saved again) runs one provider
// modify per action as a Pulse task; the provider keeps one backup, so the
// "updated" snack offers 撤销 once. Archives the provider cannot rewrite
// (no PULSE_VFS_CAP_MODIFY) answer with a read-only notice.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace pulse {
struct AppState;
namespace app { struct Tab; }

// The tab shows a folder inside an archive (or any other VFS mount).
bool IsVfsFolder(const std::wstring& path);
// The mount holding `vfs_path` can change its contents.
bool VfsWritable(const std::wstring& vfs_path);
void ShowVfsReadOnlyNotice(AppState& s);

void VfsExtractSelectionTo(AppState& s);  // context menu: pick a folder, extract, move there
void VfsCopySelection(AppState& s);       // Ctrl+C: extract to a staging folder, CF_HDROP
// Drag-out: extracts the selection now (the drag waits; releasing the button or
// Esc cancels). Returns the staged paths, empty when nothing can be dragged.
std::vector<std::wstring> VfsStageSelectionForDrag(AppState& s, const pulse::app::Tab& tab);

void VfsPasteInto(AppState& s, const std::wstring& vfs_dir);
bool VfsDropInto(AppState& s, const std::vector<std::wstring>& sources, const std::wstring& vfs_dir);
void VfsDeleteSelection(AppState& s);
void VfsRenameItem(AppState& s, const std::wstring& vfs_path, const std::wstring& new_name);
// After an archive member was opened from vfs_cache: saving it writes it back.
// Thread-safe (called from the worker that extracted it).
void VfsWatchOpenedFile(HWND owner, const std::wstring& vfs_path, const std::wstring& local_path);

// WM_VFS_OP_DONE (lParam owns the payload) and WM_VFS_UNDO (the snack's 撤销).
void HandleVfsOpDone(AppState& s, WPARAM kind, LPARAM payload);
void HandleVfsUndo(AppState& s);

// Quick Look / the details pane: dragging an archive member out extracts it,
// dropping files on the archive preview adds them (through pulse.7zip, opened
// quietly). Installed once at startup.
void InstallPreviewArchiveBridge();

}  // namespace pulse
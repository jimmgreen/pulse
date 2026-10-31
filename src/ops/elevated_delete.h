#pragma once
#include "elevated_transfer.h"
#include <shobjidl.h>
#include <atomic>
#include <cstdint>
#include <memory>
namespace pulse::ops {
// STA worker only. Recycle uses ITransferSource::RecycleItem, never DeleteItem.
// Permanent deletion uses IFileOperation. Neither requests elevation or Shell UI.
// Only local fixed volumes have a Recycle Bin: on a share (\\server\share, a
// mapped drive) or a removable drive a delete is permanent, which Explorer
// confirms first. `volume_error`: the volume lookup failed (the path is not
// judged; the delete itself reports why).
bool RecycleBinAvailable(const std::wstring& path, DWORD* volume_error = nullptr);

// What the Recycle Bin of a path's volume will take. The Shell does not
// refuse what the bin cannot keep, it deletes it for good: a bin turned off
// ("Don't move files to the Recycle Bin", or by policy) or an item larger
// than the bin's maximum size. Both are checked before anything is touched.
struct RecycleBinLimits {
    bool disabled = false;   // NukeOnDelete or the NoRecycleFiles policy
    uint64_t max_bytes = 0;  // the bin's maximum size; 0 = not known
    uint64_t used_bytes = 0; // the volume's used space (nothing larger can exist)
};
RecycleBinLimits ReadRecycleBinLimits(const std::wstring& path, DWORD* volume_error = nullptr);
// Advisory snapshot only; execution must still check current recycle policy.
struct RecyclePolicy {
    bool available = false;
    DWORD volume_error = 0;
    RecycleBinLimits limits;
};
RecyclePolicy QueryRecyclePolicy(const std::wstring& path);

// A recycle the bin would turn into a permanent delete: the item is larger
// than the bin. Nothing was deleted.
constexpr HRESULT kRecycleBinTooSmall = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x7A10);
// S_OK when `path` can go to the bin; COPYENGINE_E_RECYCLE_BIN_NOT_FOUND when
// the bin is off; kRecycleBinTooSmall when it is larger than the bin. Folders
// are measured (links not entered), stopping once past the limit. `measure_folders`
// false: only files are judged (advisory preflight).
HRESULT CheckRecycleFits(const std::wstring& path, const RecycleBinLimits& limits, std::atomic<bool>& cancel,
                         bool measure_folders = true);
// Shell objects one recycle task reuses from item to item: the Recycle Bin,
// the transfer source of the folder last recycled from and that folder's
// volume check. Same STA thread as the deletes; release before CoUninitialize.
class RecycleCache {
public:
    RecycleCache();
    ~RecycleCache();
    RecycleCache(const RecycleCache&) = delete;
    RecycleCache& operator=(const RecycleCache&) = delete;
    struct State;
    State& state() { return *state_; }
private:
    std::unique_ptr<State> state_;
};
// The Shell item of a file-system path. With `folder` (the Shell item of the
// path's parent folder) a plain name is created from it, which skips parsing
// the whole path; anything else, or a failure, parses the full path.
HRESULT CreateItemForPath(const std::wstring& path, IShellItem* folder, IShellItem** item);
ShellTransferResult DeleteWithCurrentToken(const std::vector<std::wstring>& sources,
    bool permanent, HWND owner, std::atomic<bool>& cancel, ShellTransferProgress progress = {},
    RecycleCache* cache = nullptr);
}

// app_vfs_ops.cpp - see app_vfs_ops.h.
#include "app_vfs_ops.h"
#include "app_internal.h"
#include "app_runtime.h"
#include "app_messages.h"
#include "app_navigation.h"
#include "app_ops_ui.h"
#include "app_commands.h"
#include "extensions/vfs_path.h"
#include "extensions/vfs_registry.h"
#include "extensions/native/command_contributions.h"
#include "../ui/quick_preview_window.h"
#include "../ops/clipboard.h"
#include "../ui/confirm_dialog.h"
#include "../common/localization.h"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

namespace pulse {
namespace {

namespace vfs = pulse::extensions::vfs;
using I = l10n::StringId;

constexpr uint64_t kVfsDragLimitBytes = 512ull << 20;  // larger selections: 解压所选到…

std::wstring VfsFill(std::wstring text, const wchar_t* key, const std::wstring& value) {
    const std::wstring k(key);
    for (size_t at = text.find(k); at != std::wstring::npos; at = text.find(k, at + value.size()))
        text.replace(at, k.size(), value);
    return text;
}

std::wstring VfsLeaf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"/\\");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// The archive's own name: what the mount shows for its root.
std::wstring VfsArchiveName(const std::wstring& vfs_path) {
    const auto parsed = vfs::ParseVfsPath(vfs_path);
    if (!parsed) return VfsLeaf(vfs_path);
    for (const vfs::VfsMountInfo& m : vfs::VfsRegistry::Instance().ListMounts())
        if (m.package_id == parsed->package_id && m.mount_id == parsed->mount_id && !m.display_name.empty())
            return m.display_name;
    return VfsLeaf(vfs_path);
}

std::wstring VfsMountRoot(const std::wstring& vfs_path) {
    const auto parsed = vfs::ParseVfsPath(vfs_path);
    return parsed ? vfs::FormatVfsPath(parsed->package_id, parsed->mount_id) : std::wstring();
}

std::wstring VfsParentOf(const std::wstring& vfs_path) {
    auto parent = vfs::ParentVfsPath(vfs_path);
    return parent ? *parent : VfsMountRoot(vfs_path);
}

// %TEMP%\Pulse\vfs_cache\<pid>_x<seq>: where items wait on their way out.
std::wstring VfsStagingDir() {
    static std::atomic<uint64_t> seq{1};
    wchar_t temp[MAX_PATH + 2]{};
    const DWORD n = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    if (n == 0 || n >= std::size(temp)) return {};
    std::filesystem::path dir(std::wstring_view(temp, n));
    dir /= L"Pulse";
    dir /= L"vfs_cache";
    dir /= std::to_wstring(GetCurrentProcessId()) + L"_x" + std::to_wstring(seq.fetch_add(1));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return ec ? std::wstring() : dir.wstring();
}

std::vector<std::wstring> VfsStagedChildren(const std::wstring& dir) {
    std::vector<std::wstring> out;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(dir, ec)) out.push_back(item.path().wstring());
    return out;
}

// A provider without extract_items (an older plugin): file by file, folders
// walked through their listings. Slower, same result.
PulseStatus VfsExtractOneByOne(const std::wstring& item, const std::filesystem::path& dest, bool is_dir,
                               vfs::VfsOperationHooks& hooks, vfs::VfsProgressInfo& done, int depth = 0) {
    auto& registry = vfs::VfsRegistry::Instance();
    const auto cancelled = [&hooks] { return hooks.is_cancelled && hooks.is_cancelled(); };
    if (cancelled()) return PULSE_ERROR_STATE;
    if (!is_dir) {
        done.current = dest.filename().wstring();
        if (hooks.progress) hooks.progress(done);
        const PulseStatus st = registry.ExtractFile(item, dest.wstring(), cancelled);
        ++done.completed_items;
        return st;
    }
    if (depth > 64) return PULSE_ERROR_ARGUMENT;
    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    if (ec) return PULSE_ERROR_STATE;
    std::vector<vfs::VfsEntryData> children;
    PulseStatus st = registry.ListDirectory(item, children, cancelled);
    if (st != PULSE_OK) return st;
    const auto dir = vfs::ParseVfsPath(item);
    if (!dir) return PULSE_ERROR_ARGUMENT;
    for (const vfs::VfsEntryData& child : children) {
        const std::wstring child_path = vfs::JoinVfsChild(*dir, child.name);
        if (child_path.empty()) continue;
        st = VfsExtractOneByOne(child_path, dest / child.name, (child.flags & PULSE_VFS_ENTRY_DIRECTORY) != 0, hooks,
                                done, depth + 1);
        if (st != PULSE_OK) return st;
    }
    return PULSE_OK;
}

// ExtractItems, or the file-by-file way when the provider has no extract_items.
PulseStatus VfsExtract(const std::vector<std::wstring>& paths, const std::wstring& dest_dir,
                       vfs::VfsOperationHooks& hooks) {
    auto& registry = vfs::VfsRegistry::Instance();
    if (paths.empty()) return PULSE_ERROR_ARGUMENT;
    if (registry.CapabilitiesOf(paths.front()) & PULSE_VFS_CAP_EXTRACT_ITEMS)
        return registry.ExtractItems(paths, dest_dir, hooks);
    vfs::VfsProgressInfo done;
    done.total_items = paths.size();
    for (const std::wstring& item : paths) {
        vfs::VfsEntryData info;
        const PulseStatus st = registry.Stat(item, info, hooks.is_cancelled);
        if (st != PULSE_OK) return st;
        const PulseStatus out = VfsExtractOneByOne(item, std::filesystem::path(dest_dir) / VfsLeaf(item),
                                                   (info.flags & PULSE_VFS_ENTRY_DIRECTORY) != 0, hooks, done);
        if (out != PULSE_OK) return out;
    }
    return PULSE_OK;
}

enum class VfsOpKind { ExtractTo, Copy, Modify, WriteBack, Undo };

struct VfsOpResult {
    VfsOpKind kind = VfsOpKind::Modify;
    PulseStatus status = PULSE_OK;
    bool cancelled = false;
    std::wstring message;
    std::wstring archive_name;
    std::wstring mount_root;
    std::wstring refresh_dir;   // the archive folder to list again
    std::wstring staging_dir;   // ExtractTo / Copy
    std::wstring dest_dir;      // ExtractTo
    std::wstring file_name;     // WriteBack
};

void VfsPost(HWND owner, std::unique_ptr<VfsOpResult> result) {
    if (owner && IsWindow(owner) &&
        PostMessageW(owner, WM_VFS_OP_DONE, 0, reinterpret_cast<LPARAM>(result.get())))
        result.release();
}

ops::OpStatus VfsTaskStatus(const vfs::VfsProgressInfo& p) {
    ops::OpStatus st;
    st.completed_items = p.completed_items;
    st.total_items = p.total_items;
    st.transferred_bytes = p.completed_bytes;
    st.total_bytes = p.total_bytes;
    st.current_item = p.current;
    if (p.total_bytes) st.percent = 100.0f * static_cast<float>(p.completed_bytes) / static_cast<float>(p.total_bytes);
    else if (p.total_items) st.percent = 100.0f * static_cast<float>(p.completed_items) / static_cast<float>(p.total_items);
    return st;
}

vfs::VfsOperationHooks VfsTaskHooks(ops::OpsManager* ops, uint64_t task) {
    vfs::VfsOperationHooks hooks;
    hooks.is_cancelled = [ops, task] { return ops->ExternalTaskCancelled(task); };
    hooks.progress = [ops, task](const vfs::VfsProgressInfo& p) { ops->UpdateExternalTask(task, VfsTaskStatus(p)); };
    return hooks;
}

void VfsFinishTask(ops::OpsManager* ops, uint64_t task, PulseStatus status, bool cancelled, const std::wstring& message) {
    ops::OpStatus st;
    st.last_error = message;
    ops->FinishExternalTask(task,
                            cancelled ? ops::ExternalOutcome::Cancelled
                            : status == PULSE_OK ? ops::ExternalOutcome::Completed
                                                 : ops::ExternalOutcome::Failed,
                            st);
}

// The one archive change the 撤销 of the last "updated" snack takes back.
struct VfsUndoSlot {
    std::mutex mu;
    std::wstring mount_root, archive_name, refresh_dir;
};
VfsUndoSlot& VfsUndo() {
    static VfsUndoSlot slot;
    return slot;
}

// Changes of one archive run one after another (the provider serializes them too).
void VfsRunModify(AppState& s, std::vector<vfs::VfsChangeRequest> changes, VfsOpKind kind,
                  std::wstring refresh_dir, std::wstring file_name = {}) {
    if (changes.empty()) return;
    auto result = std::make_unique<VfsOpResult>();
    result->kind = kind;
    result->mount_root = VfsMountRoot(changes.front().vfs_path);
    result->archive_name = VfsArchiveName(changes.front().vfs_path);
    result->refresh_dir = std::move(refresh_dir);
    result->file_name = std::move(file_name);
    ops::OpsManager* ops = &s.ops;
    const uint64_t task = ops->CreateExternalTask(VfsFill(l10n::Get(I::VfsUpdateTaskTitle), L"{name}", result->archive_name),
                                                  l10n::Get(I::VfsTaskKind));
    const HWND owner = s.hwnd;
    std::thread([owner, ops, task, changes = std::move(changes), result = std::move(result)]() mutable {
        vfs::VfsOperationHooks hooks = VfsTaskHooks(ops, task);
        result->status = vfs::VfsRegistry::Instance().Modify(changes, hooks);
        result->cancelled = result->status != PULSE_OK && ops->ExternalTaskCancelled(task);
        result->message = hooks.message;
        VfsFinishTask(ops, task, result->status, result->cancelled, result->message);
        VfsPost(owner, std::move(result));
    }).detach();
}

// Extract `paths` (one folder of one archive) into a fresh staging folder, then
// hand the result to the UI thread.
void VfsRunExtract(AppState& s, std::vector<std::wstring> paths, VfsOpKind kind, std::wstring dest_dir) {
    if (paths.empty()) return;
    auto result = std::make_unique<VfsOpResult>();
    result->kind = kind;
    result->archive_name = VfsArchiveName(paths.front());
    result->dest_dir = std::move(dest_dir);
    result->staging_dir = VfsStagingDir();
    if (result->staging_dir.empty()) return;
    const std::wstring title =
        kind == VfsOpKind::ExtractTo
            ? VfsFill(l10n::Get(I::VfsExtractTaskTitle), L"{name}", result->archive_name)
            : VfsFill(VfsFill(l10n::Get(I::VfsExportTaskTitle), L"{n}", std::to_wstring(paths.size())), L"{name}",
                      result->archive_name);
    ops::OpsManager* ops = &s.ops;
    const uint64_t task = ops->CreateExternalTask(title, l10n::Get(I::VfsTaskKind), {}, result->dest_dir);
    const HWND owner = s.hwnd;
    std::thread([owner, ops, task, paths = std::move(paths), result = std::move(result)]() mutable {
        vfs::VfsOperationHooks hooks = VfsTaskHooks(ops, task);
        result->status = VfsExtract(paths, result->staging_dir, hooks);
        result->cancelled = result->status != PULSE_OK && ops->ExternalTaskCancelled(task);
        result->message = hooks.message;
        VfsFinishTask(ops, task, result->status, result->cancelled, result->message);
        VfsPost(owner, std::move(result));
    }).detach();
}

std::vector<std::wstring> VfsSelection(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !IsVfsFolder(tab->current_path)) return {};
    std::vector<std::wstring> out;
    for (std::wstring& p : SelectedFullPaths(*tab))
        if (vfs::IsVfsPath(p)) out.push_back(std::move(p));
    return out;
}

void VfsNotice(AppState& s, const std::wstring& text, const std::wstring& detail = {}) {
    if (!s.hwnd) return;
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest, text, detail, {});
}

// Can the archive at `vfs_path` take this change? Says why not otherwise.
bool VfsCheckWritable(AppState& s, const std::wstring& vfs_path) {
    if (VfsWritable(vfs_path)) return true;
    ShowVfsReadOnlyNotice(s);
    return false;
}

// ---- opened members: saved again -> written back --------------------------

struct VfsWatched {
    HWND owner = nullptr;
    std::wstring vfs_path, local_path;
    FILETIME written{};
    ULONGLONG since = 0;
    bool changed = false;   // a new time was seen; written back once it holds still
};

struct VfsWatchList {
    std::mutex mu;
    std::vector<VfsWatched> items;
    bool running = false;
};
VfsWatchList& VfsWatches() {
    static VfsWatchList list;
    return list;
}

bool VfsLastWrite(const std::wstring& path, FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    out = data.ftLastWriteTime;
    return true;
}

bool VfsReadable(const std::wstring& path) {  // the editor has finished writing it
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

void VfsWatchLoop() {
    constexpr ULONGLONG kKeepWatching = 12ull * 60 * 60 * 1000;
    for (;;) {
        Sleep(1500);
        std::vector<VfsWatched> due;
        {
            auto& list = VfsWatches();
            std::lock_guard lock(list.mu);
            const ULONGLONG now = GetTickCount64();
            for (auto it = list.items.begin(); it != list.items.end();) {
                FILETIME t{};
                if (!IsWindow(it->owner) || now - it->since > kKeepWatching || !VfsLastWrite(it->local_path, t)) {
                    it = list.items.erase(it);
                    continue;
                }
                if (CompareFileTime(&t, &it->written) != 0) {
                    it->written = t;
                    it->changed = true;   // wait one more round for it to hold still
                } else if (it->changed && VfsReadable(it->local_path)) {
                    it->changed = false;
                    due.push_back(*it);
                }
                ++it;
            }
            if (list.items.empty()) {
                list.running = false;
                return;
            }
        }
        for (const VfsWatched& w : due) {
            auto* path = new std::pair<std::wstring, std::wstring>(w.vfs_path, w.local_path);
            if (!PostMessageW(w.owner, WM_VFS_OP_DONE, 1, reinterpret_cast<LPARAM>(path))) delete path;
        }
    }
}

// Extracts vfs `paths` into a fresh staging folder while the mouse button is
// still down (a drag waits for it); Esc cancels. The staged items, or empty.
std::vector<std::wstring> VfsStagePaths(AppState& s, const std::vector<std::wstring>& paths) {
    const std::wstring dir = VfsStagingDir();
    if (dir.empty() || paths.empty()) return {};
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    vfs::VfsOperationHooks hooks;
    hooks.is_cancelled = [] { return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0; };
    const PulseStatus st = VfsExtract(paths, dir, hooks);
    SetCursor(old);
    if (st != PULSE_OK) {
        if (!hooks.is_cancelled())
            VfsNotice(s, VfsFill(l10n::Get(I::VfsExtractFailed), L"{name}", VfsArchiveName(paths.front())), hooks.message);
        return {};
    }
    return VfsStagedChildren(dir);
}

// Archive previews (Quick Look, details pane): which archive file each quietly
// opened mount is, so a finished change reloads the preview showing it.
std::vector<std::pair<std::wstring, std::wstring>>& PreviewMounts() {  // mount root, archive file
    static std::vector<std::pair<std::wstring, std::wstring>> mounts;
    return mounts;
}

std::wstring VfsPreviewMount(AppState& s, const std::wstring& archive) {
    std::wstring root = extensions::native::MountArchiveQuietly(s, archive);
    if (!root.empty()) root = VfsMountRoot(root);
    if (root.empty()) {
        VfsNotice(s, l10n::Get(I::ArchivePluginRequired));
        return {};
    }
    auto& mounts = PreviewMounts();
    std::erase_if(mounts, [&](const auto& m) { return m.first == root || _wcsicmp(m.second.c_str(), archive.c_str()) == 0; });
    mounts.emplace_back(root, archive);
    if (mounts.size() > 16) mounts.erase(mounts.begin());
    return root;
}

// "a\\b" inside the mount `root`; empty when a name is not valid there.
std::wstring VfsMemberPath(const std::wstring& root, const std::wstring& member) {
    std::wstring path = root;
    size_t start = 0;
    while (start < member.size()) {
        size_t end = member.find_first_of(L"\\/", start);
        if (end == std::wstring::npos) end = member.size();
        if (end > start) {
            const auto dir = vfs::ParseVfsPath(path);
            if (!dir) return {};
            path = vfs::JoinVfsChild(*dir, std::wstring_view(member).substr(start, end - start));
            if (path.empty()) return {};
        }
        start = end + 1;
    }
    return path;
}

std::vector<std::wstring> PreviewStage(HWND owner, const std::wstring& archive,
                                       const std::vector<std::wstring>& members, uint64_t bytes) {
    AppState* s = GetAppState(owner);
    if (!s) s = GetAppState(GetAncestor(owner, GA_ROOT));
    if (!s || members.empty()) return {};
    if (bytes > kVfsDragLimitBytes) {
        VfsNotice(*s, l10n::Get(I::VfsDragTooLarge));
        return {};
    }
    const std::wstring root = VfsPreviewMount(*s, archive);
    if (root.empty()) return {};
    std::vector<std::wstring> paths;
    for (const std::wstring& m : members) {
        std::wstring p = VfsMemberPath(root, m);
        if (p.empty() || p == root) return {};
        paths.push_back(std::move(p));
    }
    return VfsStagePaths(*s, paths);
}

bool PreviewAdd(HWND owner, const std::wstring& archive, const std::wstring& dir,
                const std::vector<std::wstring>& files) {
    AppState* s = GetAppState(owner);
    if (!s) s = GetAppState(GetAncestor(owner, GA_ROOT));
    if (!s) return false;
    std::vector<std::wstring> sources;
    for (const std::wstring& f : files)
        if (!f.empty() && _wcsicmp(f.c_str(), archive.c_str()) != 0) sources.push_back(f);
    if (sources.empty()) return false;
    const std::wstring root = VfsPreviewMount(*s, archive);
    if (root.empty()) return false;
    const std::wstring target = VfsMemberPath(root, dir);
    return !target.empty() && VfsDropInto(*s, sources, target);
}

}  // namespace

void InstallPreviewArchiveBridge() {
    ui::QuickPreviewArchiveBridge bridge;
    bridge.stage = &PreviewStage;
    bridge.add = &PreviewAdd;
    ui::SetQuickPreviewArchiveBridge(bridge);
}

bool IsVfsFolder(const std::wstring& path) { return vfs::IsVfsPath(path); }

bool VfsWritable(const std::wstring& vfs_path) {
    return (vfs::VfsRegistry::Instance().CapabilitiesOf(vfs_path) & PULSE_VFS_CAP_MODIFY) != 0;
}

void ShowVfsReadOnlyNotice(AppState& s) { VfsNotice(s, l10n::Get(I::VfsReadOnlyFormat)); }

void VfsExtractSelectionTo(AppState& s) {
    std::vector<std::wstring> paths = VfsSelection(s);
    if (paths.empty()) return;
    std::wstring dest;
    if (!PickFolder(s, dest, l10n::Get(I::VfsExtractSelectedTo).c_str(), L"vfs_extract") || dest.empty()) return;
    VfsRunExtract(s, std::move(paths), VfsOpKind::ExtractTo, std::move(dest));
}

void VfsCopySelection(AppState& s) {
    std::vector<std::wstring> paths = VfsSelection(s);
    if (!paths.empty()) VfsRunExtract(s, std::move(paths), VfsOpKind::Copy, {});
}

std::vector<std::wstring> VfsStageSelectionForDrag(AppState& s, const app::Tab& tab) {
    std::vector<std::wstring> paths;
    uint64_t bytes = 0;
    for (int index : tab.SelectedIndices()) {
        const fs::DirEntry e = tab.EntryAt(static_cast<size_t>(index));
        std::wstring full = EntryFullPath(tab, index);
        if (!vfs::IsVfsPath(full)) continue;
        bytes += e.size;
        if (e.is_dir) bytes += kVfsDragLimitBytes / 8;  // its contents are not known here
        paths.push_back(std::move(full));
    }
    if (paths.empty()) return {};
    if (bytes > kVfsDragLimitBytes) {
        VfsNotice(s, l10n::Get(I::VfsDragTooLarge));
        return {};
    }
    // The drag starts once the items are out; the mouse button is still down.
    return VfsStagePaths(s, paths);
}

bool VfsDropInto(AppState& s, const std::vector<std::wstring>& sources, const std::wstring& vfs_dir) {
    if (sources.empty() || !vfs::IsVfsPath(vfs_dir)) return false;
    if (!VfsCheckWritable(s, vfs_dir)) return true;
    const auto dir = vfs::ParseVfsPath(vfs_dir);
    if (!dir) return false;
    std::vector<vfs::VfsChangeRequest> changes;
    for (const std::wstring& src : sources) {
        if (src.empty() || vfs::IsVfsPath(src)) continue;  // archive to archive: not supported
        vfs::VfsChangeRequest c;
        c.kind = PULSE_VFS_CHANGE_PUT;
        c.vfs_path = vfs::JoinVfsChild(*dir, VfsLeaf(src));
        c.source_path = src;
        if (!c.vfs_path.empty()) changes.push_back(std::move(c));
    }
    if (changes.empty()) return false;
    VfsRunModify(s, std::move(changes), VfsOpKind::Modify, vfs_dir);
    return true;
}

void VfsPasteInto(AppState& s, const std::wstring& vfs_dir) {
    ops::ClipboardData clip;
    if (!ops::ReadClipboard(clip) || clip.paths.empty()) return;
    // A cut is added like a copy: the originals stay where they are.
    VfsDropInto(s, clip.paths, vfs_dir);
}

void VfsDeleteSelection(AppState& s) {
    std::vector<std::wstring> paths = VfsSelection(s);
    if (paths.empty() || !VfsCheckWritable(s, paths.front())) return;
    ui::ConfirmDialogSpec spec;
    spec.title = VfsArchiveName(paths.front());
    spec.message = VfsFill(l10n::Get(I::VfsDeleteConfirm), L"{n}", std::to_wstring(paths.size()));
    spec.confirm_text = l10n::Get(I::Delete);
    spec.cancel_text = l10n::Get(I::Cancel);
    spec.danger = true;
    spec.default_choice = ui::ConfirmChoice::Cancel;
    for (size_t i = 0; i < paths.size() && i < 5; ++i) spec.items.push_back(VfsLeaf(paths[i]));
    spec.items_total = paths.size();
    if (!ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor)) return;
    std::vector<vfs::VfsChangeRequest> changes;
    for (const std::wstring& p : paths) {
        vfs::VfsChangeRequest c;
        c.kind = PULSE_VFS_CHANGE_DELETE;
        c.vfs_path = p;
        changes.push_back(std::move(c));
    }
    VfsRunModify(s, std::move(changes), VfsOpKind::Modify, VfsParentOf(paths.front()));
}

void VfsRenameItem(AppState& s, const std::wstring& vfs_path, const std::wstring& new_name) {
    if (new_name.empty() || new_name == VfsLeaf(vfs_path) || !VfsCheckWritable(s, vfs_path)) return;
    const std::wstring parent = VfsParentOf(vfs_path);
    const auto dir = vfs::ParseVfsPath(parent);
    if (!dir) return;
    vfs::VfsChangeRequest c;
    c.kind = PULSE_VFS_CHANGE_RENAME;
    c.vfs_path = vfs_path;
    c.new_vfs_path = vfs::JoinVfsChild(*dir, new_name);
    if (c.new_vfs_path.empty()) return;
    VfsRunModify(s, {std::move(c)}, VfsOpKind::Modify, parent);
}

void VfsWatchOpenedFile(HWND owner, const std::wstring& vfs_path, const std::wstring& local_path) {
    if (!VfsWritable(vfs_path)) return;
    VfsWatched w;
    w.owner = owner;
    w.vfs_path = vfs_path;
    w.local_path = local_path;
    w.since = GetTickCount64();
    if (!VfsLastWrite(local_path, w.written)) return;
    auto& list = VfsWatches();
    std::lock_guard lock(list.mu);
    list.items.push_back(std::move(w));
    if (!list.running) {
        list.running = true;
        std::thread(VfsWatchLoop).detach();
    }
}

void HandleVfsOpDone(AppState& s, WPARAM kind, LPARAM payload) {
    if (kind == 1) {  // an opened member was saved: put it back into its archive
        std::unique_ptr<std::pair<std::wstring, std::wstring>> saved(
            reinterpret_cast<std::pair<std::wstring, std::wstring>*>(payload));
        vfs::VfsChangeRequest c;
        c.kind = PULSE_VFS_CHANGE_PUT;
        c.vfs_path = saved->first;
        c.source_path = saved->second;
        VfsRunModify(s, {std::move(c)}, VfsOpKind::WriteBack, VfsParentOf(saved->first), VfsLeaf(saved->first));
        return;
    }
    std::unique_ptr<VfsOpResult> r(reinterpret_cast<VfsOpResult*>(payload));
    if (!r) return;
    const bool out = r->kind == VfsOpKind::ExtractTo || r->kind == VfsOpKind::Copy;
    if (r->cancelled) return;
    if (r->status != PULSE_OK) {
        if (out) {
            VfsNotice(s, VfsFill(l10n::Get(I::VfsExtractFailed), L"{name}", r->archive_name), r->message);
        } else if (r->status == PULSE_ERROR_PERMISSION) {
            VfsNotice(s, l10n::Get(I::VfsEncryptedReadOnly));
        } else {
            VfsNotice(s, VfsFill(l10n::Get(I::VfsUpdateFailed), L"{name}", r->archive_name), r->message);
        }
        if (!out && !r->refresh_dir.empty()) RefreshPath(s, r->refresh_dir, RefreshReason::OperationCompleted);
        return;
    }
    if (r->kind == VfsOpKind::ExtractTo) {
        // Out of the staging folder into the chosen one: the transfer engine
        // asks about name clashes and shows the progress.
        ops::OpRequest req;
        req.type = ops::OpType::Move;
        req.sources = VfsStagedChildren(r->staging_dir);
        req.dest_dir = r->dest_dir;
        if (req.sources.empty()) return;
        SubmitWithConflictResolution(s, std::move(req));
        return;
    }
    if (r->kind == VfsOpKind::Copy) {
        const std::vector<std::wstring> staged = VfsStagedChildren(r->staging_dir);
        if (staged.empty()) return;
        CollectPathsToTray(s, staged, false);
        ShowStatusFeedback(s, VfsFill(l10n::Get(I::VfsCopiedSnack), L"{n}", std::to_wstring(staged.size())));
        return;
    }
    if (!r->refresh_dir.empty()) RefreshPath(s, r->refresh_dir, RefreshReason::OperationCompleted);
    // A preview showing that archive lists it again.
    for (const auto& [root, file] : PreviewMounts()) {
        if (root != r->mount_root) continue;
        ui::QuickPreviewWindow::ReloadEverywhere(file);
    }
    if (r->kind == VfsOpKind::Undo) {
        VfsNotice(s, VfsFill(l10n::Get(I::VfsUndoneSnack), L"{name}", r->archive_name));
        return;
    }
    {
        auto& slot = VfsUndo();
        std::lock_guard lock(slot.mu);
        slot.mount_root = r->mount_root;
        slot.archive_name = r->archive_name;
        slot.refresh_dir = r->refresh_dir;
    }
    const std::wstring text =
        r->kind == VfsOpKind::WriteBack
            ? VfsFill(VfsFill(l10n::Get(I::VfsWriteBackSnack), L"{file}", r->file_name), L"{name}", r->archive_name)
            : VfsFill(l10n::Get(I::VfsUpdatedSnack), L"{name}", r->archive_name);
    if (s.hwnd)
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Undo, text, {},
                                       {{l10n::Get(I::SnackUndo), WM_VFS_UNDO}});
}

void HandleVfsUndo(AppState& s) {
    std::wstring root, refresh;
    {
        auto& slot = VfsUndo();
        std::lock_guard lock(slot.mu);
        root.swap(slot.mount_root);
        refresh = slot.refresh_dir;
    }
    if (root.empty()) return;
    vfs::VfsChangeRequest c;
    c.kind = PULSE_VFS_CHANGE_RESTORE;
    c.vfs_path = root;
    VfsRunModify(s, {std::move(c)}, VfsOpKind::Undo, refresh);
}

}  // namespace pulse

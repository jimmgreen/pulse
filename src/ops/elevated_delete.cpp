#include "elevated_delete.h"
#include "../common/path_utils.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <sherrors.h>
#include <wrl/client.h>
#include <algorithm>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace pulse::ops {
namespace {
using Microsoft::WRL::ComPtr;
constexpr HRESULT kCancelled = HRESULT_FROM_WIN32(ERROR_CANCELLED);
class DeleteStageScope {
public:
    DeleteStageScope(const DeleteStageObserver& observer, DeleteStage stage) : observer_(observer), stage_(stage) { Emit(true, S_OK); }
    ~DeleteStageScope() { if (!ended_) Emit(false, E_UNEXPECTED); }
    void End(HRESULT hr) { if (!ended_) { ended_ = true; Emit(false, hr); } }
private:
    void Emit(bool begin, HRESULT hr) noexcept {
        try { if (observer_) observer_(stage_, begin, hr); } catch (...) {}
    }
    const DeleteStageObserver& observer_;
    DeleteStage stage_;
    bool ended_ = false;
};
template<class Work> HRESULT TraceDeleteStage(const DeleteStageObserver& observer, DeleteStage stage, Work&& work) {
    DeleteStageScope trace(observer, stage);
    const HRESULT result = work();
    trace.End(result);
    return result;
}
std::wstring ItemPath(IShellItem* item) {
    PWSTR raw = nullptr;
    if (!item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) return {};
    std::wstring path;
    try { if (raw) path = raw; }
    catch (...) { CoTaskMemFree(raw); throw; }
    CoTaskMemFree(raw); return path;
}
bool Missing(const std::wstring& item) {
    if (GetFileAttributesW(item.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}
std::wstring ErrorText(HRESULT hr) {
    PWSTR text = nullptr;
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring message;
    if (count && text) message.assign(text, count);
    if (text) LocalFree(text);
    // No system text (e.g. COPYENGINE_E_*): leave it empty so the UI side can
    // describe the HRESULT in the user's language.
    return message;
}
// RecycleItem has a recycle-only contract. Unlike a generic DeleteItem request,
// failure is never interpreted here as permission to invoke permanent deletion.
class RecycleSink final : public ITransferAdviseSink {
public:
    RecycleSink(const std::wstring& source, std::atomic<bool>& cancel, const ShellTransferProgress& progress)
        : source_(source), cancel_(cancel), progress_(progress) {}
    HRESULT failure = S_OK;
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_ITransferAdviseSink) return E_NOINTERFACE;
        *out = static_cast<ITransferAdviseSink*>(this); AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override { const ULONG refs = --refs_; if (!refs) delete this; return refs; }
    IFACEMETHODIMP UpdateProgress(ULONGLONG done, ULONGLONG total, int, int, int, int) override {
        try { if (progress_) progress_(source_, total ? static_cast<float>(100.0 * static_cast<double>(done) / static_cast<double>(total)) : -1.0f); }
        catch (...) { return Fail(E_FAIL); }
        return cancel_.load() ? kCancelled : S_OK;
    }
    IFACEMETHODIMP UpdateTransferState(TRANSFER_ADVISE_STATE) override { return cancel_.load() ? kCancelled : S_OK; }
    IFACEMETHODIMP ConfirmOverwrite(IShellItem*, IShellItem*, LPCWSTR) override { return Fail(E_ACCESSDENIED); }
    IFACEMETHODIMP ConfirmEncryptionLoss(IShellItem*) override { return Fail(E_ACCESSDENIED); }
    IFACEMETHODIMP FileFailure(IShellItem*, LPCWSTR, HRESULT hr, LPWSTR, ULONG) override { return Fail(hr); }
    IFACEMETHODIMP SubStreamFailure(IShellItem*, LPCWSTR, HRESULT hr) override { return Fail(hr); }
    IFACEMETHODIMP PropertyFailure(IShellItem*, const PROPERTYKEY*, HRESULT hr) override { return Fail(hr); }
private:
    HRESULT Fail(HRESULT hr) { if (SUCCEEDED(hr)) hr = E_FAIL; if (SUCCEEDED(failure)) failure = hr; return hr; }
    std::atomic<ULONG> refs_{1};
    const std::wstring& source_;
    std::atomic<bool>& cancel_;
    const ShellTransferProgress& progress_;
};
std::wstring ParentKey(const std::wstring& path) {
    std::wstring key = path::StripExtendedPathPrefix(path);
    const size_t slash = key.find_last_of(L'\\');
    key.resize(slash == std::wstring::npos ? 0 : slash);
    if (!key.empty()) CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    return key;
}

// A native recycle is one rename, so the Shell is not told about each item:
// one SHCNE_UPDATEDIR per affected folder plus one Recycle Bin update replace
// the per-item DELETE/CREATE pair, which cost ~4.8 ms per item on a SATA SSD
// (1000 files: 6246 ms per-item vs 1459 ms batched). Pending folders are
// published at most once a second during a batch and always when it ends.
constexpr ULONGLONG kRecycleNotifyIntervalMs = 1000;
void AddRecycledParent(RecycledParents& parents, const std::wstring& source) {
    std::wstring parent = native_recycle::detail::NormalizePlainPath(source);
    const size_t slash = parent.find_last_of(L'\\');
    if (slash == std::wstring::npos) return;
    parent.resize(slash == 2 && parent[1] == L':' ? 3 : slash);  // a root keeps its slash: "C:\x" -> "C:\"
    if (parent.empty()) return;
    std::wstring key = parent;
    CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    parents.try_emplace(std::move(key), std::move(parent));
}
uint32_t NotifyRecycledParents(RecycledParents& parents) {
    const auto count = static_cast<uint32_t>(parents.size());
    for (const auto& entry : parents)
        native_recycle::detail::NotifyShellPath(SHCNE_UPDATEDIR, entry.second.c_str());
    parents.clear();
    return count;
}
}  // namespace

struct RecycleCache::State {
    ComPtr<IShellItem> bin;
    std::wstring transfer_parent;
    ComPtr<ITransferSource> transfer;
    ComPtr<IShellItem> parent;  // the folder `transfer` serves (transfer_parent)
    std::wstring volume_parent;
    bool volume_checked = false;
    bool available = false;
    DWORD volume_error = 0;
    RecycleBinLimits limits;
    std::wstring native_bin_dir;
    std::unordered_map<std::wstring, uint64_t> native_bins_max_bytes;
    std::unordered_set<std::wstring> protected_payloads_upper;
    uint64_t native_recycled_count = 0;
    // Folders that lost a natively recycled item and have not been told yet.
    RecycledParents notify_parents;
    ULONGLONG notify_last = 0;
};
RecycleCache::RecycleCache() : state_(std::make_unique<State>()) {}
RecycleCache::~RecycleCache() = default;

RecycleBatchQuotaSummary FlushRecycleCache(RecycleCache& cache) {
    RecycleBatchQuotaSummary summary;
    auto& state = cache.state();
    if (state.native_recycled_count == 0) return summary;
    const ULONGLONG notify_started = GetTickCount64();
    summary.notified_parents = NotifyRecycledParents(state.notify_parents);
    pulse::recycle_empty::NotifyShell();
    summary.notify_ms = GetTickCount64() - notify_started;
    state.notify_last = 0;
    const ULONGLONG started = GetTickCount64();
    for (const auto& [bin_dir, max_bytes] : state.native_bins_max_bytes) {
        if (max_bytes == 0) continue;
        uint64_t used = 0;
        uint64_t victim_bytes = 0;
        const uint32_t queued = native_recycle::QueueQuotaCompaction(
            bin_dir, max_bytes, state.protected_payloads_upper, used, victim_bytes);
        summary.bin_used_bytes += used;
        summary.bin_max_bytes = (std::max)(summary.bin_max_bytes, max_bytes);
        summary.victims_queued += queued;
        summary.victim_bytes += victim_bytes;
    }
    summary.quota_check_ms = GetTickCount64() - started;
    state.native_recycled_count = 0;
    state.native_bins_max_bytes.clear();
    state.protected_payloads_upper.clear();
    return summary;
}

// Parsing a full path walks it from the namespace root: known folders,
// libraries and property handlers, 3-17 ms per item under a user profile.
// Asking the item's own folder for its name skips all of that. Only a plain
// name is asked for this way; a root, an extended path, a stream or a name
// Win32 would trim is parsed whole, as before.
HRESULT CreateItemForPath(const std::wstring& path, IShellItem* folder, IShellItem** item) {
    if (!item) return E_POINTER;
    *item = nullptr;
    if (folder && !path.starts_with(L"\\\\?\\") && path.find(L'/') == std::wstring::npos) {
        const size_t slash = path.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash > 0 && slash + 1 < path.size() && path[slash - 1] != L'\\') {
            const std::wstring leaf = path.substr(slash + 1);
            if (leaf.find(L':') == std::wstring::npos && leaf.back() != L'.' && leaf.back() != L' ' &&
                SUCCEEDED(SHCreateItemFromRelativeName(folder, leaf.c_str(), nullptr, IID_PPV_ARGS(item))))
                return S_OK;
        }
    }
    return SHCreateItemFromParsingName(path::StripExtendedPathPrefix(path).c_str(), nullptr, IID_PPV_ARGS(item));
}

namespace {
HRESULT RecycleOnly(const std::wstring& source, std::atomic<bool>& cancel,
                    ShellTransferResult& result, const ShellTransferProgress& progress,
                    RecycleCache* cache, const DeleteStageObserver& observer) {
    ComPtr<IShellItem> item, parent, bin;
    ComPtr<ITransferSource> transfer;
    const std::wstring parent_key = cache ? ParentKey(source) : std::wstring{};
    const bool same_folder = cache && cache->state().transfer && cache->state().parent &&
        !parent_key.empty() && cache->state().transfer_parent == parent_key;
    HRESULT hr = TraceDeleteStage(observer, DeleteStage::CreateItem, [&] { return CreateItemForPath(source, same_folder ? cache->state().parent.Get() : nullptr, &item); });
    if (FAILED(hr)) return hr;
    if (same_folder) {
        transfer = cache->state().transfer;
    } else {
        hr = TraceDeleteStage(observer, DeleteStage::GetParent, [&] { return item->GetParent(&parent); });
        if (FAILED(hr)) return hr;
        hr = TraceDeleteStage(observer, DeleteStage::BindTransfer, [&] { return parent->BindToHandler(nullptr, BHID_Transfer, IID_PPV_ARGS(&transfer)); });
        if (FAILED(hr)) return hr;
        if (cache) {
            cache->state().transfer = transfer;
            cache->state().parent = parent;
            cache->state().transfer_parent = parent_key;
        }
    }
    if (cache && cache->state().bin) {
        bin = cache->state().bin;
    } else {
        hr = TraceDeleteStage(observer, DeleteStage::GetBin, [&] { return SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&bin)); });
        if (FAILED(hr)) return hr;
        if (cache) cache->state().bin = bin;
    }
    ComPtr<RecycleSink> sink;
    sink.Attach(new (std::nothrow) RecycleSink(source, cancel, progress));
    if (!sink) return E_OUTOFMEMORY;
    DWORD cookie = 0;
    hr = TraceDeleteStage(observer, DeleteStage::Advise, [&] { return transfer->Advise(sink.Get(), &cookie); });
    if (FAILED(hr)) return hr;
    ComPtr<IShellItem> recycled;
    bool attempted = false;
    if (cancel.load()) hr = kCancelled;
    else {
        if (progress) progress(source, -1.0f);
        if (cancel.load()) hr = kCancelled;
        else {
            attempted = true;
            hr = TraceDeleteStage(observer, DeleteStage::RecycleItem, [&] { return transfer->RecycleItem(item.Get(), bin.Get(), TSF_NORMAL, &recycled); });
        }
    }
    (void)TraceDeleteStage(observer, DeleteStage::Unadvise, [&] { return transfer->Unadvise(cookie); });
    // A failed transfer source is not trusted with the next item.
    if (cache && FAILED(hr)) {
        cache->state().transfer.Reset();
        cache->state().parent.Reset();
        cache->state().transfer_parent.clear();
    }
    DeleteStageScope confirm_stage(observer, DeleteStage::ConfirmMissing);
    const DWORD attributes = GetFileAttributesW(source.c_str());
    const DWORD source_error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    const bool missing = attributes == INVALID_FILE_ATTRIBUTES &&
        (source_error == ERROR_FILE_NOT_FOUND || source_error == ERROR_PATH_NOT_FOUND);
    confirm_stage.End(missing ? S_OK : source_error ? HRESULT_FROM_WIN32(source_error) : S_FALSE);
    // A returned recycled object proves a change even if the original path was
    // recreated concurrently. Unknown source state after the call cannot be
    // treated as proof of no mutation and must not enable replay.
    if (recycled || (attempted && attributes == INVALID_FILE_ATTRIBUTES)) result.mutated = true;
    if ((hr == S_OK || hr == COPYENGINE_S_DONT_PROCESS_CHILDREN) && recycled && missing) {
        result.sources.push_back(source);
        return FAILED(sink->failure) ? sink->failure : S_OK;
    }
    if (FAILED(hr)) return hr;
    if (FAILED(sink->failure)) return sink->failure;
    // Skipped/pending results and a missing output object are not completion.
    return E_FAIL;
}
class DeleteSink final : public IFileOperationProgressSink {
public:
    DeleteSink(const std::wstring& original, std::atomic<bool>& cancel,
               ShellTransferResult& result, const ShellTransferProgress& progress)
        : original_(original), cancel_(cancel), result_(result), progress_(progress) {
        const DWORD attributes = GetFileAttributesW(original.c_str());
        directory_ = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    bool DeferredDirectory() const { return deferred_directory_; }
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileOperationProgressSink) return E_NOINTERFACE;
        *out = static_cast<IFileOperationProgressSink*>(this); AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override { const ULONG refs = --refs_; if (!refs) delete this; return refs; }
    HRESULT Cancel() const { return cancel_.load() ? kCancelled : S_OK; }
    IFACEMETHODIMP StartOperations() override { return Cancel(); }
    IFACEMETHODIMP FinishOperations(HRESULT hr) override { NoteFailure(hr); return S_OK; }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem* item) override {
        if (FAILED(Cancel())) return kCancelled;
        try {
            current_ = ItemPath(item);
            if (path::EqualInsensitive(path::StripExtendedPathPrefix(original_), current_))
                item->QueryInterface(IID_PPV_ARGS(&root_identity_));
            if (progress_) progress_(current_, -1.0f);
        } catch (...) { NoteFailure(E_OUTOFMEMORY); return E_OUTOFMEMORY; }
        return Cancel();
    }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem* item, HRESULT hr, IShellItem*) override {
        NoteFailure(hr);
        try {
            const auto from = ItemPath(item);
            ComPtr<IUnknown> identity;
            if (item) item->QueryInterface(IID_PPV_ARGS(&identity));
            const bool root = path::EqualInsensitive(path::StripExtendedPathPrefix(original_), from) ||
                (root_identity_ && identity.Get() == root_identity_.Get());
            // The filesystem provider reports NOT_HANDLED for a directory
            // before deleting its children. It is not a completion callback;
            // only the final successful operation plus absence can confirm it.
            if (directory_ && root && hr == COPYENGINE_S_NOT_HANDLED)
                deferred_directory_ = true;
            // Ignore skip/pending codes; only actual delete completion counts.
            const bool completed_status = hr == S_OK || hr == COPYENGINE_S_DONT_PROCESS_CHILDREN;
            if (completed_status) {
                result_.mutated = true;
                if (root && Missing(original_)) {
                    if (result_.sources.empty() || result_.sources.back() != original_)
                        result_.sources.push_back(original_);
                }
            }
        } catch (...) { NoteFailure(E_OUTOFMEMORY); return E_OUTOFMEMORY; }
        return Cancel();
    }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP UpdateProgress(UINT total, UINT done) override {
        try { if (progress_) progress_(current_, total ? static_cast<float>(100.0 * done / total) : -1.0f); }
        catch (...) { NoteFailure(E_FAIL); return E_FAIL; }
        return Cancel();
    }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }
private:
    void NoteFailure(HRESULT hr) { if (FAILED(hr) && SUCCEEDED(result_.hr)) result_.hr = hr; }
    std::atomic<ULONG> refs_{1};
    const std::wstring& original_;
    bool directory_ = false;
    bool deferred_directory_ = false;
    std::atomic<bool>& cancel_;
    ShellTransferResult& result_;
    const ShellTransferProgress& progress_;
    std::wstring current_;
    ComPtr<IUnknown> root_identity_;
};
}
namespace {
constexpr wchar_t kExplorerPolicies[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";

bool ReadDword(HKEY root, const std::wstring& key, const wchar_t* name, DWORD& value) {
    DWORD size = sizeof(value);
    return RegGetValueW(root, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
}

bool PolicyDword(const wchar_t* name, DWORD& value) {
    return ReadDword(HKEY_CURRENT_USER, kExplorerPolicies, name, value) ||
           ReadDword(HKEY_LOCAL_MACHINE, kExplorerPolicies, name, value);
}

std::wstring LongPath(const std::wstring& path) {
    if (path.size() < 240 || path.starts_with(L"\\\\?\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

// Bytes under `folder`, stopping once past `limit`. Links are not entered,
// the same as the recycle itself. A folder that cannot be listed fails the
// measurement with its Win32 error: its size is unknown, so the limit cannot
// be shown to hold (the Shell would delete an oversized item for good). This
// matches native_recycle::MeasureFolderFast. A folder that vanished during
// the walk holds nothing and is skipped.
HRESULT MeasureFolder(const std::wstring& folder, uint64_t limit, std::atomic<bool>& cancel, uint64_t& total) {
    total = 0;
    std::vector<std::wstring> pending{folder};
    while (!pending.empty() && total <= limit && !cancel.load()) {
        const std::wstring current = std::move(pending.back());
        pending.pop_back();
        WIN32_FIND_DATAW data{};
        const std::wstring pattern = LongPath(current + (current.ends_with(L'\\') ? L"*" : L"\\*"));
        HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                       FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) continue;
            return HRESULT_FROM_WIN32(error != ERROR_SUCCESS ? error : ERROR_ACCESS_DENIED);
        }
        do {
            if (cancel.load(std::memory_order_relaxed)) break;
            const wchar_t* name = data.cFileName;
            if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    pending.push_back(current + (current.ends_with(L'\\') ? L"" : L"\\") + name);
            } else {
                total += (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            }
        } while (total <= limit && FindNextFileW(find, &data));
        const DWORD next_error = total <= limit && !cancel.load(std::memory_order_relaxed) ? GetLastError() : ERROR_NO_MORE_FILES;
        FindClose(find);
        if (next_error != ERROR_NO_MORE_FILES) return HRESULT_FROM_WIN32(next_error);
    }
    return S_OK;
}
}  // namespace

namespace {
RecycleBinLimits ReadRecycleBinLimitsForRoot(const std::wstring& root) {
    RecycleBinLimits limits;
    DWORD value = 0;
    ULARGE_INTEGER total{}, free{};
    if (GetDiskFreeSpaceExW(root.c_str(), nullptr, &total, &free))
        limits.used_bytes = total.QuadPart - (std::min)(free.QuadPart, total.QuadPart);
    // Policy size (percent of the volume) wins over the user's setting.
    if (PolicyDword(L"RecycleBinSize", value) && value <= 100) {
        limits.max_bytes = total.QuadPart / 100 * value;
        return limits;
    }
    wchar_t volume_name[64]{};
    std::wstring guid;
    if (GetVolumeNameForVolumeMountPointW(root.c_str(), volume_name, 64)) {
        const std::wstring name = volume_name;
        const size_t open = name.find(L'{'), close = name.find(L'}');
        if (open != std::wstring::npos && close != std::wstring::npos && close > open)
            guid = name.substr(open, close - open + 1);
    }
    bool have_capacity = false;
    if (!guid.empty()) {
        const std::wstring key =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\BitBucket\\Volume\\" + guid;
        if (ReadDword(HKEY_CURRENT_USER, key, L"NukeOnDelete", value) && value != 0) {
            limits.disabled = true;
            return limits;
        }
        if (ReadDword(HKEY_CURRENT_USER, key, L"MaxCapacity", value)) {  // MB
            limits.max_bytes = static_cast<uint64_t>(value) << 20;
            have_capacity = true;
        }
    }
    if (!have_capacity && total.QuadPart) {
        // No setting saved yet: Windows' default, 10% of the first 40 GB and
        // 5% of the rest.
        constexpr uint64_t k40GB = 40ull << 30;
        const uint64_t bytes = total.QuadPart;
        limits.max_bytes = (std::min)(bytes, k40GB) / 10 + (bytes > k40GB ? (bytes - k40GB) / 20 : 0);
    }
    return limits;
}

} // namespace

RecycleBinLimits ReadRecycleBinLimits(const std::wstring& path, DWORD* volume_error) {
    if (volume_error) *volume_error = 0;
    DWORD value = 0;
    if (PolicyDword(L"NoRecycleFiles", value) && value != 0) {
        RecycleBinLimits limits;
        limits.disabled = true;
        return limits;
    }
    std::vector<wchar_t> root(32768);
    if (!GetVolumePathNameW(LongPath(path).c_str(), root.data(), static_cast<DWORD>(root.size()))) {
        if (volume_error) *volume_error = GetLastError();
        return {};
    }
    return ReadRecycleBinLimitsForRoot(root.data());
}

RecyclePolicy QueryRecyclePolicy(const std::wstring& path) {
    RecyclePolicy policy;
    if (path::StripExtendedPathPrefix(path).starts_with(L"\\\\")) return policy;
    std::vector<wchar_t> root(32768);
    if (!GetVolumePathNameW(LongPath(path).c_str(), root.data(), static_cast<DWORD>(root.size()))) {
        policy.volume_error = GetLastError();
        return policy;
    }
    if (GetDriveTypeW(root.data()) != DRIVE_FIXED) return policy;
    DWORD value = 0;
    if (PolicyDword(L"NoRecycleFiles", value) && value != 0) policy.limits.disabled = true;
    else policy.limits = ReadRecycleBinLimitsForRoot(root.data());
    policy.available = !policy.limits.disabled;
    return policy;
}

HRESULT CheckRecycleFits(const std::wstring& path, const RecycleBinLimits& limits, std::atomic<bool>& cancel,
                         bool measure_folders) {
    if (cancel.load(std::memory_order_relaxed)) return kCancelled;
    if (limits.disabled) return COPYENGINE_E_RECYCLE_BIN_NOT_FOUND;
    if (!limits.max_bytes || (limits.used_bytes && limits.max_bytes >= limits.used_bytes)) return S_OK;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(LongPath(path).c_str(), GetFileExInfoStandard, &data)) return S_OK;  // the recycle reports it
    uint64_t bytes = 0;
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        bytes = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    } else if (measure_folders && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        const HRESULT measured = MeasureFolder(path, limits.max_bytes, cancel, bytes);
        if (cancel.load()) return kCancelled;
        if (FAILED(measured)) return measured;  // size unknown: nothing touched
    }
    return bytes > limits.max_bytes ? kRecycleBinTooSmall : S_OK;
}

bool RecycleBinAvailable(const std::wstring& path, DWORD* volume_error) {
    const auto policy = QueryRecyclePolicy(path);
    if (volume_error) *volume_error = policy.volume_error;
    return policy.available;
}

ShellTransferResult DeleteWithCurrentToken(const std::vector<std::wstring>& sources,
    bool permanent, HWND owner, std::atomic<bool>& cancel, ShellTransferProgress progress,
    RecycleCache* cache, DeleteStageObserver observer, const DeleteHooks* hooks) {
    ShellTransferResult result;
    std::unordered_map<std::wstring, uint64_t> local_native_bins;
    std::unordered_set<std::wstring> local_protected_upper;
    uint64_t local_native_count = 0;
    RecycledParents local_parents;
    for (const auto& source : sources) {
        if (cancel.load()) { result.hr = kCancelled; result.cancelled = true; break; }
        if (!permanent) {
            // The UI sends such items as permanent deletes after asking
            // (DeletePaths); a recycle request for them is refused, never
            // turned into a silent permanent delete.
            DeleteStageScope policy_stage(observer, DeleteStage::Policy);
            DWORD volume_error = 0;
            bool available = false;
            RecycleBinLimits limits;
            // A mount point is checked on its own: its volume is not its parent's.
            const DWORD source_attributes = cache ? GetFileAttributesW(LongPath(source).c_str()) : INVALID_FILE_ATTRIBUTES;
            const bool cacheable = source_attributes != INVALID_FILE_ATTRIBUTES &&
                !(source_attributes & FILE_ATTRIBUTE_REPARSE_POINT);
            const std::wstring volume_parent = cacheable ? ParentKey(source) : std::wstring{};
            if (cache && cache->state().volume_checked && !volume_parent.empty() &&
                cache->state().volume_parent == volume_parent) {
                available = cache->state().available;
                volume_error = cache->state().volume_error;
                limits = cache->state().limits;
            } else {
                const auto policy = QueryRecyclePolicy(source);
                available = policy.available;
                volume_error = policy.volume_error;
                limits = policy.limits;
                if (cache && !volume_parent.empty()) {
                    auto& state = cache->state();
                    state.volume_checked = true;
                    state.volume_parent = volume_parent;
                    state.limits = limits;
                    state.available = available;
                    state.volume_error = volume_error;
                }
            }
            policy_stage.End(available ? S_OK : volume_error ? HRESULT_FROM_WIN32(volume_error) : COPYENGINE_E_RECYCLE_BIN_NOT_FOUND);
            if (!available) {
                if (volume_error) {
                    result.hr = HRESULT_FROM_WIN32(volume_error);
                } else {
                    result.hr = COPYENGINE_E_RECYCLE_BIN_NOT_FOUND;
                    result.error = L"This location cannot be safely sent to the Recycle Bin. The item was not deleted.";
                }
                break;
            }
            // Fast path: direct same-volume $I v2 + MoveFileExW($R). Single
            // FindFirstFileExW measurement (no double walk) and <1 ms atomic
            // move, with background quota compaction instead of synchronous
            // Shell purging after the source already vanished.
            native_recycle::NativeRecycleResult native;
            {
                DeleteStageScope native_stage(observer, DeleteStage::NativeRecycle);
                const std::wstring cached_bin = cache ? cache->state().native_bin_dir : std::wstring{};
                // A bin as large as the volume's used space takes anything
                // (CheckRecycleFits): no limit to prove, so a folder that
                // cannot be listed is not refused for an unknowable size.
                const uint64_t native_limit =
                    limits.used_bytes && limits.max_bytes >= limits.used_bytes ? 0 : limits.max_bytes;
                native = native_recycle::RecycleItemNative(
                    source, native_limit, cancel,
                    hooks ? hooks->on_scan : native_recycle::ScanProgressFn{},
                    false, {}, cached_bin);
                native.metrics.bin_max_bytes = limits.max_bytes;
                native_stage.End(native.handled ? native.hr : S_FALSE);
            }
            if (hooks && hooks->on_native_metrics) {
                try { hooks->on_native_metrics(native.metrics); } catch (...) {}
            }
            if (native.handled) {
                result.hr = native.hr;
                result.cancelled = native.cancelled || cancel.load() || result.hr == kCancelled;
                if (result.cancelled && SUCCEEDED(result.hr)) result.hr = kCancelled;
                if (native.mutated) result.mutated = true;
                if (SUCCEEDED(result.hr)) {
                    result.sources.push_back(source);
                    if (!native.recycled_payload.empty())
                        result.destinations.push_back(native.recycled_payload);
                    if (cache) {
                        auto& state = cache->state();
                        if (!native.bin_dir.empty()) {
                            state.native_bin_dir = native.bin_dir;
                            state.native_bins_max_bytes[native.bin_dir] = limits.max_bytes;
                        }
                        if (!native.recycled_payload.empty())
                            state.protected_payloads_upper.insert(
                                native_recycle::detail::UpperFold(native.recycled_payload));
                        ++state.native_recycled_count;
                        AddRecycledParent(state.notify_parents, source);
                        const ULONGLONG now = GetTickCount64();
                        if (state.notify_last == 0) {
                            state.notify_last = now;
                        } else if (now - state.notify_last >= kRecycleNotifyIntervalMs) {
                            NotifyRecycledParents(state.notify_parents);
                            pulse::recycle_empty::NotifyShell();
                            state.notify_last = now;
                        }
                    } else {
                        if (!native.bin_dir.empty())
                            local_native_bins[native.bin_dir] = limits.max_bytes;
                        if (!native.recycled_payload.empty())
                            local_protected_upper.insert(
                                native_recycle::detail::UpperFold(native.recycled_payload));
                        ++local_native_count;
                        AddRecycledParent(local_parents, source);
                    }
                    continue;
                }
                if (result.hr == kRecycleBinTooSmall)
                    result.error = L"This item is larger than the Recycle Bin can hold. It was not deleted.";
                else if (result.hr == COPYENGINE_E_RECYCLE_BIN_NOT_FOUND)
                    result.error = L"This location cannot be safely sent to the Recycle Bin. The item was not deleted.";
                break;
            }

            // Fallback path (uninitialized $Recycle.Bin, OneDrive/reparse root,
            // cross-volume mount point): preserve full Shell compatibility.
            // The bin's size limit: an item larger than the bin would be
            // deleted for good by the Shell, so it is refused here, untouched.
            result.hr = TraceDeleteStage(observer, DeleteStage::SizePreflight, [&] { return CheckRecycleFits(source, limits, cancel); });
            if (FAILED(result.hr)) {
                result.cancelled = result.hr == kCancelled;
                if (result.hr == kRecycleBinTooSmall)
                    result.error = L"This item is larger than the Recycle Bin can hold. It was not deleted.";
                else if (result.hr == COPYENGINE_E_RECYCLE_BIN_NOT_FOUND)
                    result.error = L"This location cannot be safely sent to the Recycle Bin. The item was not deleted.";
                break;
            }
            result.hr = RecycleOnly(source, cancel, result, progress, cache, observer);
            result.cancelled = cancel.load() || result.hr == kCancelled || result.hr == E_ABORT || result.hr == COPYENGINE_E_USER_CANCELLED;
            if (result.cancelled && SUCCEEDED(result.hr)) result.hr = kCancelled;
            if (FAILED(result.hr) || result.cancelled) break;
            continue;
        }
        ComPtr<IFileOperation> operation;
        result.hr = TraceDeleteStage(observer, DeleteStage::CreateOperation, [&] { return CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation)); });
        if (FAILED(result.hr)) break;
        result.hr = operation->SetOwnerWindow(owner);
        if (FAILED(result.hr)) break;
        result.hr = operation->SetOperationFlags(FOF_NO_UI | FOF_NO_CONNECTED_ELEMENTS | FOFX_EARLYFAILURE | FOFX_NOCOPYHOOKS);
        if (FAILED(result.hr)) break;
        ComPtr<IShellItem> item;
        result.hr = SHCreateItemFromParsingName(path::StripExtendedPathPrefix(source).c_str(), nullptr, IID_PPV_ARGS(&item));
        if (FAILED(result.hr)) break;
        ComPtr<DeleteSink> sink;
        sink.Attach(new (std::nothrow) DeleteSink(source, cancel, result, progress));
        if (!sink) { result.hr = E_OUTOFMEMORY; break; }
        DWORD cookie = 0;
        result.hr = operation->Advise(sink.Get(), &cookie);
        if (FAILED(result.hr)) break;
        result.hr = TraceDeleteStage(observer, DeleteStage::QueueDelete, [&] { return operation->DeleteItem(item.Get(), nullptr); });
        if (SUCCEEDED(result.hr)) {
            const HRESULT performed = TraceDeleteStage(observer, DeleteStage::PerformOperations, [&] { return operation->PerformOperations(); });
            if (FAILED(performed) && SUCCEEDED(result.hr)) result.hr = performed;
            BOOL aborted = FALSE;
            const HRESULT queried = TraceDeleteStage(observer, DeleteStage::QueryAborted, [&] { return operation->GetAnyOperationsAborted(&aborted); });
            if (FAILED(queried) && SUCCEEDED(result.hr)) result.hr = queried;
            if (aborted && SUCCEEDED(result.hr)) result.hr = HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
            const bool source_missing = Missing(source);
            if (source_missing) result.mutated = true;
            if (SUCCEEDED(result.hr) && !aborted && sink->DeferredDirectory() && source_missing &&
                (result.sources.empty() || result.sources.back() != source))
                result.sources.push_back(source);
        }
        (void)TraceDeleteStage(observer, DeleteStage::Unadvise, [&] { return operation->Unadvise(cookie); });
        result.cancelled = cancel.load() || result.hr == kCancelled || result.hr == E_ABORT || result.hr == COPYENGINE_E_USER_CANCELLED;
        if (result.cancelled && SUCCEEDED(result.hr)) result.hr = kCancelled;
        if (FAILED(result.hr) || result.cancelled) break;
        if (result.sources.empty() || result.sources.back() != source) {
            // No completion callback must never become a fabricated success.
            result.hr = E_FAIL; break;
        }
    }
    if (!cache && local_native_count > 0) {
        NotifyRecycledParents(local_parents);
        pulse::recycle_empty::NotifyShell();
        for (const auto& [bin_dir, max_bytes] : local_native_bins) {
            if (max_bytes == 0) continue;
            uint64_t used = 0, victim_bytes = 0;
            (void)native_recycle::QueueQuotaCompaction(bin_dir, max_bytes, local_protected_upper, used, victim_bytes);
        }
    }
    if (FAILED(result.hr) && !result.cancelled && result.error.empty()) result.error = ErrorText(result.hr);
    return result;
}
}

#include "ops_manager.h"
#include "elevated_delete.h"
#include "elevated_transfer_client.h"
#include "transfer_rate_estimator.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"
#include "../fs/drive_kind.h"
#include <sherrors.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <iterator>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace pulse::ops {
namespace {
bool PermissionFailure(HRESULT hr) {
    return hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_PRIVILEGE_NOT_HELD) ||
        hr == HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED) ||
        hr == static_cast<HRESULT>(0x80270021L) || hr == static_cast<HRESULT>(0x80270022L);
}
// Localized text for the failures users actually hit; the elevated helper
// has no localization and FormatMessage knows nothing about COPYENGINE_E_*.
std::wstring KnownDeleteError(HRESULT hr) {
    switch (static_cast<uint32_t>(hr)) {
    case 0x80070005u:  // E_ACCESSDENIED
    case 0x80270021u:  // COPYENGINE_E_ACCESS_DENIED_SRC
    case 0x80270022u:  // COPYENGINE_E_ACCESS_DENIED_DEST
        return l10n::Get(l10n::StringId::AccessDeniedTheItemMayBeProtectedItsPermissionsMayNotAllowDe).c_str();
    case 0x80070020u:  // ERROR_SHARING_VIOLATION
    case 0x80070021u:  // ERROR_LOCK_VIOLATION
    case 0x80270027u:  // COPYENGINE_E_SHARING_VIOLATION_SRC
    case 0x80270028u:  // COPYENGINE_E_SHARING_VIOLATION_DEST
        return l10n::Get(l10n::StringId::TheItemIsInUseByAnotherProgramCloseItAndTryAgain).c_str();
    case static_cast<uint32_t>(kRecycleBinTooSmall):
    case static_cast<uint32_t>(COPYENGINE_E_RECYCLE_SIZE_TOO_BIG):
        return l10n::Get(l10n::StringId::RecycleBinTooSmallNotDeleted).c_str();
    case static_cast<uint32_t>(COPYENGINE_E_RECYCLE_BIN_NOT_FOUND):
    case static_cast<uint32_t>(COPYENGINE_E_RECYCLE_FORCE_NUKE):
        return l10n::Get(l10n::StringId::ThisLocationCannotBeSafelySentToTheRecycleBinTheItemWasNotDe).c_str();
    case static_cast<uint32_t>(COPYENGINE_E_RECYCLE_PATH_TOO_LONG):
        return l10n::Get(l10n::StringId::RecyclePathTooLongNotDeleted).c_str();
    default:
        return {};
    }
}

// The Recycle Bin would not keep the item, so it is still there untouched:
// the Shell would have deleted it for good (#276). None for other failures.
RecycleRefusal RecycleRefusalOf(HRESULT hr, const std::wstring& source) {
    switch (static_cast<uint32_t>(hr)) {
    case static_cast<uint32_t>(kRecycleBinTooSmall):
    case static_cast<uint32_t>(COPYENGINE_E_RECYCLE_SIZE_TOO_BIG):
        return RecycleRefusal::TooLarge;
    case static_cast<uint32_t>(COPYENGINE_E_RECYCLE_FORCE_NUKE):
        return RecycleRefusal::Disabled;
    case static_cast<uint32_t>(COPYENGINE_E_RECYCLE_BIN_NOT_FOUND):
        return ReadRecycleBinLimits(source).disabled ? RecycleRefusal::Disabled : RecycleRefusal::Location;
    default:
        return RecycleRefusal::None;
    }
}

// Held open by someone for a moment: a thumbnailer, the indexer, a virus
// scan. Access denied is not one of these (see PermissionFailure).
bool InUseFailure(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION) ||
        hr == HRESULT_FROM_WIN32(ERROR_USER_MAPPED_FILE) || hr == COPYENGINE_E_SHARING_VIOLATION_SRC;
}
// Waits before an item in use is tried again; a run of failures skips them.
constexpr DWORD kInUseRetryMs[] = {100, 250, 500};
constexpr size_t kInUseRetryMaxFailuresInARow = 3;
// As for copies: a run of failures with no success between stops the task.
constexpr size_t kStopAfterFailuresInARow = 64;

// Sleeps `ms`; false when the task is cancelled meanwhile.
bool WaitUnlessCancelled(std::atomic<bool>& cancel, DWORD ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    for (ULONGLONG now = GetTickCount64(); now < until; now = GetTickCount64()) {
        if (cancel.load()) return false;
        Sleep(static_cast<DWORD>((std::min<ULONGLONG>)(until - now, 25)));
    }
    return !cancel.load();
}
std::wstring DeleteError(const ShellTransferResult& result) {
    if (auto known = KnownDeleteError(result.hr); !known.empty()) return known;
    if (!result.error.empty()) return result.error;
    if (result.hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return l10n::Get(l10n::StringId::AdministratorPermissionWasNotGrantedRetryOrSkipThisItem).c_str();
    wchar_t* text = nullptr;
    std::wstring error;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(result.hr), 0,
            reinterpret_cast<LPWSTR>(&text), 0, nullptr) && text) error = text;
    if (text) LocalFree(text);
    if (!error.empty()) return error;
    wchar_t code[16];
    swprintf_s(code, L"0x%08X", static_cast<unsigned>(result.hr));
    return std::wstring(l10n::Get(l10n::StringId::CouldNotDeleteThisItemError).c_str()) + code +
           l10n::Get(l10n::StringId::Text3072).c_str();
}

// Permanent delete without the Shell. IFileOperation spends ~20 ms per item
// on setup; a handle with POSIX delete semantics removes the name at once,
// so a folder is empty, and deletable, as soon as its children are gone.
// FILE_DISPOSITION_INFO_EX is spelled out because the Windows 8.1 build
// targets headers older than it.
constexpr auto kFileDispositionInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
constexpr DWORD kDispositionDelete = 0x1;
constexpr DWORD kDispositionPosixSemantics = 0x2;
constexpr DWORD kDispositionIgnoreReadonly = 0x10;
std::atomic<bool> g_disposition_ex_unsupported{false};

std::wstring NativePath(const std::wstring& path) {
    if (path.size() < 240 || path.starts_with(L"\\\\?\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::wstring JoinChild(const std::wstring& dir, const wchar_t* name) {
    if (!dir.empty() && dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

// ERROR_SUCCESS, or why the item is still there.
DWORD DeleteNativeItem(const std::wstring& path, DWORD attributes) {
    const std::wstring api_path = NativePath(path);
    if (!g_disposition_ex_unsupported.load(std::memory_order_relaxed)) {
        HANDLE handle = CreateFileW(api_path.c_str(), DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return GetLastError();
        struct { DWORD flags; } info{kDispositionDelete | kDispositionPosixSemantics | kDispositionIgnoreReadonly};
        const BOOL deleted = SetFileInformationByHandle(handle, kFileDispositionInfoEx, &info, sizeof(info));
        const DWORD code = deleted ? ERROR_SUCCESS : GetLastError();
        CloseHandle(handle);
        if (deleted) return ERROR_SUCCESS;
        // Before Windows 10 1809, or a file system without it (FAT, some
        // shares): use the classic calls from now on.
        if (code != ERROR_INVALID_PARAMETER && code != ERROR_NOT_SUPPORTED &&
            code != ERROR_INVALID_FUNCTION)
            return code;
        g_disposition_ex_unsupported.store(true, std::memory_order_relaxed);
    }
    if (attributes & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(api_path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    const BOOL removed = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(api_path.c_str())
                                                                 : DeleteFileW(api_path.c_str());
    if (removed) return ERROR_SUCCESS;
    const DWORD code = GetLastError();
    if (attributes & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(api_path.c_str(), attributes);
    return code;
}

// On a share every delete waits for a round trip (open, mark, close), and
// the server serves several at once: overlapping them is what makes a large
// delete fast there. A local disk gains nothing from it.
constexpr unsigned kShareDeleteThreads = 8;
constexpr size_t kOverlapMinFiles = 32;    // in one folder tree
constexpr size_t kOverlapMinSources = 8;   // selected items

bool DeletesOverlapOn(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return true;
    if (path.starts_with(L"\\\\") && !path.starts_with(L"\\\\?\\") && !path.starts_with(L"\\\\.\\"))
        return true;
    return ::pulse::fs::IsRemoteLikeDrive(path);
}

// Calls work(i) for every i below count on up to `threads` threads until one
// returns false. The calling thread only waits, calling tick() about every
// 50 ms: progress is reported from it alone. Without threads, it does the
// work itself.
template <typename Work, typename Tick>
void RunOverlapped(size_t count, unsigned threads, Work&& work, Tick&& tick) {
    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    const auto drain = [&] {
        while (!stop.load()) {
            const size_t i = next.fetch_add(1);
            if (i >= count) break;
            if (!work(i)) stop.store(true);
        }
    };
    std::mutex mutex;
    std::condition_variable finished;
    size_t running = 0;
    std::vector<std::thread> workers;
    const size_t wanted = (std::min)(static_cast<size_t>(threads), count);
    for (size_t t = 0; t < wanted; ++t) {
        {
            std::lock_guard lock(mutex);
            ++running;
        }
        try {
            workers.emplace_back([&] {
                drain();
                std::lock_guard lock(mutex);
                if (--running == 0) finished.notify_all();
            });
        } catch (const std::system_error&) {
            std::lock_guard lock(mutex);
            --running;
            break;
        }
    }
    if (workers.empty()) {
        drain();
        return;
    }
    {
        std::unique_lock lock(mutex);
        while (!finished.wait_for(lock, std::chrono::milliseconds(50), [&] { return running == 0; })) {
            lock.unlock();
            tick();
            lock.lock();
        }
    }
    for (auto& worker : workers) worker.join();
}

struct NativeDeleteOutcome {
    bool done = false;       // the source no longer exists
    bool mutated = false;    // something under it was deleted
    bool cancelled = false;
    DWORD error = ERROR_SUCCESS;  // why the item it stopped at is still there
};

bool NativeInUse(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_USER_MAPPED_FILE;
}

// Deletes one source, links as links (never entered). Stops at the first
// item it cannot delete and leaves the rest, with its error UI, to the Shell
// path. Lists the tree first so progress has a total. With threads, the
// files go first, several at once, then the folders in order.
template <typename Progress>
NativeDeleteOutcome DeleteNative(const std::wstring& source, std::atomic<bool>& cancel, Progress&& progress,
                                 unsigned threads = 1) {
    NativeDeleteOutcome outcome;
    // Never a volume root; anything that is not a plain path goes to the Shell.
    if (source.size() <= 3 || source.starts_with(L"::") || source.find(L'/') != std::wstring::npos)
        return outcome;
    const DWORD root_attributes = GetFileAttributesW(NativePath(source).c_str());
    if (root_attributes == INVALID_FILE_ATTRIBUTES) return outcome;
    struct Item { std::wstring path; DWORD attributes; };
    std::vector<Item> items;  // pre-order: every folder before its contents
    items.push_back({source, root_attributes});
    if ((root_attributes & FILE_ATTRIBUTE_DIRECTORY) && !(root_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        std::vector<size_t> pending{0};
        while (!pending.empty()) {
            if (cancel.load()) { outcome.cancelled = true; return outcome; }
            const std::wstring folder = items[pending.back()].path;
            pending.pop_back();
            WIN32_FIND_DATAW data{};
            const std::wstring pattern = JoinChild(NativePath(folder), L"*");
            HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                           nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find == INVALID_HANDLE_VALUE) {
                if (GetLastError() == ERROR_FILE_NOT_FOUND) continue;
                return outcome;  // unreadable: the Shell path reports it
            }
            do {
                const wchar_t* name = data.cFileName;
                if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
                items.push_back({JoinChild(folder, name), data.dwFileAttributes});
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    pending.push_back(items.size() - 1);
            } while (FindNextFileW(find, &data));
            const DWORD code = GetLastError();
            FindClose(find);
            if (code != ERROR_NO_MORE_FILES) return outcome;
        }
    }
    const size_t total = items.size();
    size_t removed = 0;
    bool files_removed = false;
    if (threads > 1) {
        std::vector<size_t> files;
        for (size_t i = 0; i < total; ++i)
            if (!(items[i].attributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(i);
        if (files.size() >= kOverlapMinFiles) {
            std::atomic<size_t> deleted{0};
            std::atomic<DWORD> first_error{ERROR_SUCCESS};
            RunOverlapped(files.size(), threads, [&](size_t k) {
                if (cancel.load()) return false;
                const Item& item = items[files[k]];
                const DWORD code = DeleteNativeItem(item.path, item.attributes);
                if (code == ERROR_SUCCESS || code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                    deleted.fetch_add(1);
                    return true;
                }
                DWORD none = ERROR_SUCCESS;
                first_error.compare_exchange_strong(none, code);
                return false;
            }, [&] { progress(source, static_cast<float>(deleted.load() * 100.0 / total)); });
            removed = deleted.load();
            if (removed) outcome.mutated = true;
            if (cancel.load()) { outcome.cancelled = true; return outcome; }
            if (first_error.load() != ERROR_SUCCESS) {
                outcome.error = first_error.load();
                return outcome;
            }
            files_removed = true;
        }
    }
    // Reverse pre-order: contents before their folder.
    ULONGLONG last_report = 0;
    for (size_t i = total; i-- > 0;) {
        if (files_removed && !(items[i].attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (cancel.load()) { outcome.cancelled = true; return outcome; }
        const DWORD code = DeleteNativeItem(items[i].path, items[i].attributes);
        if (code != ERROR_SUCCESS && code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
            outcome.error = code;
            return outcome;
        }
        outcome.mutated = true;
        ++removed;
        const ULONGLONG now = GetTickCount64();
        if (total > 1 && now - last_report >= 50) {
            last_report = now;
            progress(items[i].path, static_cast<float>(removed * 100.0 / total));
        }
    }
    outcome.done = true;
    return outcome;
}
}

void OpsManager::RunAuthorizedDelete(const OpRequest& req, uint64_t task_id) {
    transfer_cancel_.store(shell_cancel_requested_.load());
    transfer_pause_.store(false);
    transfer_active_.store(true);
    SetStatus([](OpStatus& status) { status.can_pause = false; });
    const bool permanent = req.type == OpType::RealDelete;
    std::vector<std::wstring> completed;
    completed.reserve(req.sources.size());
    size_t skipped = 0;
    HRESULT failure = S_OK;
    std::wstring error;
    // An item that fails no longer stops the task (as for copies): it is
    // listed, the rest go on, and the dialog can retry what failed. Items the
    // Recycle Bin refuses are left for the UI to offer a permanent delete.
    std::vector<OpFailure> item_failures;
    uint64_t failed_items = 0;
    size_t failures_in_a_row = 0;
    std::vector<std::wstring> refused;
    RecycleRefusal refusal = RecycleRefusal::None;
    bool defer_authorization = false;
    std::wstring authorization_error;
    const auto started = GetTickCount64();
    TransferRateEstimator rate;
    rate.SetEtaFloor(0.0);
    rate.Reset(started, 0);
    const auto update_rate = [&](OpStatus& status) {
        // Only confirmed source deletions count, including on the elevated path.
        // Skipped sources reduce remaining work without inflating the delete rate.
        rate.Observe(GetTickCount64(), completed.size(), req.sources.size() - skipped);
        status.items_per_second = rate.speed();
        status.peak_items_per_second = (std::max)(status.peak_items_per_second,
                                                status.items_per_second);
        status.eta_seconds = rate.eta_seconds();
    };
    // Set while the authorization view is up; leaving it is shown at once,
    // ordinary per-item progress is throttled.
    bool authorization_shown = false;
    RecycleCache recycle_cache;
    // On a share, several selected items are deleted at once first. Only
    // what that pass deleted is taken from it; anything else is attempted
    // again below, in order, with its retries, prompts and errors as before.
    enum : char { kNotAhead, kMutatedAhead, kDeletedAhead };
    const unsigned overlap = permanent && !req.sources.empty() && DeletesOverlapOn(req.sources.front())
        ? kShareDeleteThreads : 1;
    std::vector<char> ahead(req.sources.size(), kNotAhead);
    if (overlap > 1 && req.sources.size() >= kOverlapMinSources && !transfer_cancel_.load()) {
        std::atomic<size_t> deleted_ahead{0};
        RunOverlapped(req.sources.size(), overlap, [&](size_t i) {
            if (transfer_cancel_.load()) return false;
            const auto native = DeleteNative(req.sources[i], transfer_cancel_, [](const std::wstring&, float) {});
            if (native.done) {
                ahead[i] = kDeletedAhead;
                deleted_ahead.fetch_add(1);
            } else if (native.mutated) {
                ahead[i] = kMutatedAhead;
            }
            return true;
        }, [&] {
            const size_t done = deleted_ahead.load();
            SetProgressStatus([&](OpStatus& status) {
                status.phase = OpPhase::Running;
                status.current_item = req.sources[(std::min)(done, req.sources.size() - 1)];
                status.percent = static_cast<float>(done * 100.0 / req.sources.size());
            });
        });
    }
    for (size_t index = 0; index < req.sources.size(); ++index) {
        const std::wstring& source = req.sources[index];
        if (transfer_cancel_.load()) break;
        SetProgressStatus([&](OpStatus& status) {
            status.current_item = source;
            status.last_error.clear();
            status.phase = OpPhase::Running;
        });
        // After "Retry", stay on the authorization view until the attempt
        // actually deletes something or ends; the helper's "ready" signal and
        // its initial 0% progress otherwise flash the progress/estimate view
        // just before the same error brings the authorization view back.
        bool retrying = false;
        ElevatedTransferCallbacks callbacks;
        callbacks.authorization = [&](bool awaiting) {
            if (retrying && !awaiting) return;
            authorization_shown = awaiting;
            SetStatus([&](OpStatus& status) {
                status.authorization = awaiting ? AuthorizationState::Requesting : AuthorizationState::None;
                status.authorization_retrying = false;
                status.summary = awaiting
                    ? l10n::Get(l10n::StringId::RequestingAdministratorPermission).c_str()
                    : l10n::Get(l10n::StringId::Deleting).c_str();
            });
        };
        auto progress = [&](const std::wstring& item, float percent) {
            if (retrying) {
                if (percent <= 0) return;
                retrying = false;
            }
            const auto update = [&](OpStatus& status) {
                status.authorization = AuthorizationState::None;
                status.authorization_retrying = false;
                status.current_item = item.empty() ? source : item;
                status.completed_items = completed.size();
                status.percent = req.sources.empty() ? 0.0f :
                    static_cast<float>((completed.size() + skipped + failed_items + refused.size() +
                                        (percent < 0 ? 0 : percent / 100.0)) * 100.0 / req.sources.size());
                update_rate(status);
            };
            if (authorization_shown) { authorization_shown = false; SetStatus(update); }
            else SetProgressStatus(update);
        };
        callbacks.progress = [&](const OpStatus& status) { progress(status.current_item, status.percent); };
        ShellTransferResult result;
        bool native_mutated = false;
        size_t in_use_retries = 0;
        if (permanent) {
            NativeDeleteOutcome native;
            if (ahead[index] == kDeletedAhead) {
                native.done = native.mutated = true;
            } else {
                native_mutated = ahead[index] == kMutatedAhead;
                native = DeleteNative(source, transfer_cancel_, progress, overlap);
            }
            ahead[index] = kNotAhead;
            // In use for a moment: tried again here, at once, rather than by
            // the Shell, whose attempt takes about a second on an item held.
            while (!native.done && !native.cancelled && NativeInUse(native.error) &&
                   in_use_retries < std::size(kInUseRetryMs) && failures_in_a_row < kInUseRetryMaxFailuresInARow) {
                native_mutated = native_mutated || native.mutated;
                if (!WaitUnlessCancelled(transfer_cancel_, kInUseRetryMs[in_use_retries++])) {
                    native.cancelled = true;
                    break;
                }
                native = DeleteNative(source, transfer_cancel_, progress, overlap);
            }
            native_mutated = native_mutated || native.mutated;
            if (native.done || native.cancelled) {
                result.mutated = native.mutated;
                if (native.done) result.sources = {source};
                if (native.cancelled) {
                    result.cancelled = true;
                    result.hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                }
            }
        }
        // What the native pass could not delete takes the Shell path, which
        // brings the elevation and in-use prompts.
        const bool handled = !result.sources.empty() || result.cancelled;
        // The operation itself checks access. Probing each source and its parent
        // first duplicates file-system work for every ordinary recycle. A denied
        // attempt with no mutation still enters the existing elevation path.
        bool authorized = false;
        for (; !handled;) {
            if (authorized && defer_authorization) {
                const auto choice = WaitForAuthorization(task_id, authorization_error, source);
                if (choice == AuthorizationChoice::Cancel) break;
                if (choice == AuthorizationChoice::Skip) { ++skipped; result = ShellTransferResult{}; break; }
                defer_authorization = false;
            }
            result = authorized
                ? DeleteWithElevatedHelper({source}, permanent, UiWindow(), transfer_cancel_, callbacks)
                : DeleteWithCurrentToken({source}, permanent, UiWindow(), transfer_cancel_, progress,
                                         permanent ? nullptr : &recycle_cache);
            if (!authorized && !result.mutated && PermissionFailure(result.hr) && !transfer_cancel_.load()) {
                authorized = true;
                continue;
            }
            if (!authorized && !result.mutated && !result.cancelled && InUseFailure(result.hr) &&
                in_use_retries < std::size(kInUseRetryMs) && failures_in_a_row < kInUseRetryMaxFailuresInARow) {
                if (!WaitUnlessCancelled(transfer_cancel_, kInUseRetryMs[in_use_retries++])) break;
                continue;
            }
            if (SUCCEEDED(result.hr) || result.mutated || transfer_cancel_.load() || !authorized) break;
            authorization_error = DeleteError(result);
            const auto choice = WaitForAuthorization(task_id, authorization_error, source);
            if (choice == AuthorizationChoice::Retry) { retrying = true; continue; }
            if (choice == AuthorizationChoice::Skip) {
                ++skipped;
                defer_authorization = true;
                result = ShellTransferResult{};
            }
            break;
        }
        if (native_mutated) result.mutated = true;
        completed.insert(completed.end(), result.sources.begin(), result.sources.end());
        const auto finish_item = [&](OpStatus& status) {
            // A retried attempt on a reused elevated session never invokes the
            // authorization callback, so leave the Requesting view explicitly.
            status.authorization = AuthorizationState::None;
            status.authorization_retrying = false;
            status.completed_items = completed.size();
            update_rate(status);
        };
        if (!authorization_shown && !authorized) SetProgressStatus(finish_item);
        else { authorization_shown = false; SetStatus(finish_item); }
        // Also when a partly deleted item was then skipped.
        if (result.mutated && (FAILED(result.hr) || result.cancelled || result.sources.empty())) {
        CompletedOperation refresh;
        refresh.task_id = task_id;
            refresh.type = req.type;
            refresh.sources = {source};
            refresh.refresh_only = true;
            std::lock_guard lock(mutex_);
            completions_.push_back(std::move(refresh));
        }
        if (result.cancelled && !PermissionFailure(result.hr)) transfer_cancel_.store(true);
        if (transfer_cancel_.load()) break;
        if (SUCCEEDED(result.hr)) {
            failures_in_a_row = 0;
            continue;
        }
        if (!permanent && !result.mutated) {
            if (const RecycleRefusal why = RecycleRefusalOf(result.hr, source); why != RecycleRefusal::None) {
                refused.push_back(source);
                // One reason covers them all, as DeletePaths asks it.
                if (why == RecycleRefusal::Location || refusal == RecycleRefusal::None ||
                    (why == RecycleRefusal::Disabled && refusal == RecycleRefusal::TooLarge))
                    refusal = why;
                continue;
            }
        }
        if (IsStopTransferError(result.hr) || ++failures_in_a_row >= kStopAfterFailuresInARow) {
            failure = result.hr;
            error = DeleteError(result);
            break;
        }
        if (item_failures.size() < kMaxReportedFailures)
            item_failures.push_back({source, {}, DeleteError(result), result.hr});
        ++failed_items;
    }
    // What the overlapped pass deleted or changed beyond where the loop
    // stopped is reported all the same.
    for (size_t index = 0; index < ahead.size(); ++index) {
        if (ahead[index] == kDeletedAhead) {
            completed.push_back(req.sources[index]);
        } else if (ahead[index] == kMutatedAhead) {
            CompletedOperation refresh;
            refresh.task_id = task_id;
            refresh.type = req.type;
            refresh.sources = {req.sources[index]};
            refresh.refresh_only = true;
            std::lock_guard lock(mutex_);
            completions_.push_back(std::move(refresh));
        }
    }
    if (!completed.empty()) {
        OpRequest finished = req;
        finished.sources = completed;
        PushUndo(finished, task_id);
    }
    const bool cancelled = transfer_cancel_.load();
    // Items held open by another program say by which (a bounded number:
    // each probe asks Restart Manager).
    if (!cancelled) {
        size_t probed = 0;
        for (auto& failed : item_failures) {
            if (probed >= 16) break;
            if (!IsLockLikeError(failed.hr)) continue;
            ++probed;
            const auto owners = FindLockOwners(LockProbeFiles(failed.source, 16), CurrentModuleDirectory());
            if (owners.empty()) continue;
            const LockOwner& owner = owners.front();
            const std::wstring name = owner.app_name.empty() ? owner.image_name : owner.app_name;
            if (name.empty()) continue;
            const std::wstring format = l10n::Get(l10n::StringId::TransferInUseBy).c_str();
            if (format.find(L"%s") == std::wstring::npos) continue;
            wchar_t text[512];
            swprintf_s(text, format.c_str(), name.c_str());
            failed.reason = text;
        }
    }
    // One failed item and it is in use: the file-in-use dialog, as when the
    // task stopped there; its retry covers just that item.
    LockReport lock_report;
    if (FAILED(failure) && !cancelled) {
        lock_report = ProbeLock(req, task_id, failure, error);
    } else if (!cancelled && failed_items == 1 && item_failures.size() == 1 &&
               IsLockLikeError(item_failures.front().hr)) {
        OpRequest narrowed = req;
        narrowed.sources = {item_failures.front().source};
        lock_report = ProbeLock(narrowed, task_id, item_failures.front().hr,
                                item_failures.front().reason + L" | " + item_failures.front().source);
    }
    if (failed_items > 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_retries_.push_back(FailedItemsRetry{task_id, req, item_failures});
        while (failed_retries_.size() > 8) failed_retries_.pop_front();
    }
    // The worker publishes them with the finished task.
    recycle_refused_ = refused;
    recycle_refusal_ = refusal;
    skipped += refused.size();
    diagnostics::runtime::Event("file_delete_result", {{"task", task_id}, {"permanent", permanent},
        {"hresult", static_cast<uint32_t>(failure)}, {"cancelled", cancelled},
        {"completed", completed.size()}, {"skipped", skipped}, {"failed", failed_items},
        {"refused", refused.size()}, {"elapsed_ms", GetTickCount64() - started}});
    SetStatus([&](OpStatus& status) {
        status.active = false;
        status.authorization = AuthorizationState::None;
        status.can_skip_authorization = false;
        status.can_pause = false;
        ++status.completed_ops;
        status.completed_items = completed.size();
        status.bytes_per_second = 0;
        status.items_per_second = 0;
        status.eta_seconds = 0;
        status.locked_path = lock_report.path;
        status.lock_owners = lock_report.owners;
        status.failures = item_failures;
        status.failed_items = failed_items;
        if (cancelled || FAILED(failure)) {
            status.phase = OpPhase::Failed;
            status.last_error = cancelled ? l10n::Get(l10n::StringId::Canceled).c_str() : error;
            status.summary = status.last_error;
        } else if (failed_items > 0) {
            // The rest went through; the dialog lists what did not.
            status.phase = OpPhase::Failed;
            status.percent = 100;
            status.last_error = item_failures.front().reason + L" | " + item_failures.front().source;
            wchar_t text[128];
            swprintf_s(text, l10n::Get(l10n::StringId::TransferItemsFailed).c_str(),
                       static_cast<int>((std::min<uint64_t>)(failed_items, INT_MAX)));
            status.summary = text;
        } else {
            status.phase = OpPhase::Completed;
            status.percent = 100;
            status.last_error.clear();
            status.summary = skipped ? l10n::Get(l10n::StringId::DeletionFinishedSkipped).c_str() + std::to_wstring(skipped)
                                     : l10n::Get(l10n::StringId::DeletionCompleted).c_str();
        }
    });
    transfer_active_.store(false);
}
}
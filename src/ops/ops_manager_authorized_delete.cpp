#include "ops_manager.h"
#include "native_delete_stream.h"
#include "elevated_delete.h"
#include "elevated_transfer_client.h"
#include "transfer_rate_estimator.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"
#include "../common/delete_diagnostics.h"
#include "../fs/drive_kind.h"
#include <sherrors.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <iterator>
#include <mutex>
#include <vector>
#include <unordered_set>

namespace pulse::ops {
namespace {
using native_delete::DeleteNative;
using native_delete::NativeDeleteOutcome;
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
bool NativeInUse(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_USER_MAPPED_FILE;
}


}

void OpsManager::RunAuthorizedDelete(const OpRequest& req, uint64_t task_id) {
    transfer_cancel_.store(shell_cancel_requested_.load());
    transfer_pause_.store(false);
    transfer_active_.store(true);
    SetStatus([](OpStatus& status) { status.can_pause = false; });
    const bool permanent = req.type == OpType::RealDelete;
    std::vector<std::wstring> completed;
    std::unordered_set<std::wstring> completed_roots;
    completed.reserve(req.sources.size());
    size_t skipped = 0;
    HRESULT failure = S_OK;
    std::wstring error;
    // An item that fails no longer stops the task (as for copies): it is
    // listed, the rest go on, and the dialog can retry what failed. Items the
    // Recycle Bin refuses are left for the UI to offer a permanent delete.
    std::vector<OpFailure> item_failures;
    bool request_replay_unsafe = false;
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
    for (size_t index = 0; index < req.sources.size(); ++index) {
        const std::wstring& source = req.sources[index];
        if (transfer_cancel_.load()) break;
        if (permanent) {
            bool covered = completed_roots.contains(source);
            for (size_t boundary = source.find_last_of(L'\\'); !covered && boundary != std::wstring::npos;) {
                covered = completed_roots.contains(source.substr(0, boundary)) ||
                          completed_roots.contains(source.substr(0, boundary + 1));
                if (boundary == 0) break;
                boundary = source.find_last_of(L'\\', boundary - 1);
            }
            if (covered && GetFileAttributesW(native_delete::NativePath(source).c_str()) == INVALID_FILE_ATTRIBUTES) {
                const DWORD code = GetLastError();
                if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                    ++skipped;
                    continue;
                }
            }
        }
        if (transfer_cancel_.load()) break;
        const auto current_item = item_control_.Begin(task_id, transfer_cancel_);
        diagnostics::runtime::Event("operation_item_begin", {{"task", task_id}, {"token", current_item->token},
            {"type", static_cast<uint64_t>(req.type)}, {"index", index}});
        SetStatus([&](OpStatus& status) {
            status.current_item_token = current_item->token;
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
                status.current_item = source; // The skip target is this selected root, not a changing child.
                (void)item;
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
        bool native_terminal = false;
        size_t in_use_retries = 0;
        if (permanent) {
            const bool remote =
                (source.starts_with(L"\\\\") && !source.starts_with(L"\\\\?\\")) ||
                source.starts_with(L"\\\\?\\UNC\\") || ::pulse::fs::IsRemoteLikeDrive(source);
            const unsigned workers = remote ? 8u : 1u;
            diagnostics::runtime::Event("operation_delete_backend_begin", {{"task", task_id}, {"token", current_item->token}, {"backend", 0}});
            NativeDeleteOutcome native = DeleteNative(source, current_item->cancel, progress, workers);
            diagnostics::runtime::Event("operation_delete_backend_end", {{"task", task_id}, {"token", current_item->token},
                {"backend", 0}, {"error", native.error}, {"cancelled", native.cancelled}, {"mutated", native.mutated}});
            // In use for a moment: tried again here, at once, rather than by
            // the Shell, whose attempt takes about a second on an item held.
            while (!native.done && !native.cancelled && !native.enumeration_failed && !native.mutated && !native_mutated && NativeInUse(native.error) &&
                   in_use_retries < std::size(kInUseRetryMs) && failures_in_a_row < kInUseRetryMaxFailuresInARow) {
                native_mutated = native_mutated || native.mutated;
                if (!WaitUnlessCancelled(current_item->cancel, kInUseRetryMs[in_use_retries++])) {
                    native.cancelled = true;
                    break;
                }
                native = DeleteNative(source, current_item->cancel, progress, workers);
            }
            native_mutated = native_mutated || native.mutated;
            native_terminal = native.mutated || native.enumeration_failed || native.cancelled;
            request_replay_unsafe = request_replay_unsafe || (!native.done && native_terminal);
            if (native.done || native_terminal) {
                if (!native.done && native.error != ERROR_SUCCESS) result.hr = HRESULT_FROM_WIN32(native.error);
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
        const bool handled = !result.sources.empty() || result.cancelled || native_terminal;
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
            diagnostics::runtime::Event("operation_delete_backend_begin", {{"task", task_id}, {"token", current_item->token}, {"backend", authorized ? 2u : 1u}});
            result = authorized
                ? DeleteWithElevatedHelper({source}, permanent, UiWindow(), current_item->cancel, callbacks)
                : DeleteWithCurrentToken({source}, permanent, UiWindow(), current_item->cancel, progress,
                                         permanent ? nullptr : &recycle_cache);
            diagnostics::runtime::Event("operation_delete_backend_end", {{"task", task_id}, {"token", current_item->token},
                {"backend", authorized ? 2u : 1u}, {"hr", static_cast<uint32_t>(result.hr)},
                {"cancelled", result.cancelled}, {"mutated", result.mutated}});
            if (!authorized && !result.mutated && PermissionFailure(result.hr) && !current_item->cancel.load()) {
                authorized = true;
                continue;
            }
            if (!authorized && !result.mutated && !result.cancelled && InUseFailure(result.hr) &&
                in_use_retries < std::size(kInUseRetryMs) && failures_in_a_row < kInUseRetryMaxFailuresInARow) {
                if (!WaitUnlessCancelled(current_item->cancel, kInUseRetryMs[in_use_retries++])) break;
                continue;
            }
            if (SUCCEEDED(result.hr) || result.mutated || current_item->cancel.load() || !authorized) break;
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
        const bool item_skipped = item_control_.Finish(current_item);
        SetStatus([](OpStatus& status) { status.current_item_token = 0; });
        diagnostics::runtime::Event("operation_item_end", {{"task", task_id}, {"token", current_item->token},
            {"hr", static_cast<uint32_t>(result.hr)}, {"skip", item_skipped}, {"cancel", transfer_cancel_.load()},
            {"mutated", result.mutated || native_mutated}});
        if (native_mutated) result.mutated = true;
        diagnostics::deletion::Item(task_id, index, source, result.hr,
            std::any_of(result.sources.begin(), result.sources.end(), [&](const auto& actual) {
                return diagnostics::deletion::Key(actual) == diagnostics::deletion::Key(source);
            }), result.cancelled, result.mutated);
        completed.insert(completed.end(), result.sources.begin(), result.sources.end());
        if (permanent) completed_roots.insert(result.sources.begin(), result.sources.end());
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
        if (item_skipped && !transfer_cancel_.load()) {
            if (result.sources.empty()) ++skipped;
            continue;
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
    if (!completed.empty()) {
        OpRequest finished = req;
        finished.sources = completed;
        PushUndo(finished, task_id);
    }
    const bool cancelled = transfer_cancel_.load();
    if (req.sources.size() > diagnostics::deletion::kItemLimit)
        diagnostics::runtime::Event("file_delete_diagnostics_limit", {{"task", task_id},
            {"sources", req.sources.size()}, {"item_limit", diagnostics::deletion::kItemLimit}});
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
    if (FAILED(failure) && !cancelled && !request_replay_unsafe) {
        lock_report = ProbeLock(req, task_id, failure, error);
    } else if (!cancelled && !request_replay_unsafe && failed_items == 1 && item_failures.size() == 1 &&
               IsLockLikeError(item_failures.front().hr)) {
        OpRequest narrowed = req;
        narrowed.sources = {item_failures.front().source};
        lock_report = ProbeLock(narrowed, task_id, item_failures.front().hr,
                                item_failures.front().reason + L" | " + item_failures.front().source);
    }
    if (failed_items > 0 && !request_replay_unsafe) {
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

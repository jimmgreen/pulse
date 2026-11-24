#include "content_instant_session.h"
#include "content_scope.h"
#include "document_reader.h"
#include "index_feed.h"
#include "../fs/fs_watch.h"
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace pulse::index {
namespace {
bool Under(const std::wstring& path, const std::wstring& root) {
    const auto p = ContentScopeKey(path), r = ContentScopeKey(root);
    return p == r || (p.starts_with(r) && (r.ends_with(L'\\') || (p.size() > r.size() && p[r.size()] == L'\\')));
}
struct Changes {
    struct Change { std::wstring path; bool imported = false; };
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::mutex mutex;
    std::condition_variable wake;
    std::map<std::wstring, Change> pending;
    DWORD error = 0;
    ContentSubscriptionFailure failure = ContentSubscriptionFailure::None;
    uint32_t failure_mask = 0;
    uint64_t dropped = 0;
    bool sealed = false, initial_complete = false, failed_during_scan = false;
    void Fail(ContentSubscriptionFailure source, DWORD code) {
        if (sealed) return;
        failure_mask |= 1u << static_cast<uint32_t>(source);
        if (!error) { error = code; failure = source; failed_during_scan = !initial_complete; }
    }
    ~Changes() { CloseHandle(stop); }
};
void LogSubscriptionFailure(const Changes& state, uint64_t generation, size_t pending) noexcept {
    try {
        const char* names[]{"none", "queue_overflow", "feed_connect", "feed_not_ready", "feed_gap",
            "watch_overflow", "watch_not_ready", "transport", "metadata", "directory_enumeration", "reconcile"};
        wchar_t directory[32768]{};
        DWORD count = GetEnvironmentVariableW(L"PULSE_CONTENT_TIMING_DIR", directory, ARRAYSIZE(directory));
        std::filesystem::path root;
        if (count && count < ARRAYSIZE(directory)) root = directory;
        else {
            count = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, ARRAYSIZE(directory));
            if (!count || count >= ARRAYSIZE(directory)) return;
            root = std::filesystem::path(directory) / L"Pulse" / L"logs";
        }
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (error) return;
        const auto path = root / (L"content-subscription-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
        SYSTEMTIME utc{}; GetSystemTime(&utc);
        char record[768]{};
        const int size = sprintf_s(record,
            "{\"event\":\"subscription_paused\",\"utc\":\"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\","
            "\"pid\":%lu,\"generation\":%llu,\"reason\":\"%s\",\"sources\":%u,\"error\":%lu,"
            "\"during_initial_scan\":%s,\"known_pending\":%zu,\"dropped\":%llu}\n",
            utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds,
            GetCurrentProcessId(), generation, names[static_cast<size_t>(state.failure)], state.failure_mask,
            state.error, state.failed_during_scan ? "true" : "false", pending, state.dropped);
        if (size <= 0) return;
        static std::mutex sink_mutex;
        std::lock_guard lock(sink_mutex);
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER length{}, end{};
        if (GetFileSizeEx(file, &length) && length.QuadPart + size > 1024 * 1024) {
            CloseHandle(file);
            if (!MoveFileExW(path.c_str(), (path.wstring() + L".1").c_str(), MOVEFILE_REPLACE_EXISTING)) return;
            file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return;
        }
        if (SetFilePointerEx(file, end, nullptr, FILE_END)) {
            DWORD written = 0; WriteFile(file, record, static_cast<DWORD>(size), &written, nullptr);
        }
        CloseHandle(file);
    } catch (...) {}
}
}
bool RunInstantContentSession(ContentIndex& index, ContentSearchRequest request,
    const std::atomic<bool>& cancelled, ContentBatchCallback callback, const ContentInstantSessionHooks* hooks) {
    const auto config = index.Configuration();
    Changes changes;
    if (!changes.stop) return false;
    std::jthread monitor([&](std::stop_token stopped) {
        while (!stopped.stop_requested() && !cancelled) {
            if (WaitForSingleObject(changes.stop, 25) == WAIT_OBJECT_0) return;
        }
        SetEvent(changes.stop); changes.wake.notify_all();
    });
    const bool uncovered_scope = request.allow_uncovered_root && !request.root.empty() &&
        std::none_of(config.roots.begin(), config.roots.end(), [&](const auto& root) {
            return Under(request.root, root.path);
        });
    const auto in_root = [&](const std::wstring& path) {
        return std::any_of(config.roots.begin(), config.roots.end(),
                           [&](const auto& root) { return Under(path, root.path); }) ||
            (uncovered_scope && Under(path, request.root));
    };
    const auto included = [&](const std::wstring& path) {
        return !path.empty() && !IsInternalContentPath(path) && !ContentPathExcluded(config, path) &&
            in_root(path) &&
            (request.root.empty() ? request.roots.empty() ||
                std::any_of(request.roots.begin(), request.roots.end(), [&](const auto& root) { return Under(path, root); })
                : Under(path, request.root));
    };
    auto enqueue = [&](const std::wstring& path, bool imported = false) {
        const bool ancestor = !request.root.empty() && Under(request.root, path) && in_root(path);
        if (path.empty() || IsInternalContentPath(path) || ContentPathExcluded(config, path) || (!included(path) && !ancestor)) return;
        std::lock_guard lock(changes.mutex);
        if (changes.sealed) return;
        const auto key = ContentScopeKey(path);
        const auto found = changes.pending.find(key);
        if (found != changes.pending.end()) { found->second.path = path; found->second.imported |= imported; }
        else if (changes.pending.size() >= 4096) {
            ++changes.dropped; changes.Fail(ContentSubscriptionFailure::QueueOverflow, ERROR_NOTIFY_ENUM_DIR);
        } else changes.pending.emplace(key, Changes::Change{path, imported});
        changes.wake.notify_all();
    };
    auto fail = [&](ContentSubscriptionFailure source, DWORD error = ERROR_NOTIFY_ENUM_DIR) {
        std::lock_guard lock(changes.mutex);
        changes.Fail(source, error); changes.wake.notify_all();
    };
    // The subscription is armed before the initial scan, so changes during it cannot fall into a gap.
    IndexFeedConnection feed(changes.stop);
    FileFeedPage initial;
    uint64_t epoch = 0, cursor = 0;
    if (request.subscribe && hooks && hooks->arm) hooks->arm(enqueue, fail);
    if (request.subscribe && !hooks && config.shared_scope && !uncovered_scope) {
        const bool connected = feed.Request(true, L"", 0, 0, initial);
        if (connected && initial.ready && !initial.gap) {
            epoch = initial.epoch; cursor = initial.next;
        } else fail(!connected ? ContentSubscriptionFailure::FeedConnect : initial.gap ?
            ContentSubscriptionFailure::FeedGap : ContentSubscriptionFailure::FeedNotReady);
    }
    std::vector<std::unique_ptr<fs::DirWatch>> watches;
    const auto start_dir_watch = [&](const std::wstring& watch_root) {
        auto watch = std::make_unique<fs::DirWatch>();
        auto armed = std::make_shared<std::atomic<bool>>(false);
        watch->Start(watch_root, [&, base = watch_root, armed](bool overflow, auto events) {
            // Recursive DirWatch publishes its initial arming as a resync notification.
            // Wait for that barrier before scanning; later resyncs are real gaps.
            if (overflow && !armed->exchange(true)) return;
            if (overflow) fail(ContentSubscriptionFailure::WatchOverflow);
            for (const auto& event : events) {
                if (!event.name.empty()) enqueue(base + L"\\" + event.name,
                    event.action == FILE_ACTION_ADDED || event.action == FILE_ACTION_RENAMED_NEW_NAME);
                if (!event.old_name.empty()) enqueue(base + L"\\" + event.old_name);
            }
        }, true);
        const auto until = GetTickCount64() + 2000;
        while (!cancelled && (!armed->load() || !watch->Armed()) && GetTickCount64() < until) Sleep(10);
        if (!armed->load() || !watch->Armed()) fail(ContentSubscriptionFailure::WatchNotReady);
        watches.push_back(std::move(watch));
    };
    if (request.subscribe && !hooks) {
        if (uncovered_scope) start_dir_watch(request.root);
        else if (!config.shared_scope) for (const auto& root : config.roots) start_dir_watch(root.path);
    }
    std::jthread collector;
    if (request.subscribe && !hooks && config.shared_scope && !uncovered_scope && epoch) collector = std::jthread([&] {
        while (WaitForSingleObject(changes.stop, 0) != WAIT_OBJECT_0) {
            FileFeedPage page;
            const bool connected = feed.Request(true, L"", epoch, cursor, page);
            if (!connected || !page.ready || page.gap) {
                if (WaitForSingleObject(changes.stop, 0) != WAIT_OBJECT_0)
                    fail(!connected ? ContentSubscriptionFailure::FeedConnect : page.gap ?
                        ContentSubscriptionFailure::FeedGap : ContentSubscriptionFailure::FeedNotReady);
                return;
            }
            for (const auto& event : page.records) {
                enqueue(event.path, event.kind == ChangeKind::Created || event.kind == ChangeKind::MovedIn || event.kind == ChangeKind::Renamed);
                enqueue(event.old_path);
            }
            cursor = page.next;
            if (page.done && WaitForSingleObject(changes.stop, 150) == WAIT_OBJECT_0) return;
        }
    });
    struct Stop {
        Changes& state;
        ~Stop() { SetEvent(state.stop); state.wake.notify_all(); }
    } stop{changes};
    std::map<std::wstring, std::wstring> matched;
    request.task_scan = true; request.after_revision = 0; request.incremental = false;
    bool accepted = true;
    ContentSearchProgress final;
    const auto search = [&](const ContentSearchRequest& query, ContentBatchCallback deliver) {
        return hooks && hooks->search ? hooks->search(query, cancelled, std::move(deliver)) :
            index.SearchTask(query, cancelled, std::move(deliver));
    };
    const auto attributes = [&](const std::wstring& path) {
        return hooks && hooks->attributes ? hooks->attributes(path) : GetFileAttributesW(path.c_str());
    };
    const auto list_directory = [&](const std::wstring& path, std::vector<std::wstring>& children) {
        if (hooks && hooks->list_directory) return hooks->list_directory(path, children);
        std::error_code error;
        std::filesystem::directory_iterator it(path, error), end;
        for (; !error && it != end && !cancelled; it.increment(error)) children.push_back(it->path().wstring());
        return cancelled ? DWORD{ERROR_CANCELLED} : static_cast<DWORD>(error.value());
    };
    const bool ok = search(request, [&](auto progress, auto hits) {
        for (const auto& hit : hits) matched[ContentScopeKey(hit.path)] = hit.path;
        progress.live = request.subscribe;
        final = progress;
        if (progress.done) { std::lock_guard lock(changes.mutex); changes.initial_complete = true; }
        accepted = callback(progress, std::move(hits)); return accepted;
    });
    { std::lock_guard lock(changes.mutex); changes.initial_complete = true; }
    if (!ok || !accepted || cancelled || !request.subscribe) return ok && accepted;
    while (!cancelled) {
        std::map<std::wstring, Changes::Change> pending;
        DWORD gap = 0;
        {
            std::unique_lock lock(changes.mutex);
            changes.wake.wait(lock, [&] { return cancelled || changes.error || !changes.pending.empty(); });
            if (cancelled) break;
            // Coalesce bursts from an editor's temporary-file/rename sequence.
            changes.wake.wait_for(lock, std::chrono::milliseconds(150), [&] { return cancelled.load(); });
            pending.swap(changes.pending); gap = changes.error;
            if (gap) changes.sealed = true;
        }
        if (gap) {
            // Freeze producers, then reconcile the paths we still know before
            // reporting unknown coverage. Never silently restart a full scan.
            SetEvent(changes.stop);
            watches.clear();
            if (collector.joinable()) collector.join();
        }
        std::set<std::wstring> files;
        std::vector<ContentHit> removed;
        for (const auto& [key, change] : pending) {
            if (cancelled) break;
            const auto& path = change.path;
            bool reconciled = false, missing = false;
            DWORD error = ERROR_SUCCESS;
            auto stage = ContentSubscriptionFailure::Metadata;
            std::set<std::wstring> discovered;
            // One notification must survive a transient metadata or directory
            // failure. Keep retries bounded and never publish a partial listing.
            for (unsigned attempt = 0; attempt < 3 && !cancelled; ++attempt) {
                discovered.clear(); error = ERROR_SUCCESS; missing = false;
                stage = ContentSubscriptionFailure::Metadata;
                const DWORD attr = attributes(path);
                if (attr == INVALID_FILE_ATTRIBUTES) {
                    error = GetLastError();
                    missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
                    if (missing) error = ERROR_SUCCESS;
                    else if (!error) error = ERROR_GEN_FAILURE;
                } else if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) discovered.insert(path);
                    else if (change.imported) {
                        std::vector<std::wstring> directories{path};
                        while (!directories.empty() && !error && !cancelled) {
                            auto directory = std::move(directories.back()); directories.pop_back();
                            std::vector<std::wstring> children;
                            error = list_directory(directory, children);
                            if (error) { stage = ContentSubscriptionFailure::DirectoryEnumeration; break; }
                            for (const auto& child : children) {
                                if (cancelled) break;
                                if (!included(child)) continue;
                                const DWORD child_attr = attributes(child);
                                if (child_attr == INVALID_FILE_ATTRIBUTES) {
                                    error = GetLastError();
                                    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { error = 0; continue; }
                                    if (!error) error = ERROR_GEN_FAILURE;
                                    stage = ContentSubscriptionFailure::Metadata; break;
                                }
                                if (child_attr & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                                if (child_attr & FILE_ATTRIBUTE_DIRECTORY) directories.push_back(child);
                                else discovered.insert(child);
                            }
                        }
                    }
                }
                if (!error) { reconciled = true; break; }
                {
                    std::lock_guard lock(changes.mutex);
                    if (changes.error) break;
                }
                if (attempt == 2 || WaitForSingleObject(changes.stop, 150 * (attempt + 1)) != WAIT_TIMEOUT) break;
            }
            if (cancelled) break;
            if (!reconciled) { fail(stage, error); continue; }
            if (missing) {
                for (auto it = matched.begin(); it != matched.end();) {
                    if (Under(it->second, path)) { ContentHit hit; hit.path = it->second; hit.removed = true; removed.push_back(std::move(hit)); it = matched.erase(it); }
                    else ++it;
                }
            } else files.insert(discovered.begin(), discovered.end());
        }
        if (!removed.empty()) {
            auto progress = final; progress.delta = true; progress.error = 0; progress.done = true;
            if (!callback(progress, std::move(removed))) return false;
        }
        for (const auto& path : files) {
            if (cancelled) break;
            const auto extension = std::filesystem::path(path).extension().wstring();
            if (!IsIndexedContentExtension(extension) || !included(path)) continue;
            auto delta = request; delta.subscribe = false; delta.candidate_paths = {path};
            std::vector<ContentHit> hits;
            ContentSearchProgress progress;
            search(delta, [&](const auto& state, auto batch) {
                progress = state;
                hits.insert(hits.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
                return !cancelled;
            });
            if (cancelled) break;
            const auto key = ContentScopeKey(path);
            if (!progress.error && hits.empty() && matched.erase(key)) {
                ContentHit hit; hit.path = path; hit.removed = true; hits.push_back(std::move(hit));
            }
            for (const auto& hit : hits) if (!hit.removed) matched[ContentScopeKey(hit.path)] = hit.path;
            progress.delta = true; progress.live = true; progress.done = true;
            if (!callback(progress, std::move(hits))) return false;
        }
        if (gap && !cancelled) {
            LogSubscriptionFailure(changes, request.generation, pending.size());
            final.delta = true; final.done = true; final.live = false; final.error = 0;
            final.subscription_error = gap; final.subscription_failure = changes.failure;
            callback(final, {}); return false;
        }
    }
    return false;
}
}

#include "content_task_search.h"
#include "../fs/cloud_state.h"
#include "content_directory_listing.h"
#include "content_scope.h"
#include "document_reader.h"
#include "text_task_reader.h"
#include "index_feed.h"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <set>
#include <array>
#include <map>
#include "search_trace.h"

namespace pulse::index {
namespace {
std::wstring NativePath(const std::wstring &path) {
    if (path.starts_with(L"\\\\?\\"))
        return path;
    return path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
}
uint64_t Time(FILETIME value) {
    return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
bool InScope(const std::wstring &path, const std::wstring &root, bool recursive) {
    const auto key = ContentScopeKey(path), base = ContentScopeKey(root);
    if (!ContentPathUnder(key, base))
        return false;
    return recursive || key.substr(base.size() + (base.ends_with(L'\\') ? 0 : 1)).find(L'\\') == std::wstring::npos;
}
bool SystemPath(const std::wstring &path) {
    const auto key = ContentScopeKey(path);
    if (key.size() < 3 || key[1] != L':')
        return false;
    const auto top =
        key.substr(3, key.find(L'\\', 3) == std::wstring::npos ? std::wstring::npos : key.find(L'\\', 3) - 3);
    return top == L"windows" || top == L"$recycle.bin" || top == L"system volume information" ||
           top == L"pagefile.sys" || top == L"hiberfil.sys" || top == L"swapfile.sys";
}
struct Job {
    std::wstring path, version;
    uint64_t size = 0, modified = 0, maximum = 0;
    text::Encoding encoding = text::Encoding::Auto;
    uint64_t enqueued_us = 0;
};
// Each root preserves FIFO order. All roots in a category share its original
// memory budget; task exit deletes any spilled candidate metadata.
struct JobQueue {
    HANDLE file = INVALID_HANDLE_VALUE;
    uint64_t read_at = 0, write_at = 0, count = 0;
    std::deque<Job> pending;
    static size_t Cost(const Job& job) {
        return sizeof(Job) + (job.path.size() + job.version.size() + 2) * sizeof(wchar_t);
    }
    ~JobQueue() {
        CloseSpill();
    }
    void CloseSpill() {
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        read_at = write_at = 0;
    }
    bool Transfer(void *data, DWORD size, uint64_t offset, bool write) {
        LARGE_INTEGER position{};
        position.QuadPart = offset;
        if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
            return false;
        DWORD done = 0;
        return (write ? WriteFile(file, data, size, &done, nullptr) : ReadFile(file, data, size, &done, nullptr)) &&
               done == size;
    }
    bool Push(const Job &job, uint64_t &budget, size_t &memory_bytes) {
        if (count == pending.size() && memory_bytes + Cost(job) <= 8 * 1024 * 1024) {
            pending.push_back(job);
            memory_bytes += Cost(job);
            ++count;
            return true;
        }
        ipc::PayloadWriter payload;
        payload.PutString(job.path);
        payload.PutString(job.version);
        payload.PutU64(job.size);
        payload.PutU64(job.modified);
        payload.PutU64(job.maximum);
        payload.PutU32(static_cast<uint32_t>(job.encoding));
        payload.PutU64(job.enqueued_us);
        DWORD size = static_cast<DWORD>(payload.data().size());
        if (size > 1024 * 1024 || budget + size + sizeof(size) > 1024ull * 1024 * 1024) {
            SetLastError(ERROR_DISK_FULL);
            return false;
        }
        if (file == INVALID_HANDLE_VALUE) {
            wchar_t folder[MAX_PATH]{}, path[MAX_PATH]{};
            if (!GetTempPathW(MAX_PATH, folder) || !GetTempFileNameW(folder, L"pct", 0, path))
                return false;
            file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                const auto error = GetLastError();
                DeleteFileW(path);
                SetLastError(error);
                return false;
            }
        }
        if (!Transfer(&size, sizeof(size), write_at, true) ||
            !Transfer(const_cast<uint8_t *>(payload.data().data()), size, write_at + sizeof(size), true))
            return false;
        write_at += size + sizeof(size);
        budget += size + sizeof(size);
        ++count;
        return true;
    }
    bool Pop(Job &job, size_t &memory_bytes) {
        if (!pending.empty()) {
            memory_bytes -= Cost(pending.front());
            job = std::move(pending.front());
            pending.pop_front();
            --count;
            return true;
        }
        DWORD size = 0;
        if (!count || !Transfer(&size, sizeof(size), read_at, false) || size > 1024 * 1024)
            return false;
        std::vector<uint8_t> bytes(size);
        if (!Transfer(bytes.data(), size, read_at + sizeof(size), false))
            return false;
        ipc::PayloadReader payload(bytes.data(), bytes.size());
        uint32_t encoding = 0;
        if (!payload.GetString(job.path) || !payload.GetString(job.version) || !payload.GetU64(job.size) ||
            !payload.GetU64(job.modified) || !payload.GetU64(job.maximum) || !payload.GetU32(encoding) ||
            !payload.GetU64(job.enqueued_us) || payload.remaining())
            return false;
        job.encoding = static_cast<text::Encoding>(encoding);
        read_at += size + sizeof(size);
        --count;
        return true;
    }
};
struct RootFairQueue {
    std::map<size_t, JobQueue> roots;
    std::deque<size_t> ready;
    uint64_t count = 0;
    size_t memory_bytes = 0;
    bool Push(size_t root, const Job& job, uint64_t& budget) {
        auto& queue = roots.try_emplace(root).first->second;
        const bool was_empty = !queue.count;
        if (!queue.Push(job, budget, memory_bytes)) return false;
        if (was_empty) ready.push_back(root);
        ++count;
        return true;
    }
    bool Pop(Job& job) {
        if (ready.empty()) return false;
        const auto root = ready.front();
        auto found = roots.find(root);
        if (found == roots.end() || !found->second.Pop(job, memory_bytes)) return false;
        ready.pop_front();
        if (found->second.count) ready.push_back(root);
        else found->second.CloseSpill();
        --count;
        return true;
    }
};
struct PdfGroups {
    std::map<std::wstring, size_t> directories;
    static std::wstring Key(const std::wstring& path) {
        auto key = ContentScopeKey(path);
        if (key.starts_with(L"\\\\?\\unc\\")) key.replace(0, 8, L"\\\\");
        else if (key.starts_with(L"\\\\?\\")) key.erase(0, 4);
        return key;
    }
    size_t Select(const std::wstring& path, const std::wstring& root, size_t root_index, size_t root_count) {
        const auto key = Key(path), base = Key(root);
        if (!ContentPathUnder(key, base)) return root_index;
        const auto begin = base.size() + (base.ends_with(L'\\') ? 0 : 1);
        const auto separator = key.find(L'\\', begin);
        if (separator == std::wstring::npos) return root_index;
        const auto directory = key.substr(0, separator);
        const auto found = directories.find(directory);
        if (found != directories.end()) return found->second;
        if (directories.size() >= 128) return root_index;
        const auto group = root_count + directories.size();
        directories.emplace(directory, group);
        return group;
    }
};
struct Result {
    std::optional<ContentHit> hit;
};
} // namespace

bool RunContentTaskSupplement(const ContentIndexConfig &config, const ContentSearchRequest &request,
                              const std::atomic<bool> &cancelled, const ContentTaskCache &cache,
                              ContentSearchProgress progress, size_t hits, ContentBatchCallback callback,
                              ContentTaskReader reader, ContentTiming* supplied_timing) {
    std::unique_ptr<ContentTiming> owned_timing;
    if (!supplied_timing) owned_timing = std::make_unique<ContentTiming>(request.generation);
    auto& timing = supplied_timing ? *supplied_timing : *owned_timing;
    const bool default_reader = !reader;
    std::wstring hint;
    if (default_reader) {
        if (request.excluded_needles.empty() && !request.whole_word) {
            if (request.match_mode == ContentMatchMode::Phrase) {
                hint = request.needle;
                if (hint.empty())
                    for (const auto &word : request.needles) {
                        if (!hint.empty())
                            hint.push_back(L' ');
                        hint.append(word);
                    }
            } else if (request.needles.size() == 1)
                hint = request.needles.front();
            else if (request.needles.empty()) {
                const auto begin = std::find_if_not(request.needle.begin(), request.needle.end(),
                                                    [](wchar_t c) { return iswspace(c); });
                const auto end = std::find_if(begin, request.needle.end(), [](wchar_t c) { return iswspace(c); });
                if (std::all_of(end, request.needle.end(), [](wchar_t c) { return iswspace(c); }))
                    hint.assign(begin, end);
            }
        }

    }
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::condition_variable wake;
    std::array<RootFairQueue, 3> queues;
    std::deque<Result> results;
    std::vector<std::jthread> threads;
    uint64_t queued_bytes = 0, total = 0, scanned = 0, read_bytes = 0;
    bool enumeration_done = false, enumeration_complete = false;
    DWORD error = progress.error;
    std::wstring current_root;
    HANDLE cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!cancel_event) {
        progress.done = true;
        progress.error = GetLastError();
        timing.Finish(0, 0, hits, false, progress.error);
        callback(progress, {});
        return false;
    }
    struct EventOwner {
        HANDLE value;
        ~EventOwner() {
            CloseHandle(value);
        }
    } event_owner{cancel_event};
    struct Join {
        std::atomic<bool> &stop;
        std::condition_variable &wake;
        std::vector<std::jthread> &threads;
        HANDLE event;
        void Finish() {
            stop = true;
            SetEvent(event);
            wake.notify_all();
            for (auto &thread : threads)
                CancelSynchronousIo(thread.native_handle());
            threads.clear();
        }
        ~Join() {
            Finish();
        }
    } join{stop, wake, threads, cancel_event};
    auto interrupted = [&] { return stop.load() || cancelled.load(); };
    const auto filename = ParseQuery(request.filename_query);
    auto fail = [&](DWORD value) {
        std::lock_guard lock(mutex);
        error = value ? value : ERROR_GEN_FAILURE;
        stop = true;
        SetEvent(cancel_event);
        wake.notify_all();
    };
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    const unsigned cpu = (std::max)(1u, std::thread::hardware_concurrency());
    // One reserved worker per category. Extra slots require both CPU and RAM.
    const auto memory_slots =
        static_cast<unsigned>((std::min)(uint64_t{6}, memory.ullAvailPhys / (256ull * 1024 * 1024)));
    const unsigned slots = (std::max)(3u, (std::min)({6u, cpu, (std::max)(3u, memory_slots)}));
    std::array<unsigned, 3> lane_slots{1, 1, 1};
    for (unsigned i = 3; i < slots; ++i)
        ++lane_slots[(i - 3) % 3];
    TraceSearch("content_task_scan_start", request.generation);
    TraceSearch("content_task_workers", slots);
    progress.scanned_files = 0;
    progress.scanned_bytes = 0;
    progress.total_files = 0;
    progress.done = false;
    bool accepted = true;
    std::shared_ptr<DocumentReadPool> document_pool;
    try {
        document_pool = CreateDocumentReadPool();
        for (size_t lane = 0; lane < 3; ++lane)
            for (unsigned i = 0; i < lane_slots[lane]; ++i)
                threads.emplace_back([&, lane, document_pool] {
                    try {
                        DocumentReadSession session(document_pool);
                        TaskTextLiteralReader text_reader(hint, request.case_sensitive);
                        for (;;) {
                            Job job;
                            size_t work_lane = lane;
                            {
                                std::unique_lock lock(mutex);
                                wake.wait(lock,
                                          [&] { return interrupted() || queues[lane].count || enumeration_done; });
                                if (interrupted())
                                    return;
                                if (!queues[work_lane].count) {
                                    // Keep category reservations while more candidates
                                    // may arrive. Once enumeration is complete, idle
                                    // workers help the remaining document/text queues.
                                    work_lane = queues.size();
                                    for (const size_t candidate : {size_t{1}, size_t{2}, size_t{0}})
                                        if (queues[candidate].count) { work_lane = candidate; break; }
                                    if (work_lane == queues.size()) return;
                                }
                                if (!queues[work_lane].Pop(job)) {
                                    lock.unlock();
                                    fail(ERROR_READ_FAULT);
                                    return;
                                }
                            }
                            const auto started = GetTickCount64();
                            const auto token = timing.BeginFile(static_cast<ContentTimingLane>(work_lane), job.path, job.enqueued_us);
                            TraceSearch("content_task_file_start", request.generation, job.path);
                            uint64_t bytes = 0;
                            DWORD read_error = 0;
                            Result result;
                            DocumentReadMetrics document_metrics;
                            try {
                                std::wstring body;
                                const auto native = NativePath(job.path);
                                WIN32_FILE_ATTRIBUTE_DATA data{};
                                // Metadata opens may block on a device. They belong to
                                // workers, never the single candidate enumerator.
                                bool read = GetFileAttributesExW(native.c_str(), GetFileExInfoStandard, &data) != FALSE;
                                if (!read) read_error = GetLastError();
                                constexpr DWORD unsafe = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                                    FILE_ATTRIBUTE_DEVICE | ::pulse::fs::kCloudPlaceholderAttrs;
                                read = read && !(data.dwFileAttributes & unsafe);
                                job.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                                job.modified = Time(data.ftLastWriteTime);
                                read = read && job.size >= request.minimum_file_bytes && job.size <= job.maximum &&
                                    MatchContentFilename(job.path, job.size, job.modified, filename);
                                const bool literal_text = default_reader && !hint.empty() &&
                                    !IsExtractedDocumentExtension(std::filesystem::path(job.path).extension().wstring());
                                const bool handle_preflight = literal_text && cache.native_version;
                                if (read && !handle_preflight) {
                                    job.version = cache.version(job.path);
                                    if (job.version.empty()) { read = false; read_error = ERROR_ACCESS_DENIED; }
                                    else if (cache.fresh(job.path, job.size, job.modified, job.version)) {
                                        timing.ReuseCachedFile(static_cast<ContentTimingLane>(work_lane));
                                        read = false;
                                    }
                                }
                                uint32_t skipped_lines = 0;
                                std::wstring checked_text_version;
                                if (read && !handle_preflight) timing.FileStage(token, ContentTimingStage::Extract);
                                if (read && literal_text) {
                                    TaskTextLiteralResult text_result;
                                    if (handle_preflight) {
                                        // Validate cached versions with the same handle that reads
                                        // the text, avoiding a separate version-only file open.
                                        read = text_reader.Read(native, job.maximum, text_result, &read_error, job.encoding, interrupted,
                                            [&](const TaskTextMetadata& metadata) {
                                                job.size = metadata.size; job.modified = metadata.modified;
                                                job.version.assign(metadata.version);
                                                if (job.size < request.minimum_file_bytes ||
                                                    !MatchContentFilename(job.path, job.size, job.modified, filename)) return false;
                                                if (cache.fresh(job.path, job.size, job.modified, job.version)) {
                                                    timing.ReuseCachedFile(static_cast<ContentTimingLane>(work_lane));
                                                    return false;
                                                }
                                                timing.FileStage(token, ContentTimingStage::Extract);
                                                return true;
                                            });
                                        if (text_result.skipped) read = false;
                                    } else {
                                        read = text_reader.Read(native, job.maximum, text_result, &read_error, job.encoding, interrupted);
                                    }
                                    bytes = text_result.bytes_read;
                                    skipped_lines = text_result.skipped_lines;
                                    if (read && cache.native_version) checked_text_version = std::move(text_result.version);
                                    body = std::move(text_result.body);
                                } else if (read && default_reader) {
                                    read = ReadSearchableDocument(native, job.maximum, body, bytes, &read_error,
                                        job.encoding, interrupted, hint, request.case_sensitive, &document_metrics);
                                } else if (read) {
                                    read = reader(native, job.maximum, body, bytes, &read_error, job.encoding, interrupted);
                                }
                                if (read && !interrupted()) {
                                    timing.FileStage(token, ContentTimingStage::MetadataIo);
                                    if ((checked_text_version.empty() ? cache.version(job.path) : checked_text_version) != job.version)
                                        read_error = ERROR_RETRY;
                                    else {
                                        timing.FileStage(token, ContentTimingStage::Match);
                                        const auto match = MatchCachedContent(body, request, &stop);
                                        if (match != std::wstring::npos) {
                                            result.hit =
                                                MakeCachedContentHit(job.path, job.size, job.modified, body, match);
                                            result.hit->line = static_cast<uint32_t>((std::min)(uint64_t{UINT32_MAX},
                                                uint64_t(result.hit->line) + skipped_lines));
                                            result.hit->file_id = 0;
                                        }
                                    }
                                }
                            } catch (const std::bad_alloc &) {
                                read_error = ERROR_NOT_ENOUGH_MEMORY;
                            } catch (...) {
                                read_error = ERROR_GEN_FAILURE;
                            }
                            TraceSearch("content_task_file_ms", GetTickCount64() - started, job.path);
                            timing.EndFile(token, result.hit.has_value(), read_error, bytes,
                                work_lane == static_cast<size_t>(ContentTimingLane::Text) ? nullptr : &document_metrics);
                            {
                                std::unique_lock lock(mutex);
                                if (read_error && read_error != ERROR_CANCELLED)
                                    error = read_error;
                                if (result.hit) {
                                    wake.wait(lock, [&] { return interrupted() || results.size() < 32; });
                                    if (interrupted())
                                        return;
                                    results.push_back(std::move(result));
                                }
                                ++scanned;
                                read_bytes += bytes;
                            }
                            wake.notify_all();
                        }
                    } catch (...) {
                        fail(ERROR_NOT_ENOUGH_MEMORY);
                    }
                });
        threads.emplace_back([&] {
            bool complete = true;
            try {
                PdfGroups pdf_groups;
                const std::wstring uncovered_root = (request.allow_uncovered_root && !request.root.empty())
                    ? ContentScopeKey(request.root) : std::wstring{};
                auto visit = [&](const std::wstring &path) {
                    if (interrupted())
                        return false;
                    const auto dot = path.find_last_of(L'.');
                    const auto ext =
                        dot == std::wstring::npos ? std::wstring_view{} : std::wstring_view(path).substr(dot);
                    if (!IsIndexedContentExtension(ext) || cache.excluded(path) ||
                        (request.skip_system_locations && SystemPath(path)))
                        return true;
                    const auto root = std::find_if(config.roots.begin(), config.roots.end(),
                                                   [&](const auto &scope) { return InScope(path, scope.path, true); });
                    const bool uncovered_match = root == config.roots.end() &&
                        !uncovered_root.empty() && InScope(path, uncovered_root, request.recursive);
                    if ((root == config.roots.end() && !uncovered_match) ||
                        (!request.root.empty() && !InScope(path, request.root, request.recursive)) ||
                        (!request.roots.empty() &&
                         std::none_of(request.roots.begin(), request.roots.end(),
                                      [&](const auto &scope) { return InScope(path, scope, request.recursive); })))
                        return true;
                    const bool document = IsExtractedDocumentExtension(ext);
                    const auto maximum =
                        (std::min)(document ? config.maximum_document_bytes : config.maximum_file_bytes,
                                   document ? request.maximum_document_bytes : request.maximum_file_bytes);
                    const size_t lane = ContentScopeKey(std::wstring(ext)) == L".pdf" ? 0 : document ? 1 : 2;
                    const std::wstring& root_path = uncovered_match ? uncovered_root : root->path;
                    const pulse::text::Encoding root_encoding = uncovered_match ? config.default_encoding : root->encoding;
                    const size_t root_index = uncovered_match ? config.roots.size()
                                                              : static_cast<size_t>(root - config.roots.begin());
                    const size_t root_count = uncovered_match ? config.roots.size() + 1 : config.roots.size();
                    // Only PDF has the long backlog that needs directory fairness.
                    // Office/text retain their original global admission order.
                    const auto group = lane == 0 ? pdf_groups.Select(path, root_path, root_index, root_count) : 0;
                    std::unique_lock lock(mutex);
                    ++total;
                    current_root = root_path;
                    const auto enqueued = timing.Enqueue(static_cast<ContentTimingLane>(lane));
                    if (!queues[lane].Push(group, {path, {}, 0, 0, maximum, root_encoding, enqueued}, queued_bytes)) {
                        const auto code = GetLastError();
                        lock.unlock();
                        fail(code);
                        return false;
                    }
                    lock.unlock();
                    wake.notify_all();
                    return true;
                };
                if (!request.candidate_paths.empty()) {
                    std::set<std::wstring> visited;
                    for (const auto& path : request.candidate_paths) {
                        if (visited.insert(ContentScopeKey(path)).second && !visit(path)) break;
                    }
                } else if (config.shared_scope && request.root.empty() && request.roots.empty()) {
                    IndexFeedConnection feed(cancel_event);
                    uint64_t epoch = 0, cursor = 0;
                    while (!interrupted()) {
                        FileFeedPage page;
                        const auto feed_started = ContentTiming::NowMicros();
                        const bool requested = feed.Request(false, L"", epoch, cursor, page);
                        timing.RecordStage(ContentTimingStage::FeedEnumerate, feed_started);
                        if (!requested || !page.ready || page.gap) {
                            complete = false;
                            std::lock_guard lock(mutex);
                            error = interrupted() ? ERROR_CANCELLED : ERROR_RETRY;
                            break;
                        }
                        epoch = page.epoch;
                        for (const auto &record : page.records)
                            if (!visit(record.path))
                                break;
                        if (page.done)
                            break;
                        if (page.next <= cursor) {
                            complete = false;
                            std::lock_guard lock(mutex);
                            error = ERROR_INVALID_DATA;
                            break;
                        }
                        cursor = page.next;
                    }
                } else {
                    const bool constrained = !request.root.empty() || !request.roots.empty();
                    const bool walk_recursive = !constrained || request.recursive;
                    std::vector<std::wstring> configured;
                    for (const auto& root : config.roots) {
                        const auto key = ContentScopeKey(root.path);
                        if (std::any_of(configured.begin(), configured.end(),
                            [&](const auto& old) { return ContentPathUnder(key, old); })) continue;
                        std::erase_if(configured, [&](const auto& old) { return ContentPathUnder(old, key); });
                        configured.push_back(key);
                    }
                    std::vector<std::wstring> scopes;
                    const auto primary = ContentScopeKey(request.root);
                    if (request.roots.empty()) {
                        if (!primary.empty()) scopes.push_back(primary);
                    } else {
                        for (const auto& scope : request.roots) {
                            const auto key = ContentScopeKey(scope);
                            if (primary.empty() || key == primary ||
                                (walk_recursive && ContentPathUnder(key, primary))) scopes.push_back(key);
                            else if (walk_recursive && ContentPathUnder(primary, key)) scopes.push_back(primary);
                        }
                    }
                    struct WalkRoot { std::wstring path, anchor; };
                    std::vector<WalkRoot> roots;
                    auto add_root = [&](const std::wstring& path, const std::wstring& anchor) {
                        auto covers = [&](const std::wstring& a, const std::wstring& b) {
                            return a == b || (walk_recursive && ContentPathUnder(b, a));
                        };
                        if (std::any_of(roots.begin(), roots.end(),
                            [&](const auto& old) { return covers(old.path, path); })) return;
                        std::erase_if(roots, [&](const auto& old) { return covers(path, old.path); });
                        roots.push_back({path, anchor});
                    };
                    for (const auto& configured_root : configured) {
                        if (!constrained) add_root(configured_root, configured_root);
                        for (const auto& scope : scopes) {
                            if (ContentPathUnder(scope, configured_root)) add_root(scope, configured_root);
                            else if (walk_recursive && ContentPathUnder(configured_root, scope))
                                add_root(configured_root, configured_root);
                        }
                    }
                    if (!uncovered_root.empty()) {
                        for (const auto& scope : scopes) {
                            if (ContentPathUnder(scope, uncovered_root)) add_root(scope, uncovered_root);
                        }
                    }
                    // Starting below a configured root must not bypass the DFS
                    // exclusions for junctions, offline directories or excluded ancestors.
                    auto reachable = [&](const WalkRoot& root) {
                        if (root.path == root.anchor) return true;
                        size_t component = root.anchor.size() + (root.anchor.ends_with(L'\\') ? 0 : 1);
                        for (;;) {
                            const auto separator = root.path.find(L'\\', component);
                            const auto ancestor = root.path.substr(0, separator);
                            if (interrupted() || cache.excluded(ancestor) ||
                                (request.skip_system_locations && SystemPath(ancestor))) return false;
                            const DWORD attributes = GetFileAttributesW(NativePath(ancestor).c_str());
                            if (attributes == INVALID_FILE_ATTRIBUTES) {
                                const DWORD enumeration_error = GetLastError();
                                complete = false;
                                std::lock_guard lock(mutex);
                                if (!error) error = enumeration_error ? enumeration_error : ERROR_GEN_FAILURE;
                                return false;
                            }
                            if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) return false;
                            if (separator == std::wstring::npos) return true;
                            component = separator + 1;
                        }
                    };
                    struct Frame {
                        std::wstring path;
                        HANDLE find = INVALID_HANDLE_VALUE;
                        WIN32_FIND_DATAW data{};
                    };
                    std::vector<Frame> stack;
                    struct CloseFinds {
                        std::vector<Frame> &stack;
                        ~CloseFinds() {
                            for (auto &frame : stack)
                                if (frame.find != INVALID_HANDLE_VALUE)
                                    content_listing::Close(frame.find);
                        }
                    } close_finds{stack};
                    auto open = [&](const std::wstring &path) {
                        if (cache.excluded(path) || (request.skip_system_locations && SystemPath(path)))
                            return;
                        Frame frame;
                        frame.path = path;
                        frame.find = content_listing::First(NativePath(path) + L"\\*", frame.data,
                                                            FIND_FIRST_EX_LARGE_FETCH);
                        if (frame.find != INVALID_HANDLE_VALUE)
                            stack.push_back(std::move(frame));
                        else {
                            const DWORD enumeration_error = GetLastError();
                            if (content_listing::EmptyDirectoryResult(NativePath(path), enumeration_error)) return;
                            complete = false;
                            std::lock_guard lock(mutex);
                            if (!error) error = enumeration_error ? enumeration_error : ERROR_GEN_FAILURE;
                        }
                    };
                    for (const auto &root : roots) {
                        if (interrupted()) break;
                        if (!reachable(root)) continue;
                        open(root.path);
                        while (!stack.empty() && !interrupted()) {
                            auto &frame = stack.back();
                            const auto data = frame.data;
                            const auto child = frame.path + L"\\" + data.cFileName;
                            if (!content_listing::Next(frame.find, frame.data)) {
                                const DWORD enumeration_error = GetLastError();
                                if (enumeration_error != ERROR_NO_MORE_FILES) {
                                    complete = false;
                                    std::lock_guard lock(mutex);
                                    if (!error) error = enumeration_error ? enumeration_error : ERROR_GEN_FAILURE;
                                }
                                content_listing::Close(frame.find);
                                stack.pop_back();
                            }
                            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L".."))
                                continue;
                            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                                if (walk_recursive && !(data.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)))
                                    open(child);
                            } else if (!visit(child))
                                break;
                        }
                        if (interrupted())
                            break;
                    }
                }
            } catch (...) {
                complete = false;
                fail(ERROR_NOT_ENOUGH_MEMORY);
            }
            {
                std::lock_guard lock(mutex);
                enumeration_done = true;
                enumeration_complete = complete && !interrupted();
                if (enumeration_complete) timing.EnumerationComplete();
                TraceSearch("content_task_enumerated", total);
            }
            wake.notify_all();
        });
        auto last_update = GetTickCount64() - 100;
        for (;;) {
            Result result;
            bool have_result = false, finished = false;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(25), [&] {
                    return !results.empty() || interrupted() || (enumeration_done && scanned == total);
                });
                if (!results.empty()) {
                    result = std::move(results.front());
                    results.pop_front();
                    have_result = true;
                }
                progress.scanned_files = scanned;
                progress.scanned_bytes = read_bytes;
                progress.total_files = enumeration_complete ? total : 0;
                progress.current_root = current_root;
                progress.error = error;
                finished = enumeration_done && scanned == total && results.empty();
            }
            wake.notify_all();
            if (have_result) {
                last_update = GetTickCount64();
                ++hits;
                if (request.maximum_hits && hits >= request.maximum_hits) progress.truncated = true;
                timing.FlushProgress(progress.scanned_files, progress.total_files, hits);
                accepted = callback(progress, {std::move(*result.hit)});
            } else if (GetTickCount64() - last_update >= 100) {
                last_update = GetTickCount64();
                timing.FlushProgress(progress.scanned_files, progress.total_files, hits);
                accepted = callback(progress, {});
            }
            if (!accepted || interrupted() || progress.truncated || finished)
                break;
        }
    } catch (const std::bad_alloc &) {
        fail(ERROR_NOT_ENOUGH_MEMORY);
    } catch (...) {
        fail(ERROR_GEN_FAILURE);
    }
    join.Finish();
    document_pool.reset();
    timing.Finish(scanned, enumeration_complete ? total : 0, hits, cancelled || !accepted,
        cancelled || !accepted ? ERROR_CANCELLED : error);
    if (!accepted)
        return false;
    {
        std::lock_guard lock(mutex);
        progress.error = cancelled ? ERROR_CANCELLED : error;
        progress.total_files = enumeration_complete ? total : 0;
        progress.scanned_files = scanned;
        progress.scanned_bytes = read_bytes;
    }
    progress.done = true;
    TraceSearch("content_task_scan_done", progress.scanned_files);
    callback(progress, {});
    return !cancelled && (enumeration_complete || progress.truncated);
}
} // namespace pulse::index

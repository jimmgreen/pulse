#include "directories_adapter.h"
#include "../../../fs/fs_enum.h"
#include "../../../fs/fs_watch.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulse::extensions::native {
namespace {
constexpr size_t kDirectoryLimit = 4, kPendingLimit = 32, kWatchLimit = 8;
constexpr size_t kEntryLimit = 65536, kNameBytesLimit = 16 * 1024 * 1024;
constexpr ULONGLONG kWatchStartTimeout = 5000;
std::atomic<uint64_t> last_id{0};
std::atomic<unsigned> active_workers{0};
uint64_t NewId() {
    auto previous = last_id.load(std::memory_order_relaxed);
    while (previous != UINT64_MAX) {
        if (last_id.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed)) return previous + 1;
    }
    return 0;
}
PulseStringView View(const std::string &value) { return {value.data(), static_cast<uint32_t>(value.size())}; }
std::string Utf8(const std::wstring &value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("invalid_utf16");
    std::string result(static_cast<size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                             result.data(), count, nullptr, nullptr)) throw std::runtime_error("invalid_utf16");
    return result;
}
bool DeviceComponent(std::wstring value) {
    const auto dot = value.find(L'.');
    if (dot != std::wstring::npos) value.resize(dot);
    while (!value.empty() && (value.back() == L' ' || value.back() == L'.')) value.pop_back();
    for (auto &c : value) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
    return value == L"CON" || value == L"PRN" || value == L"AUX" || value == L"NUL" ||
           value == L"CONIN$" || value == L"CONOUT$" ||
           (value.size() == 4 && (value.starts_with(L"COM") || value.starts_with(L"LPT")) &&
            value.back() >= L'1' && value.back() <= L'9');
}
bool Path(PulseStringView input, std::wstring &output) {
    if (!input.data || !input.length || input.length > 131072 ||
        std::memchr(input.data, 0, input.length)) return false;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data,
                                         static_cast<int>(input.length), nullptr, 0);
    if (count <= 0 || count >= 32767) return false;
    output.resize(static_cast<size_t>(count));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data, static_cast<int>(input.length), output.data(), count);
    std::replace(output.begin(), output.end(), L'/', L'\\');
    if (output.starts_with(L"\\\\.\\")) return false;
    std::wstring core = output;
    if (core.starts_with(L"\\\\?\\")) {
        core.erase(0, 4);
        if (core.size() >= 4 && _wcsnicmp(core.c_str(), L"UNC\\", 4) == 0) {
            core = L"\\\\" + core.substr(4);
            output = L"\\\\?\\UNC\\" + core.substr(2);
        }
    }
    size_t start = 0;
    if (core.size() >= 3 && ((core[0] >= L'A' && core[0] <= L'Z') || (core[0] >= L'a' && core[0] <= L'z')) &&
        core[1] == L':' && core[2] == L'\\') start = 3;
    else if (core.starts_with(L"\\\\")) {
        const auto server = core.find(L'\\', 2);
        if (server == std::wstring::npos || server == 2) return false;
        const auto share = core.find(L'\\', server + 1);
        if (server + 1 == core.size() || share == server + 1) return false;
        const auto server_name = core.substr(2, server - 2);
        const auto share_name = core.substr(server + 1, share == std::wstring::npos ? share : share - server - 1);
        if (server_name == L"." || server_name == L".." || share_name == L"." || share_name == L"..") return false;
        start = 2;
    } else return false;
    for (size_t index = start; index < core.size(); ++index) {
        const wchar_t c = core[index];
        if (c < 32 || c == L':' || c == L'?' || c == L'*' || c == L'"' || c == L'<' || c == L'>' || c == L'|')
            return false;
    }
    while (start < core.size()) {
        const auto end = core.find(L'\\', start);
        const auto component = core.substr(start, end == std::wstring::npos ? end : end - start);
        if (DeviceComponent(component)) return false;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return true;
}
struct Entry {
    uint64_t id = 0, size = 0, modified = 0;
    uint32_t attributes = 0, flags = 0;
    std::string name;
    std::wstring leaf;
};
struct Snapshot {
    uint64_t id = 0;
    std::wstring path;
    std::string utf8_path;
    std::vector<Entry> rows;
};
struct Job {
    uint64_t id = 0, directory = 0, generation = 0;
    uint32_t operation = 0, offset = 0, limit = 0;
    std::atomic<bool> cancelled{false};
    std::wstring path;
    std::shared_ptr<const Snapshot> snapshot;
};
struct Result {
    std::shared_ptr<Job> job;
    std::shared_ptr<const Snapshot> snapshot;
    PulseStatus status = PULSE_OK;
    DWORD system_error = 0;
    std::string error;
    bool folder = false;
};
struct WatchSignal { bool armed = false, invalidated = false, overflow = false; };
// Only host-owned values reach workers. DLL callbacks and user pointers live in Impl on the UI thread.
struct WorkerState {
    std::atomic<bool> closed{false};
    std::atomic<unsigned> active{0};
    std::mutex mutex;
    std::vector<Result> results;
    std::unordered_map<uint64_t, WatchSignal> signals;
    void Complete(Result result) {
        std::lock_guard lock(mutex);
        std::erase_if(results, [](const Result &pending) { return pending.job->cancelled.load(); });
        if (!closed && !result.job->cancelled) results.push_back(std::move(result));
    }
    void Signal(uint64_t id, bool armed, bool overflow) {
        std::lock_guard lock(mutex);
        if (closed) return;
        const auto found = signals.find(id);
        if (found == signals.end()) return;
        auto &signal = found->second;
        signal.armed = signal.armed || armed;
        signal.invalidated = signal.invalidated || !armed;
        signal.overflow = signal.overflow || (!armed && overflow);
    }
};
Result Run(const std::shared_ptr<Job> &job) {
    Result result;
    result.job = job;
    if (job->cancelled) return result;
    const auto path = fs::NormalizePath(job->path);
    const DWORD attributes = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        result.status = PULSE_ERROR_STATE;
        result.system_error = GetLastError();
        result.error = "path_unavailable";
        return result;
    }
    if (job->operation == PULSE_DIRECTORY_OPEN_ENTRY) {
        result.folder = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return result;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        result.status = PULSE_ERROR_STATE;
        result.system_error = ERROR_DIRECTORY;
        result.error = "not_directory";
        return result;
    }
    std::vector<fs::DirEntry> entries;
    fs::EnumerationOptions options;
    options.cancelled = [job] { return job->cancelled.load(); };
    fs::EnumerateDirectory(path, entries, options);
    if (entries.size() > kEntryLimit) {
        result.status = PULSE_ERROR_STATE;
        result.system_error = ERROR_BUFFER_OVERFLOW;
        result.error = "directory_limit_exceeded";
        return result;
    }
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->id = NewId();
    if (!snapshot->id) { result.status = PULSE_ERROR_STATE; result.error = "id_exhausted"; return result; }
    snapshot->path = path;
    snapshot->utf8_path = Utf8(path);
    snapshot->rows.reserve(entries.size());
    size_t bytes = 0;
    for (const auto &source : entries) {
        if (job->cancelled) return result;
        Entry entry;
        entry.id = NewId();
        if (!entry.id) { result.status = PULSE_ERROR_STATE; result.error = "id_exhausted"; return result; }
        entry.leaf = source.name;
        entry.name = Utf8(source.name);
        bytes += entry.name.size();
        if (bytes > kNameBytesLimit) {
            result.status = PULSE_ERROR_STATE;
            result.system_error = ERROR_BUFFER_OVERFLOW;
            result.error = "directory_name_limit_exceeded";
            return result;
        }
        entry.size = source.size;
        entry.modified = (static_cast<uint64_t>(source.mtime.dwHighDateTime) << 32) | source.mtime.dwLowDateTime;
        entry.attributes = source.attrs;
        entry.flags = (source.is_dir ? PULSE_DIRECTORY_ENTRY_FOLDER : 0u) |
                      (source.is_reparse ? PULSE_DIRECTORY_ENTRY_REPARSE : 0u);
        snapshot->rows.push_back(std::move(entry));
    }
    result.snapshot = std::move(snapshot);
    return result;
}
template<class Function> PulseStatus Boundary(Function &&function) noexcept {
    try { return function(); } catch (...) { return PULSE_ERROR_STATE; }
}
} // namespace
#if defined(PULSE_DIRECTORIES_TEST)
void ExhaustDirectoryIdsForTest() noexcept { last_id = UINT64_MAX; }
#endif

struct DirectoriesAdapter::Impl {
    struct Directory {
        uint64_t generation = 1;
        std::wstring path;
        std::shared_ptr<const Snapshot> snapshot;
    };
    struct Request {
        std::shared_ptr<Job> job;
        PulseDirectoryCompletionFn callback = nullptr;
        void *user = nullptr;
    };
    struct Watch {
        uint64_t directory = 0, sequence = 0;
        ULONGLONG started = 0;
        PulseDirectoryChangeFn callback = nullptr;
        void *user = nullptr;
        std::unique_ptr<fs::DirWatch> source;
        bool ready = false, failed = false;
    };
    CallerBinding caller;
    DirectoriesBackend backend;
    const DWORD thread = GetCurrentThreadId();
    bool closed = false, polling = false;
    unsigned callbacks = 0;
    std::shared_ptr<WorkerState> workers = std::make_shared<WorkerState>();
    std::unordered_map<uint64_t, Directory> directories;
    std::unordered_map<uint64_t, Request> requests;
    std::unordered_map<uint64_t, Watch> watches;
    Impl(CallerBinding value, DirectoriesBackend functions) : caller(std::move(value)), backend(std::move(functions)) {}
    ~Impl() { Close(); }
    PulseStatus Ready() {
        if (GetCurrentThreadId() != thread || closed) return PULSE_ERROR_STATE;
        if (!backend.live || !backend.open_item) return PULSE_ERROR_UNAVAILABLE;
        if (!backend.live(caller)) { Close(); return PULSE_ERROR_STATE; }
        return PULSE_OK;
    }
    bool TransferTargetValid(PulseDirectoryId id) {
        if (Ready() != PULSE_OK) return false;
        const auto found = directories.find(id);
        return found != directories.end() && found->second.snapshot;
    }
    bool Start(const std::shared_ptr<Job> &job) {
        if (workers->active.fetch_add(1) >= kPendingLimit) { --workers->active; return false; }
        if (active_workers.fetch_add(1) >= kPendingLimit) { --active_workers; --workers->active; return false; }
        const auto state = workers;
        try {
            std::thread([state, job] {
                Result result;
                result.job = job;
                try { result = Run(job); }
                catch (const std::exception &error) {
                    result.status = PULSE_ERROR_STATE;
                    result.error = error.what();
                    result.system_error = result.error.find("timed out") != std::string::npos ? ERROR_TIMEOUT : ERROR_READ_FAULT;
                } catch (...) {
                    result.status = PULSE_ERROR_STATE;
                    result.error = "enumeration_failed";
                    result.system_error = ERROR_READ_FAULT;
                }
                try { state->Complete(std::move(result)); } catch (...) {}
                --active_workers;
                --state->active;
            }).detach();
        } catch (...) { --active_workers; --workers->active; return false; }
        return true;
    }
    std::shared_ptr<Job> Submit(uint32_t operation, uint64_t directory, PulseDirectoryCompletionFn callback, void *user) {
        if (requests.size() >= kPendingLimit) return {};
        auto job = std::make_shared<Job>();
        job->id = NewId(); job->directory = directory; job->operation = operation;
        if (!job->id) return {};
        requests.emplace(job->id, Request{job, callback, user});
        return job;
    }
    PulseStatus Open(PulseStringView path, PulseDirectoryCompletionFn callback, void *user, uint64_t *output) {
        if (!output) return PULSE_ERROR_ARGUMENT;
        *output = 0;
        const auto status = Ready();
        if (status != PULSE_OK) return status;
        std::wstring copy;
        if (!callback || !Path(path, copy)) return PULSE_ERROR_ARGUMENT;
        if (directories.size() >= kDirectoryLimit) return PULSE_ERROR_STATE;
        const uint64_t id = NewId();
        if (!id) return PULSE_ERROR_STATE;
        auto job = Submit(PULSE_DIRECTORY_OPEN, id, callback, user);
        if (!job) return PULSE_ERROR_STATE;
        directories.emplace(id, Directory{1, copy, {}});
        job->path = std::move(copy); job->generation = 1;
        if (!Start(job)) { requests.erase(job->id); directories.erase(id); return PULSE_ERROR_STATE; }
        *output = job->id;
        return PULSE_OK;
    }
    PulseStatus Refresh(uint64_t id, PulseDirectoryCompletionFn callback, void *user, uint64_t *output) {
        if (!output) return PULSE_ERROR_ARGUMENT;
        *output = 0;
        const auto status = Ready();
        if (status != PULSE_OK) return status;
        if (!callback) return PULSE_ERROR_ARGUMENT;
        const auto found = directories.find(id);
        if (found == directories.end() || !found->second.snapshot) return PULSE_ERROR_STATE;
        auto job = Submit(PULSE_DIRECTORY_REFRESH, id, callback, user);
        if (!job) return PULSE_ERROR_STATE;
        job->path = found->second.path;
        job->generation = found->second.generation + 1;
        if (!Start(job)) { requests.erase(job->id); return PULSE_ERROR_STATE; }
        found->second.generation = job->generation;
        for (auto it = requests.begin(); it != requests.end();) {
            const auto &old = it->second.job;
            if (old->directory == id && old->operation == PULSE_DIRECTORY_REFRESH && old->id != job->id) {
                old->cancelled = true; it = requests.erase(it);
            } else ++it;
        }
        *output = job->id;
        return PULSE_OK;
    }
    PulseStatus SnapshotRequest(uint32_t operation, uint64_t id, uint64_t snapshot_id, uint64_t entry_id,
                                uint32_t offset, uint32_t limit, PulseDirectoryCompletionFn callback, void *user,
                                uint64_t *output) {
        if (!output) return PULSE_ERROR_ARGUMENT;
        *output = 0;
        const auto status = Ready();
        if (status != PULSE_OK) return status;
        if (!callback || (operation == PULSE_DIRECTORY_LIST && (!limit || limit > 256))) return PULSE_ERROR_ARGUMENT;
        const auto found = directories.find(id);
        if (found == directories.end() || !found->second.snapshot || found->second.snapshot->id != snapshot_id)
            return PULSE_ERROR_STATE;
        const auto snapshot = found->second.snapshot;
        if (operation == PULSE_DIRECTORY_LIST && offset > snapshot->rows.size()) return PULSE_ERROR_ARGUMENT;
        const Entry *entry = nullptr;
        if (operation == PULSE_DIRECTORY_OPEN_ENTRY) {
            const auto item = std::find_if(snapshot->rows.begin(), snapshot->rows.end(),
                                            [entry_id](const Entry &value) { return value.id == entry_id; });
            if (item == snapshot->rows.end()) return PULSE_ERROR_STATE;
            entry = &*item;
        }
        auto job = Submit(operation, id, callback, user);
        if (!job) return PULSE_ERROR_STATE;
        job->snapshot = snapshot; job->offset = offset; job->limit = limit;
        if (entry) {
            job->path = snapshot->path + (snapshot->path.ends_with(L"\\") ? L"" : L"\\") + entry->leaf;
            if (!Start(job)) { requests.erase(job->id); return PULSE_ERROR_STATE; }
        } else {
            Result result; result.job = job;
            workers->Complete(std::move(result));
        }
        *output = job->id;
        return PULSE_OK;
    }
    PulseStatus Unwatch(uint64_t id) {
        const auto status = Ready();
        if (status != PULSE_OK) return status;
        const auto found = watches.find(id);
        if (found == watches.end()) return PULSE_ERROR_STATE;
        { std::lock_guard lock(workers->mutex); workers->signals.erase(id); }
        found->second.source->StopAsync();
        watches.erase(found);
        return PULSE_OK;
    }
    PulseStatus AddWatch(uint64_t directory, PulseDirectoryChangeFn callback, void *user, uint64_t *output) {
        if (!output) return PULSE_ERROR_ARGUMENT;
        *output = 0;
        const auto status = Ready();
        if (status != PULSE_OK) return status;
        if (!callback) return PULSE_ERROR_ARGUMENT;
        const auto found = directories.find(directory);
        if (found == directories.end() || !found->second.snapshot || watches.size() >= kWatchLimit)
            return PULSE_ERROR_STATE;
        const auto id = NewId();
        if (!id) return PULSE_ERROR_STATE;
        Watch watch;
        watch.directory = directory; watch.started = GetTickCount64(); watch.callback = callback; watch.user = user;
        watch.source = std::make_unique<fs::DirWatch>();
        { std::lock_guard lock(workers->mutex); workers->signals.emplace(id, WatchSignal{}); }
        std::weak_ptr<WorkerState> weak = workers;
        if (!watch.source->Start(found->second.snapshot->path, [weak, id](bool overflow, std::vector<fs::DirNotifyEvent> events) {
                if (const auto state = weak.lock()) state->Signal(id, fs::ArmedAt(overflow, events) != 0, overflow);
            }, false, true)) {
            std::lock_guard lock(workers->mutex); workers->signals.erase(id);
            return PULSE_ERROR_STATE;
        }
        watches.emplace(id, std::move(watch));
        *output = id;
        return PULSE_OK;
    }
    PulseStatus Cancel(uint64_t id) {
        const auto status = Ready();
        if (status != PULSE_OK) return status;
        const auto found = requests.find(id);
        if (found == requests.end()) return PULSE_ERROR_STATE;
        found->second.job->cancelled = true;
        if (found->second.job->operation == PULSE_DIRECTORY_OPEN) directories.erase(found->second.job->directory);
        requests.erase(found);
        return PULSE_OK;
    }
    PulseStatus CloseDirectory(uint64_t id) {
        const auto status = Ready();
        if (status != PULSE_OK) return status;
        if (!directories.erase(id)) return PULSE_ERROR_STATE;
        for (auto it = requests.begin(); it != requests.end();) {
            if (it->second.job->directory == id) { it->second.job->cancelled = true; it = requests.erase(it); }
            else ++it;
        }
        std::vector<uint64_t> stopped;
        for (const auto &[watch_id, watch] : watches) if (watch.directory == id) stopped.push_back(watch_id);
        for (const auto watch_id : stopped) Unwatch(watch_id);
        return PULSE_OK;
    }
    PulseStatus Close() noexcept {
        if (GetCurrentThreadId() != thread) return PULSE_ERROR_STATE;
        closed = true;
        workers->closed = true;
        for (auto &[id, request] : requests) { (void)id; request.job->cancelled = true; }
        requests.clear();
        watches.clear();
        directories.clear();
        { std::lock_guard lock(workers->mutex); workers->results.clear(); workers->signals.clear(); }
        return callbacks ? PULSE_ERROR_STATE : PULSE_OK;
    }
    bool Current(const Request &request) const {
        const auto found = directories.find(request.job->directory);
        if (found == directories.end() || request.job->cancelled) return false;
        if (request.job->operation == PULSE_DIRECTORY_OPEN || request.job->operation == PULSE_DIRECTORY_REFRESH)
            return found->second.generation == request.job->generation;
        return found->second.snapshot && request.job->snapshot && found->second.snapshot->id == request.job->snapshot->id;
    }
    void Deliver(Result result) {
        auto found = requests.find(result.job->id);
        if (found == requests.end()) return;
        Request request = std::move(found->second);
        requests.erase(found);
        if (!Current(request)) return;
        auto &job = *request.job;
        if (Ready() != PULSE_OK) {
            if (closed) return;
            result.status = PULSE_ERROR_STATE; result.error = "stale_owner";
        }
        auto directory = directories.find(job.directory);
        if (result.status == PULSE_OK && (job.operation == PULSE_DIRECTORY_OPEN || job.operation == PULSE_DIRECTORY_REFRESH)) {
            if (!result.snapshot) { result.status = PULSE_ERROR_STATE; result.error = "invalid_snapshot"; }
            else { directory->second.snapshot = result.snapshot; directory->second.path = result.snapshot->path; }
        }
        if (result.status == PULSE_OK && job.operation == PULSE_DIRECTORY_OPEN_ENTRY) {
            result.status = backend.open_item(caller, job.path, result.folder);
            if (result.status != PULSE_OK) result.error = "open_dispatch_failed";
            if (closed || !backend.live(caller) || !Current(request)) return;
            directory = directories.find(job.directory);
        }
        PulseDirectoryReply reply{};
        reply.size = sizeof(reply); reply.operation = job.operation; reply.request_id = job.id;
        reply.status = result.status; reply.system_error = result.system_error; reply.error = View(result.error);
        reply.directory_id = job.directory; reply.next_offset = UINT32_MAX;
        const auto snapshot = directory->second.snapshot;
        std::vector<PulseDirectoryRow> rows;
        if (snapshot) {
            reply.snapshot_id = snapshot->id; reply.path = View(snapshot->utf8_path); reply.total = snapshot->rows.size();
            if (job.operation == PULSE_DIRECTORY_LIST && result.status == PULSE_OK) {
                const size_t end = (std::min)(snapshot->rows.size(), static_cast<size_t>(job.offset) + job.limit);
                for (size_t index = job.offset; index < end; ++index) {
                    const auto &entry = snapshot->rows[index];
                    rows.push_back({entry.id, View(entry.name), entry.size, entry.modified, entry.attributes, entry.flags});
                }
                reply.rows = rows.data(); reply.count = static_cast<uint32_t>(rows.size());
                reply.next_offset = end < snapshot->rows.size() ? static_cast<uint32_t>(end) : UINT32_MAX;
            } else if (result.status == PULSE_OK && !snapshot->rows.empty()) reply.next_offset = 0;
        }
        if (job.operation == PULSE_DIRECTORY_OPEN && result.status != PULSE_OK) {
            directories.erase(directory); reply.directory_id = 0;
        }
        ++callbacks;
        try { request.callback(request.user, &reply); } catch (...) { Close(); }
        --callbacks;
    }
    void Change(uint64_t id, uint32_t kind, bool overflow = false, const char *error = nullptr) {
        const auto found = watches.find(id);
        if (found == watches.end() || closed) return;
        auto &watch = found->second;
        PulseDirectoryChange change{};
        change.size = sizeof(change); change.kind = kind; change.watch_id = id; change.directory_id = watch.directory;
        change.sequence = ++watch.sequence; change.overflow = overflow;
        change.status = error ? PULSE_ERROR_STATE : PULSE_OK;
        if (error) change.error = {error, static_cast<uint32_t>(std::strlen(error))};
        const auto callback = watch.callback; void *const user = watch.user;
        ++callbacks;
        try { callback(user, &change); } catch (...) { Close(); }
        --callbacks;
    }
    void Poll() {
        if (polling || Ready() != PULSE_OK) return;
        polling = true;
        struct Reset { bool &value; ~Reset() { value = false; } } reset{polling};
        std::vector<Result> results;
        std::unordered_map<uint64_t, WatchSignal> signals;
        {
            std::lock_guard lock(workers->mutex);
            results.swap(workers->results);
            for (auto &[id, signal] : workers->signals) { signals.emplace(id, signal); signal = {}; }
        }
        for (auto &result : results) {
            if (Ready() != PULSE_OK) return;
            Deliver(std::move(result));
        }
        for (const auto &[id, signal] : signals) {
            if (Ready() != PULSE_OK) return;
            auto found = watches.find(id);
            if (found == watches.end() || found->second.failed) continue;
            const bool already_ready = found->second.ready;
            if (signal.armed && !already_ready) {
                found->second.ready = true;
                Change(id, PULSE_DIRECTORY_WATCH_READY);
                found = watches.find(id);
                if (found == watches.end() || closed) continue;
            }
            if ((!found->second.ready && signal.invalidated) ||
                (!found->second.ready && GetTickCount64() - found->second.started >= kWatchStartTimeout)) {
                found->second.failed = true; found->second.source->StopAsync();
                Change(id, PULSE_DIRECTORY_WATCH_ERROR, signal.overflow, "watch_unavailable");
            } else if (found->second.ready && (signal.invalidated || (already_ready && signal.armed))) {
                Change(id, PULSE_DIRECTORY_INVALIDATED, signal.overflow || signal.armed);
            }
        }
    }
};

DirectoriesAdapter::DirectoriesAdapter(CallerBinding caller, DirectoriesBackend backend)
    : impl_(std::make_shared<Impl>(std::move(caller), std::move(backend))) {}
DirectoriesAdapter::~DirectoriesAdapter() { Close(); }
PulseDirectoriesV1 DirectoriesAdapter::Table() const noexcept {
    PulseDirectoriesV1 table{};
    table.size = sizeof(table); table.version = PULSE_DIRECTORIES_VERSION_1; table.context = impl_.get();
    table.open = [](void *context, PulseStringView path, PulseDirectoryCompletionFn callback, void *user, uint64_t *output) {
        return Boundary([&] { return context ? static_cast<Impl *>(context)->Open(path, callback, user, output) : PULSE_ERROR_ARGUMENT; });
    };
    table.refresh = [](void *context, uint64_t directory, PulseDirectoryCompletionFn callback, void *user, uint64_t *output) {
        return Boundary([&] { return context ? static_cast<Impl *>(context)->Refresh(directory, callback, user, output) : PULSE_ERROR_ARGUMENT; });
    };
    table.list = [](void *context, uint64_t directory, uint64_t snapshot, uint32_t offset, uint32_t limit,
                    PulseDirectoryCompletionFn callback, void *user, uint64_t *output) {
        return Boundary([&] { return context ? static_cast<Impl *>(context)->SnapshotRequest(PULSE_DIRECTORY_LIST, directory,
            snapshot, 0, offset, limit, callback, user, output) : PULSE_ERROR_ARGUMENT; });
    };
    table.open_entry = [](void *context, uint64_t directory, uint64_t snapshot, uint64_t entry,
                          PulseDirectoryCompletionFn callback, void *user, uint64_t *output) {
        return Boundary([&] { return context ? static_cast<Impl *>(context)->SnapshotRequest(PULSE_DIRECTORY_OPEN_ENTRY,
            directory, snapshot, entry, 0, 0, callback, user, output) : PULSE_ERROR_ARGUMENT; });
    };
    table.watch = [](void *context, uint64_t directory, PulseDirectoryChangeFn callback, void *user, uint64_t *output) {
        return Boundary([&] { return context ? static_cast<Impl *>(context)->AddWatch(directory, callback, user, output) : PULSE_ERROR_ARGUMENT; });
    };
    table.unwatch = [](void *context, uint64_t watch) {
        return Boundary([&] { return context ? static_cast<Impl *>(context)->Unwatch(watch) : PULSE_ERROR_ARGUMENT; });
    };
    table.close = [](void *context, uint64_t directory) {
        return Boundary([&] { return context ? static_cast<Impl *>(context)->CloseDirectory(directory) : PULSE_ERROR_ARGUMENT; });
    };
    table.cancel = [](void *context, uint64_t request) {
        return Boundary([&] { return context ? static_cast<Impl *>(context)->Cancel(request) : PULSE_ERROR_ARGUMENT; });
    };
    return table;
}
PulseStatus DirectoriesAdapter::Query(uint32_t version, void *output, uint32_t size) const noexcept {
    return Boundary([&]() -> PulseStatus {
        if (!output) return PULSE_ERROR_ARGUMENT;
        if (version != PULSE_DIRECTORIES_VERSION_1 || size < sizeof(PulseDirectoriesV1)) return PULSE_ERROR_ABI;
        const auto status = impl_->Ready();
        if (status != PULSE_OK) return status;
        const auto table = Table(); std::memcpy(output, &table, sizeof(table));
        return PULSE_OK;
    });
}
PulseStatus DirectoriesAdapter::Close() noexcept { return impl_ ? impl_->Close() : PULSE_OK; }
std::optional<DirectoryTransferTarget> DirectoriesAdapter::ResolveTransferTarget(PulseDirectoryId id) const noexcept {
    try {
        const auto keep_alive = impl_;
        if (!keep_alive || !keep_alive->TransferTargetValid(id)) return std::nullopt;
        const auto found = keep_alive->directories.find(id);
        std::weak_ptr<Impl> weak = keep_alive;
        return DirectoryTransferTarget{found->second.path, [weak, id]() noexcept {
            try {
                const auto current = weak.lock();
                return current && current->TransferTargetValid(id);
            } catch (...) { return false; }
        }};
    } catch (...) { return std::nullopt; }
}
std::optional<DirectoryTransferSources> DirectoriesAdapter::ResolveTransferSources(PulseDirectoryId id,
    uint64_t snapshot_id, const uint64_t* entries, uint32_t count) const noexcept {
    try {
        const auto keep_alive = impl_;
        if (!keep_alive || keep_alive->Ready() != PULSE_OK || !id || !snapshot_id || !entries ||
            count == 0 || count > 64) return std::nullopt;
        const auto directory = keep_alive->directories.find(id);
        if (directory == keep_alive->directories.end() || !directory->second.snapshot ||
            directory->second.snapshot->id != snapshot_id) return std::nullopt;
        DirectoryTransferSources result;
        result.paths.reserve(count); result.sizes.reserve(count); result.modified.reserve(count);
        std::unordered_set<uint64_t> unique;
        for (uint32_t index = 0; index < count; ++index) {
            if (!entries[index] || !unique.insert(entries[index]).second) return std::nullopt;
            const auto found = std::find_if(directory->second.snapshot->rows.begin(), directory->second.snapshot->rows.end(),
                [&](const Entry& row) { return row.id == entries[index]; });
            if (found == directory->second.snapshot->rows.end() ||
                (found->flags & (PULSE_DIRECTORY_ENTRY_FOLDER | PULSE_DIRECTORY_ENTRY_REPARSE))) return std::nullopt;
            std::wstring path = directory->second.snapshot->path;
            if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
            path += found->leaf;
            result.paths.push_back(std::move(path)); result.sizes.push_back(found->size); result.modified.push_back(found->modified);
        }
        std::weak_ptr<Impl> weak = keep_alive;
        result.valid = [weak, id, snapshot_id]() noexcept {
            try {
                const auto current = weak.lock();
                if (!current || current->Ready() != PULSE_OK) return false;
                const auto found = current->directories.find(id);
                return found != current->directories.end() && found->second.snapshot &&
                    found->second.snapshot->id == snapshot_id;
            } catch (...) { return false; }
        };
        return result;
    } catch (...) { return std::nullopt; }
}
std::optional<DirectoryTreeTransferSource> DirectoriesAdapter::ResolveTreeTransferSource(PulseDirectoryId id,
    uint64_t snapshot_id, uint64_t entry_id) const noexcept {
    try {
        const auto keep_alive = impl_;
        if (!keep_alive || keep_alive->Ready() != PULSE_OK || !id || !snapshot_id || !entry_id)
            return std::nullopt;
        const auto directory = keep_alive->directories.find(id);
        if (directory == keep_alive->directories.end() || !directory->second.snapshot ||
            directory->second.snapshot->id != snapshot_id)
            return std::nullopt;
        const auto found = std::find_if(directory->second.snapshot->rows.begin(), directory->second.snapshot->rows.end(),
            [entry_id](const Entry& row) { return row.id == entry_id; });
        if (found == directory->second.snapshot->rows.end() ||
            !(found->flags & PULSE_DIRECTORY_ENTRY_FOLDER) || (found->flags & PULSE_DIRECTORY_ENTRY_REPARSE))
            return std::nullopt;
        std::wstring path = directory->second.snapshot->path;
        if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
        path += found->leaf;
        std::weak_ptr<Impl> weak = keep_alive;
        return DirectoryTreeTransferSource{std::move(path), found->leaf, found->size, found->modified,
            [weak, id, snapshot_id, entry_id]() noexcept {
                try {
                    const auto current = weak.lock();
                    if (!current || current->Ready() != PULSE_OK) return false;
                    const auto directory = current->directories.find(id);
                    if (directory == current->directories.end() || !directory->second.snapshot ||
                        directory->second.snapshot->id != snapshot_id) return false;
                    return std::any_of(directory->second.snapshot->rows.begin(), directory->second.snapshot->rows.end(),
                        [entry_id](const Entry& row) {
                            return row.id == entry_id && (row.flags & PULSE_DIRECTORY_ENTRY_FOLDER) &&
                                !(row.flags & PULSE_DIRECTORY_ENTRY_REPARSE);
                        });
                } catch (...) { return false; }
            }};
    } catch (...) { return std::nullopt; }
}
bool DirectoriesAdapter::CallbackActive() const noexcept { return impl_ && impl_->callbacks != 0; }
void DirectoriesAdapter::Poll() noexcept {
    const auto keep_alive = impl_;
    try { if (keep_alive) keep_alive->Poll(); } catch (...) { if (keep_alive) keep_alive->Close(); }
}
} // namespace pulse::extensions::native

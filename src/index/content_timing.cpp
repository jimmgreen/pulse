#include "content_timing.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <psapi.h>
#include <string>
#include <vector>

namespace pulse::index {
namespace {
constexpr size_t kLanes = static_cast<size_t>(ContentTimingLane::Count);
constexpr size_t kStages = static_cast<size_t>(ContentTimingStage::Count);
constexpr size_t kWorkers = 16;
constexpr size_t kSlowSamples = 8;
constexpr size_t kPathChars = 768;
constexpr uint64_t kRotateBytes = 2 * 1024 * 1024;
// Logs of earlier processes (current, .1 and .summary files) kept on disk.
constexpr size_t kRetainedForeignLogs = 24;
constexpr uint64_t kSlowMicros = 250000;
constexpr std::array<const char*, kLanes> kLaneNames{"pdf", "office", "text"};
constexpr std::array<const char*, kStages> kStageNames{
    "cache_wait", "metadata_prepare", "feed_enumerate", "queue_wait", "metadata_io", "extract", "match"};
struct Duration {
    uint64_t count = 0, sum_us = 0, max_us = 0;
    void Add(uint64_t elapsed) noexcept { ++count; sum_us += elapsed; max_us = (std::max)(max_us, elapsed); }
};
void AddDocument(DocumentReadMetrics& sum, const DocumentReadMetrics& metrics) noexcept {
    AddReaderMetrics(sum, metrics);
    sum.package_us += metrics.package_us;
    sum.enumerate_us += metrics.enumerate_us;
    sum.part_open_us += metrics.part_open_us;
    sum.parts_inclusive_us += metrics.parts_inclusive_us;
    sum.stream_read_us += metrics.stream_read_us;
    sum.stream_bytes += metrics.stream_bytes;
    sum.stream_reads += metrics.stream_reads;
    sum.filter_us += metrics.filter_us;
    sum.cpu_us += metrics.cpu_us;
    sum.pdf_load_us += metrics.pdf_load_us; sum.pdf_page_us += metrics.pdf_page_us;
    sum.pdf_text_us += metrics.pdf_text_us; sum.pdf_pages += metrics.pdf_pages; sum.pdf_fallbacks += metrics.pdf_fallbacks;
    sum.pdf_read_calls += metrics.pdf_read_calls; sum.pdf_requested_bytes += metrics.pdf_requested_bytes;
    sum.pdf_reader_us += metrics.pdf_reader_us; sum.pdf_file_reads += metrics.pdf_file_reads;
    sum.pdf_file_bytes += metrics.pdf_file_bytes; sum.pdf_file_io_us += metrics.pdf_file_io_us;
    sum.pdf_cache_hits += metrics.pdf_cache_hits; sum.pdf_map_views += metrics.pdf_map_views;
    sum.pdf_mapped_bytes += metrics.pdf_mapped_bytes; sum.pdf_map_fallbacks += metrics.pdf_map_fallbacks;
}

struct Lane {
    uint64_t queued = 0, started = 0, completed = 0, matched = 0, errors = 0, cached_files = 0;
    uint64_t first_enqueue_us = 0, first_start_us = 0, first_hit_us = 0;
    uint64_t last_completed_us = 0, completed_us = 0, reported_read_bytes = 0;
    std::array<Duration, kStages> stages{};
    struct ErrorCount { DWORD code = 0; uint64_t count = 0; };
    std::array<ErrorCount, 16> error_codes{};
    uint64_t other_errors = 0;
    DocumentReadMetrics document;
};
struct Worker {
    uint64_t id = 0, started_us = 0, enqueued_us = 0, stage_started_us = 0;
    ContentTimingLane lane = ContentTimingLane::Text;
    ContentTimingStage stage = ContentTimingStage::MetadataIo;
    std::array<wchar_t, kPathChars> path{};
    std::array<uint64_t, kStages> stages{};
};
struct SlowSample {
    uint64_t duration_us = 0;
    ContentTimingLane lane = ContentTimingLane::Text;
    DWORD error = ERROR_SUCCESS;
    bool matched = false;
    std::array<wchar_t, kPathChars> path{};
    std::array<uint64_t, kStages> stages{};
};
struct Snapshot {
    std::array<Lane, kLanes> lanes{};
    std::array<Duration, kStages> stages{};
    std::array<Worker, kWorkers> workers{};
    std::array<SlowSample, kSlowSamples> slow{};
    struct ParserFailure {
        uint64_t at_us = 0;
        ContentTimingLane lane = ContentTimingLane::Pdf;
        DWORD error = 0;
        std::array<wchar_t, kPathChars> path{};
        DocumentReadMetrics metrics;
    };
    std::array<ParserFailure, 16> parser_failures{};
    uint64_t parser_failure_count = 0;
    uint64_t first_hit_us = 0, dropped_workers = 0;
    uint64_t mapping_budget_bytes = 0, mapping_effective_bytes = 0;
    bool enumeration_complete = false;
};
bool Valid(ContentTimingLane lane) noexcept { return static_cast<size_t>(lane) < kLanes; }
bool Valid(ContentTimingStage stage) noexcept { return static_cast<size_t>(stage) < kStages; }
uint64_t Since(uint64_t now, uint64_t start) noexcept { return now >= start ? now - start : 0; }
void CopyPath(std::array<wchar_t, kPathChars>& destination, std::wstring_view path) noexcept {
    destination.fill(L'\0');
    if (path.size() < destination.size()) std::copy(path.begin(), path.end(), destination.begin());
    else {
        constexpr size_t tail = 256, head = kPathChars - tail - 4;
        std::copy_n(path.begin(), head, destination.begin());
        destination[head] = L'.'; destination[head + 1] = L'.'; destination[head + 2] = L'.';
        std::copy_n(path.end() - tail, tail, destination.begin() + head + 3);
    }
}
std::wstring Environment(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed || needed > 32768) return {};
    std::wstring value(needed, L'\0');
    const DWORD count = GetEnvironmentVariableW(name, value.data(), needed);
    if (!count || count >= needed) return {};
    value.resize(count);
    return value;
}
// Each process writes its own content-timing-<pid> files; without pruning they
// accumulate forever. Keep the newest files of other processes only.
void PruneForeignLogs(const std::filesystem::path& directory) noexcept {
    try {
        const std::wstring own = L"content-timing-" + std::to_wstring(GetCurrentProcessId()) + L".";
        struct Entry { std::filesystem::path path; std::filesystem::file_time_type time; };
        std::vector<Entry> entries;
        std::error_code error;
        for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
            const auto name = it->path().filename().wstring();
            if (!name.starts_with(L"content-timing-") || name.starts_with(own)) continue;
            if (!name.ends_with(L".jsonl") && !name.ends_with(L".jsonl.1")) continue;
            std::error_code status;
            if (it->is_symlink(status) || !it->is_regular_file(status)) continue;
            const auto time = it->last_write_time(status);
            if (!status) entries.push_back({it->path(), time});
        }
        if (entries.size() <= kRetainedForeignLogs) return;
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time > b.time; });
        for (size_t i = kRetainedForeignLogs; i < entries.size(); ++i) DeleteFileW(entries[i].path.c_str());
    } catch (...) {}
}
std::wstring LogPath() {
    auto directory = Environment(L"PULSE_CONTENT_TIMING_DIR");
    if (directory.empty()) {
        directory = Environment(L"LOCALAPPDATA");
        if (directory.empty()) return {};
        directory += L"\\Pulse\\logs";
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return {};
    PruneForeignLogs(directory);
    return (std::filesystem::path(directory) /
        (L"content-timing-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl")).wstring();
}
void JsonString(std::string& out, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    out += '"';
}
void JsonPath(std::string& out, const std::array<wchar_t, kPathChars>& path) {
    const int length = static_cast<int>(wcsnlen_s(path.data(), path.size()));
    const int size = WideCharToMultiByte(CP_UTF8, 0, path.data(), length, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>((std::max)(size, 0)), '\0');
    if (size) WideCharToMultiByte(CP_UTF8, 0, path.data(), length, utf8.data(), size, nullptr, nullptr);
    JsonString(out, utf8);
}
void Number(std::string& out, const char* key, uint64_t value) {
    out += ",\""; out += key; out += "\":"; out += std::to_string(value);
}
void Durations(std::string& out, const std::array<Duration, kStages>& stages) {
    out += "{\"units\":\"microseconds\"";
    for (size_t i = 0; i < stages.size(); ++i) {
        out += ",\""; out += kStageNames[i]; out += "\":{\"count\":" + std::to_string(stages[i].count);
        Number(out, "sum_us", stages[i].sum_us); Number(out, "max_us", stages[i].max_us); out += '}';
    }
    out += '}';
}
void WriteLog(const std::wstring& path, const std::string& record) noexcept {
    try {
        if (path.empty() || record.size() > kRotateBytes) return;
        static std::mutex sink_mutex;
        std::lock_guard lock(sink_mutex);
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size)) { CloseHandle(file); return; }
        if (static_cast<uint64_t>(size.QuadPart) + record.size() > kRotateBytes) {
            CloseHandle(file);
            const auto backup = path + L".1";
            if (!MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
            file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return;
        } else {
            LARGE_INTEGER end{};
            if (!SetFilePointerEx(file, end, nullptr, FILE_END)) { CloseHandle(file); return; }
        }
        DWORD written = 0;
        WriteFile(file, record.data(), static_cast<DWORD>(record.size()), &written, nullptr);
        CloseHandle(file);
    } catch (...) {}
}
}

struct ContentTiming::Impl {
    std::mutex mutex;
    Snapshot state;
    std::wstring path;
    uint64_t generation = 0, started_us = 0, last_flush_us = 0, next_id = 1;
    uint64_t scanned = 0, total = 0, hits = 0;
    bool finished = false;
    Impl(uint64_t value) : path(LogPath()), generation(value), started_us(NowMicros()) {}
    void Add(ContentTimingStage stage, uint64_t elapsed, ContentTimingLane lane) noexcept {
        if (!Valid(stage)) return;
        const auto index = static_cast<size_t>(stage);
        state.stages[index].Add(elapsed);
        if (Valid(lane)) state.lanes[static_cast<size_t>(lane)].stages[index].Add(elapsed);
    }
    Worker* Find(FileToken token) noexcept {
        if (!token.id) return nullptr;
        for (auto& worker : state.workers) if (worker.id == token.id) return &worker;
        return nullptr;
    }
    void EndStage(Worker& worker, uint64_t now) noexcept {
        const uint64_t elapsed = Since(now, worker.stage_started_us);
        Add(worker.stage, elapsed, worker.lane);
        if (Valid(worker.stage)) worker.stages[static_cast<size_t>(worker.stage)] += elapsed;
        worker.stage_started_us = now;
    }
    void Write(const char* event, uint64_t new_scanned, uint64_t new_total, uint64_t new_hits,
               bool force, bool terminal, bool cancelled, DWORD error) {
        Snapshot snapshot;
        const uint64_t now = NowMicros();
        {
            std::lock_guard lock(mutex);
            if (finished) return;
            scanned = new_scanned; total = new_total; hits = new_hits;
            if (hits && !state.first_hit_us) state.first_hit_us = (std::max)(uint64_t{1}, Since(now, started_us));
            if (!force && last_flush_us && Since(now, last_flush_us) < 1000000) return;
            last_flush_us = now;
            if (terminal) finished = true;
            snapshot = state;
        }
        std::string record;
        record.reserve(12000);
        record = "{\"event\":"; JsonString(record, event);
        Number(record, "pid", GetCurrentProcessId()); Number(record, "generation", generation);
        Number(record, "qpc_start_us", started_us); Number(record, "elapsed_us", Since(now, started_us));
        Number(record, "scanned", new_scanned); Number(record, "total", new_total); Number(record, "hits", new_hits);
        Number(record, "first_hit_us", snapshot.first_hit_us); Number(record, "error", error);
        record += snapshot.enumeration_complete ? ",\"enumeration_complete\":true" : ",\"enumeration_complete\":false";
        Number(record, "mapping_budget_bytes", snapshot.mapping_budget_bytes);
        Number(record, "mapping_effective_bytes", snapshot.mapping_effective_bytes);
        PROCESS_MEMORY_COUNTERS_EX process{};
        process.cb = sizeof(process);
        const bool process_memory_ok = K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)) != FALSE;
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        const bool physical_memory_ok = GlobalMemoryStatusEx(&memory) != FALSE;
        Number(record, "working_set_bytes", static_cast<uint64_t>(process.WorkingSetSize));
        Number(record, "private_bytes", static_cast<uint64_t>(process.PrivateUsage));
        Number(record, "available_physical_bytes", memory.ullAvailPhys);
        record += process_memory_ok ? ",\"process_memory_ok\":true" : ",\"process_memory_ok\":false";
        record += physical_memory_ok ? ",\"physical_memory_ok\":true" : ",\"physical_memory_ok\":false";
        record += cancelled ? ",\"cancelled\":true" : ",\"cancelled\":false";
        SYSTEMTIME utc{}; GetSystemTime(&utc);
        char timestamp[40]{};
        sprintf_s(timestamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay,
            utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
        record += ",\"utc\":"; JsonString(record, timestamp);
        record += ",\"stages\":"; Durations(record, snapshot.stages);
        record += ",\"lanes\":{";
        for (size_t i = 0; i < kLanes; ++i) {
            if (i) record += ',';
            const auto& lane = snapshot.lanes[i];
            JsonString(record, kLaneNames[i]); record += ":{\"queued\":" + std::to_string(lane.queued);
            Number(record, "started", lane.started); Number(record, "completed", lane.completed);
            Number(record, "matched", lane.matched); Number(record, "errors", lane.errors);
            Number(record, "cached_files", lane.cached_files);
            record += ",\"error_codes\":{";
            bool have_error = false;
            for (const auto& count : lane.error_codes) if (count.count) {
                if (have_error) record += ',';
                have_error = true;
                JsonString(record, std::to_string(count.code)); record += ':' + std::to_string(count.count);
            }
            record += '}'; Number(record, "other_errors", lane.other_errors);
            Number(record, "first_enqueue_us", lane.first_enqueue_us); Number(record, "first_start_us", lane.first_start_us);
            Number(record, "first_hit_us", lane.first_hit_us);
            Number(record, "completed_us", lane.completed_us);
            Number(record, "reported_read_bytes", lane.reported_read_bytes);
            record += ",\"document\":{\"package_us\":" + std::to_string(lane.document.package_us);
            Number(record, "enumerate_us", lane.document.enumerate_us);
            Number(record, "parts_inclusive_us", lane.document.parts_inclusive_us);
            Number(record, "part_open_us", lane.document.part_open_us);
            Number(record, "stream_read_us", lane.document.stream_read_us);
            Number(record, "stream_bytes", lane.document.stream_bytes);
            Number(record, "stream_reads", lane.document.stream_reads);
            Number(record, "filter_us", lane.document.filter_us);
            Number(record, "cpu_us", lane.document.cpu_us);
            Number(record, "pdf_load_us", lane.document.pdf_load_us);
            Number(record, "pdf_page_us", lane.document.pdf_page_us);
            Number(record, "pdf_text_us", lane.document.pdf_text_us);
            Number(record, "pdf_pages", lane.document.pdf_pages);
            Number(record, "pdf_fallbacks", lane.document.pdf_fallbacks);
            Number(record, "reader_queue_us", lane.document.reader_queue_us);
            Number(record, "admission_wait_us", lane.document.admission_wait_us);
            Number(record, "process_start_us", lane.document.process_start_us);
            Number(record, "response_us", lane.document.response_us);
            Number(record, "process_stop_us", lane.document.process_stop_us);
            Number(record, "process_starts", lane.document.process_starts);
            Number(record, "read_attempts", lane.document.read_attempts);
            Number(record, "memory_recycles", lane.document.memory_recycles);
            Number(record, "fresh_filter_fallbacks", lane.document.fresh_filter_fallbacks);
            Number(record, "fresh_filter_recoveries", lane.document.fresh_filter_recoveries);
            Number(record, "fresh_filter_skips", lane.document.fresh_filter_skips);
            Number(record, "last_fresh_filter_first_error", lane.document.fresh_filter_first_error);
            Number(record, "last_fresh_filter_first_exit", lane.document.fresh_filter_first_exit);
            Number(record, "attempt_cpu_us", lane.document.attempt_cpu_us);
            Number(record, "last_released_private_bytes", lane.document.released_private_bytes);
            Number(record, "last_released_memory_known", lane.document.released_memory_known);
            Number(record, "pdf_read_calls", lane.document.pdf_read_calls);
            Number(record, "pdf_requested_bytes", lane.document.pdf_requested_bytes);
            Number(record, "pdf_reader_us", lane.document.pdf_reader_us);
            Number(record, "pdf_file_reads", lane.document.pdf_file_reads);
            Number(record, "pdf_file_bytes", lane.document.pdf_file_bytes);
            Number(record, "pdf_file_io_us", lane.document.pdf_file_io_us);
            Number(record, "pdf_cache_hits", lane.document.pdf_cache_hits);
            Number(record, "pdf_map_views", lane.document.pdf_map_views);
            Number(record, "pdf_mapped_bytes", lane.document.pdf_mapped_bytes);
            Number(record, "pdf_map_fallbacks", lane.document.pdf_map_fallbacks);
            record += '}';
            uint64_t active_count = 0, oldest_age = 0;
            const Worker* oldest = nullptr;
            for (const auto& worker : snapshot.workers) if (worker.id && static_cast<size_t>(worker.lane) == i) {
                ++active_count;
                const auto age = Since(now, worker.started_us);
                if (!oldest || age > oldest_age) { oldest = &worker; oldest_age = age; }
            }
            Number(record, "active", active_count); Number(record, "oldest_active_us", oldest_age);
            if (oldest) {
                record += ",\"oldest_stage\":"; JsonString(record, kStageNames[static_cast<size_t>(oldest->stage)]);
                Number(record, "oldest_stage_us", Since(now, oldest->stage_started_us));
                if (oldest_age >= kSlowMicros) { record += ",\"oldest_path\":"; JsonPath(record, oldest->path); }
            }
            record += ",\"stages\":"; Durations(record, lane.stages); record += '}';
        }
        record += "},\"slow_samples\":[";
        bool comma = false;
        for (const auto& slow : snapshot.slow) if (slow.duration_us) {
            if (comma) record += ',';
            comma = true;
            record += "{\"lane\":"; JsonString(record, kLaneNames[static_cast<size_t>(slow.lane)]);
            Number(record, "duration_us", slow.duration_us); Number(record, "error", slow.error);
            record += slow.matched ? ",\"matched\":true" : ",\"matched\":false";
            record += ",\"path\":"; JsonPath(record, slow.path);
            record += ",\"stages_us\":{";
            for (size_t i = 0; i < kStages; ++i) {
                if (i) record += ',';
                JsonString(record, kStageNames[i]); record += ':'; record += std::to_string(slow.stages[i]);
            }
            record += "}}";
        }
        record += ']'; Number(record, "parser_failure_count", snapshot.parser_failure_count);
        record += ",\"parser_failures\":[";
        bool first_failure = true;
        for (const auto& failure : snapshot.parser_failures) {
            if (!failure.at_us) continue;
            if (!first_failure) record += ',';
            first_failure = false;
            record += "{\"path\":"; JsonPath(record, failure.path);
            record += ",\"lane\":"; JsonString(record, kLaneNames[static_cast<size_t>(failure.lane)]);
            Number(record, "at_us", failure.at_us); Number(record, "error", failure.error);
            Number(record, "stage", failure.metrics.child_failure_stage);
            Number(record, "exit_known", failure.metrics.child_exit_known);
            Number(record, "exit_code", failure.metrics.child_exit_code);
            Number(record, "peak_private_bytes", failure.metrics.child_peak_private_bytes);
            Number(record, "private_bytes", failure.metrics.child_private_bytes);
            Number(record, "memory_known", failure.metrics.child_memory_known);
            Number(record, "read_attempts", failure.metrics.read_attempts);
            Number(record, "fresh_filter_fallbacks", failure.metrics.fresh_filter_fallbacks);
            Number(record, "fresh_filter_recoveries", failure.metrics.fresh_filter_recoveries);
            Number(record, "fresh_filter_skips", failure.metrics.fresh_filter_skips);
            Number(record, "fresh_filter_first_error", failure.metrics.fresh_filter_first_error);
            Number(record, "fresh_filter_first_exit", failure.metrics.fresh_filter_first_exit);
            record += '}';
        }
        record += ']'; Number(record, "dropped_workers", snapshot.dropped_workers); record += "}\r\n";
        WriteLog(path, record);
        if (terminal) WriteLog(path + L".summary.jsonl", record);
    }
};

uint64_t ContentTiming::NowMicros() noexcept {
    static const uint64_t frequency = []() noexcept {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0 ? static_cast<uint64_t>(value.QuadPart) : 0;
    }();
    LARGE_INTEGER value{};
    if (!frequency || !QueryPerformanceCounter(&value) || value.QuadPart < 0) return GetTickCount64() * 1000;
    const auto ticks = static_cast<uint64_t>(value.QuadPart);
    return (ticks / frequency) * 1000000 + (ticks % frequency) * 1000000 / frequency;
}
ContentTiming::ContentTiming(uint64_t generation) noexcept {
    try {
        impl_ = std::make_unique<Impl>(generation);
        impl_->Write("start", 0, 0, 0, true, false, false, ERROR_SUCCESS);
    } catch (...) { impl_.reset(); }
}
ContentTiming::~ContentTiming() {
    if (!impl_) return;
    try { impl_->Write("abandoned", impl_->scanned, impl_->total, impl_->hits, true, true, true, ERROR_OPERATION_ABORTED); }
    catch (...) {}
}
uint64_t ContentTiming::Enqueue(ContentTimingLane lane) noexcept {
    const auto now = NowMicros();
    try {
        if (!impl_ || !Valid(lane)) return now;
        std::lock_guard lock(impl_->mutex);
        auto& value = impl_->state.lanes[static_cast<size_t>(lane)];
        ++value.queued;
        value.completed_us = 0;
        if (!value.first_enqueue_us) value.first_enqueue_us = (std::max)(uint64_t{1}, Since(now, impl_->started_us));
    } catch (...) {}
    return now;
}
ContentTiming::FileToken ContentTiming::BeginFile(ContentTimingLane lane, std::wstring_view path, uint64_t enqueued_us) noexcept {
    try {
        if (!impl_ || !Valid(lane)) return {};
        const auto now = NowMicros();
        std::lock_guard lock(impl_->mutex);
        auto& value = impl_->state.lanes[static_cast<size_t>(lane)];
        ++value.started;
        if (!value.first_start_us) value.first_start_us = (std::max)(uint64_t{1}, Since(now, impl_->started_us));
        const auto queue_wait = enqueued_us ? Since(now, enqueued_us) : 0;
        impl_->Add(ContentTimingStage::QueueWait, queue_wait, lane);
        for (auto& worker : impl_->state.workers) if (!worker.id) {
            worker = {};
            worker.id = impl_->next_id++;
            worker.lane = lane; worker.started_us = now; worker.enqueued_us = enqueued_us;
            worker.stage_started_us = now; worker.stage = ContentTimingStage::MetadataIo;
            worker.stages[static_cast<size_t>(ContentTimingStage::QueueWait)] = queue_wait;
            CopyPath(worker.path, path);
            return {worker.id};
        }
        ++impl_->state.dropped_workers;
    } catch (...) {}
    return {};
}
void ContentTiming::FileStage(FileToken token, ContentTimingStage stage) noexcept {
    try {
        if (!impl_ || !Valid(stage)) return;
        const auto now = NowMicros();
        std::lock_guard lock(impl_->mutex);
        if (auto* worker = impl_->Find(token)) { impl_->EndStage(*worker, now); worker->stage = stage; }
    } catch (...) {}
}
void ContentTiming::EndFile(FileToken token, bool matched, DWORD error, uint64_t reported_bytes,
                            const DocumentReadMetrics* metrics) noexcept {
    try {
        if (!impl_) return;
        const auto now = NowMicros();
        std::lock_guard lock(impl_->mutex);
        auto* worker = impl_->Find(token);
        if (!worker) return;
        impl_->EndStage(*worker, now);
        auto& lane = impl_->state.lanes[static_cast<size_t>(worker->lane)];
        ++lane.completed;
        lane.reported_read_bytes += reported_bytes;
        if (metrics) AddDocument(lane.document, *metrics);
        if (metrics && metrics->child_failure_stage && error != ERROR_CANCELLED) {
            auto& failure = impl_->state.parser_failures[impl_->state.parser_failure_count++ % impl_->state.parser_failures.size()];
            failure.at_us = (std::max)(uint64_t{1}, Since(now, impl_->started_us));
            failure.lane = worker->lane; failure.error = error;
            failure.path = worker->path; failure.metrics = *metrics;
        }
        lane.last_completed_us = (std::max)(uint64_t{1}, Since(now, impl_->started_us));
        if (impl_->state.enumeration_complete && lane.completed == lane.queued)
            lane.completed_us = lane.last_completed_us;
        if (error) {
            ++lane.errors;
            auto found = std::find_if(lane.error_codes.begin(), lane.error_codes.end(),
                [&](const auto& value) { return value.code == error || !value.count; });
            if (found != lane.error_codes.end()) { found->code = error; ++found->count; }
            else ++lane.other_errors;
        }
        if (matched) {
            ++lane.matched;
            if (!lane.first_hit_us) lane.first_hit_us = (std::max)(uint64_t{1}, Since(now, impl_->started_us));
        }
        const auto duration = Since(now, worker->started_us);
        if (duration >= kSlowMicros) {
            auto& samples = impl_->state.slow;
            const auto slot = std::min_element(samples.begin(), samples.end(),
                [](const auto& a, const auto& b) { return a.duration_us < b.duration_us; });
            if (duration > slot->duration_us) {
                slot->duration_us = duration; slot->lane = worker->lane; slot->error = error; slot->matched = matched;
                slot->path = worker->path; slot->stages = worker->stages;
            }
        }
        worker->id = 0;
    } catch (...) {}
}
void ContentTiming::ReuseCachedFile(ContentTimingLane lane) noexcept {
    try {
        if (!impl_ || !Valid(lane)) return;
        std::lock_guard lock(impl_->mutex);
        ++impl_->state.lanes[static_cast<size_t>(lane)].cached_files;
    } catch (...) {}
}
void ContentTiming::EnumerationComplete() noexcept {
    try {
        if (!impl_) return;
        std::lock_guard lock(impl_->mutex);
        impl_->state.enumeration_complete = true;
        for (auto& lane : impl_->state.lanes)
            if (lane.queued && lane.completed == lane.queued) lane.completed_us = lane.last_completed_us;
    } catch (...) {}
}
void ContentTiming::RecordDocumentMetrics(FileToken token, const DocumentReadMetrics& metrics) noexcept {
    try {
        if (!impl_) return;
        std::lock_guard lock(impl_->mutex);
        if (auto* worker = impl_->Find(token)) {
            auto& sum = impl_->state.lanes[static_cast<size_t>(worker->lane)].document;
            AddDocument(sum, metrics);
        }
    } catch (...) {}
}
void ContentTiming::RecordReadBytes(FileToken token, uint64_t bytes) noexcept {
    try {
        if (!impl_) return;
        std::lock_guard lock(impl_->mutex);
        if (auto* worker = impl_->Find(token))
            impl_->state.lanes[static_cast<size_t>(worker->lane)].reported_read_bytes += bytes;
    } catch (...) {}
}
void ContentTiming::RecordStage(ContentTimingStage stage, uint64_t started_us, ContentTimingLane lane) noexcept {
    try {
        if (!impl_ || !Valid(stage)) return;
        const auto now = NowMicros();
        std::lock_guard lock(impl_->mutex);
        impl_->Add(stage, Since(now, started_us), lane);
    } catch (...) {}
}
void ContentTiming::FlushProgress(uint64_t scanned, uint64_t total, uint64_t hits, bool force) noexcept {
    try { if (impl_) impl_->Write("progress", scanned, total, hits, force, false, false, ERROR_SUCCESS); }
    catch (...) {}
}
void ContentTiming::SetCacheMapping(uint64_t budget_bytes, uint64_t effective_bytes) noexcept {
    try {
        if (!impl_) return;
        std::lock_guard lock(impl_->mutex);
        impl_->state.mapping_budget_bytes = budget_bytes;
        impl_->state.mapping_effective_bytes = effective_bytes;
    } catch (...) {}
}
void ContentTiming::Finish(uint64_t scanned, uint64_t total, uint64_t hits, bool cancelled, DWORD error) noexcept {
    try { if (impl_) impl_->Write("finish", scanned, total, hits, true, true, cancelled, error); }
    catch (...) {}
}
}

#include "../index/content_timing.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {
using pulse::index::ContentTiming;
using pulse::index::ContentTimingLane;
using pulse::index::ContentTimingStage;
int failures = 0;
void Check(bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; }
std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
struct Fixture {
    std::filesystem::path directory;
    std::wstring old_environment;
    Fixture() {
        wchar_t previous[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"PULSE_CONTENT_TIMING_DIR", previous, 32768);
        if (length && length < 32768) old_environment.assign(previous, length);
        directory = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"content_timing_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(directory);
        SetEnvironmentVariableW(L"PULSE_CONTENT_TIMING_DIR", directory.c_str());
    }
    ~Fixture() {
        SetEnvironmentVariableW(L"PULSE_CONTENT_TIMING_DIR", old_environment.empty() ? nullptr : old_environment.c_str());
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    std::filesystem::path Log() const { return directory / (L"content-timing-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl"); }
};
}
int main() {
    Fixture fixture;
    DWORD initial_handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &initial_handles);
    const auto clock_start = ContentTiming::NowMicros();
    Sleep(5);
    Check(ContentTiming::NowMicros() > clock_start, "QPC microsecond clock increases");
    {
        ContentTiming timing(701);
        timing.SetCacheMapping(16ull * 1024 * 1024 * 1024, 2 * 1024 * 1024);
        const auto initial_size = std::filesystem::file_size(fixture.Log());
        timing.RecordStage(ContentTimingStage::CacheWait, ContentTiming::NowMicros() - 15000);
        timing.RecordStage(ContentTimingStage::MetadataPrepare, ContentTiming::NowMicros() - 3000);
        timing.RecordStage(ContentTimingStage::FeedEnumerate, ContentTiming::NowMicros() - 2000);
        std::vector<std::jthread> workers;
        constexpr unsigned count = 6, files = 200;
        const auto aggregate_start = ContentTiming::NowMicros();
        for (unsigned i = 0; i < count; ++i) workers.emplace_back([&] {
            for (unsigned j = 0; j < files; ++j) {
                const auto queued = timing.Enqueue(ContentTimingLane::Text);
                const auto token = timing.BeginFile(ContentTimingLane::Text, L"fast-path-not-logged.txt", queued);
                timing.FileStage(token, ContentTimingStage::Extract);
                timing.FileStage(token, ContentTimingStage::Match);
                timing.EndFile(token, true);
                timing.ReuseCachedFile(ContentTimingLane::Text);
            }
        });
        workers.clear();
        std::printf("1200 concurrent in-memory file records: %llu us\n",
            static_cast<unsigned long long>(ContentTiming::NowMicros() - aggregate_start));
        Check(std::filesystem::file_size(fixture.Log()) == initial_size, "workers never write per-file log records");
        timing.FlushProgress(count * files, count * files + 1, count * files);
        Check(std::filesystem::file_size(fixture.Log()) == initial_size, "progress snapshots throttle to one second");
        const auto queued = timing.Enqueue(ContentTimingLane::Office);
        const auto token = timing.BeginFile(ContentTimingLane::Office, L"C:\\slow\\quote\"\nreport.docx", queued - 20000);
        timing.FileStage(token, ContentTimingStage::Extract);
        Sleep(270);
        timing.FlushProgress(count * files, count * files + 1, count * files, true);
        auto log = Read(fixture.Log());
        Check(log.find("\"mapping_budget_bytes\":17179869184,\"mapping_effective_bytes\":2097152") != std::string::npos &&
              log.find("\"working_set_bytes\":") != std::string::npos && log.find("\"private_bytes\":") != std::string::npos &&
              log.find("\"available_physical_bytes\":") != std::string::npos &&
              log.find("\"process_memory_ok\":true,\"physical_memory_ok\":true") != std::string::npos,
              "snapshots include mapping budget, effective mapping and current memory counters");
        Check(log.find("\"oldest_stage\":\"extract\"") != std::string::npos &&
              log.find("\"oldest_path\":") != std::string::npos && log.find("quote\\\"\\u000areport.docx") != std::string::npos,
              "slow active worker shows stage, age and escaped bounded path");
        timing.FileStage(token, ContentTimingStage::Match);
        timing.RecordReadBytes(token, 4096);
        pulse::index::DocumentReadMetrics metrics;
        metrics.package_us = 7; metrics.stream_bytes = 1024; metrics.filter_us = 11;
        metrics.pdf_load_us = 13; metrics.pdf_page_us = 17; metrics.pdf_text_us = 19;
        metrics.pdf_pages = 23; metrics.pdf_fallbacks = 1;
        metrics.pdf_read_calls = 101; metrics.pdf_requested_bytes = 102;
        metrics.pdf_file_reads = 103; metrics.pdf_file_bytes = 104;
        metrics.pdf_reader_us = 105; metrics.pdf_file_io_us = 106; metrics.pdf_cache_hits = 107;
        metrics.reader_queue_us = 29; metrics.admission_wait_us = 31; metrics.process_start_us = 37;
        metrics.response_us = 41; metrics.process_stop_us = 43; metrics.process_starts = 3;
        metrics.read_attempts = 4; metrics.memory_recycles = 1; metrics.attempt_cpu_us = 113;
        metrics.fresh_filter_fallbacks = 2; metrics.fresh_filter_recoveries = 1; metrics.fresh_filter_skips = 1;
        metrics.fresh_filter_first_error = ERROR_NOT_ENOUGH_MEMORY; metrics.fresh_filter_first_exit = 0xe0000008;
        metrics.released_private_bytes = 114; metrics.released_memory_known = 1;
        timing.RecordDocumentMetrics(token, metrics);
        timing.EndFile(token, false, ERROR_BAD_FORMAT);
        const auto failed = timing.BeginFile(ContentTimingLane::Pdf, L"fast-pipe-failure.pdf", timing.Enqueue(ContentTimingLane::Pdf));
        pulse::index::DocumentReadMetrics failure;
        failure.child_failure_stage = 2; failure.child_exit_known = 1; failure.child_exit_code = 0xe1234567;
        failure.child_memory_known = 1; failure.child_peak_private_bytes = 33554432;
        failure.read_attempts = 1;
        timing.EndFile(failed, false, ERROR_BROKEN_PIPE, 0, &failure);
        timing.EnumerationComplete();
        timing.Finish(count * files + 2, count * files + 2, count * files, true, ERROR_CANCELLED);
        Check(Read(fixture.Log().wstring() + L".summary.jsonl").find("\"event\":\"finish\"") != std::string::npos,
              "terminal summary survives separately from progress rotation");
        const auto final_size = std::filesystem::file_size(fixture.Log());
        timing.Finish(0, 0, 0, false);
        timing.FlushProgress(0, 0, 0, true);
        Check(std::filesystem::file_size(fixture.Log()) == final_size, "finish is terminal and idempotent");
        log = Read(fixture.Log());
        Check(log.find("\"read_attempts\":4,\"memory_recycles\":1") != std::string::npos &&
              log.find("\"attempt_cpu_us\":113") != std::string::npos &&
              log.find("\"last_released_private_bytes\":114,\"last_released_memory_known\":1") != std::string::npos,
              "attempt CPU and released-current memory remain distinct in diagnostics");
        Check(log.find("\"fresh_filter_fallbacks\":2,\"fresh_filter_recoveries\":1,\"fresh_filter_skips\":1") != std::string::npos &&
              log.find("\"last_fresh_filter_first_error\":8,\"last_fresh_filter_first_exit\":" + std::to_string(0xe0000008u)) != std::string::npos,
              "fresh-process compatibility fallback keeps counts and original failure separately");
        Check(log.find("\"pdf_read_calls\":101") != std::string::npos &&
              log.find("\"pdf_file_reads\":103") != std::string::npos &&
              log.find("\"pdf_file_bytes\":104") != std::string::npos &&
              log.find("\"pdf_reader_us\":105") != std::string::npos &&
              log.find("\"pdf_file_io_us\":106") != std::string::npos,
              "PDF callback and actual file-read metrics survive aggregation");
        Check(log.find("\"parser_failure_count\":1") != std::string::npos &&
              log.find("\"path\":\"fast-pipe-failure.pdf\"") != std::string::npos &&
              log.find("\"exit_code\":" + std::to_string(0xe1234567u)) != std::string::npos &&
              log.find("\"peak_private_bytes\":33554432") != std::string::npos,
              "fast broken-pipe failures retain path, exit and memory outside slow samples");
        Check(log.find("\"pdf_load_us\":13") != std::string::npos && log.find("\"pdf_page_us\":17") != std::string::npos &&
              log.find("\"pdf_text_us\":19") != std::string::npos && log.find("\"pdf_pages\":23") != std::string::npos &&
              log.find("\"pdf_fallbacks\":1") != std::string::npos, "PDF phase and fallback diagnostics survive aggregation");
        Check(log.find("\"reader_queue_us\":29") != std::string::npos &&
              log.find("\"admission_wait_us\":31") != std::string::npos &&
              log.find("\"process_start_us\":37") != std::string::npos &&
              log.find("\"response_us\":41") != std::string::npos &&
              log.find("\"process_stop_us\":43") != std::string::npos &&
              log.find("\"process_starts\":3") != std::string::npos,
              "parser queue, admission, startup, response and release diagnostics survive aggregation");
        Check(log.find("\"enumeration_complete\":true") != std::string::npos &&
              log.find("\"reported_read_bytes\":4096") != std::string::npos &&
              log.find("\"package_us\":7") != std::string::npos &&
              log.find("\"stream_bytes\":1024") != std::string::npos,
              "sealed lane completion and extraction counters survive in terminal record");
        Check(log.find("\"queued\":1200,\"started\":1200,\"completed\":1200,\"matched\":1200,\"errors\":0") != std::string::npos,
              "concurrent class counters retain every file");
        Check(log.find("\"error_codes\":{\"11\":1}") != std::string::npos,
              "class diagnostics distinguish format errors from timeout and access errors");
        Check(log.find("\"cached_files\":1200") != std::string::npos,
              "concurrent cache reuse counters report avoided extraction");
        Check(log.find("\"event\":\"finish\"") != std::string::npos && log.find("\"cancelled\":true") != std::string::npos &&
              log.find("\"cache_wait\":{\"count\":1") != std::string::npos &&
              log.find("\"slow_samples\":[{\"lane\":\"office\"") != std::string::npos &&
              log.find("\"error\":11") != std::string::npos && log.find("fast-path-not-logged") == std::string::npos,
              "terminal cancellation, phase aggregates and slow error sample are recorded without fast paths");
    }
    {
        ContentTiming timing(704);
        const auto token = timing.BeginFile(ContentTimingLane::Pdf, L"recovered-filter.pdf", timing.Enqueue(ContentTimingLane::Pdf));
        pulse::index::DocumentReadMetrics recovered;
        recovered.child_failure_stage = 4; recovered.child_exit_known = 1; recovered.child_exit_code = STILL_ACTIVE;
        recovered.read_attempts = 2; recovered.fresh_filter_fallbacks = 1; recovered.fresh_filter_recoveries = 1;
        recovered.fresh_filter_first_error = ERROR_NOT_ENOUGH_MEMORY;
        timing.EndFile(token, true, ERROR_SUCCESS, 0, &recovered);
        timing.Finish(1, 1, 1, false);
        const auto log = Read(fixture.Log());
        const auto path = log.find("\"path\":\"recovered-filter.pdf\"");
        const auto record = path == std::string::npos ? std::string{} : log.substr(path, log.find('}', path) - path);
        Check(record.find("\"error\":0") != std::string::npos &&
              record.find("\"fresh_filter_recoveries\":1") != std::string::npos &&
              record.find("\"fresh_filter_first_error\":8") != std::string::npos,
              "recovered document records final success separately from initial OOM");
    }
    // Seed a sparse-sized log to cross the production limit without generating
    // thousands of files or changing the logger's real rotation threshold.
    {
        HANDLE file = CreateFileW(fixture.Log().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        LARGE_INTEGER end{}; end.QuadPart = 2 * 1024 * 1024;
        Check(file != INVALID_HANDLE_VALUE && SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file),
              "prepare small rotation boundary fixture");
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        ContentTiming timing(702);
        timing.Finish(0, 0, 0, false);
        Check(std::filesystem::file_size(fixture.Log()) < 2 * 1024 * 1024 &&
              std::filesystem::file_size(fixture.Log().wstring() + L".1") == 2 * 1024 * 1024,
              "2 MiB rotation retains exactly one backup");
    }
    {
        const auto now = std::filesystem::file_time_type::clock::now();
        auto stale = [&](int i) { return fixture.directory / (L"content-timing-" + std::to_wstring(900000 + i) + L".jsonl"); };
        for (int i = 0; i < 40; ++i) {
            { std::ofstream file(stale(i)); file << "{}\n"; }
            std::filesystem::last_write_time(stale(i), now - std::chrono::hours(48) + std::chrono::minutes(i));
        }
        const auto unrelated = fixture.directory / L"unrelated.jsonl";
        { std::ofstream file(unrelated); file << "x"; }
        ContentTiming timing(705);
        timing.Finish(0, 0, 0, false);
        size_t stale_left = 0;
        for (const auto& entry : std::filesystem::directory_iterator(fixture.directory))
            if (entry.path().filename().wstring().starts_with(L"content-timing-9")) ++stale_left;
        Check(stale_left == 24 && std::filesystem::exists(stale(39)) && std::filesystem::exists(stale(16)) &&
              !std::filesystem::exists(stale(15)) && std::filesystem::exists(unrelated) && std::filesystem::exists(fixture.Log()),
              "earlier processes' timing logs are pruned to the newest 24 files; own and unrelated files are kept");
    }
    {
        const auto impossible = fixture.directory / L"regular-file";
        { std::ofstream file(impossible); file << "file"; }
        SetEnvironmentVariableW(L"PULSE_CONTENT_TIMING_DIR", impossible.c_str());
        ContentTiming disabled(703);
        const auto token = disabled.BeginFile(ContentTimingLane::Pdf, L"unwritable-log.pdf", disabled.Enqueue(ContentTimingLane::Pdf));
        disabled.FileStage(token, ContentTimingStage::Extract);
        disabled.EndFile(token, true);
        disabled.Finish(1, 1, 1, false);
        Check(Read(impossible) == "file", "unwritable log destination does not affect the caller");
    }
    DWORD final_handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &final_handles);
    Check(final_handles <= initial_handles, "logger releases file and worker handles");
    std::printf("failures=%d; temporary directory removed on exit\n", failures);
    return failures ? 1 : 0;
}

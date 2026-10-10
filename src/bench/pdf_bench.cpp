#include "../index/document_reader.h"
#include "../index/document_protocol.h"
#include "../common/runtime_log.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc < 4 || argc > 5) return 2;
    SetEnvironmentVariableW(L"PULSE_PDF_ENGINE", argv[1]);
    pulse::index::DocumentReadSession session;
    // Optional parent-side runtime evidence for failure-fixture runs.
    wchar_t runtime_root[MAX_PATH]{};
    const DWORD runtime_chars = GetEnvironmentVariableW(L"PULSE_BENCH_RUNTIME_DIR", runtime_root, MAX_PATH);
    const bool runtime = runtime_chars && runtime_chars < MAX_PATH &&
        pulse::diagnostics::runtime::Initialize(runtime_root, "test");
    std::wstring body; uint64_t bytes = 0; DWORD error = 0;
    pulse::index::DocumentReadMetrics metrics;
    const auto start = pulse::index::DocumentMicros();
    const auto cancel_us = argc >= 5 ? _wcstoui64(argv[4], nullptr, 10) * 1000 : 0;
    const bool ok = pulse::index::ReadSearchableDocument(argv[2], pulse::index::document::kMaximumFileBytes,
        body, bytes, &error, pulse::text::Encoding::Auto,
        [&] { return cancel_us && pulse::index::DocumentMicros() - start >= cancel_us; },
        argv[3], false, &metrics);
    const auto elapsed = pulse::index::DocumentMicros() - start;
    SIZE_T peak = 0, private_bytes = 0, peak_private_bytes = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{sizeof(entry)};
    if (snapshot != INVALID_HANDLE_VALUE) {
        if (Process32FirstW(snapshot, &entry)) do {
            if (entry.th32ParentProcessID != GetCurrentProcessId() || _wcsicmp(entry.szExeFile, L"Pulse.Document.exe")) continue;
            HANDLE child = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID);
            PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
            if (child && GetProcessMemoryInfo(child, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
                peak = (std::max)(peak, memory.PeakWorkingSetSize); private_bytes = (std::max)(private_bytes, memory.PrivateUsage);
                peak_private_bytes = (std::max)(peak_private_bytes, memory.PeakPagefileUsage);
            }
            if (child) CloseHandle(child);
        } while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
    }
    std::cout << "{\"ok\":" << (ok ? "true" : "false") << ",\"error\":" << error << ",\"us\":" << elapsed
        << ",\"cpu_us\":" << metrics.cpu_us << ",\"chars\":" << body.size() << ",\"bytes\":" << bytes
        << ",\"peak_working_set\":" << peak << ",\"private_bytes\":" << private_bytes
        << ",\"peak_private_bytes\":" << peak_private_bytes
        << ",\"pdf_load_us\":" << metrics.pdf_load_us << ",\"pdf_page_us\":" << metrics.pdf_page_us
        << ",\"pdf_text_us\":" << metrics.pdf_text_us << ",\"pdf_pages\":" << metrics.pdf_pages
        << ",\"pdf_fallbacks\":" << metrics.pdf_fallbacks
        << ",\"pdf_read_calls\":" << metrics.pdf_read_calls << ",\"pdf_requested_bytes\":" << metrics.pdf_requested_bytes
        << ",\"pdf_reader_us\":" << metrics.pdf_reader_us << ",\"pdf_file_reads\":" << metrics.pdf_file_reads
        << ",\"pdf_file_bytes\":" << metrics.pdf_file_bytes << ",\"pdf_file_io_us\":" << metrics.pdf_file_io_us
        << ",\"pdf_cache_hits\":" << metrics.pdf_cache_hits << ",\"pdf_map_views\":" << metrics.pdf_map_views
        << ",\"pdf_mapped_bytes\":" << metrics.pdf_mapped_bytes << ",\"pdf_map_fallbacks\":" << metrics.pdf_map_fallbacks
        << ",\"child_exit_code\":" << metrics.child_exit_code << ",\"child_exit_known\":" << metrics.child_exit_known
        << ",\"child_failure_stage\":" << metrics.child_failure_stage
        << ",\"child_peak_private_bytes\":" << metrics.child_peak_private_bytes
        << ",\"child_private_bytes\":" << metrics.child_private_bytes << ",\"child_memory_known\":" << metrics.child_memory_known
        << ",\"read_attempts\":" << metrics.read_attempts << ",\"memory_recycles\":" << metrics.memory_recycles
        << ",\"attempt_cpu_us\":" << metrics.attempt_cpu_us
        << ",\"fresh_filter_fallbacks\":" << metrics.fresh_filter_fallbacks
        << ",\"fresh_filter_recoveries\":" << metrics.fresh_filter_recoveries << ",\"fresh_filter_skips\":" << metrics.fresh_filter_skips
        << ",\"fresh_filter_first_error\":" << metrics.fresh_filter_first_error << ",\"fresh_filter_first_exit\":" << metrics.fresh_filter_first_exit
        << ",\"released_private_bytes\":" << metrics.released_private_bytes << ",\"released_memory_known\":" << metrics.released_memory_known
        << "}\n";
    const int count = WideCharToMultiByte(CP_UTF8, 0, body.data(), static_cast<int>(body.size()), nullptr, 0, nullptr, nullptr);
    std::string encoded(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, body.data(), static_cast<int>(body.size()), encoded.data(), count, nullptr, nullptr);
    std::cout.write(encoded.data(), encoded.size());
    if (runtime) {
        pulse::diagnostics::runtime::Flush(5000);
        pulse::diagnostics::runtime::Shutdown();
    }
    return 0;
}

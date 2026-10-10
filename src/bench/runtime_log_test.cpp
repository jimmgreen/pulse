#include "../common/runtime_log.h"
#include "../common/diagnostics_exporter.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

using namespace pulse::diagnostics::runtime;
namespace {
std::string Read(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}
int main() {
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"runtime-log-" + std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(root);
    bool ok = true;
    auto check = [&](bool value, const char* label) { ok &= value; std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; };
    Event("not_initialized");
    check(!Enabled(), "uninitialized logging is disabled");
    check(!Initialize(root.wstring(), "C:\\private\\secret.txt"), "reject path as component identifier");
    check(!GetHealth().enabled && GetHealth().initialization_error == ERROR_INVALID_PARAMETER,
        "initialization failure exposes its reason");
    check(Initialize(root.wstring(), "test", {2 * 1024 * 1024, 40}), "initialize isolated runtime logging");
    check(GetHealth().enabled && GetHealth().initialization_error == 0, "successful initialization clears failure health");
    const auto id = NextId();
    SetLastError(ERROR_ACCESS_DENIED);
    Event("operation_begin", {{"operation", id}, {"error", 5}});
    check(GetLastError() == ERROR_ACCESS_DENIED, "recording preserves caller Win32 error state");
    Event("C:\\private\\secret.txt");
    Event("bad_field", {{"query phrase", 123}});
    const auto started = GetTickCount64();
    std::vector<std::thread> producers;
    for (unsigned thread = 0; thread < 4; ++thread) producers.emplace_back([thread] {
        for (unsigned i = 0; i < 100; ++i) Event("parallel_event", {{"producer", thread}, {"index", i}});
    });
    for (auto& producer : producers) producer.join();
    std::cout << "[TIME] enqueue_400_ms=" << GetTickCount64() - started << '\n';
    Sleep(100);
    Event("operation_end", {{"operation", id}});
    Event("operation_warning", {{"error", 5}}, Level::Warning);
    check(Flush(), "flush writes queued events before returning");
    check(GetHealth().pending_events == 0 && GetHealth().write_errors == 0, "drained logger exposes healthy status");
    Shutdown();
    const auto file = root / L"Diagnostics" / L"Runtime" / (L"test-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
    const auto text = Read(file);
    check(!Enabled(), "shutdown disables producers and drains queued records");
    check(text.find("process_start") != std::string::npos && text.find("process_stop") != std::string::npos,
        "normal lifecycle has start and stop records");
    check(text.find("operation_begin") != std::string::npos && text.find("operation_end") != std::string::npos,
        "operation correlation survives asynchronous delivery");
    size_t count = 0, at = 0;
    while ((at = text.find("\"event\":\"parallel_event\"", at)) != std::string::npos) { ++count; ++at; }
    check(count == 400, "concurrent producers preserve all records within queue capacity");
    check(text.find("process_health") != std::string::npos && text.find("private_bytes") != std::string::npos &&
        text.find("logical_processors") != std::string::npos, "health includes CPU normalization and memory evidence");
    check(text.find("secret.txt") == std::string::npos && text.find("query phrase") == std::string::npos &&
        text.find("\"dropped_events\":2") != std::string::npos, "invalid identifiers are omitted and counted");
    check(text.find("\"utc\":") != std::string::npos && text.find("\"version\":") != std::string::npos &&
        text.find("\"session\":") != std::string::npos, "records identify time, build and process session");
    check(text.find("\"event\":\"operation_warning\",\"severity\":1") != std::string::npos,
        "warning level is preserved in runtime records");
    const auto exported = root / L"export";
    std::filesystem::create_directory(exported);
    pulse::diagnostics::ExportOptions export_options;
    export_options.source_root = root.wstring(); export_options.destination = exported.wstring();
    export_options.include_dumps = false;
    check(pulse::diagnostics::Export(export_options) && Read(exported / L"Runtime" / file.filename()) == text,
        "real runtime records survive the existing diagnostics export pipeline");
    const auto rotated = root / L"rotation";
    std::filesystem::create_directory(rotated);
    check(Initialize(rotated.wstring(), "test", {4096, 60000}), "restart logger with isolated rotation threshold");
    for (unsigned i = 0; i < 250; ++i) Event("retained_failure", {{"error", 5}, {"operation", i}}, Level::Error);
    check(Flush(), "flush persists aggregated critical evidence");
    const auto critical_file = rotated / L"Diagnostics" / L"Runtime" /
        (L"test-" + std::to_wstring(GetCurrentProcessId()) + L".critical.jsonl");
    const auto critical_text = Read(critical_file);
    check(critical_text.find("\"occurrences\":250") != std::string::npos &&
        critical_text.find("\"operation\":0") != std::string::npos &&
        critical_text.find("\"operation\":249") != std::string::npos &&
        critical_text.find("\"severity\":2") != std::string::npos,
        "repeated failures retain first and last correlation IDs plus total count");
    for (unsigned i = 0; i < 100; ++i) Event("rotation_event", {{"index", i}});
    const auto flush_start = GetTickCount64();
    (void)Flush(0);
    check(GetTickCount64() - flush_start < 1000, "zero-timeout flush remains bounded with pending events");
    check(Flush(), "flush still succeeds after a bounded polling attempt");
    check(Read(critical_file) == critical_text, "ordinary log rotation cannot displace critical evidence");
    for (unsigned i = 0; i < 50; ++i) Event("another_failure", {{"error", i}}, Level::Error);
    check(Flush(), "flush persists bounded distinct failure patterns");
    const auto bounded_critical = Read(critical_file);
    check(std::count(bounded_critical.begin(), bounded_critical.end(), '\n') == 33 &&
        bounded_critical.find("retained_failure") != std::string::npos &&
        bounded_critical.find("\"error\":49") != std::string::npos && GetHealth().dropped_events == 19,
        "critical pattern capacity retains earliest and recent failures and counts eviction");
    Shutdown();
    const auto rotated_file = rotated / L"Diagnostics" / L"Runtime" / file.filename();
    const auto previous = std::filesystem::path(rotated_file.wstring() + L".1");
    check(std::filesystem::exists(previous) && std::filesystem::file_size(previous) <= 4096 &&
        std::filesystem::file_size(rotated_file) <= 4096, "rotation bounds current and previous log files");
    check(Read(rotated_file).find("process_stop") != std::string::npos, "rotation retains latest shutdown evidence");
    const auto critical_export = root / L"critical-export";
    std::filesystem::create_directory(critical_export);
    export_options.source_root = rotated.wstring(); export_options.destination = critical_export.wstring();
    check(pulse::diagnostics::Export(export_options) && Read(critical_export / L"Runtime" / critical_file.filename()) == bounded_critical,
        "critical evidence survives diagnostics export");
    const auto blocked_root = root / L"blocked";
    std::ofstream(blocked_root) << "fixture";
    check(!Initialize(blocked_root.wstring(), "test") && GetHealth().initialization_error != 0,
        "unusable data directory exposes initialization failure");
    const auto failures = root / L"write-failure";
    std::filesystem::create_directory(failures);
    check(Initialize(failures.wstring(), "test"), "initialize write failure fixture");
    check(Flush(), "write failure fixture starts healthy");
    const auto locked_path = failures / L"Diagnostics" / L"Runtime" / file.filename();
    HANDLE locked = CreateFileW(locked_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "lock isolated log against writes");
    Event("blocked_write");
    check(!Flush() && GetHealth().last_write_error == ERROR_SHARING_VIOLATION && GetHealth().write_errors > 0,
        "flush reports write failure and health retains the Win32 reason");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    Event("write_recovered");
    check(!Flush() && Read(locked_path).find("write_recovered") != std::string::npos,
        "subsequent successful writes do not hide earlier evidence loss");
    Shutdown();
    const auto critical_failures = root / L"critical-write-failure";
    std::filesystem::create_directory(critical_failures);
    check(Initialize(critical_failures.wstring(), "test"), "initialize critical write failure fixture");
    Event("critical_before_lock", {{"error", 5}}, Level::Error);
    check(Flush(), "critical write fixture starts healthy");
    const auto locked_critical = critical_failures / L"Diagnostics" / L"Runtime" / critical_file.filename();
    HANDLE critical_lock = CreateFileW(locked_critical.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(critical_lock != INVALID_HANDLE_VALUE, "lock critical snapshot against replacement");
    Event("critical_after_lock", {{"error", 6}}, Level::Error);
    check(!Flush() && GetHealth().write_errors > 0 && GetHealth().last_write_error != 0 &&
        Read(locked_critical).find("critical_before_lock") != std::string::npos &&
        Read(locked_critical).find("critical_after_lock") == std::string::npos,
        "failed critical replacement reports degraded health and preserves previous snapshot");
    if (critical_lock != INVALID_HANDLE_VALUE) CloseHandle(critical_lock);
    Shutdown();
    check(!Flush(), "flush rejects disabled logger");
    // Keep only isolated artifacts for an independent JSON parser check.
    std::wcout << L"[INFO] fixture=" << root.wstring() << L'\n';
    return ok ? 0 : 1;
}

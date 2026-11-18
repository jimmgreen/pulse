#include "../common/runtime_log.h"
#include "../app/win_e_diagnostics.h"
#include "../common/diagnostics_exporter.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
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
int main(int argc, char** argv) {
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"runtime-log-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root);
    bool ok = true;
    auto check = [&](bool value, const char* label) {
        ok &= value;
        std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!value) {
            const auto health = GetHealth();
            std::cout << "[HEALTH] error=" << health.last_write_error << " failures=" << health.write_errors
                << " operation=" << health.last_write_operation << " pending=" << health.pending_events << '\n';
        }
    };
    if (argc == 2 && std::string_view(argv[1]) == "--win-e-foreground") {
        check(Initialize(root.wstring(), "wine"), "initialize shortcut diagnostic fixture");
        const RECT monitor{0, 0, 2560, 1440};
        pulse::app::LogWinEForeground(true, 123, true, monitor, monitor);
        pulse::app::LogWinEForeground(false, 456, false, {}, {});
        pulse::app::LogWinEGate(1, 789);
        check(Flush(), "flush actual production foreground records");
        check(GetHealth().dropped_events == 0, "foreground records fit logger field limit");
        Shutdown();
        const auto file = root / L"Diagnostics" / L"Runtime" /
            (L"wine-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
        const auto contents = Read(file);
        check(contents.find("\"kind_at_log\":1") != std::string::npos &&
            contents.find("\"foreground_pid\":123") != std::string::npos,
            "fullscreen evidence is persisted");
        check(contents.find("\"kind_at_log\":2") != std::string::npos &&
            contents.find("\"foreground_pid\":456") != std::string::npos,
            "unknown foreground evidence is persisted");
        check(contents.find("\"observed_left_win\":1") != std::string::npos &&
            contents.find("\"async_win\":0") != std::string::npos &&
            contents.find("\"eligible\":0") != std::string::npos,
            "modifier disagreement evidence is persisted");
        return ok ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--critical-burst") {
        check(Initialize(root.wstring(), "burst"), "initialize critical burst measurement");
        const auto snapshot = root / L"Diagnostics" / L"Runtime" /
            (L"burst-" + std::to_wstring(GetCurrentProcessId()) + L".critical.jsonl");
        Event("repeated_failure", {{"error", 5}, {"operation", 111}}, Level::Error);
        const auto first_start = GetTickCount64();
        while (Read(snapshot).find("repeated_failure") == std::string::npos &&
            GetTickCount64() - first_start < 2000) Sleep(1);
        check(Read(snapshot).find("repeated_failure") != std::string::npos,
            "first failure persists without Flush");
        const auto first_ms = GetTickCount64() - first_start;
        const auto start = GetTickCount64();
        std::vector<std::thread> producers;
        for (unsigned thread = 0; thread < 4; ++thread) producers.emplace_back([thread] {
            for (unsigned i = 0; i < 50; ++i) {
                Event("repeated_failure", {{"error", 5}, {"operation", 1000 + thread * 50 + i}}, Level::Error);
                Sleep(5);
            }
        });
        for (auto& producer : producers) producer.join();
        check(Flush(), "Flush persists all concurrent repeated failures");
        const auto contents = Read(snapshot);
        check(contents.find("\"occurrences\":201") != std::string::npos &&
            contents.find("\"operation\":111") != std::string::npos,
            "concurrent burst retains exact count and first evidence");
        std::cout << "[MEASURE] events=201 critical_writes=" << GetHealth().critical_writes
            << " burst_ms=" << GetTickCount64() - start << " first_ms=" << first_ms << '\n';
        check(GetHealth().critical_writes < 50, "repeated burst coalesces critical snapshot writes");
        Event("repeated_failure", {{"error", 5}, {"operation", 2202}}, Level::Error);
        const auto bounded_start = GetTickCount64();
        while (Read(snapshot).find("\"occurrences\":202") == std::string::npos &&
            GetTickCount64() - bounded_start < 2000) Sleep(1);
        check(Read(snapshot).find("\"occurrences\":202") != std::string::npos,
            "deferred count persists within a bounded interval without Flush");
        std::cout << "[MEASURE] deferred_ms=" << GetTickCount64() - bounded_start << '\n';
        Event("new_failure", {{"error", 6}}, Level::Error);
        const auto new_start = GetTickCount64();
        while (Read(snapshot).find("new_failure") == std::string::npos &&
            GetTickCount64() - new_start < 2000) Sleep(1);
        check(Read(snapshot).find("new_failure") != std::string::npos,
            "a new failure pattern is not held behind the duplicate interval");
        std::cout << "[MEASURE] new_pattern_ms=" << GetTickCount64() - new_start << '\n';
        Event("repeated_failure", {{"error", 5}, {"operation", 2203}}, Level::Error);
        Shutdown();
        check(Read(snapshot).find("\"occurrences\":203") != std::string::npos,
            "shutdown forces the deferred final count to disk");
        const auto failure_root = root / "forced-failure";
        std::filesystem::create_directory(failure_root);
        check(Initialize(failure_root.wstring(), "burst"), "initialize forced duplicate failure fixture");
        Event("repeated_failure", {{"error", 5}}, Level::Error);
        check(Flush(), "persist first evidence before duplicate failure injection");
        const auto locked_snapshot = failure_root / "Diagnostics" / "Runtime" / snapshot.filename();
        HANDLE snapshot_lock = CreateFileW(locked_snapshot.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, 0, nullptr);
        check(snapshot_lock != INVALID_HANDLE_VALUE, "lock duplicate snapshot against replacement");
        Event("repeated_failure", {{"error", 5}}, Level::Error);
        check(!Flush() && GetHealth().write_errors > 0 &&
            Read(locked_snapshot).find("\"occurrences\":1") != std::string::npos,
            "Flush forces a deferred duplicate and reports replacement failure without erasing evidence");
        if (snapshot_lock != INVALID_HANDLE_VALUE) CloseHandle(snapshot_lock);
        Shutdown();
        return ok ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--text-fields") {
        check(Initialize(root.wstring(), "text_test"), "initialize isolated text logging");
        std::wstring path = LR"(C:\中文\"component".exe)";
        path += wchar_t(10); path += wchar_t(9);
        SetLastError(ERROR_ACCESS_DENIED);
        TextEvent("component_path", {{"path", path}, {"reason", L"attached"}}, {{"pid", 42}});
        check(GetLastError() == ERROR_ACCESS_DENIED, "text logging preserves caller error");
        path.assign(L"mutated_after_enqueue");
        std::wstring long_value(kMaxTextFieldLength + 20, L'x');
        TextEvent("long_path", {{"path", long_value}});
        std::wstring surrogate(kMaxTextFieldLength - 1, L'y');
        surrogate += L"\xd83d\xde00";
        TextEvent("surrogate_boundary", {{"path", surrogate}});
        TextEvent("duplicate_field", {{"pid", L"invalid"}}, {{"pid", 3}});
        Event("legacy_numeric", {{"number", 123}});
        check(Flush(), "flush text and legacy events");
        Shutdown();
        const auto file = root / L"Diagnostics" / L"Runtime" /
            (L"text_test-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
        const auto text = Read(file);
        check(text.find(R"("path":"C:\\\u4e2d\u6587\\\"component\".exe\u000a\u0009")") != std::string::npos,
            "Chinese path, backslashes, quotes and control characters are JSON escaped");
        check(text.find("mutated_after_enqueue") == std::string::npos,
            "queued text owns its value before caller mutation");
        check(text.find("\"path\":\"" + std::string(kMaxTextFieldLength, 'x') + "\"") != std::string::npos &&
            text.find(std::string(kMaxTextFieldLength + 1, 'x')) == std::string::npos,
            "text truncates at documented limit");
        check(text.find("\\ud83d") == std::string::npos && text.find("surrogate_boundary") != std::string::npos,
            "truncation does not split surrogate pair");
        check(text.find("\"reason\":\"attached\"") != std::string::npos &&
            text.find("\"pid\":42") != std::string::npos && text.find("\"number\":123") != std::string::npos,
            "text fields are strings and legacy fields remain numbers");
        check(text.find("\"text_truncated\":true") != std::string::npos,
            "truncated text emits explicit record metadata");
        check(text.find("duplicate_field") == std::string::npos, "reject mixed-type duplicate field keys");
        std::filesystem::remove_all(root);
        return ok ? 0 : 1;
    }
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
    const auto batch_health = GetHealth();
    check(batch_health.log_opens < batch_health.log_writes,
        "burst records reuse a validated log handle within each batch");
    std::cout << "[COUNT] opens=" << batch_health.log_opens << " writes=" << batch_health.log_writes << '\n';
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
    for (bool temporary_lock : {true, false}) {
        const auto rotation_lock_root = root / (temporary_lock ? L"rotation-short-lock" : L"rotation-permanent-lock");
        std::filesystem::create_directory(rotation_lock_root);
        check(Initialize(rotation_lock_root.wstring(), "test", {4096, 60000}), "initialize rotation lock fixture");
        check(Flush(), "rotation lock fixture starts healthy");
        const auto rotation_path = rotation_lock_root / L"Diagnostics" / L"Runtime" / file.filename();
        const auto old_path = std::filesystem::path(rotation_path.wstring() + L".1");
        { std::ofstream seed(old_path); seed << "previous evidence"; }
        HANDLE rotation_lock = CreateFileW(old_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, 0, nullptr);
        check(rotation_lock != INVALID_HANDLE_VALUE, "lock rotation destination against replacement");
        std::thread unlocker;
        std::atomic<bool> unlock_ready{false};
        std::atomic<ULONGLONG> unlocked_at{0};
        if (temporary_lock) {
            const auto opens = GetHealth().log_opens;
            unlocker = std::thread([=, &unlock_ready, &unlocked_at] {
                unlock_ready = true;
                const auto deadline = GetTickCount64() + 1000;
                while (GetHealth().log_opens == opens && GetTickCount64() < deadline) Sleep(1);
                Sleep(5);
                CloseHandle(rotation_lock);
                unlocked_at = GetTickCount64();
            });
            while (!unlock_ready.load()) Sleep(0);
        }
        // A single large line necessarily rotates the 4 KiB fixture.
        const std::wstring large(2048, L'中');
        const auto lock_test_start = GetTickCount64();
        TextEvent("rotation_lock_event", {{"reason", large}});
        const bool flushed = Flush();
        const auto flush_finished = GetTickCount64();
        if (temporary_lock) {
            unlocker.join();
            if (!flushed) std::cout << "[LOCK TIMING] release_ms=" << (unlocked_at.load() - lock_test_start)
                << " flush_ms=" << (flush_finished - lock_test_start) << '\n';
            check(flushed && GetHealth().write_errors == 0 &&
                Read(rotation_path).find("rotation_lock_event") != std::string::npos,
                "short rotation lock retries without losing the pending record");
        } else {
            check(!flushed && GetHealth().write_errors > 0 &&
                std::string_view(GetHealth().last_write_operation) == "rotate" &&
                Read(old_path) == "previous evidence",
                "permanent rotation lock fails Flush and preserves previous evidence");
            CloseHandle(rotation_lock);
        }
        Shutdown();
    }
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

#include "../app/win_e_trace.h"
#include "../common/runtime_log.h"
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace pulse::app {
struct WinETraceTestPeer {
    static std::unique_ptr<WinETrace> Gated(HANDLE gate) {
        return std::unique_ptr<WinETrace>(new WinETrace(gate));
    }
};
}
namespace {
uint64_t Number(const std::string& line, const char* field) {
    const auto key = std::string("\"") + field + "\":";
    const auto pos = line.find(key);
    if (pos == std::string::npos) return UINT64_MAX;
    uint64_t value = UINT64_MAX;
    std::from_chars(line.data() + pos + key.size(), line.data() + line.size(), value);
    return value;
}
bool Event(const std::string& line, const char* name) {
    return line.find(std::string("\"event\":\"") + name + "\"") != std::string::npos;
}
}
int main() {
    using namespace pulse;
    using Record = app::WinETraceRecord;
    namespace log = diagnostics::runtime;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"win-e-trace-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root);
    bool ok = true;
    const auto check = [&](bool value, const char* label) {
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label); ok &= value;
    };
    check(log::Initialize(root.wstring(), "winetrace"), "initialize actual runtime logger");
    const HWND a = CreateWindowExW(0, L"STATIC", L"private-a", WS_OVERLAPPED,
        5, 10, 120, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND b = CreateWindowExW(0, L"STATIC", L"private-b", WS_OVERLAPPED,
        150, 200, 180, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(a && b && gate, "create hidden window contexts and deterministic worker gate");
    if (!a || !b || !gate) {
        if (a) DestroyWindow(a);
        if (b) DestroyWindow(b);
        if (gate) CloseHandle(gate);
        log::Shutdown(); return 1;
    }
    auto trace = app::WinETraceTestPeer::Gated(gate);
    trace->CountE(true); trace->CountE(true); trace->CountE(false);
    const auto now = GetTickCount64();
    for (unsigned i = 0; i < 256; ++i) {
        Record record;
        record.foreground = i < 4 ? a : b;
        record.tick = now;
        record.generation = i;
        record.cycle = i < 4 ? 101 : 202;
        record.result = 1;
        if (i == 1 || i == 5) {
            record.kind = Record::Submission;
            record.id_high = i == 1 ? 1001 : 2001;
            record.id_low = i == 1 ? 1002 : 2002;
            record.action = 1;
        } else if (i < 6) {
            record.kind = Record::Input;
            record.phase = i == 2 ? 4 : i == 3 ? 5 : 3;
            record.input_time = 1234 + i;
            record.flags = 0x80;
        } else record.kind = Record::Hook;
        trace->Push(record);
    }
    for (unsigned i = 0; i < 17; ++i) trace->Push({});
    SetEvent(gate);
    trace.reset(); // Stop immediately: all accepted records still must drain.
    CloseHandle(gate);
    DestroyWindow(a); DestroyWindow(b);
    check(log::Flush(10000), "stopped trace records flush to actual JSONL");
    const auto logger_health = log::GetHealth();
    log::Shutdown();
    const auto file = root / L"Diagnostics" / L"Runtime" /
        (L"winetrace-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
    std::ifstream stream(file);
    std::vector<std::string> records, health, contexts;
    bool fields_ok = true;
    for (std::string line; std::getline(stream, line);) {
        if (Event(line, "win_e_input") || Event(line, "win_e_chord") || Event(line, "win_e_hook_refresh"))
            records.push_back(line);
        if (Event(line, "win_e_trace_health")) health.push_back(line);
        if (Event(line, "win_e_context_window")) contexts.push_back(line);
        if (line.find("\"event\":\"win_e_") != std::string::npos) {
            const auto begin = line.find("\"data\":{");
            unsigned numeric = 0;
            for (size_t i = begin == std::string::npos ? line.size() : begin + 8; i + 1 < line.size(); ++i)
                if (line[i] == ':' && line[i + 1] >= '0' && line[i + 1] <= '9') ++numeric;
            fields_ok &= numeric <= 12 && line.find("fields_truncated") == std::string::npos;
        }
    }
    check(records.size() == 256, "stop drains every accepted ring record without inventing dropped inputs");
    bool ordered = records.size() == 256;
    for (size_t i = 0; i < records.size(); ++i) ordered &= Number(records[i], "generation") == i;
    check(ordered, "persisted trace keeps exact producer order across full ring");
    if (records.size() >= 6) {
        const auto first = Number(records[0], "context_id"), second = Number(records[4], "context_id");
        check(first != UINT64_MAX && first != second && Number(records[1], "context_id") == first &&
            Number(records[2], "context_id") == first && Number(records[3], "context_id") == first &&
            Number(records[5], "context_id") == second, "inputs and submissions correlate to captured window contexts");
        check(Number(records[0], "cycle") == 101 && Number(records[1], "cycle") == 101 &&
            Number(records[2], "cycle") == 101 && Number(records[2], "phase") == 4 &&
            Number(records[3], "cycle") == 101 && Number(records[3], "phase") == 5,
            "repeat and key-up retain the submitted chord cycle");
        check(Number(records[1], "id_high") == 1001 && Number(records[1], "id_low") == 1002 &&
            Number(records[5], "id_high") == 2001 && Number(records[5], "id_low") == 2002 &&
            Number(records[5], "cycle") == 202, "different request UUID halves persist without conflation");
        bool found_a = false, found_b = false;
        for (const auto& line : contexts) {
            found_a |= Number(line, "context_id") == first && Number(line, "hwnd") == reinterpret_cast<uintptr_t>(a);
            found_b |= Number(line, "context_id") == second && Number(line, "hwnd") == reinterpret_cast<uintptr_t>(b);
        }
        check(found_a && found_b, "trace context IDs join actual hidden HWND geometry events");
    }
    bool drops_visible = false;
    for (const auto& line : health)
        drops_visible |= Number(line, "records") == 256 && Number(line, "dropped") == 17 && Number(line, "stopping") == 1;
    check(drops_visible, "final health persists exact overflow count and drained stop state");
    bool counters_visible = false;
    for (const auto& line : health)
        counters_visible |= Number(line, "e_down") == 2 && Number(line, "e_up") == 1;
    check(counters_visible, "unmatched E activity remains countable without recording ordinary key sequences");
    check(fields_ok, "each persisted trace/context event fits twelve numeric fields");
    check(logger_health.dropped_events == 0 && logger_health.write_errors == 0,
        "logger itself lost no accepted trace records");
    std::wprintf(L"Log: %ls\n", file.c_str());
    return ok ? 0 : 1;
}

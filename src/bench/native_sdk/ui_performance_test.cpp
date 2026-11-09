#include "../../app/extensions/native/dispatch_adapter.h"
#include "../../app/extensions/native/ui_adapter.h"
#include "../../common/json_value.h"
#include "../../common/windows_compat.h"
#include "../../ui/extension_panel.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <objbase.h>
#include <psapi.h>
#include <thread>
#include <vector>

namespace {
constexpr size_t kSamples = 2000, kWarmup = 200, kDispatchSamples = 1000, kDispatchWarmup = 100;
int64_t Tick() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
PulseStringView S(const char *value) { return {value, static_cast<uint32_t>(std::strlen(value))}; }
std::atomic<const char *> phase{"startup"};
std::atomic<size_t> ui_samples{0}, dispatch_samples{0};
ULONGLONG deadline = 0;
bool quit_seen = false;
bool WithinBudget() { return !quit_seen && GetTickCount64() < deadline; }
bool Pump() {
    phase.store("message_pump");
    const ULONGLONG until = GetTickCount64() + 2;
    MSG message{};
    for (unsigned count = 0; count < 32 && WithinBudget() && GetTickCount64() <= until; ++count) {
        if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            break;
        if (message.message == WM_QUIT) {
            quit_seen = true;
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return WithinBudget();
}
void Progress(std::ofstream &log, const char *name) {
    phase.store(name);
    log << "[STAGE] " << name << " uiSamples=" << ui_samples.load() << " dispatchCallbacks=" << dispatch_samples.load()
        << '\n';
    log.flush();
    std::printf("[STAGE] %s uiSamples=%zu dispatchCallbacks=%zu\n", name, ui_samples.load(), dispatch_samples.load());
    std::fflush(stdout);
}
struct Watchdog {
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread worker;
    explicit Watchdog(const std::filesystem::path &directory) {
        if (!finished)
            return;
        worker = std::thread([this, directory] {
            if (WaitForSingleObject(finished, 120000) == WAIT_OBJECT_0)
                return;
            auto record = pulse::json::Value::Object();
            record.set("ok", false);
            record.set("error", "benchmark_hard_timeout");
            record.set("phase", phase.load());
            record.set("uiCompletedPairs", static_cast<uint64_t>(ui_samples.load()));
            record.set("dispatchCompletedIncludingWarmup", static_cast<uint64_t>(dispatch_samples.load()));
            record.set("requiredUiSamples", static_cast<int>(kSamples));
            record.set("requiredDispatchSamples", static_cast<int>(kDispatchSamples));
            record.set("hardBudgetMilliseconds", 120000);
            std::ofstream diagnostic(directory / "timeout.json", std::ios::binary);
            diagnostic << pulse::json::Serialize(record) << '\n';
            diagnostic.flush();
            // Only this standalone benchmark process is terminated. Never used by Pulse production code.
            TerminateProcess(GetCurrentProcess(), 3);
        });
    }
    ~Watchdog() {
        if (finished) {
            SetEvent(finished);
            if (worker.joinable())
                worker.join();
            CloseHandle(finished);
        }
    }
};

struct Com {
    HRESULT value = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Com() {
        if (SUCCEEDED(value))
            CoUninitialize();
    }
};
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value)
            CloseHandle(value);
    }
};
pulse::json::Value Memory() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    auto result = pulse::json::Value::Object();
    const bool ok = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                         sizeof(counters)) != FALSE;
    result.set("available", ok);
    if (ok)
        result.set("privateBytes", std::to_string(counters.PrivateUsage));
    return result;
}
pulse::json::Value Stats(const std::vector<int64_t> &ticks, int64_t frequency) {
    auto sorted = ticks;
    std::sort(sorted.begin(), sorted.end());
    auto result = pulse::json::Value::Object();
    result.set("samples", static_cast<int>(ticks.size()));
    if (!sorted.empty()) {
        const double us = 1000000.0 / static_cast<double>(frequency);
        result.set("p50Microseconds", static_cast<double>(sorted[(sorted.size() * 50 + 99) / 100 - 1]) * us);
        result.set("p95Microseconds", static_cast<double>(sorted[(sorted.size() * 95 + 99) / 100 - 1]) * us);
        result.set("totalMeasuredMilliseconds",
                   static_cast<double>(std::accumulate(ticks.begin(), ticks.end(), int64_t{0})) * us / 1000.0);
    }
    return result;
}
pulse::json::Value Environment(int64_t frequency) {
    auto result = pulse::json::Value::Object();
    result.set("qpcFrequencyHz", frequency);
    result.set("processId", static_cast<int64_t>(GetCurrentProcessId()));
    result.set("pointerBits", static_cast<int>(sizeof(void *) * 8));
#ifdef _MSC_VER
    result.set("msvcVersion", _MSC_VER);
#endif
#ifdef PULSE_NATIVE_PERF_BUILD_MODE
    result.set("buildConfiguration", PULSE_NATIVE_PERF_BUILD_MODE);
#endif
#ifdef NDEBUG
    result.set("compilationMode", "NDEBUG");
#else
    result.set("compilationMode", "assertions-enabled");
#endif
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    result.set("logicalProcessors", static_cast<int>(info.dwNumberOfProcessors));
    result.set("processorArchitecture", static_cast<int>(info.wProcessorArchitecture));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    using VersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    auto query = reinterpret_cast<VersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (query && query(&version) == 0) {
        result.set("windowsMajor", static_cast<int>(version.dwMajorVersion));
        result.set("windowsMinor", static_cast<int>(version.dwMinorVersion));
        result.set("windowsBuild", static_cast<int>(version.dwBuildNumber));
    }
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char stamp[40]{};
    std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u:%02u:%02uZ", static_cast<unsigned>(utc.wYear),
                  static_cast<unsigned>(utc.wMonth), static_cast<unsigned>(utc.wDay), static_cast<unsigned>(utc.wHour),
                  static_cast<unsigned>(utc.wMinute), static_cast<unsigned>(utc.wSecond));
    result.set("utc", stamp);
    result.set("theme", "light");
    result.set("scale", 1);
    return result;
}
struct DispatchItem {
    int64_t begin = 0, elapsed = 0;
    HANDLE done = nullptr;
};
void PULSE_CALL Arrived(void *user) {
    auto &item = *static_cast<DispatchItem *>(user);
    item.elapsed = Tick() - item.begin;
    SetEvent(item.done);
}
pulse::json::Value DispatchBenchmark(int64_t frequency, bool &success, std::ofstream &log) {
    using pulse::extensions::native::DispatchAdapter;
    auto result = pulse::json::Value::Object();
    result.set("requiredSamples", static_cast<int>(kDispatchSamples));
    result.set("warmup", static_cast<int>(kDispatchWarmup));
    result.set("inFlight", 1);
    DispatchAdapter adapter([] { return true; });
    PulseDispatchV1 api{};
    Handle done{CreateEventW(nullptr, FALSE, FALSE, nullptr)}, finished{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!done.value || !finished.value || adapter.Query(PULSE_DISPATCH_VERSION, &api, sizeof(api)) != PULSE_OK) {
        success = false;
        result.set("error", "dispatch_initialization_failed");
        return result;
    }
    std::vector<DispatchItem> items(kDispatchSamples + kDispatchWarmup);
    for (auto &item : items)
        item.done = done.value;
    std::atomic<size_t> completed{0};
    std::atomic<bool> failed{false}, abort{false};
    const auto begin = Tick();
    std::thread worker([&] {
        for (auto &item : items) {
            if (abort.load()) {
                failed.store(true);
                break;
            }
            uint64_t token = 0;
            item.begin = Tick();
            if (api.post(api.context, Arrived, &item, &token) != PULSE_OK ||
                WaitForSingleObject(done.value, 5000) != WAIT_OBJECT_0) {
                failed.store(true);
                break;
            }
            // The UI may signal done solely to release this wait during budget cancellation.
            if (abort.load()) {
                failed.store(true);
                break;
            }
            dispatch_samples.store(++completed);
        }
        SetEvent(finished.value);
    });
    Progress(log, "dispatch_sampling");
    size_t last_logged = 0;
    while (WaitForSingleObject(finished.value, 0) != WAIT_OBJECT_0 && WithinBudget()) {
        // INPUTAVAILABLE also wakes for messages left by the bounded pump.
        const DWORD waited = MsgWaitForMultipleObjectsEx(1, &finished.value, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (waited == WAIT_OBJECT_0 + 1 && !Pump())
            break;
        if (completed.load() >= last_logged + 250) {
            last_logged = completed.load();
            Progress(log, "dispatch_sampling");
        }
    }
    if (!WithinBudget()) {
        failed.store(true);
        abort.store(true);
        adapter.Close();
        SetEvent(done.value);
        Progress(log, "dispatch_budget_exhausted");
    }
    worker.join();
    adapter.Close();
    std::vector<int64_t> samples;
    for (size_t i = kDispatchWarmup; i < completed.load(); ++i)
        samples.push_back(items[i].elapsed);
    result.set("statistics", Stats(samples, frequency));
    result.set("elapsedMillisecondsIncludingWarmup",
               static_cast<double>(Tick() - begin) * 1000.0 / static_cast<double>(frequency));
    result.set("definition", "QPC immediately before worker post to first callback instruction; includes scheduler, "
                             "queue and post overhead; no JavaScript comparison");
    result.set("ok", !failed.load() && samples.size() == kDispatchSamples);
    success &= !failed.load() && samples.size() == kDispatchSamples;
    return result;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    using namespace pulse;
    if (argc != 2 || !std::filesystem::path(argv[1]).is_absolute() || std::filesystem::exists(argv[1])) {
        std::fprintf(stderr, "Usage: pulse_native_ui_performance_test ABSOLUTE-NEW-OUTPUT-DIRECTORY\n");
        return 2;
    }
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    std::ofstream log(directory / "benchmark.log");
    deadline = GetTickCount64() + 60000;
    Watchdog watchdog(directory);
    if (!watchdog.finished)
        return 2;
    Progress(log, "initialization");
    auto report = json::Value::Object();
    bool success = true;
    auto check = [&](bool ok, const char *text) {
        success &= ok;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
        log << (ok ? "[PASS] " : "[FAIL] ") << text << '\n';
        log.flush();
        std::fflush(stdout);
    };
    compat::EnableDpiAwareness();
    Com com;
    LARGE_INTEGER frequency{};
    if (FAILED(com.value) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        return 2;
    report.set("environment", Environment(frequency.QuadPart));
    HWND owner = CreateWindowExW(0, L"STATIC", L"Native API performance owner", WS_OVERLAPPEDWINDOW, 0, 0, 800, 680,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner)
        return 2;
    {
        extensions::UiTree tree;
        tree.root.type = extensions::UiNodeType::Stack;
        tree.root.id = "root";
        extensions::UiNode input;
        input.type = extensions::UiNodeType::TextInput;
        input.id = "input";
        input.text = "Value";
        input.state = "value";
        extensions::UiNode label;
        label.type = extensions::UiNodeType::Text;
        label.id = "label";
        label.binding = "state.value";
        tree.root.children = {input, label};
        tree.state = {{"value", "initial"}};
        ui::ExtensionPanel direct;
        ui::ExtensionPanelSpec spec;
        spec.title = L"State update baseline";
        spec.tree = tree;
        Progress(log, "direct_panel_show");
        const bool direct_created = direct.Show(owner, std::move(spec), false, D2D1::ColorF(0x0078d4), 1);
        extensions::native::UiAdapter adapter(owner, false, D2D1::ColorF(0x0078d4), 1, [] { return true; });
        PulseUiModelV1 api{};
        const bool queried = adapter.QueryModel(PULSE_UI_MODEL_VERSION, &api, sizeof(api)) == PULSE_OK;
        std::array<PulseUiModelNode, 3> nodes{};
        for (auto &node : nodes) {
            node.size = sizeof(node);
        }
        nodes[0].type = PULSE_UI_MODEL_STACK;
        nodes[0].id = S("root");
        nodes[0].children = {1, nodes.data() + 1, 2};
        nodes[0].gap = {1, 12};
        nodes[1].type = PULSE_UI_MODEL_TEXT_INPUT;
        nodes[1].id = S("input");
        nodes[1].text = {1, S("Value")};
        nodes[1].state = {1, S("value")};
        nodes[2].type = PULSE_UI_MODEL_TEXT;
        nodes[2].id = S("label");
        nodes[2].binding = {1, S("state.value")};
        PulseUiState initial{S("value"), S("initial")};
        PulseUiModelTree description{}; description.size = sizeof(description); description.root = nodes.data(); description.states = &initial; description.state_count = 1;
        uint64_t panel = 0;
        Progress(log, "public_panel_create");
        const bool public_created =
            queried && api.create(api.context, S("State update baseline"), &description, &panel) == PULSE_OK;
        check(direct_created && public_created,
              "create identical real input and bound text controls through both paths");
        if (direct_created && public_created) {
            Pump();
            const size_t total = kWarmup + kSamples;
            std::vector<std::string> values;
            values.reserve(total);
            std::vector<std::vector<extensions::UiStateEntry>> direct_changes;
            direct_changes.reserve(total);
            std::vector<PulseUiState> public_changes;
            public_changes.reserve(total);
            for (size_t i = 0; i < total; ++i)
                values.push_back("value-" + std::to_string(i));
            for (const auto &value : values) {
                direct_changes.push_back({{"value", value}});
                public_changes.push_back({S("value"), {value.data(), static_cast<uint32_t>(value.size())}});
            }
            Progress(log, "ui_warmup");
            size_t warmup_completed = 0;
            for (size_t i = 0; i < kWarmup && WithinBudget(); ++i) {
                phase.store("warmup_direct_set_states");
                success &= direct.SetTreeStates(direct_changes[i]);
                phase.store("warmup_public_set_states");
                success &= api.set_states(api.context, panel, &public_changes[i], 1) == PULSE_OK;
                ++warmup_completed;
                if (!Pump())
                    break;
            }
            check(warmup_completed == kWarmup, "full warmup budget completed");
            std::vector<int64_t> direct_ticks, public_ticks;
            direct_ticks.reserve(kSamples);
            public_ticks.reserve(kSamples);
            Progress(log, "ui_sampling");
            report.set("privateMemoryBefore", Memory());
            const auto wall_begin = Tick();
            size_t direct_ok = 0, public_ok = 0;
            for (size_t sample = 0; sample < kSamples && warmup_completed == kWarmup && WithinBudget(); ++sample) {
                const size_t i = sample + kWarmup;
                auto direct_call = [&] {
                    phase.store("direct_set_states");
                    const auto begin = Tick();
                    const bool ok = direct.SetTreeStates(direct_changes[i]);
                    direct_ticks.push_back(Tick() - begin);
                    direct_ok += ok ? 1u : 0u;
                };
                auto public_call = [&] {
                    phase.store("public_set_states");
                    const auto begin = Tick();
                    const bool ok = api.set_states(api.context, panel, &public_changes[i], 1) == PULSE_OK;
                    public_ticks.push_back(Tick() - begin);
                    public_ok += ok ? 1u : 0u;
                };
                if (sample % 2) {
                    public_call();
                    direct_call();
                } else {
                    direct_call();
                    public_call();
                }
                ui_samples.store(sample + 1);
                if ((sample + 1) % 250 == 0)
                    Progress(log, "ui_sampling");
                if (!Pump())
                    break; // Outside measurement; bounded, not a frame-latency benchmark.
            }
            report.set("pairedLoopWallMilliseconds",
                       static_cast<double>(Tick() - wall_begin) * 1000.0 / static_cast<double>(frequency.QuadPart));
            report.set("privateMemoryAfter", Memory());
            report.set("directSetStates", Stats(direct_ticks, frequency.QuadPart));
            report.set("publicSetStates", Stats(public_ticks, frequency.QuadPart));
            report.set("requiredSamplesPerPath", static_cast<int>(kSamples));
            report.set("warmupPerPath", static_cast<int>(kWarmup));
            report.set("completedWarmupPerPath", static_cast<int>(warmup_completed));
            report.set("callOrder", "alternating direct/public per pair");
            report.set(
                "measurement",
                "Synchronous SetTreeStates/set_states entry-to-return including validation and control notification; "
                "strings and direct state vectors prepared outside timing; painting is not timed per frame");
            report.set("memoryScope",
                       "Entire benchmark process PrivateBytes with both panels and sample storage resident; includes "
                       "host/background fluctuations, not plugin-exclusive allocation or a leak conclusion");
            char bytes[1024]{};
            uint32_t length = 0;
            const auto read = api.get_state(api.context, panel, S("value"), bytes, sizeof(bytes), &length);
            check(direct_ok == kSamples && public_ok == kSamples, "both paths complete the full 2000-sample budget");
            const std::string expected = direct_ticks.empty()
                                             ? (warmup_completed ? values[warmup_completed - 1] : "initial")
                                             : values[kWarmup + direct_ticks.size() - 1];
            check(read == PULSE_OK && direct.TreeStateValue("value") == expected &&
                      std::string(bytes, length) == expected,
                  "direct and public paths retain identical expected final state");
        }
        Progress(log, "closing_ui_panels");
        adapter.Stop();
        direct.Close();
    }
    if (WithinBudget())
        report.set("workerToUiDispatch", DispatchBenchmark(frequency.QuadPart, success, log));
    else {
        success = false;
        report.set("dispatchSkipped", "overall_time_budget_or_quit");
    }
    report.set("softBudgetMilliseconds", 60000);
    report.set("hardBudgetMilliseconds", 120000);
    report.set("quitReceived", quit_seen);
    report.set("softBudgetExceeded", GetTickCount64() >= deadline);
    Progress(log, "writing_report");
    check(success, "SDK baseline and independent dispatch sample budgets satisfied");
    DestroyWindow(owner);
    report.set("ok", success);
    const auto serialized = json::Serialize(report);
    std::ofstream output(directory / "report.json", std::ios::binary);
    output << serialized << '\n';
    log << serialized << '\n';
    log.flush();
    std::printf("%s\n", serialized.c_str());
    return success && output && log ? 0 : 1;
}

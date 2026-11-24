#include "../app/app_state.h"
#include "../app/app_runtime.h"
#include "../app/hang_watch.h"
#include "../common/runtime_log.h"
#include "../common/diagnostics_package.h"
#include <commctrl.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <utility>
#include <stdexcept>

namespace {
using namespace pulse;
namespace fsys = std::filesystem;
constexpr UINT_PTR kTimer = 0x43445051;
struct Probe {
    AppState* state = nullptr;
    fsys::path root;
    std::jthread worker;
    std::atomic<bool> done{false};
    bool worker_ok = false, owns_root = false;
    int stage = 0, failures = 0, injected = 0;
    DWORD inject_delay = 0;
    uint64_t task = 0;
    ULONGLONG deadline = 0;
    std::ostringstream report;
    void Check(bool ok, const char* name) {
        report << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!ok) ++failures;
    }
};
std::unique_ptr<Probe> probe;
std::string Read(const fsys::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
uint32_t Le(const std::string& data, size_t at, size_t bytes) {
    if (at + bytes > data.size()) return 0;
    uint32_t value = 0;
    for (size_t i = 0; i < bytes; ++i) value |= uint32_t(static_cast<unsigned char>(data[at + i])) << (8 * i);
    return value;
}
void VerifyExport(Probe& p) {
    p.Check(diagnostics::runtime::Flush(5000), "background runtime flush completed");
    diagnostics::ExportOptions options;
    options.source_root = p.root.wstring();
    options.include_dumps = false;
    options.support_report = true;
    diagnostics::PackageResult package;
    std::wstring error;
    const bool exported = diagnostics::ExportSupportPackage(options, (p.root / L"export").wstring(), false, {}, package, error);
    p.Check(exported && package.user_complete, "actual support exporter created complete user package");
    p.report << "PACKAGE " << fsys::path(package.directory).string() << '\n';
    const auto zip = Read(fsys::path(package.directory) / L"Pulse-diagnostics.zip");
    std::string runtime, perf;
    size_t at = 0;
    while (at + 30 <= zip.size() && Le(zip, at, 4) == 0x04034b50) {
        const auto method = Le(zip, at + 8, 2);
        const size_t size = Le(zip, at + 18, 4), name_size = Le(zip, at + 26, 2), extra = Le(zip, at + 28, 2);
        const size_t body = at + 30 + name_size + extra;
        if (method != 0 || body > zip.size() || size > zip.size() - body) break;
        const auto name = zip.substr(at + 30, name_size);
        if (name.find("Runtime/") != std::string::npos) runtime += zip.substr(body, size);
        if (name.find("Perf/") != std::string::npos) perf += zip.substr(body, size);
        at = body + size;
    }
    p.Check(at + 4 <= zip.size() && Le(zip, at, 4) == 0x02014b50, "export ZIP stored entries and central directory parsed");
    p.Check(!runtime.empty() && !perf.empty(), "ZIP contains nonempty Runtime and Perf evidence");
    const auto healthy_start = runtime.find("\"event\":\"copy_diagnostics_healthy\"");
    const auto healthy_end = runtime.find("\"event\":\"copy_diagnostics_healthy_end\"");
    p.Check(healthy_start != std::string::npos && healthy_end != std::string::npos && healthy_end > healthy_start,
        "healthy phase markers exported");
    p.Check(healthy_start != std::string::npos && healthy_end != std::string::npos &&
        runtime.substr(healthy_start, healthy_end - healthy_start).find("ui_probe_late") == std::string::npos,
        "healthy pumping interval has no late-probe event");
    p.Check(runtime.find("ui_probe_recovered") != std::string::npos, "UI recovery acknowledgement exported");
    // Event/stack names follow the production hang-watch contract.
    const auto count_event = [&](const std::string& event) {
        size_t count = 0, pos = 0;
        const auto needle = "\"event\":\"" + event + "\"";
        while ((pos = runtime.find(needle, pos)) != std::string::npos) { ++count; pos += needle.size(); }
        return count;
    };
    p.report << "EXPORTED late=" << count_event("ui_probe_late")
             << " recovered=" << count_event("ui_probe_recovered")
             << " slow_dispatch=" << count_event("ui_slow_dispatch") << '\n';
    p.Check(count_event("ui_probe_late") >= 2, "both injected stalls produced background late UI probes");
    p.Check(count_event("ui_probe_recovered") >= 2, "both injected probes recovered");
    p.Check(count_event("ui_slow_dispatch") >= 2, "both recovered slow dispatches persisted");
    size_t correlated = 0;
    bool medium = false, long_stall = false;
    std::istringstream lines(runtime);
    for (std::string line; std::getline(lines, line);) {
        if (line.find("\"event\":\"ui_slow_dispatch\"") == std::string::npos) continue;
        const auto field = [&](const char* key) {
            const auto needle = std::string("\"") + key + "\":";
            const auto pos = line.find(needle);
            return pos == std::string::npos ? uint64_t{0} : std::stoull(line.substr(pos + needle.size()));
        };
        if (field("task") != p.task || field("message") != app::hang::UiProbeMessage()) continue;
        ++correlated;
        const auto elapsed = field("elapsed_ms");
        medium |= elapsed >= 190 && elapsed < 600;
        long_stall |= elapsed >= 640;
    }
    p.Check(correlated >= 2 && medium && long_stall,
        "200ms and 650ms real dispatch records correlate with actual copy task and probe message");
    p.Check(perf.find("dispatch_id=") != std::string::npos && perf.find("status=captured") != std::string::npos, "Perf includes captured dispatch stall stack record");
    bool bytes_ok = true;
    for (int i = 0; i < 2048; ++i) {
        const auto name = std::to_wstring(i) + L".txt";
        const auto source = Read(p.root / L"source" / name);
        bytes_ok &= source.size() == 1024 && source == Read(p.root / L"destination" / L"source" / name);
    }
    p.Check(bytes_ok, "background verification confirms all 2048 copied file bytes");
    p.report << "RESULT failures=" << p.failures << '\n';
    std::ofstream out(p.root / L"VERIFICATION.txt", std::ios::binary);
    out << p.report.str();
}
LRESULT CALLBACK Inject(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (probe && message == app::hang::UiProbeMessage() && probe->inject_delay) {
        const DWORD delay = std::exchange(probe->inject_delay, 0);
        const auto status = probe->state->ops.QueryTask(probe->task);
        probe->Check(status && status->active, "injection runs while actual copy task remains active (paused)");
        // Probe-only deliberate blockage. This TU is absent from normal packages.
        const auto started = GetTickCount64();
        Sleep(delay);
        probe->report << "INJECT requested_ms=" << delay << " elapsed_ms=" << GetTickCount64() - started << '\n';
        ++probe->injected;
    }
    return DefSubclassProc(hwnd, message, wp, lp);
}
void CALLBACK Tick(HWND hwnd, UINT, UINT_PTR, DWORD) {
    if (!probe) return;
    auto& p = *probe;
    if (p.stage == 0) {
        app::hang::StopFrameWatch();
        p.stage = 1;
        p.worker = std::jthread([&p] {
            try {
                p.owns_root = fsys::create_directory(p.root);
                if (!p.owns_root) throw std::runtime_error("fixture root already exists");
                diagnostics::runtime::Shutdown();
                fsys::create_directories(p.root / L"Diagnostics" / L"Perf");
                fsys::create_directories(p.root / L"source");
                fsys::create_directories(p.root / L"destination");
                fsys::create_directories(p.root / L"export");
                const std::string bytes(1024, 'x');
                for (int i = 0; i < 2048; ++i) {
                    std::ofstream file(p.root / L"source" / (std::to_wstring(i) + L".txt"), std::ios::binary);
                    file << bytes;
                    if (!file) throw std::runtime_error("fixture write");
                }
                p.worker_ok = diagnostics::runtime::Initialize(p.root.wstring(), "app");
            } catch (...) { p.worker_ok = false; }
            p.done = true;
        });
    } else if (p.stage == 1 && p.done) {
        p.worker.join();
        p.Check(p.worker_ok, "private fixture and runtime initialized in background");
        if (!p.worker_ok) {
            if (!p.owns_root) {
                KillTimer(hwnd, kTimer);
                RemoveWindowSubclass(ServiceWindow(*p.state), Inject, kTimer);
                DestroyWindow(hwnd);
                PostQuitMessage(2);
                return;
            }
            p.done = false;
            p.stage = 10;
            p.worker = std::jthread([&p] {
                std::ofstream(p.root / L"VERIFICATION.txt") << p.report.str() << "RESULT failures=" << p.failures << '\n';
                p.done = true;
            });
            return;
        }
        const bool watch_started = app::hang::StartFrameWatch((p.root / L"Diagnostics" / L"Perf" /
            (L"frame-" + std::to_wstring(GetCurrentProcessId()) + L".log")).wstring());
        p.Check(watch_started, "production frame/dispatch watcher started on UI thread");
        if (!watch_started) { p.stage = 9; p.deadline = GetTickCount64(); return; }
        app::hang::SetProbeWindow(ServiceWindow(*p.state));
        diagnostics::runtime::Event("copy_diagnostics_healthy");
        p.deadline = GetTickCount64() + 1200;
        p.stage = 2;
    } else if (p.stage == 2 && GetTickCount64() >= p.deadline) {
        diagnostics::runtime::Event("copy_diagnostics_healthy_end");
        ops::OpRequest request;
        request.type = ops::OpType::Copy;
        request.sources = {(p.root / L"source").wstring()};
        request.dest_dir = (p.root / L"destination").wstring();
        p.task = p.state->ops.Submit(std::move(request));
        p.Check(p.task && p.state->ops.RetainTask(p.task), "submitted and retained actual private directory copy");
        p.done = false;
        p.worker = std::jthread([&p] {
            p.worker_ok = false;
            const auto end = GetTickCount64() + 10000;
            while (GetTickCount64() < end) {
                if (p.state->ops.PauseTask(p.task)) { p.worker_ok = true; break; }
                Sleep(1);
            }
            p.done = true;
        });
        p.stage = 3;
    } else if (p.stage == 3 && p.done) {
        p.worker.join();
        p.Check(p.worker_ok, "actual copy paused to keep real operation context during injection");
        p.deadline = GetTickCount64() + 400;
        p.stage = 4;
    } else if (p.stage == 4 && GetTickCount64() >= p.deadline) {
        p.inject_delay = 200;
        p.deadline = GetTickCount64() + 5000;
        p.stage = 5;
    } else if ((p.stage == 5 || p.stage == 7) && GetTickCount64() >= p.deadline) {
        p.Check(false, "timed out waiting for posted probe injection");
        p.inject_delay = 0;
        p.state->ops.ResumeTask(p.task);
        p.deadline = GetTickCount64() + 30000;
        p.stage = 8;
    } else if (p.stage == 5 && p.injected == 1) {
        p.deadline = GetTickCount64() + 500;
        p.stage = 6;
    } else if (p.stage == 6 && GetTickCount64() >= p.deadline) {
        p.inject_delay = 650;
        p.deadline = GetTickCount64() + 5000;
        p.stage = 7;
    } else if (p.stage == 7 && p.injected == 2) {
        p.Check(p.state->ops.ResumeTask(p.task), "real copy resumed after both probe-only stalls");
        p.deadline = GetTickCount64() + 30000;
        p.stage = 8;
    } else if (p.stage == 8) {
        const auto status = p.state->ops.QueryTask(p.task);
        const bool terminal = status && (status->phase == ops::OpPhase::Completed || status->phase == ops::OpPhase::Failed);
        if (!terminal && GetTickCount64() < p.deadline) return;
        p.Check(status && status->phase == ops::OpPhase::Completed, "actual private copy completed");
        p.state->ops.ReleaseTask(p.task);
        p.deadline = GetTickCount64() + 700;
        p.stage = 9;
    } else if (p.stage == 9 && GetTickCount64() >= p.deadline) {
        app::hang::StopFrameWatch();
        p.done = false;
        p.stage = 10;
        p.worker = std::jthread([&p] {
            try { VerifyExport(p); }
            catch (...) {
                p.Check(false, "background export threw");
                std::ofstream(p.root / L"VERIFICATION.txt") << p.report.str();
            }
            p.done = true;
        });
    } else if (p.stage == 10 && p.done) {
        p.worker.join();
        const int result = p.failures ? 1 : 0;
        KillTimer(hwnd, kTimer);
        RemoveWindowSubclass(ServiceWindow(*p.state), Inject, kTimer);
        DestroyWindow(hwnd);
        PostQuitMessage(result);
    }
}
} // namespace

bool ScheduleCopyDiagnosticsProbe(pulse::AppState& state, const wchar_t* root) {
    if (!state.isolatedTest || !root || !*root || probe) return false;
    probe = std::make_unique<Probe>();
    probe->state = &state;
    probe->root = fsys::path(root) / (L"app-diag-" + std::to_wstring(GetCurrentProcessId()));
    probe->report << "PROBE_ONLY: intentional 200/650ms UI handler Sleep; real copy paused; no throughput claim.\n"
                  << "Export API and ZIP acceptance, not Settings button interaction.\n";
    if (!SetWindowSubclass(ServiceWindow(state), Inject, kTimer, 0)) return false;
    return SetTimer(state.hwnd, kTimer, 20, Tick) != 0;
}

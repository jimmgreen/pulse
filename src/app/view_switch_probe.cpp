#include "app_commands.h"
#include "app_navigation.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace pulse;
namespace pulse::ui {
struct ThumbnailCacheTestAccess {
    struct Counts { size_t pending, queued; uint64_t uploads; };
    static Counts Read(ThumbnailCache& cache) {
        std::lock_guard lock(cache.mutex_);
        return {cache.pending_.size(), cache.queue_.size(), cache.upload_sequence_};
    }
};
}
namespace {
using Clock = std::chrono::steady_clock;
struct Frame { double elapsed, total, draw, present, gap; };
struct Capture {
    HWND window = nullptr;
    Clock::time_point start, previous;
    std::vector<Frame> frames;
    size_t attempts = 0;
    HRESULT draw_status = S_OK, present_status = S_OK;
    bool hidden = false;
    bool functional = false;
    size_t occluded = 0;
};
Capture* capture = nullptr; // UI thread only, scoped to one measured transition.
double Ms(Clock::duration value) { return std::chrono::duration<double, std::milli>(value).count(); }
bool Pump() {
    MSG message{};
    if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); return false; }
        TranslateMessage(&message); DispatchMessageW(&message);
    } else Sleep(1);
    return true;
}
bool Settle(AppState& state, DWORD duration) {
    const auto deadline = GetTickCount64() + duration;
    while (GetTickCount64() < deadline) if (!Pump() || !IsWindow(state.hwnd)) return false;
    return true;
}
bool Ready(AppState& state) {
    const auto deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline) {
        auto* tab = ActiveTab(state);
        if (tab && !tab->loading) return true;
        if (!Pump() || !IsWindow(state.hwnd)) return false;
    }
    return false;
}
double Percentile(const std::vector<Frame>& frames, double Frame::* member, double percentile) {
    std::vector<double> values;
    for (const auto& frame : frames) values.push_back(frame.*member);
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(std::ceil(percentile * values.size())) - 1];
}
}

// Records attempted frames; performance samples require a successful real Present.
void RecordViewSwitchFrame(AppState& state, double total_ms, HRESULT draw_status, HRESULT present_status, bool hidden) {
    if (!capture || capture->window != state.hwnd || capture->frames.size() >= 2048) return;
    ++capture->attempts;
    capture->draw_status = draw_status; capture->present_status = present_status; capture->hidden = hidden;
    if (present_status == DXGI_STATUS_OCCLUDED) ++capture->occluded;
    if (hidden || FAILED(draw_status) ||
        (present_status != S_OK && !(capture->functional && present_status == DXGI_STATUS_OCCLUDED))) return;
    const auto now = Clock::now();
    capture->frames.push_back({Ms(now - capture->start), total_ms, state.timing.draw_ms,
        state.timing.present_ms, Ms(now - capture->previous)});
    capture->previous = now;
}

int RunViewSwitchProbe(AppState& state, const wchar_t* output) {
    std::vector<wchar_t> data(32768);
    if (!state.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data.data(), static_cast<DWORD>(data.size()))) return 2;
    const auto base = std::filesystem::path(data.data()).lexically_normal();
    const auto log_path = std::filesystem::path(output).lexically_normal();
    auto* tab = ActiveTab(state);
    if (!base.is_absolute() || log_path.parent_path() != base || !tab ||
        fs::NormalizePath(tab->current_path) != fs::NormalizePath((base / L"files").wstring())) return 2;
    FILE* log = nullptr;
    if (_wfopen_s(&log, output, L"wb") || !log) return 3;
    struct Cleanup { FILE* file; ~Cleanup() { capture = nullptr; fclose(file); } } cleanup{log};
    ShowWindow(state.hwnd, SW_SHOWNORMAL);
    // A covered composition swap chain returns DXGI_STATUS_OCCLUDED instead
    // of presenting. Keep only this short-lived isolated probe above its host.
    SetWindowPos(state.hwnd, HWND_TOPMOST, 80, 80, 1280, 800, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    if (!Ready(state) || !Settle(state, 500)) return 4;
    tab = ActiveTab(state);
    const size_t count = tab->EntryCount();
    if (count != 100 && count != 10000 && count != 50000) {
        fprintf(log, "ERROR expected 100/10000/50000 actual=%zu\n", count); return 2;
    }
    fprintf(log, "# entries=%zu scale=%.3f window=1280x800 sample_ms=1000; cold=first transition after enumeration, not OS-cache-cold; hot=repeated in same process\n", count, state.scale);
    fprintf(log, "# FRAME round,target,index,elapsed_ms,total_ms,draw_ms,present_ms,gap_ms (first gap includes command latency)\n");
    fprintf(log, "# SUMMARY round,target,entries,command_ms,first_present_ms,frames,total_p95,total_max,draw_p95,present_p95,gap_p95,gap_max\n");
    ChooseViewMode(state, ui::ViewMode::Details);
    if (!Settle(state, 500)) return 4;
    if (GetEnvironmentVariableW(L"PULSE_TEST_THUMBNAIL_SWITCH", nullptr, 0)) {
        const bool functional = GetEnvironmentVariableW(L"PULSE_TEST_THUMBNAIL_FUNCTIONAL", nullptr, 0) != 0;
        if (functional) fprintf(log, "# FUNCTIONAL mode: successful EndDraw frames; no presentation-performance claims\n");
        for (const auto source : {ui::ViewMode::MediumIcons, ui::ViewMode::LargeIcons}) {
            state.renderer.EvictThumbnails();
            Capture loading; loading.window = state.hwnd; loading.functional = functional;
            loading.start = loading.previous = Clock::now(); capture = &loading;
            ChooseViewMode(state, source);
            const auto deadline = GetTickCount64() + 10000;
            auto& thumbnails = state.core.render_caches.thumbnails;
            while ((loading.frames.empty() || ui::ThumbnailCacheTestAccess::Read(thumbnails).pending == 0) &&
                   GetTickCount64() < deadline)
                if (!Pump()) return 4;
            capture = nullptr;
            const auto before = ui::ThumbnailCacheTestAccess::Read(thumbnails);
            if (loading.frames.empty() || !before.pending) {
                fprintf(log, "ERROR source=%d frames=%zu pending=%zu queued=%zu uploads=%llu visible=%d iconic=%d mode=%d\n",
                    static_cast<int>(source), loading.frames.size(), before.pending, before.queued,
                    static_cast<unsigned long long>(before.uploads), IsWindowVisible(state.hwnd),
                    IsIconic(state.hwnd), ActiveTab(state) ? static_cast<int>(ActiveTab(state)->view_mode) : -1);
                fflush(log);
                fprintf(log, "ERROR attempts=%zu draw_status=%08lx present_status=%08lx hidden=%d\n",
                    loading.attempts, static_cast<unsigned long>(loading.draw_status),
                    static_cast<unsigned long>(loading.present_status), loading.hidden);
                state.compositor.SaveSnapshot((base / L"source-failed.png").c_str());
                return 5;
            }
            // The source view must actually have unfinished image requests.
            Capture sample; sample.window = state.hwnd; sample.frames.reserve(256); sample.functional = functional;
            sample.start = sample.previous = Clock::now(); capture = &sample;
            ChooseViewMode(state, ui::ViewMode::List);
            const double command_ms = Ms(Clock::now() - sample.start);
            if (!Settle(state, 1000)) return 4;
            const auto prefix = source == ui::ViewMode::MediumIcons ? L"medium" : L"large";
            capture = nullptr;
            const auto after = ui::ThumbnailCacheTestAccess::Read(thumbnails);
            if (sample.frames.empty() || !ActiveTab(state) || ActiveTab(state)->view_mode != ui::ViewMode::List ||
                ActiveTab(state)->EntryCount() != count) return 5;
            const char* name = source == ui::ViewMode::MediumIcons ? "medium-list-loading" : "large-list-loading";
            fprintf(log, "CACHE,%s,pending_before=%zu,queued_before=%zu,uploads_before=%llu,pending_after=%zu,queued_after=%zu,uploads_after=%llu\n",
                name, before.pending, before.queued, static_cast<unsigned long long>(before.uploads),
                after.pending, after.queued, static_cast<unsigned long long>(after.uploads));
            if (after.uploads != before.uploads || after.queued != 0) {
                fprintf(log, "ERROR outgoing file thumbnails uploaded or kept queued during list morph\n"); return 7;
            }
            if (functional) {
                fprintf(log, "FUNCTIONAL,%s,PASS,rendered_frames=%zu,pending_source=%zu,upload_delta=0,queued_after=0\n",
                    name, sample.frames.size(), before.pending);
                fprintf(log, "PERF_UNAVAILABLE,%s,occluded_source=%zu,occluded_target=%zu; functional mode does not measure presentation performance\n",
                    name, loading.occluded, sample.occluded);
            }
            if (!functional) for (size_t i = 0; i < sample.frames.size(); ++i) {
                const auto& frame = sample.frames[i];
                fprintf(log, "FRAME,0,%s,%zu,%.3f,%.3f,%.3f,%.3f,%.3f\n", name, i,
                    frame.elapsed, frame.total, frame.draw, frame.present, frame.gap);
            }
            if (!functional) fprintf(log, "SUMMARY,0,%s,%zu,%.3f,%.3f,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                name, count, command_ms, sample.frames.front().elapsed, sample.frames.size(),
                Percentile(sample.frames, &Frame::total, .95), Percentile(sample.frames, &Frame::total, 1),
                Percentile(sample.frames, &Frame::draw, .95), Percentile(sample.frames, &Frame::present, .95),
                Percentile(sample.frames, &Frame::gap, .95), Percentile(sample.frames, &Frame::gap, 1));
            fflush(log);
            if (!state.compositor.SaveSnapshot((base / (std::wstring(prefix) + L"-list-settled.png")).c_str())) return 6;
        }
        return 0;
    }
    const struct { const char* name; ui::ViewMode mode; bool wall; } steps[] = {
        {"medium", ui::ViewMode::MediumIcons, false}, {"large", ui::ViewMode::LargeIcons, false},
        {"wall", ui::ViewMode::LargeIcons, true}, {"list", ui::ViewMode::List, false},
        {"details", ui::ViewMode::Details, false}};
    for (int round = 0; round < 4; ++round) for (const auto& step : steps) {
        if (!Ready(state)) return 4;
        Capture sample; sample.window = state.hwnd; sample.frames.reserve(256);
        sample.start = sample.previous = Clock::now(); capture = &sample;
        if (step.wall) ShowIconWall(state, true); else ChooseViewMode(state, step.mode);
        const double command_ms = Ms(Clock::now() - sample.start);
        const bool pumped = Settle(state, 1000);
        capture = nullptr;
        if (!pumped) return 4;
        tab = ActiveTab(state);
        if (!tab || tab->loading || tab->EntryCount() != count || sample.frames.empty()) {
            fprintf(log, "ERROR unstable listing or no successful presented frame round=%d target=%s\n", round, step.name); return 5;
        }
        for (size_t i = 0; i < sample.frames.size(); ++i) {
            const auto& frame = sample.frames[i];
            fprintf(log, "FRAME,%d,%s,%zu,%.3f,%.3f,%.3f,%.3f,%.3f\n", round, step.name, i,
                frame.elapsed, frame.total, frame.draw, frame.present, frame.gap);
        }
        fprintf(log, "SUMMARY,%d,%s,%zu,%.3f,%.3f,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
            round, step.name, count, command_ms, sample.frames.front().elapsed, sample.frames.size(),
            Percentile(sample.frames, &Frame::total, .95), Percentile(sample.frames, &Frame::total, 1),
            Percentile(sample.frames, &Frame::draw, .95), Percentile(sample.frames, &Frame::present, .95),
            Percentile(sample.frames, &Frame::gap, .95), Percentile(sample.frames, &Frame::gap, 1));
        fflush(log);
        if (round == 0) {
            const auto image = base / (std::wstring(L"view-") + std::wstring(step.name, step.name + strlen(step.name)) + L".png");
            if (!state.compositor.SaveSnapshot(image.c_str())) return 6;
        }
    }
    for (int index = 0; index < 12; ++index) {
        ChooseViewMode(state, index % 2 ? ui::ViewMode::MediumIcons : ui::ViewMode::List);
        if (!Settle(state, 60)) return 4;
    }
    if (!Settle(state, 800) || !ActiveTab(state) || ActiveTab(state)->EntryCount() != count) return 5;
    if (!state.compositor.SaveSnapshot((base / L"view-rapid-settled.png").c_str())) return 6;
    fprintf(log, "RAPID,12,interval_ms=60,settled_ms=800,entries=%zu,ok\n", count);
    return 0;
}

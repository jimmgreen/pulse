#include "../ui/wall_layout_job.h"
#include "../ui/wall_layout_source.h"
#include <chrono>
#include <cstdio>
#include <thread>
using Clock = std::chrono::steady_clock;
int main() {
    using namespace pulse::ui;
    auto snapshot = std::make_shared<std::vector<pulse::fs::DirEntry>>(100000);
    for (size_t i = 0; i < snapshot->size(); ++i) (*snapshot)[i].name = std::to_wstring(i) + L".jpg";
    auto source = std::make_shared<WallLayoutSource>(snapshot, nullptr, L"C:\\wall-bench", false);
    WallLayoutJob job;
    const auto submit_start = Clock::now();
    job.RequestPrepared([source](const auto& generation, uint64_t expected) {
        return source->Prepare(generation, expected, true);
    }, 1200, 160, 1, [source] { source->Committed(); });
    const auto submitted = Clock::now();
    std::shared_ptr<const WallRows> rows;
    const auto deadline = submitted + std::chrono::seconds(15);
    while (!rows && Clock::now() < deadline) {
        rows = job.TakeReady();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!rows || rows->x.size() != snapshot->size()) return 1;
    std::printf("[PASS] Background source builds 100000 immutable entries; UI submit %.3f ms, ready %.3f ms\n",
        std::chrono::duration<double,std::milli>(submitted-submit_start).count(),
        std::chrono::duration<double,std::milli>(Clock::now()-submitted).count());
    const auto old_start = Clock::now();
    std::vector<uint64_t> old_keys(snapshot->size(), 0);
    std::vector<float> old_aspects(snapshot->size(), 0);
    for (size_t i = 0; i < snapshot->size(); ++i) {
        const auto& entry = (*snapshot)[i];
        if (IsWallPicture(entry.name, entry.is_dir))
            old_keys[i] = WallAspects::Hash(WallLayoutSource::Path(entry, L"C:\\wall-bench", false));
    }
    WallAspects::Instance().Lookup(old_keys.data(), old_aspects.data(), old_keys.size());
    std::vector<std::pair<uint64_t, std::wstring>> unknown;
    for (size_t i = 0; i < snapshot->size(); ++i) {
        if (old_keys[i] && old_aspects[i] <= 0)
            unknown.emplace_back(old_keys[i], WallLayoutSource::Path((*snapshot)[i], L"C:\\wall-bench", false));
    }
    std::vector<float> projected(snapshot->size(), 0.85f);
    for (size_t i = 0; i < snapshot->size(); ++i) if (old_keys[i]) projected[i] = old_aspects[i];
    const auto legacy_rows = BuildWallRows(projected, 1200, 160, 1);
    std::printf("BENCH legacy UI keys+lookup+unknown-list+projection+rows %.3f ms, items %zu; excludes Probe submission/IO\n",
        std::chrono::duration<double,std::milli>(Clock::now()-old_start).count(), legacy_rows.x.size());    std::atomic<uint64_t> generation{1};
    WallLayoutSource baseline(snapshot, nullptr, L"C:\\wall-bench", false);
    const auto baseline_start = Clock::now();
    auto values = baseline.Prepare(generation, 1, true);
    auto old_rows = BuildWallRows(*values, 1200, 160, 1);
    baseline.Committed();
    std::printf("BENCH current worker source+rows run synchronously (same-work reference) %.3f ms\n", std::chrono::duration<double,std::milli>(Clock::now()-baseline_start).count());
    const auto stable_start = Clock::now();
    for (int i = 0; i < 1000; ++i) if (source->Prepare(generation, 1, false)) return 2;
    std::printf("[PASS] 1000 stable revisions %.3f ms, zero projections or layout builds\n", std::chrono::duration<double,std::milli>(Clock::now()-stable_start).count());
    const auto updates_start = Clock::now();
    for (uint64_t i = 0; i < 1000; ++i) {
        WallAspects::Instance().Note(i + 1, 1.5f);
        if (source->Prepare(generation, 1, false)) return 3;
    }
    std::printf("[PASS] 1000 unrelated updates %.3f ms, zero full lookups or layout builds\n", std::chrono::duration<double,std::milli>(Clock::now()-updates_start).count());
    const auto path = WallLayoutSource::Path((*snapshot)[123], L"C:\\wall-bench", false);
    WallAspects::Instance().Note(WallAspects::ContentHash(path, 0, 0), 2.0f);
    values = source->Prepare(generation, 1, false);
    if (!values || (*values)[123] != 2.0f) return 4;
    if (!source->Prepare(generation, 1, false)) return 5;
    source->Committed();
    if (source->Prepare(generation, 1, false)) return 6;
    std::puts("[PASS] Relevant change persists until layout commit");
    auto order = std::make_shared<const std::vector<int>>(std::vector<int>{123, 1, 123});
    WallLayoutSource filtered(snapshot, order, L"C:\\wall-bench", false);
    values = filtered.Prepare(generation, 1, true);
    if (!values || values->size() != 3 || (*values)[0] != 2 || (*values)[2] != 2) return 7;
    std::puts("[PASS] Background filtering preserves duplicate source mappings");
    WallLayoutSource cancelled(snapshot, nullptr, L"C:\\wall-bench", false);
    if (cancelled.Prepare(generation, 2, true)) return 8;
    if (!cancelled.Prepare(generation, 1, true)) return 9;
    std::puts("[PASS] Cancelled initialization restarts without partial state");
}

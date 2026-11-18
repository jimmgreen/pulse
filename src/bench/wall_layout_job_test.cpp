#include "../ui/wall_layout_job.h"
#include <chrono>
#include <cstdio>
#include <thread>
int main() {
    using namespace pulse::ui;
    WallLayoutJob job;
    for (int i = 0; i < 50; ++i) job.RequestRows(std::vector<float>(100000, 1.5f), 600.0f + i, 100, 1);
    job.RequestRows({1, 2, 3}, 700, 100, 1);
    std::shared_ptr<const WallRows> ready;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready && std::chrono::steady_clock::now() < deadline) {
        ready = job.TakeReady();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto expected = BuildWallRows({1, 2, 3}, 700, 100, 1);
    if (!ready || ready->x != expected.x || ready->w != expected.w) return 1;
    std::puts("[PASS] Latest generation replaces rapid resize requests");
    job.RequestRows(std::vector<float>(100000, 1), 700, 100, 1);
    job.Cancel();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (job.TakeReady()) return 2;
    std::puts("[PASS] Cancelled layout cannot publish");
    std::atomic<uint64_t> generation{2};
    if (!BuildWallRows({1,2,3},700,100,1,&generation,1).x.empty()) return 3;
    std::puts("[PASS] Row calculation observes generation cancellation");
    const auto before = BuildWallRows(std::vector<float>(100, 1), 500, 100, 1);
    const auto after = BuildWallRows(std::vector<float>(100, 2), 500, 100, 1);
    const int item = before.row_first[3];
    const float scroll = before.row_top[3] + 7;
    const float adjusted = WallAnchorScroll(before, after, scroll);
    if (std::abs(adjusted - after.row_top[static_cast<size_t>(after.RowOf(item))] - 7) > 0.01f) return 4;
    std::puts("[PASS] Reflow preserves same immutable item and row offset");
    WallLayoutJob other;
    other.RequestRows({1, 1}, 400, 100, 1);
    job.Cancel();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    ready.reset();
    while (!ready && std::chrono::steady_clock::now() < end) {
        ready = other.TakeReady();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!ready || ready->x.size() != 2) return 5;
    std::puts("[PASS] Independent view cancellation preserves other view");}

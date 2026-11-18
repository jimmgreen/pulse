#include "../ui/wall_row_aspects.h"
#include <chrono>
#include <cstdio>
#include <string_view>

int main(int argc, char** argv) {
    using pulse::ui::WallRowAspects;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    std::vector<float> source{0, 1.5f, 2.0f};
    std::vector<int> order{2, 0};
    int projections = 0;
    const auto project = [&] {
        ++projections;
        std::vector<float> result;
        for (int index : order) result.push_back(source[static_cast<size_t>(index)]);
        return result;
    };
    WallRowAspects cached;
    check(cached.Refresh(1, project) && cached.Values() == std::vector<float>{2, 0}, "Initial filtered projection");
    const float* data = cached.Values().data();
    for (int i = 0; i < 100; ++i) cached.Refresh(1, project);
    check(projections == 1 && cached.Values().data() == data, "Stable frames allocate and project nothing");
    source[1] = 3;
    check(!cached.Refresh(2, project) && projections == 2, "Unrelated aspect update does not rebuild rows");
    check(!cached.Refresh(2, project) && projections == 2, "Unrelated revision is remembered");
    source[0] = 1.25f;
    check(cached.Refresh(3, project) && cached.Values()[1] == 1.25f, "Async visible aspect triggers rebuild");
    WallRowAspects other_order;
    order = {0, 2};
    check(other_order.Refresh(3, project) && other_order.Values() == std::vector<float>{1.25f, 2}, "New filter owner gets separate projection");
    WallRowAspects other_layout;
    check(other_layout.Refresh(3, project), "New width size or DPI cache entry initializes");
    source[0] = 0;
    check(other_order.Refresh(4, project) && other_order.Values()[0] == 0, "Recreated source revision refreshes missing aspects");
    if (argc > 1 && std::string_view(argv[1]) == "--bench") {
        constexpr size_t count = 100000;
        constexpr int frames = 1000;
        std::vector<float> large(count, 1.5f);
        std::vector<int> map(count);
        for (size_t i = 0; i < count; ++i) map[i] = static_cast<int>(count - i - 1);
        size_t visits = 0;
        const auto build = [&] {
            std::vector<float> values(count, 0.85f);
            for (size_t i = 0; i < count; ++i) values[i] = large[static_cast<size_t>(map[i])];
            visits += count;
            return values;
        };
        auto baseline = build();
        size_t equal = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; ++i) equal += baseline == build();
        const auto middle = std::chrono::steady_clock::now();
        const size_t baseline_visits = visits - count;
        WallRowAspects fast;
        fast.Refresh(1, build);
        visits = 0;
        const auto fast_start = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; ++i) equal += !fast.Refresh(1, build);
        const auto end = std::chrono::steady_clock::now();
        std::printf("BENCH %zu rows x %d stable calls: old %.3f ms / %zu mapped rows; cached %.3f ms / %zu mapped rows; checksum %zu\n",
            count, frames, std::chrono::duration<double, std::milli>(middle-start).count(), baseline_visits,
            std::chrono::duration<double, std::milli>(end-fast_start).count(), visits, equal);
        check(visits == 0 && equal == frames * 2, "Stable large-directory work is independent of item count");
    }
    return failures ? 1 : 0;
}

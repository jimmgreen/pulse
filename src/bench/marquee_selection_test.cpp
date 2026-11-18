#include "../app/marquee_selection.h"
#include "../ui/picker_marquee.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <string_view>

namespace {
thread_local bool counting = false;
thread_local size_t allocations = 0, allocated_bytes = 0;
int failures = 0;
using Set = std::unordered_set<int>;
using Delta = pulse::app::MarqueeSelection;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
Set Reference(const Set& base, bool additive, bool toggle, const std::vector<int>& hits) {
    Set result;
    if (additive) result = base;
    for (int i : hits) {
        if (toggle && base.contains(i)) result.erase(i);
        else result.insert(i);
    }
    return result;
}
Set Expanded(const Set& selected, bool all, int count) {
    if (!all) return selected;
    Set result;
    for (int i = 0; i < count; ++i) result.insert(i);
    return result;
}
bool Step(Delta& delta, Set& selected, bool& all, const Set& base,
    bool additive, bool toggle, int count, std::vector<int> hits) {
    const Set before = Expanded(selected, all, count);
    const Set expected = Reference(base, additive, toggle, hits);
    const bool changed = delta.Apply(selected, all, base, additive, toggle, count, hits);
    return Expanded(selected, all, count) == expected && changed == (before != expected) &&
        all == (count > 0 && expected.size() == static_cast<size_t>(count)) &&
        (!all || selected.empty());
}
void Regression() {
    {
        pulse::ui::PickerMarquee gesture;
        Set selected{0, 1};
        gesture.Begin(selected, 1, 0, 100, 100, 0, true, false);
        Check(!gesture.Move(102, 102, 4, 4) && gesture.Move(110, 120, 4, 4), "picker respects drag threshold");
        gesture.hits = {1, 2};
        gesture.Apply(selected, 3);
        Check(selected == Set({0, 2}), "picker Ctrl marquee toggles against immutable starting selection");
        gesture.hits = {2}; gesture.Apply(selected, 3);
        Check(selected == Set({0, 1, 2}), "picker shrink restores crossed item and retains explicit all-selection");
        const auto rectangle = gesture.Rect(40, {0, 0, 200, 200});
        Check(rectangle.top == 60 && rectangle.bottom == 120, "picker marquee anchor follows scrolled content");
        gesture.Begin(selected, 2, 0, 0, 0, 0, false, true);
        gesture.hits = {3}; gesture.Apply(selected, 4);
        Check(selected == Set({0, 1, 2, 3}), "picker Shift marquee adds without compacting away filenames");
        gesture.Begin(selected, 2, 0, 0, 0, 0, false, false);
        gesture.hits = {2}; gesture.Apply(selected, 4);
        Check(selected == Set({2}) && gesture.before == Set({0, 1, 2, 3}), "picker replace marquee retains cancellation baseline");
        gesture.Reset();
        Check(!gesture.pending && !gesture.active && gesture.before.empty(), "picker release resets gesture state");
    }
    std::mt19937 rng(0x51983u);
    bool random_ok = true;
    for (int mode = 0; mode < 3; ++mode) {
        for (int gesture = 0; gesture < 120; ++gesture) {
            const int count = gesture % 65;
            Set base, selected;
            for (int i = 0; i < count; ++i) {
                if (rng() % 3 == 0) base.insert(i);
                if (rng() % 2 == 0) selected.insert(i);
            }
            bool all = gesture % 7 == 0 && count > 0;
            if (all) selected.clear();
            Delta delta;
            for (int frame = 0; frame < 80; ++frame) {
                std::vector<int> hits;
                for (int i = 0; i < count; ++i) if (rng() % 4 == 0) {
                    hits.push_back(i);
                    if (rng() % 3 == 0) hits.push_back(i);
                }
                std::shuffle(hits.begin(), hits.end(), rng);
                random_ok &= Step(delta, selected, all, base, mode != 0, mode == 2, count, hits);
                random_ok &= Step(delta, selected, all, base, mode != 0, mode == 2, count, hits);
            }
        }
    }
    Check(random_ok, "random old-rebuild equivalence and unchanged revision (57,600 frames)");
    bool boundary = true;
    for (int mode = 0; mode < 3; ++mode) {
        Delta delta;
        Set selected{1, 3}, base{1, 3}; bool all = false;
        for (const auto& hits : std::vector<std::vector<int>>{{}, {0,1,2,3}, {3,2,1,0}, {2}, {}, {3}, {0,1,2,3}, {}})
            boundary &= Step(delta, selected, all, base, mode != 0, mode == 2, 4, hits);
        delta.Reset();
        base = {0, 2};
        boundary &= Step(delta, selected, all, base, mode != 0, mode == 2, 4, {0,2,3});
        // Esc restores the pre-press selection; ending the gesture drops all
        // delta state, so the next gesture starts independently.
        selected = {1, 3}; all = false; delta.Reset();
        boundary &= Step(delta, selected, all, base, mode != 0, mode == 2, 4, {1});
    }
    Check(boundary, "replace/union/xor expand shrink reverse empty all reset and rollback reuse");
    int identities[8]{};
    const pulse::app::MarqueeContext owner{&identities[0], &identities[1], &identities[2], &identities[3], 7};
    bool isolation = owner == owner;
    auto other = owner; other.pane = &identities[4]; isolation &= other != owner;
    other = owner; other.tab = &identities[4]; isolation &= other != owner;
    other = owner; other.snapshot = &identities[4]; isolation &= other != owner;
    other = owner; other.tree = &identities[4]; isolation &= other != owner;
    other = owner; ++other.generation; isolation &= other != owner;
    Check(isolation, "pane tab directory snapshot tree generation identity isolation");
    bool released = true, reused = true;
    for (const int size : {40, 100000}) {
        Set scratch;
        std::vector<int> hit_scratch;
        for (int i = 0; i < size; ++i) { scratch.insert(i); hit_scratch.push_back(i); }
        const size_t buckets = scratch.bucket_count(), capacity = hit_scratch.capacity();
        pulse::app::ClearMarqueeScratch(scratch);
        pulse::app::ClearMarqueeScratch(hit_scratch);
        if (size > 1024) released &= scratch.bucket_count() <= pulse::app::kMarqueeRetainedBucketCount && hit_scratch.capacity() == 0;
        else reused &= scratch.bucket_count() == buckets && hit_scratch.capacity() == capacity;
        released &= scratch.empty() && hit_scratch.empty();
        Delta gesture;
        Set current, original;
        bool compact = false;
        for (int i = 0; i < size; ++i) hit_scratch.push_back(i);
        gesture.Apply(current, compact, original, false, false, size, hit_scratch);
        const size_t prior_hits = gesture.RetainedHitCapacity(), prior_buckets = gesture.RetainedBucketCount();
        gesture.Reset();
        if (size > 1024) released &= gesture.RetainedHitCapacity() == 0 && gesture.RetainedBucketCount() <= pulse::app::kMarqueeRetainedBucketCount;
        else reused &= gesture.RetainedHitCapacity() == prior_hits && gesture.RetainedBucketCount() == prior_buckets;
        // Reset leaves a compact all-selection intact on its owning tab.
        released &= compact && current.empty();
        released &= Step(gesture, current, compact, original, false, false, size, {0});
    }
    Check(released, "end/reset releases 100k scratch capacities and permits next gesture");
    Check(reused, "end/reset retains 40-item scratch capacity for reuse");
    Delta empty; Set selected, base; bool all = false;
    Check(Step(empty, selected, all, base, false, false, 0, {}), "empty directory");
    Delta same; selected={1,2}; base=selected;
    Check(Step(same, selected, all, base, false, false, 4, {2,1}), "first replace with identical result does not change revision");
}
struct Measure { double ms; size_t calls, bytes; size_t checksum; };
Measure Perf(bool incremental, int mode) {
    constexpr int count = 100100;
    Set base, selected;
    for (int i = 0; i < 100000; ++i) base.insert(i);
    selected = base;
    bool all = false;
    Delta delta;
    std::vector<int> hits;
    hits.reserve(64);
    // Include the one-time initialization cost, using the same starting state.
    allocations = allocated_bytes = 0;
    counting = true;
    const auto start = std::chrono::steady_clock::now();
    size_t checksum = 0;
    for (int frame = 0; frame < 200; ++frame) {
        hits.clear();
        const int offset = (frame / 2) % 31;
        for (int i = 0; i < 40; ++i) hits.push_back(99980 + offset + i);
        if (incremental) delta.Apply(selected, all, base, mode != 0, mode == 2, count, hits);
        else {
            selected.clear(); all = false;
            if (mode != 0) selected = base;
            for (int i : hits) {
                if (mode == 2 && base.contains(i)) selected.erase(i);
                else selected.insert(i);
            }
            if (selected.size() == count) { all = true; selected.clear(); }
        }
        checksum += all ? count : selected.size();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
    counting = false;
    return {ms, allocations, allocated_bytes, checksum};
}
void Performance() {
    for (int mode = 0; mode < 3; ++mode) {
        const Measure old = Perf(false, mode), now = Perf(true, mode);
        std::printf("mode=%d frames=200 base=100000 hits=40 old_ms=%.3f new_ms=%.3f old_allocs=%zu new_allocs=%zu old_bytes=%zu new_bytes=%zu checksum=%zu/%zu\n",
            mode, old.ms, now.ms, old.calls, now.calls, old.bytes, now.bytes, old.checksum, now.checksum);
        Check(old.checksum == now.checksum, "performance workload result count identical");
        if (mode != 0) Check(now.calls < old.calls / 10, "large base allocation reduction exceeds 90 percent including initialization");
    }
}
} // namespace
void* operator new(size_t bytes) {
    if (counting) { ++allocations; allocated_bytes += bytes; }
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--perf") Performance();
    else Regression();
    std::printf("Failures: %d\n", failures);
    return failures ? 1 : 0;
}

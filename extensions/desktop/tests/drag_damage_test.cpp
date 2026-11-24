#include "host/model/drag_damage.h"
#include <chrono>
#include <cstdio>

using namespace pulse::desktop::model;

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    auto covers = [](RectI outer, RectI inner) {
        return outer.x <= inner.x && outer.y <= inner.y && outer.right() >= inner.right() && outer.bottom() >= inner.bottom();
    };
    DragDamageSnapshot a{{{40, 40, 300, 240}, {400, 40, 300, 240}, {760, 40, 300, 240}}, {}, {}};
    auto b = a;
    check(ComputeDragDamage(a, b).empty(), "snapped no-op produces no dirty region");
    b.frames[0].x += 10;
    auto dirty = ComputeDragDamage(a, b);
    check(covers(dirty, a.frames[0]) && covers(dirty, b.frames[0]), "move covers old and new frame");
    check(dirty.right() < a.frames[1].x, "unaffected neighbor excluded");
    b.frames[1].y += 260;
    dirty = ComputeDragDamage(a, b);
    check(covers(dirty, a.frames[1]) && covers(dirty, b.frames[1]), "collision-displaced neighbor covered");
    b = a; b.frames[0].w += 80;
    check(covers(ComputeDragDamage(a, b), b.frames[0]), "resize covers expanded frame");
    b = a; a.preview = a.frames[0]; b.preview = b.frames[2]; b.stack_target = "third";
    check(covers(ComputeDragDamage(a, b), a.preview) && covers(ComputeDragDamage(a, b), b.preview), "stack target transition covers both previews");
    a = b; b.stack_target.clear();
    check(!ComputeDragDamage(a, b).empty(), "stack highlight state change redraws unchanged geometry");

    DesktopModelState state;
    state.settings.organize_enabled = true;
    for (int i = 0; i < 4; ++i) {
        DesktopGroup group;
        group.id = "group" + std::to_string(i);
        group.x = 40 + i * 400;
        group.y = 40;
        group.cols = 3;
        group.visible_rows = 2;
        group.focus_visible = i != 1;
        state.groups.push_back(group);
    }
    for (int i = 0; i < 800; ++i) {
        DesktopItem item;
        item.name = L"Item" + std::to_wstring(800 - i);
        item.path = L"C:\\fixture\\" + item.name;
        item.pinned_group_id = state.groups[static_cast<size_t>(i) % state.groups.size()].id;
        state.items.push_back(item);
    }
    std::unordered_map<std::string, int> pages{{"group0", 2}};
    for (DesktopScene scene : {DesktopScene::Work, DesktopScene::Focus}) {
        state.settings.active_scene = scene;
        auto cached = ComputeGroupViews(state, 0, &pages);
        state.groups[0].x += 20;
        state.groups[1].y += 30;
        check(UpdateMovedGroupViewOrigins(state.groups, cached), "cached group identity accepted");
        const auto full = ComputeGroupViews(state, 0, &pages);
        bool equal = cached.size() == full.size();
        for (size_t i = 0; i < full.size(); ++i) {
            equal = equal && cached[i].frame == full[i].frame && cached[i].member_item_indices == full[i].member_item_indices &&
                cached[i].visible_item_indices == full[i].visible_item_indices && cached[i].page_index == full[i].page_index &&
                cached[i].page_count == full[i].page_count && cached[i].page_capacity == full[i].page_capacity;
        }
        check(equal, "origin-only cache equals full computation including Focus and pagination");
    }
    auto cached = ComputeGroupViews(state, 0, &pages);
    auto invalid = cached; invalid[0].group_id = "stale";
    check(!UpdateMovedGroupViewOrigins(state.groups, invalid), "stale cache requests full rebuild");
    constexpr int iterations = 300;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) { state.groups[0].x++; cached = ComputeGroupViews(state, 0, &pages); }
    const double full_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) { state.groups[0].x++; UpdateMovedGroupViewOrigins(state.groups, cached); }
    const double cached_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("LOCAL CPU model-only %d moves/800 items: full_ms=%.3f cached_ms=%.3f (not Win10 frame timing)\n", iterations, full_ms, cached_ms);
    return failures ? 1 : 0;
}

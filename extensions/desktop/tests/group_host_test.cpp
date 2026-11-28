// Exercise the real host message dispatch with model persistence redirected in-memory.
#include "../../../src/common/utf8_file.h"

#include <fstream>
#define PULSE_DESKTOP_DRAG_TEST 1
#define main DesktopPluginEntryForGroupTest
#include "host/host_main.cpp"
#undef main

namespace {
struct DesktopGroupHostTestPeer {
    static int RunAiVisibility() {
        wchar_t path[MAX_PATH * 4]{};
        const DWORD length = GetEnvironmentVariableW(L"PULSE_DESKTOP_AI_TEST_DIR", path, ARRAYSIZE(path));
        if (!length || length >= ARRAYSIZE(path)) return 2;
        const std::wstring settings = std::wstring(path, length) + L"\\ai.json";
        DesktopHostSession host({}, 0);
        Prepare(host);
        int failures = 0;
        const auto check = [&](bool ok, const char* name) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
            failures += !ok;
        };
        check(pulse::WriteUtf8FileAtomic(settings, L"{\"enabled\":false}"), "write isolated master switch off");
        overlays::ContextMenuSpec spec{};
        host.FillGroupContextMenuSpec(0, spec);
        check(!spec.ai_organize_enabled, "real group menu spec derives master visibility from saved settings");
        host.ExecuteContextMenuCommand("ai-organize-group", spec);
        host.ExecuteContextMenuCommand("ai-organize", {});
        check(!host.overlays_.IsOpen() && host.ai_req_seq_ == 0, "stale group and desktop commands do not open drawer");
        host.StartAiPlanRequest(ai::AiOrganizeStrategy::ArchiveClutter, {}, true);
        check(!host.active_ai_session_ && host.ai_req_seq_ == 0, "master off blocks even a stale local-only drawer request");
        check(pulse::WriteUtf8FileAtomic(settings, L"{}"), "set master on without a model key");
        host.FillGroupContextMenuSpec(0, spec);
        host.OpenAiOrganizeDrawer();
        check(spec.ai_organize_enabled && host.overlays_.ActiveKind() == overlays::OverlayKind::AiOrganizeDrawer &&
              host.ai_view_state_.phase == overlays::AiDrawerPhase::UnconfiguredGuide && !host.active_ai_session_,
            "master on restores actual drawer with unconfigured guide (no provider call)");
        check(pulse::WriteUtf8FileAtomic(settings, L"{\"enabled\":false}"), "disable while drawer is open");
        bool handled = false;
        host.OnSurfaceMessage(nullptr, WM_TIMER, kTimerHeartbeat, 0, handled);
        check(handled && !host.overlays_.IsOpen() && !host.active_ai_session_,
            "desktop heartbeat dismisses active AI drawer after disabling");
        return failures ? 1 : 0;
    }

    static void Prepare(DesktopHostSession& host) {
        host.CancelGroupDrag(false);
        host.group_springs_.Clear();
        host.group_ghost_.Hide();
        host.group_hover_.Reset();
        host.state_ = {};
        host.state_.settings.organize_enabled = true;
        host.state_.settings.tiles_enabled = false;
        for (int i = 0; i < 3; ++i) {
            model::DesktopGroup group;
            group.id = "group-" + std::to_string(i); group.title = L"Fixture";
            group.x = 24 + i * 360; group.y = 80; group.cols = 3; group.visible_rows = 3;
            host.state_.groups.push_back(group);
            for (int j = 0; j < 7; ++j) {
                model::DesktopItem item;
                item.path = L"C:\\fixture\\" + std::to_wstring(i) + L"-" + std::to_wstring(j) + L".txt";
                item.name = std::to_wstring(j) + L".txt"; item.extension = L".txt";
                item.pinned_group_id = group.id; item.loose_pos = {1700, 900};
                host.state_.items.push_back(item);
            }
        }
        host.interaction_ = {};
        host.undo_.Clear(); host.test_saved_model_.clear();
        host.RefreshGroupViews();
    }
    static int Run() {
        int count = 0, failed = 0;
        auto check = [&](bool ok, const char* name) {
            ++count; failed += !ok; std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        };
        DesktopHostSession host({}, 0);
        auto send = [&](UINT msg, WPARAM wp = 0, LPARAM lp = 0) {
            bool handled = false; host.OnSurfaceMessage(nullptr, msg, wp, lp, handled);
        };
        Prepare(host);
        host.BeginGroupDrag(0, 60, 95, false);
        const auto original = model::SerializeModelStateJson(host.state_);
        send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(63, 98));
        check(!host.group_drag_session_.Active(), "group header movement below six DIP stays pending");
        send(WM_LBUTTONUP, 0, MAKELPARAM(63, 98));
        check(host.undo_.UndoDepth() == 0 && host.test_saved_model_.empty(), "header click creates no layout undo or save");
        Prepare(host);
        host.BeginGroupDrag(0, 60, 95, false);
        const auto before = model::SerializeModelStateJson(host.state_);
        const auto* cached_members = host.group_views_[0].visible_item_indices.data();
        const auto cached_item_rect = model::GroupItemRect(host.group_views_[0], 0);
        send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(200, 300));
        check(host.group_views_[0].visible_item_indices.data() == cached_members,
              "ordinary group movement reuses cached membership without regrouping");
        const auto moved_item_rect = model::GroupItemRect(host.group_views_[0], 0);
        check(moved_item_rect.x - cached_item_rect.x == host.state_.groups[0].x - 24 &&
              moved_item_rect.y - cached_item_rect.y == host.state_.groups[0].y - 80,
              "cached group movement translates shared item hit rectangles");
        check(host.group_drag_session_.Active(), "real mouse dispatch starts group drag after threshold");
        const auto once = model::SerializeModelStateJson(host.state_);
        send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(200, 300));
        check(model::SerializeModelStateJson(host.state_) == once, "same cursor replays snapshot without cumulative collision drift");
        host.SaveModel();
        check(host.test_saved_model_ == before, "background SaveModel excludes group preview");
        send(WM_KEYDOWN, VK_ESCAPE);
        check(model::SerializeModelStateJson(host.state_) == before && !host.group_drag_session_.Pending(),
              "Escape restores original groups and clears transaction");
        check(!host.undo_.CanUndo(), "Escape adds no undo entry");
        Prepare(host);
        host.BeginGroupDrag(0, 60, 95, false);
        const auto capture_before = model::SerializeModelStateJson(host.state_);
        send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(200, 300)); send(WM_CAPTURECHANGED);
        check(model::SerializeModelStateJson(host.state_) == capture_before, "capture loss restores group preview");
        host.BeginGroupDrag(0, 60, 95, false);
        send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(200, 300)); send(WM_CANCELMODE);
        check(model::SerializeModelStateJson(host.state_) == capture_before, "cancel mode restores group preview");
        Prepare(host);
        host.BeginGroupDrag(0, 60, 95, false);
        send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(200, 300));
        send(WM_LBUTTONUP, 0, MAKELPARAM(200, 300));
        check(host.undo_.UndoDepth() == 1 && !host.test_saved_model_.empty(), "group release creates exactly one undo and persists");
        check(host.undo_.Undo(host.state_) && host.state_.groups[0].x == 24, "group move undo restores origin");

        Prepare(host);
        pulse::desktop::interaction::GroupDragSession session;
        session.Begin(host.state_.groups, host.group_views_, 0, {60,95}, false, {0,0,1920,1080},
                      model::DesktopIconSize::Medium, {});
        session.Update(host.state_.groups, {440,95}, 1000);
        check(!session.MergeReady(1599) && session.MergeReady(1600), "merge requires complete 600ms dwell");
        session.Update(host.state_.groups, {700,600}, 1601);
        check(session.MergeTarget().empty() && !session.MergeReady(3000), "leaving target cancels merge dwell");
        session.Update(host.state_.groups, {440,95}, 3100);
        bool merged = false;
        check(session.Commit(host.state_.groups, 3700, merged) && merged &&
              model::TabMembers(host.state_.groups, "group-0").size() == 2, "dwell commit merges the intended two groups");
        host.RefreshGroupViews();
        check(host.CurrentGroupFrames().size() == 2, "inactive tab contributes no duplicate obstacle frame");
        const auto stack = model::TabMembers(host.state_.groups, "group-0");
        const int old_x = host.state_.groups[stack[0]].x;
        host.BeginGroupDrag(stack[0], old_x + 30, 95, false);
        send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(old_x + 130, 400));
        check(host.state_.groups[stack[0]].x == host.state_.groups[stack[1]].x &&
              host.state_.groups[stack[0]].y == host.state_.groups[stack[1]].y, "stack drag moves every member together");
        send(WM_CAPTURECHANGED);
        check(host.state_.groups[stack[0]].x == old_x && host.state_.groups[stack[1]].x == old_x,
              "stack capture cancellation restores every member");

        Prepare(host);
        const auto approach_groups = host.state_.groups;
        host.BeginGroupDrag(0, 60, 95, false);
        bool fixed_during_approach = true;
        int first_displaced_cursor = -1;
        const auto approach_start = GetTickCount64() - 1400;
        for (int cursor = 72; cursor <= 432; cursor += 12) {
            host.UpdateGroupDrag(cursor, 95, approach_start + static_cast<uint64_t>(cursor - 60));
            for (size_t i = 1; i < approach_groups.size(); ++i) {
                if (host.state_.groups[i].x != approach_groups[i].x || host.state_.groups[i].y != approach_groups[i].y) {
                    fixed_during_approach = false;
                    if (first_displaced_cursor < 0) first_displaced_cursor = cursor;
                }
            }
        }
        std::printf("approach first_displaced_cursor=%d\n", first_displaced_cursor);
        check(fixed_during_approach, "incremental approach keeps every non-source group stationary before merge zone");
        host.UpdateGroupDrag(440, 95, approach_start + 500);
        send(WM_LBUTTONUP, 0, MAKELPARAM(440,95));
        check(model::TabMembers(host.state_.groups,"group-0").size() == 2 && host.undo_.UndoDepth() == 1,
              "incremental approach and dwell release merge once without chasing target");

        Prepare(host);
        const auto before_overlap=model::SerializeModelStateJson(host.state_);
        host.BeginGroupDrag(0,60,95,false);
        host.UpdateGroupDrag(300,95,1000);
        check(host.group_drag_session_.MergeTarget().empty()&&host.state_.groups[1].x==384&&
              model::ComputeGroupFrame(host.state_.groups[0]).intersects(model::ComputeGroupFrame(host.state_.groups[1])),
              "pre-zone overlap changes only source preview and has no collision side effects");
        send(WM_LBUTTONUP,0,MAKELPARAM(300,95));
        bool separated=true;
        for(size_t i=0;i<host.state_.groups.size();++i)
            for(size_t j=i+1;j<host.state_.groups.size();++j)
                separated=separated&&!model::ComputeGroupFrame(host.state_.groups[i]).intersects(model::ComputeGroupFrame(host.state_.groups[j]));
        check(separated&&host.undo_.UndoDepth()==1&&model::TabMembers(host.state_.groups,"group-0").size()==1,
              "ordinary release resolves preview overlap once without merging");
        check(host.undo_.Undo(host.state_)&&model::SerializeModelStateJson(host.state_)==before_overlap,
              "ordinary release undo restores source and all affected neighbors");
        Prepare(host);
        const auto before_cancel=model::SerializeModelStateJson(host.state_);
        host.BeginGroupDrag(0,60,95,false);
        for(int cursor=72;cursor<=432;cursor+=12)host.UpdateGroupDrag(cursor,95,1000+cursor);
        send(WM_CAPTURECHANGED);
        check(model::SerializeModelStateJson(host.state_)==before_cancel&&host.undo_.UndoDepth()==0,
              "capture cancellation restores whole incremental trajectory snapshot");
        Prepare(host);
        const auto before_resize=model::SerializeModelStateJson(host.state_);
        host.BeginGroupDrag(0,100,100,true);
        bool resize_neighbors_fixed=true;
        for(int delta=12;delta<=240;delta+=12) {
            host.UpdateGroupDrag(100+delta,100,1000+delta);
            resize_neighbors_fixed=resize_neighbors_fixed&&host.state_.groups[1].x==384&&host.state_.groups[1].y==80&&
                host.state_.groups[2].x==744&&host.state_.groups[2].y==80;
        }
        check(resize_neighbors_fixed&&host.state_.groups[0].cols>3,
              "incremental resize preview leaves every non-source group stationary");
        send(WM_CAPTURECHANGED);
        check(model::SerializeModelStateJson(host.state_)==before_resize&&host.undo_.UndoDepth()==0,
              "resize cancellation restores original geometry without undo");

        Prepare(host);
        host.BeginGroupDrag(0, 60, 95, false);
        host.UpdateGroupDrag(440, 95, GetTickCount64() - 700);
        check(host.state_.groups[1].x == 384 && host.state_.groups[1].y == 80,
              "merge dwell does not push the target away from pointer");
        send(WM_LBUTTONUP, 0, MAKELPARAM(440,95));
        check(model::TabMembers(host.state_.groups,"group-0").size() == 2 && host.undo_.UndoDepth() == 1,
              "real release commits dwell merge with exactly one undo");
        check(host.undo_.Undo(host.state_) && model::TabMembers(host.state_.groups,"group-0").size() == 1,
              "merge undo restores independent groups");
        Prepare(host);
        model::MergeGroupTabs(host.state_.groups,"group-0","group-1"); host.RefreshGroupViews();
        host.BeginGroupDrag(0, 600, 250, true);
        send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(690,330));
        check(host.state_.groups[0].cols == host.state_.groups[1].cols &&
              host.state_.groups[0].visible_rows == host.state_.groups[1].visible_rows && host.state_.groups[0].cols > 3,
              "stack resize updates all members from starting geometry");
        send(WM_LBUTTONUP,0,MAKELPARAM(690,330));
        check(host.undo_.UndoDepth()==1, "stack resize release adds one undo");
        Prepare(host);
        const auto resize_cell=model::IconCellMetrics(model::DesktopIconSize::Medium);
        session.Begin(host.state_.groups,host.group_views_,0,{100,100},true,{0,0,1920,1080},model::DesktopIconSize::Medium,{});
        session.Update(host.state_.groups,{100+resize_cell.x,100},1000);
        check(host.state_.groups[0].cols==4,"positive one-cell resize expands exactly one column");
        session.Update(host.state_.groups,{100-resize_cell.x,100},1100);
        check(host.state_.groups[0].cols==2,"negative one-cell resize shrinks exactly one column");
        session.Cancel(host.state_.groups);
        Prepare(host);
        for (int i=3;i<9;++i) {
            auto group=host.state_.groups[1]; group.id="group-"+std::to_string(i);
            host.state_.groups.push_back(group);
            model::MergeGroupTabs(host.state_.groups,group.id,"group-1");
        }
        model::MergeGroupTabs(host.state_.groups,"group-2","group-1");
        model::SelectGroupTab(host.state_.groups,"group-1"); host.RefreshGroupViews();
        const auto full=model::SerializeModelStateJson(host.state_);
        session.Begin(host.state_.groups,host.group_views_,0,{60,95},false,{0,0,1920,1080},model::DesktopIconSize::Medium,{});
        session.Update(host.state_.groups,{440,95},1000);
        check(!session.Commit(host.state_.groups,1600,merged) && !merged && model::SerializeModelStateJson(host.state_)==full,
              "over-limit merge rolls back atomically instead of moving source");

        Prepare(host);
        model::MergeGroupTabs(host.state_.groups, "group-0", "group-1"); host.RefreshGroupViews();
        const auto tab_layout = render::ComputeGroupTabLayout(host.group_views_[0]);
        const auto tab = std::find_if(tab_layout.tabs.begin(), tab_layout.tabs.end(), [](const auto& t) { return t.group_id == "group-1"; });
        if (tab != tab_layout.tabs.end()) {
            const int x = tab->rect.x + tab->rect.w / 2, y = tab->rect.y + tab->rect.h / 2;
            send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x,y)); send(WM_LBUTTONUP, 0, MAKELPARAM(x,y));
        }
        check(tab != tab_layout.tabs.end() && host.state_.groups[1].tab_active && !host.state_.groups[0].tab_active,
              "actual tab click changes active member");
        check(host.undo_.UndoDepth() == 1 && !host.test_saved_model_.empty(), "tab switch is persisted once without drag undo");
        check(!host.FindItemCellRect(host.state_.items[0].path).has_value(), "inactive tab item has no loose or drag anchor");
        host.undo_.Clear(); host.test_saved_model_.clear();
        const auto inactive_layout=render::ComputeGroupTabLayout(host.group_views_[1]);
        const auto inactive_tab=std::find_if(inactive_layout.tabs.begin(),inactive_layout.tabs.end(),[](const auto& t){return t.group_id=="group-0";});
        if(inactive_tab!=inactive_layout.tabs.end()) {
            const int x=inactive_tab->rect.x+inactive_tab->rect.w/2,y=inactive_tab->rect.y+inactive_tab->rect.h/2;
            send(WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
            send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x,y+250));
            send(WM_LBUTTONUP,0,MAKELPARAM(x,y+250));
        }
        check(inactive_tab!=inactive_layout.tabs.end()&&host.undo_.UndoDepth()==1&&host.state_.groups[0].tab_active,
              "dragging inactive tab combines selection and movement in one undo");
        overlays::ContextMenuSpec spec; host.FillGroupContextMenuSpec(0, spec);
        check(spec.group_tabbed, "group menu detects tab stack");
        host.ExecuteContextMenuCommand("group-detach-tab", spec);
        check(model::TabMembers(host.state_.groups, "group-1").size() == 1 &&
              model::TabMembers(host.state_.groups, "group-0").size() == 1, "menu command detaches active group");

        for (int mode = 0; mode < 4; ++mode) {
            Prepare(host); overlays::ContextMenuSpec view_spec; host.FillGroupContextMenuSpec(0, view_spec);
            host.ExecuteContextMenuCommand("group-view:" + std::to_string(mode), view_spec);
            host.RefreshGroupViews();
            const auto& view = host.group_views_[0];
            bool hits = !view.visible_item_indices.empty();
            for (size_t i = 0; i < view.visible_item_indices.size(); ++i) {
                const auto rect = model::GroupItemRect(view, i);
                const auto hit = host.HitTestItem(rect.x + rect.w / 2, rect.y + rect.h / 2);
                hits = hits && !rect.empty() && hit && *hit == view.visible_item_indices[i];
            }
            check(hits, "every group view uses matching rendered item index for clicks");
            check(host.state_.groups[1].view_mode == model::GroupViewMode::Icons, "view change remains per group");
        }

        Prepare(host); host.state_.groups[0].collapsed = true; host.RefreshGroupViews();
        const auto collapsed = model::SerializeModelStateJson(host.state_);
        host.UpdateGroupHoverAt(60,95,true,1000); host.UpdateGroupHoverAt(60,95,true,1249);
        check(!host.group_views_[0].expanded, "hover waits 250ms before expansion");
        host.UpdateGroupHoverAt(60,95,true,1250);
        check(host.group_views_[0].temporarily_expanded && host.state_.groups[0].collapsed,
              "hover expands only runtime view, preserving collapsed setting");
        host.SaveModel(); check(host.test_saved_model_ == collapsed, "temporary expansion is absent from persisted state");
        const auto content = model::GroupItemRect(host.group_views_[0],0);
        host.UpdateGroupHoverAt(content.x+4,content.y+4,true,1500);
        check(host.group_views_[0].temporarily_expanded, "entering expanded content keeps group open");
        host.interaction_.dragging_items = true;
        host.UpdateGroupHoverAt(content.x+4,content.y+4,true,1550);
        check(host.group_views_[0].temporarily_expanded, "dragged file stays inside expanded group");
        host.interaction_.dragging_items = false;
        host.state_.groups[1].x=host.state_.groups[0].x; host.state_.groups[1].y=content.y;
        host.RefreshGroupViews();
        const auto top_item=model::GroupItemRect(host.group_views_[0],0);
        send(WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(top_item.x+8,top_item.y+8));
        check(host.pending_item_drag_&&!host.group_drag_session_.Pending()&&
              host.interaction_.focused_path==host.state_.items[host.group_views_[0].visible_item_indices[0]].path,
              "temporary content click wins over underlying group header");
        send(WM_CAPTURECHANGED);
        host.UpdateGroupHoverAt(1800,1000,false,1600); host.UpdateGroupHoverAt(1800,1000,false,1999);
        check(host.group_views_[0].temporarily_expanded, "leave grace retains expansion for 400ms");
        host.UpdateGroupHoverAt(1800,1000,false,2000);
        check(!host.group_views_[0].expanded && host.state_.groups[0].collapsed, "leave closes only transient expansion");
        pulse::desktop::interaction::GroupHoverSession hover;
        hover.Update("one",true,false,100); hover.Update("one",true,false,350);
        hover.Update({},false,true,1000); hover.Update({},false,true,3000);
        check(hover.Expanded() == "one", "menu hold preserves expanded group without pointer on surface");
        hover.Update({},false,false,3100); hover.Update({},false,false,3500);
        check(hover.Expanded().empty(), "closing menu starts delayed collapse");
        Prepare(host);
        host.pending_tile_idx_ = 9;
        send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(300,300));
        check(!host.pending_tile_idx_ && !host.tile_drag_session_.Active(), "stale pending tile index is cancelled before drag access");

        // Reference-aligned group drag ghost preview, calm non-pushing nearest free spot, and spring snap-back.
        Prepare(host);
        host.BeginGroupDrag(0, 60, 95, false);
        send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(207, 307));
        check(host.interaction_.dragging_group && host.interaction_.dragging_group_id == "group-0" &&
              !host.interaction_.drag_group_preview_rect.empty(),
              "group drag activates smooth translucent target ghost preview");
        const model::RectI predicted_ghost = host.interaction_.drag_group_preview_rect;
        const int release_x = host.state_.groups[0].x;
        const int release_y = host.state_.groups[0].y;
        send(WM_LBUTTONUP, 0, MAKELPARAM(207, 307));
        check(!host.interaction_.dragging_group &&
              host.state_.groups[0].x == predicted_ghost.x && host.state_.groups[0].y == predicted_ghost.y &&
              host.state_.groups[1].x == 384 && host.state_.groups[2].x == 744,
              "group release commits exact ghost target while keeping sibling groups stationary");
        check((release_x != predicted_ghost.x || release_y != predicted_ghost.y) &&
              !host.group_springs_.Empty() && host.interaction_.group_anim_positions.contains("group-0"),
              "off-grid group release launches SwiftUI spring snap-back animation");
        host.TickMotionAnimations(NowAnimMs() + 200.0);
        check(host.interaction_.group_anim_positions.contains("group-0"),
              "group spring snap-back animates smoothly mid-flight");
        host.TickMotionAnimations(NowAnimMs() + 800.0);
        check(host.group_springs_.Empty() && host.interaction_.group_anim_positions.empty(),
              "group spring snap-back settles cleanly to committed slot");

        // 查看: 整理 + 小组件 / 只显示小组件, and 显示小组件 on its own.
        Prepare(host);
        host.ExecuteContextMenuCommand("view-mode-widgets", {});
        check(!host.state_.settings.organize_enabled && host.state_.settings.tiles_enabled && !host.test_saved_model_.empty(),
              "只显示小组件 turns organizing off, shows widgets and persists");
        host.ExecuteContextMenuCommand("view-mode-organize", {});
        check(host.state_.settings.organize_enabled && host.state_.settings.tiles_enabled, "整理 + 小组件 brings groups back");
        host.ExecuteContextMenuCommand("toggle-tiles", {});
        check(host.state_.settings.organize_enabled && !host.state_.settings.tiles_enabled,
              "显示小组件 toggles widgets without touching organizing");

        // 新建分组: a free spot, the next free name, one undo; selected files move in; beside a group's 「+」.
        Prepare(host);
        const std::wstring new_title = l10n::Get(l10n::StringId::DesktopGroupNew);
        const std::string g1 = host.CreateNewGroupNear(1000, 600);
        const auto new_group = [&](const std::string& id) -> const model::DesktopGroup* {
            for (const auto& g : host.state_.groups) if (g.id == id) return &g;
            return nullptr;
        };
        const auto overlaps_other = [&](const std::string& id) {
            const auto* g = new_group(id);
            if (!g) return true;
            const auto f = model::ComputeGroupFrame(*g);
            for (const auto& o : host.state_.groups)
                if (o.id != id && model::ComputeGroupFrame(o).intersects(f)) return true;
            return false;
        };
        check(host.state_.groups.size() == 4 && new_group(g1) && new_group(g1)->title == new_title &&
              !overlaps_other(g1) && host.undo_.UndoDepth() == 1 && !host.test_saved_model_.empty(),
              "新建分组 adds one titled, non-overlapping group with one undo");
        const std::string g2 = host.CreateNewGroupNear(1000, 600);
        check(new_group(g2) && new_group(g2)->title == new_title + L" 2" && g2 != g1 && !overlaps_other(g2),
              "a second new group takes the next free name and its own spot");
        const std::wstring moved = host.state_.items[0].path;
        const std::string g3 = host.CreateNewGroupNear(1000, 600, {moved});
        check(host.state_.items[0].pinned_group_id == g3 && !host.state_.items[0].force_loose,
              "新建分组并移入 moves the selected file into the new group");
        {
            const auto keep = host.state_;
            // A fresh desktop with room right of the middle card (the test work area is 1280 wide).
            Prepare(host);
            host.state_.groups[2].y = 460; host.RefreshGroupViews();
            const auto f1 = model::ComputeGroupFrame(host.state_.groups[1]);
            const std::string g4 = host.CreateNewGroupNear(0, 0, {}, "group-1");
            const auto* n4 = new_group(g4);
            if (n4) std::printf("beside: card right=%d y=%d, new x=%d y=%d\n", f1.right(), f1.y, n4->x, n4->y);
            check(n4 && n4->x >= f1.right() && n4->x <= f1.right() + 2 * model::kSnapGridPx &&
                  n4->y == host.state_.groups[1].y && !overlaps_other(g4),
                  "the header 「+」 places the new group right beside its card");
            host.state_ = keep; host.RefreshGroupViews();
        }
        host.state_.settings.organize_enabled = false;
        const std::string g5 = host.CreateNewGroupNear(1000, 600);
        check(new_group(g5) && host.state_.settings.organize_enabled, "a new group switches 整理 back on so it is seen");
        check(host.undo_.Undo(host.state_) && !new_group(g5), "undo removes the new group");

        // 任意拖动: the slot under the pointer, an indexed drop, and reordering inside a group.
        Prepare(host);
        {
            const auto& view = host.group_views_[0];
            const auto r4 = model::GroupItemRect(view, 4);
            host.drop_insert_index_.reset(); host.interaction_.drop_insert_caret = {};
            host.UpdateItemDropSlot(0, r4.x + 2, r4.y + r4.h / 2);
            check(host.drop_insert_index_ && *host.drop_insert_index_ == 4 && !host.interaction_.drop_insert_caret.empty(),
                  "pointer at an icon's left edge targets the slot before it, with a caret");
        }
        const auto member_paths = [&](size_t gi) {
            std::vector<std::wstring> out;
            for (const size_t idx : host.group_views_[gi].member_item_indices) out.push_back(host.state_.items[idx].path);
            return out;
        };
        const auto before_drop = model::SerializeModelStateJson(host.state_);
        const std::wstring foreign = L"C:\\fixture\\1-0.txt";
        host.DropItemsIntoGroup("group-0", {foreign}, size_t{2});
        auto order = member_paths(0);
        check(order.size() == 8 && order[2] == foreign && host.state_.groups[0].sort_by == model::GroupSortBy::Manual &&
              host.undo_.UndoDepth() == 1, "a file dropped between icons lands exactly there and the group turns manual");
        host.DropItemsIntoGroup("group-0", {L"C:\\fixture\\0-6.txt"}, size_t{0});
        order = member_paths(0);
        check(order.size() == 8 && order[0] == L"C:\\fixture\\0-6.txt" && order[3] == foreign,
              "dragging inside the group reorders it");
        host.DropItemsIntoGroup("group-0", {L"C:\\fixture\\2-0.txt"}, std::nullopt);
        order = member_paths(0);
        check(order.size() == 9 && order.back() == L"C:\\fixture\\2-0.txt", "a plain drop on a manual group appends");
        check(host.undo_.Undo(host.state_) && host.undo_.Undo(host.state_) && host.undo_.Undo(host.state_) &&
              model::SerializeModelStateJson(host.state_) == before_drop, "each drop undoes on its own back to the start");

        // Holding a dragged file on 下一页 turns the page; leaving or ending the drag disarms it.
        Prepare(host);
        host.state_.groups[0].visible_rows = 1; host.RefreshGroupViews();
        const int pages = host.group_views_[0].page_count;
        host.interaction_.dragging_items = true;
        host.drag_flip_group_ = "group-0"; host.drag_flip_step_ = 1;
        host.FlipDragPage();
        const int p1 = host.group_views_[0].page_index;
        host.FlipDragPage(); host.FlipDragPage(); host.FlipDragPage();
        check(pages >= 3 && p1 == 1 && host.group_views_[0].page_index == pages - 1, "page flips forward and stops at the last page");
        host.drag_flip_step_ = -1; host.FlipDragPage();
        check(host.group_views_[0].page_index == pages - 2, "下一页 / 上一页 both flip");
        host.interaction_.dragging_items = false; host.FlipDragPage();
        check(host.drag_flip_group_.empty(), "flipping stops when the drag ends");
        host.drag_flip_group_ = "group-0"; host.drag_flip_step_ = 1;
        host.drop_insert_index_ = 3; host.interaction_.drop_insert_caret = {10, 10, 3, 40};
        host.pending_item_drag_ = true;
        send(WM_CAPTURECHANGED);
        check(host.drag_flip_group_.empty() && !host.drop_insert_index_ && host.interaction_.drop_insert_caret.empty(),
              "capture loss clears the caret, the slot and the page flip");

        // 锁定分组: a locked card ignores drags and resizes but still takes header clicks.
        Prepare(host);
        {
            overlays::ContextMenuSpec lspec{};
            host.FillGroupContextMenuSpec(0, lspec);
            check(!lspec.group_locked, "a group starts unlocked");
            host.ExecuteContextMenuCommand("toggle-lock", lspec);
            check(host.state_.groups[0].locked && !host.test_saved_model_.empty(), "锁定分组 locks the card and saves");
            host.FillGroupContextMenuSpec(0, lspec);
            check(lspec.group_locked, "the menu shows the lock as checked");
            const int x0 = host.state_.groups[0].x, y0 = host.state_.groups[0].y, c0 = host.state_.groups[0].cols;
            host.test_info_toasts_.clear();
            host.BeginGroupDrag(0, 60, 95, false);
            send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(200, 300));
            send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(260, 320));
            send(WM_LBUTTONUP, 0, MAKELPARAM(260, 320));
            check(host.state_.groups[0].x == x0 && host.state_.groups[0].y == y0 && !host.group_drag_session_.Pending() &&
                  host.undo_.UndoDepth() == 0, "a locked card does not move");
            check(host.test_info_toasts_.size() == 1, "dragging a locked card says once why it stays");
            host.BeginGroupDrag(0, 300, 300, true);
            check(!host.group_drag_session_.Pending() && host.state_.groups[0].cols == c0, "a locked card does not resize");
            host.ExecuteContextMenuCommand("group-cols:5", lspec);
            check(host.state_.groups[0].cols == c0, "nor change its width from the menu");
            host.ExecuteContextMenuCommand("toggle-lock", lspec);
            host.BeginGroupDrag(0, 60, 95, false);
            send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(200, 300));
            send(WM_LBUTTONUP, 0, MAKELPARAM(200, 300));
            check(!host.state_.groups[0].locked && (host.state_.groups[0].x != x0 || host.state_.groups[0].y != y0),
                  "unlocked, it moves again");
        }

        // Dock: files dropped on an app open with it; the wheel steps through an app's windows.
        {
            render::DockBarItem app;
            app.kind = render::DockBarItemKind::App; app.key = L"c:\\apps\\editor.exe";
            app.launch_path = L"C:\\Apps\\Editor.exe"; app.title = L"Editor"; app.pinned = true;
            render::DockBarItem link = app;
            link.key = L"ms-settings:"; link.launch_path = L"ms-settings:"; link.title = L"Settings";
            render::DockBarItem viewer = app;
            viewer.key = L"c:\\apps\\viewer.exe"; viewer.launch_path = L"C:\\Apps\\Viewer.exe";
            viewer.windows = {reinterpret_cast<HWND>(0x10), reinterpret_cast<HWND>(0x20), reinterpret_cast<HWND>(0x30)};
            const auto keep_items = host.live_data_.dock.items;
            host.live_data_.dock.items = {app, link, viewer};
            check(host.DockItemOpensFiles(app) && !host.DockItemOpensFiles(link) && host.DockItemOpensFiles(viewer),
                  "an exe or a running app takes files; a settings link does not");
            check(host.DockOpenWithLabel(app).find(L"Editor") != std::wstring::npos, "the drop pill names the app");
            check(host.IsAppLikePath(L"C:\\x\\Tool.EXE") && host.IsAppLikePath(L"C:\\x\\a.lnk") && !host.IsAppLikePath(L"C:\\x\\a.txt"),
                  "programs and shortcuts are pinned rather than opened");
            check(host.QuoteCommandArgument(L"C:\\a b\\x.txt") == L"\"C:\\a b\\x.txt\"" &&
                  host.QuoteCommandArgument(L"C:\\") == L"\"C:\\\\\"", "file arguments are quoted the way the app reads them");
            host.test_dock_activations_.clear();
            host.DropPathsOnDockApp(app.key, {L"C:\\d\\a b.txt", L"C:\\d\\c.md"});
            check(host.test_dock_activations_.size() == 1 &&
                  host.test_dock_activations_[0] == L"open-with:C:\\Apps\\Editor.exe|C:\\d\\a b.txt|C:\\d\\c.md",
                  "files dropped on a Dock app open with it");
            host.test_dock_activations_.clear();
            host.dock_wheel_key_.clear();
            for (int i = 0; i < 4; ++i) host.CycleDockAppWindows(2, -WHEEL_DELTA);
            check(host.test_dock_activations_ == std::vector<std::wstring>{L"cycle:0", L"cycle:1", L"cycle:2", L"cycle:0"},
                  "wheel down steps through the app's windows and wraps");
            host.CycleDockAppWindows(2, WHEEL_DELTA);
            check(host.test_dock_activations_.back() == L"cycle:2", "wheel up steps back");
            host.CycleDockAppWindows(2, -40); host.CycleDockAppWindows(2, -40);
            check(host.test_dock_activations_.size() == 5, "small touchpad steps add up first");
            host.CycleDockAppWindows(2, -40);
            check(host.test_dock_activations_.size() == 6 && host.test_dock_activations_.back() == L"cycle:0",
                  "a full notch of them moves one window");
            check(!host.CycleDockAppWindows(0, -WHEEL_DELTA), "an app with no windows leaves the wheel to the desktop");
            host.live_data_.dock.items = keep_items;
        }

        // 筛选: the keyword narrows a card without saving or undo; drops count among the shown icons.
        Prepare(host);
        {
            overlays::ContextMenuSpec fl_spec{};
            host.FillGroupContextMenuSpec(0, fl_spec);
            check(!fl_spec.group_filtered, "a group starts unfiltered");
            host.SetGroupFilter("group-0", L"3");
            check(host.group_views_[0].total_item_count == 1 && host.group_views_[0].unfiltered_item_count == 7 &&
                  host.group_views_[0].member_item_indices.size() == 7, "筛选 shows only matching names");
            check(host.undo_.UndoDepth() == 0 && host.test_saved_model_.empty(), "filtering neither saves nor adds undo");
            host.FillGroupContextMenuSpec(0, fl_spec);
            check(fl_spec.group_filtered, "the menu offers 清除筛选 while filtered");
            host.ExecuteContextMenuCommand("group-filter-clear", fl_spec);
            check(host.state_.groups[0].filter_text.empty() && host.group_views_[0].total_item_count == 7, "清除筛选 shows everything again");
            host.SetGroupFilter("group-0", L"5");
            const std::wstring fl_foreign = L"C:\\fixture\\1-0.txt";
            host.DropItemsIntoGroup("group-0", {fl_foreign}, size_t{0});
            const auto fl_order = member_paths(0);
            const auto fl_new = std::find(fl_order.begin(), fl_order.end(), fl_foreign);
            check(fl_new != fl_order.end() && fl_new + 1 != fl_order.end() && *(fl_new + 1) == L"C:\\fixture\\0-5.txt",
                  "a drop before the only shown icon lands before it in the full order");
            host.SetGroupFilter("group-0", {});
            host.state_.groups[1].collapsed = true; host.RefreshGroupViews();
            check(!host.group_views_[1].expanded, "a rolled-up card shows no icons");
            host.BeginGroupFilter("group-1");
            check(host.group_hover_.Expanded() == "group-1" && host.group_views_[1].temporarily_expanded &&
                  host.state_.groups[1].collapsed, "筛选 opens a rolled-up card for the search without unrolling it");
            host.group_hover_.Reset();
        }

        // 窗口预览: resting on a running app opens live previews; they stay while the pointer is on them.
        Prepare(host);
        {
            render::DockBarItem pv_app;
            pv_app.kind = render::DockBarItemKind::App; pv_app.key = L"c:\\apps\\editor.exe";
            pv_app.launch_path = L"C:\\Apps\\Editor.exe"; pv_app.title = L"Editor"; pv_app.pinned = true;
            render::DockBarItem pv_viewer = pv_app;
            pv_viewer.key = L"c:\\apps\\viewer.exe"; pv_viewer.running = true;
            pv_viewer.windows = {reinterpret_cast<HWND>(0x10), reinterpret_cast<HWND>(0x20), reinterpret_cast<HWND>(0x30)};
            pv_viewer.draw_cx = 640.0f; pv_viewer.draw_cy = 690.0f; pv_viewer.draw_size = 48.0f;
            const auto pv_keep = host.live_data_.dock.items;
            host.live_data_.dock.items = {pv_app, pv_viewer};
            host.state_.settings.dock_enabled = true;
            host.dock_preview_hover_.Reset();
            host.dock_bar_pointer_inside_ = true; host.dock_bar_reveal_ = 1.0f; host.dock_bar_hovered_idx_ = 1;
            host.test_pointer_over_preview_ = false;
            host.TickDockPreview(1000);
            host.TickDockPreview(1300);
            check(!host.dock_preview_.IsOpen(), "no preview the instant the pointer arrives");
            host.TickDockPreview(1460);
            check(host.dock_preview_.IsOpenFor(pv_viewer.key) && host.dock_preview_.Windows().size() == 3 &&
                  host.dock_preview_.Layout().shown == 3, "resting on a running app previews each of its windows");
            const RECT pv_panel = host.dock_preview_.Layout().panel;
            const RECT pv_icon = host.BuildDockStackRequest(1).icon_screen;
            check(pv_panel.bottom <= pv_icon.top && pv_panel.left < (pv_icon.left + pv_icon.right) / 2 &&
                  pv_panel.right > (pv_icon.left + pv_icon.right) / 2, "the previews sit above the icon, centred on it");
            host.dock_bar_hovered_idx_ = -1; host.dock_bar_pointer_inside_ = false; host.test_pointer_over_preview_ = true;
            host.TickDockPreview(1500); host.TickDockPreview(2400);
            check(host.dock_preview_.IsOpen(), "moving onto the previews keeps them open");
            host.test_pointer_over_preview_ = false;
            host.TickDockPreview(2500);
            check(host.dock_preview_.IsOpen(), "leaving gives a moment of grace");
            host.TickDockPreview(2900);
            check(!host.dock_preview_.IsOpen(), "then the previews close");
            host.dock_bar_pointer_inside_ = true; host.dock_bar_hovered_idx_ = 1;
            host.TickDockPreview(3000); host.TickDockPreview(3500);
            check(host.dock_preview_.IsOpen(), "coming back opens them again");
            host.DismissDockPreview();
            host.TickDockPreview(3600); host.TickDockPreview(4600);
            check(!host.dock_preview_.IsOpen(), "a click on the icon closes them until the pointer leaves it");
            host.dock_bar_hovered_idx_ = 0; host.TickDockPreview(4700);
            host.dock_bar_hovered_idx_ = 1; host.TickDockPreview(4800); host.TickDockPreview(5300);
            check(host.dock_preview_.IsOpen(), "after leaving the icon it previews again");
            host.dock_bar_hovered_idx_ = 0; host.TickDockPreview(5400); host.TickDockPreview(5800);
            check(!host.dock_preview_.IsOpen(), "an app without windows has no preview");
            host.dock_bar_hovered_idx_ = 1; host.TickDockPreview(5900); host.TickDockPreview(6400);
            host.interaction_.dragging_items = true; host.TickDockPreview(6450);
            check(!host.dock_preview_.IsOpen(), "a drag closes the previews at once");
            host.interaction_.dragging_items = false;
            host.state_.settings.dock_window_preview = false;
            host.TickDockPreview(7000); host.TickDockPreview(8000);
            check(!host.dock_preview_.IsOpen(), "turned off in the Dock settings, there are no previews");
            host.state_.settings.dock_window_preview = true;
            // Layout: a side Dock puts them beside the icon; too many shrink, then the rest is left out.
            const RECT pv_work{0, 0, 1280, 720}, pv_side_icon{0, 300, 56, 356};
            const auto pv_side = overlays::LayoutDockWindowPreview({SIZE{1600, 900}, SIZE{800, 1200}}, pv_side_icon, pv_work, 1.0f, 1);
            check(pv_side.shown == 2 && pv_side.panel.left >= pv_side_icon.right && pv_side.cards[1].top > pv_side.cards[0].top,
                  "beside a left Dock the previews stack to the right of it");
            const RECT pv_t0 = pv_side.thumbs[0], pv_t1 = pv_side.thumbs[1];
            check((pv_t0.right - pv_t0.left) > (pv_t0.bottom - pv_t0.top) && (pv_t1.bottom - pv_t1.top) > (pv_t1.right - pv_t1.left),
                  "each thumbnail keeps its window's shape");
            const auto pv_many = overlays::LayoutDockWindowPreview(std::vector<SIZE>(12, SIZE{1600, 1000}), RECT{600, 670, 648, 718},
                                                                   pv_work, 1.0f, 0);
            check(pv_many.shown < 12 && pv_many.shown >= 4 && pv_many.panel.left >= 0 && pv_many.panel.right <= 1280,
                  "many windows shrink to fit the screen and the rest is left out");
            host.dock_preview_.Close();
            host.dock_preview_hover_.Reset();
            host.dock_bar_pointer_inside_ = false; host.dock_bar_hovered_idx_ = -1;
            host.live_data_.dock.items = pv_keep;
        }

        // 弹簧文件夹: a file drag resting on a Dock folder opens it; a subfolder held under the drag opens in
        // turn; the drop lands in the folder under the pointer.
        Prepare(host);
        {
            const std::filesystem::path sp_tmp = std::filesystem::current_path();
            const std::filesystem::path sp_dir = sp_tmp / (L"pulse_spring_" + std::to_wstring(GetCurrentProcessId()));
            std::error_code sp_ec;
            std::filesystem::remove_all(sp_dir, sp_ec);
            std::filesystem::create_directories(sp_dir / L"Sub", sp_ec);
            { std::ofstream(sp_dir / L"a.txt") << "x"; }
            const std::wstring sp_src = (sp_tmp / L"pulse_spring_src.txt").wstring();
            render::DockBarItem sp_folder;
            sp_folder.kind = render::DockBarItemKind::Folder; sp_folder.key = L"special:downloads";
            sp_folder.launch_path = sp_dir.wstring(); sp_folder.title = L"下载";
            sp_folder.draw_cx = 700.0f; sp_folder.draw_cy = 690.0f; sp_folder.draw_size = 48.0f;
            render::DockBarItem sp_apps = sp_folder;
            sp_apps.key = L"special:apps"; sp_apps.launch_path.clear();
            const auto sp_keep = host.live_data_.dock.items;
            host.live_data_.dock.items = {sp_folder, sp_apps};
            check(host.DockItemTakesFilesAsFolder(sp_folder) && !host.DockItemTakesFilesAsFolder(sp_apps),
                  "a Dock folder takes dropped files; the 应用程序 stack does not");
            host.ArmSpringLoad(sp_folder.key);
            host.TickSpringLoad(GetTickCount64());
            check(!host.dock_stack_.IsOpen(), "the folder does not spring open at once");
            host.TickSpringLoad(GetTickCount64() + 1000);
            check(host.dock_stack_.IsOpenFor(sp_folder.key) && host.dock_stack_.IsSpringLoaded(), "resting on it opens its stack");
            const int sp_sub = host.dock_stack_.EntryIndexByName(L"Sub");
            const POINT sp_sub_pt = host.dock_stack_.EntryCenterDip(sp_sub);
            const ULONGLONG sp_t0 = 50000;
            const DWORD sp_eff = host.dock_stack_.SimulateDragOver({sp_src}, static_cast<float>(sp_sub_pt.x),
                                                                   static_cast<float>(sp_sub_pt.y), 0, sp_t0);
            check(sp_sub >= 0 && sp_eff == DROPEFFECT_MOVE, "over a subfolder on the same drive the drop is a move");
            host.dock_stack_.SimulateSpringTick(sp_t0 + 300);
            check(_wcsicmp(host.dock_stack_.CurrentFolder().c_str(), sp_dir.wstring().c_str()) == 0, "not yet inside");
            host.dock_stack_.SimulateSpringTick(sp_t0 + 700);
            check(_wcsicmp(host.dock_stack_.CurrentFolder().c_str(), (sp_dir / L"Sub").wstring().c_str()) == 0,
                  "holding the drag on a subfolder opens it inside the stack");
            host.test_dock_activations_.clear();
            const POINT sp_any{40, 80};
            host.dock_stack_.SimulateDrop({sp_src}, static_cast<float>(sp_any.x), static_cast<float>(sp_any.y), 0);
            check(host.test_dock_activations_.size() == 1 &&
                  host.test_dock_activations_[0] == L"spring-move:" + (sp_dir / L"Sub").wstring() + L"|" + sp_src &&
                  !host.dock_stack_.IsOpen(), "the drop lands in the open subfolder and the stack closes");
            host.OpenSpringLoadedStack(0);
            const int sp_file = host.dock_stack_.EntryIndexByName(L"a.txt");
            const POINT sp_file_pt = host.dock_stack_.EntryCenterDip(sp_file);
            host.test_dock_activations_.clear();
            host.dock_stack_.SimulateDrop({sp_src}, static_cast<float>(sp_file_pt.x), static_cast<float>(sp_file_pt.y), MK_CONTROL);
            check(sp_file >= 0 && host.test_dock_activations_.size() == 1 &&
                  host.test_dock_activations_[0] == L"spring-copy:" + sp_dir.wstring() + L"|" + sp_src,
                  "Ctrl copies, and a drop on a file goes into the folder shown");
            using overlays::DockStackDropEffect;
            check(DockStackDropEffect({L"C:\\d\\a.txt"}, L"C:\\d", 0) == DROPEFFECT_NONE &&
                  DockStackDropEffect({L"C:\\d\\sub"}, L"C:\\d\\sub\\deeper", 0) == DROPEFFECT_NONE &&
                  DockStackDropEffect({L"C:\\d\\a.txt"}, L"D:\\e", 0) == DROPEFFECT_COPY &&
                  DockStackDropEffect({L"C:\\d\\a.txt"}, L"D:\\e", MK_SHIFT) == DROPEFFECT_MOVE &&
                  DockStackDropEffect({L"C:\\d\\a.txt"}, L"c:/E/", 0) == DROPEFFECT_MOVE &&
                  DockStackDropEffect({L"C:\\d\\a.txt"}, L"C:\\", MK_CONTROL) == DROPEFFECT_COPY &&
                  DockStackDropEffect({L"C:\\d\\a.txt"}, L"", 0) == DROPEFFECT_NONE,
                  "move on the same drive, copy across drives, never into the same folder or itself");
            host.dock_stack_.Close(false);
            host.live_data_.dock.items = sp_keep;
            std::filesystem::remove_all(sp_dir, sp_ec);
        }

        // 布局快照: save, list, restore as one undo, the daily snapshot, and nothing outside the folder.
        Prepare(host);
        {
            const std::filesystem::path dir = std::filesystem::current_path() /
                (L"group_host_snapshots_" + std::to_wstring(GetCurrentProcessId()));
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            host.snapshot_dir_override_ = dir.wstring();
            host.state_.groups[1].locked = true;
            check(host.SaveLayoutSnapshot(false), "保存当前布局 writes a snapshot");
            const auto files = host.ListLayoutSnapshots();
            const auto name = files.empty() ? std::nullopt : model::ParseLayoutSnapshotFileName(files[0]);
            check(files.size() == 1 && name && !name->automatic, "the snapshot is listed by its time");
            check(host.LayoutSnapshotMenuEntries().size() == 1, "the desktop menu lists it");
            host.state_.groups[1].locked = false;
            host.state_.groups.pop_back();
            host.state_.items[0].pinned_group_id = "group-1";
            host.undo_.Clear();
            const bool restored = !files.empty() && host.RestoreLayoutSnapshot(files[0]);
            check(restored && host.state_.groups.size() == 3 && host.state_.groups[1].locked &&
                  host.state_.items[0].pinned_group_id == "group-0", "restoring puts the groups and file places back");
            check(host.undo_.UndoDepth() == 1 && host.undo_.Undo(host.state_) && host.state_.groups.size() == 2,
                  "the restore undoes in one step");
            check(!host.RestoreLayoutSnapshot(L"..\\layout_state.json"), "only snapshot names are restored");
            host.MaybeTakeDailyLayoutSnapshot();
            check(host.ListLayoutSnapshots().size() == 2, "the first start of the day takes a daily snapshot");
            host.MaybeTakeDailyLayoutSnapshot();
            check(host.ListLayoutSnapshots().size() == 2, "and only one a day");
            std::filesystem::remove_all(dir, ec);
            host.snapshot_dir_override_.clear();
        }

        std::printf("Group host interaction: %d cases, %d failures\n", count, failed);
        return failed ? 1 : 0;
    }
};
}
int main(int argc, char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int result = argc == 2 && std::string_view(argv[1]) == "--ai-visibility"
        ? DesktopGroupHostTestPeer::RunAiVisibility()
        : DesktopGroupHostTestPeer::Run();
    CoUninitialize();
    return result;
}


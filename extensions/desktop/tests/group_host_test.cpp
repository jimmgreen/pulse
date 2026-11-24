// Exercise the real host message dispatch with model persistence redirected in-memory.
#include "../../../src/common/utf8_file.h"

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


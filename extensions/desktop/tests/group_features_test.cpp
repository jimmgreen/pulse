#include "host/model/group_tabs.h"
#include "host/model/drag_damage.h"
#include <cstdio>
using namespace pulse::desktop::model;
int main() {
    int failures=0;
    auto check=[&](bool ok,const char* name){std::printf("[%s] %s\n",ok?"PASS":"FAIL",name);failures+=!ok;};
    DesktopModelState state;
    state.settings.organize_enabled=true;
    for(int i=0;i<3;++i){DesktopGroup g;g.id="g"+std::to_string(i);g.x=24+i*400;g.y=32;g.cols=3;g.visible_rows=3;g.rule.extensions={i==0?L".txt":i==1?L".png":L".pdf"};state.groups.push_back(g);}
    for(int i=0;i<40;++i){DesktopItem it;it.name=L"Item"+std::to_wstring(i);it.path=L"C:\\fixture\\"+it.name;it.extension=i%3==0?L".txt":i%3==1?L".png":L".pdf";state.items.push_back(it);}
    const auto original_views=ComputeGroupViews(state,0);
    const auto icon_metrics=IconCellMetrics(state.settings.icon_size);
    const auto first_icon=GroupItemRect(original_views[0],0);
    check(first_icon==RectI{original_views[0].frame.x+kGroupPaddingX,original_views[0].frame.y+kGroupHeaderHeight+kGroupPaddingTop,icon_metrics.x,icon_metrics.y}&&original_views[0].page_capacity==9,
        "default Icons preserves legacy cell origin size and capacity");
    const auto original_ids=std::vector<std::string>{state.groups[0].id,state.groups[1].id,state.groups[2].id};
    const auto target_x=state.groups[1].x;
    check(MergeGroupTabs(state.groups,"g0","g1"),"merge independent groups");
    check(state.groups[0].id==original_ids[0]&&state.groups[1].id==original_ids[1]&&state.groups[2].id==original_ids[2],"merge preserves vector order and rule priority");
    check(state.groups[0].x==target_x&&state.groups[1].x==target_x&&state.groups[0].tab_active&&!state.groups[1].tab_active,"merge keeps target geometry and selects moving tab");
    auto views=ComputeGroupViews(state,0);
    bool membership=true;for(size_t i=0;i<views.size();++i)membership=membership&&views[i].member_item_indices==original_views[i].member_item_indices;
    check(membership&&views[1].frame.empty()&&!views[1].member_item_indices.empty(),"inactive tab keeps rule membership while hiding frame");
    check(SelectGroupTab(state.groups,"g1")&&state.groups[1].tab_active&&!state.groups[0].tab_active,"select changes only active tab");
    state.groups[0].view_mode=GroupViewMode::Details;
    state.groups[1].view_mode=GroupViewMode::LargeIcons;
    DesktopModelState decoded;
    check(DeserializeModelStateJson(SerializeModelStateJson(state),decoded)&&decoded.groups[0].view_mode==GroupViewMode::Details&&decoded.groups[1].view_mode==GroupViewMode::LargeIcons&&decoded.groups[1].tab_active,"roundtrip independent view modes and selected tab");
    DesktopModelState legacy;
    check(DeserializeModelStateJson(R"({"groups":[{"id":"legacy"}]})",legacy)&&legacy.groups[0].view_mode==GroupViewMode::Icons&&legacy.groups[0].tab_active&&legacy.groups[0].tab_stack_id.empty(),"legacy JSON missing new fields defaults to standalone icons");
    const RectI work{0,0,1800,1000};
    MoveGroupInteractive(state.groups,1,64,64,work,{},DesktopIconSize::Medium);
    check(state.groups[0].x==state.groups[1].x&&state.groups[0].y==state.groups[1].y,"moving active tab moves whole stack");
    ResizeGroupColumns(state.groups,1,4,2,work,{},DesktopIconSize::Medium);
    check(state.groups[0].cols==4&&state.groups[1].cols==4&&state.groups[0].visible_rows==2,"resizing active tab synchronizes shared dimensions");
    auto before_collision=state.groups;
    ResolveGroupCollisions(state.groups,std::nullopt,work,{},DesktopIconSize::Medium);
    check(state.groups[0].x==state.groups[1].x&&state.groups[0].y==state.groups[1].y,"inactive tab not independently displaced by collisions");
    state.groups[1].collapsed=true;NormalizeGroupTabs(state.groups);
    const auto persistent=SerializeModelStateJson(state);
    const auto closed=ComputeGroupViews(state,0);
    const auto expanded=ComputeGroupViews(state,0,nullptr,"g1");
    check(!closed[1].expanded&&expanded[1].expanded&&expanded[1].temporarily_expanded&&expanded[1].frame.h>closed[1].frame.h,"hover temporarily expands selected collapsed group");
    check(SerializeModelStateJson(state)==persistent&&expanded[0].frame.empty(),"hover expansion leaves persistence and inactive tabs unchanged");
    state.groups[1].collapsed=false;NormalizeGroupTabs(state.groups);
    RectI frame{};
    for(auto mode:{GroupViewMode::Icons,GroupViewMode::LargeIcons,GroupViewMode::Tiles,GroupViewMode::Details}) {
        state.groups[1].view_mode=mode;
        auto layout=ComputeGroupViews(state,0);
        if(mode==GroupViewMode::Icons)frame=layout[1].frame;
        bool inside=layout[1].frame==frame&&!layout[1].visible_item_indices.empty();
        for(size_t i=0;i<layout[1].visible_item_indices.size();++i){auto r=GroupItemRect(layout[1],i);inside=inside&&!r.empty()&&r.x>=layout[1].content_rect.x&&r.right()<=layout[1].content_rect.right()&&r.bottom()<=layout[1].content_rect.bottom();}
        check(inside,"view mode preserves outer size and every item rect is inside content");
        check(GroupItemRect(layout[1],layout[1].visible_item_indices.size()).empty(),"out of range item rect is empty");
    }
    const auto fail_json=SerializeModelStateJson(state);
    check(!DetachGroupTab(state.groups,"g1",{0,0,10,10},{})&&SerializeModelStateJson(state)==fail_json,"no-space detach is atomic");
    check(DetachGroupTab(state.groups,"g1",work,{})&&state.groups[1].tab_stack_id.empty()&&state.groups[0].tab_stack_id.empty(),"detach finds independent position and normalizes singleton");
    check(!ComputeGroupFrame(state.groups[0]).intersects(ComputeGroupFrame(state.groups[1])),"detached card does not overlap remaining stack");
    state.groups=before_collision;
    state.groups.erase(state.groups.begin()+1);NormalizeGroupTabs(state.groups);
    check(state.groups[0].tab_active&&state.groups[0].tab_stack_id.empty(),"deleting selected tab activates remaining singleton");
    std::vector<DesktopGroup> many;
    for(int i=0;i<9;++i){DesktopGroup g;g.id=std::to_string(i);many.push_back(g);}
    bool merged=true;for(int i=1;i<8;++i)merged=merged&&MergeGroupTabs(many,std::to_string(i),"0");
    check(merged&&TabMembers(many,"0").size()==8&&!MergeGroupTabs(many,"8","0"),"stack limit eight is enforced without partial merge");
    std::vector<DesktopGroup> four;
    for(int i=0;i<4;++i){DesktopGroup g;g.id="s"+std::to_string(i);g.x=24+400*i;four.push_back(g);}
    check(MergeGroupTabs(four,"s0","s1")&&MergeGroupTabs(four,"s2","s3")&&MergeGroupTabs(four,"s0","s2")&&TabMembers(four,"s3").size()==4,
        "merge joins entire source and target stacks");
    check(four[0].tab_active&&!four[1].tab_active&&!four[2].tab_active&&!four[3].tab_active,
        "whole-stack merge selects dragged group exclusively");
    four.erase(four.begin());NormalizeGroupTabs(four);
    int active_count=0;for(const auto& g:four)active_count+=g.tab_active?1:0;
    check(active_count==1&&TabMembers(four,four[0].id).size()==3,"deleted active member gets deterministic remaining-stack fallback");
    auto cached=ComputeGroupViews(state,0);
    const auto cell_before=GroupItemRect(cached[0],0);
    state.groups[0].x+=16;state.groups[0].y+=24;
    check(UpdateMovedGroupViewOrigins(state.groups,cached)&&GroupItemRect(cached[0],0).x==cell_before.x+16&&GroupItemRect(cached[0],0).y==cell_before.y+24,
        "origin-only drag cache translates shared content geometry");
    return failures?1:0;
}

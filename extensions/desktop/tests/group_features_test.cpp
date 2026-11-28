#include "host/model/group_tabs.h"
#include "host/model/drag_damage.h"
#include "host/group_interaction.h"
#include <cstdio>
#include <unordered_map>
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

    // Files dragged onto a card land between icons (任意拖动): slot, caret, and the card's order after.
    {
        DesktopModelState drop;
        drop.settings.organize_enabled=true;
        DesktopGroup a;a.id="a";a.x=24;a.y=32;a.cols=3;a.visible_rows=2;a.sort_by=GroupSortBy::Name;a.sort_ascending=true;
        DesktopGroup b=a;b.id="b";b.x=424;
        drop.groups={a,b};
        for(int i=0;i<8;++i){DesktopItem it;it.name=L"F"+std::to_wstring(i);it.path=L"C:\\d\\"+it.name;it.extension=L".txt";
            it.pinned_group_id=i<7?"a":"b";drop.items.push_back(it);}
        auto dv=ComputeGroupViews(drop,0);
        const auto& va=dv[0];
        check(va.visible_item_indices.size()==6&&va.page_count==2,"drop fixture pages a seven-file card");
        const auto c0=GroupItemRect(va,0),c1=GroupItemRect(va,1),c4=GroupItemRect(va,4);
        auto s0=ComputeGroupDropSlot(va,c0.x+4,c0.y+10);
        check(s0&&s0->insert_index==0&&s0->caret.x==c0.x-1&&s0->caret.w==3,"left half of the first icon inserts before it");
        auto s1=ComputeGroupDropSlot(va,c0.x+c0.w-4,c0.y+10);
        check(s1&&s1->insert_index==1&&s1->caret.x==c1.x-1,"right half of an icon inserts after it");
        auto s4=ComputeGroupDropSlot(va,c4.x+c4.w/2+2,c4.y+c4.h/2);
        check(s4&&s4->insert_index==5&&s4->caret.y>=c4.y&&s4->caret.bottom()<=c4.bottom(),"second row caret stays in its row");
        auto end_row=ComputeGroupDropSlot(va,va.content_rect.right()-1,c0.y+10);
        check(end_row&&end_row->insert_index==3&&end_row->caret.x==c0.x+3*va.cell_width-1&&end_row->caret.y<c4.y,
              "right edge of a full row lands at its end with the caret on that row");
        check(!ComputeGroupDropSlot(va,va.frame.x+4,va.frame.y+8),"the header has no slot (the card takes the files at its end)");
        auto pages=std::unordered_map<std::string,int>{{"a",1}};
        auto dv2=ComputeGroupViews(drop,0,&pages);
        const auto p0=GroupItemRect(dv2[0],0);
        auto sp=ComputeGroupDropSlot(dv2[0],p0.x+p0.w-2,p0.y+4);
        check(sp&&sp->insert_index==7,"a slot on page two counts the files on page one");
        auto below=ComputeGroupDropSlot(dv2[0],dv2[0].content_rect.right()-2,dv2[0].content_rect.bottom()-2);
        check(below&&below->insert_index==7&&below->caret.x==p0.right()-1,"below the last icon lands right after it");
        DesktopModelState empty_state=drop;for(auto& it:empty_state.items)it.pinned_group_id="a";
        auto ev=ComputeGroupViews(empty_state,0);
        auto se=ComputeGroupDropSlot(ev[1],ev[1].content_rect.x+8,ev[1].content_rect.y+8);
        check(se&&se->insert_index==0&&se->caret.empty(),"an empty card takes files without a caret");
        drop.groups[0].view_mode=GroupViewMode::Details;
        auto dd=ComputeGroupViews(drop,0);
        const auto r1=GroupItemRect(dd[0],1);
        auto sd=ComputeGroupDropSlot(dd[0],r1.x+20,r1.y+r1.h-3);
        check(sd&&sd->insert_index==2&&sd->caret.h==3&&sd->caret.y==r1.bottom()-1,"details view inserts between rows");

        const std::vector<std::wstring> order{L"A",L"B",L"C",L"D",L"E"};
        check(ComposeManualOrder(order,std::vector<std::wstring>{L"X"},2)==std::vector<std::wstring>{L"A",L"B",L"X",L"C",L"D",L"E"},
              "a file from another card joins at the slot");
        check(ComposeManualOrder(order,std::vector<std::wstring>{L"a"},4)==std::vector<std::wstring>{L"B",L"C",L"D",L"A",L"E"},
              "moving a member later accounts for its old place (paths compare case-insensitively)");
        check(ComposeManualOrder(order,std::vector<std::wstring>{L"E",L"B"},0)==std::vector<std::wstring>{L"B",L"E",L"A",L"C",L"D"},
              "several members keep the card's relative order");
        check(ComposeManualOrder(order,std::vector<std::wstring>{L"C",L"Y"},99)==std::vector<std::wstring>{L"A",L"B",L"D",L"E",L"C",L"Y"},
              "an index past the end appends");
        std::vector<DesktopGroup> gs=drop.groups;gs[1].sort_by=GroupSortBy::Manual;gs[1].manual_order={L"C:\\d\\F7",L"C:\\d\\F1"};
        check(ApplyGroupManualOrder(gs,"a",{L"C:\\d\\F1",L"C:\\d\\F0"})&&gs[0].sort_by==GroupSortBy::Manual&&
              gs[0].manual_order.size()==2&&gs[1].manual_order==std::vector<std::wstring>{L"C:\\d\\F7"},
              "the card switches to manual order and the path leaves the other card's order");
        check(!ApplyGroupManualOrder(gs,"a",{L"C:\\d\\F0"}),"an already manual card reports no switch");
        drop.groups[0].view_mode=GroupViewMode::Icons;
        drop.groups[0].sort_by=GroupSortBy::Manual;
        drop.groups[0].manual_order=ComposeManualOrder({},std::vector<std::wstring>{L"C:\\d\\F5",L"C:\\d\\F2"},0);
        auto mv=ComputeGroupViews(drop,0);
        check(drop.items[mv[0].visible_item_indices[0]].name==L"F5"&&drop.items[mv[0].visible_item_indices[1]].name==L"F2",
              "the composed order is what the card shows");

        std::vector<DesktopGroup> named(2);named[0].title=L"新建分组";named[1].title=L"新建分组 2";
        check(NextNewGroupTitle({},L"新建分组")==L"新建分组"&&NextNewGroupTitle(named,L"新建分组")==L"新建分组 3",
              "new groups get the first free title");
    }
    {   // 布局快照: names, order, pruning, the daily rule, and putting one back.
        LayoutSnapshotName n;n.year=2026;n.month=11;n.day=26;n.hour=14;n.minute=30;n.second=5;
        const auto f=MakeLayoutSnapshotFileName(n);
        check(f==L"snap-20261126-143005.json","a hand-made snapshot is named by its time");
        const auto back=ParseLayoutSnapshotFileName(f);
        check(back&&!back->automatic&&back->day==26&&back->second==5,"snapshot names read back");
        n.automatic=true;
        check(MakeLayoutSnapshotFileName(n)==L"auto-20261126-143005.json","a daily snapshot has its own prefix");
        check(!ParseLayoutSnapshotFileName(L"layout.json")&&!ParseLayoutSnapshotFileName(L"snap-20261326-143005.json")&&
              !ParseLayoutSnapshotFileName(L"..\\snap-20261126-143005.json"),"other files, bad dates and paths are not snapshots");
        const std::vector<std::wstring> files{L"auto-20261124-090000.json",L"snap-20261125-120000.json",L"notes.txt",
                                              L"auto-20261126-090000.json",L"snap-20261120-080000.json"};
        const auto sorted=SortLayoutSnapshots(files);
        check(sorted.size()==4&&sorted[0]==L"auto-20261126-090000.json"&&sorted[3]==L"snap-20261120-080000.json","snapshots list newest first");
        check(LayoutSnapshotsToPrune(files,1,1)==std::vector<std::wstring>{L"auto-20261124-090000.json",L"snap-20261120-080000.json"},
              "pruning keeps the newest of each kind");
        LayoutSnapshotName today;today.year=2026;today.month=11;today.day=26;
        check(!DailyLayoutSnapshotDue(files,today),"no second daily snapshot on the same day");
        today.day=27;
        check(DailyLayoutSnapshotDue(files,today),"a new day takes a daily snapshot");
        DesktopModelState snap;DesktopGroup a;a.id="a";a.x=100;a.locked=true;snap.groups.push_back(a);
        DesktopItem i1;i1.path=L"C:\\d\\One.txt";i1.pinned_group_id="a";snap.items.push_back(i1);
        DesktopModelState cur;DesktopGroup b;b.id="b";cur.groups.push_back(b);
        DesktopItem c1;c1.path=L"c:\\D\\one.TXT";c1.force_loose=true;cur.items.push_back(c1);
        DesktopItem c2;c2.path=L"C:\\d\\New.txt";c2.pinned_group_id="b";cur.items.push_back(c2);
        DesktopItem c3;c3.path=L"C:\\d\\App.lnk";c3.pinned_group_id=kAppsTileGroupId;cur.items.push_back(c3);
        check(ApplyLayoutSnapshot(snap,cur)==1&&cur.groups.size()==1&&cur.groups[0].id=="a"&&cur.groups[0].locked,
              "restoring brings the snapshot's groups back");
        check(cur.items[0].pinned_group_id=="a"&&!cur.items[0].force_loose,"a known file goes back to its group (paths compare case-insensitively)");
        check(cur.items[1].pinned_group_id.empty()&&cur.items[2].pinned_group_id==kAppsTileGroupId,
              "a newer file leaves a group that is gone; the apps tile keeps its shortcut");
        DesktopModelState rt;
        check(DeserializeModelStateJson(SerializeModelStateJson(cur),rt)&&rt.groups[0].locked,"锁定分组 survives saving");
        DesktopModelState old_file;
        check(DeserializeModelStateJson(SerializeModelStateJson(DesktopModelState{}),old_file)&&(old_file.groups.empty()||!old_file.groups[0].locked),
              "older layouts load unlocked");
    }
    {   // 筛选 (in-group filter) and hover-expand of rolled-up groups
        DesktopModelState fs;fs.settings.organize_enabled=true;
        DesktopGroup fg;fg.id="docs";fg.x=24;fg.y=32;fg.cols=2;fg.visible_rows=2;fg.sort_by=GroupSortBy::Name;fs.groups.push_back(fg);
        const wchar_t* names[]={L"Budget 2026.xlsx",L"budget-old.xlsx",L"Contract.pdf",L"Invoice March.pdf",L"Notes.txt",L"Photo.png"};
        for(const wchar_t* n:names){DesktopItem it;it.name=n;it.path=std::wstring(L"C:\\d\\")+n;it.pinned_group_id="docs";fs.items.push_back(it);}
        const auto plain=ComputeGroupViews(fs,0);
        check(plain[0].total_item_count==6&&plain[0].unfiltered_item_count==6&&plain[0].shown_item_indices==plain[0].member_item_indices&&plain[0].page_count==2,
              "without a filter every member is shown and paged");
        check(NameMatchesGroupFilter(L"Budget 2026.xlsx",L"  bUdGeT ")&&!NameMatchesGroupFilter(L"Notes.txt",L"budget")&&
              NameMatchesGroupFilter(L"Notes.txt",L"   ")&&NameMatchesGroupFilter(L"发票-三月.pdf",L"三月")&&!NameMatchesGroupFilter(L"ab",L"abc"),
              "filter matches a case-insensitive substring and ignores surrounding spaces");
        fs.groups[0].filter_text=L"budget";
        const auto filtered=ComputeGroupViews(fs,0);
        bool only_budget=filtered[0].total_item_count==2;
        for(size_t idx:filtered[0].visible_item_indices)only_budget=only_budget&&NameMatchesGroupFilter(fs.items[idx].name,L"budget");
        check(only_budget&&filtered[0].unfiltered_item_count==6&&filtered[0].page_count==1&&filtered[0].visible_item_indices.size()==2,
              "a filter shows only matching names on a single page");
        check(filtered[0].member_item_indices.size()==6&&filtered[0].frame==plain[0].frame,
              "hidden files stay members (never fall back onto the desktop) and the card keeps its size");
        std::unordered_map<std::string,int> pages{{"docs",1}};
        check(ComputeGroupViews(fs,0,&pages)[0].page_index==0,"a stale page index is clamped into the filtered pages");
        fs.groups[0].filter_text=L"zzz";
        const auto none=ComputeGroupViews(fs,0);
        check(none[0].total_item_count==0&&none[0].visible_item_indices.empty()&&none[0].member_item_indices.size()==6,"no match leaves the card empty");
        DesktopModelState rt;fs.groups[0].filter_text=L"budget";
        check(DeserializeModelStateJson(SerializeModelStateJson(fs),rt)&&rt.groups[0].filter_text.empty(),"the filter keyword is never saved");
        const std::vector<size_t> members{0,1,2,3,4,5},shown{1,3,5};
        check(FilteredToMemberIndex(members,shown,0)==1&&FilteredToMemberIndex(members,shown,2)==5&&FilteredToMemberIndex(members,shown,3)==6&&
              FilteredToMemberIndex(members,{},0)==6,"a drop position among filtered icons maps before the same icon in the full order");
        // Hover-expand: 250 ms to open, stays while the pointer (or an open edit) is on it, closes 400 ms after leaving.
        pulse::desktop::interaction::GroupHoverSession hs;
        hs.Update("docs",true,false,1000);
        check(hs.Expanded().empty()&&hs.Pending(),"a rolled-up group does not open the instant the pointer arrives");
        hs.Update("docs",true,false,1200);
        check(hs.Expanded().empty(),"still closed before the 250 ms enter delay");
        check(hs.Update("docs",true,false,1260)&&hs.Expanded()=="docs","it opens after hovering 250 ms");
        hs.Update("",false,true,1300);hs.Update("",false,true,2000);
        check(hs.Expanded()=="docs","an open edit or overlay keeps it open after the pointer leaves");
        hs.Update("",false,false,2100);
        check(hs.Expanded()=="docs","leaving starts the 400 ms close delay");
        hs.Update("docs",true,false,2300);hs.Update("",false,false,2400);hs.Update("",false,false,2700);
        check(hs.Expanded()=="docs","coming back within the delay keeps it open and restarts the delay");
        check(hs.Update("",false,false,2800)&&hs.Expanded().empty(),"it rolls up again 400 ms after the pointer leaves");
        hs.Update("docs",false,false,3000);hs.Update("docs",false,false,4000);
        check(hs.Expanded().empty()&&!hs.Pending(),"an expanded group never hover-opens");
        fs.groups[0].collapsed=true;fs.groups[0].filter_text.clear();
        const auto rolled=ComputeGroupViews(fs,0);
        const auto hovered=ComputeGroupViews(fs,0,nullptr,"docs");
        check(!rolled[0].expanded&&rolled[0].visible_item_indices.empty()&&hovered[0].expanded&&hovered[0].temporarily_expanded&&
              !hovered[0].visible_item_indices.empty()&&hovered[0].frame.h>rolled[0].frame.h&&fs.groups[0].collapsed,
              "hover shows a rolled-up card's contents without changing its saved state");
        hs.Reset();hs.Force("docs");
        check(hs.Expanded()=="docs"&&ComputeGroupViews(fs,0,nullptr,hs.Expanded())[0].temporarily_expanded,"筛选 opens a rolled-up card at once");
    }
    return failures?1:0;
}

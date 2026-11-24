#include "group_tabs.h"
#include <algorithm>
#include <unordered_set>

namespace pulse::desktop::model {
namespace {
void Geometry(DesktopGroup& to, const DesktopGroup& from) {
    to.x=from.x; to.y=from.y; to.cols=from.cols; to.visible_rows=from.visible_rows;
    to.collapsed=from.collapsed; to.rest_origin=from.rest_origin;
}
}
std::vector<size_t> TabMembers(const std::vector<DesktopGroup>& groups, std::string_view id) {
    const auto it=std::find_if(groups.begin(),groups.end(),[&](const auto& g){return g.id==id;});
    if(it==groups.end())return {};
    std::vector<size_t> result;
    for(size_t i=0;i<groups.size();++i)
        if(groups[i].id==id||(!it->tab_stack_id.empty()&&groups[i].tab_stack_id==it->tab_stack_id))result.push_back(i);
    std::stable_sort(result.begin(),result.end(),[&](size_t a,size_t b){return groups[a].tab_order<groups[b].tab_order;});
    return result;
}
void NormalizeGroupTabs(std::vector<DesktopGroup>& groups) {
    std::unordered_set<std::string> done;
    for(size_t i=0;i<groups.size();++i) {
        if(groups[i].tab_stack_id.empty()){groups[i].tab_active=true;groups[i].tab_order=0;continue;}
        if(!done.insert(groups[i].tab_stack_id).second)continue;
        auto members=TabMembers(groups,groups[i].id);
        if(members.size()==1){groups[i].tab_stack_id.clear();groups[i].tab_active=true;groups[i].tab_order=0;continue;}
        if(members.size()>kMaxGroupTabs) {
            for(size_t j=kMaxGroupTabs;j<members.size();++j){auto& g=groups[members[j]];g.tab_stack_id.clear();g.tab_active=true;g.tab_order=0;}
            members.resize(kMaxGroupTabs);
        }
        size_t selected=members.front();
        for(size_t idx:members)if(groups[idx].tab_active){selected=idx;break;}
        const auto geometry=groups[selected];
        for(size_t j=0;j<members.size();++j){auto& g=groups[members[j]];Geometry(g,geometry);g.tab_order=static_cast<int>(j);g.tab_active=members[j]==selected;}
    }
}
bool SelectGroupTab(std::vector<DesktopGroup>& groups,std::string_view id) {
    const auto members=TabMembers(groups,id);
    if(members.empty())return false;
    NormalizeGroupTabs(groups);
    DesktopGroup geometry=groups[members.front()];
    for(size_t idx:members)if(groups[idx].tab_active)geometry=groups[idx];
    for(size_t idx:members){Geometry(groups[idx],geometry);groups[idx].tab_active=groups[idx].id==id;}
    return true;
}
bool MergeGroupTabs(std::vector<DesktopGroup>& groups,std::string_view moving_id,std::string_view target_id) {
    if(moving_id==target_id)return false;
    auto trial=groups;NormalizeGroupTabs(trial);
    auto moving=TabMembers(trial,moving_id),target=TabMembers(trial,target_id);
    if(moving.empty()||target.empty()||moving==target||moving.size()+target.size()>kMaxGroupTabs)return false;
    const auto target_it=std::find_if(trial.begin(),trial.end(),[&](const auto& g){return g.id==target_id;});
    const DesktopGroup geometry=*target_it;
    std::string stack=geometry.tab_stack_id;
    if(stack.empty()) {
        stack="tabs:"+geometry.id;
        while(std::any_of(trial.begin(),trial.end(),[&](const auto& g){return g.tab_stack_id==stack;}))stack+="_";
    }
    target.insert(target.end(),moving.begin(),moving.end());
    for(size_t j=0;j<target.size();++j){auto& g=trial[target[j]];Geometry(g,geometry);g.tab_stack_id=stack;g.tab_order=static_cast<int>(j);g.tab_active=g.id==moving_id;}
    groups=std::move(trial);return true;
}
bool DetachGroupTab(std::vector<DesktopGroup>& groups,std::string_view id,const RectI& work,
                   const RectI& board,DesktopIconSize icon_size,std::span<const RectI> extra) {
    const auto members=TabMembers(groups,id);
    if(members.size()<2||work.empty())return false;
    auto trial=groups;NormalizeGroupTabs(trial);
    const size_t idx=static_cast<size_t>(std::find_if(trial.begin(),trial.end(),[&](const auto& g){return g.id==id;})-trial.begin());
    auto& detached=trial[idx];detached.tab_stack_id.clear();detached.tab_active=true;detached.tab_order=0;detached.rest_origin.reset();
    NormalizeGroupTabs(trial);
    const auto shape=ComputeGroupFrame(detached,icon_size);
    std::vector<RectI> obstacles(extra.begin(),extra.end());if(!board.empty())obstacles.push_back(board);
    for(size_t i=0;i<trial.size();++i)if(i!=idx&&trial[i].tab_active)obstacles.push_back(ComputeGroupFrame(trial[i],icon_size));
    size_t attempts=0;
    for(int y=work.y+kSnapGridPx;y+shape.h<=work.bottom();y+=kSnapGridPx)
        for(int x=work.x+kSnapGridPx;x+shape.w<=work.right();x+=kSnapGridPx) {
            if(++attempts>65536)return false;
            const RectI candidate{x,y,shape.w,shape.h};
            if(std::any_of(obstacles.begin(),obstacles.end(),[&](const auto& r){return candidate.intersects(r);}))continue;
            detached.x=x;detached.y=y;groups=std::move(trial);return true;
        }
    return false;
}
RectI GroupItemRect(const GroupComputedView& view,size_t index) noexcept {
    if(!view.tab_active||!view.expanded||view.frame.empty()||index>=view.visible_item_indices.size()||view.item_columns<1)return {};
    const int col=static_cast<int>(index%static_cast<size_t>(view.item_columns));
    const int row=static_cast<int>(index/static_cast<size_t>(view.item_columns));
    const RectI rect{view.content_rect.x+col*view.cell_width,view.content_rect.y+view.details_header_height+row*view.cell_height,
        view.cell_width,view.cell_height};
    if(rect.right()>view.content_rect.right()||rect.bottom()>view.content_rect.bottom())return {};
    return rect;
}
}

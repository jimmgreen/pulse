#include "../app/pane_layout.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>

namespace {
int failures = 0;
void Check(bool ok, const char* text) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << '\n';
    failures += !ok;
}
bool Same(D2D1_RECT_F a, D2D1_RECT_F b) {
    return std::abs(a.left-b.left)<0.001f && std::abs(a.right-b.right)<0.001f &&
        std::abs(a.top-b.top)<0.001f && std::abs(a.bottom-b.bottom)<0.001f;
}
std::set<int> Intersections(const pulse::ui::ViewLayout& layout, D2D1_RECT_F box,
                            const std::vector<int>& sources) {
    std::set<int> result;
    const auto [first, last] = layout.VisibleRange();
    for (int row=first; row>=0 && row<=last; ++row) {
        const auto item=layout.ItemRect(row);
        if (item.right<=box.left || item.left>=box.right ||
            item.bottom<=box.top || item.top>=box.bottom) continue;
        result.insert(sources[static_cast<size_t>(row)]);
    }
    return result;
}
}
int main() {
    using namespace pulse::app;
    using namespace pulse::ui;
    // Opaque identities: geometry never dereferences Pane or starts providers.
    int identities[3]{};
    auto* a=reinterpret_cast<Pane*>(&identities[0]);
    auto* b=reinterpret_cast<Pane*>(&identities[1]);
    auto* c=reinterpret_cast<Pane*>(&identities[2]);
    SplitContainer root;
    root.is_leaf=false; root.orientation=SplitOrientation::Vertical;
    root.first=std::make_unique<SplitContainer>(); root.first->pane=a;
    root.second=std::make_unique<SplitContainer>(); root.second->pane=b;
    for (float scale : {1.0f,1.25f,1.5f,2.0f}) {
        const float desired=4*112*scale-1;
        const D2D1_RECT_F bounds{285,200,285+desired*2+8*scale,1100};
        root.ratio=0.5f;
        std::vector<std::pair<Pane*,D2D1_RECT_F>> painted;
        LayoutWindowPanes(root,bounds,scale,painted);
        for (Pane* pane : {a,b}) {
            const auto drawn=painted[pane==a?0:1].second;
            const auto input=FocusedWindowPaneRect(&root,pane,bounds,scale);
            Check(Same(drawn,input),"focused pane geometry equals rendered split geometry");
            for (size_t count : {size_t{4},size_t{5}}) {
                const ViewLayout visible(ViewMode::MediumIcons,drawn,count,0,0,scale);
                const ViewLayout hit(ViewMode::MediumIcons,input,count,0,0,scale);
                Check(visible.Metrics().columns==3,"fixture renders exactly three columns");
                const auto first=visible.ItemRect(0), second=visible.ItemRect(1);
                const D2D1_RECT_F box{first.left+30,first.top+15,second.right-30,
                    first.top+visible.Metrics().cell_height*2-15};
                const std::vector<int> sources=count==4?std::vector<int>{0,1,2,3}:std::vector<int>{0,1,2,3,4};
                const std::set<int> expected=count==4?std::set<int>{0,1,3}:std::set<int>{0,1,3,4};
                Check(Intersections(visible,box,sources)==expected,"rendered rectangle covers intended columns on both rows");
                Check(Intersections(hit,box,sources)==expected,"input rectangles select Word row and exclude third column");
                const std::vector<int> filtered=count==4?std::vector<int>{6,2,9,1}:std::vector<int>{6,2,9,1,7};
                const std::set<int> mapped=count==4?std::set<int>{6,2,1}:std::set<int>{6,2,1,7};
                Check(Intersections(hit,box,filtered)==mapped,"filtered source identities preserve geometric selection");
            }
        }
        root.second->is_leaf=false; root.second->pane=nullptr;
        root.second->orientation=SplitOrientation::Horizontal;
        root.second->first=std::make_unique<SplitContainer>(); root.second->first->pane=b;
        root.second->second=std::make_unique<SplitContainer>(); root.second->second->pane=c;
        painted.clear(); LayoutWindowPanes(root,bounds,scale,painted);
        for (const auto& [pane,rect] : painted)
            Check(Same(rect,FocusedWindowPaneRect(&root,pane,bounds,scale)),"nested horizontal split has one input and paint geometry");
        root.second->first.reset(); root.second->second.reset();
        root.second->is_leaf=true; root.second->pane=b;
    }
    // Exact GUI reproduction: 1600x960 at 125%, persisted splitRatios=4300.
    root.ratio=0.43f;
    const D2D1_RECT_F actual_bounds{285,200,1594,930};
    std::vector<std::pair<Pane*,D2D1_RECT_F>> actual;
    LayoutWindowPanes(root,actual_bounds,1.25f,actual);
    auto drawn=actual.front().second;
    auto input=FocusedWindowPaneRect(&root,a,actual_bounds,1.25f);
    // Both paths remove the same 40 DIP pane header before constructing grids.
    drawn.top+=50; input.top+=50;
    const D2D1_RECT_F marquee{350,270,610,640};
    const std::vector<int> identity{0,1,2,3,4};
    const ViewLayout visible(ViewMode::MediumIcons,drawn,5,0,0,1.25f);
    const ViewLayout hit(ViewMode::MediumIcons,input,5,0,0,1.25f);
    Check(Intersections(visible,marquee,identity)==std::set<int>{0,1,3,4},"4300 GUI rectangle expected rendered identities");
    Check(Intersections(hit,marquee,identity)==std::set<int>{0,1,3,4},"4300 GUI rectangle input matches rendered identities");
    for (ViewMode mode : {ViewMode::ExtraLargeIcons,ViewMode::List,ViewMode::Details}) {
        const ViewLayout v(mode,drawn,5,0,0,1.25f), h(mode,input,5,0,0,1.25f);
        bool same=true;
        for (int i=0;i<5;++i) same &= Same(v.ItemRect(i),h.ItemRect(i));
        Check(same,"4300 extra-large/list/details use same input and paint cells");
    }
    SplitContainer single; single.pane=a;
    const D2D1_RECT_F bounds{20,30,1100,900};
    Check(Same(bounds,FocusedWindowPaneRect(&single,a,bounds,1.25f)),"single pane retains unchanged bounds");
    Check(Same(bounds,FocusedWindowPaneRect(nullptr,a,bounds,1.25f)),"missing tree keeps content fallback");
    // Bug 4: a marquee whose start scrolled off screen still covers every item
    // between that start and the pointer (RangeForSpan vs. a full scan).
    for (ViewMode mode : {ViewMode::Details,ViewMode::List,ViewMode::MediumIcons}) {
        const D2D1_RECT_F list{100,200,900,800};
        const size_t count=5000;
        const bool columns=mode==ViewMode::List;
        const ViewLayout probe(mode,list,count,0,0,1.25f);
        const float scroll=columns ? std::min(3000.0f,probe.MaxScrollX()) : std::min(3000.0f,probe.MaxScrollY());
        const ViewLayout layout(mode,list,count,columns?scroll:0,columns?0:scroll,1.25f);
        // Started at the first item before scrolling, now dragged to mid-viewport.
        const D2D1_RECT_F box=columns ? D2D1_RECT_F{list.left+10-scroll,list.top+5,list.left+400,list.bottom-5}
                                      : D2D1_RECT_F{list.left+10,list.top+5-scroll,list.left+500,list.top+300};
        std::set<int> brute, ranged;
        for (int i=0;i<static_cast<int>(count);++i) {
            const auto item=layout.ItemRect(i);
            if (item.right<=box.left || item.left>=box.right || item.bottom<=box.top || item.top>=box.bottom) continue;
            brute.insert(i);
        }
        const auto [lo,hi]=columns ? layout.RangeForSpan(box.left,box.right) : layout.RangeForSpan(box.top,box.bottom);
        for (int i=lo;i>=0 && i<=hi;++i) {
            const auto item=layout.ItemRect(i);
            if (item.right<=box.left || item.left>=box.right || item.bottom<=box.top || item.top>=box.bottom) continue;
            ranged.insert(i);
        }
        Check(scroll>0 && brute.count(0)==1 && brute.size()>10,"scrolled marquee fixture reaches the off-screen first item");
        Check(ranged==brute,"RangeForSpan covers every item of an off-screen marquee");
        const auto onscreen=layout.VisibleRange();
        Check(onscreen.first>0,"fixture start lies outside VisibleRange (old behaviour lost it)");
    }
    std::cout << "[INFO] production split/layout geometry only; physical input is verified separately\n";
    return failures?1:0;
}

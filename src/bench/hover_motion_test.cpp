#include "../ui/ui_motion.h"
#include "../ui/hover_animation.h"
#include <cstdio>
#include <cmath>

int main() {
    using namespace pulse::ui::motion;
    SpringRectMotion motion;
    motion.SetAnimationsEnabled(true);
    const SpringSpec critical{1600.0f,80.0f};
    const auto rect = [](float y) { return D2D1::RectF(0,y,300,y+30); };
    motion.Update(1,0,rect(0),1,1000,critical);
    motion.Update(1,1,rect(30),2,1016,critical);
    auto previous = motion.Update(1,1,rect(30),3,1032,critical);
    bool ok = true;
    uint64_t frame = 4;
    for (uint64_t now=1048; now<1240; now+=16) {
        const int key = now<1100 ? 2 : 3;
        const auto current = motion.Update(1,key,rect(static_cast<float>(key*30)),frame++,now,critical);
        ok &= current.top >= previous.top && current.top <= key*30.0f && std::isfinite(current.top);
        if (now==1048 || now==1112) ok &= current.top > previous.top;
        previous = current;
    }
    const auto held = motion.Update(1,4,rect(120),frame++,1224,critical);
    ok &= std::fabs(held.top-previous.top)<0.001f;
    const auto settled = motion.Update(1,4,rect(120),frame++,5000,critical);
    ok &= settled.top==120 && !motion.Active(5000);
    const auto hidden = motion.Update(2,0,rect(0),frame+2,5016,critical);
    ok &= hidden.top==0 && !motion.Active(5016);
    ListHoverMotion hover;
    hover.SetAnimationsEnabled(true);
    hover.Update(1,0,rect(0),1,1000);
    hover.Update(1,1,rect(30),2,1016);
    auto first = hover.Update(1,1,rect(30),3,1032);
    auto next = hover.Update(1,2,rect(60),4,1048);
    auto span = hover.Span();
    ok &= next.top>first.top && next.top<=60 && span.top<=first.top && span.bottom>=90;
    auto later = hover.Update(1,2,rect(60),5,1100);
    ok &= later.top>=next.top && later.top<=60;
    hover.Hide();
    ok &= !hover.Active(1100) && hover.Span().bottom==0;
    ok &= hover.Update(1,3,rect(90),6,1116).top==90;
    hover.SetAnimationsEnabled(false);
    ok &= hover.Update(1,4,rect(120),7,1132).top==120 && !hover.Active(1132);
    std::printf("[%s] fast-to-slow retarget, position continuity, long-gap settle, context reset\n",ok?"PASS":"FAIL");
    pulse::ui::HoverAnimation animation;
    animation.Bind(L"fixture.gif", 1, 100, true);
    animation.Commit(3, 100, 1000);
    bool frames = animation.Request(1099) == 0 && animation.Request(1100) == 1 && animation.Current() == 0;
    frames &= animation.Request(10000) == 1 && animation.Waiting();
    animation.Commit(3, 100, 10000);
    frames &= animation.Current() == 1 && animation.Request(10099) == 1;
    frames &= animation.Request(10100) == 2;
    animation.Commit(3, 100, 10100);
    frames &= animation.Request(10200) == 0;
    animation.Stop();
    frames &= animation.NextDelay(10200) == 0;
    animation.Bind(L"fixture.gif", 2, 100, true);
    frames &= animation.Current() == 0;
    animation.Commit(2, 0, 11000);
    frames &= animation.NextDelay(11000) == 0;
    animation.Bind(L"fixture.gif", 2, 100, false);
    animation.Commit(2, 100, 12000);
    frames &= animation.NextDelay(12000) == 0 && animation.Request(13000) == 0;
    std::printf("[%s] hover animation timing, pending hold, wrap, failure, identity, paged and hidden states\n",frames?"PASS":"FAIL");
    ok &= frames;
    return ok?0:1;
}

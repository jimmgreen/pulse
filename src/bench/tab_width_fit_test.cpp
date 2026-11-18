#include "../ui/tab_width_fit.h"
#include "../ui/ui_motion.h"
#include <cstdio>
#include <numeric>
#include <cmath>

static bool FitSingleRow(std::vector<float>& widths, const std::vector<size_t>& ids,
                         float budget, float scale, size_t active) {
    using namespace pulse::ui;
    if (FitTabWidths(widths, ids, budget, 64 * scale)) return true;
#ifdef PULSE_TAB_NARROW_BASELINE
    const float active_width = (std::min)(widths[active], 140 * scale);
    for (auto i : ids) widths[i] = i == active ? active_width : 40 * scale;
    return std::accumulate(widths.begin(), widths.end(), 0.f) <= budget;
#else
    return FitCompactTabWidths(widths, ids, budget, 40 * scale, active, 140 * scale);
#endif
}

int main() {
    using namespace pulse::ui;
    int fails = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok?"PASS":"FAIL", label); fails += !ok; };
    std::vector<size_t> ids{0,1,2,3,4,5,6,7};
    std::vector<float> widths(8,180);
#ifdef PULSE_TAB_BASELINE
    const bool fit = 780.f/8 >= 104.f;
#else
    const bool fit = FitTabWidths(widths, ids, 780, 64);
#endif
    check(fit, "eight tabs retain labels below old 104 DIP cliff");
    check(std::accumulate(widths.begin(),widths.end(),0.f)<=780.01f && widths[0]>97.f, "available width is consumed without overflow");
    widths={40,80,180,240,140,200,150,130};
    check(FitTabWidths(widths,ids,780,64) && widths[0]==40 && widths[1]==80 && widths[3]>100, "short and pinned widths release budget to longer titles");
    check(!FitTabWidths(widths,ids,400,64), "genuinely narrow strip retains icon fallback");
    std::vector<size_t> dozen;
    for (size_t i = 0; i < 12; ++i) dozen.push_back(i);
    for (float scale : {1.f, 1.5f, 2.f}) {
        bool full = true;
        bool bounded = true;
        for (int dip = 480; dip <= 800; ++dip) {
            std::vector<float> crowded(12, 160 * scale);
            const float budget = static_cast<float>(dip) * scale;
            const bool fits = FitSingleRow(crowded, dozen, budget, scale, 5);
            const float used = std::accumulate(crowded.begin(), crowded.end(), 0.f);
            full &= fits && std::abs(used - budget) < .02f;
            for (float w : crowded) bounded &= w >= 40 * scale - .01f && w <= 160 * scale;
            if (dip == 767)
                printf("[BOUNDARY] scale=%.1f budget=%.1f used=%.3f spare=%.3f\n", scale, budget, used, budget-used);
        }
        check(full, "twelve tabs consume budget throughout the 40-to-64 DIP transition");
        check(bounded, "transition keeps every visible tab within its usable width");
    }
    widths.assign(12, 160);
    widths[0] = widths[1] = 36;
    check(FitTabWidths(widths, dozen, 712, 64) && widths[0] == 36 && widths[1] == 36,
          "fixed pinned slots do not trigger premature label collapse");
    widths.assign(12, 160);
    widths[0] = 36;
    check(FitSingleRow(widths, dozen, 620, 1, 5) && widths[0] == 36 && widths[5] == 140 &&
          std::abs(std::accumulate(widths.begin(), widths.end(), 0.f) - 620) < .02f,
          "compact allocation preserves pinned width and active title priority");
    widths.assign(12, 160);
    const auto original = widths;
    check(!FitSingleRow(widths, dozen, 479, 1, 5), "overflow remains necessary below the icon minimum");
#ifndef PULSE_TAB_NARROW_BASELINE
    check(widths == original, "failed allocation leaves overflow inputs unchanged");
#endif
    widths = {160, 999, 160};
    check(FitSingleRow(widths, {0,2}, 110, 1, 2) && widths[1] == 999 &&
          std::abs(widths[0] + widths[2] - 110) < .02f,
          "hidden group members do not consume the visible budget");
#ifdef PULSE_TAB_BASELINE
    check(false, "explicit single-row rectangles retain tab motion");
#else
    check(TabStripMotionSupported(true,0) && !TabStripMotionSupported(true,1), "explicit single-row rectangles retain tab motion");
#endif
    std::vector<motion::SpringRectMotion> channels(33);
    for(auto& c:channels) { c.Update(1,1,D2D1::RectF(0,0,100,1),1,1000,motion::spring::Smooth); c.Update(1,2,D2D1::RectF(400,0,500,1),2,1016,motion::spring::Smooth); }
    bool active=false; for(auto& c:channels) active|=c.Active(1020);
    check(active,"concurrent slot and plate animations share one frame timestamp");
    for(unsigned frame=3;frame<150;++frame) for(auto& c:channels) c.Update(1,2,D2D1::RectF(400,0,500,1),frame,1016+frame*16,motion::spring::Smooth);
    active=false;for(auto& c:channels)active|=c.Active(4000);
    check(!active,"all concurrent channels settle and release animation demand");
    channels[0].SetAnimationsEnabled(false);
    check(channels[0].Update(1,3,D2D1::RectF(0,0,100,1),151,4016,motion::spring::Smooth).left==0 && !channels[0].Active(4016),"reduced motion snaps without continuing frames");
    return fails?1:0;
}

#include "../app/dialog_jump_popup_geometry.h"
#include <cstdio>

int main() {
    using pulse::app::PlaceDialogJumpPopup;
    int failures=0;
    auto check=[&](bool ok,const char* name) {
        std::printf("[%s] %s\n",ok ? "PASS" : "FAIL",name);
        if(!ok) ++failures;
    };
    const RECT work{0,0,1920,1040},bar{200,700,1000,750};
    auto p=PlaceDialogJumpPopup(bar,work,36,1,420,300,4,false,true);
    check(p.rect.left==236 && p.rect.top==396 && p.above,"search aligns to input field above bar");
    p=PlaceDialogJumpPopup(bar,work,780,1,420,300,4,true,true);
    check(p.rect.right==980,"places aligns to more button right edge");
    p=PlaceDialogJumpPopup(bar,work,36,1.5f,630,450,6,false,true);
    check(p.rect.left==254 && p.rect.top==244,"150 percent converts anchor exactly once");
    p=PlaceDialogJumpPopup({-1800,700,-900,750},{-1920,0,0,1080},36,2,840,600,8,false,true);
    check(p.rect.left==-1728 && p.rect.top==92,"negative monitor origin at 200 percent");
    p=PlaceDialogJumpPopup({1800,700,2200,750},work,36,1,420,300,4,false,true);
    check(p.rect.left==1500 && p.rect.right==1920,"right edge remains in work area");
    p=PlaceDialogJumpPopup({-400,700,100,750},work,36,1,420,300,4,false,true);
    check(p.rect.left==0,"left edge remains in work area");
    p=PlaceDialogJumpPopup({200,100,1000,150},work,36,1,420,300,4,false,true);
    check(!p.above && p.rect.top==154,"opens below when insufficient space above");
    p=PlaceDialogJumpPopup(bar,work,36,1,420,400,4,false,false);
    check(p.above && p.rect.top==296,"opens above when insufficient space below");
    p=PlaceDialogJumpPopup({10,20,100,50},{0,0,160,120},36,1,420,300,4,false,true);
    check(p.rect.left==0 && p.rect.top==0 && p.rect.right==160 && p.rect.bottom==120,"small work area bounds popup size");
    const auto moved=PlaceDialogJumpPopup({500,800,1300,850},work,36,1,420,300,4,false,true);
    check(moved.rect.left==536 && moved.rect.top==496,"updated bar geometry moves popup with host");
    return failures ? 1 : 0;
}

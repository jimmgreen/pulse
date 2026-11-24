#include "../app/desktop_preview_session.h"
#include <cstdio>
int main() {
    int total=0, failed=0;
    auto check=[&](bool ok,const char* name) { ++total; failed+=!ok; std::printf("[%s] %s\n",ok?"PASS":"FAIL",name); };
    pulse::DesktopPreviewSession s;
    check(!s.Request(L"a",true,false,0,500),"hover disabled");
    check(s.Request(L"a",false,false,0,500)&&s.Ready(0),"explicit ignores hover preference and delay");
    check(!s.Cancel(L"a")&&!s.Request(L"b",true,true,1,500),"cancel and hover preserve explicit");
    s.Reset(); check(s.Request(L"a",true,true,0,500),"hover armed");
    check(!s.Ready(499)&&s.Ready(500),"configured delay boundary");
    auto old=s.generation;
    check(!s.Request(L"a",true,true,100,500)&&s.due==500,"same candidate does not restart delay");
    check(s.Request(L"b",true,true,200,500)&&!s.Accept(old),"rapid switch rejects old metadata");
    check(!s.Cancel(L"a")&&s.path==L"b","stale cancel preserves newer hover");
    old=s.generation; check(s.Cancel(L"b")&&!s.Accept(old),"cancel invalidates metadata");
    s.Request(L"a",true,true,0,500);old=s.generation;s.Reset();
    check(!s.Accept(old)&&!s.Ready(1000),"owner destruction invalidates timer and metadata");
    s.Request(L"a",true,true,0,500);s.Request(L"a",false,false,100,500);
    check(!s.hover&&s.Ready(100)&&!s.Cancel(L"a"),"explicit promotes passive preview");
    s.Reset();
    check(!s.Outside(true,true,100)&&!s.Outside(true,true,249)&&s.Outside(true,true,250),"visible hover allows 150ms gap crossing");
    check(!s.Outside(false,true,251)&&!s.Outside(true,true,300),"entering popup clears leave deadline");
    check(s.Outside(true,false,301),"unshown candidate cancels immediately on leave");
    std::printf("%d cases, %d failures\n",total,failed);return failed?1:0;
}


#include "../app/new_menu_state.h"
#include <atomic>
#include <iostream>
#include <thread>

using Cache = pulse::app::NewMenuCache<int>;
using State = pulse::app::NewMenuLoadState;
int main() {
    int failed=0,passed=0;
    auto check=[&](bool ok,const char* label){std::cout<<(ok?"[PASS] ":"[FAIL] ")<<label<<'\n';ok?++passed:++failed;};
    Cache cache;
    check(cache.Read().state==State::Loading && cache.Read().list.empty(),"cold state explicitly loading");
    check(cache.BeginRefresh() && !cache.BeginRefresh(),"single flight excludes duplicate enumeration");
    auto cold=cache.Read();
    std::atomic<bool> release{false}, started{false};
    std::thread worker([&]{started=true;while(!release.load())std::this_thread::yield();cache.Complete({4,5});});
    while(!started.load())std::this_thread::yield();
    check(cache.Read().state==State::Loading && cache.Read().revision==cold.revision,"UI reads return while enumeration blocked");
    release=true;worker.join();
    auto ready=cache.Read();
    check(ready.state==State::Ready && ready.list==std::vector<int>({4,5}) && ready.revision>cold.revision,"completion publishes list and revision together");
    check(cold.list.empty() && cold.state==State::Loading,"dispatch snapshot remains immutable until UI accepts");
    check(cache.BeginRefresh() && cache.Read().list==ready.list && cache.Read().state==State::Ready,"warm refresh retains usable rows");
    cache.Fail();
    check(cache.Read().state==State::Failed && cache.Read().list==ready.list,"failure preserves previous usable templates");
    check(cache.BeginRefresh() && cache.Read().state==State::Ready,"failure releases single-flight slot and warm retry keeps usable rows");
    cache.Complete({});
    check(cache.Read().state==State::Ready && cache.Read().list.empty(),"successful empty result is ready not permanently loading");
    Cache rejected; rejected.BeginRefresh(); rejected.Fail();
    check(rejected.Read().state==State::Failed && rejected.BeginRefresh(),"queue rejection surfaces failure and permits retry");
    rejected.Complete({9});
    check(rejected.Read().state==State::Ready && rejected.Read().list==std::vector<int>({9}),"retry succeeds after initial failure");
    std::cout<<"summary: passed="<<passed<<" failed="<<failed<<'\n';
    return failed?1:0;
}



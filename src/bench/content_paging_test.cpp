#include "../index/content_index.h"
#include "../index/content_search_client.h"
#include "../index/content_result_store.h"
#include "../index/content_search_session.h"
#include "../../third_party/sqlite/sqlite3.h"
#include <filesystem>
#include <fstream>
#include <future>
#include <atomic>
#include <algorithm>
#include <cstdio>
using namespace pulse;
namespace {
int failures=0;
void Check(bool ok,const char* label) { printf("[%s] %s\n",ok ? "PASS":"FAIL",label);fflush(stdout);if(!ok)++failures; }
bool WaitRow(const std::shared_ptr<index::ContentResultStore>& store,size_t i,index::ContentResultStore::Row& row) {
    const auto start=GetTickCount64();
    while(GetTickCount64()-start<5000) { if(store->Get(i,row)) return true; Sleep(2); }
    return false;
}
std::wstring Name(int i) { wchar_t name[40]{}; swprintf_s(name,L"f%05d.%s",i,i%2 ? L"txt":L"md"); return name; }
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--store-filter-metadata") {
        constexpr size_t count=12000, snippet_chars=512;
        auto store=std::make_shared<index::ContentResultStore>(nullptr,0);
        std::vector<index::ContentHit> hits;
        for(size_t i=0;i<count;++i) {
            index::ContentHit hit;
            hit.name=Name(static_cast<int>(i)); hit.path=L"C:\\metadata-fixture\\"+hit.name;
            hit.snippet=std::wstring(snippet_chars,L'x'); hit.size=i; hit.modified=0x123400000000ull+i;
            hit.line=42; hit.file_id=i+1; hits.push_back(std::move(hit));
        }
        Check(store->Append(hits),"metadata fixture appends 12000 full-snippet rows");
        hits.clear(); hits.shrink_to_fit();
        const auto wait_filter=[&] {
            const auto end=GetTickCount64()+30000;
            while(store->Filtering() && GetTickCount64()<end) Sleep(1);
            return !store->Filtering() && !store->Error();
        };
        std::atomic<bool> fields_valid{true};
        auto filter=[&](const fs::DirEntry& e) {
            const auto stamp=(uint64_t{e.mtime.dwHighDateTime}<<32)|e.mtime.dwLowDateTime;
            if(e.name!=Name(static_cast<int>(e.size)) || e.full_path!=L"C:\\metadata-fixture\\"+e.name ||
                stamp!=0x123400000000ull+e.size || e.attrs!=FILE_ATTRIBUTE_NORMAL) fields_valid=false;
            return e.size%2==0;
        };
        const auto now=[] { return std::chrono::steady_clock::now(); };
        const auto elapsed=[&](auto start) { return std::chrono::duration<double,std::milli>(now()-start).count(); };
        std::vector<double> filter_times,clear_times,match_times;
        for(int run=0;run<5;++run) {
            auto start=now(); store->SetFilter(filter);
            const bool filtered=wait_filter(); filter_times.push_back(elapsed(start));
            Check(filtered && fields_valid && store->Count()==count/2,"filter sees unchanged metadata and returns exact even rows");
            start=now();
            auto done=std::make_shared<std::promise<index::ContentResultStore::Selection>>(); auto future=done->get_future();
            store->Match([](const fs::DirEntry& e){return e.size%4==0;},[done](auto result){done->set_value(std::move(result));});
            const bool ready=future.wait_for(std::chrono::seconds(30))==std::future_status::ready;
            match_times.push_back(elapsed(start));
            auto result=ready ? future.get() : index::ContentResultStore::Selection{};
            Check(ready && !result.error && result.matches.size()==count/4 && result.matches.front()==0 && result.matches.back()==5998,
                "Match returns correct filtered display positions without changing rows");
            start=now(); store->SetFilter({});
            Check(wait_filter() && store->Count()==count,"clearing filter restores all rows"); clear_times.push_back(elapsed(start));
        }
        store->SetFilter(filter); Check(wait_filter(),"filter ready for display and resolve checks");
        index::ContentResultStore::Row row;
        Check(WaitRow(store,1,row) && row.entry.size==2 && row.file_id==3 && row.snippet==L"L42  "+std::wstring(snippet_chars,L'x'),
            "display pages retain exact line prefix, snippet and identity");
        auto resolved=std::make_shared<std::promise<index::ContentResultStore::Selection>>();auto selection=resolved->get_future();
        store->Resolve({0,1},false,0,[resolved](auto result){resolved->set_value(std::move(result));},store->OrderRevision());
        const bool selected=selection.wait_for(std::chrono::seconds(30))==std::future_status::ready;
        auto result=selected ? selection.get() : index::ContentResultStore::Selection{};
        Check(selected && !result.error && result.rows.size()==2 && result.rows[1].second.entry.size==2 && result.rows[1].second.snippet==row.snippet,
            "selection resolve preserves order and full snippets");
        index::ContentHit changed;
        changed.file_id=3;changed.name=Name(2);changed.path=L"C:\\metadata-fixture\\"+changed.name;
        changed.size=3;changed.modified=9;changed.line=5;changed.snippet=L"updated";
        Check(store->ApplyChanges({changed},index::ContentResultSort::Size,true) && store->Count()==count/2-1 &&
            WaitRow(store,0,row) && row.entry.size==count-2,"live update retains filter membership and descending order");
        auto entered=std::make_shared<std::promise<void>>();auto gate=entered->get_future();
        auto release=std::make_shared<std::promise<void>>();auto released=release->get_future().share();
        store->FindIdentity(1,[entered,released](int){entered->set_value();released.wait();});
        const bool blocked=gate.wait_for(std::chrono::seconds(30))==std::future_status::ready;
        std::atomic<int> obsolete_calls{0};
        store->SetFilter([&](const fs::DirEntry&){++obsolete_calls;return false;});
        store->SetFilter([](const fs::DirEntry& e){return e.size<10;});
        release->set_value();
        Check(blocked && wait_filter() && obsolete_calls==0 && store->Count()==10,"latest filter cancels queued stale filter without publishing old results");
        Check(store->CachedRows()<=index::ContentResultStore::kPageSize*index::ContentResultStore::kCachePages,"metadata scan preserves bounded display cache");
        for(auto* times:{&filter_times,&clear_times,&match_times}) std::sort(times->begin(),times->end());
        printf("metadata_scan rows=%zu snippet_chars=%zu fixture_snippet_payload_bytes_per_full_scan=%zu filter_median_ms=%.3f clear_median_ms=%.3f match_median_ms=%.3f failures=%d\n",
            count,snippet_chars,count*snippet_chars*sizeof(wchar_t),filter_times[2],clear_times[2],match_times[2],failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--task-stream") {
        index::ContentSearchRequest request; request.generation=440; request.task_scan=true;
        request.paged_results=true; request.sort=index::ContentResultSort::Name;
        request.previous_results=std::make_shared<index::ContentResultStore>(nullptr,0);
        index::ContentSearchSession session(request,nullptr,0);
        auto deliver=[&](std::vector<index::ContentHit> hits,bool done=false,bool delta=false) {
            index::ContentSearchUpdate update; update.progress.generation=request.generation;
            update.progress.done=done; update.progress.delta=delta; update.hits=std::move(hits);
            return session.Accept(std::move(update));
        };
        auto make=[](const wchar_t* name,uint64_t size,uint64_t id) {
            index::ContentHit hit{L"C:\\fixture\\"+std::wstring(name),name,L"3d3s matching body",size,0,1}; hit.file_id=id; return hit;
        };
        auto z=make(L"z-cache.txt",50,11),y=make(L"y-cache.txt",40,12);
        auto update=deliver({z,y}); auto store=session.Results(); index::ContentResultStore::Row row;
        Check(update && update->results==store && !session.Done() && store->Count()==2 && WaitRow(store,0,row) && row.file_id==12,
            "task publishes cached first batch in requested order before completion");
        const auto selected_order = store->OrderRevision();
        auto doc=make(L"a.xls",10,0),code=make(L"m.cpp",30,0);
        update=deliver({doc,code});
        Check(update && !session.Done() && store->Count()==4 && store->Get(0,row) && row.entry.name==doc.name,
            "document and code batches merge into visible global order without blank cached page");
        std::promise<index::ContentResultStore::Selection> stale_selection;
        auto stale_result = stale_selection.get_future();
        store->Resolve({0}, false, 2, [&](auto value) { stale_selection.set_value(std::move(value)); }, selected_order);
        Check(stale_result.get().error == ERROR_CANCELLED, "selection from previous order cannot resolve to an inserted file");
        auto duplicate=doc; duplicate.path=L"c:\\FIXTURE\\A.XLS";
        update=deliver({duplicate,doc});
        Check(update && store->Count()==4 && store->RawCount()==4,"same Windows path across both sources is admitted only once");
        auto sequence=[&](const std::wstring& path) {
            sqlite3* db=nullptr; sqlite3_stmt* statement=nullptr; sqlite3_int64 seq=-1;
            if(sqlite3_open16(store->CachePath().c_str(),&db)==SQLITE_OK &&
                sqlite3_prepare_v2(db,"SELECT seq FROM hits WHERE path=?1 COLLATE BINARY",-1,&statement,nullptr)==SQLITE_OK) {
                sqlite3_bind_text16(statement,1,path.data(),static_cast<int>(path.size()*sizeof(wchar_t)),SQLITE_TRANSIENT);
                if(sqlite3_step(statement)==SQLITE_ROW) seq=sqlite3_column_int64(statement,0);
            }
            sqlite3_finalize(statement); sqlite3_close(db); return seq;
        };
        const auto before=sequence(doc.path);
        store->SetSort(index::ContentResultSort::Size,true);
        auto deadline=GetTickCount64()+5000; while(store->Sorting() && GetTickCount64()<deadline) Sleep(1);
        auto newest=make(L"c.cpp",60,0); update=deliver({newest});
        Check(update && store->Count()==5 && store->Get(0,row) && row.entry.name==newest.name,
            "later task batches honor newest user sort instead of original query order");
        doc.file_id=101; update=deliver({doc},false,true);
        Check(!update && store->Count()==5 && store->Get(0,row),"incomplete realtime delta keeps displayed pages intact");
        update=deliver({},true,true);
        Check(update && store->Count()==5 && before>=0 && sequence(doc.path)==before && store->Get(4,row) && row.file_id==101,
            "temporary identity upgrades preserve spool sequence and visible row position");
        auto old_cached=y;old_cached.file_id=0;update=deliver({old_cached});
        Check(update && store->Count()==5 && store->Get(2,row) && row.file_id==12,
            "late temporary duplicate cannot downgrade a cached identity");
        std::promise<int> identity;auto found=identity.get_future();store->FindIdentity(101,[&](int value){identity.set_value(value);});
        Check(found.get()==4,"upgraded index identity resolves existing display row");
        update=session.Fail(ERROR_CANCELLED);
        Check(update && update->progress.error==ERROR_CANCELLED && update->results==store && store->Count()==5 && store->Get(0,row),
            "cancelled task retains already published mixed-source rows");
        std::atomic<size_t> filter_calls{0};store->SetFilter([&](const fs::DirEntry& entry){++filter_calls;return entry.size>=30;});
        deadline=GetTickCount64()+5000;while(store->Filtering() && GetTickCount64()<deadline) Sleep(1);
        Check(!store->Filtering() && store->Count()==4 && WaitRow(store,0,row),"task stream filtering primes visible cache");
        const auto calls=filter_calls.load();code.size=20;
        Check(store->StreamUpsert({code},request.sort,false) && store->Count()==3 && store->Get(0,row) && row.entry.name==newest.name && filter_calls==calls+1,
            "filtered stream updates only changed membership and retains current sort");
        Check(store->CachedRows()<=index::ContentResultStore::kCachePages*index::ContentResultStore::kPageSize,"mixed-source display cache stays bounded");
        auto tied=std::make_shared<index::ContentResultStore>(nullptr,0);
        auto temporary=make(L"first.xls",1,0),cached=make(L"second.txt",1,20);
        Check(tied->StreamUpsert({temporary,cached},index::ContentResultSort::Size,false) && WaitRow(tied,0,row) && row.entry.name==temporary.name,
            "equal sort keys retain first displayed task identity");
        temporary.file_id=101;
        Check(tied->ApplyChanges({temporary},index::ContentResultSort::Size,false) && tied->Get(0,row) && row.file_id==101 && tied->Count()==2,
            "index identity promotion cannot reorder equal-key displayed rows");
        printf("failures=%d\n",failures);return failures ? 1:0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--store-append-latency") {
        auto store=std::make_shared<index::ContentResultStore>(nullptr,0);
        constexpr size_t count=20000,batch_size=64;
        std::vector<double> batches;
        index::ContentResultStore::Row row;
        bool correct=true,readable=true;
        const auto begin=std::chrono::steady_clock::now();
        for(size_t offset=0;offset<count;offset+=batch_size) {
            std::vector<index::ContentHit> hits;
            for(size_t i=offset;i<std::min(count,offset+batch_size);++i) {
                index::ContentHit hit{L"C:\\fixture\\"+Name(static_cast<int>(i)),Name(static_cast<int>(i)),L"synthetic matching snippet",i,0,1};
                hit.file_id=i+1; hits.push_back(std::move(hit));
            }
            const auto append_begin=std::chrono::steady_clock::now();
            if(!store->Append(hits)) {correct=false;break;}
            batches.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-append_begin).count());
            if(!offset) {
                readable=WaitRow(store,0,row);
                store->SetSort(index::ContentResultSort::Size,true);
                const auto deadline=GetTickCount64()+5000;
                while(store->Sorting() && GetTickCount64()<deadline) Sleep(1);
                correct &= !store->Sorting();
            }
            const bool ready=store->Get(0,row);
            readable &= ready;
            correct &= ready && row.file_id==std::min(count,offset+batch_size);
        }
        const double total_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        std::sort(batches.begin(),batches.end());
        printf("append rows=%zu batch_size=%zu total_ms=%.3f p95_ms=%.3f max_ms=%.3f\n",count,batch_size,total_ms,
            batches.empty() ? 0:batches[(batches.size()-1)*95/100],batches.empty() ? 0:batches.back());
        Check(correct && store->Count()==count && !store->Error(),"every streamed batch keeps requested descending order");
        Check(readable,"first cached page stays synchronously readable after each append");
        Check(WaitRow(store,count-1,row) && row.file_id==1,"streamed sort retains stable identity at final last position");
        Check(store->CachedRows()<=index::ContentResultStore::kCachePages*index::ContentResultStore::kPageSize,"streamed sort cache remains bounded");
        std::atomic<size_t> filter_calls{0};
        store->SetFilter([&](const fs::DirEntry& entry){++filter_calls;return entry.size%2==0;});
        const auto deadline=GetTickCount64()+5000;
        while(store->Filtering() && GetTickCount64()<deadline) Sleep(1);
        Check(!store->Filtering() && store->Count()==count/2 && WaitRow(store,0,row),"filtered streaming fixture primes display cache");
        index::ContentHit accepted{L"C:\\fixture\\accepted.txt",L"accepted.txt",L"body",count,0,1}; accepted.file_id=count+1;
        index::ContentHit excluded{L"C:\\fixture\\excluded.txt",L"excluded.txt",L"body",count+1,0,1}; excluded.file_id=count+2;
        Check(store->Append({accepted,excluded}) && store->Count()==count/2+1 && store->Get(0,row) && row.file_id==count+1,
            "filtered append merges only admitted identities into exact order");
        Check(filter_calls==count+2,"append never reruns filter for prior hits");
        printf("failures=%d\n",failures); return failures ? 1:0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--store-sort-latency") {
        auto store=std::make_shared<index::ContentResultStore>(nullptr,0);
        constexpr size_t count=20000;
        std::vector<index::ContentHit> hits;
        hits.reserve(count);
        for (size_t i=0;i<count;++i) {
            index::ContentHit hit{L"C:\\fixture\\"+Name(static_cast<int>(i)),Name(static_cast<int>(i)),L"synthetic matching snippet",i,0,1};
            hit.file_id=i+1; hits.push_back(std::move(hit));
        }
        Check(store->Append(hits),"latency fixture stores 20000 synthetic hits without file scanning");
        index::ContentResultStore::Row row;
        Check(WaitRow(store,0,row) && row.file_id==1,"latency fixture primes first display page");
        for (bool descending : {true,false,true}) {
            std::promise<void> entered,release;
            auto ready=entered.get_future(); auto resume=release.get_future();
            store->FindIdentity(1,[&](int){entered.set_value();resume.wait();});
            ready.wait();
            const auto old_identity=row.file_id;
            const auto begin=std::chrono::steady_clock::now();
            store->SetSort(index::ContentResultSort::Size,descending);
            const auto submitted=std::chrono::steady_clock::now();
            const double submit_ms=std::chrono::duration<double,std::milli>(submitted-begin).count();
            Check(store->Sorting() && store->Get(0,row) && row.file_id==old_identity,"queued sort keeps previous display page readable");
            const auto work_begin=std::chrono::steady_clock::now();
            release.set_value();
            bool readable=true;
            while (store->Sorting() && std::chrono::steady_clock::now()-work_begin<std::chrono::seconds(5)) {
                readable=store->Get(0,row) && readable;
                Sleep(1);
            }
            const double finish_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-work_begin).count();
            printf("sort direction=%s submit_ms=%.3f completion_ms=%.3f rows=%zu\n",descending ? "descending":"ascending",submit_ms,finish_ms,count);
            Check(submit_ms<100.0,"SetSort returns within 100 ms");
            Check(!store->Sorting() && finish_ms<5000.0 && !store->Error(),"background sorting completes within five seconds");
            Check(readable && store->Get(0,row) && row.file_id==(descending ? count:1),"display remains readable during sorting and publishes correct first identity");
        }
        Check(store->CachedRows()<=index::ContentResultStore::kCachePages*index::ContentResultStore::kPageSize,"latency sorting preserves bounded display cache");
        printf("failures=%d\n",failures); return failures ? 1:0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--store-realtime") {
        auto store=std::make_shared<index::ContentResultStore>(nullptr,0);
        index::ContentHit a{L"C:\\fixture\\z.txt",L"z.txt",L"body",1,0,1}; a.file_id=11;
        index::ContentHit b{L"C:\\fixture\\a.md",L"a.md",L"body",2,0,1}; b.file_id=12;
        Check(store->Append({a,b}),"append stable identities");
        index::ContentResultStore::Row row;
        Check(WaitRow(store,0,row) && row.file_id==11,"page reads stable identity");
        auto old=a; old.removed=true; a.path=b.path; a.name=b.name;
        Check(store->ApplyChanges({a,old},index::ContentResultSort::Type,false),"rename over existing result then delete old path");
        Check(store->Count()==1 && WaitRow(store,0,row) && row.file_id==11 && row.entry.full_path==a.path,"overwrite retains source identity");
        std::promise<int> found; auto future=found.get_future();
        store->FindIdentity(11,[&](int i){found.set_value(i);});
        Check(future.wait_for(std::chrono::seconds(5))==std::future_status::ready && future.get()==0,"identity resolves renamed result");
        b.path=L"C:\\fixture\\b.txt"; b.name=L"b.txt";
        Check(store->ApplyChanges({b},index::ContentResultSort::Type,true),"type sort update");
        std::promise<void> entered, release;
        auto entered_future=entered.get_future(); auto release_future=release.get_future();
        store->FindIdentity(11,[&](int){entered.set_value();release_future.wait();});
        entered_future.wait();
        store->SetFilter([](const fs::DirEntry&){return true;});
        Check(store->ApplyChanges({a},index::ContentResultSort::Type,true),"update while filter queued");
        release.set_value();
        auto deadline=GetTickCount64()+5000;
        while(store->Filtering() && GetTickCount64()<deadline) Sleep(2);
        Check(!store->Filtering() && store->Count()==2 && WaitRow(store,0,row) && row.file_id==12,"filter completes and retains descending type order");
        Check(store->ApplyChanges({old},index::ContentResultSort::Type,true) && store->Count()==2,"late old-path removal preserves renamed result");
        std::promise<index::ContentResultStore::Selection> matches;
        auto matched=matches.get_future();
        store->Match([](const fs::DirEntry& e){return e.name==L"b.txt";},[&](auto v){matches.set_value(std::move(v));});
        Check(matched.get().matches==std::vector<int>{0},"pattern selection uses sorted visible positions");
        b.name=L"\u00c4B.txt"; b.path=L"C:\\fixture\\"+b.name;
        a.name=L"\u00e4A.txt"; a.path=L"C:\\fixture\\"+a.name;
        Check(store->ApplyChanges({a,b},index::ContentResultSort::Name,false) && WaitRow(store,0,row) && row.file_id==11,"Unicode name ordering matches ordinal initial query");
        std::atomic<int> filter_calls{0};
        store->SetFilter([&](const fs::DirEntry& e){++filter_calls;return e.size<=2;});
        deadline=GetTickCount64()+5000;
        while(store->Filtering() && GetTickCount64()<deadline) Sleep(2);
        Check(WaitRow(store,0,row),"prime sorted display cache");
        const auto calls=filter_calls.load();
        std::promise<void> sort_entered,sort_release;
        auto sort_ready=sort_entered.get_future(); auto sort_go=sort_release.get_future();
        store->FindIdentity(11,[&](int){sort_entered.set_value();sort_go.wait();});
        sort_ready.wait();
        store->SetSort(index::ContentResultSort::Size,true);
        store->SetSort(index::ContentResultSort::Name,true);
        store->SetSort(index::ContentResultSort::Size,false);
        Check(store->Sorting() && store->Get(0,row) && row.file_id==11,"queued sorting preserves cached rows");
        sort_release.set_value();
        deadline=GetTickCount64()+5000;
        while(store->Sorting() && GetTickCount64()<deadline) Sleep(2);
        Check(!store->Sorting() && !store->Error() && store->Get(0,row) && row.file_id==11 && filter_calls==calls,"latest sort wins without rerunning filter or content matching");
        store->SetSort(index::ContentResultSort::Size,true);
        deadline=GetTickCount64()+5000;
        while(store->Sorting() && GetTickCount64()<deadline) Sleep(2);
        Check(!store->Sorting() && store->Get(0,row) && row.file_id==12 && row.snippet==L"L1  body","direction changes preserve hit identity and snippet");
        a.size=3;
        Check(store->ApplyChanges({a},index::ContentResultSort::Name,false) && store->Count()==1 && store->Get(0,row) && row.file_id==12,"live update retains filter and atomically replaces cached page");
        a.size=1;
        Check(store->ApplyChanges({a},index::ContentResultSort::Name,false) && store->Get(0,row) && row.file_id==12,"live update uses latest requested sort");
        Check(store->CachedRows()<=index::ContentResultStore::kCachePages*index::ContentResultStore::kPageSize,"sorting keeps cache bounded");
        store->SetFilter({});
        deadline=GetTickCount64()+5000;
        while(store->Filtering() && GetTickCount64()<deadline) Sleep(2);
        Check(WaitRow(store,0,row),"prime streaming append page");
        index::ContentHit larger{L"C:\\fixture\\large.txt",L"large.txt",L"body",4,0,1}; larger.file_id=13;
        index::ContentHit smaller{L"C:\\fixture\\small.txt",L"small.txt",L"body",0,0,1}; smaller.file_id=14;
        Check(store->Append({larger,smaller}) && store->Count()==4 && store->Get(0,row) && row.file_id==13 && store->Get(3,row) && row.file_id==14,"streaming append merges larger and smaller hits into requested descending order without dropping cached page");
        printf("failures=%d\n",failures); return failures ? 1:0;
    }

    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,ARRAYSIZE(module));
    const auto base=std::filesystem::path(module).parent_path().parent_path()/L"bench_data"/(L"paging-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(base);
    constexpr int count=12050;
    std::wstring spool_path;
    {
        auto store=std::make_shared<index::ContentResultStore>(nullptr,0);
        std::vector<index::ContentHit> batch;
        for(int i=0;i<count;++i) {
            batch.push_back({L"C:\\fixture\\"+Name(i),Name(i),L"20 complete result",static_cast<uint64_t>(i),0,1});
            if(batch.size()==64 || i==count-1) { if(!store->Append(batch)) { Check(false,"append disk spool"); break; } batch.clear(); }
        }
        Check(store->Count()==count,"disk spool keeps every match past 10000");
        spool_path=store->CachePath();
        index::ContentResultStore::Row row;
        for(const size_t i:{size_t{0},size_t{10000},size_t{12049},size_t{4096},size_t{0}})
            Check(WaitRow(store,i,row) && row.entry.name==Name(static_cast<int>(i)),"random page access retains global row identity");
        for(size_t i=0;i<count;i+=256) WaitRow(store,i,row);
        Check(store->CachedRows()<=index::ContentResultStore::kCachePages*index::ContentResultStore::kPageSize,"resident display pages remain bounded after scrolling entire result set");
        std::promise<index::ContentResultStore::Selection> selected;
        store->Resolve({1,10001,12049},false,count,[&](auto value){selected.set_value(std::move(value));});
        const auto resolved=selected.get_future().get();
        Check(!resolved.error && resolved.rows.size()==3 && resolved.rows[1].second.entry.name==Name(10001),"cross-page selection resolves correct files");
        std::promise<index::ContentResultStore::Selection> all;
        store->Resolve({},true,count,[&](auto value){all.set_value(std::move(value));});
        Check(all.get_future().get().rows.size()==count,"select-all resolution includes unloaded pages");
        store->SetFilter([](const fs::DirEntry& e){return e.name.starts_with(L"f1204");});
        const auto deadline=GetTickCount64()+5000;
        while(store->Filtering() && GetTickCount64()<deadline) Sleep(2);
        Check(store->Count()==10 && WaitRow(store,9,row) && row.entry.name==Name(12049),"filter scans the full disk result set, including unloaded tail");
        store->SetFilter({});
        while(store->Filtering() && GetTickCount64()<deadline) Sleep(2);
        Check(store->Count()==count,"clearing filter restores complete results");
    }
    for(int i=0;i<100 && GetFileAttributesW(spool_path.c_str())!=INVALID_FILE_ATTRIBUTES;++i) Sleep(10);
    Check(GetFileAttributesW(spool_path.c_str())==INVALID_FILE_ATTRIBUTES,"query spool removed when its owner is released");
    const auto root=base/L"files", profile=base/L"profile";
    std::filesystem::create_directories(root);
    const auto dbdir=profile/L"Pulse"/L"ContentIndex";std::filesystem::create_directories(dbdir);
    for(int i=0;i<count;++i) std::ofstream(root/Name(i)) << "20 complete result " << i;
    index::ContentIndexConfig config;config.roots={{root.wstring(),text::Encoding::Auto}};
    {
        index::ContentIndex db((dbdir/L"content-v1.sqlite").wstring());
        Check(db.Configure(config) && db.WaitUntilIdle(120000),"12050-document isolated index ready");
        for(auto sort:{index::ContentResultSort::Index,index::ContentResultSort::Name,index::ContentResultSort::Path,index::ContentResultSort::Type,index::ContentResultSort::Size,index::ContentResultSort::Mtime}) {
            index::ContentSearchRequest r;r.needle=L"20";r.sort=sort;r.sort_desc=true;
            std::atomic<bool> cancel{false};size_t hits=0,first=0;bool done=false,truncated=false,ordered=true;
            std::wstring previous; uint64_t previous_size=UINT64_MAX,previous_time=UINT64_MAX;
            db.Search(r,cancel,[&](const auto& progress,auto batch) {
                if(!first && !batch.empty()) first=batch.size();
                for(const auto& hit:batch) {
                    const auto key=sort==index::ContentResultSort::Path ? hit.path : sort==index::ContentResultSort::Type ? hit.name.substr(hit.name.find_last_of(L'.')+1) : hit.name;
                    if(hits && (sort==index::ContentResultSort::Name || sort==index::ContentResultSort::Path || sort==index::ContentResultSort::Type))
                        ordered &= CompareStringOrdinal(previous.c_str(),-1,key.c_str(),-1,TRUE)!=CSTR_LESS_THAN;
                    if(sort==index::ContentResultSort::Size) ordered &= hit.size<=previous_size;
                    if(sort==index::ContentResultSort::Mtime) ordered &= hit.modified<=previous_time;
                    previous=key;previous_size=hit.size;previous_time=hit.modified;++hits;
                }
                done=progress.done;truncated|=progress.truncated;return true;
            });
            Check(done && !truncated && hits==count && first==1 && ordered,"global ordering and first-hit delivery cover more than 10000 matches");
        }
    }
    wchar_t previous[32768]{};const auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",previous,ARRAYSIZE(previous));
    SetEnvironmentVariableW(L"LOCALAPPDATA",profile.c_str());
    {
        index::ContentSearchClient client;client.Start(nullptr,0);client.Configure(config);
        for(int i=0;i<400 && client.GetConfig().roots.empty();++i) Sleep(10);
        index::ContentSearchRequest r;r.needle=L"20";r.generation=100;r.paged_results=true;r.sort=index::ContentResultSort::Name;r.sort_desc=true;
        client.SearchAsync(r);bool done=false,valid=true;std::shared_ptr<index::ContentResultStore> store;
        const auto start=GetTickCount64();
        while(!done && GetTickCount64()-start<30000) {
            index::ContentSearchUpdate update;
            while(client.TakeUpdate(update)) {valid &= update.hits.empty() && !update.progress.error && !update.progress.truncated;store=update.results;done=update.progress.done;}
            if(!done) Sleep(5);
        }
        Check(done && valid && store && store->Count()==count,"IPC produces complete disk-backed result set without retaining hit batches");
        index::ContentResultStore::Row row;
        Check(store && WaitRow(store,10000,row) && row.entry.name==Name(count-1-10000),"page beyond old limit follows global descending sort");
        client.Cancel();r.generation=101;client.SearchAsync(r);client.Cancel();
        index::ContentSearchUpdate stale;Sleep(20);
        Check(!client.TakeUpdate(stale),"cancelled paging query cannot publish stale updates");
        client.Stop();
    }
    SetEnvironmentVariableW(L"LOCALAPPDATA",n ? previous:nullptr);
    printf("failures=%d\n",failures);wprintf(L"Fixture: %s\n",base.c_str());return failures ? 1:0;
}

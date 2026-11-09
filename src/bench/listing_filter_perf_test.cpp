#include "../app/app_model.h"
#include "../app/name_pattern.h"
#include "../app/folder_alias.h"
#include "../app/entry_sort.h"
#include "../app/entry_group.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_snapshot.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <random>
#include <cwctype>
#include <psapi.h>

namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }
using namespace pulse;
namespace {
bool ReferenceMatch(std::wstring text, std::wstring pattern) {
    for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
    for (auto& c : pattern) c = static_cast<wchar_t>(std::towlower(c));
    if (pattern.find_first_of(L"*?") == std::wstring::npos) return text.find(pattern) != std::wstring::npos;
    size_t si=0, pi=0, star=std::wstring::npos, match=0;
    while(si<text.size()) {
        if(pi<pattern.size() && pattern[pi]==L'*') { star=pi++; match=si; }
        else if(pi<pattern.size() && (pattern[pi]==L'?' || pattern[pi]==text[si])) { ++si; ++pi; }
        else if(star!=std::wstring::npos) { pi=star+1; si=++match; }
        else return false;
    }
    while(pi<pattern.size() && pattern[pi]==L'*') ++pi;
    return pi==pattern.size();
}
int CheckSnapshotLifetime() {
    int failures = 0;
    auto check = [&](bool good, const char* label) {
        std::printf("[%s] %s\n", good ? "PASS" : "FAIL", label);
        failures += !good;
    };
    app::Pane pane;
    pane.view.current_path = L"C:/isolated";
    auto rows = std::make_shared<std::vector<fs::DirEntry>>(20000);
    size_t payload_bytes = sizeof(*rows) + rows->capacity() * sizeof(fs::DirEntry);
    for (size_t i = 0; i < rows->size(); ++i) {
        (*rows)[i].name = std::wstring(80, L'x') + std::to_wstring(i) + L".txt";
        payload_bytes += ((*rows)[i].name.capacity() + 1) * sizeof(wchar_t);
    }
    std::weak_ptr<const std::vector<fs::DirEntry>> previous = rows;
    pane.view.SetSnapshot(rows);
    {
        ui::PaneViewModel view;
        app::FillPaneViewModel(view, pane);
    }
    rows.reset();
    pane.view.SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>());
    check(previous.expired(), "hidden tab releases old snapshot without another view fill");
    std::printf("snapshot_payload entries=20000 estimated_bytes=%zu retained_after_replace=%zu\n",
        payload_bytes, previous.expired() ? size_t{0} : payload_bytes);

    auto active_rows = std::make_shared<std::vector<fs::DirEntry>>(1);
    (*active_rows)[0].name = L"still-visible.txt";
    previous = active_rows;
    pane.view.SetSnapshot(active_rows);
    std::weak_ptr<ui::RowPresentationCache> old_cache;
    {
        ui::PaneViewModel view;
        app::FillPaneViewModel(view, pane);
        old_cache = view.row_cache;
        active_rows.reset();
        pane.view.SetSnapshot(nullptr);
        check(!previous.expired() && view.snapshot && view.snapshot->at(0).name == L"still-visible.txt" &&
            view.row_cache && view.row_cache->snapshot == view.snapshot,
            "active view keeps its snapshot and row cache valid across replacement");
        ui::PaneViewModel replacement;
        app::FillPaneViewModel(replacement, pane);
        check(replacement.row_cache != view.row_cache && !replacement.row_cache->snapshot,
            "new view gets a distinct cache for the replacement snapshot");
    }
    check(previous.expired() && old_cache.expired(), "old snapshot and row cache release after active view ends");
    return failures ? 1 : 0;
}
int CheckDerivedCaches() {
    int failures = 0;
    auto check = [&](bool good, const char* label) {
        std::printf("[%s] %s\n", good ? "PASS" : "FAIL", label); failures += !good;
    };
    app::Pane pane;
    auto& tab = pane.view;
    tab.current_path = L"C:\\isolated";
    tab.group_by = static_cast<int>(app::GroupBy::Name);
    tab.filter_text = L".txt";
    auto rows = std::make_shared<std::vector<fs::DirEntry>>(20000);
    for (auto& row : *rows) row.name = L"alpha-long-file-name-for-lifetime.txt";
    std::weak_ptr<const std::vector<fs::DirEntry>> old_snapshot = rows;
    tab.SetSnapshot(rows);
    ui::PaneViewModel active;
    app::FillPaneViewModel(active, pane);
    std::weak_ptr<const ui::ListGroups> old_groups = active.groups;
    std::weak_ptr<const ui::PaneViewModel::FilterMap> old_filter = active.filter_map;
    rows.reset();
    tab.group_by = 0;
    tab.SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>());
    check(!old_snapshot.expired() && active.groups && active.groups->front().count == 20000 &&
        active.snapshot->size() == 20000, "active grouped view survives snapshot replacement");
    active = {};
    check(old_snapshot.expired() && old_groups.expired() && old_filter.expired(),
        "hidden tab releases old grouped snapshot, spans and filter without refill");
    rows = std::make_shared<std::vector<fs::DirEntry>>(3);
    (*rows)[0].name=L"a.txt"; (*rows)[1].name=L"b.txt"; (*rows)[2].name=L"c.txt";
    tab.SetSnapshot(rows);
    tab.compare_marks = std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{1,0,2});
    tab.compare_diff_only = true;
    app::FillPaneViewModel(active, pane);
    check(active.filter_map && *active.filter_map == std::vector<int>({0,2}), "compare intersects current name filter");
    std::weak_ptr<const std::vector<uint8_t>> marks = tab.view_compare_marks;
    old_filter = tab.view_compare_map;
    auto base = std::weak_ptr<const ui::PaneViewModel::FilterMap>(tab.view_compare_base);
    tab.compare_marks.reset();
    tab.SetSnapshot(nullptr);
    check(active.compare_marks && active.compare_marks->size()==3 && active.filter_map->size()==2,
        "active compare view keeps its maps after invalidation");
    active = {};
    check(marks.expired() && old_filter.expired() && base.expired(), "inactive compare derived arrays release without refill");

    auto catalog = std::make_shared<app::TagCatalogSnapshot>();
    catalog->revision = 87331;
    catalog->tags = {{L"first",0x123456}, {L"second",0x654321}};
    catalog->paths = {{L"c:\\isolated\\a.txt",0},{L"c:\\isolated\\b.jpg",1}};
    const auto previous_catalog = app::CurrentTagCatalog();
    app::PublishTagCatalog(catalog);
    std::vector<fs::DirEntry> source(6);
    const wchar_t* names[] = {L"a.txt",L"b.jpg",L"c.txt",L"z.txt",L"9.bin",L"folder"};
    for(size_t i=0;i<source.size();++i) {
        source[i].name=names[i]; source[i].size=i*100000;
        source[i].full_path=(i%2 ? L"C:\\one\\" : L"C:\\two\\")+source[i].name;
    }
    const auto date_clock=app::MakeGroupClock();
    const auto file_time=[](uint64_t ticks) { return FILETIME{static_cast<DWORD>(ticks),static_cast<DWORD>(ticks>>32)}; };
    for(size_t i=0;i<source.size();++i) {
        source[i].mtime=file_time(i%2 ? date_clock.today : date_clock.yesterday);
        source[i].ctime=file_time(i%2 ? date_clock.yesterday : date_clock.today);
        source[i].atime=file_time(i%3 ? date_clock.today : 0);
    }
    source.back().is_dir=true;
    tab.filter_text.clear();
    bool equivalent=true, cached=true, folded=true;
    for(const auto by : {app::GroupBy::Name,app::GroupBy::Date,app::GroupBy::Type,app::GroupBy::Size,app::GroupBy::Tag,app::GroupBy::Location}) {
        for(const auto direction : {ui::SortDirection::Asc,ui::SortDirection::Desc}) {
            for(const auto column : {ui::SortColumn::Name,ui::SortColumn::Mtime,ui::SortColumn::Created,ui::SortColumn::Accessed}) {
                auto sorted=std::make_shared<std::vector<fs::DirEntry>>(source);
                auto clock=by==app::GroupBy::Date ? app::MakeGroupClock(column) : app::GroupClock{};
                if(by==app::GroupBy::Tag) clock.tags=app::TagGroupsForFolder(tab.current_path);
                std::stable_sort(sorted->begin(),sorted->end(),[&](const auto& a,const auto& b) {
                    return app::GroupCompare(a,b,by,clock,column,direction)<0;
                });
                tab.group_by=static_cast<int>(by); tab.sort_column=column; tab.sort_direction=direction;
                tab.SetSnapshot(sorted);
                for(const auto* filter : {L"",L".txt"}) {
                    tab.filter_text=filter;
                    app::FillPaneViewModel(active,pane);
                    std::vector<int> expected;
                    app::CollectFilterMatches(tab,nullptr,expected);
                    size_t group=0;
                    int previous=-1;
                    for(size_t r=0;r<expected.size();++r) {
                        const int at=expected[r];
                        if(previous<0 || app::GroupCompare((*sorted)[previous],(*sorted)[at],by,clock,column,direction)!=0) {
                            equivalent=equivalent && active.groups && group<active.groups->size();
                            if(active.groups && group<active.groups->size()) {
                                if(group>0) {
                                    const auto& previous_span=(*active.groups)[group-1];
                                    equivalent=equivalent && previous_span.count==static_cast<int>(r)-previous_span.first;
                                }
                                const auto& span=(*active.groups)[group++];
                                equivalent=equivalent && span.first==static_cast<int>(r) && span.key==app::GroupKey((*sorted)[at],by,clock);
                            }
                        }
                        previous=at;
                    }
                    equivalent=equivalent && active.groups && active.groups->size()==group;
                    if(active.groups && !active.groups->empty()) {
                        const auto& last=active.groups->back();
                        equivalent=equivalent && last.count==static_cast<int>(expected.size())-last.first;
                    }
                    const auto held=active.groups;
                    app::FillPaneViewModel(active,pane);
                    cached=cached && active.groups==held;
                    if(held && !held->empty()) {
                        tab.collapsed_groups[tab.current_path].insert(held->front().key); ++tab.group_collapse_rev;
                        app::FillPaneViewModel(active,pane);
                        folded=folded && active.groups!=held && active.groups->front().collapsed;
                        tab.collapsed_groups.clear(); ++tab.group_collapse_rev;
                    }
                }
            }
        }
    }
    check(equivalent,"group ranks preserve original comparison across modes, directions, columns and filtered rows");
    check(cached,"unchanged grouped view reuses shared spans");
    check(folded,"collapse revision rebuilds correct group state");
    tab.group_by=static_cast<int>(app::GroupBy::Name); tab.filter_text.clear();
    rows=std::make_shared<std::vector<fs::DirEntry>>(3);
    (*rows)[0].name=L"a.txt"; (*rows)[1].name=L"z.txt"; (*rows)[2].name=L"b.txt";
    tab.SetSnapshot(rows); app::FillPaneViewModel(active,pane);
    check(active.groups && active.groups->empty(),"noncontiguous groups retain flat fallback");
    app::PublishTagCatalog(previous_catalog);
    return failures ? 1 : 0;
}
int BenchGroups() {
    constexpr size_t count=20000;
    auto rows=std::make_shared<std::vector<fs::DirEntry>>(count);
    auto tags=std::make_shared<app::TagCatalogSnapshot>();
    tags->revision=87332; tags->tags={{L"first",1},{L"second",2}};
    app::Pane pane; pane.view.current_path=L"C:\\group-bench";
    pane.view.group_by=static_cast<int>(app::GroupBy::Tag);
    for(size_t i=0;i<count;++i) {
        (*rows)[i].name=L"document-with-a-long-name-"+std::to_wstring(i)+L".txt";
        tags->paths.push_back({L"c:\\group-bench\\"+(*rows)[i].name,static_cast<int>(i/(count/2))});
    }
    const auto previous=app::CurrentTagCatalog(); app::PublishTagCatalog(tags);
    app::GroupClock clock; clock.tags=app::TagGroupsForFolder(pane.view.current_path);
    const auto now=[] { return std::chrono::steady_clock::now(); };
    const auto ms=[](auto a,auto b) { return std::chrono::duration<double,std::milli>(b-a).count(); };
    size_t old_checksum=0,new_checksum=0;
    std::vector<double> old_times,new_times;
    for(int run=0;run<12;++run) {
        for(int order=0;order<2;++order) {
            const bool cached=(run+order)%2!=0;
            size_t boundaries=0;
            const auto start=now();
            int previous_rank=cached ? app::GroupRank((*rows)[0],app::GroupBy::Tag,clock) : 0;
            for(size_t i=1;i<count;++i) {
                if(cached) {
                    const int rank=app::GroupRank((*rows)[i],app::GroupBy::Tag,clock);
                    boundaries+=app::GroupCompareRanks((*rows)[i-1],(*rows)[i],app::GroupBy::Tag,
                        ui::SortColumn::Name,ui::SortDirection::Asc,previous_rank,rank)!=0;
                    previous_rank=rank;
                } else boundaries+=app::GroupCompare((*rows)[i-1],(*rows)[i],app::GroupBy::Tag,clock,
                    ui::SortColumn::Name,ui::SortDirection::Asc)!=0;
            }
            const double elapsed=ms(start,now());
            (cached?new_times:old_times).push_back(elapsed);
            (cached?new_checksum:old_checksum)+=boundaries;
        }
    }
    std::sort(old_times.begin(),old_times.end());std::sort(new_times.begin(),new_times.end());
    pane.view.SetSnapshot(rows); ui::PaneViewModel view;
    auto start=now(); app::FillPaneViewModel(view,pane); const double first=ms(start,now());
    const auto spans=view.groups; bool cache_same=true;
    start=now();
    for(int i=0;i<1000;++i) { app::FillPaneViewModel(view,pane); cache_same=cache_same && view.groups==spans; }
    const double repeated=ms(start,now())/1000;
    std::printf("group_tag entries=%zu original_adjacent_median_ms=%.3f cached_rank_median_ms=%.3f checksums=%zu/%zu first_view_ms=%.3f repeat_view_ms=%.6f cache_same=%d\n",
        count,old_times[6],new_times[6],old_checksum,new_checksum,first,repeated,cache_same?1:0);
    app::PublishTagCatalog(previous);
    return old_checksum==12 && new_checksum==old_checksum && cache_same ? 0 : 1;
}

struct FilterMemory {
    size_t private_resident = 0;
    size_t private_commit = 0;
    bool valid = false;
};
FilterMemory ReadFilterMemory() {
    FilterMemory result;
    // Allocate the query buffer before the measurement; do not trim working sets.
    std::vector<unsigned char> storage(16 * 1024 * 1024);
    auto* info = reinterpret_cast<PSAPI_WORKING_SET_INFORMATION*>(storage.data());
    if (!QueryWorkingSet(GetCurrentProcess(), info, static_cast<DWORD>(storage.size()))) return result;
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    for (ULONG_PTR i = 0; i < info->NumberOfEntries; ++i)
        if (!info->WorkingSetInfo[i].Shared) result.private_resident += system.dwPageSize;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) return result;
    result.private_commit = counters.PrivateUsage;
    result.valid = true;
    return result;
}
int BenchFilterCapacity() {
    constexpr size_t count = 200000;
    constexpr size_t tabs = 16;
    auto rows = std::make_shared<std::vector<fs::DirEntry>>(count);
    for (size_t i = 0; i < count; ++i)
        (*rows)[i].name = (i == 0 ? L"rare_" : i % 2 ? L"odd_" : L"even_") + std::to_wstring(i) + L".txt";
    app::Pane pane;
    pane.view.show_hidden_files = true;
    pane.view.show_protected_os_files = true;
    pane.view.SetSnapshot(rows);
    const wchar_t* queries[] = {L"missing_", L"rare_", L"odd_", L".txt", L"even_", L".txt"};
    const size_t expected[] = {0, 1, count/2, count, count/2-1, count};
    int failures = 0;
    size_t checksum = 0;
    std::vector<double> totals;
    for (int run = 0; run < 9; ++run) {
        const auto begin = std::chrono::steady_clock::now();
        for (size_t q = 0; q < std::size(queries); ++q) {
            pane.view.filter_text = queries[q];
            const auto start = std::chrono::steady_clock::now();
            ui::PaneViewModel view;
            app::FillPaneViewModel(view, pane);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
            if (!view.filter_map || view.filter_map->size() != expected[q]) ++failures;
            if (view.filter_map) {
                checksum += view.filter_map->size();
                if (run > 0) std::printf("filter q=%zu run=%d ms=%.6f size=%zu capacity=%zu\n",
                    q, run, ms, view.filter_map->size(), view.filter_map->capacity());
            }
        }
        if (run > 0) totals.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now()-begin).count());
    }
    std::sort(totals.begin(),totals.end());
    std::printf("six_filters_median_ms=%.6f checksum=%zu failures=%d\n",totals[totals.size()/2],checksum,failures);
    // Warm both memory sampling and filter paths before keeping independent tab maps.
    ReadFilterMemory();
    const auto before = ReadFilterMemory();
    std::vector<std::unique_ptr<app::Pane>> held;
    size_t capacities = 0;
    for (size_t i=0;i<tabs;++i) {
        auto tab=std::make_unique<app::Pane>();
        tab->view.show_hidden_files=true; tab->view.show_protected_os_files=true;
        tab->view.SetSnapshot(rows); tab->view.filter_text=L"rare_";
        ui::PaneViewModel view; app::FillPaneViewModel(view,*tab);
        if (!view.filter_map || *view.filter_map != std::vector<int>{0}) ++failures;
        capacities += view.filter_map ? view.filter_map->capacity() : 0;
        held.push_back(std::move(tab));
    }
    const auto after=ReadFilterMemory();
    std::printf("held_tabs=%zu capacity_payload_bytes=%zu memory_valid=%d private_resident_delta=%lld private_commit_delta=%lld\n",
        tabs, capacities*sizeof(int),before.valid&&after.valid?1:0,
        static_cast<long long>(after.private_resident)-static_cast<long long>(before.private_resident),
        static_cast<long long>(after.private_commit)-static_cast<long long>(before.private_commit));
    return failures?1:0;
}


int CheckFilterCapacity() {
    app::Tab tab;
    tab.show_hidden_files = true; tab.show_protected_os_files = true;
    auto rows = std::make_shared<std::vector<fs::DirEntry>>(20000);
    for (size_t i=0;i<rows->size();++i) (*rows)[i].name=i==0?L"rare.txt":i%2?L"odd.txt":L"even.txt";
    tab.SetSnapshot(rows);
    int failures=0;
    const auto check=[&](bool good,const char* label) {
        std::printf("[%s] %s\n",good?"PASS":"FAIL",label); failures+=!good;
    };
    std::vector<int> result;
    tab.filter_text=L"missing"; app::CollectFilterMatches(tab,nullptr,result);
    check(result.empty() && result.capacity()==0,"zero-match large filter releases payload");
    tab.filter_text=L"rare"; app::CollectFilterMatches(tab,nullptr,result);
    check(result==std::vector<int>{0} && result.capacity()<4096,"sparse filter retains only small result payload");
    tab.filter_text=L".txt"; app::CollectFilterMatches(tab,nullptr,result);
    check(result.size()==20000 && result.capacity()==20000,"dense filter preserves original full reservation");
    tab.filter_text=L"odd"; app::CollectFilterMatches(tab,nullptr,result);
    check(result.size()==10000 && result.capacity()==20000,"half-match filter avoids compaction copy");
    tab.filter_text=L"rare"; app::CollectFilterMatches(tab,nullptr,result);
    check(result==std::vector<int>{0} && result.capacity()<4096,"reused dense buffer releases after sparse query");
    return failures?1:0;
}

int CheckFilters() {
    int failures=0;
    auto check=[&](bool good,const char* label) { std::printf("[%s] %s\n",good?"PASS":"FAIL",label); failures+=!good; };
    std::mt19937 random(7321);
    const std::wstring alphabet=L"aAbB01 _.-\u6587\u4ef6\u00c9\u00e9\u0130\u03a3\u03c3";
    bool same=true;
    for(int trial=0;trial<20000;++trial) {
        std::wstring name,pattern;
        for(unsigned i=0,n=random()%60;i<n;++i) name+=alphabet[random()%alphabet.size()];
        for(unsigned i=0,n=random()%12;i<n;++i) pattern+=random()%5==0 ? (random()%2?L'*':L'?') : alphabet[random()%alphabet.size()];
        same=same && app::NamePattern(pattern).Matches(name)==ReferenceMatch(name,pattern);
    }
    check(same,"20000 case/unicode/substring/wildcard comparisons match previous semantics");
    app::Tab empty;
    std::vector<int> empty_matches{1};
    app::CollectFilterMatches(empty,nullptr,empty_matches);
    check(empty_matches.empty(),"null snapshot clears filter results");
    empty.SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>());
    empty.filter_text=L"x";
    app::CollectFilterMatches(empty,nullptr,empty_matches);
    check(empty_matches.empty(),"empty snapshot has no rows to dereference");
    app::Pane pane; auto& tab=pane.view;
    tab.current_path=L"C:/isolated";
    auto rows=std::make_shared<std::vector<fs::DirEntry>>(4);
    (*rows)[0].name=L"Report.JPG"; (*rows)[1].name=L"hidden.jpg"; (*rows)[1].attrs=FILE_ATTRIBUTE_HIDDEN;
    (*rows)[2].name=L"notes.txt"; (*rows)[3].name=std::wstring(600,L'x')+L".jpg";
    tab.SetSnapshot(rows); tab.filter_text=L"*.jpg";
    std::vector<int> matches;
    app::CollectFilterMatches(tab,nullptr,matches);
    check(matches==std::vector<int>({0,3}),"hidden attributes and long names preserve visible matches");
    app::PlacesCatalog tags; tags.persist=false;
    app::ColorTag red; red.id=L"isolated-red"; red.name=L"Red"; tags.tags.push_back(red);
    std::vector<app::TagAdsUpdate> deferred;
    tags.SetTaggedBatch(0,{L"C:/isolated/Report.JPG"},true,&deferred);
    (*rows)[0].full_path=L"C:/isolated/Report.JPG";
    tab.filter_text=L"*.jpg #red"; app::CollectFilterMatches(tab,&tags,matches);
    check(matches==std::vector<int>({0}),"prepared names retain combined tag filtering");
    app::AliasStore::Instance().Set((*rows)[0].full_path,L"Vacation");
    tab.filter_text=L"vacation"; app::CollectFilterMatches(tab,nullptr,matches);
    check(matches==std::vector<int>({0}),"aliases remain a fallback when the name does not match");
    app::AliasStore::Instance().Set((*rows)[0].full_path,L"");
    const auto content=app::ContentFilter(L"*.jpg",tags);
    check(content((*rows)[0]) && !content((*rows)[2]),"content filter uses identical prepared name semantics");
    tab.filter_text=L"*.jpg";
    tab.tree=std::make_shared<app::DetailsTree>(); tab.tree->base=rows; tab.tree->base_size=rows->size();
    auto combined=std::make_shared<std::vector<fs::DirEntry>>(*rows);
    fs::DirEntry child; child.name=L"child.JPG"; combined->push_back(child); tab.tree->combined=combined;
    app::CollectFilterMatches(tab,nullptr,matches);
    check(matches==std::vector<int>({0,3,4}),"expanded child rows are filtered from the current tree");
    auto replaced=std::make_shared<std::vector<fs::DirEntry>>(1); (*replaced)[0].name=L"new.txt";
    tab.SetSnapshot(replaced); app::CollectFilterMatches(tab,nullptr,matches);
    check(matches.empty(),"snapshot switch does not reuse stale tree or filter rows");
    tab.filter_text=L"NEW"; app::CollectFilterMatches(tab,nullptr,matches);
    check(matches==std::vector<int>({0}),"rapid filter replacement uses current snapshot");
    ui::PaneViewModel model; app::FillPaneViewModel(model,pane); const auto cached=model.filter_map;
    app::FillPaneViewModel(model,pane);
    check(cached==model.filter_map,"unchanged filter reuses its map");
    tab.content_action_ready=true; tab.all_selected=true; tab.content_action_count=9;
    app::CollectFilterMatches(tab,nullptr,matches);
    check(matches==std::vector<int>({0}),"stale action counts cannot read beyond snapshot rows");
    tab.content_results=std::make_shared<index::ContentResultStore>(nullptr,0);
    app::CollectFilterMatches(tab,nullptr,matches);
    check(matches.size()==9,"content results retain their action-row branch");
    tab.content_results.reset();
    return failures?1:0;
}

int CheckPinyinFilters() {
    int failures = 0;
    app::Pane pane;
    auto rows = std::make_shared<std::vector<fs::DirEntry>>(2);
    (*rows)[0].name = L"中文文件.txt";
    (*rows)[1].name = L"other.txt";
    pane.view.SetSnapshot(rows);
    app::PlacesCatalog places;
    places.persist = false;
    for (const auto* query : {L"zhongwen", L"ZW", L"wenjian", L"wj"}) {
        pane.view.filter_text = query;
        std::vector<int> matches;
        app::CollectFilterMatches(pane.view, nullptr, matches);
        ui::PaneViewModel model;
        app::FillPaneViewModel(model, pane, nullptr);
        const bool good = matches == std::vector<int>{0} && model.EntryCount() == 1 &&
            app::ContentFilter(query, places)((*rows)[0]);
        std::printf("[%s] pinyin filter and selection agree\n", good ? "PASS" : "FAIL");
        failures += !good;
    }
    const bool wildcard = !app::NamePattern(L"zhong*", true).Matches((*rows)[0].name) &&
        app::NamePattern(L"*.txt", true).Matches((*rows)[0].name);
    std::printf("[%s] wildcard remains literal\n", wildcard ? "PASS" : "FAIL");
    return failures + !wildcard;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    if (std::wstring_view(argv[1]) == L"--check-pinyin") return CheckPinyinFilters();
    if (std::wstring_view(argv[1]) == L"--check") return CheckFilters() | CheckSnapshotLifetime() | CheckDerivedCaches() | CheckFilterCapacity();
    if (std::wstring_view(argv[1]) == L"--filter-capacity") return BenchFilterCapacity();
    if (std::wstring_view(argv[1]) == L"--group-bench") return BenchGroups();
    const std::wstring path = fs::NormalizePath(argv[1]);
    app::PlacesCatalog places; places.persist = false;
    const auto now = [] { return std::chrono::steady_clock::now(); };
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::vector<double> enumeration, sort, apply, view, cached, filtering;
    size_t count = 0, checksum = 0;
    for (int run = 0; run < 6; ++run) {
        auto rows = std::make_shared<std::vector<fs::DirEntry>>();
        const auto t0 = now();
        fs::EnumerateDirectory(path, *rows);
        const auto t1 = now();
        app::SortEntries(*rows, ui::SortColumn::Name, ui::SortDirection::Asc);
        const auto t2 = now();
        app::Pane pane; pane.view.current_path = path;
        pane.view.SetSnapshot(rows);
        const auto t3 = now();
        ui::PaneViewModel model;
        app::FillPaneViewModel(model, pane, &places);
        const auto t4 = now();
        fs::SnapshotStore store;
        store.Update(path, 1, rows);
        uint64_t generation = 0;
        app::Pane reopened; reopened.view.current_path = path;
        reopened.view.SetSnapshot(store.GetOrStart(path, generation));
        app::FillPaneViewModel(model, reopened, &places);
        const auto t5 = now();
        for (const auto* query : {L"document", L"document_1", L"document_19", L"*.jpg", L"*19??*", L"absent"}) {
            pane.view.filter_text = query;
            app::FillPaneViewModel(model, pane, &places);
            checksum += model.EntryCount();
        }
        const auto t6 = now();
        count = rows->size();
        if (run) {
            enumeration.push_back(ms(t0,t1)); sort.push_back(ms(t1,t2)); apply.push_back(ms(t2,t3));
            view.push_back(ms(t3,t4)); cached.push_back(ms(t4,t5)); filtering.push_back(ms(t5,t6));
        }
    }
    auto report = [](const char* stage, std::vector<double>& times) {
        std::sort(times.begin(),times.end());
        std::printf("%s median_ms=%.3f min_ms=%.3f max_ms=%.3f\n",stage,times[2],times.front(),times.back());
    };
    std::printf("entries=%zu checksum=%zu\n",count,checksum);
    report("enumerate",enumeration); report("sort",sort); report("apply_snapshot",apply);
    report("first_view",view); report("cache_reopen",cached); report("six_filters",filtering);
    return count == 20000 && checksum == 255468 ? 0 : 1;
}

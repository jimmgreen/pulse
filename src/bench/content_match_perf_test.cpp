#include "../index/content_search.h"
#include "../index/content_index.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>

namespace {
thread_local bool count_allocations = false;
thread_local size_t allocation_count = 0, allocation_bytes = 0;
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
    failures += !ok;
}
}
void* operator new(size_t size) {
    if (void* p = std::malloc(size ? size : 1)) {
        if (count_allocations) { ++allocation_count; allocation_bytes += size; }
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

int RunContentMatchPerf() {
    using namespace pulse::index;
    const std::wstring text = L"alpha_long_search_token beta_long_search_token gamma_long_search_token";
    ContentSearchRequest request;
    request.needles = {L"alpha_long_search_token", L"beta_long_search_token", L"gamma_long_search_token"};
    const auto benchmark = [&](const char* label) {
        allocation_count = allocation_bytes = 0;
        const auto begin = std::chrono::steady_clock::now();
        count_allocations = true;
        size_t sum = 0;
        for (int i = 0; i < 10000; ++i) sum += MatchCachedContent(text, request);
        count_allocations = false;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-begin).count();
        std::cout << "PERF " << label << " ms=" << ms << " allocations=" << allocation_count << " requested_bytes=" << allocation_bytes << '\n';
        Check(sum == 0, "matching benchmark preserves first offset");
    };
    benchmark("words");
    request.needles.clear(); request.needle = text;
    benchmark("split_words");
    request.match_mode = ContentMatchMode::Phrase;
    benchmark("phrase");
    request.needle.clear(); request.needles = {L"alpha_long_search_token", L"beta_long_search_token", L"gamma_long_search_token"};
    Check(MatchCachedContent(text, request) == 0, "phrase joins explicit words with spaces");
    request = {}; request.needle = L" \t\n";
    Check(MatchCachedContent(L"anything", request) == 0, "whitespace-only query keeps match-all behavior");
    request.needles = {L""};
    Check(MatchCachedContent(L"anything", request) == std::wstring::npos, "explicit empty word remains non-matching");
    request.match_mode = ContentMatchMode::AnyWord; request.needles = {L"", L"beta"};
    Check(MatchCachedContent(L"beta", request) == 0, "any-word skips explicit empty words");
    request = {}; request.needle = L"  beta\talpha\n";
    Check(MatchCachedContent(L"Alpha BETA", request) == 0, "split whitespace and case-insensitive all words retain earliest offset");
    request.case_sensitive = true;
    Check(MatchCachedContent(L"Alpha BETA", request) == std::wstring::npos, "case-sensitive mismatch retained");
    request = {}; request.needles = {L"missing", L"beta"}; request.match_mode = ContentMatchMode::AnyWord;
    Check(MatchCachedContent(L"alpha beta", request) == 6, "any-word returns earliest existing word");
    request = {}; request.needle = L"beta"; request.whole_word = true;
    Check(MatchCachedContent(L"betamax beta", request) == 8, "whole-word skips prefix matches");
    request.excluded_needles = {L"betamax"};
    Check(MatchCachedContent(L"betamax beta", request) == std::wstring::npos, "excluded word takes precedence");
    request = {}; request.needle.assign(1, static_cast<wchar_t>(towlower(L'\u00c4')));
    Check(MatchCachedContent(L"\u00c4", request) == 0, "non-ASCII case folding follows active CRT locale");
    request.needle = L"\u4e2d\u6587";
    Check(MatchCachedContent(L"x \u4e2d\u6587 y", request) == 2, "Unicode exact text offset retained");
    request = {}; request.needle = L"needle";
    std::wstring boundary(16382, L'x'); boundary += L"needle";
    std::atomic<bool> cancelled{false};
    Check(MatchCachedContent(boundary, request, &cancelled) == 16382, "slice-boundary match retains overlap");
    cancelled = true;
    Check(MatchCachedContent(boundary, request, &cancelled) == std::wstring::npos, "cancelled content scan stops");
    const std::wstring long_path = L"C:\\folder\\a_very_long_filename_for_allocation_probe.txt";
    const auto empty_query = ParseQuery(L"");
    allocation_count = allocation_bytes = 0;
    count_allocations = true;
    bool names = true;
    for (int i=0;i<10000;++i) names &= MatchContentFilename(long_path, 1, 0, empty_query);
    count_allocations = false;
    std::cout << "PERF empty_filename allocations=" << allocation_count << " requested_bytes=" << allocation_bytes << '\n';
    Check(names, "empty filename query accepts candidates");

    namespace fs = std::filesystem;
    const auto parent = fs::absolute(L"bench_data"); fs::create_directories(parent);
    const auto root = parent/(L"content-match-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(root.c_str(), nullptr)) return 2;
    request = {}; request.needle = L"needle";
    for (int i=0;i<512;++i) {
        const auto path = root/(L"long_candidate_filename_"+std::to_wstring(511-i)+(i%2 ? L".txt" : L".log"));
        std::ofstream out(path, std::ios::binary); out << "first\nNeedle body\n"; out.close();
        if (!out) { Check(false,"write exclusive corpus"); fs::remove_all(root); return 1; }
        request.candidate_paths.push_back(path.wstring());
    }
    for (auto sort : {ContentResultSort::Name, ContentResultSort::Path, ContentResultSort::Type}) {
        for (bool descending : {false,true}) {
            request.sort=sort; request.sort_desc=descending;
            auto expected=request.candidate_paths;
            std::sort(expected.begin(),expected.end(),[&](const auto& a,const auto& b) {
                const auto key=[&](const auto& path) {
                    if(sort==ContentResultSort::Path) return path;
                    const auto name=fs::path(path).filename().wstring();
                    return sort==ContentResultSort::Type ? name.substr(name.find_last_of(L'.')+1) : name;
                };
                const auto x=key(a),y=key(b);
                int cmp=CompareStringOrdinal(x.c_str(),-1,y.c_str(),-1,TRUE)-CSTR_EQUAL;
                if(!cmp) cmp=CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)-CSTR_EQUAL;
                return descending ? cmp>0 : cmp<0;
            });
            std::vector<std::wstring> actual;
            ContentSearchProgress final;
            cancelled=false;
            allocation_count=allocation_bytes=0;
            const auto begin=std::chrono::steady_clock::now();
            count_allocations=true;
            const bool ok=RunContentSearch(request,cancelled,[&](const auto& progress,auto batch) {
                final=progress;
                for(const auto& hit:batch) actual.push_back(hit.path);
                return true;
            });
            count_allocations=false;
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            std::cout << "CORPUS sort=" << static_cast<int>(sort) << " desc=" << descending << " ms=" << ms
                << " allocations=" << allocation_count << " requested_bytes=" << allocation_bytes << '\n';
            Check(ok && final.done && final.error==0 && actual==expected,"real corpus results and ordinal sort match reference");
        }
    }
    cancelled=false;
    ContentSearchProgress final;
    const bool completed=RunContentSearch(request,cancelled,[&](const auto& progress,auto) {
        final=progress; if(progress.scanned_files>0) cancelled=true; return true;
    });
    Check(!completed && final.done && final.error==ERROR_CANCELLED,"real corpus cancellation reports terminal cancelled status");
    fs::remove_all(root);
    return failures ? 1 : 0;
}

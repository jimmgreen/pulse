#ifndef PREVIEW_IDENTITY_BASELINE
#define PREVIEW_IDENTITY_NEW
#endif
#include "../ui/table_view.h"
#include "../ui/markdown_view.h"
#include "../ui/tree_view.h"
#include "../ui/archive_preview.h"
#include <chrono>
#include <cstdio>
#include <memory>
#include <algorithm>
#include <array>
#include <cstring>

static std::wstring TablePayload() {
    std::wstring p = L"PULSETBL\t1\nS\tcsv\tCSV\t20\t5000\t0\tcomma\t0\nX\tsource\n";
    const std::wstring cell(32, L'v');
    for (int r = 0; r < 5000; ++r) { p += L"R"; for(int c=0;c<20;++c) p += L"\t" + cell; p += L'\n'; }
    return p;
}
static std::wstring MarkdownPayload() {
    std::wstring p = L"PULSEMD\t1\nS\tsource\n";
    for(int i=0;i<10000;++i) p += L"B\tp\t\t0\t0\t\t" + std::wstring(256,L'm') + L"\t\n";
    return p;
}
#ifdef PREVIEW_IDENTITY_NEW
#include "../ui/preview_text_identity.h"
#endif

template<class V, class Load> bool Bench(const char* name, V& view, Load load, std::wstring body) {
    const auto shared = std::make_shared<const std::wstring>(std::move(body));
    if (!load(view, *shared)) return false;
    std::wstring preview = view.PlainText();
    size_t checksum = 0;
    std::array<double,10> before{};
#ifdef PREVIEW_IDENTITY_NEW
    std::array<double,10> after{};
    V candidate;
    if (!load(candidate, shared)) return false;
    std::wstring candidate_preview = candidate.PlainText();
    pulse::ui::PreviewTextIdentity identity;
    identity.Changed(&candidate, candidate.TextRevision(), candidate.PlainText(), candidate_preview);
#endif
    const auto measure = [&](bool use_identity) {
        const auto t = std::chrono::steady_clock::now();
        for (int frame=0;frame<100;++frame) {
#ifdef PREVIEW_IDENTITY_NEW
            if(use_identity) {
                if(!load(candidate,shared)) return -1.0;
                if(identity.Changed(&candidate,candidate.TextRevision(),candidate.PlainText(),candidate_preview)) candidate_preview=candidate.PlainText();
            } else
#else
            (void)use_identity;
#endif
            { if(!load(view,*shared)) return -1.0; if(preview!=view.PlainText()) preview=view.PlainText(); }
#ifdef PREVIEW_IDENTITY_NEW
            checksum += use_identity ? candidate_preview.size() : preview.size();
#else
            checksum += preview.size();
#endif
        }
        return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
    };
    for(size_t i=0;i<10;++i) {
#ifdef PREVIEW_IDENTITY_NEW
        if(i%2) { after[i]=measure(true); before[i]=measure(false); }
        else { before[i]=measure(false); after[i]=measure(true); }
        std::printf("sample %s round=%zu frames=100 legacy_ms=%.6f identity_ms=%.6f\n",name,i,before[i],after[i]);
#else
        before[i]=measure(false); std::printf("baseline %s round=%zu frames=100 ms=%.6f\n",name,i,before[i]);
#endif
    }
    std::sort(before.begin(),before.end());
    std::printf("median %s chars=%zu plain=%zu legacy_ms=%.6f checksum=%zu\n",name,shared->size(),preview.size(),(before[4]+before[5])/2,checksum);
#ifdef PREVIEW_IDENTITY_NEW
    std::sort(after.begin(),after.end()); std::printf("median %s identity_ms=%.6f\n",name,(after[4]+after[5])/2);
#endif
    return true;
}

#ifdef PREVIEW_IDENTITY_NEW
namespace pulse::ui {
struct PreviewBufferLifetimeTest {
    static bool Correctness() {
        bool ok = true;
        const auto check=[&](bool b,const char* name){std::printf("[%s] %s\n",b?"PASS":"FAIL",name);ok&=b;};
        const auto owned=[](std::wstring s){return std::make_shared<const std::wstring>(std::move(s));};
        TableView t; MarkdownView m; TreeView tree; ArchivePreview archive;
        auto table=owned(L"PULSETBL\t1\nS\tcsv\tFirst\t1\t1\t0\tcomma\t0\nR\talpha\nS\tcsv\tSecond\t1\t1\t0\tcomma\t0\nR\tbeta\nX\traw\n");
        check(t.SetPayload(table)&&t.PlainText()==L"alpha\n"&&t.Source()==L"raw","shared table loads searchable and source text");
        const auto r=t.TextRevision();
        check(t.SetPayload(table)&&t.TextRevision()==r,"same identity leaves body revision unchanged");
        PreviewTextIdentity sync; std::wstring preview;
        check(sync.Changed(&t,r,t.PlainText(),preview),"initial text synchronizes");preview=t.PlainText();
        check(!sync.Changed(&t,r,t.PlainText(),preview),"unchanged revision skips body comparison");
        t.SwitchSheet(1);
        check(t.SetPayload(table)&&t.TextRevision()!=r&&t.PlainText()==L"beta\n"&&sync.Changed(&t,t.TextRevision(),t.PlainText(),preview),"sheet switch keeps selected sheet and updates body revision");
        preview=t.PlainText();
        sync.Reset(); preview=t.Source();
        check(sync.Changed(&t,t.TextRevision(),t.PlainText(),preview),"source mode reset restores rendered body");
        auto md=owned(L"PULSEMD\t1\nS\traw\nB\tp\t\t0\t0\t\tfirst\t\n");
        check(m.SetPayload(md,L"same.md")&&m.PlainText()==L"first","shared markdown loads");
        auto next=owned(L"PULSEMD\t1\nS\tnew raw\nB\tp\t\t0\t0\t\tsecond\t\n");
        const auto mr=m.TextRevision();
        check(m.SetPayload(next,L"same.md")&&m.PlainText()==L"second"&&m.Source()==L"new raw"&&m.TextRevision()!=mr,"same path new payload refreshes markdown and source");
        check(m.SetPayload(next,L"other.md")&&m.path_==L"other.md","same body new markdown path is accepted separately");
        const auto body=m.PlainText(); const auto repeated=m.TextRevision();
        check(m.SetPayload(next,L"other.md")&&m.TextRevision()==repeated&&m.PlainText()==body,"markdown repeated shared identity is stable");
        std::wstring mutable_body=*next; m.SetPayload(mutable_body,L"other.md");
        mutable_body.replace(mutable_body.find(L"second"),6,L"legacy");
        check(m.SetPayload(mutable_body,L"other.md")&&m.PlainText()==L"legacy","legacy mutable string cannot reuse immutable identity");
        check(m.SetPayload(next,L"other.md")&&m.PlainText()==L"second","return from legacy input revalidates original shared body");
        auto tr=owned(L"PULSETREE\t1\nH\tjson\t0\t0\t\t0\t0\nN\t0\ta\t\t\t1\t\nN\t1\ts\t\tvalue\t0\t\nX\tjson raw\n");
        check(tree.SetPayload(tr)&&tree.PlainText().find(L"value")!=std::wstring::npos,"shared tree searchable text loads");
        const auto tree_body=tree.PlainText();const auto treer=tree.TextRevision();
        tree.expanded_[0]=0;tree.BuildRows();
        check(tree.SetPayload(tr)&&tree.PlainText()==tree_body&&tree.TextRevision()==treer,"tree folding preserves full search text and revision");
        auto ar=owned(L"PULSEARC\t1\tZIP\t0\t0\n0\tf\t0\t0\t-\tfirst.txt\n");
        check(archive.SetPayload(ar,L"same.zip",1)&&archive.nodes_.size()==1,"shared archive loads");
        check(archive.SetPayload(ar,L"same.zip",99)&&archive.file_size_==99,"archive identity hit updates file size");
        check(archive.SetPayload(ar,L"other.zip",99)&&archive.file_name_==L"other.zip","archive file identity changes");
        auto invalid=owned(L"invalid");
        check(!m.SetPayload(invalid,L"same.md")&&!m.HasData()&&m.payload_identity_.expired(),"invalid markdown is not cached as successful");
        check(!t.SetPayload(invalid)&&t.payload_identity_.expired(),"invalid table does not poison identity");
        check(!tree.SetPayload(invalid)&&tree.payload_identity_.expired(),"invalid tree does not poison identity");
        check(!archive.SetPayload(invalid,L"same.zip",0)&&archive.payload_identity_.expired(),"invalid archive does not poison identity");
        t.Clear();m.Clear();tree.Clear();archive.Clear();
        check(t.payload_identity_.expired()&&m.payload_identity_.expired()&&tree.payload_identity_.expired()&&archive.payload_identity_.expired(),"Clear releases all identity tokens");
        check(t.SetPayload(table)&&m.SetPayload(md,L"same.md")&&tree.SetPayload(tr)&&archive.SetPayload(ar,L"same.zip",0),"same shared inputs reopen after Clear");
        std::weak_ptr<const std::wstring> weak=tr;tr.reset();
        check(!weak.expired()&&tree.HasData()&&tree.Source()==L"json raw"&&tree.PlainText().find(L"value")!=std::wstring::npos,
            "shared storage survives cache owner release with source and search text intact");
        tree.Clear();
        check(weak.expired(),"Clear releases the final shared payload owner");
        // Lazy sheet responses can return success while being intentionally ignored.
        t.Clear();
        const auto lazy=[&](int selected){return owned(L"PULSETBL\t1\nW\t2\t1\t"+std::to_wstring(selected)+L"\nS\txlsx\tA\t1\t1\t0\tnot-loaded\t0\nR\ta\nS\txlsx\tB\t1\t1\t0\tnot-loaded\t0\nR\tb\n");};
        auto sheet0=lazy(0),sheet1=lazy(1);t.SetPayload(sheet0);t.SwitchSheet(1);
        check(t.SetPayload(sheet0)&&t.sheet_==1&&t.awaiting_sheet_,"identity hit cannot clear awaiting different sheet");
        t.SetPayload(sheet1);t.SwitchSheet(0);
        auto stale1=owned(*lazy(1)+L"X\tstale source\n");const auto lr=t.TextRevision();
        check(t.SetPayload(stale1)&&t.sheet_==0&&t.TextRevision()==lr&&t.payload_identity_.lock()!=stale1,"superseded response is not adopted as identity");
        check(t.SetPayload(sheet0)&&t.sheet_==0&&!t.awaiting_sheet_,"selected lazy sheet response commits and clears wait");
        return ok;
    }
};
}
#endif
int main(int argc,char** argv) {
#ifdef PREVIEW_IDENTITY_NEW
    if(!pulse::ui::PreviewBufferLifetimeTest::Correctness()) return 1;
#endif
    if(argc==2&&std::strcmp(argv[1],"--correctness")==0) return 0;
    pulse::ui::TableView table; pulse::ui::MarkdownView md;
    const bool a=Bench("table5000x20",table,[](auto& v,const auto& p){return v.SetPayload(p);},TablePayload());
    const bool b=Bench("markdown10000",md,[](auto& v,const auto& p){return v.SetPayload(p,L"fixture.md");},MarkdownPayload());
    return a&&b?0:1;
}

#include "../ui/table_view.h"
#include "../ui/tree_view.h"
#include "../ui/markdown_view.h"
#include "../ui/archive_preview.h"
#include <cstdio>
#include <memory>

namespace pulse::ui {
struct PreviewBufferLifetimeTest {
    static size_t Bytes(const std::wstring& s) {
        return s.capacity()>std::wstring{}.capacity()?(s.capacity()+1)*sizeof(wchar_t):0;
    }
    template<class V,class Load> static bool Check(const char* name,V& view,Load load,std::wstring value) {
        auto payload=std::make_shared<const std::wstring>(std::move(value));
        if(!load(payload))return false;
        std::printf("storage %s chars=%zu input_capacity_bytes=%zu view_duplicate_capacity_bytes=%zu\n",name,payload->size(),Bytes(*payload),Bytes(view.payload_));
        bool ok=true;
        const auto check=[&](bool valid,const char* what){std::printf("[%s] %s: %s\n",valid?"PASS":"FAIL",name,what);ok&=valid;};
        check(view.shared_payload_==payload&&Bytes(view.payload_)==0,"no independent raw-body allocation");
        const std::wstring original=*payload;
        check(load(original)&&view.shared_payload_==payload&&Bytes(view.payload_)==0,
            "equal legacy input keeps shared storage without forcing a copy");
        std::wstring source,plain;
        if constexpr(requires{view.Source();})source=view.Source();
        if constexpr(requires{view.PlainText();})plain=view.PlainText();
        std::weak_ptr<const std::wstring> old=payload;
        auto replacement=std::make_shared<const std::wstring>(original);
        payload.reset();
        check(load(replacement)&&old.expired()&&view.shared_payload_==replacement,"same-content refresh transfers ownership without retaining old cache body");
        old=replacement;replacement.reset();
        check(!old.expired()&&view.Payload()==original,"cache release leaves full immutable payload alive");
        if constexpr(requires{view.Source();})check(view.Source()==source,"source remains intact after cache release");
        if constexpr(requires{view.PlainText();})check(view.PlainText()==plain,"search/copy text remains intact after cache release");
        view.Clear();
        check(old.expired()&&Bytes(view.payload_)==0&&!view.shared_payload_,"Clear releases final shared allocation");
        auto reopen=std::make_shared<const std::wstring>(original);
        check(load(reopen)&&view.shared_payload_==reopen,"reopen restores correct owning storage");
        old=reopen;reopen.reset();
        check(load(original+L"\n")&&old.expired()&&!view.shared_payload_&&Bytes(view.payload_)>0,"changed legacy input releases shared owner and keeps independent value");
        if constexpr(requires{view.Source();})check(view.Source()==source,"legacy source is unchanged");
        if constexpr(requires{view.PlainText();})check(view.PlainText()==plain,"legacy searchable body is unchanged");
        view.Clear();view.Clear();
        check(Bytes(view.payload_)==0&&!view.shared_payload_,"repeated Clear frees both storage modes");
        return ok;
    }
    static bool Run() {
        bool ok=true;
        TableView table;TreeView tree;MarkdownView md;ArchivePreview archive;
        std::wstring t=L"PULSETBL\t1\nS\tcsv\tCSV\t20\t5000\t0\tcomma\t0\nX\tsource\n";
        for(int r=0;r<5000;++r){t+=L"R";for(int c=0;c<20;++c)t+=L"\t"+std::wstring(32,L'v');t+=L'\n';}
        ok &= Check("table5000x20",table,[&](const auto& p){return table.SetPayload(p);},std::move(t));
        std::wstring m=L"PULSEMD\t1\nS\tsource\n";
        for(int i=0;i<10000;++i)m+=L"B\tp\t\t0\t0\t\t"+std::wstring(256,L'm')+L"\t\n";
        ok &= Check("markdown10000",md,[&](const auto& p){return md.SetPayload(p,L"fixture.md");},std::move(m));
        std::wstring tr=L"PULSETREE\t1\nH\tjson\t0\t0\t\t0\t0\nN\t0\ta\t\t\t10000\t\nX\tjson raw\n";
        for(int i=0;i<10000;++i)tr+=L"N\t1\ts\t\t"+std::wstring(32,L'v')+L"\t0\t\n";
        ok &= Check("tree10000",tree,[&](const auto& p){return tree.SetPayload(p);},std::move(tr));
        std::wstring a=L"PULSEARC\t1\tZIP\t0\t0\n";
        for(int i=0;i<20000;++i)a+=L"0\tf\t0\t0\t-\tf"+std::to_wstring(i)+L".txt\n";
        ok &= Check("archive20000",archive,[&](const auto& p){return archive.SetPayload(p,L"fixture.zip",0);},std::move(a));
        return ok;
    }
};
}
int main(){return pulse::ui::PreviewBufferLifetimeTest::Run()?0:1;}

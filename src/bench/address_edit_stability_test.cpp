#include "../app/app_internal.h"
#include "../ui/ui_renderer.h"
#include "../common/localization.h"
#include <functional>
#include <iostream>

void Render(pulse::AppState&);
namespace {
struct Messages { int margins=0,positions=0,redraw=0,hidden=0,text=0,size=0,selection=0; bool trace=false; };
LRESULT CALLBACK Observe(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
    auto& counts=*reinterpret_cast<Messages*>(data);
    if(msg==EM_SETMARGINS) ++counts.margins;
    if(msg==WM_WINDOWPOSCHANGED) ++counts.positions;
    if(msg==WM_SETREDRAW) ++counts.redraw;
    if(msg==WM_SETTEXT) ++counts.text;
    if(msg==EM_SETSEL) {
        ++counts.selection;
        if(counts.trace) std::cout<<"[TRACE] EM_SETSEL "<<wp<<","<<lp<<std::endl;
    }
    if(msg==WM_SIZE) ++counts.size;
    const auto result=DefSubclassProc(hwnd,msg,wp,lp);
    if(counts.trace && msg==WM_CHAR) {
        DWORD a=0,b=0;SendMessageW(hwnd,EM_GETSEL,reinterpret_cast<WPARAM>(&a),reinterpret_cast<LPARAM>(&b));
        std::cout<<"[TRACE] WM_CHAR "<<wp<<" selection="<<a<<","<<b<<" length="<<GetWindowTextLengthW(hwnd)<<std::endl;
    }
    if(msg==WM_SETREDRAW && !wp && !(GetWindowLongPtrW(hwnd,GWL_STYLE)&WS_VISIBLE)) ++counts.hidden;
    return result;
}
std::function<bool()> g_menu_step;
void CALLBACK MenuStep(HWND hwnd,UINT,UINT_PTR id,DWORD) {
    if(!g_menu_step || !g_menu_step()) KillTimer(hwnd,id);
}
// The fixture parent is a STATIC window; route the edit's notifications the way
// the main window does so live search is queued by the edit itself.
LRESULT CALLBACK ForwardEditNotifications(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
    auto& s=*reinterpret_cast<pulse::AppState*>(data);
    if(msg==WM_COMMAND && reinterpret_cast<HWND>(lp)==s.hwndAddressEdit && HIWORD(wp)==EN_CHANGE) {
        pulse::QueueAddressSearch(s);
        pulse::UpdateAddressSuggestions(s);
        return 0;
    }
    return DefSubclassProc(hwnd,msg,wp,lp);
}
}
int main() {
    using namespace pulse;
    OleInitialize(nullptr); l10n::Initialize(GetModuleHandleW(nullptr),L"zh-CN");
    auto owned=std::make_unique<AppState>(); auto& s=*owned;
    s.isolatedTest=true; s.appPrefs.persist=false; s.searchHistory.persist=false;
    s.hwnd=CreateWindowExW(0,L"STATIC",L"",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,-20000,-20000,1100,720,nullptr,nullptr,nullptr,nullptr);
    int failures=0;
    auto check=[&](bool ok,const char* label){std::cout<<(ok ? "[PASS] ":"[FAIL] ")<<label<<std::endl;if(!ok)++failures;};
    if(!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor); s.compositor.RecreateTextFormats(1);s.renderer.SetScale(1);
    check(s.compositor.LumaTextEnabled(),"fixture uses actual LumaText hosted-editor path");
    s.window_tabs.EnsureDefault();s.pane=s.window_tabs.Active()->FocusedPane();
    auto* tab=ActiveTab(s);tab->current_path=app::MakeSearchPath(L"content:3d3s");
    tab->pending_generation=990;tab->search_content_active=true;
    auto store=std::make_shared<index::ContentResultStore>(nullptr,0);
    index::ContentHit hit{L"C:\\fixture\\3d3s.txt",L"3d3s.txt",L"3d3s indexed result",5,0,1};hit.file_id=1;
    store->Append({hit});
    s.hwndAddressEdit=CreateHostedEdit(s,AddressEditProc);
    if(!s.hwndAddressEdit) return 2;
    s.addressEditing=true;s.addressSearching=true;s.addressSearchContent=true;s.addressSearchAnimation=1;
    SetWindowTextW(s.hwndAddressEdit,L"3d3s"); LayoutAddressEditor(s);
    ShowWindow(s.hwnd,SW_SHOWNOACTIVATE);ShowWindow(s.hwndAddressEdit,SW_SHOW);SetFocus(s.hwndAddressEdit);
    SendMessageW(s.hwndAddressEdit,EM_SETSEL,1,3);
    Render(s);UpdateWindow(s.hwndAddressEdit);
    Messages messages;
    SetWindowSubclass(s.hwndAddressEdit,Observe,98,reinterpret_cast<DWORD_PTR>(&messages));
    bool stable=true;
    for(int i=0;i<60;++i) {
        index::ContentSearchUpdate update;update.progress.generation=990;update.progress.scanned_files=i;
        update.progress.total_files=1000;update.results=store;ApplyContentSearchUpdate(s,std::move(update));
        Render(s);UpdateWindow(s.hwndAddressEdit);
        DWORD first=0,last=0;SendMessageW(s.hwndAddressEdit,EM_GETSEL,reinterpret_cast<WPARAM>(&first),reinterpret_cast<LPARAM>(&last));
        wchar_t text[32]{};GetWindowTextW(s.hwndAddressEdit,text,32);
        stable &= first==1 && last==3 && wcscmp(text,L"3d3s")==0 && GetFocus()==s.hwndAddressEdit && IsWindowVisible(s.hwndAddressEdit);
    }
    std::cout<<"frames=60 margins="<<messages.margins<<" positions="<<messages.positions<<" size="<<messages.size
             <<" redraw="<<messages.redraw<<" hidden="<<messages.hidden<<" text="<<messages.text<<std::endl;
    check(stable,"continuous search notifications preserve query text selection and focus");
    check(messages.margins==0 && messages.positions==0 && messages.size==0,"unchanged search editor geometry does not reapply native layout");
    check(messages.redraw==0 && messages.hidden==0 && messages.text==0,"search frames never hide or reinitialize LumaText edit surface");
    messages={};
    SetWindowPos(s.hwnd,nullptr,-20000,-20000,720,720,SWP_NOACTIVATE|SWP_NOZORDER);Render(s);
    check(messages.positions==1,"actual window resize updates hosted editor once");
    messages={};Render(s);
    check(messages.positions==0 && messages.redraw==0,"resized layout becomes stable on next frame");
    SendMessageW(s.hwndAddressEdit,EM_SETSEL,4,4);SendMessageW(s.hwndAddressEdit,WM_CHAR,L'x',0);
    wchar_t typed[32]{};GetWindowTextW(s.hwndAddressEdit,typed,32);
    check(wcscmp(typed,L"3d3sx")==0,"typing still updates the native edit after layout stabilizes");
    messages.trace=true;
    auto read_query=[&] {wchar_t value[256]{};GetWindowTextW(s.hwndAddressEdit,value,ARRAYSIZE(value));return std::wstring(value);};
    auto type=[&](const wchar_t* value,bool live) {
        for(const wchar_t* p=value;*p;++p) {
            SendMessageW(s.hwndAddressEdit,WM_CHAR,*p,0);
            if(live) {QueueAddressSearch(s);SubmitAddressSearch(s,true);}
            Render(s);UpdateWindow(s.hwndAddressEdit);
        }
    };
    SetWindowTextW(s.hwndAddressEdit,L"3d3s");SendMessageW(s.hwndAddressEdit,EM_SETSEL,4,4);
    type(L"abcd",false);
    check(read_query()==L"3d3sabcd","continuous typing appends characters in order across parent renders");
    SetWindowTextW(s.hwndAddressEdit,L"3d3s");SendMessageW(s.hwndAddressEdit,EM_SETSEL,4,4);
    type(L"abcd",true);
    check(read_query()==L"3d3sabcd","live query submission does not move subsequent typed characters to the front");
    SetWindowTextW(s.hwndAddressEdit,L"1234");SendMessageW(s.hwndAddressEdit,EM_SETSEL,2,2);
    type(L"abc",true);
    check(read_query()==L"12abc34","middle insertion preserves cursor across live query submission");
    SetWindowTextW(s.hwndAddressEdit,L"3d3s");SendMessageW(s.hwndAddressEdit,EM_SETSEL,0,0);
    RECT edit_rect{};GetClientRect(s.hwndAddressEdit,&edit_rect);
    const auto click=MAKELPARAM(edit_rect.right-4,edit_rect.bottom/2);
    SendMessageW(s.hwndAddressEdit,WM_LBUTTONDOWN,MK_LBUTTON,click);
    SendMessageW(s.hwndAddressEdit,WM_LBUTTONUP,0,click);
    type(L"abc",true);
    check(read_query()==L"3d3sabc","LumaText mouse placement at text end preserves following typing order");
    RemoveWindowSubclass(s.hwndAddressEdit,Observe,98);

    // Typing pauses long enough for the first live query, which leaves the plain
    // folder for a search page, then continues: with an IME (commit, pause,
    // commit) and with plain characters. The next text must follow the caret.
    SetWindowSubclass(s.hwnd,ForwardEditNotifications,97,reinterpret_cast<DWORD_PTR>(&s));
    auto pump=[&](DWORD ms) {
        const ULONGLONG until=GetTickCount64()+ms; MSG m{};
        while(GetTickCount64()<until) {
            while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&m);DispatchMessageW(&m);}
            TickAddressSearch(s,GetTickCount64());
            Render(s);UpdateWindow(s.hwndAddressEdit);Sleep(8);
        }
    };
    auto commit_ime=[&](const wchar_t* value) {
        SendMessageW(s.hwndAddressEdit,WM_IME_STARTCOMPOSITION,0,0);
        for(const wchar_t* p=value;*p;++p) SendMessageW(s.hwndAddressEdit,WM_CHAR,*p,1);
        SendMessageW(s.hwndAddressEdit,WM_IME_ENDCOMPOSITION,0,0);
    };
    auto type_chars=[&](const wchar_t* value) {
        for(const wchar_t* p=value;*p;++p) SendMessageW(s.hwndAddressEdit,WM_CHAR,*p,1);
    };
    auto caret_at=[&](DWORD where) {
        DWORD a=0,b=0;SendMessageW(s.hwndAddressEdit,EM_GETSEL,reinterpret_cast<WPARAM>(&a),reinterpret_cast<LPARAM>(&b));
        return a==where && b==where;
    };
    auto describe=[&](const char* label) {
        DWORD a=0,b=0;SendMessageW(s.hwndAddressEdit,EM_GETSEL,reinterpret_cast<WPARAM>(&a),reinterpret_cast<LPARAM>(&b));
        std::cout<<"[INFO] "<<label<<" text=";
        for(wchar_t c:read_query()) std::cout<<std::hex<<static_cast<unsigned>(c)<<' ';
        std::cout<<std::dec<<"sel="<<a<<","<<b<<" searching="<<s.addressSearching<<" results="<<IsAddressSearchResults(ActiveTab(s))
                 <<" focus="<<(GetFocus()==s.hwndAddressEdit)<<std::endl;
    };
    wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
    std::wstring folder=temp;while(folder.size()>3 && folder.back()==L'\\') folder.pop_back();
    auto fresh_search=[&](bool content) {
        s.addressIgnoreKillFocus=true;
        tab->search_content_active=false;tab->pending_generation=0;tab->content_results.reset();
        tab->NavigateTo(folder);tab->current_path=folder;
        s.addressSearching=false;s.addressEditing=false;s.addressSearchComposing=false;
        s.appPrefs.address_search_content=content;s.appPrefs.address_search_current=true;
        ShowAddressSearch(s);
        s.addressIgnoreKillFocus=false;
        s.addressSearchAnimation=1;
        pump(60);
    };
    const wchar_t* const kFirst=L"\u6587\u4ef6";      // wen jian
    const wchar_t* const kSecond=L"\u6d4b\u8bd5";     // ce shi
    const wchar_t* const kThird=L"\u62a5\u544a";      // bao gao
    fresh_search(false);
    const HWND first_edit=s.hwndAddressEdit;
    check(first_edit && s.addressSearching && read_query().empty(),"plain folder opens an empty name search");
    commit_ime(kFirst);describe("ime first commit");
    pump(450);describe("after live query");
    check(IsAddressSearchResults(ActiveTab(s)),"live query reached the search page");
    commit_ime(kSecond);describe("ime second commit");
    check(read_query()==std::wstring(kFirst)+kSecond && caret_at(4),"IME text after a live query follows the caret");
    check(s.hwndAddressEdit==first_edit,"live query keeps the same editor window");
    pump(450);
    commit_ime(kThird);describe("ime third commit");
    check(read_query()==std::wstring(kFirst)+kSecond+kThird && caret_at(6),"IME text on a results page follows the caret");
    fresh_search(false);
    type_chars(L"abc");pump(450);describe("ascii after live query");
    type_chars(L"def");
    check(read_query()==L"abcdef" && caret_at(6),"typed text after a live query follows the caret");
    fresh_search(true);
    commit_ime(kFirst);pump(450);describe("content ime after live query");
    commit_ime(kSecond);
    check(read_query()==std::wstring(kFirst)+kSecond && caret_at(4),"content-mode IME text after a live query follows the caret");
    // The history popup opens over an empty box and keeps a modal loop running
    // while the user types into the search box (its external filter edit).
    s.searchHistory.persist=false;
    s.searchHistory.Record(L"setup-dia",app::MakeSearchPath(L"setup-dia"));
    fresh_search(false);
    int step=0;bool menu_open_while_typing=false;std::wstring history_text;DWORD history_caret=0;
    ULONGLONG step_at=GetTickCount64();
    g_menu_step=[&]() -> bool {
        TickAddressSearch(s,GetTickCount64());Render(s);
        if(GetTickCount64()<step_at) return true;
        switch(step++) {
        case 0: menu_open_while_typing=s.searchHistoryOpen && s.menu && s.menu->IsOpen();
                commit_ime(kFirst);describe("history ime first");step_at=GetTickCount64()+450;return true;
        case 1: describe("history after live query");commit_ime(kSecond);describe("history ime second");
                history_text=read_query();
                {DWORD a=0;SendMessageW(s.hwndAddressEdit,EM_GETSEL,reinterpret_cast<WPARAM>(&a),reinterpret_cast<LPARAM>(&history_caret));}
                step_at=GetTickCount64()+100;return true;
        default: if(s.menu && s.menu->IsOpen()) s.menu->Dismiss();return false;
        }
    };
    SetTimer(s.hwnd,0x5352,30,MenuStep);
    ShowAddressSearchHistory(s);
    KillTimer(s.hwnd,0x5352);g_menu_step=nullptr;
    std::cout<<"[INFO] history popup open while typing="<<menu_open_while_typing<<" steps="<<step<<std::endl;
    check(step>=2,"history popup modal loop ran the typing script");
    check(history_text==std::wstring(kFirst)+kSecond && history_caret==4,"IME text typed under the history popup follows the caret");
    RemoveWindowSubclass(s.hwnd,ForwardEditNotifications,97);
    s.addressIgnoreKillFocus=true;DestroyWindow(s.hwndAddressEdit);s.hwndAddressEdit=nullptr;
    s.renderer.SetCompositor(nullptr);s.compositor.Shutdown();DestroyWindow(s.hwnd);s.hwnd=nullptr;
    std::cout<<"failures="<<failures<<std::endl;OleUninitialize();return failures ? 1:0;
}

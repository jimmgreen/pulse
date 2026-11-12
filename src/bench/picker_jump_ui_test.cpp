#include "../picker_inprocess/picker_ui_api_v2.h"
#include "picker_interaction_test_api.h"
#include "picker_jump_ui_test_api.h"
#include "picker_places_test_api.h"
#include <shobjidl.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
std::atomic<unsigned> failures = 0;
void Check(bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); std::fflush(stdout); if (!ok) ++failures; }
PulsePickerUiV2Text Text(const std::wstring& s) { return {s.data(), static_cast<uint32_t>(s.size())}; }
bool Send(HWND window, UINT message, WPARAM wp=0, LPARAM lp=0) {
    DWORD process=0; GetWindowThreadProcessId(window,&process); if(process!=GetCurrentProcessId())return false;
    DWORD_PTR result=0; return SendMessageTimeoutW(window,message,wp,lp,SMTO_BLOCK|SMTO_ABORTIFHUNG,3000,&result)!=0 && result!=0;
}
bool SendOnly(HWND window, UINT message, WPARAM wp=0, LPARAM lp=0) {
    DWORD_PTR result=0;return SendMessageTimeoutW(window,message,wp,lp,SMTO_BLOCK|SMTO_ABORTIFHUNG,3000,&result)!=0;
}
bool Wait(const std::function<bool()>& predicate, const std::atomic<bool>& done) {
    const auto end=std::chrono::steady_clock::now()+10s;
    while(!done && std::chrono::steady_clock::now()<end) {if(predicate())return true;std::this_thread::sleep_for(20ms);}return predicate();
}
void Click(HWND window, POINT p) {
    DWORD_PTR result=0;
    SendMessageTimeoutW(window,WM_MOUSEMOVE,0,MAKELPARAM(p.x,p.y),SMTO_BLOCK|SMTO_ABORTIFHUNG,3000,&result);
    SendMessageTimeoutW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(p.x,p.y),SMTO_BLOCK|SMTO_ABORTIFHUNG,3000,&result);
    PostMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(p.x,p.y));
}
POINT Center(RECT r) {return {(r.left+r.right)/2,(r.top+r.bottom)/2};}
bool Capture(HWND window,const std::filesystem::path& path) {
    // The state assertion precedes capture; let the checkmark transition finish.
    std::this_thread::sleep_for(250ms);
    const auto text=path.wstring();
    if(!SetPropW(window,L"Pulse.PickerTestSnapshot",const_cast<wchar_t*>(text.c_str())))return false;
    DWORD_PTR result=0;
    const bool painted=SendMessageTimeoutW(window,WM_PAINT,0,0,SMTO_BLOCK|SMTO_ABORTIFHUNG,5000,&result)!=0;
    RemovePropW(window,L"Pulse.PickerTestSnapshot");
    return painted && std::filesystem::exists(path) && std::filesystem::file_size(path)>0;
}
HWND Menu() {
    HWND found=nullptr; EnumWindows([](HWND w,LPARAM p)->BOOL {DWORD process=0;GetWindowThreadProcessId(w,&process);wchar_t c[64]{};GetClassNameW(w,c,64);
        if(process==GetCurrentProcessId() && IsWindowVisible(w) && std::wstring_view(c)==L"PulseFluentMenu") {*reinterpret_cast<HWND*>(p)=w;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&found));return found;
}
HWND Confirmation() {
    HWND found=nullptr;EnumWindows([](HWND w,LPARAM p)->BOOL {DWORD process=0;GetWindowThreadProcessId(w,&process);wchar_t c[64]{};GetClassNameW(w,c,64);
        if(process==GetCurrentProcessId() && IsWindowVisible(w) && std::wstring_view(c)==L"PulseConfirmWindow") {*reinterpret_cast<HWND*>(p)=w;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&found));return found;
}
struct State {
    std::atomic<bool> done=false,edit_matches=false,property_matches=false;
    std::atomic<unsigned> host_item=0, host_control=0;
    std::atomic<unsigned> checks=0,edits=0,radio=0,properties=0; std::atomic<unsigned> last_control=0,file_ok=0;
    PulsePickerUiV2Api* api=nullptr;PulsePickerUiV2Session session=0;
    std::atomic<unsigned> filter_index=0,filter_events=0;
    std::atomic<bool> replace_filename_on_type=false;
    std::thread driver; std::function<bool(HWND,HWND,State&)> drive; bool driven=false;
};
HRESULT WINAPI Event(void* context,const PulsePickerUiV2Event* event) {
    auto& state=*static_cast<State*>(context);
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED){state.host_control=event->control_id;state.host_item=event->item_id;}
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_CHECK_TOGGLED){state.last_control=event->control_id;++state.checks;}
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_FILE_OK)++state.file_ok;
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_EDIT_CHANGED){state.edit_matches=event->control_id==201 && std::wstring_view(event->control_text.data,event->control_text.units)==L"edited value";++state.edits;}
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED && event->control_id==202)state.radio=event->item_id;
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_PROPERTY_CHANGED){state.property_matches=event->control_id==7 && event->property_value_count==1 && std::wstring_view(event->property_values[0].data,event->property_values[0].units)==L"updated property";++state.properties;}
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_TYPE_CHANGED) {
        std::vector<wchar_t> folder(32768),name(32768),selection(32768),candidate(32768);
        PulsePickerUiV2Snapshot snapshot{};snapshot.size=sizeof(snapshot);
        snapshot.folder={folder.data(),32768};snapshot.filename={name.data(),32768};snapshot.selection={selection.data(),32768};snapshot.candidate={candidate.data(),32768};
        if(state.api->Query(state.session,event->show_id,event->generation,&snapshot)==S_OK){state.filter_index=snapshot.filter_index;++state.filter_events;}
        if(state.replace_filename_on_type.exchange(false))
            SendMessageW(GetDlgItem(event->window,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"host-owned.txt"));
    }
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_READY) {
        const auto desktop=GetThreadDesktop(GetCurrentThreadId()); const auto window=event->window;
        state.driver=std::thread([&state,desktop,window] {
            SetThreadDesktop(desktop);
            SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(window));
            HWND sender=CreateWindowExW(0,L"STATIC",L"Jump test sender",0,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            state.driven=sender && state.drive(window,sender,state);
            if(auto menu=Menu())PostMessageW(menu,WM_KEYDOWN,VK_ESCAPE,0);
            if(auto prompt=Confirmation())PostMessageW(prompt,WM_CLOSE,0,0);
            if(IsWindow(window))PostMessageW(window,WM_CLOSE,0,0);
            if(sender)DestroyWindow(sender);
        });
    }return S_OK;
}
bool Geometry(HWND window, HWND sender, pulse::ui::PickerJumpUiTest& q) {
    q={};
    if(Send(window,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q)))return q.ready;
    // Retained private test DLL predates the context toolbar test fields.
    constexpr auto alignment=alignof(pulse::ui::PickerJumpUiTest);
    q.size=static_cast<UINT>((offsetof(pulse::ui::PickerJumpUiTest,jump_file)+sizeof(bool)+alignment-1)/alignment*alignment);
    return Send(window,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q)) && q.ready;
}
bool Selection(HWND window,size_t count) {
    pulse::ui::PickerInteractionTest q;return Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&q)) && q.selected_paths.size()==count;
}
bool HasRow(HWND window,const std::wstring& path) {
    pulse::ui::PickerPlacesTestPoint q;q.path=path.c_str();
    return Send(window,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&q));
}
std::wstring Filename(HWND window) {
    wchar_t text[1024]{};SendOnly(GetDlgItem(window,102),WM_GETTEXT,std::size(text),reinterpret_cast<LPARAM>(text));return text;
}
bool JumpFixture(HWND window,HWND sender,const std::wstring& path,bool file,bool save) {
    pulse::ui::PickerJumpUiTest fixture;fixture.jump_path=path.c_str();fixture.jump_file=file;
    if(!Send(window,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&fixture)))return false;
    pulse::ui::PickerInteractionTest key;key.key='L';key.ctrl=true;
    if(!Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&key)))return false;
    if(!SendOnly(GetDlgItem(window,101),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"fixture-result")))return false;
    key={};key.key=VK_RETURN;key.ctrl=save;key.edit_id=101;
    return Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&key));
}
void Run(PulsePickerUiV2Api& api,PulsePickerUiV2Session session,PulsePickerUiV2Config& config,
         const std::function<bool(HWND,HWND,State&)>& drive,const char* label,const std::wstring& expected={},uint32_t expected_filter=UINT32_MAX) {
    const auto validated=api.ValidateConfig(session,&config);
    std::printf("CONFIG validate=%08lX access=%u options=%u save=%u controls=%u properties=%u\n",static_cast<unsigned long>(validated),config.access_policy,config.open_options,config.save_options,config.control_count,config.property_count);
    State state;state.drive=drive;state.api=&api;state.session=session;
    PulsePickerUiV2Callbacks callbacks{sizeof(callbacks),0,Event,&state};
    std::vector<wchar_t> path(PULSE_PICKER_UI_V2_MAX_UNITS+1),paths(PULSE_PICKER_UI_V2_MAX_PACKED_UNITS+1);
    PulsePickerUiV2Result result{};result.size=sizeof(result);result.path={path.data(),static_cast<uint32_t>(path.size())};result.paths={paths.data(),static_cast<uint32_t>(paths.size())};
    const auto hr=api.Show(session,&config,&callbacks,&result);
    state.done=true;if(state.driver.joinable())state.driver.join();
    std::printf("SHOW hr=%08lX status=%u driven=%u\n",static_cast<unsigned long>(hr),result.status,state.driven);
    Check(state.driven && (expected.empty() ? result.status==PULSE_PICKER_UI_V2_CANCELLED :
        result.status==PULSE_PICKER_UI_V2_ACCEPTED && state.file_ok==1 && std::wstring(path.data())==expected &&
        (expected_filter==UINT32_MAX || result.filter_index==expected_filter)),label);
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc<3){std::puts("Usage: picker_jump_ui_test ABSOLUTE_DLL ABSOLUTE_OUTPUT_DIR [--interactive] [--case=options|multi|create|save|jump|complex|context]");return 2;}
    bool interactive=false;std::wstring selected_case;
    for(int i=3;i<argc;++i){const std::wstring_view arg=argv[i];if(arg==L"--interactive")interactive=true;else if(arg.starts_with(L"--case="))selected_case=arg.substr(7);else return 2;}
    if(!selected_case.empty() && selected_case!=L"options" && selected_case!=L"multi" && selected_case!=L"create" && selected_case!=L"save" && selected_case!=L"jump" && selected_case!=L"complex" && selected_case!=L"context" && selected_case!=L"extension" && selected_case!=L"recentthumb" && selected_case!=L"hostmenu")return 2;
    const auto run_case=[&](const wchar_t* name){return selected_case.empty() || selected_case==name;};
    const auto desktop_name=L"PulseJumpUi_"+std::to_wstring(GetCurrentProcessId());
    const auto desktop=interactive ? OpenInputDesktop(0,FALSE,DESKTOP_CREATEWINDOW|DESKTOP_READOBJECTS|DESKTOP_WRITEOBJECTS) :
        CreateDesktopW(desktop_name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!desktop || !SetThreadDesktop(desktop))return 2;
    const auto output=std::filesystem::absolute(argv[2]); const auto root=output/L"fixture";
    if(std::filesystem::exists(root)){std::puts("fixture already exists; preserve it");return 2;}
    std::filesystem::create_directories(root);std::ofstream(root/L"a.txt")<<"a";std::ofstream(root/L"b.txt")<<"b";
    struct Cleanup {std::filesystem::path root;~Cleanup(){std::error_code ec;std::filesystem::remove_all(root,ec);}}cleanup{root};
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR",output.c_str());
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 2;
    const auto module=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const auto get=module?reinterpret_cast<decltype(&PulsePickerUiGetApiV2)>(GetProcAddress(module,"PulsePickerUiGetApiV2")):nullptr;
    PulsePickerUiV2Api api{};api.size=sizeof(api);if(!get || get(2,sizeof(api),&api)!=S_OK){Check(false,"load real UI DLL");return 1;}
    const auto layout=reinterpret_cast<int(WINAPI*)()>(GetProcAddress(module,"PulsePickerJumpLayoutTest"));
    Check(layout && layout()==0,"real DLL full-width navigation and jump-strip layout regression");
    PulsePickerUiV2Initialize initialize{sizeof(initialize),2};PulsePickerUiV2Session session=0;
    if(api.Initialize(&initialize,&session)!=S_OK)return 2;
    const std::wstring folder=root.wstring(),cancel=L"Leave";
    PulsePickerUiV2Config config{};config.size=sizeof(config);config.version=2;config.initial_folder=Text(folder);config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
    Check(api.ValidateConfig(session,&config)==S_OK,"legacy config accepted without provenance tail");
    PulsePickerUiV2FolderPolicyConfig extended{};extended.base.base=config;extended.base.base.size=sizeof(extended);
    Check(api.ValidateConfig(session,&extended.base.base)==S_OK,"policy zero accepted without cancel label");
    extended.folder_policy=PULSE_PICKER_UI_V2_FOLDER_ALLOW_AUTO_JUMP;
    Check(api.ValidateConfig(session,&extended.base.base)==S_OK,"explicit allow-auto-jump policy accepted");
    extended.folder_policy=2;Check(api.ValidateConfig(session,&extended.base.base)==E_INVALIDARG,"unknown folder policy rejected");
    extended.folder_policy=0;extended.reserved=1;Check(api.ValidateConfig(session,&extended.base.base)==E_INVALIDARG,"reserved folder policy tail rejected");
    extended.reserved=0;extended.base.cancel_button_label=Text(cancel);
    Check(api.ValidateConfig(session,&extended.base.base)==E_INVALIDARG,"cancel text without presence bit rejected");
    extended.base.base.label_flags=PULSE_PICKER_UI_V2_LABEL_CANCEL;
    Check(api.ValidateConfig(session,&extended.base.base)==S_OK,"provenance and cancel label coexist");
    extended.base.base.size=sizeof(PulsePickerUiV2CancelLabelConfig);
    Check(api.ValidateConfig(session,&extended.base.base)==S_OK,"legacy cancel label extension remains accepted");
    extended.base.base.size=sizeof(extended)-1;
    Check(api.ValidateConfig(session,&extended.base.base)==E_INVALIDARG,"truncated policy size rejected before reading tail");
    std::vector<std::wstring> labels={L"A",L"B",L"C",L"D",L"E",L"F",L"G",L"H"};std::vector<PulsePickerUiV2Control> controls;
    for(unsigned i=0;i<labels.size();++i){PulsePickerUiV2Control c{};c.size=sizeof(c);c.kind=PULSE_PICKER_UI_V2_CONTROL_CHECK;c.id=100+i;c.state=3;c.label=Text(labels[i]);controls.push_back(c);}
    config.controls=controls.data();config.control_count=static_cast<uint32_t>(controls.size());
    if(run_case(L"options")) Run(api,session,config,[&](HWND window,HWND sender,State& state){
        pulse::ui::PickerJumpUiTest q;const bool geometry=Wait([&]{return Geometry(window,sender,q)&&q.option_count && q.more.right>q.more.left;},state.done);
        std::printf("OPTIONS ready=%u count=%u first=%ld,%ld,%ld,%ld more=%ld,%ld,%ld,%ld\n",q.ready,q.option_count,q.options[0].left,q.options[0].top,q.options[0].right,q.options[0].bottom,q.more.left,q.more.top,q.more.right,q.more.bottom);
        Check(Capture(window,output/L"options-before.png"),"capture compact-options initial window");
        if(!geometry)return false;
        Click(window,Center(q.options[0]));
        if(!Wait([&]{return state.checks>=1;},state.done)){
            Geometry(window,sender,q);
            std::printf("OPTIONS timeout down=%d up=%d activate=%d pressed=%d\n",q.last_down_hit,q.last_up_hit,q.last_activate,q.current_pressed);return false;
        }
        Check(state.last_control==100,"compact checkbox click reaches custom control rather than JumpRow");
        Check(Capture(window,output/L"options.png"),"capture actual compact-options picker window");
        const auto expected=100+q.option_count;const auto before=state.checks.load();
        Click(window,Center(q.more));if(!Wait([]{return Menu()!=nullptr;},state.done))return false;
        Check(true,"more-options click opens actual menu");
        PostMessageW(Menu(),WM_KEYDOWN,VK_RETURN,0);
        if(!Wait([&]{return state.checks>before;},state.done))return false;
        std::printf("MORE expected=%u actual=%u\n",expected,state.last_control.load());
        Check(state.last_control==expected,"overflow menu toggles first hidden checkbox");return true;
    },"compact and overflow UI complete then cancel cleanly");
    config.controls=nullptr;config.control_count=0;config.open_options=PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT;
    Check(api.ValidateConfig(session,&config)==S_OK,"multiselect UI configuration validates");
    if(run_case(L"multi")) Run(api,session,config,[&](HWND window,HWND sender,State& state){
        pulse::ui::PickerJumpUiTest q;const bool ready=Wait([&]{return Geometry(window,sender,q)&&q.row_check_count>=2;},state.done);
        std::printf("MULTI ready=%u checks=%u p0=%ld,%ld p1=%ld,%ld\n",q.ready,q.row_check_count,q.row_checks[0].x,q.row_checks[0].y,q.row_checks[1].x,q.row_checks[1].y);
        Check(Capture(window,output/L"multiselect-before.png"),"capture multiselect initial window");
        if(!ready)return false;
        Click(window,q.row_checks[0]);if(!Wait([&]{return Selection(window,1);},state.done)){std::puts("MULTI first check timeout");return false;}
        if(!Geometry(window,sender,q)||q.row_check_count<2)return false;
        std::printf("MULTI first down=%d up=%d activate=%d pressed=%d\n",q.last_down_hit,q.last_up_hit,q.last_activate,q.current_pressed);
        Click(window,q.row_checks[1]);if(!Wait([&]{return Selection(window,2);},state.done)){
            pulse::ui::PickerInteractionTest selection;Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&selection));
            std::printf("MULTI second check timeout selected=%zu p1=%ld,%ld\n",selection.selected_paths.size(),q.row_checks[1].x,q.row_checks[1].y);
            Geometry(window,sender,q);
            std::printf("MULTI second down=%d up=%d activate=%d pressed=%d\n",q.last_down_hit,q.last_up_hit,q.last_activate,q.current_pressed);
            for(const auto& path:selection.selected_paths)std::wprintf(L"SELECTED %ls\n",path.c_str());
            Check(Capture(window,output/L"multiselect-failure.png"),"capture multiselect selection failure");return false;
        }
        Check(true,"two actual RowCheck clicks retain both selections without Ctrl");
        Check(Capture(window,output/L"multiselect.png"),"capture actual multiselect picker window");
        if(!Geometry(window,sender,q)||!q.remove_count)return false;
        Click(window,Center(q.remove_selection[0]));if(!Wait([&]{return Selection(window,1);},state.done))return false;
        Check(true,"selection tray remove click removes exactly one item");return true;
    },"multiselect row check and tray removal complete then cancel");
    config.open_options=0;
    if(run_case(L"create")) Run(api,session,config,[&](HWND window,HWND sender,State& state){
        pulse::ui::PickerJumpUiTest geometry;if(!Wait([&]{return Geometry(window,sender,geometry);},state.done))return false;
        pulse::ui::PickerInteractionTest key;key.key='L';key.ctrl=true;
        if(!Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&key)))return false;
        const auto edit=GetDlgItem(window,101);if(!edit)return false;
        DWORD_PTR ignored=0;if(!SendMessageTimeoutW(edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"created"),SMTO_BLOCK,3000,&ignored))return false;
        key={};key.key=VK_RETURN;key.ctrl=true;key.shift=true;key.edit_id=101;
        if(!Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&key)))return false;
        const auto expected=(root/L"created").wstring();
        const bool entered=Wait([&]{pulse::ui::PickerInteractionTest q;return Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&q)) && q.current==expected;},state.done);
        Check(entered && std::filesystem::is_directory(expected),"query CtrlShiftEnter creates exact folder and enters it");return entered;
    },"query creation completes then cancels cleanly");
    const std::wstring relative=L"child\\report.txt",leaf=L"report.txt";
    config.filename=Text(leaf);config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
    config.save_options=PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
    if(run_case(L"save")) {
    Run(api,session,config,[&](HWND window,HWND sender,State& state){
        pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(window,sender,q);},state.done))return false;
        if(!SendOnly(GetDlgItem(window,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(relative.c_str())))return false;
        if(!SendOnly(GetDlgItem(window,102),WM_KEYDOWN,VK_RETURN) || !Wait([]{return Confirmation()!=nullptr;},state.done))return false;
        Check(!std::filesystem::exists(root/L"child") && !state.file_ok,"relative save asks before creating parent or FileOk");
        SendOnly(Confirmation(),WM_CLOSE);
        if(!Wait([&]{return !Confirmation() && Geometry(window,sender,q);},state.done))return false;
        Check(!std::filesystem::exists(root/L"child") && !state.file_ok,"declining relative-parent prompt leaves filesystem unchanged");
        if(!SendOnly(GetDlgItem(window,102),WM_KEYDOWN,VK_RETURN) || !Wait([]{return Confirmation()!=nullptr;},state.done))return false;
        const auto prompt=Confirmation();SendOnly(prompt,WM_KEYDOWN,VK_TAB);SendOnly(prompt,WM_KEYDOWN,VK_RETURN);
        return Wait([&]{return !IsWindow(window);},state.done);
    },"confirmed relative-parent creation reaches exactly one FileOk",(root/L"child"/L"report.txt").wstring());
    Check(std::filesystem::is_directory(root/L"child") && !std::filesystem::exists(root/L"child"/L"report.txt"),
        "picker creates confirmed parent but never creates saved document");
    const std::wstring strict=L"required\\report.txt";config.native_options=FOS_PATHMUSTEXIST;
    Run(api,session,config,[&](HWND window,HWND sender,State& state){
        pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(window,sender,q);},state.done))return false;
        if(!SendOnly(GetDlgItem(window,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(strict.c_str())))return false;
        if(!SendOnly(GetDlgItem(window,102),WM_KEYDOWN,VK_RETURN) || !Wait([&]{return Geometry(window,sender,q);},state.done))return false;
        const bool rejected=!Confirmation() && !state.file_ok && !std::filesystem::exists(root/L"required");
        Check(rejected,"FOS_PATHMUSTEXIST never offers or creates missing relative parent");return rejected;
    },"required-parent save remains open until cancellation");
    }
    if(run_case(L"jump")) {
        const auto target=root/L"jump-target";std::filesystem::create_directory(target);
        const auto document=target/L"report.txt";std::ofstream(document)<<"preserved";
        config.native_options=0;config.filename=Text(leaf);
        Run(api,session,config,[&](HWND window,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(window,sender,q);},state.done))return false;
            if(!JumpFixture(window,sender,target.wstring(),false,true))return false;
            if(!Wait([&]{return GetPropW(window,L"Pulse.PickerOverwriteGeneration")!=nullptr;},state.done))return false;
            pulse::ui::PickerInteractionTest current;
            if(!Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&current)))return false;
            Check(current.current==target.wstring() && IsWindow(window),"CtrlEnter commits jump directory and waits at real overwrite confirmation");
            Check(Capture(window,output/L"jump-overwrite.png"),"capture actual CtrlEnter overwrite confirmation");
            q={};if(!Send(window,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q)))return false;
            Check(!q.primary_enabled,"bottom Save stays disabled during overwrite choice");
            Click(window,Center(q.overwrite_replace));return Wait([&]{return !IsWindow(window);},state.done);
        },"CtrlEnter preserves overwrite confirmation and exactly one FileOk",document.wstring());
        std::ifstream file(document);std::string content;file>>content;Check(content=="preserved","picker never writes the existing save document");
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;config.save_options=0;config.filename={};
        Run(api,session,config,[&](HWND window,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(window,sender,q);},state.done))return false;
            if(!JumpFixture(window,sender,document.wstring(),true,false))return false;
            const bool selected=Wait([&]{pulse::ui::PickerInteractionTest current;return Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&current)) && current.current==target.wstring() && current.selected_paths==std::vector<std::wstring>{document.wstring()};},state.done);
            if(!selected){pulse::ui::PickerInteractionTest current;Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&current));std::wprintf(L"FILEJUMP current=%ls selected=%zu expected=%ls\n",current.current.c_str(),current.selected_paths.size(),document.c_str());for(const auto& item:current.selected_paths)std::wprintf(L"FILEJUMP selected=%ls\n",item.c_str());Capture(window,output/L"jump-file-failed.png");}
            Check(selected,"file jump lists parent and selects exact returned file");return selected;
        },"file-result jump selects file without accepting dialog");
    }
    if(run_case(L"complex")) {
        const std::wstring edit_label=L"Description",radio_label=L"Mode",first=L"First",second=L"Second",initial=L"initial";
        PulsePickerUiV2ControlItem items[2]={{sizeof(PulsePickerUiV2ControlItem),10,3,0,Text(first)},
            {sizeof(PulsePickerUiV2ControlItem),20,3,0,Text(second)}};
        PulsePickerUiV2Control custom[2]{};
        custom[0].size=sizeof(custom[0]);custom[0].kind=PULSE_PICKER_UI_V2_CONTROL_EDIT;custom[0].id=201;custom[0].state=3;custom[0].label=Text(edit_label);custom[0].text=Text(initial);
        custom[1].size=sizeof(custom[1]);custom[1].kind=PULSE_PICKER_UI_V2_CONTROL_RADIO;custom[1].id=202;custom[1].state=3;custom[1].label=Text(radio_label);custom[1].items=items;custom[1].item_count=2;custom[1].has_selected=1;custom[1].selected_item_id=10;
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;config.save_options=0;config.native_options=0;config.filename={};config.controls=custom;config.control_count=2;
        Run(api,session,config,[&](HWND window,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(window,sender,q)&&q.option_count==2;},state.done))return false;
            Click(window,Center(q.options[0]));const auto edit=GetDlgItem(window,2000);
            if(!Wait([&]{return IsWindowVisible(edit)!=FALSE;},state.done))return false;
            Check(true,"text capsule expands original edit panel");
            if(!SendOnly(edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"edited value")) || !Wait([&]{return state.edits>0;},state.done))return false;
            Check(state.edit_matches,"real edit change publishes exact control id and text");
            Check(Capture(window,output/L"complex-edit.png"),"capture expanded text and radio panel");
            SendOnly(edit,WM_KEYDOWN,VK_ESCAPE);
            if(!Wait([&]{return IsWindow(window) && !IsWindowVisible(edit) && Geometry(window,sender,q)&&q.option_count==2;},state.done))return false;
            Check(true,"Escape from edit collapses panel without cancelling dialog");
            Check(Capture(window,output/L"complex-collapsed.png"),"capture collapsed updated capsules");
            if(!Geometry(window,sender,q))return false;
            Click(window,Center(q.options[1]));
            if(!Wait([&]{return IsWindowVisible(edit)!=FALSE;},state.done)){
                Geometry(window,sender,q);std::printf("RADIO expand timeout down=%d up=%d activate=%d pressed=%d options=%u\n",q.last_down_hit,q.last_up_hit,q.last_activate,q.current_pressed,q.option_count);return false;
            }
            Check(true,"radio capsule opens original panel");
            SendOnly(window,WM_KEYDOWN,VK_DOWN);
            if(!Wait([&]{return state.radio==20;},state.done)){std::printf("RADIO callback timeout selected=%u\n",state.radio.load());return false;}
            Check(true,"radio capsule expands and keyboard activation publishes selected item");
            SendOnly(window,WM_KEYDOWN,VK_ESCAPE);
            if(!Wait([&]{return IsWindow(window) && !IsWindowVisible(edit);},state.done))return false;
            pulse::ui::PickerInteractionTest geometry;if(!Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&geometry)))return false;
            Click(window,Center(geometry.cancel));return Wait([&]{return !IsWindow(window);},state.done);
        },"complex text/radio Escape collapse and actual Cancel close");
        const std::wstring property_label=L"Title";const auto value=Text(initial);
        PulsePickerUiV2Property property{sizeof(property),7,PULSE_PICKER_UI_V2_PROPERTY_SCALAR,Text(property_label),&value,1};
        config.controls=nullptr;config.control_count=0;config.properties=&property;config.property_count=1;
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;config.save_options=PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;config.filename=Text(leaf);
        Run(api,session,config,[&](HWND window,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(window,sender,q)&&q.option_count==1;},state.done))return false;
            Click(window,Center(q.options[0]));const auto edit=GetDlgItem(window,2001);
            if(!Wait([&]{return IsWindowVisible(edit)!=FALSE;},state.done))return false;
            Check(true,"property capsule expands original property editor");
            if(!SendOnly(edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"updated property")) || !Wait([&]{return state.properties>0;},state.done))return false;
            Check(state.property_matches,"property edit publishes exact index and scalar value");
            Check(Capture(window,output/L"complex-property.png"),"capture expanded scalar property panel");
            SendOnly(edit,WM_KEYDOWN,VK_ESCAPE);
            if(!Wait([&]{return IsWindow(window) && !IsWindowVisible(edit);},state.done))return false;
            Check(true,"property Escape only collapses editor");
            pulse::ui::PickerInteractionTest geometry;if(!Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&geometry)))return false;
            Click(window,Center(geometry.cancel));return Wait([&]{return !IsWindow(window);},state.done);
        },"property edit collapses then actual Cancel closes without save");
    }
    if(run_case(L"context")) {
        const auto binary=root/L"only.bin";std::ofstream(binary)<<"binary";
        const std::wstring text_label=L"Text",binary_label=L"Binary",text_pattern=L"*.txt",binary_pattern=L"*.bin";
        PulsePickerUiV2Filter filters[2]={{sizeof(PulsePickerUiV2Filter),0,Text(text_label),Text(text_pattern)},
            {sizeof(PulsePickerUiV2Filter),0,Text(binary_label),Text(binary_pattern)}};
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;config.save_options=0;config.native_options=0;
        config.filename={};config.controls=nullptr;config.control_count=0;config.properties=nullptr;config.property_count=0;
        config.filters=filters;config.filter_count=2;config.filter_index=0;
        Run(api,session,config,[&](HWND window,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;
            if(!Wait([&]{return Geometry(window,sender,q)&&q.context_filter_count==2&&HasRow(window,(root/L"a.txt").wstring());},state.done))return false;
            Click(window,Center(q.context_filters[1]));
            if(!Wait([&]{return state.filter_events && state.filter_index==1 && Geometry(window,sender,q)&&HasRow(window,binary.wstring())&&!HasRow(window,(root/L"a.txt").wstring());},state.done))return false;
            Check(true,"top Binary capsule publishes filter index and filters actual listing");
            Click(window,Center(q.context_filters[0]));
            if(!Wait([&]{return state.filter_events>=2 && state.filter_index==0 && Geometry(window,sender,q)&&HasRow(window,(root/L"a.txt").wstring())&&!HasRow(window,binary.wstring());},state.done))return false;
            Check(true,"top Text capsule restores filter index and actual listing");
            Check(Capture(window,output/L"context-filters.png"),"capture actual body context toolbar filters");
            if(q.new_folder.right<=q.new_folder.left)return false;Click(window,Center(q.new_folder));
            pulse::ui::PickerInteractionTest interaction;
            if(!Wait([&]{interaction={};return Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&interaction)) && interaction.rename_edit!=nullptr;},state.done))return false;
            const auto draft=root/L"draft-cancelled";
            SendOnly(interaction.rename_edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"draft-cancelled"));
            Check(!std::filesystem::exists(draft),"New Folder name entry does not create on disk");
            Check(Capture(window,output/L"context-new-folder.png"),"capture provisional New Folder naming row");
            SendOnly(interaction.rename_edit,WM_KEYDOWN,VK_ESCAPE);
            Check(!std::filesystem::exists(draft),"Escape cancels New Folder without creation");
            if(!Geometry(window,sender,q))return false;Click(window,Center(q.new_folder));
            if(!Wait([&]{interaction={};return Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&interaction)) && interaction.rename_edit;},state.done))return false;
            SendOnly(interaction.rename_edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"a.txt"));
            SendOnly(interaction.rename_edit,WM_KEYDOWN,VK_RETURN);
            if(!Wait([&]{interaction={};return Geometry(window,sender,q)&&Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&interaction)) && !interaction.rename_edit;},state.done))return false;
            Check(interaction.current==root.wstring() && std::filesystem::is_regular_file(root/L"a.txt"),"New Folder conflict preserves existing file and current directory");
            Check(Capture(window,output/L"context-new-folder-conflict.png"),"capture New Folder conflict feedback");
            if(!Geometry(window,sender,q))return false;Click(window,Center(q.new_folder));
            if(!Wait([&]{interaction={};return Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&interaction)) && interaction.rename_edit;},state.done))return false;
            SendOnly(interaction.rename_edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"confirmed-folder"));
            SendOnly(interaction.rename_edit,WM_KEYDOWN,VK_RETURN);
            const auto confirmed=root/L"confirmed-folder";
            const bool entered=Wait([&]{interaction={};return Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&interaction)) && Geometry(window,sender,q) && interaction.current==confirmed.wstring();},state.done);
            if(!entered){std::wprintf(L"NEWFOLDER current=%ls expected=%ls exists=%u rename=%p ready=%u\n",interaction.current.c_str(),confirmed.c_str(),std::filesystem::exists(confirmed),interaction.rename_edit,interaction.ready);Capture(window,output/L"context-new-folder-failed.png");}
            Check(entered&&std::filesystem::is_directory(confirmed),"confirmed New Folder creates asynchronously and enters");
            if(!entered)return false;
            pulse::ui::PickerInteractionTest key;key.key='L';key.ctrl=true;
            if(!Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&key)))return false;
            SendOnly(GetDlgItem(window,101),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"C:\\"));
            key={};key.key=VK_RETURN;key.edit_id=101;
            Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&key));
            if(!Wait([&]{interaction={};return Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&interaction)) && interaction.ready && interaction.current==L"C:\\";},state.done))return false;
            return Capture(window,output/L"context-short-path.png");
        },"context filters and original New Folder complete then cancel");
    }
    if(run_case(L"hostmenu")) {
        const std::wstring encoding=L"Encoding (E):",tools=L"Tools",utf8=L"UTF-8",utf16=L"UTF-16",name=L"report.txt";
        PulsePickerUiV2ControlItem items[2]={{sizeof(PulsePickerUiV2ControlItem),10,3,0,Text(utf8)},
            {sizeof(PulsePickerUiV2ControlItem),20,3,0,Text(utf16)}};
        PulsePickerUiV2Control custom[2]{};
        for(unsigned i=0;i<2;++i){custom[i].size=sizeof(custom[i]);custom[i].id=300+i;custom[i].state=3;custom[i].items=items;custom[i].item_count=2;}
        custom[0].kind=PULSE_PICKER_UI_V2_CONTROL_COMBO;custom[0].label=Text(encoding);custom[0].has_selected=1;custom[0].selected_item_id=10;
        custom[1].kind=PULSE_PICKER_UI_V2_CONTROL_MENU;custom[1].label=Text(tools);
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;config.save_options=0;config.native_options=0;config.filename=Text(name);config.save_options=PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;config.controls=custom;config.control_count=2;
        Run(api,session,config,[&](HWND window,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;
            if(!Wait([&]{return Geometry(window,sender,q)&&q.option_count==1&&q.footer_tool.right>q.footer_tool.left;},state.done))return false;
            Check(Capture(window,output/L"host-combo-tools.png"),"capture host encoding label combo and real Tools menu");
            Click(window,Center(q.options[0]));
            if(!Wait([&]{return Menu()!=nullptr;},state.done))return false;
            PostMessageW(Menu(),WM_KEYDOWN,VK_DOWN,0);PostMessageW(Menu(),WM_KEYDOWN,VK_RETURN,0);
            if(!Wait([&]{return state.host_control==300 && state.host_item==20;},state.done))return false;
            Check(true,"host combo selects UTF-16 with exact ItemSelected id 20");
            Check(Capture(window,output/L"host-combo-selected.png"),"capture updated UTF-16 host combo value");
            if(!Geometry(window,sender,q))return false;
            Click(window,Center(q.footer_tool));
            if(!Wait([&]{return Menu()!=nullptr;},state.done))return false;
            PostMessageW(Menu(),WM_KEYDOWN,VK_DOWN,0);PostMessageW(Menu(),WM_KEYDOWN,VK_RETURN,0);
            const bool callback=Wait([&]{return state.host_control==301 && state.host_item==20;},state.done);
            Check(callback,"footer Tools preserves original host menu callback");return callback;
        },"host encoding combo and footer Tools then cancel");
    }
    if(run_case(L"recentthumb")) {
        const auto picture=root/L"recent-green.png";
        const unsigned char png[]={137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,64,0,0,0,64,8,6,0,0,0,170,105,113,222,0,0,0,154,73,68,65,84,120,156,237,208,49,13,0,32,0,192,48,148,160,14,157,88,3,25,37,97,199,254,102,99,238,117,126,110,104,128,174,1,26,160,107,128,6,232,26,160,1,186,6,104,128,174,1,26,160,107,128,6,232,26,160,1,186,6,104,128,174,1,26,160,107,128,6,232,26,160,1,186,6,104,128,174,1,26,160,107,128,6,232,26,160,1,186,6,104,128,174,1,26,160,107,128,6,232,26,160,1,186,6,104,128,174,1,26,160,107,128,6,232,26,160,1,186,6,104,128,174,1,26,160,107,128,6,232,26,160,1,186,6,104,128,174,1,26,160,107,128,6,232,26,48,31,64,200,1,23,160,129,242,14,25,72,94,49,0,0,0,0,73,69,78,68,174,66,96,130};
        {std::ofstream file(picture,std::ios::binary);file.write(reinterpret_cast<const char*>(png),sizeof(png));}
        WIN32_FILE_ATTRIBUTE_DATA metadata{};
        Check(GetFileAttributesExW(picture.c_str(),GetFileExInfoStandard,&metadata)!=FALSE,"thumbnail fixture metadata available");
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;config.save_options=0;config.native_options=0;config.filename={};config.controls=nullptr;config.control_count=0;
        Run(api,session,config,[&](HWND window,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;
            if(!Wait([&]{return Geometry(window,sender,q);},state.done))return false;
            const auto path=picture.wstring();q.recent_path=path.c_str();q.recent_attributes=metadata.dwFileAttributes;
            q.recent_modified=(static_cast<uint64_t>(metadata.ftLastWriteTime.dwHighDateTime)<<32)|metadata.ftLastWriteTime.dwLowDateTime;
            q.recent_size=(static_cast<uint64_t>(metadata.nFileSizeHigh)<<32)|metadata.nFileSizeLow;
            if(!Send(window,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q)))return false;
            const bool bitmap=Wait([&]{q={};return Geometry(window,sender,q)&&q.recent_bitmap_draws>0;},state.done);
            std::printf("RECENT_BITMAP_DRAWS=%u\n",q.recent_bitmap_draws);
            Check(bitmap,"recent card without HBITMAP obtains real isolated-cache bitmap");
            Check(Capture(window,output/L"recent-thumbnail.png"),"capture real recent thumbnail card");
            return bitmap;
        },"isolated recent thumbnail cache completes then cancel");
    }
    if(run_case(L"extension")) {
        const std::wstring text_label=L"Text",image_label=L"Images",all_label=L"All files",text_pattern=L"*.txt",image_pattern=L"*.png;*.jpg",all_pattern=L"*.*";
        PulsePickerUiV2Filter filters[3]={{sizeof(PulsePickerUiV2Filter),0,Text(text_label),Text(text_pattern)},
            {sizeof(PulsePickerUiV2Filter),0,Text(image_label),Text(image_pattern)},
            {sizeof(PulsePickerUiV2Filter),0,Text(all_label),Text(all_pattern)}};
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;config.save_options=PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;config.native_options=0;
        config.filename=Text(leaf);config.controls=nullptr;config.control_count=0;config.properties=nullptr;config.property_count=0;
        config.filters=filters;config.filter_count=3;config.filter_index=0;
        std::filesystem::create_directory(root/L"child");
        Run(api,session,config,[&](HWND window,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(window,sender,q);},state.done))return false;
            const auto initial_type_events=state.filter_events.load();
            state.replace_filename_on_type=true;
            SendOnly(GetDlgItem(window,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"report.PNG"));
            if(!Wait([&]{return state.filter_index==1 && state.filter_events==initial_type_events+1 && Filename(window)==L"host-owned.txt" && Geometry(window,sender,q);},state.done)){
                std::wprintf(L"EXTENSION first events=%u index=%u filename=%ls reentry_pending=%u\n",state.filter_events.load(),state.filter_index.load(),Filename(window).c_str(),state.replace_filename_on_type.load());
                Check(Capture(window,output/L"extension-first.png"),"capture extension first-change diagnostic");return false;
            }
            Check(true,"extension type change preserves callback-owned filename without recursive type change");
            SendOnly(GetDlgItem(window,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"user.txt"));
            if(!Wait([&]{return state.filter_index==0 && state.filter_events==initial_type_events+2 && Geometry(window,sender,q);},state.done))return false;
            Check(Filename(window)==L"user.txt","later user edit resumes unique extension synchronization");
            SendOnly(GetDlgItem(window,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"quick.JPG"));
            SendOnly(GetDlgItem(window,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"quick.txt"));
            SendOnly(GetDlgItem(window,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"child\\final.PNG"));
            if(!Wait([&]{return state.filter_index==1 && state.filter_events==initial_type_events+5 && Filename(window)==L"child\\final.PNG" && Geometry(window,sender,q);},state.done))return false;
            Check(true,"rapid filename edits finish with latest relative leaf type and unchanged input");
            Check(Capture(window,output/L"extension-sync.png"),"capture synchronized save type and unchanged explicit extension");
            SendOnly(GetDlgItem(window,102),WM_KEYDOWN,VK_RETURN);return Wait([&]{return !IsWindow(window);},state.done);
        },"typed extension synchronization reaches original FileOk save path and exact type index",(root/L"child"/L"final.PNG").wstring(),1);
        const std::wstring duplicate_label=L"Other PNG",duplicate_pattern=L"*.PNG";
        PulsePickerUiV2Filter ambiguous[3]={filters[0],filters[1],{sizeof(PulsePickerUiV2Filter),0,Text(duplicate_label),Text(duplicate_pattern)}};
        // Arbitrary wildcard Save configs are intentionally outside this ABI;
        // that matching boundary is covered by the pure model test instead.
        config.filters=ambiguous;config.filter_count=3;config.filter_index=0;
        Run(api,session,config,[&](HWND window,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(window,sender,q);},state.done))return false;
            const auto initial_type_events=state.filter_events.load();
            for(const auto* input:{L"report.png",L".hidden.png",L"report",L"report.",L"report.unknown"}) {
                SendOnly(GetDlgItem(window,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(input));
                if(!Wait([&]{return Geometry(window,sender,q);},state.done))return false;
                std::wprintf(L"EXTENSION ambiguous events=%u index=%u input=%ls filename=%ls\n",state.filter_events.load(),state.filter_index.load(),input,Filename(window).c_str());
                Check(state.filter_events==initial_type_events && state.filter_index==0 && Filename(window)==input,"ambiguous or incomplete extension never guesses caller format or rewrites input");
            }
            return true;
        },"ambiguous extension input keeps caller type until cancellation");
    }
    Check(api.Shutdown(session)==S_OK,"real picker session shuts down");CoUninitialize();
    std::printf("FAILURES=%u\n",failures.load());return failures?1:0;
}

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
static std::atomic<int> installed{0}, removed{0};
static HHOOK WINAPI FakeInstall(int,HOOKPROC,HINSTANCE,DWORD) {++installed;return reinterpret_cast<HHOOK>(1);}
static LRESULT WINAPI TestDispatch(const MSG* message) {
    if(message->message==WM_APP+0x111 && message->lParam) {
        reinterpret_cast<TIMERPROC>(message->lParam)(nullptr,WM_TIMER,0,0);return 0;
    }
    return DispatchMessageW(message);
}
static BOOL WINAPI FakeRemove(HHOOK) {++removed;return TRUE;}
#define DispatchMessageW TestDispatch
#define SetWindowsHookExW FakeInstall
#define UnhookWindowsHookEx FakeRemove
#include "../ui/preview_handler_pan.h"
#undef DispatchMessageW
#undef SetWindowsHookExW
#undef UnhookWindowsHookEx
namespace pulse::ui {
struct PreviewHandlerPanTest {
    static void CALLBACK Begin(HWND,UINT,UINT_PTR,DWORD) {
        auto* input=PreviewHandlerPan::current_input_;
        input->suppress_left_up=true;
        input->desired_hook=true;
        PostThreadMessageW(GetCurrentThreadId(),PreviewHandlerPan::kHookCommand,0,0);
    }
    static void CALLBACK Release(HWND,UINT,UINT_PTR,DWORD) {
        MSLLHOOKSTRUCT mouse{};
        PreviewHandlerPan::MouseHook(HC_ACTION,WM_LBUTTONUP,reinterpret_cast<LPARAM>(&mouse));
    }
    struct Start { HDESK desktop;std::shared_ptr<PreviewHandlerPan::InputState> state; };
    static DWORD WINAPI Main(void* ptr) {
        std::unique_ptr<Start> start(static_cast<Start*>(ptr));
        if(!SetThreadDesktop(start->desktop)) return 2;
        return PreviewHandlerPan::InputMain(new std::shared_ptr<PreviewHandlerPan::InputState>(start->state));
    }
    static int Run(HDESK desktop) {
        auto wait=[](auto pred){const auto end=GetTickCount64()+2000;while(!pred()&&GetTickCount64()<end)Sleep(5);return pred();};
        int failures=0;
        auto check=[&](bool ok,const char* text){printf("[%s] %s\n",ok?"PASS":"FAIL",text);failures+=!ok;};
        PreviewHandlerPan pan;
        pan.input_=std::make_shared<PreviewHandlerPan::InputState>();
        auto state=pan.input_;
        pan.RequestHook(true);pan.RequestHook(false);
        state->thread=CreateThread(nullptr,0,Main,new Start{desktop,state},0,nullptr);
        check(state->thread && wait([&]{return state->thread_id.load()!=0;}),"private input thread starts");
        Sleep(30);
        check(installed==0,"enable-disable before queue creation does not install idle hook");
        PostThreadMessageW(state->thread_id,WM_APP+0x111,1,reinterpret_cast<LPARAM>(Begin));
        check(wait([&]{return installed==1;}),"real input loop installs hook adapter");
        pan.Disable();Sleep(50);
        check(removed==0,"disable preserves hook while swallowed gesture awaits release");
        PostThreadMessageW(state->thread_id,WM_APP+0x111,1,reinterpret_cast<LPARAM>(Release));
        check(wait([&]{return removed==1;}),"matching release reaches input callback then removes hook");
        state->stop=true;PostThreadMessageW(state->thread_id,WM_NULL,0,0);
        check(WaitForSingleObject(state->thread,2000)==WAIT_OBJECT_0,"input thread exits after balanced gesture");
        return failures;
    }
};
}
int wmain(){
    const auto name=L"PulsePanAudit-"+std::to_wstring(GetCurrentProcessId());
    HDESK desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!desktop)return 2;
    int failures=pulse::ui::PreviewHandlerPanTest::Run(desktop);
    CloseDesktop(desktop);return failures?1:0;
}

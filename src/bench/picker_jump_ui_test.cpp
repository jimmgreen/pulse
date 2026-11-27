#if defined(PULSE_PICKER_TRAY_CONSUME_TEST)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00
#endif
#include <ole2.h>
#include "../picker_inprocess/picker_ui_api_v2.h"
#include "picker_interaction_test_api.h"
#include "picker_jump_ui_test_api.h"
#include "picker_places_test_api.h"
#include "picker_view_switch_test_api.h"
#include "../common/picker_save_request.h"
#include "../common/picker_tag_catalog_shared.h"
#include <shobjidl.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef PULSE_PICKER_TRAY_CONSUME_TEST
#include "picker_tray_ui_fixture.h"
#endif

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
    std::atomic<unsigned> host_item=0, host_control=0, folder_changed=0;
    std::atomic<unsigned> checks=0,edits=0,radio=0,properties=0; std::atomic<unsigned> last_control=0,file_ok=0;
    PulsePickerUiV2Api* api=nullptr;PulsePickerUiV2Session session=0;
    std::atomic<unsigned> filter_index=0,filter_events=0;
    std::atomic<bool> replace_filename_on_type=false, veto_file_ok=false;
    std::thread driver; std::function<bool(HWND,HWND,State&)> drive; bool driven=false, expect_accept=false;
    std::function<void(const PulsePickerUiV2Event&)> ready_action;
    std::function<void(const PulsePickerUiV2Event&)> file_ok_action;
};
HRESULT WINAPI Event(void* context,const PulsePickerUiV2Event* event) {
    auto& state=*static_cast<State*>(context);
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGED) ++state.folder_changed;
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED){state.host_control=event->control_id;state.host_item=event->item_id;}
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_CHECK_TOGGLED){state.last_control=event->control_id;++state.checks;}
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_FILE_OK){++state.file_ok;if(state.file_ok_action)state.file_ok_action(*event);if(state.veto_file_ok)return S_FALSE;}
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
        if (state.ready_action) state.ready_action(*event);
        const auto desktop=GetThreadDesktop(GetCurrentThreadId()); const auto window=event->window;
        state.driver=std::thread([&state,desktop,window] {
            SetThreadDesktop(desktop);
            SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(window));
            HWND sender=CreateWindowExW(0,L"STATIC",L"Jump test sender",0,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            state.driven=sender && state.drive(window,sender,state);
            // FileOk is followed by asynchronous final access/identity checks.
            // Do not turn a successful candidate into a cancellation during those checks.
            if (state.driven && state.expect_accept) {
                const auto deadline=std::chrono::steady_clock::now()+10s;
                while(!state.done && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(10ms);
            }
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
         const std::function<bool(HWND,HWND,State&)>& drive,const char* label,const std::wstring& expected={},uint32_t expected_filter=UINT32_MAX,
         std::function<void(const PulsePickerUiV2Event&)> ready_action={}, uint32_t expected_count=0,
         uint32_t packed_capacity=0, HRESULT expected_error=S_OK,
         std::function<bool(const PulsePickerUiV2Result&)> validate_result={},
         std::function<void(const PulsePickerUiV2Event&)> file_ok_action={},
         uint32_t expected_failure_status=PULSE_PICKER_UI_V2_FAILED) {
    const auto validated=api.ValidateConfig(session,&config);
    std::printf("CONFIG validate=%08lX access=%u options=%u save=%u controls=%u properties=%u\n",static_cast<unsigned long>(validated),config.access_policy,config.open_options,config.save_options,config.control_count,config.property_count);
    State state;state.drive=drive;state.api=&api;state.session=session;state.expect_accept=!expected.empty() || static_cast<bool>(validate_result) || FAILED(expected_error);
    state.ready_action=std::move(ready_action);
    state.file_ok_action=std::move(file_ok_action);
    PulsePickerUiV2Callbacks callbacks{sizeof(callbacks),0,Event,&state};
    std::vector<wchar_t> path(PULSE_PICKER_UI_V2_MAX_UNITS+1),paths(PULSE_PICKER_UI_V2_MAX_PACKED_UNITS+1);
    PulsePickerUiV2Result result{};result.size=sizeof(result);result.path={path.data(),static_cast<uint32_t>(path.size())};result.paths={paths.data(),static_cast<uint32_t>(paths.size())};
    if(packed_capacity)result.paths.capacity=packed_capacity;
    const auto hr=api.Show(session,&config,&callbacks,&result);
    state.done=true;if(state.driver.joinable())state.driver.join();
    std::printf("SHOW hr=%08lX status=%u driven=%u\n",static_cast<unsigned long>(hr),result.status,state.driven);
    if(FAILED(expected_error)) {
        Check(state.driven&&state.file_ok==1&&hr==expected_error&&result.status==expected_failure_status&&result.path_count==0,label);
        return;
    }
    if(validate_result) {
        Check(state.driven && hr==S_OK && result.status==PULSE_PICKER_UI_V2_ACCEPTED && state.file_ok==1 &&
            (expected_filter==UINT32_MAX || result.filter_index==expected_filter) &&
            (!expected_count || result.path_count==expected_count) && validate_result(result),label);
        return;
    }
    Check(state.driven && (expected.empty() ? result.status==PULSE_PICKER_UI_V2_CANCELLED :
        result.status==PULSE_PICKER_UI_V2_ACCEPTED && state.file_ok==1 && std::wstring(path.data())==expected &&
        (expected_filter==UINT32_MAX || result.filter_index==expected_filter) && (!expected_count || result.path_count==expected_count)),label);
}

class SaveFollowupEndpoint {
public:
    SaveFollowupEndpoint() {
        worker_ = std::thread([this] {
            WNDCLASSW type{};
            type.lpfnWndProc = WindowProc;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = L"PulseSaveFollowupTestEndpoint";
            RegisterClassW(&type);
            const HWND window = CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0,
                HWND_MESSAGE, nullptr, type.hInstance, this);
            window_.store(window);
            started_.store(true);
            if (!window) return;
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            window_.store(nullptr);
        });
        while (!started_.load()) std::this_thread::sleep_for(1ms);
    }
    ~SaveFollowupEndpoint() {
        if (const HWND window = window_.load()) PostMessageW(window, WM_CLOSE, 0, 0);
        if (worker_.joinable()) worker_.join();
    }
    HWND Window() const { return window_.load(); }
    std::vector<pulse::picker_save::Request> Requests() {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    std::vector<pulse::picker_save::Completion> Drain() { return observer_.Drain(); }
    unsigned Rejected() const { return rejected_.load(); }
private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<SaveFollowupEndpoint*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<SaveFollowupEndpoint*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_COPYDATA && self) {
            pulse::picker_save::Request request;
            const auto* packet = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            DWORD sender_pid = 0;
            GetWindowThreadProcessId(reinterpret_cast<HWND>(wp), &sender_pid);
            if (!pulse::picker_save::Decode(packet, request) || sender_pid != request.source_pid ||
                !self->observer_.Submit(request)) {
                ++self->rejected_;
                return FALSE;
            }
            std::lock_guard lock(self->mutex_);
            self->requests_.push_back(std::move(request));
            return TRUE;
        }
        if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, wp, lp);
    }
    pulse::picker_save::Observer observer_;
    std::mutex mutex_;
    std::vector<pulse::picker_save::Request> requests_;
    std::atomic<HWND> window_{nullptr};
    std::atomic<bool> started_{false};
    std::atomic<unsigned> rejected_{0};
    std::thread worker_;
};

bool FollowupWait(const std::function<bool()>& predicate, std::chrono::milliseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < end);
    return predicate();
}

void SaveFollowupCases(PulsePickerUiV2Api& api, PulsePickerUiV2Session session,
    const std::filesystem::path& root, const std::filesystem::path& output) {
    const std::wstring folder = root.wstring();
    for (unsigned variant = 0; variant < 7; ++variant) {
        SaveFollowupEndpoint endpoint;
        Check(endpoint.Window() != nullptr, "save follow-up owns an isolated message-only endpoint");
        if (!endpoint.Window()) continue;
        const bool existing = variant == 2;
        const bool veto = variant == 4;
        const bool cancel = variant == 5;
        const bool short_buffer = variant == 6;
        const bool accepts = variant < 4;
        const bool writes = variant == 1 || existing;
        const bool notify = variant != 0 && variant != 3;
        const bool copy = variant != 0;
        const auto leaf = L"followup-" + std::to_wstring(variant) + L".txt";
        const auto path = (root / leaf).wstring();
        if (existing) std::ofstream(path, std::ios::binary) << "existing baseline";
        pulse::picker_save::Stamp expected_before;
        Check(pulse::picker_save::Capture(path, expected_before) == ERROR_SUCCESS,
            "caller-side reference baseline captured outside picker UI thread");
        PulsePickerUiV2Config config{};
        config.size = sizeof(config); config.version = 2;
        config.initial_folder = Text(folder); config.filename = Text(leaf);
        const std::wstring text_label = L"Text", png_label = L"PNG images", all_label = L"All files";
        const std::wstring text_pattern = L"*.txt", png_pattern = L"*.png", all_pattern = L"*.*";
        PulsePickerUiV2Filter filters[] = {
            {sizeof(PulsePickerUiV2Filter), 0, Text(text_label), Text(text_pattern)},
            {sizeof(PulsePickerUiV2Filter), 0, Text(png_label), Text(png_pattern)},
            {sizeof(PulsePickerUiV2Filter), 0, Text(all_label), Text(all_pattern)}
        };
        config.filters = filters; config.filter_count = static_cast<uint32_t>(std::size(filters));
        config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
        config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
        config.appearance = existing || variant == 3 ? PULSE_PICKER_UI_V2_DARK : 0;
        auto drive = [&](HWND window, HWND sender, State& state) {
            state.veto_file_ok = veto;
            pulse::ui::PickerJumpUiTest q;
            if (!Wait([&] { return Geometry(window, sender, q); }, state.done)) return false;
            Check(!q.save_notify && !q.save_copy, "save follow-up options default off in isolated session");
            q = {}; q.save_followup_endpoint = endpoint.Window();
            if (!Send(window, pulse::ui::kPickerJumpUiTestMessage,
                reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q))) return false;
            if (!Wait([&] { return Geometry(window, sender, q) && q.save_followup_available &&
                q.save_notify_option.right > q.save_notify_option.left &&
                q.save_copy_option.right > q.save_copy_option.left; }, state.done)) return false;
            if (variant == 3) {
                q = {}; q.client_width_dip = 640;
                if (!Send(window, pulse::ui::kPickerJumpUiTestMessage,
                    reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q))) return false;
                if (!Wait([&] {
                    RECT bounds{}; GetClientRect(window, &bounds);
                    const UINT dpi = GetDpiForWindow(window);
                    return Geometry(window, sender, q) && bounds.right >= MulDiv(639, static_cast<int>(dpi), 96) &&
                        bounds.right <= MulDiv(641, static_cast<int>(dpi), 96);
                }, state.done)) return false;
            }
            if (notify) {
                Click(window, Center(q.save_notify_option));
                if (!Wait([&] { return Geometry(window, sender, q) && q.save_notify; }, state.done)) return false;
            }
            if (copy) {
                Click(window, Center(q.save_copy_option));
                if (!Wait([&] { return Geometry(window, sender, q) && q.save_copy; }, state.done)) return false;
            }
            Check(q.save_notify == notify && q.save_copy == copy,
                "actual save option chip clicks set independent request flags");
            RECT client{}, overlap{};
            GetClientRect(window, &client);
            const auto visible = [&](RECT rect) {
                return rect.right > rect.left && rect.bottom > rect.top && rect.left >= 0 && rect.top >= 0 &&
                    rect.right <= client.right && rect.bottom <= client.bottom;
            };
            Check(visible(q.save_notify_option) && visible(q.save_copy_option) &&
                !IntersectRect(&overlap, &q.save_notify_option, &q.save_copy_option),
                "save follow-up option hit rectangles remain visible and nonoverlapping");
            if (accepts) {
                const wchar_t* screenshot = variant == 0 ? L"save-followup-default.png" :
                    variant == 1 ? L"save-followup-light.png" :
                    variant == 2 ? L"save-followup-dark.png" : L"save-followup-dark-narrow.png";
                Check(Capture(window, output / screenshot), "capture actual save follow-up options state");
            }
            Check(endpoint.Requests().empty(), "option clicks never send a save observation request");
            const auto diagnose_overwrite = [&](const char* stage) {
                const bool geometry = Geometry(window, sender, q);
                const auto filename = Filename(window);
                std::printf("SAVE_FOLLOWUP_OVERWRITE stage=%s geometry=%u ready=%u draft_checked=%u live_conflict=%u primary_enabled=%u "
                    "live_save=%ld,%ld,%ld,%ld overwrite=%ld,%ld,%ld,%ld filename=%ls save_options=%u native_options=%u access=%u\n",
                    stage, geometry, q.ready, q.save_draft_checked, q.live_conflict, q.primary_enabled,
                    q.live_save.left, q.live_save.top, q.live_save.right, q.live_save.bottom,
                    q.overwrite_replace.left, q.overwrite_replace.top, q.overwrite_replace.right, q.overwrite_replace.bottom,
                    filename.c_str(), config.save_options, config.native_options, config.access_policy);
                std::fflush(stdout);
                Check(Capture(window, output / L"overwrite-timeout.png"), "capture failed overwrite stage without changing picker state");
            };
            if (existing) {
                if (!Wait([&] { return Geometry(window, sender, q) && q.live_conflict &&
                    q.live_save.right > q.live_save.left; }, state.done)) {
                    diagnose_overwrite("wait-live-conflict"); return false;
                }
                std::puts("SAVE_FOLLOWUP_OVERWRITE stage=click-live-save"); std::fflush(stdout);
                Click(window, Center(q.live_save));
                if (!Wait([&] {
                    q = {};
                    return Send(window, pulse::ui::kPickerJumpUiTestMessage,
                        reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&q)) &&
                        q.overwrite_replace.right > q.overwrite_replace.left;
                }, state.done)) {
                    diagnose_overwrite("wait-final-overwrite"); return false;
                }
                Check(state.file_ok == 0 && endpoint.Requests().empty() &&
                    std::filesystem::file_size(path) == expected_before.size,
                    "live Check and save preserves final overwrite confirmation without writing or submitting follow-up");
                Check(Capture(window, output / L"save-followup-overwrite.png"),
                    "capture actual dark save follow-up final overwrite confirmation");
                Click(window, Center(q.overwrite_replace));
            } else if (!SendOnly(GetDlgItem(window, 102), WM_KEYDOWN, VK_RETURN)) return false;
            if (!Wait([&] { return state.file_ok == 1; }, state.done)) {
                if (existing) diagnose_overwrite("wait-file-ok-after-replace");
                return false;
            }
            if (veto) Check(IsWindow(window) && endpoint.Requests().empty(),
                "FileOk veto leaves picker open without save follow-up submission");
            return true;
        };
        auto file_ok = [&](const PulsePickerUiV2Event& event) {
            Check(endpoint.Requests().empty(), "save request is absent at FileOk before final result delivery");
            if (cancel) Check(api.Close(session, event.show_id, HRESULT_FROM_WIN32(ERROR_CANCELLED)) == S_OK,
                "save follow-up caller cancels through real Close at FileOk");
        };
        std::function<bool(const PulsePickerUiV2Result&)> validate;
        if (accepts) validate = [&](const PulsePickerUiV2Result& result) {
            return result.path_count == 1 && result.path.data && std::wstring(result.path.data) == path;
        };
        Run(api, session, config, drive,
            accepts ? "save result delivers exact path before application writes" :
            veto ? "vetoed save cancels without delivering a result" :
            cancel ? "Close preserves CLOSED status and returns no save path" :
            "packed result capacity failure returns no save path",
            {}, UINT32_MAX, {}, accepts ? 1 : 0, short_buffer ? 1 : 0,
            short_buffer ? HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) :
            cancel ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK,
            std::move(validate), file_ok, cancel ? PULSE_PICKER_UI_V2_CLOSED : PULSE_PICKER_UI_V2_FAILED);
        if (!accepts) {
            Check(!FollowupWait([&] { return !endpoint.Requests().empty(); }, 1800ms) &&
                endpoint.Rejected() == 0 && endpoint.Drain().empty(),
                "veto cancel and packed-buffer failure send no save request or completion");
            Check(!std::filesystem::exists(path), "failed save delivery never creates a target file");
            continue;
        }
        const bool received = FollowupWait([&] { return !endpoint.Requests().empty(); }, 5000ms);
        const auto requests = endpoint.Requests();
        Check(received && requests.size() == 1 && endpoint.Rejected() == 0,
            "delivered save result sends exactly one valid WM_COPYDATA observation request");
        if (requests.size() != 1) continue;
        const auto& request = requests.front();
        Check(request.path == path && request.source_pid == GetCurrentProcessId() && request.request_id &&
            request.notify_if_no_tab == notify && request.copy_path == copy,
            "decoded request preserves returned path process ID and actual option flags");
        Check(request.before.exists == expected_before.exists && request.before.volume == expected_before.volume &&
            request.before.file_id == expected_before.file_id && request.before.size == expected_before.size &&
            request.before.write_time == expected_before.write_time,
            "request carries exact absent or existing baseline from before result delivery");
        Check(endpoint.Drain().empty(), "returning a path alone is not a completed save");
        if (!writes) {
            Check(!FollowupWait([&] { return !endpoint.Drain().empty(); }, 1800ms),
                "returned path without a writer remains incomplete past default quiet interval");
            Check(!std::filesystem::exists(path), "observer never creates unwritten selected target");
            continue;
        }
        HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(writer != INVALID_HANDLE_VALUE, "application opens real writer only after successful result delivery");
        if (writer == INVALID_HANDLE_VALUE) continue;
        DWORD written = 0;
        Check(WriteFile(writer, "first", 5, &written, nullptr) && written == 5, "application writes first chunk");
        Check(!FollowupWait([&] { return !endpoint.Drain().empty(); }, 1800ms),
            "live application writer prevents completion beyond default quiet interval");
        Check(WriteFile(writer, "second", 6, &written, nullptr) && written == 6, "application writes second chunk");
        Check(!FollowupWait([&] { return !endpoint.Drain().empty(); }, 300ms),
            "second chunk with held writer still does not complete");
        CloseHandle(writer);
        std::vector<pulse::picker_save::Completion> completed;
        const bool finished = FollowupWait([&] { completed = endpoint.Drain(); return !completed.empty(); }, 5000ms);
        Check(finished && completed.size() == 1 && completed[0].request.request_id == request.request_id &&
            completed[0].after.exists && completed[0].after.size == 11,
            "real save completes once only after writer closes and metadata remains quiet");
        Check(!FollowupWait([&] { return !endpoint.Drain().empty(); }, 250ms),
            "completed save never emits a duplicate observation");
    }
}

int ArchiveCases(PulsePickerUiV2Api& api, PulsePickerUiV2Session session,
                 const std::filesystem::path& root, const std::filesystem::path& output) {
    const auto fixture=output.parent_path()/L"archive-ui-fixtures"/L"sample.zip";
    const auto worker=std::filesystem::absolute(L"build/scroll-preview-7zip-d7f39c03/plugin/pulse_7zip_worker.exe");
    const auto read=[](const std::filesystem::path& path) {
        std::ifstream stream(path,std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>());
    };
    if(!std::filesystem::is_regular_file(fixture)||!std::filesystem::is_regular_file(worker)) {
        Check(false,"real ZIP fixture and shipped 7-Zip worker exist");return 1;
    }
    const auto archive=root/L"sample.zip";
    std::filesystem::copy_file(fixture,archive);
    const auto before=read(archive);
    const auto archive_path=archive.wstring(),worker_path=worker.wstring(),folder=root.wstring();
    const std::wstring label=L"Text documents",pattern=L"*.txt";
    PulsePickerUiV2Filter filter{sizeof(PulsePickerUiV2Filter),0,Text(label),Text(pattern)};
    const auto empty=[](const std::filesystem::path& path) {
        std::error_code ec;
        return !std::filesystem::exists(path,ec)||std::filesystem::is_empty(path,ec);
    };
    const auto click_sync=[](HWND window,POINT point) {
        const LPARAM coordinates=MAKELPARAM(point.x,point.y);
        return SendOnly(window,WM_MOUSEMOVE,0,coordinates)&&
            SendOnly(window,WM_LBUTTONDOWN,MK_LBUTTON,coordinates)&&
            SendOnly(window,WM_LBUTTONUP,0,coordinates);
    };
    const auto diagnose=[](HWND window,HWND sender,const char* stage) {
        pulse::ui::PickerInteractionTest state;
        Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&state));
        pulse::ui::PickerJumpUiTest geometry;Geometry(window,sender,geometry);
        std::printf("ARCHIVE_STAGE %s current=%ls ready=%u selected=%zu hit_down=%d hit_up=%d activate=%d rows=%u archive_enabled=%u\n",
            stage,state.current.c_str(),state.ready,state.selected_paths.size(),geometry.last_down_hit,
            geometry.last_up_hit,geometry.last_activate,geometry.display_count,geometry.archive_enabled);
        for(const auto& selected:state.selected_paths)std::printf("ARCHIVE_SELECTED %ls\n",selected.c_str());
        for(unsigned i=0;i<(std::min)(geometry.display_count,32u);++i)
            std::printf("ARCHIVE_ROW %u raw=%ls drawn=%ls archive=%u\n",i,geometry.raw_names[i],geometry.drawn_names[i],geometry.archive_rows[i]);
        std::fflush(stdout);
    };
    const auto double_click=[&](HWND window,HWND sender,State& state,const std::wstring& path) {
        pulse::ui::PickerPlacesTestPoint row;row.path=path.c_str();
        if(!Send(window,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&row)))return false;
        std::printf("ARCHIVE_DOUBLE_CLICK target=%ls point=%ld,%ld\n",path.c_str(),row.point.x,row.point.y);std::fflush(stdout);
        if(!click_sync(window,row.point))return false;
        const bool selected=Wait([&] {
            pulse::ui::PickerInteractionTest selected;
            return Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&selected))&&
                std::find(selected.selected_paths.begin(),selected.selected_paths.end(),path)!=selected.selected_paths.end();
        },state.done);
        diagnose(window,sender,"after-first-click");
        Check(selected,"first synchronous click selects exact requested row before double-click");
        if(!selected)return false;
        if(!Send(window,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&row)))return false;
        if(!SendOnly(window,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(row.point.x,row.point.y)))return false;
        return SendOnly(window,WM_LBUTTONUP,0,MAKELPARAM(row.point.x,row.point.y));
    };
    for(unsigned variant=0;variant<5;++variant) {
        const bool accepted=variant<2,veto=variant==2,cancel=variant==3,short_buffer=variant==4;
        const auto staging=output/(L"archive-staging-"+std::to_wstring(variant));
        const auto staging_path=staging.wstring();
        std::wstring candidate_path,accepted_path;
        std::atomic<unsigned> stage=0;
        PulsePickerUiV2Config config{};config.size=sizeof(config);config.version=2;
        config.initial_folder=Text(folder);config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
        config.filters=&filter;config.filter_count=1;config.native_options=FOS_STRICTFILETYPES;
        config.appearance=variant==1?PULSE_PICKER_UI_V2_DARK:0;
        if(variant==1)config.open_options=PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT;
        auto drive=[&](HWND window,HWND sender,State& state) {
            state.veto_file_ok=veto;
            pulse::ui::PickerJumpUiTest geometry;
            if(!Wait([&]{return Geometry(window,sender,geometry);},state.done))return false;
            pulse::ui::PickerJumpUiTest runtime;runtime.archive_worker=worker_path.c_str();runtime.archive_staging=staging_path.c_str();runtime.client_width_dip=variant==1?720.0f:0.0f;
            if(!Send(window,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&runtime)))return false;
            if(!Wait([&]{return Geometry(window,sender,geometry)&&HasRow(window,archive_path);},state.done))return false;
            Check(true,"TXT caller filter still exposes actual outer ZIP row");
            diagnose(window,sender,"outer-ready");
            Check(Capture(window,output/(L"archive-stage-"+std::to_wstring(variant)+L"-outer.png")),"capture outer archive row before double-click");
            stage=1;
            if(!double_click(window,sender,state,archive_path))return false;
            const auto at=[&](const std::wstring& path) {
                pulse::ui::PickerInteractionTest q;
                return Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&q))&&q.ready&&q.current==path;
            };
            if(!Wait([&]{return at(archive_path)&&HasRow(window,archive_path+L"\\root.txt");},state.done)) {
                diagnose(window,sender,"archive-root-timeout");return false;
            }
            diagnose(window,sender,"archive-root-ready");
            Check(Capture(window,output/(L"archive-stage-"+std::to_wstring(variant)+L"-root.png")),"capture entered archive root");
            Check(!HasRow(window,archive_path+L"\\hidden.png")&&HasRow(window,archive_path+L"\\sub"),"archive listing applies TXT filter while keeping virtual directories");
            stage=2;
            if(!double_click(window,sender,state,archive_path+L"\\sub"))return false;
            if(!Wait([&]{return at(archive_path+L"\\sub")&&HasRow(window,archive_path+L"\\sub\\nested.txt");},state.done))return false;
            diagnose(window,sender,"archive-subdirectory-ready");
            Check(Capture(window,output/(L"archive-stage-"+std::to_wstring(variant)+L"-sub.png")),"capture entered archive subdirectory");
            stage=3;
            if(!SendOnly(window,WM_SYSKEYDOWN,VK_UP,static_cast<LPARAM>(1u<<29)))return false;
            if(!Wait([&]{return at(archive_path)&&HasRow(window,archive_path+L"\\root.txt");},state.done))return false;
            Check(state.file_ok==0,"double-click navigation and Alt-Up never submit virtual directory");
            if(accepted)Check(Capture(window,output/(variant?L"archive-dark.png":L"archive-light.png")),"capture actual archive listing theme");
            const auto selected=archive_path+L"\\root.txt";
            pulse::ui::PickerPlacesTestPoint row;row.path=selected.c_str();
            if(!Send(window,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&row)))return false;
            stage=4;
            if(!click_sync(window,row.point))return false;
            if(!Wait([&] {
                pulse::ui::PickerInteractionTest chosen;
                return Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&chosen))&&
                    chosen.selected_paths.size()==1&&chosen.selected_paths.front()==selected;
            },state.done))return false;
            diagnose(window,sender,"selected-member-before-submit");
            pulse::ui::PickerInteractionTest q;
            if(!Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&q)))return false;
            stage=5;
            if(!click_sync(window,Center(q.primary)))return false;
            if(!Wait([&]{return state.file_ok==1;},state.done))return false;
            if(veto) {
                const bool cleaned=Wait([&]{return empty(staging);},state.done);
                Check(cleaned&&IsWindow(window),"FileOk veto releases extracted result while picker stays open");
                return cleaned;
            }
            return true;
        };
        auto on_file_ok=[&](const PulsePickerUiV2Event& event) {
            std::vector<wchar_t> buffer(32768),current(32768),filename(32768),selection(32768),selected(32768),candidates(32768);
            PulsePickerUiV2Snapshot snapshot{};snapshot.size=sizeof(snapshot);snapshot.candidate={buffer.data(),static_cast<uint32_t>(buffer.size())};
            snapshot.folder={current.data(),32768};snapshot.filename={filename.data(),32768};snapshot.selection={selection.data(),32768};
            snapshot.selection_paths={selected.data(),32768};snapshot.candidate_paths={candidates.data(),32768};
            const auto hr=api.Query(session,event.show_id,event.generation,&snapshot);
            candidate_path=buffer.data();
            std::printf("ARCHIVE_FILE_OK variant=%u stage=%u hr=%08lX folder=%ls filename=%ls selection=%ls candidate=%ls candidates=%u\n",
                variant,stage.load(),static_cast<unsigned long>(hr),current.data(),filename.data(),selection.data(),candidate_path.c_str(),snapshot.candidate_count);
            std::fflush(stdout);
            Check(hr==S_OK&&!candidate_path.empty()&&candidate_path.starts_with(staging_path+L"\\")&&
                read(candidate_path)=="archive-ui-root\n","FileOk receives real materialized local text file, never archive virtual path");
            if(cancel)Check(api.Close(session,event.show_id,HRESULT_FROM_WIN32(ERROR_CANCELLED))==S_OK,"caller cancellation at FileOk is accepted");
        };
        std::function<bool(const PulsePickerUiV2Result&)> validate;
        if(accepted)validate=[&](const PulsePickerUiV2Result& result) {
            accepted_path=result.path.data;
            return accepted_path==candidate_path&&read(accepted_path)=="archive-ui-root\n";
        };
        Run(api,session,config,drive,accepted?"archive selection returns retained readable local file":
            veto?"vetoed archive selection then cancels cleanly":cancel?"cancelled archive delivery returns no selection":"insufficient packed buffer fails archive delivery atomically",
            {},0,{},accepted?1:0,short_buffer?1:0,short_buffer?HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER):cancel?HRESULT_FROM_WIN32(ERROR_CANCELLED):S_OK,std::move(validate),on_file_ok,
            cancel?PULSE_PICKER_UI_V2_CLOSED:PULSE_PICKER_UI_V2_FAILED);
        if(accepted)Check(!accepted_path.empty()&&read(accepted_path)=="archive-ui-root\n","accepted archive file remains readable after picker closes");
        else {
            const auto deadline=std::chrono::steady_clock::now()+10s;
            while(!empty(staging)&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(20ms);
            Check(empty(staging),"unaccepted archive delivery leaves no staging tree after asynchronous cleanup");
        }
        Check(!before.empty()&&read(archive)==before,"original ZIP remains byte-for-byte unchanged (same SHA256)");
    }
    return failures.load()?1:0;
}
// Measure two genuine edit HWNDs. Separate cases distinguish blank row
// suppression from accidentally removing a meaningful group or caption.
int PlaceholderRows(PulsePickerUiV2Api& api, PulsePickerUiV2Session session, const std::wstring& folder) {
    int reference = -1;
    for (int variant = 0; variant < 5; ++variant) {
        std::vector<PulsePickerUiV2Control> controls;
        const std::wstring context = L"Import options", group = variant == 3 ? L"Sequence options" : L"",
            text = variant == 4 ? L"PNG sequence" : (variant == 2 ? L" " : L"");
        const auto add = [&](uint32_t kind, DWORD id, const std::wstring& label, bool child) {
            PulsePickerUiV2Control c{}; c.size = sizeof(c); c.kind = kind; c.id = id; c.state = 3;
            c.has_parent = child; c.parent_id = child ? 20 : 0; c.label = Text(label);
            controls.push_back(c);
        };
        const std::wstring empty;
        add(PULSE_PICKER_UI_V2_CONTROL_TEXT, 1, context, false);
        add(PULSE_PICKER_UI_V2_CONTROL_EDIT, 2, empty, false);
        if (variant) {
            add(PULSE_PICKER_UI_V2_CONTROL_GROUP, 20, group, false);
            add(PULSE_PICKER_UI_V2_CONTROL_TEXT, 21, text, true);
        }
        add(PULSE_PICKER_UI_V2_CONTROL_EDIT, 3, empty, variant != 0);
        PulsePickerUiV2Config config{}; config.size = sizeof(config); config.version = 2;
        config.initial_folder = Text(folder); config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
        config.controls = controls.data(); config.control_count = static_cast<uint32_t>(controls.size());
        Run(api, session, config, [&](HWND window, HWND, State&) {
            RECT first{}, second{};
            HWND a = GetDlgItem(window, 2001), b = GetDlgItem(window, 2000 + static_cast<int>(controls.size()) - 1);
            if (!a || !b || !IsWindowVisible(a) || !IsWindowVisible(b) ||
                !GetWindowRect(a, &first) || !GetWindowRect(b, &second)) return false;
            const int distance = second.top - first.top;
            if (variant == 0) reference = distance;
            std::printf("PLACEHOLDER variant=%d gap_px=%d base_px=%d\n", variant, distance, reference);
            Check(reference > 0 && (variant >= 3 ? distance > reference : distance == reference),
                "only a meaningful group title or static label reserves a row between editable fields");
            return true;
        }, "both actual input controls remain visible on the same picker", {}, UINT32_MAX,
        [&](const PulsePickerUiV2Event& event) {
            if (variant != 2) return;
            const HWND top = GetDlgItem(event.window, 2001), bottom = GetDlgItem(event.window, 2004);
            const auto gap = [&] {
                // Provider callbacks restore the caller's DPI context. Measure
                // in the same window context as the post-READY driver.
                const auto previous = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(event.window));
                if (!previous) return -1L;
                RECT a{}, b{};
                const auto distance = IsWindowVisible(top) && IsWindowVisible(bottom) &&
                    GetWindowRect(top, &a) && GetWindowRect(bottom, &b) ? b.top - a.top : -1L;
                SetThreadDpiAwarenessContext(previous);
                return distance;
            };
            const auto before = gap();
            auto changed = controls[3];
            const std::wstring meaningful = L"PNG sequence";
            changed.label = Text(meaningful);
            const HRESULT updated = api.UpdateControl(session, event.show_id, &changed);
            const auto during = gap();
            const HRESULT restored = api.UpdateControl(session, event.show_id, &controls[3]);
            const auto after = gap();
            std::printf("LIVE_LABEL before=%ld during=%ld after=%ld update=%08lX restore=%08lX\n",
                before, during, after, static_cast<unsigned long>(updated), static_cast<unsigned long>(restored));
            Check(updated == S_OK && restored == S_OK && reference > 0 && before == reference &&
                during > before && after == before, "same-window relabel expands then releases exactly one caption row");
        });
    }
    return failures.load() ? 1 : 0;
}
#include "picker_any_format_ui_cases.inc"
#include "picker_tag_group_ui_cases.inc"
#include "picker_tree_ui_cases.inc"
}
int wmain(int argc,wchar_t** argv) {
    if(argc<3){std::puts("Usage: picker_jump_ui_test ABSOLUTE_DLL ABSOLUTE_OUTPUT_DIR [--interactive] [--case=options|multi|create|save|save-followup|jump|complex|context|archive]");return 2;}
    bool interactive=false;std::wstring selected_case;
    for(int i=3;i<argc;++i){const std::wstring_view arg=argv[i];if(arg==L"--interactive")interactive=true;else if(arg.starts_with(L"--case="))selected_case=arg.substr(7);else return 2;}
    if(!selected_case.empty() && selected_case!=L"options" && selected_case!=L"multi" && selected_case!=L"create" && selected_case!=L"save" && selected_case!=L"jump" && selected_case!=L"complex" && selected_case!=L"context" && selected_case!=L"extension" && selected_case!=L"hostmenu" && selected_case!=L"placeholder-rows" && selected_case!=L"middle" && selected_case!=L"formats" && selected_case!=L"incoming" && selected_case!=L"save-draft" && selected_case!=L"tray" && selected_case!=L"preview" && selected_case!=L"new-files" && selected_case!=L"display-settings" && selected_case!=L"selection-footer" && selected_case!=L"save-feedback" && selected_case!=L"save-list" && selected_case!=L"dimensions" && selected_case!=L"format-notes" && selected_case!=L"tray-consume" && selected_case!=L"save-advisory" && selected_case!=L"archive" && selected_case!=L"save-followup" && selected_case!=L"any-format" && selected_case!=L"tag-group" && selected_case!=L"save-tray" && selected_case!=L"tree")return 2;
    const auto run_case=[&](const wchar_t* name){return selected_case.empty() || selected_case==name;};
    if (selected_case.empty() || selected_case == L"tray") {
        const auto helper = std::filesystem::absolute(argv[1]).parent_path() / L"Pulse.Preview.exe";
        if (!std::filesystem::is_regular_file(helper)) {
            std::fwprintf(stderr,L"MISSING_RUNTIME_DEPENDENCY=%ls\n",helper.c_str());
            return 2;
        }
    }

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
    if (selected_case == L"save-followup") {
        PulsePickerUiV2Initialize init{sizeof(init), 2}; PulsePickerUiV2Session session = 0;
        if (api.Initialize(&init, &session) != S_OK) return 2;
        SaveFollowupCases(api, session, root, output);
        Check(api.Shutdown(session) == S_OK, "release actual save follow-up picker session");
        CoUninitialize(); std::printf("FAILURES=%u\n", failures.load()); return failures.load() ? 1 : 0;
    }
    if (selected_case == L"any-format" || selected_case == L"tag-group" || selected_case == L"tree") {
        PulsePickerUiV2Initialize init{sizeof(init), 2}; PulsePickerUiV2Session session = 0;
        if (api.Initialize(&init, &session) != S_OK) return 2;
        if (selected_case == L"tree") TreeCases(api, session, root, output);
        else if (selected_case == L"any-format") AnyFormatCases(api, session, root, output);
        else TagGroupCases(api, session, root, output);
        Check(api.Shutdown(session) == S_OK, "release isolated picker integration session");
        CoUninitialize(); std::printf("FAILURES=%u\n", failures.load()); return failures.load() ? 1 : 0;
    }
    if (selected_case == L"archive") {
        PulsePickerUiV2Initialize init{sizeof(init),2};PulsePickerUiV2Session session=0;
        if(api.Initialize(&init,&session)!=S_OK)return 2;
        ArchiveCases(api,session,root,output);
        Check(api.Shutdown(session)==S_OK,"release actual archive picker session");
        CoUninitialize();std::printf("FAILURES=%u\n",failures.load());return failures.load()?1:0;
    }
    if (selected_case == L"placeholder-rows") {
        PulsePickerUiV2Initialize init{sizeof(init), 2}; PulsePickerUiV2Session session = 0;
        if (api.Initialize(&init, &session) != S_OK) return 2;
        PlaceholderRows(api, session, root.wstring());
        Check(api.Shutdown(session) == S_OK, "release actual picker session");
        CoUninitialize(); std::printf("FAILURES=%u\n", failures.load()); return failures.load() ? 1 : 0;
    }
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
        pulse::ui::PickerJumpUiTest q;q.set_pulse_fixture=true;q.pulse_count=0;
        if(!Send(window,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q)))return false;
        const bool ready=Wait([&]{return Geometry(window,sender,q)&&q.row_check_count>=2;},state.done);
        std::printf("MULTI ready=%u checks=%u p0=%ld,%ld p1=%ld,%ld\n",q.ready,q.row_check_count,q.row_checks[0].x,q.row_checks[0].y,q.row_checks[1].x,q.row_checks[1].y);
        Check(Capture(window,output/L"multiselect-before.png"),"capture multiselect initial window");
        if(!ready)return false;
        if(!Wait([&]{return Geometry(window,sender,q)&&q.row_check_count>=2;},state.done))return false;
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
            Check(!q.save_format_notice[0],"caller filename replacement clears obsolete inference explanation");
            Check(true,"extension type change preserves callback-owned filename without recursive type change");
            SendOnly(GetDlgItem(window,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"user.txt"));
            if(!Wait([&]{return state.filter_index==0 && state.filter_events==initial_type_events+2 && Geometry(window,sender,q);},state.done))return false;
            Check(Filename(window)==L"user.txt","later user edit resumes unique extension synchronization");
            Check(std::wstring(q.save_format_notice).find(L".txt")!=std::wstring::npos,"auto-switch explains typed suffix");
            SendOnly(GetDlgItem(window,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"quick.JPG"));
            SendOnly(GetDlgItem(window,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"quick.txt"));
            SendOnly(GetDlgItem(window,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"child\\final.PNG"));
            if(!Wait([&]{return state.filter_index==1 && state.filter_events==initial_type_events+5 && Filename(window)==L"child\\final.PNG" && Geometry(window,sender,q);},state.done))return false;
            Check(std::wstring(q.save_format_notice).find(L".PNG")!=std::wstring::npos,"rapid edits retain only latest inference explanation");
            Check(true,"rapid filename edits finish with latest relative leaf type and unchanged input");
            Check(Capture(window,output/L"extension-sync.png"),"capture synchronized save type and unchanged explicit extension");
            SendOnly(GetDlgItem(window,102),WM_KEYDOWN,VK_RETURN);return Wait([&]{return !IsWindow(window);},state.done);
        },"typed extension synchronization reaches original FileOk save path and exact type index",(root/L"child"/L"final.PNG").wstring(),1);
        // Equal-specificity alternatives are ambiguous; a dedicated PNG type beats Images by contract.
        const std::wstring duplicate_label=L"Other PNG",duplicate_pattern=L"*.PNG;*.JPG";
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
    if(run_case(L"save-feedback")) {
        for(const auto* name:{L"banner-v1.txt",L"banner_v02.txt",L"banner.txt",L"banner2.txt",L"banner-v1.png"}) std::ofstream(root/name)<<"sample";
        std::filesystem::create_directory(root/L"banner-v3.txt");
        std::filesystem::create_directory(root/L"elsewhere");
        const std::wstring tl=L"Text",il=L"PNG",tp=L"*.txt",ip=L"*.png",name=L"banner-v4.txt";
        PulsePickerUiV2Filter filters[2]={{sizeof(PulsePickerUiV2Filter),0,Text(tl),Text(tp)},{sizeof(PulsePickerUiV2Filter),0,Text(il),Text(ip)}};
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;config.save_options=PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;config.native_options=0;
        config.filename=Text(name);config.controls=nullptr;config.control_count=0;config.properties=nullptr;config.property_count=0;
        config.filters=filters;config.filter_count=2;config.filter_index=0;
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;
            const auto badges=[&](unsigned count){return Wait([&]{SendOnly(w,WM_PAINT);return Geometry(w,sender,q)&&q.series_badge_count==count;},state.done);};
            const auto type=[&](const wchar_t* value){SendOnly(GetDlgItem(w,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(value));};
            Check(badges(4),"actual list marks four cross-format version-family files, excludes directory and plain-number neighbor");
            Check(Capture(w,output/L"save-series.png"),"capture shared row badges in actual save dialog");
            for(const auto* value:{L"other.txt",L"",L"bad?.txt",L"elsewhere\banner-v4.txt"}) {
                type(value);Check(badges(0),"unrelated, empty, invalid and other-folder drafts clear series badges");
            }
            type(L"banner-v4.png");
            Check(badges(4)&&state.filter_index==1&&std::wstring(q.save_format_notice).find(L".png")!=std::wstring::npos,"format inference refreshes visible family and explains suffix");
            Check(Capture(w,output/L"save-format-notice.png"),"capture neutral format explanation with series badge");
            type(L"banner-v1.txt");
            Check(Wait([&]{return Geometry(w,sender,q)&&q.live_conflict&&q.save_draft_checked;},state.done),"existing filename retains live overwrite conflict above explanation");
            Check(Capture(w,output/L"save-conflict-priority.png"),"capture overwrite conflict priority");
            type(L"banner-v5.txt");Check(badges(4)&&!q.save_format_notice[0],"same-format edit clears stale automatic-switch message");
            type(L"banner-v5.png");if(!badges(4))return false;
            int text=-1;for(unsigned i=0;i<q.format_count;++i)if(q.format_indices[i]==0)text=static_cast<int>(i);
            if(text<0)return false;Click(w,Center(q.formats[text]));
            Check(Wait([&]{return Geometry(w,sender,q)&&state.filter_index==0&&!q.save_format_notice[0];},state.done),"manual format choice clears auto-switch explanation");
            return true;
        },"save feedback remains informational and cancels without saving");
    }
#ifdef PULSE_PICKER_TRAY_CONSUME_TEST
    if(run_case(L"tray-consume")) {
        const auto shelf=root/L"tray-backing";std::filesystem::create_directory(shelf);
        const auto ordinary=(shelf/L"ordinary.txt").wstring(),content=(shelf/L"snippet.txt").wstring();
        std::ofstream(ordinary)<<"ordinary backing";std::ofstream(content)<<"owned text backing";
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;config.save_options=0;config.native_options=0;config.filename={};
        config.filters=nullptr;config.filter_count=0;config.controls=nullptr;config.control_count=0;config.properties=nullptr;config.property_count=0;
        const auto click_path=[](HWND w,const std::wstring& path){pulse::ui::PickerPlacesTestPoint p;p.kind=pulse::ui::PickerPlacesTestPoint::Kind::Sidebar;p.path=path.c_str();if(!Send(w,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&p)))return false;Click(w,p.point);return true;};
        for(unsigned variant=0;variant<6;++variant){
            PickerTrayUiFixture fixture({ordinary,content},1);
            auto tray=fixture.Snapshot();Check(tray.tray_items.size()==2,"real StagingTray fixture supplies reference and owned materialization");
            config.open_options=variant==1?PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT:0;
            config.appearance=variant==2?PULSE_PICKER_UI_V2_DARK:0;
            const bool accepts=variant<=2;
            Run(api,session,config,[&](HWND w,HWND sender,State& state){
                pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done))return false;
                q={};q.tray_snapshot=&tray;if(!Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q)))return false;
                if(!Wait([&]{return Geometry(w,sender,q)&&q.tray_count==2;},state.done))return false;
                Check(!q.tray_remove_after_open,"isolated test never imports installed per-host setting");
                if(variant==1){
                    Check(q.tray_remove_option.right<=q.tray_remove_option.left,"multiselect hides remove-after-open footer checkbox until tray item selected");
                    if(!click_path(w,q.tray_all_token))return false;
                    if(!Wait([&]{return Geometry(w,sender,q)&&q.basket_count==2&&q.tray_remove_option.right>q.tray_remove_option.left;},state.done))return false;
                    Check(fixture.requests==0&&fixture.Count()==2,"selecting tray files does not mutate model before confirmation");
                    Click(w,Center(q.tray_remove_option));
                    if(!Wait([&]{return Geometry(w,sender,q)&&q.tray_remove_after_open;},state.done))return false;
                    Check(Capture(w,output/L"tray-remove-option.png"),"capture footer remove-after-open checkbox in light multiselect layout");
                    pulse::ui::PickerJumpUiTest hover;hover.inspect_hover=true;hover.inspect_hover_point=Center(q.tray_remove_option);
                    Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&hover));
                    Check(std::wstring(hover.format_tooltip).find(L"临时内容保留")!=std::wstring::npos,"option tooltip explains retained temporary content");
                    pulse::ui::PickerInteractionTest interaction;Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&interaction));Click(w,Center(interaction.primary));
                } else {
                    Check(q.tray_remove_option.right>q.tray_remove_option.left,"single-select exposes footer remove-after-open checkbox when compatible tray items exist");
                    if(variant!=0){
                        Click(w,Center(q.tray_remove_option));
                        if(!Wait([&]{return Geometry(w,sender,q)&&q.tray_remove_after_open;},state.done))return false;
                    }
                    if(variant==2) {
                        q={};q.client_width_dip=720;Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q));Geometry(w,sender,q);
                        Check(Capture(w,output/L"tray-remove-dark-narrow.png"),"capture footer remove-after-open checkbox in narrow dark layout");
                        if(q.tray_remove_option.right>q.tray_remove_option.left){
                            pulse::ui::PickerJumpUiTest hover;hover.inspect_hover=true;hover.inspect_hover_point=Center(q.tray_remove_option);
                            Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&hover));
                            Check(std::wstring(hover.format_tooltip).find(L"临时内容保留")!=std::wstring::npos,"option tooltip explains retained temporary content");
                        }
                    }
                    if(variant==3) {return fixture.requests==0;} // Cancel with option on.
                    if(variant==4)state.veto_file_ok=true;
                    const unsigned index=variant==2?1:0;
                    if(!click_path(w,q.tray_tokens[index]))return false;
                    if(variant==4){
                        Check(Wait([&]{return state.file_ok==1;},state.done)&&IsWindow(w)&&fixture.requests==0&&fixture.Count()==2,"FileOk veto keeps tray records and dialog alive");
                        return true;
                    }
                }
                return Wait([&]{return !IsWindow(w);},state.done);
            },"owned picker delivers or cancels using real tray identities",accepts?(variant==2?content:ordinary):std::wstring(),UINT32_MAX,{},variant==1?2:0,
                variant==5?1:0,variant==5?HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER):S_OK);
            if(variant==1||variant==2){std::atomic<bool> waiting=false;Check(Wait([&]{return fixture.requests==1;},waiting),"successful V2 result schedules exactly one real WM_COPYDATA consume");}
            else std::this_thread::sleep_for(200ms);
            Check(fixture.requests==(variant==1||variant==2?1u:0u),"default-off cancellation and veto issue no consume request");
            Check(fixture.Count()==(variant==1?1u:2u)&&fixture.removed==(variant==1?1u:0u),"only confirmed ordinary reference leaves real model");
            if(variant==1||variant==2)Check(fixture.retained==1&&fixture.rejected==0,"owned snippet explicitly retained and reported by real model");
            Check(std::filesystem::file_size(ordinary)==16&&std::filesystem::file_size(content)==18,"both original and owned backing paths remain readable after delivery");
        }
    }
#endif
    if(run_case(L"dimensions")) {
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;config.save_options=0;config.native_options=0;
        std::filesystem::copy_file(L"assets/pulse-logo.png", root/L"pixels.png");
        std::ofstream(root/L"broken.png") << "not an image";
        const auto empty=root/L"no-images";
        std::filesystem::create_directory(empty);
        std::ofstream(empty/L"plain.txt") << "plain";
        config.controls=nullptr;config.control_count=0;config.properties=nullptr;config.property_count=0;
        config.filters=nullptr;config.filter_count=0;config.open_options=0;config.filename={};
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;
            const auto dimensions_ready=[&]{
                if(!Geometry(w,sender,q)||!q.dimensions_column)return false;
                for(unsigned i=0;i<q.display_count;++i)if(std::wstring_view(q.raw_names[i])==L"pixels.png")return std::wstring_view(q.dimensions[i])==L"1024 × 1024";
                return false;
            };
            if(!Wait(dimensions_ready,state.done)){Check(false,"source PNG dimensions published from real metadata worker");return false;}
            Check(true,"source PNG dimensions are 1024 x 1024, not thumbnail bitmap size");
            bool blanks=true;for(unsigned i=0;i<q.display_count;++i)if(std::wstring_view(q.raw_names[i])!=L"pixels.png")blanks&=q.dimensions[i][0]==0;
            Check(blanks,"nonimages, folders and invalid image metadata remain blank");
            SetWindowPos(w,nullptr,0,0,1280,720,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            Check(HasRow(w,(root/L"pixels.png").wstring()),"bring real image row into viewport before visual capture");
            Check(Capture(w,output/L"dimensions-wide.png"),"capture actual shared details column with real dimensions");
            const auto navigate=[&](const std::wstring& path){
                pulse::ui::PickerInteractionTest key;key.key='L';key.ctrl=true;
                Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&key));
                SendOnly(GetDlgItem(w,101),WM_SETTEXT,0,reinterpret_cast<LPARAM>(path.c_str()));
                SendOnly(GetDlgItem(w,101),WM_KEYDOWN,VK_RETURN);
            };
            navigate(empty.wstring());
            Check(Wait([&]{return Geometry(w,sender,q)&&q.display_count==1&&std::wstring_view(q.raw_names[0])==L"plain.txt"&&!q.dimensions_column&&!q.dimensions[0][0];},state.done),"navigation clears previous image rows and dimensions column");
            for(int i=0;i<3;++i){navigate(root.wstring());navigate(empty.wstring());}
            Check(Wait([&]{return Geometry(w,sender,q)&&q.display_count==1&&!q.dimensions_column;},state.done),"rapid navigation never publishes previous-folder dimensions into text rows");
            navigate(root.wstring());if(!Wait(dimensions_ready,state.done))return false;
            SetWindowPos(w,nullptr,0,0,640,600,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            Check(Capture(w,output/L"dimensions-narrow.png"),"capture narrow columns without squeezing filename");
            return state.file_ok==0;
        },"real image metadata remains correct across resize and navigation");
    }
    if(run_case(L"format-notes")) {
        const std::wstring name=L"draft.png";
        const std::wstring note_labels[]={L"PNG",L"WebP",L"Vendor project",L"All"};
        const std::wstring patterns[]={L"*.png",L"*.webp",L"*.vendor",L"*.*"};
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;config.open_options=0;config.save_options=PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
        config.controls=nullptr;config.control_count=0;config.properties=nullptr;config.property_count=0;config.filename=Text(name);
        for(unsigned variant=0;variant<4;++variant){
            PulsePickerUiV2Filter f{sizeof(f),0,Text(note_labels[variant]),Text(patterns[variant])};
            config.filters=&f;config.filter_count=1;config.filter_index=0;config.appearance=variant%2?PULSE_PICKER_UI_V2_DARK:0;
            Run(api,session,config,[&](HWND w,HWND sender,State& state){
                pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q)&&q.format_static_text[0]&&(variant==3||q.format_note[0]);},state.done))return false;
                const auto initial_events=state.filter_events.load();
                const auto static_rect=q.formats[0].right>q.formats[0].left?q.formats[0]:q.format_more;
                Click(w,Center(static_rect));std::this_thread::sleep_for(100ms);
                Check(state.filter_events==initial_events&&state.filter_index==0&&!Menu(),"clicking single-format static text emits no extra TypeChanged and opens no menu");
                Geometry(w,sender,q);
                const auto full_text=std::wstring(q.format_static_text);
                pulse::ui::PickerJumpUiTest hover;hover.inspect_hover=true;hover.inspect_hover_point=Center(static_rect);
                Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&hover));
                Check(std::wstring(hover.format_tooltip).find(full_text)!=std::wstring::npos,"static format hover retains complete text including extensions");
                const std::wstring text=q.format_static_text,note=q.format_note;
                Check(variant==3?text.find(L"·")!=text.npos:text.find(patterns[variant].substr(1))!=text.npos,"single format includes literal extensions or any-format explanation");
                if(variant==2)Check(note==L"Vendor project · .vendor","unknown caller format gets no invented capability claim");
                SetWindowPos(w,nullptr,0,0,1120,640,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                Check(Capture(w,output/(L"format-note-"+std::to_wstring(variant)+L".png")),"capture real static format and explanation");
                return state.file_ok==0;
            },"single-format actual Save dialog has static explanatory text");
        }
    }
    if(run_case(L"save-list")) {
        Check(std::filesystem::copy_file(L"assets/pulse-logo.png",root/L"a.png"),"copy real PNG asset into isolated thumbnail fixture");
        std::ofstream(root/L"other.bin")<<"sample";
        std::filesystem::create_directory(root/L"folder.png");
        const std::wstring tl=L"Text",il=L"PNG",al=L"All",tp=L"*.txt",ip=L"*.png",ap=L"*.*",name=L"new.txt";
        PulsePickerUiV2Filter filters[3]={{sizeof(PulsePickerUiV2Filter),0,Text(tl),Text(tp)},{sizeof(PulsePickerUiV2Filter),0,Text(il),Text(ip)},{sizeof(PulsePickerUiV2Filter),0,Text(al),Text(ap)}};
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;config.save_options=PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;config.native_options=0;
        config.filename=Text(name);config.controls=nullptr;config.control_count=0;config.properties=nullptr;config.property_count=0;
        config.filters=filters;config.filter_count=3;config.filter_index=0;
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;
            const auto ready=[&](unsigned muted){return Wait([&]{SendOnly(w,WM_PAINT);return Geometry(w,sender,q)&&q.display_count==5&&q.muted_count==muted;},state.done);};
            if(!ready(2))return false;
            std::vector<std::wstring> original_order;for(unsigned i=0;i<q.display_count;++i)original_order.push_back(q.raw_names[i]);
            bool exact=true;for(unsigned i=0;i<q.display_count;++i){const std::wstring row=q.raw_names[i];exact&=q.muted_rows[i]==(row==L"a.png"||row==L"other.bin");}
            Check(exact,"Save mutes only nonmatching files, not folders or matching rows");
            for(unsigned i=0;i<q.format_count;++i)if(q.format_indices[i]==1){
                pulse::ui::PickerJumpUiTest hover;hover.inspect_hover=true;hover.inspect_hover_point=Center(q.formats[i]);
                Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&hover));
                Check(std::wstring_view(hover.format_tooltip).starts_with(L"PNG\n"),"hover describes the unselected PNG segment, not current TXT");
            }
            Check(Capture(w,output/L"save-list-details.png"),"capture real shared details renderer with mismatch emphasis");
            const auto choose=[&](int index,unsigned muted){
                if(!Geometry(w,sender,q))return false;
                for(unsigned i=0;i<q.format_count;++i)if(q.format_indices[i]==index){Click(w,Center(q.formats[i]));return ready(muted);}
                return false;
            };
            Check(choose(1,3),"PNG selection changes emphasis without removing files");
            Check(choose(2,0),"all-files choice clears every mismatch marker");
            Check(choose(0,2),"switching back restores exact Text mismatch markers");
            std::vector<std::wstring> switched_order;for(unsigned i=0;i<q.display_count;++i)switched_order.push_back(q.raw_names[i]);
            Check(original_order==switched_order,"format toggles preserve folder and file ordering");
            pulse::ui::PickerViewSwitchTest view;view.begin=true;view.mode=2;
            Check(Send(w,pulse::ui::kPickerViewSwitchTestMessage,0,reinterpret_cast<LPARAM>(&view)),"switch actual picker to shared icon view");
            // Icon cache requests are asynchronous; wait for additional real paints before capture.
            for(int frame=0;frame<8;++frame){SendOnly(w,WM_PAINT);std::this_thread::sleep_for(200ms);}
            Check(Capture(w,output/L"save-list-icons.png"),"capture mismatch emphasis in shared icon renderer");
            Check(choose(2,0),"all-files also clears icon and thumbnail emphasis");
            Check(Capture(w,output/L"save-list-icons-all.png"),"capture same real PNG and icons without mismatch dimming");
            if(!choose(0,2))return false;
            view={};view.begin=true;view.mode=5;Send(w,pulse::ui::kPickerViewSwitchTestMessage,0,reinterpret_cast<LPARAM>(&view));
            if(!ready(2))return false;
            const auto selected=(root/L"other.bin").wstring();pulse::ui::PickerPlacesTestPoint point;point.path=selected.c_str();
            if(!Send(w,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&point)))return false;Click(w,point.point);
            Check(Wait([&]{return Filename(w)==L"other.bin"&&Geometry(w,sender,q);},state.done)&&state.file_ok==0&&state.filter_index==0,"muted file remains selectable and fills filename without submitting or changing caller type");
            SendOnly(GetDlgItem(w,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"fresh.bin"));
            SendOnly(GetDlgItem(w,102),WM_KEYDOWN,VK_RETURN);return Wait([&]{return !IsWindow(w);},state.done);
        },"non-strict Save retains explicit filename and exact caller type",(root/L"fresh.bin").wstring(),0);
        config.native_options=FOS_STRICTFILETYPES;config.appearance=PULSE_PICKER_UI_V2_DARK;
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q)&&q.muted_count==2;},state.done))return false;
            Check(Capture(w,output/L"save-list-dark.png"),"capture real dark-theme mismatch emphasis");
            SendOnly(GetDlgItem(w,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"strict.bin"));
            SendOnly(GetDlgItem(w,102),WM_KEYDOWN,VK_RETURN);
            const bool rejected=Wait([&]{return Geometry(w,sender,q)&&q.strict_type_rejected;},state.done);
            Check(rejected&&IsWindow(w)&&state.file_ok==0,"FOS_STRICTFILETYPES rejects mismatched final Save before FileOk");
            Check(Capture(w,output/L"save-list-strict.png"),"capture caller strict-type rejection with contextual files still visible");
            if(!rejected)return false;
            SendOnly(GetDlgItem(w,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"strict.txt"));
            SendOnly(GetDlgItem(w,102),WM_KEYDOWN,VK_RETURN);return Wait([&]{return !IsWindow(w);},state.done);
        },"strict Save recovers with matching filename and exact FileOk type",(root/L"strict.txt").wstring(),0);
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;config.save_options=0;config.native_options=0;config.appearance=0;config.filename={};
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q)&&q.display_count==3;},state.done))return false;
            Check(q.muted_count==0&&!HasRow(w,(root/L"a.png").wstring())&&!HasRow(w,(root/L"other.bin").wstring()),"Open keeps filtering mismatched files and carries no Save emphasis");return true;
        },"Open dialog behavior unchanged after Save sessions");
    }
    if(run_case(L"middle")) {
        const auto subfolder = root / L"middle folder";
        std::filesystem::create_directory(subfolder);
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;config.open_options=0;config.save_options=0;config.native_options=0;
        config.filename={};config.controls=nullptr;config.control_count=0;config.properties=nullptr;config.property_count=0;
        config.filters=nullptr;config.filter_count=0;config.filter_index=0;
        Run(api,session,config,[&](HWND window,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;
            const auto folder_path = subfolder.wstring();
            const auto file_path = (root/L"a.txt").wstring();
            if(!Wait([&]{return Geometry(window,sender,q)&&HasRow(window,folder_path)&&HasRow(window,file_path);},state.done))return false;
            q.middle_fixture_folder=folder_path.c_str();
            if(!Send(window,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q)))return false;
            pulse::ui::PickerPlacesTestPoint point;point.path=file_path.c_str();
            if(!Send(window,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&point)))return false;
            Click(window,point.point);
            if(!Wait([&]{return Selection(window,1);},state.done))return false;
            pulse::ui::PickerInteractionTest before;
            if(!Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&before)))return false;
            unsigned launches=0;
            const auto middle=[&](POINT target,const std::wstring& expected,bool file,const char* label) {
                if(!SendOnly(window,WM_MBUTTONDOWN,MK_MBUTTON,MAKELPARAM(target.x,target.y)) ||
                   !SendOnly(window,WM_MBUTTONUP,0,MAKELPARAM(target.x,target.y)))return false;
                pulse::ui::PickerInteractionTest after;
                if(!Geometry(window,sender,q)||!Send(window,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&after)))return false;
                const bool matched=q.middle_launch_count==++launches && q.middle_file==file && q.middle_path==expected;
                Check(matched,label);
                const bool preserved=IsWindow(window)&&!state.done&&state.file_ok==0&&before.current==after.current&&before.selected_paths==after.selected_paths;
                Check(preserved,"middle click preserves dialog, folder and selection without FileOk");
                return matched&&preserved;
            };
            if(!middle(point.point,file_path,true,"list file middle-click emits reveal intent"))return false;
            point.path=folder_path.c_str();
            if(!Send(window,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&point)) ||
               !middle(point.point,folder_path,false,"list folder middle-click emits new-tab intent"))return false;
            point.kind=pulse::ui::PickerPlacesTestPoint::Kind::Sidebar;
            if(!Send(window,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&point)) ||
               !middle(point.point,folder_path,false,"sidebar folder middle-click emits new-tab intent"))return false;
            if(!Geometry(window,sender,q)||q.middle_open_folder.right<=q.middle_open_folder.left ||
               !middle(Center(q.middle_open_folder),folder_path,false,"open-folder strip middle-click emits new-tab intent"))return false;
            SendOnly(window,WM_MBUTTONUP,0,MAKELPARAM(1,1));
            Check(Geometry(window,sender,q)&&q.middle_launch_count==launches,"non-item middle click emits no intent");
            return true;
        },"middle-click production message path keeps picker open until explicit test cancellation");
    }
    if(run_case(L"formats")) {
        std::vector<std::wstring> names, patterns;
        for (int i=0;i<10;++i) { names.push_back(L"Type"+std::to_wstring(i)); patterns.push_back(L"*.f"+std::to_wstring(i)); }
        std::vector<PulsePickerUiV2Filter> formats;
        for(size_t i=0;i<names.size();++i) formats.push_back({sizeof(PulsePickerUiV2Filter),0,Text(names[i]),Text(patterns[i])});
        const std::wstring name=L"format-result.f0";
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;config.open_options=0;config.save_options=PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;config.native_options=0;
        config.filename=Text(name);config.controls=nullptr;config.control_count=0;config.properties=nullptr;config.property_count=0;
        config.filters=formats.data();config.filter_count=static_cast<uint32_t>(formats.size());config.filter_index=0;
        Run(api,session,config,[&](HWND window,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;
            if(!Wait([&]{return Geometry(window,sender,q)&&q.format_count>1&&q.format_more.right>q.format_more.left;},state.done))return false;
            const auto check_row=[&] {
                const auto aligned=[&](RECT r){return std::abs((r.top+r.bottom)-(q.filename.top+q.filename.bottom))<=2 && r.left>q.filename.right;};
                bool inline_row=aligned(q.format_more);
                for(unsigned i=0;i<q.format_count;++i) if(q.formats[i].right>q.formats[i].left)
                    inline_row=inline_row&&aligned(q.formats[i]);
                Check(inline_row,"real filename edit and format controls share one non-overlapping row");
                return inline_row;
            };
            if(!check_row())return false;
            Check(Capture(window,output/L"formats-wide.png"),"capture real save dialog with inline filename and format controls");
            int chosen=-1;
            for(unsigned i=0;i<q.format_count;++i)
                if(q.format_indices[i]==1&&q.formats[i].right>q.formats[i].left)chosen=static_cast<int>(i);
            if(chosen<0)return false;
            const auto before=state.filter_events.load();
            Click(window,Center(q.formats[chosen]));
            if(!Wait([&]{return state.filter_index==1&&state.filter_events==before+1&&Geometry(window,sender,q);},state.done))return false;
            Check(true,"inline segment preserves caller TypeChanged index and one callback");
            SetWindowPos(window,nullptr,0,0,640,600,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            if(!Wait([&]{return Geometry(window,sender,q)&&q.format_more.right>q.format_more.left;},state.done)||!check_row())return false;
            Check(Capture(window,output/L"formats-narrow.png"),"capture real narrow save dialog");
            Click(window,Center(q.format_more));
            if(!Wait([&]{return Menu()!=nullptr&&GetWindow(Menu(),GW_CHILD)!=nullptr;},state.done))return false;
            const auto menu=Menu(); const auto edit=GetWindow(menu,GW_CHILD);
            SendOnly(edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L".f9"));
            PostMessageW(menu,WM_KEYDOWN,VK_DOWN,0);PostMessageW(menu,WM_KEYDOWN,VK_RETURN,0);
            if(!Wait([&]{return state.filter_index==9&&state.filter_events==before+2&&Geometry(window,sender,q);},state.done))return false;
            bool visible=false;
            for(unsigned i=0;i<q.format_count;++i)if(q.format_indices[i]==9&&q.formats[i].right>q.formats[i].left)visible=true;
            Check(visible,"searchable More returns exact caller index and retains selected format inline");
            SendOnly(GetDlgItem(window,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"format-result.f9"));
            SendOnly(GetDlgItem(window,102),WM_KEYDOWN,VK_RETURN);
            return visible&&Wait([&]{return !IsWindow(window);},state.done);
        },"format row completes original FileOk save with exact extension and filter",(root/L"format-result.f9").wstring(),9);
    }

    if(run_case(L"incoming")) {
        config={};config.size=sizeof(config);config.version=2;config.initial_folder=Text(folder);
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
        const auto a=(root/L"a.txt").wstring(), b=(root/L"b.txt").wstring();
        const auto wrong=(root/L"wrong.png").wstring(), missing=(root/L"missing.txt").wstring();
        std::ofstream(wrong)<<"not an image";
        const std::wstring text_label=L"Text",text_pattern=L"*.txt";
        PulsePickerUiV2Filter filter{sizeof(filter),0,Text(text_label),Text(text_pattern)};
        config.filters=&filter;config.filter_count=1;
        const auto ready=[&](HWND w,State& state,pulse::ui::PickerInteractionTest& q) {
            return Wait([&]{q={};return Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&q))&&q.ready&&q.blank_ready;},state.done);
        };
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            (void)sender;
            pulse::ui::PickerInteractionTest q;if(!ready(w,state,q))return false;
            pulse::ui::PickerOleTestDrop drop{a.c_str(),q.blank};drop.keys=0;drop.allowed=DROPEFFECT_COPY|DROPEFFECT_MOVE;
            const bool accepted=Send(w,pulse::ui::kPickerOleTestMessage,0,reinterpret_cast<LPARAM>(&drop));
            Check(accepted&&drop.effect==DROPEFFECT_COPY,"blank drop uses original file without reporting MOVE");
            return accepted&&Wait([&]{return state.file_ok==1;},state.done);
        },"blank drop reaches caller FileOk and returns original path",a);
        Check(std::filesystem::exists(a)&&std::filesystem::file_size(a)==1,"using dropped file preserves source");
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            (void)sender;
            pulse::ui::PickerInteractionTest q;if(!ready(w,state,q))return false;
            for(int variant=0;variant<4;++variant) {
                pulse::ui::PickerOleTestDrop drop{variant==0?wrong.c_str():a.c_str(),q.blank};
                drop.keys=0;drop.allowed=variant==1?DROPEFFECT_MOVE:DROPEFFECT_COPY;
                if(variant==2)drop.second_source=b.c_str();
                if(variant==3)drop.source=folder.c_str();
                const bool handled=Send(w,pulse::ui::kPickerOleTestMessage,0,reinterpret_cast<LPARAM>(&drop));
                if(variant<3)Check(!handled&&drop.effect==DROPEFFECT_NONE,"incompatible/move-only/multi-in-single drop rejected");
                if(!ready(w,state,q))return false;
            }
            pulse::ui::PickerOleTestDrop missing_drop{missing.c_str(),q.blank};
            SendOnly(w,pulse::ui::kPickerOleTestMessage,0,reinterpret_cast<LPARAM>(&missing_drop));
            if(!ready(w,state,q))return false;
            Check(state.file_ok==0&&q.current==folder,"directory and missing file never accept or navigate on use");
            return state.file_ok==0;
        },"invalid use remains in original dialog");
        const auto destination=root/L"destination";std::filesystem::create_directory(destination);
        for(int move=0;move<2;++move) {
            const auto source=root/(move?L"move.txt":L"copy.txt");std::ofstream(source)<<"transfer";
            const auto source_text=source.wstring(),dest_text=destination.wstring();
            Run(api,session,config,[&](HWND w,HWND sender,State& state){
            (void)sender;
                pulse::ui::PickerInteractionTest q;if(!ready(w,state,q))return false;
                pulse::ui::PickerPlacesTestPoint point;point.path=dest_text.c_str();
                if(!Send(w,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&point)))return false;
                pulse::ui::PickerOleTestDrop drop{source_text.c_str(),point.point};drop.keys=move?MK_SHIFT:0;drop.allowed=DROPEFFECT_COPY|DROPEFFECT_MOVE;
                const bool accepted=Send(w,pulse::ui::kPickerOleTestMessage,0,reinterpret_cast<LPARAM>(&drop));
                const bool complete=Wait([&]{return std::filesystem::exists(destination/source.filename())&&(move?!std::filesystem::exists(source):std::filesystem::exists(source));},state.done);
                Check(accepted&&complete&&drop.effect==static_cast<DWORD>(move?DROPEFFECT_NONE:DROPEFFECT_COPY)&&drop.logical_effect==static_cast<DWORD>(move?DROPEFFECT_MOVE:DROPEFFECT_COPY)&&drop.performed_effect==drop.effect&&state.file_ok==0,move?"Shift folder-row drop moves via operation queue":"folder-row drop defaults to copy via operation queue");
                pulse::ui::PickerJumpUiTest settled;
                const bool idle=Wait([&]{ settled={}; DWORD_PTR result=0; return SendMessageTimeoutW(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&settled),SMTO_BLOCK|SMTO_ABORTIFHUNG,3000,&result)&&result&&settled.ready; },state.done);
                return accepted&&complete&&idle;
            },"transfer keeps picker open instead of selecting transferred file");
        }
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            (void)sender;
            pulse::ui::PickerInteractionTest q;if(!ready(w,state,q))return false;
            pulse::ui::PickerOleTestDrop drop{a.c_str(),q.blank};drop.keys=MK_RBUTTON;drop.allowed=DROPEFFECT_COPY|DROPEFFECT_MOVE;
            PostMessageW(w,pulse::ui::kPickerOleTestMessage,0,reinterpret_cast<LPARAM>(&drop));
            HWND menu=nullptr;if(!Wait([&]{return (menu=Menu())!=nullptr;},state.done))return false;
            PostMessageW(menu,WM_KEYDOWN,VK_ESCAPE,0);
            if(!Wait([&]{return Menu()==nullptr;},state.done))return false;
            // Synchronize the posted Drop handler before stack data goes away.
            SendOnly(w,WM_NULL);
            Check(drop.effect==DROPEFFECT_NONE&&state.file_ok==0,"right-drag menu cancellation performs no transfer or use");
            return drop.effect==DROPEFFECT_NONE;
        },"right-drag cancellation leaves dialog usable");
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerInteractionTest q;if(!ready(w,state,q))return false;
            state.veto_file_ok=true;
            pulse::ui::PickerOleTestDrop drop{a.c_str(),q.blank};
            if(!Send(w,pulse::ui::kPickerOleTestMessage,0,reinterpret_cast<LPARAM>(&drop)))return false;
            pulse::ui::PickerJumpUiTest geometry;
            const bool retained=Wait([&]{return state.file_ok==1&&Geometry(w,sender,geometry);},state.done);
            Check(retained&&std::filesystem::exists(a),"caller FileOk veto keeps drop-use dialog and source intact");
            return retained;
        },"drop-use honors host veto");
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            (void)sender;
            pulse::ui::PickerInteractionTest q;if(!ready(w,state,q))return false;
            pulse::ui::PickerOleTestDrop drop{a.c_str(),q.blank};drop.keys=MK_RBUTTON;drop.allowed=DROPEFFECT_COPY|DROPEFFECT_MOVE;
            PostMessageW(w,pulse::ui::kPickerOleTestMessage,0,reinterpret_cast<LPARAM>(&drop));
            HWND menu=nullptr;if(!Wait([&]{return (menu=Menu())!=nullptr;},state.done))return false;
            PostMessageW(menu,WM_KEYDOWN,VK_RETURN,0);
            const bool accepted=Wait([&]{return state.file_ok==1;},state.done);
            SendOnly(w,WM_NULL);
            Check(accepted&&drop.logical_effect!=DROPEFFECT_MOVE,"right-drag Use returns original file instead of moving it");
            return accepted;
        },"right-drag Use reaches caller FileOk",a);
        const auto inject=[&](HWND w,HWND sender,const std::vector<std::wstring>& paths,bool directory=false) {
            pulse::ui::PickerJumpUiTest q;q.set_pulse_fixture=true;q.pulse_count=static_cast<unsigned>(paths.size());
            for(unsigned i=0;i<q.pulse_count;++i){q.pulse_paths[i]=paths[i].c_str();q.pulse_directories[i]=directory;}
            return Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q));
        };
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            (void)sender;
            pulse::ui::PickerInteractionTest q;if(!ready(w,state,q)||!inject(w,sender,{wrong,a}))return false;
            Check(Capture(w,output/L"pulse-selection-single.png"),"capture real selected-source strip");
            SendOnly(GetDlgItem(w,102),WM_SYSKEYDOWN,'0');
            return Wait([&]{return state.file_ok==1;},state.done);
        },"Alt+0 skips incompatible source and returns first compatible file",a);
        const auto external=root/L"source";std::filesystem::create_directory(external);
        const auto external_a=(external/L"external-a.txt").wstring(),external_b=(external/L"external-b.txt").wstring();
        std::ofstream(external_a)<<"a";std::ofstream(external_b)<<"b";
        config.open_options=PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT;
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            (void)sender;
            pulse::ui::PickerInteractionTest q;if(!ready(w,state,q)||!inject(w,sender,{external_a,external_b,wrong}))return false;
            SendOnly(w,WM_SYSKEYDOWN,'0');pulse::ui::PickerJumpUiTest geometry;
            if(!Wait([&]{return Geometry(w,sender,geometry)&&geometry.basket_count==2;},state.done))return false;
            Check(state.file_ok==0,"multi Alt+0 adds compatible files without premature FileOk");
            SendOnly(w,WM_SYSKEYDOWN,'0');if(!ready(w,state,q))return false;
            Geometry(w,sender,geometry);Check(geometry.basket_count==2,"repeat Add all deduplicates basket");
            Check(Capture(w,output/L"pulse-selection-multi.png"),"capture source strip with selected basket");
            inject(w,sender,{});Geometry(w,sender,geometry);
            Check(geometry.pulse_count==0&&geometry.basket_count==2,"cleared source hides strip without deleting user basket");
            SendOnly(w,WM_SYSKEYDOWN,'0');
            SendOnly(w,WM_SYSKEYDOWN,'D');
            const auto next=destination.wstring();SendOnly(GetDlgItem(w,101),WM_SETTEXT,0,reinterpret_cast<LPARAM>(next.c_str()));
            pulse::ui::PickerInteractionTest enter;enter.key=VK_RETURN;enter.edit_id=101;
            Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&enter));
            const bool navigated=Wait([&]{q={};return Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&q))&&q.current==next;},state.done);
            Check(navigated,"address navigation after Add all resets incoming-validation intent");
            if(!navigated||!Wait([&]{return Geometry(w,sender,geometry);},state.done))return false;
            q={};Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&q));
            Click(w,Center(q.primary));return Wait([&]{return state.file_ok==1;},state.done);
        },"multi source basket returns both files only after explicit submission",external_a,UINT32_MAX,{},2);
        config.open_options=0;
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerInteractionTest q;if(!ready(w,state,q)||!inject(w,sender,{a}))return false;
            pulse::ui::PickerJumpUiTest geometry;if(!Geometry(w,sender,geometry))return false;
            const auto pt=Center(geometry.pulse_items[0]);
            SendOnly(w,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(pt.x,pt.y));
            inject(w,sender,{b});
            SendOnly(w,WM_LBUTTONUP,0,MAKELPARAM(pt.x,pt.y));
            Geometry(w,sender,geometry);
            Check(state.file_ok==0&&geometry.last_activate!=700,"live source replacement cancels pending chip click");
            return state.file_ok==0;
        },"source changes do not retarget pressed chip");
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            (void)sender;
            pulse::ui::PickerInteractionTest q;if(!ready(w,state,q)||!inject(w,sender,{missing}))return false;
            SendOnly(w,WM_SYSKEYDOWN,'0');if(!ready(w,state,q))return false;
            Check(state.file_ok==0,"stale source file fails normal async validation");
            inject(w,sender,{folder},true);pulse::ui::PickerJumpUiTest geometry;Geometry(w,sender,geometry);
            Check(geometry.pulse_count==0,"directory-only source stays hidden in file picker");
            return state.file_ok==0;
        },"invalid selected source never accepts");
    }


    if(run_case(L"save-advisory")) {
        const auto readonly=root/L"read-only.txt";
        std::ofstream(readonly)<<"original read-only content";
        Check(SetFileAttributesW(readonly.c_str(),FILE_ATTRIBUTE_READONLY)!=FALSE,"create actual read-only destination fixture");
        struct RestoreAttributes {std::filesystem::path path;~RestoreAttributes(){SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL);}}restore{readonly};
        config={};config.size=sizeof(config);config.version=2;config.initial_folder=Text(folder);
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;config.save_options=PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
        const std::wstring label=L"Text",pattern=L"*.txt";
        PulsePickerUiV2Filter filter{sizeof(filter),0,Text(label),Text(pattern)};config.filters=&filter;config.filter_count=1;
        for(unsigned variant=0;variant<5;++variant) {
            const std::wstring name=variant==0?L"read-only.txt":L"fresh.txt";config.filename=Text(name);
            config.appearance=variant==1?PULSE_PICKER_UI_V2_DARK:0;
            Run(api,session,config,[&](HWND w,HWND sender,State& state){
                pulse::ui::PickerJumpUiTest q;
                if(!Wait([&]{return Geometry(w,sender,q)&&q.save_draft_checked;},state.done))return false;
                if(variant>=2) {
                    q={};q.set_save_probe_fixture=true;q.save_probe_space_known=variant==3;
                    q.save_probe_attributes=variant==2?FILE_ATTRIBUTE_OFFLINE:INVALID_FILE_ATTRIBUTES;
                    Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q));
                    Geometry(w,sender,q);
                }
                if(variant==0) {
                    Check(q.live_conflict&&!q.save_invalid&&std::wstring(q.save_conflict_text).find(L"只读")!=std::wstring::npos,"actual readonly metadata appears in existing conflict banner");
                    Check(Capture(w,output/L"save-read-only.png"),"capture actual readonly target before Save");
                    Click(w,Center(q.live_rename));
                    Check(Wait([&]{return Geometry(w,sender,q)&&q.save_draft_checked&&!q.live_conflict&&!q.save_warning[0]&&q.save_space[0];},state.done),"rename clears stale readonly warning and refreshes destination space");
                } else if(variant==1) {
                    Check(q.save_space[0]&&q.save_space_rect.right>q.save_space_rect.left,"actual destination free space has a nonoverlapping footer slot");
                    Check(Capture(w,output/L"save-space-dark.png"),"capture space beside preserved destination path");
                    q={};q.client_width_dip=640;Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q));Geometry(w,sender,q);
                    Check(q.save_space_rect.right==q.save_space_rect.left,"narrow footer folds space text rather than squeezing target path");
                    Check(Capture(w,output/L"save-space-narrow.png"),"capture narrow footer with full-size save controls");
                    pulse::ui::PickerJumpUiTest hover;hover.inspect_hover=true;hover.inspect_hover_point=Center(q.save_summary);
                    Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&hover));
                    Check(std::wstring(hover.format_tooltip).find(q.save_space)!=std::wstring::npos,"narrow summary tooltip retains complete measured space");
                    Check(Capture(w,output/L"save-space-tooltip.png"),"capture full destination and measured-space tooltip");
                    pulse::ui::PickerInteractionTest interaction;Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&interaction));Click(w,Center(interaction.primary));
                    return Wait([&]{return state.file_ok==1;},state.done);
                } else if(variant==2) {
                    Check(q.live_conflict&&!q.save_invalid&&std::wstring(q.save_conflict_text).find(L"未完全在本地")!=std::wstring::npos,"injected placeholder state warns without claiming completed cloud sync");
                    Check(Capture(w,output/L"save-online-policy.png"),"capture explicit injected offline-attribute policy state");
                } else if(variant==3) {
                    Check(!q.live_conflict&&!q.save_invalid&&q.primary_enabled&&std::wstring(q.save_warning).find(L"无可用空间")!=std::wstring::npos,"injected zero space warns but final access validation remains authoritative");
                    Check(Capture(w,output/L"save-zero-space-policy.png"),"capture explicit injected zero-space advisory state");
                } else Check(!q.save_space[0]&&!q.save_warning[0]&&q.primary_enabled,"unknown space never becomes a false full-disk warning");
                return true;
            },"save advisory preserves normal callback and cancellation",variant==1?(root/L"fresh.txt").wstring():std::wstring());
        }
        Check(std::filesystem::file_size(readonly)==26&&(GetFileAttributesW(readonly.c_str())&FILE_ATTRIBUTE_READONLY),"advisory leaves actual readonly file and attributes unchanged");
        Check(!std::filesystem::exists(root/L"fresh.txt"),"picker returns a name without pretending the caller saved it");
    }

    if(run_case(L"save-draft")) {
        config={};config.size=sizeof(config);config.version=2;config.initial_folder=Text(folder);
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;config.save_options=PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
        const std::wstring label=L"Text",pattern=L"*.txt",initial=L"report-v009.txt";
        PulsePickerUiV2Filter filter{sizeof(filter),0,Text(label),Text(pattern)};
        config.filters=&filter;config.filter_count=1;config.filename=Text(initial);
        const auto read=[&](HWND w,HWND sender,pulse::ui::PickerJumpUiTest& q){q={};return Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q));};
        const auto type=[&](HWND w,const std::wstring& name){return SendOnly(GetDlgItem(w,102),WM_SETTEXT,0,reinterpret_cast<LPARAM>(name.c_str()));};
        const auto submit=[&](HWND w){pulse::ui::PickerInteractionTest q;Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&q));Click(w,Center(q.primary));};
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;
            if(!Wait([&]{return read(w,sender,q)&&q.ready&&q.suggestion_count>=2;},state.done))return false;
            Check(Capture(w,output/L"save-suggestions-wide.png"),"capture live naming suggestions");
            int version=-1;for(unsigned i=0;i<q.suggestion_count;++i)if(q.suggestion_kinds[i]==0)version=static_cast<int>(i);
            if(version<0)return false;Click(w,Center(q.suggestions[version]));
            if(!Wait([&]{return Filename(w)==L"report-v010.txt";},state.done))return false;
            Check(state.file_ok==0,"version suggestion edits name without saving");
            submit(w);return Wait([&]{return state.file_ok==1;},state.done);
        },"version +1 preserves leading zeros and returns chosen name",(root/L"report-v010.txt").wstring());
        const auto hidden=root/L"hidden.txt";std::ofstream(hidden)<<"unchanged";SetFileAttributesW(hidden.c_str(),FILE_ATTRIBUTE_HIDDEN);
        const std::wstring hidden_name=L"hidden.txt";config.filename=Text(hidden_name);
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return read(w,sender,q)&&q.live_conflict;},state.done))return false;
            Check(!HasRow(w,hidden.wstring())&&!q.primary_enabled&&state.file_ok==0,"hidden conflict appears before Save despite filtered listing");
            Check(Capture(w,output/L"live-conflict-wide.png"),"capture real pre-submit conflict banner");
            const UINT dpi=GetDpiForWindow(w);SetWindowPos(w,nullptr,0,0,MulDiv(420,dpi,96),MulDiv(640,dpi,96),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            if(!Wait([&]{return read(w,sender,q)&&q.live_rename.right>q.live_rename.left;},state.done))return false;
            Check(Capture(w,output/L"live-conflict-narrow.png"),"capture narrow real save banner with suggestions");
            Click(w,Center(q.live_rename));
            if(!Wait([&]{return Filename(w)==L"hidden (2).txt"&&read(w,sender,q)&&!q.live_conflict&&q.primary_enabled;},state.done))return false;
            submit(w);return Wait([&]{return state.file_ok==1;},state.done);
        },"live Rename edits target and uses normal final save validation",(root/L"hidden (2).txt").wstring());
        Check(std::filesystem::file_size(hidden)==9,"renaming save target leaves existing hidden file unchanged");
        const std::wstring existing=L"a.txt";config.filename=Text(existing);
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return read(w,sender,q)&&q.live_conflict;},state.done))return false;
            Click(w,Center(q.live_save));
            if(!Wait([&]{return read(w,sender,q)&&q.overwrite_replace.right>q.overwrite_replace.left;},state.done))return false;
            Check(state.file_ok==0&&std::filesystem::file_size(root/L"a.txt")==1,"live Check and save never bypasses final overwrite confirmation");
            Click(w,Center(q.overwrite_replace));return Wait([&]{return state.file_ok==1;},state.done);
        },"confirmed overwrite retains normal caller FileOk path",(root/L"a.txt").wstring());
        config.filename=Text(initial);
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return read(w,sender,q)&&q.ready;},state.done))return false;
            for(const auto& name:std::vector<std::wstring>{L"CON.txt",L"bad?name.txt",L"trailing. ",std::wstring(260,L'x')+L".txt"}){
                type(w,name);
                const bool invalid=Wait([&]{return read(w,sender,q)&&q.save_invalid&&!q.primary_enabled&&q.save_warning[0];},state.done);
                Check(invalid,"invalid/reserved/long filename explains error before submit");if(!invalid)return false;
                const auto generation=q.validation_generation;SendOnly(GetDlgItem(w,102),WM_KEYDOWN,VK_RETURN,0);read(w,sender,q);
                Check(q.validation_generation==generation,"disabled save does not validate through keyboard Enter");
            }
            type(w,L"");read(w,sender,q);Check(!q.save_invalid&&!q.save_warning[0],"clearing filename removes stale invalid warning");
            type(w,L"changed.txt");if(!Wait([&]{return read(w,sender,q)&&q.primary_enabled;},state.done))return false;
            int original=-1;for(unsigned i=0;i<q.suggestion_count;++i)if(q.suggestion_kinds[i]==2)original=static_cast<int>(i);
            if(original<0||q.suggestions[original].right<=q.suggestions[original].left)return false;
            Click(w,Center(q.suggestions[original]));
            Check(Wait([&]{return Filename(w)==initial;},state.done),"Original restores explicit caller filename rather than guessing");
            type(w,L"a.txt");type(w,L"fresh.txt");
            const bool latest=Wait([&]{return read(w,sender,q)&&q.save_draft_checked&&!q.live_conflict&&!q.save_invalid&&q.primary_enabled;},state.done);
            Check(latest,"rapid conflict-to-fresh edit discards obsolete draft probe");
            return latest&&state.file_ok==0;
        },"invalid names and suggestion edits never invoke FileOk");
        const auto other=root/L"other";std::filesystem::create_directory(other);std::ofstream(other/L"collision.txt")<<"keep";
        const auto absolute=(other/L"collision.txt").wstring();config.filename=Text(absolute);
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return read(w,sender,q)&&q.live_conflict;},state.done))return false;
            Check(!HasRow(w,absolute)&&!q.primary_enabled,"full-path target in another directory receives live conflict warning");
            type(w,other.wstring());
            const bool directory=Wait([&]{return read(w,sender,q)&&q.save_draft_checked&&q.save_invalid&&!q.primary_enabled;},state.done);
            Check(directory,"existing directory target cannot be mistaken for replaceable file");
            return directory&&state.file_ok==0;
        },"cross-directory and directory conflicts checked asynchronously");
        SetFileAttributesW(hidden.c_str(),FILE_ATTRIBUTE_NORMAL);
    }

    if(run_case(L"preview")) {
        config={};config.size=sizeof(config);config.version=2;config.initial_folder=Text(folder);config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
        config.open_options=PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT;
        const auto resize=[&](HWND w,HWND sender,float width){pulse::ui::PickerJumpUiTest q;q.client_width_dip=width;return Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q));};
        const auto key=[&](HWND w,UINT value,bool ctrl=false){pulse::ui::PickerInteractionTest q;q.key=value;q.ctrl=ctrl;return Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&q));};
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done))return false;
            key(w,VK_DOWN);Geometry(w,sender,q);const auto before=q.selected_count;const bool preview=q.preview_requested;
            key(w,VK_SPACE);Geometry(w,sender,q);Check(q.preview_requested!=preview&&q.selected_count==before,"Space toggles preview without changing multiselection");
            if(!q.preview_requested)key(w,VK_SPACE);
            resize(w,sender,800);Geometry(w,sender,q);Check(q.preview_width_dip>0,"picker preview remains visible at 800 DIP");
            Check(q.preview_copy.right>q.preview_copy.left&&q.preview_reveal.right>q.preview_reveal.left,"visible preview actions have matching real hit targets");
            pulse::ui::PickerJumpUiTest capture;capture.middle_fixture_folder=folder.c_str();Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&capture));
            Geometry(w,sender,q);Click(w,Center(q.preview_reveal));Geometry(w,sender,q);
            Check(q.middle_launch_count==1&&q.middle_file&&q.middle_path[0],"preview reveal routes selected file through existing Pulse launch helper");
            HWND preview_child=nullptr;
            const bool content=Wait([&]{preview_child=FindWindowExW(w,nullptr,L"Pulse.QuickPreview",nullptr);return preview_child&&IsWindowVisible(preview_child)&&GetPropW(preview_child,L"Pulse.PreviewTestContentReady");},state.done);
            Check(content,"embedded preview child renders real selected-file content");
            if(content){const auto snapshot=(output/L"preview-content-800.png").wstring();SetPropW(preview_child,L"Pulse.PreviewTestSnapshot",const_cast<wchar_t*>(snapshot.c_str()));SendOnly(preview_child,WM_PAINT);RemovePropW(preview_child,L"Pulse.PreviewTestSnapshot");Check(std::filesystem::exists(snapshot),"capture embedded preview content separately from parent compositor");}
            Check(Capture(w,output/L"preview-800.png"),"capture real 800 DIP preview metadata and actions");
            resize(w,sender,720);Geometry(w,sender,q);Check(q.preview_width_dip==0&&q.preview_requested,"below 760 DIP preview collapses without losing preference");
            resize(w,sender,800);Geometry(w,sender,q);Check(q.preview_width_dip>0,"preview returns when window widens");
            const bool keep=q.preview_requested;key(w,VK_SPACE,true);Geometry(w,sender,q);Check(q.preview_requested==keep,"Ctrl Space retains selection semantics instead of toggling preview");
            const auto edit=GetDlgItem(w,102);SendOnly(edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"two"));SendOnly(edit,EM_SETSEL,3,3);SendOnly(edit,WM_CHAR,L' ',0);
            Geometry(w,sender,q);Check(Filename(w)==L"two "&&q.preview_requested==keep,"Space in filename editor remains text input");
            return state.file_ok==0;
        },"Space and narrow preview interactions cancel without returning files");
        config.native_options=FOS_FORCEPREVIEWPANEON;
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done))return false;
            key(w,VK_DOWN);resize(w,sender,720);key(w,VK_SPACE);Geometry(w,sender,q);
            Check(q.preview_requested&&q.preview_width_dip>0,"caller-forced preview survives narrow width and Space");
            Check(Capture(w,output/L"preview-forced-720.png"),"capture caller-forced narrow preview");return state.file_ok==0;
        },"forced preview contract preserved");
    }

    if(run_case(L"selection-footer")) {
        config={};config.size=sizeof(config);config.version=2;config.initial_folder=Text(folder);config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
        config.open_options=PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT;
        const auto left=root/L"left",right=root/L"right";std::filesystem::create_directory(left);std::filesystem::create_directory(right);
        const auto a=(left/L"report.txt").wstring(),b=(right/L"report.txt").wstring();
        std::ofstream(a)<<"left";std::ofstream(b)<<"right";
        std::vector<std::wstring> tray_files{a,b};
        for(int i=0;i<6;++i){const auto path=(right/(L"long-selected-file-for-overflow-"+std::to_wstring(i)+L".txt")).wstring();std::ofstream(path)<<"sample";tray_files.push_back(path);}
        const auto inject=[&](HWND w,HWND sender,const std::vector<std::wstring>& paths){
            pulse::ui::PickerJumpUiTest q;q.set_pulse_fixture=true;q.pulse_count=static_cast<unsigned>(paths.size());
            q.pulse_sizes_present=true;
            for(unsigned i=0;i<q.pulse_count;++i){q.pulse_paths[i]=paths[i].c_str();q.pulse_sizes[i]=std::filesystem::file_size(paths[i]);}
            return Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q));
        };
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done))return false;
            if(!inject(w,sender,{a}))return false;SendOnly(w,WM_SYSKEYDOWN,'0');
            if(!Wait([&]{return Geometry(w,sender,q)&&q.footer_selection_count==1;},state.done))return false;
            Check(std::wstring(q.selection_sources[0])==L"left"&&q.selection_clear.right>q.selection_clear.left,
                "one selected file has parent source and reachable clear action");
            Check(!IsWindowVisible(GetDlgItem(w,102)),"single multiselect item uses selected chip instead of editable filename overlay");
            if(!inject(w,sender,{a,b}))return false;SendOnly(w,WM_SYSKEYDOWN,'0');
            if(!Wait([&]{return Geometry(w,sender,q)&&q.footer_selection_count==2;},state.done))return false;
            Check(std::wstring(q.selection_sources[0])==L"left"&&std::wstring(q.selection_sources[1])==L"right",
                "identical names keep distinct parent source labels");
            Check(std::wstring(q.selection_summary).find(L"9 B")!=std::wstring::npos,"selection total matches both real source file sizes");
            Check(Capture(w,output/L"selection-sources.png"),"capture actual cross-directory selected sources");
            if(q.selection_more.right>q.selection_more.left){
                Click(w,Center(q.selection_more));if(!Wait([]{return Menu()!=nullptr;},state.done))return false;
                Check(Capture(Menu(),output/L"selection-more.png"),"capture actual overflow removal menu with source labels");
                SendOnly(Menu(),WM_MOUSELEAVE);PostMessageW(Menu(),WM_KEYDOWN,VK_DOWN,0);PostMessageW(Menu(),WM_KEYDOWN,VK_RETURN,0);
            }else if(q.remove_count)Click(w,Center(q.remove_selection[0]));else return false;
            if(!Wait([&]{return Geometry(w,sender,q)&&q.footer_selection_count==1;},state.done))return false;
            Check(std::wstring(q.selection_sources[0])==L"right","removing one duplicate name preserves the other full path");
            Click(w,Center(q.selection_clear));
            if(!Wait([&]{return Geometry(w,sender,q)&&q.footer_selection_count==0&&q.basket_count==0;},state.done))return false;
            Check(!q.primary_enabled&&Filename(w).empty()&&!q.selection_summary[0]&&IsWindowVisible(GetDlgItem(w,102)),
                "clear resets callback candidate, summary and editor without stale selected files");
            Check(std::filesystem::exists(a)&&std::filesystem::exists(b)&&state.file_ok==0,"clear never deletes source files or submits a callback");
            pulse::ui::PickerInteractionTest address;address.key='L';address.ctrl=true;
            Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&address));
            SendOnly(GetDlgItem(w,101),WM_SETTEXT,0,reinterpret_cast<LPARAM>(left.c_str()));
            address={};address.key=VK_RETURN;address.edit_id=101;
            Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&address));
            const bool navigated=Wait([&]{pulse::ui::PickerInteractionTest state_query;return Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&state_query))&&state_query.current==left.wstring()&&Geometry(w,sender,q);},state.done);
            Check(navigated&&q.footer_selection_count==0&&q.basket_count==0,"navigation after clear does not resurrect cached selections");
            return navigated;
        },"cross-directory remove and clear remain isolated to this dialog");
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done))return false;
            q={};q.set_tray_fixture=true;q.tray_revision=1;q.tray_count=8;q.tray_total=8;
            for(unsigned i=0;i<8;++i){q.tray_paths[i]=tray_files[i].c_str();q.tray_sizes[i]=std::filesystem::file_size(tray_files[i]);}
            Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q));
            if(!Wait([&]{return Geometry(w,sender,q)&&q.tray_all_token[0];},state.done))return false;
            pulse::ui::PickerPlacesTestPoint target;target.kind=pulse::ui::PickerPlacesTestPoint::Kind::Sidebar;target.path=q.tray_all_token;
            if(!Send(w,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&target)))return false;Click(w,target.point);
            if(!Wait([&]{return Geometry(w,sender,q)&&q.footer_selection_count==8;},state.done))return false;
            Check(q.selection_tray[0]&&q.selection_tray[1],"tray backing paths carry staging-tray source markers");
            Check(Capture(w,output/L"selection-tray.png"),"capture selected tray source labels before narrowing");
            q={};q.client_width_dip=720;Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q));
            if(!Wait([&]{return Geometry(w,sender,q)&&q.selection_more.right>q.selection_more.left;},state.done))return false;
            Check(Capture(w,output/L"selection-tray-narrow.png"),"capture narrow footer with tray sources, More and Clear");
            Geometry(w,sender,q);Click(w,Center(q.selection_more));
            if(!Wait([]{return Menu()!=nullptr;},state.done))return false;
            Check(Capture(Menu(),output/L"selection-more.png"),"capture actual overflow removal menu with tray sources");
            SendOnly(Menu(),WM_MOUSELEAVE);PostMessageW(Menu(),WM_KEYDOWN,VK_DOWN,0);PostMessageW(Menu(),WM_KEYDOWN,VK_RETURN,0);
            if(!Wait([&]{return !Menu()&&Geometry(w,sender,q)&&q.footer_selection_count==7;},state.done))return false;
            Check(true,"overflow menu removes one selected path without truncating the other seven");
            // Exercise keyboard focus through application-internal key handling.
            for(int i=0;i<40&&!q.selection_clear_focused;++i) {
                pulse::ui::PickerInteractionTest key;key.key=VK_TAB;
                Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&key));Geometry(w,sender,q);
            }
            Check(q.selection_clear_focused,"Tab reaches the visible Clear control");
            if(!q.selection_clear_focused)return false;
            pulse::ui::PickerInteractionTest enter;enter.key=VK_RETURN;
            Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&enter));
            if(!Wait([&]{return Geometry(w,sender,q)&&q.footer_selection_count==0;},state.done))return false;
            Check(q.tray_total==8&&q.tray_present&&std::all_of(tray_files.begin(),tray_files.end(),[](const auto& file){return std::filesystem::exists(file);}),
                "clearing selected tray files leaves tray snapshot and backing files intact");
            return true;
        },"tray selection clear does not mutate staging tray");
    }
    if(run_case(L"display-settings")) {
        config={};config.size=sizeof(config);config.version=2;config.initial_folder=Text(folder);config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
        const auto hidden=(root/L"hidden.txt").wstring(), protected_file=(root/L"protected.txt").wstring(), system=(root/L"system.txt").wstring();
        const auto child=root/L"z.folder";std::filesystem::create_directory(child);
        for(const auto& path:{hidden,protected_file,system})std::ofstream(std::filesystem::path(path))<<"display sample";
        Check(SetFileAttributesW(hidden.c_str(),FILE_ATTRIBUTE_HIDDEN)&&SetFileAttributesW(protected_file.c_str(),FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM)&&SetFileAttributesW(system.c_str(),FILE_ATTRIBUTE_SYSTEM),"set real hidden, protected and system-only sample attributes");
        const auto send_settings=[&](HWND w,HWND sender,const wchar_t* json){pulse::ui::PickerJumpUiTest q;q.display_prefs_json=json;return Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q));};
        const auto command=[&](HWND w,HWND sender,int value){pulse::ui::PickerJumpUiTest q;q.display_command=value;return Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q));};
        const auto drawn=[](const pulse::ui::PickerJumpUiTest& q,const wchar_t* raw,const wchar_t* display){for(unsigned i=0;i<q.display_count;++i)if(std::wstring(q.raw_names[i])==raw)return std::wstring(q.drawn_names[i])==display;return false;};
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done))return false;
            if(!send_settings(w,sender,LR"({"show_hidden_files":false,"show_protected_os_files":false,"show_file_extensions":false,"folder_sort_mode":0})"))return false;
            const bool initial=Wait([&]{SendOnly(w,WM_PAINT);return Geometry(w,sender,q)&&q.display_count==4&&drawn(q,L"a.txt",L"a");},state.done);
            Check(initial&&!HasRow(w,hidden)&&!HasRow(w,protected_file)&&HasRow(w,system),"actual rows hide known extensions and hidden files but keep system-only files");
            Check(drawn(q,L"z.folder",L"z.folder"),"directory suffix remains visible");
            Check(Capture(w,output/L"display-hidden-extensions.png"),"capture actual extension-hidden dialog");
            if(!initial)return false;
            command(w,sender,2);
            if(!Wait([&]{return Geometry(w,sender,q)&&q.display_count==5;},state.done))return false;
            Check(HasRow(w,hidden)&&!HasRow(w,protected_file),"show hidden does not reveal protected files");
            command(w,sender,51);
            if(!Wait([&]{return Geometry(w,sender,q)&&q.display_count==6;},state.done))return false;
            Check(HasRow(w,protected_file),"separate protected-files toggle reveals actual protected sample");
            command(w,sender,2);
            if(!Wait([&]{return Geometry(w,sender,q)&&q.display_count==4;},state.done))return false;
            Check(!HasRow(w,protected_file)&&q.show_protected,"protected setting alone does not bypass hidden gate");
            command(w,sender,50);
            if(!Wait([&]{SendOnly(w,WM_PAINT);return Geometry(w,sender,q)&&drawn(q,L"a.txt",L"a.txt");},state.done))return false;
            Check(true,"extension toggle invalidates the shared renderer name cache");
            command(w,sender,45);
            if(!Wait([&]{return Geometry(w,sender,q)&&q.folder_mode==1&&std::wstring(q.raw_names[q.display_count-1])==L"z.folder";},state.done))return false;
            Check(true,"folders-last command actually reorders the loader result");
            command(w,sender,40);
            if(!Wait([&]{return Geometry(w,sender,q)&&q.sort_column==5;},state.done))return false;
            command(w,sender,15);
            if(!Wait([&]{return Geometry(w,sender,q)&&q.sort_descending;},state.done))return false;
            Check(q.folder_mode==1,"creation-date descending keeps folders-last rule");
            command(w,sender,46);
            if(!Wait([&]{return Geometry(w,sender,q)&&q.folder_mode==2;},state.done))return false;
            command(w,sender,41);
            if(!Wait([&]{return Geometry(w,sender,q)&&q.sort_column==6;},state.done))return false;
            command(w,sender,42);
            if(!Wait([&]{return Geometry(w,sender,q)&&q.sort_column==4;},state.done))return false;
            Check(true,"mixed folders, access time and path menu commands load successfully");
            Check(Capture(w,output/L"display-before-menu.png"),"final sort state has a painted frame before menu input");
            Geometry(w,sender,q);
            if(q.view_menu.right>q.view_menu.left){
                Click(w,Center(q.view_menu));
                if(!Wait([]{return Menu()!=nullptr;},state.done)) {
                    Geometry(w,sender,q);DWORD foreground_pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground_pid);
                    std::printf("DISPLAY_MENU_OPEN_DIAGNOSTIC hit=%d/%d activate=%d pressed=%d rect=%ld,%ld,%ld,%ld own_foreground=%d\n",q.last_down_hit,q.last_up_hit,q.last_activate,q.current_pressed,q.view_menu.left,q.view_menu.top,q.view_menu.right,q.view_menu.bottom,foreground_pid==GetCurrentProcessId());
                    return false;
                }
                Check(Capture(Menu(),output/L"display-view-menu.png"),"capture reused Fluent menu with display switches");
                // A real popup may open under the current pointer, so its initial
                // hover is not guaranteed to be the first row. Reset only this
                // owned menu through its internal message before navigating.
                SendOnly(Menu(),WM_MOUSELEAVE);
                for(int i=0;i<5;++i)PostMessageW(Menu(),WM_KEYDOWN,VK_DOWN,0);
                PostMessageW(Menu(),WM_KEYDOWN,VK_RETURN,0);
                const bool toggled=Wait([&]{return !Menu()&&Geometry(w,sender,q)&&!q.show_extensions;},state.done);
                if(!toggled)std::printf("DISPLAY_MENU_DIAGNOSTIC open=%d hidden=%d extensions=%d protected=%d\n",Menu()!=nullptr,q.show_hidden,q.show_extensions,q.show_protected);
                Check(toggled,"actual Fluent menu selection dispatches extension toggle");
                if(!toggled)return false;
            }else Check(false,"shared view menu button hit target exists");
            return true;
        },"display preferences, visibility gates and real sorting commands complete");
        const auto selected=(root/L"a.txt").wstring();
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done))return false;
            if(!send_settings(w,sender,LR"({"show_hidden_files":false,"show_file_extensions":false})"))return false;
            if(!Wait([&]{SendOnly(w,WM_PAINT);return Geometry(w,sender,q)&&drawn(q,L"a.txt",L"a");},state.done))return false;
            pulse::ui::PickerPlacesTestPoint row;row.path=selected.c_str();if(!Send(w,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&row)))return false;
            Click(w,row.point);
            pulse::ui::PickerInteractionTest key;key.key=VK_RETURN;
            Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&key));
            return Wait([&]{return state.file_ok>0;},state.done);
        },"extension-hidden row returns its full original filename",selected);
        config.open_options=PULSE_PICKER_UI_V2_OPEN_SHOW_HIDDEN;
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done))return false;
            if(!send_settings(w,sender,LR"({"show_hidden_files":false,"show_protected_os_files":false,"show_file_extensions":true})"))return false;
            const bool all=Wait([&]{return Geometry(w,sender,q)&&q.display_count==6;},state.done);
            Check(all&&q.show_hidden&&q.show_protected,"explicit caller force-show survives both disabled Pulse settings");
            return all;
        },"caller force-show contract remains intact");
        config.open_options=0;config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
        config.save_options=PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
        const std::wstring save_name=L"display-save.txt";config.filename=Text(save_name);
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done))return false;
            if(!send_settings(w,sender,LR"({"show_hidden_files":false,"show_file_extensions":false})"))return false;
            if(!Wait([&]{SendOnly(w,WM_PAINT);return Geometry(w,sender,q)&&drawn(q,L"a.txt",L"a")&&q.primary_enabled;},state.done))return false;
            Check(Filename(w)==save_name,"hiding list extensions does not strip the Save filename editor");
            pulse::ui::PickerInteractionTest key;key.key=VK_RETURN;key.edit_id=102;
            Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&key));
            return Wait([&]{return state.file_ok>0;},state.done);
        },"Save callback retains complete extension",(root/save_name).wstring());
        for(const auto& path:{hidden,protected_file,system})SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL);
    }
    if(run_case(L"new-files")) {
        config={};config.size=sizeof(config);config.version=2;config.initial_folder=Text(folder);config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
        const auto old=(root/L"a.txt").wstring(), fresh=(root/L"b.txt").wstring();
        FILETIME now{};GetSystemTimeAsFileTime(&now);const uint64_t ticks=(static_cast<uint64_t>(now.dwHighDateTime)<<32)|now.dwLowDateTime;
        const uint64_t old_ticks=ticks-30ull*864000000000ull;FILETIME stamp{static_cast<DWORD>(old_ticks),static_cast<DWORD>(old_ticks>>32)};
        const auto file=CreateFileW(old.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        Check(file!=INVALID_HANDLE_VALUE&&SetFileTime(file,nullptr,nullptr,&stamp),"prepare isolated old/new files with actual modification times");if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done))return false;
            q={};q.set_new_since=ticks-7ull*864000000000ull;Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q));
            if(!Wait([&]{return Geometry(w,sender,q)&&q.time_filter.right>q.time_filter.left;},state.done))return false;
            Check(q.only_new_filter.right<=q.only_new_filter.left,"only-new is merged into the Time dropdown rather than a separate chip");
            const bool badges=Wait([&]{SendOnly(w,WM_PAINT);return Geometry(w,sender,q)&&HasRow(w,old)&&HasRow(w,fresh)&&q.new_badge_count==1;},state.done);
            std::printf("NEW_BADGE_DIAGNOSTIC badges=%u old_row=%d fresh_row=%d time=%d only=%d\n",q.new_badge_count,HasRow(w,old),HasRow(w,fresh),q.active_time_filter,q.only_new);
            Check(badges,"new badge is supplied only for the fresh file before filtering");
            Click(w,Center(q.time_filter));if(!Wait([]{return Menu()!=nullptr;},state.done))return false;
            SendOnly(Menu(),WM_MOUSELEAVE);
            for(int i=0;i<4;++i)PostMessageW(Menu(),WM_KEYDOWN,VK_DOWN,0);
            PostMessageW(Menu(),WM_KEYDOWN,VK_RETURN,0);
            if(!Wait([&]{return !Menu()&&Geometry(w,sender,q)&&q.only_new&&!HasRow(w,old)&&HasRow(w,fresh);},state.done))return false;
            Check(q.active_time_filter==0,"selecting only-new from Time dropdown filters to fresh files");
            Click(w,Center(q.time_filter));if(!Wait([]{return Menu()!=nullptr;},state.done))return false;
            SendOnly(Menu(),WM_MOUSELEAVE);
            for(int i=0;i<2;++i)PostMessageW(Menu(),WM_KEYDOWN,VK_DOWN,0);
            PostMessageW(Menu(),WM_KEYDOWN,VK_RETURN,0);
            if(!Wait([&]{return !Menu()&&Geometry(w,sender,q)&&q.active_time_filter==1&&!q.only_new;},state.done))return false;
            Check(HasRow(w,fresh)&&!HasRow(w,old),"switching Time dropdown to Today clears only-new and keeps Today filter");
            Check(Capture(w,output/L"new-files-today.png"),"capture active Time dropdown chip and new row badge");
            q={};q.client_width_dip=720;Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q));
            Geometry(w,sender,q);Check(q.compact_filters.right>q.compact_filters.left,"narrow window exposes one compact Filter button");
            Click(w,Center(q.compact_filters));if(!Wait([]{return Menu()!=nullptr;},state.done))return false;
            SendOnly(Menu(),WM_MOUSELEAVE);
            for(int i=0;i<6;++i)PostMessageW(Menu(),WM_KEYDOWN,VK_DOWN,0);PostMessageW(Menu(),WM_KEYDOWN,VK_RETURN,0);
            if(!Wait([&]{return !Menu()&&Geometry(w,sender,q)&&q.only_new;},state.done))return false;
            Check(q.active_time_filter==1,"compact menu keeps date and new-file controls reachable");
            Check(Capture(w,output/L"new-files-compact-720.png"),"capture compact Filter button without clipped chips");
            return state.file_ok==0;
        },"merged Time dropdown and new-file filter interactions complete without accepting files");
    }

    if(run_case(L"tray")) {
        config={};config.size=sizeof(config);config.version=2;config.initial_folder=Text(folder);config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
        const std::wstring label=L"Text",pattern=L"*.txt";
        PulsePickerUiV2Filter filter{sizeof(filter),0,Text(label),Text(pattern)};config.filters=&filter;config.filter_count=1;
        const auto shelf=root/L"shelf";std::filesystem::create_directory(shelf);
        const unsigned char tray_png[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,32,0,0,0,32,8,6,0,0,0,115,122,122,244,0,0,0,76,73,68,65,84,120,156,237,206,49,17,0,32,12,67,209,42,97,70,4,218,81,130,8,28,20,7,205,192,245,178,252,33,227,191,188,184,107,102,181,177,79,185,223,62,0,0,0,96,7,116,31,168,30,0,0,0,126,64,247,129,234,1,0,0,224,7,116,31,168,30,0,0,0,225,6,60,57,13,20,151,241,129,62,75,0,0,0,0,73,69,78,68,174,66,96,130};
        std::vector<std::wstring> files;for(unsigned i=0;i<7;++i){const auto path=shelf/(L"item"+std::to_wstring(i)+(i==0?L".png":L".txt"));
            std::ofstream f(path,std::ios::binary);if(i==0)f.write(reinterpret_cast<const char*>(tray_png),sizeof(tray_png));else f<<"x";files.push_back(path.wstring());}
        uint64_t revision=0;
        const auto inject=[&](HWND w,HWND sender,const std::vector<std::wstring>& paths,uint64_t total=0){
            pulse::ui::PickerJumpUiTest q;q.set_tray_fixture=true;q.tray_revision=++revision;q.tray_count=static_cast<unsigned>(paths.size());q.tray_total=total;
            for(unsigned i=0;i<q.tray_count;++i){q.tray_paths[i]=paths[i].c_str();q.tray_sizes[i]=std::filesystem::file_size(paths[i]);}
            return Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&q));
        };
        const auto click_path=[&](HWND w,const std::wstring& token){pulse::ui::PickerPlacesTestPoint point;point.kind=pulse::ui::PickerPlacesTestPoint::Kind::Sidebar;point.path=token.c_str();if(!Send(w,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&point)))return false;Click(w,point.point);return true;};
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done)||!inject(w,sender,files))return false;
            Geometry(w,sender,q);Check(q.tray_present&&q.tray_count==5&&q.tray_more_token[0],"tray displays first five compatible files and expands remaining items");
            Check(Capture(w,output/L"tray-single.png"),"capture real top-sidebar tray group");
            if(!click_path(w,q.tray_tokens[0]))return false;
            return Wait([&]{return state.file_ok==1;},state.done);
        },"single tray choice returns original backing path",files[1]);
        config.open_options=PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT;
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done)||!inject(w,sender,files))return false;
            Geometry(w,sender,q);if(!click_path(w,q.tray_more_token))return false;
            if(!Wait([&]{return Geometry(w,sender,q)&&q.tray_count==7;},state.done))return false;
            const auto incompatible=std::wstring(q.tray_tokens[6]);if(!click_path(w,incompatible))return false;
            Geometry(w,sender,q);Check(q.basket_count==0&&state.file_ok==0,"incompatible expanded tray entry never enters selection");
            if(!click_path(w,q.tray_tokens[0]))return false;
            if(!Wait([&]{return Geometry(w,sender,q)&&q.basket_count==1;},state.done))return false;
            if(!click_path(w,q.tray_tokens[0]))return false;
            if(!Wait([&]{return Geometry(w,sender,q)&&q.basket_count==0;},state.done))return false;
            Check(state.file_ok==0,"tray checkbox toggles picker basket without modifying tray");
            if(!click_path(w,q.tray_all_token))return false;
            if(!Wait([&]{return Geometry(w,sender,q)&&q.basket_count==6;},state.done))return false;
            Check(Capture(w,output/L"tray-multi.png"),"capture compatible tray files added to basket");
            Check(reinterpret_cast<ULONG_PTR>(GetPropW(w,L"Pulse.SidebarThumb.Checked"))>=6,"all compatible non-image tray rows use real checked controls, not icon glyphs");
            inject(w,sender,{});Geometry(w,sender,q);Check(!q.tray_present&&q.basket_count==6,"empty live tray hides group while chosen files remain");
            pulse::ui::PickerInteractionTest interaction;Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&interaction));Click(w,Center(interaction.primary));
            return Wait([&]{return state.file_ok==1;},state.done);
        },"tray Add all returns six compatible originals after explicit submit",files[1],UINT32_MAX,{},6);
        config.open_options=0;
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done)||!inject(w,sender,{files[1]}))return false;
            Geometry(w,sender,q);const std::wstring old=q.tray_tokens[0];inject(w,sender,{files[2]});
            pulse::ui::PickerPlacesTestPoint stale;stale.kind=pulse::ui::PickerPlacesTestPoint::Kind::Sidebar;stale.path=old.c_str();
            Check(!Send(w,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&stale)),"source revision change invalidates old tray action identity");
            inject(w,sender,{},65);Geometry(w,sender,q);Check(q.tray_present&&q.tray_count==0&&!q.tray_all_token[0],"incomplete tray snapshot reports unavailable rather than silently truncating Add all");
            inject(w,sender,{});Geometry(w,sender,q);return !q.tray_present&&state.file_ok==0;
        },"stale and incomplete tray snapshots never submit");
        std::vector<std::wstring> many=files;
        for(unsigned i=7;i<66;++i){const auto path=shelf/(L"item"+std::to_wstring(i)+L".txt");std::ofstream(path)<<"x";many.push_back(path.wstring());}
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done)||!inject(w,sender,many))return false;
            Geometry(w,sender,q);if(!click_path(w,q.tray_more_token))return false;
            for(unsigned page=0;page<7;++page){
                if(!Geometry(w,sender,q)||q.tray_page!=page||!q.tray_next_token[0]||!click_path(w,q.tray_next_token))return false;
            }
            Geometry(w,sender,q);Check(q.tray_page==7&&q.tray_count==8&&q.tray_previous_token[0],"66-item tray reaches files beyond original 64-row transport limit");
            const auto selected=std::wstring(q.tray_tokens[7]);
            if(!click_path(w,q.tray_previous_token))return false;
            Geometry(w,sender,q);Check(q.tray_page==6,"tray previous page preserves complete ordering");
            if(!click_path(w,q.tray_next_token))return false;
            Check(Capture(w,output/L"tray-page-eight.png"),"capture bounded tray page and previous/next actions");
            if(!click_path(w,selected))return false;
            return Wait([&]{return state.file_ok==1;},state.done);
        },"paged tray single choice returns item 65 original path",many[64]);
        config.open_options=PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT;
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done)||!inject(w,sender,many))return false;
            Geometry(w,sender,q);if(!click_path(w,q.tray_all_token))return false;
            if(!Wait([&]{return Geometry(w,sender,q)&&q.basket_count==65;},state.done))return false;
            Check(state.file_ok==0,"Add all includes all 65 compatible files, not only visible page");
            pulse::ui::PickerInteractionTest interaction;Send(w,pulse::ui::kPickerInteractionTestMessage,0,reinterpret_cast<LPARAM>(&interaction));Click(w,Center(interaction.primary));
            return Wait([&]{return state.file_ok==1;},state.done);
        },"paged tray Add all returns every compatible original",many[1],UINT32_MAX,{},65);
        config.filters=nullptr;config.filter_count=0;
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;if(!Wait([&]{return Geometry(w,sender,q);},state.done)||!inject(w,sender,{files[0],files[1]}))return false;
            const bool decoded=Wait([&]{SendOnly(w,WM_PAINT);return reinterpret_cast<ULONG_PTR>(GetPropW(w,L"Pulse.SidebarThumb.Ready"))>0;},state.done);
            Check(decoded,"real PNG backing decoded by shared sidebar ThumbnailCache");
            Check(reinterpret_cast<ULONG_PTR>(GetPropW(w,L"Pulse.SidebarThumb.Checks"))>0,"thumbnail preserves separate multiselect checkbox");
            Geometry(w,sender,q);if(!click_path(w,q.tray_tokens[0]))return false;
            if(!Wait([&]{return Geometry(w,sender,q)&&q.basket_count==1;},state.done))return false;
            const bool checked=Wait([&]{SendOnly(w,WM_PAINT);return reinterpret_cast<ULONG_PTR>(GetPropW(w,L"Pulse.SidebarThumb.Checked"))>0;},state.done);
            Check(checked,"thumbnail checkbox reflects selected original path");
            Check(Capture(w,output/L"tray-real-thumbnail.png"),"capture tray image state after decoder and checkbox assertions");
            if(!click_path(w,q.tray_tokens[0]))return false;
            return Wait([&]{return Geometry(w,sender,q)&&q.basket_count==0;},state.done)&&state.file_ok==0;
        },"image thumbnail and checkbox share production tray interactions");
        Check(std::all_of(many.begin()+1,many.end(),[](const auto& path){return std::filesystem::exists(path)&&std::filesystem::file_size(path)==1;})&&std::filesystem::file_size(many[0])==sizeof(tray_png),"picker never deletes, moves or modifies tray backing files");
    }
    if(run_case(L"save-tray")) {
        config={};config.size=sizeof(config);config.version=2;config.initial_folder=Text(folder);
        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;config.save_options=PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
        const std::wstring tl=L"Text",il=L"PNG",tp=L"*.txt",ip=L"*.png",name=L"staged-export.txt";
        PulsePickerUiV2Filter filters[2]={{sizeof(PulsePickerUiV2Filter),0,Text(tl),Text(tp)},{sizeof(PulsePickerUiV2Filter),0,Text(il),Text(ip)}};
        config.filters=filters;config.filter_count=2;config.filter_index=0;config.filename=Text(name);
        std::ofstream(root/L"other-type.png")<<"png";
        Run(api,session,config,[&](HWND w,HWND sender,State& state){
            pulse::ui::PickerJumpUiTest q;
            if(!Wait([&]{return Geometry(w,sender,q)&&q.save_to_tray_token[0]&&q.save_filter_same.right>q.save_filter_same.left;},state.done))return false;
            Check(q.preview_width_dip>0&&q.save_notify_option.right>q.save_notify_option.left&&q.save_copy_option.right>q.save_copy_option.left,
                  "Save mode opens destination preview with post-save switches");
            Check(HasRow(w,(root/L"other-type.png").wstring()),"Save All-files filter keeps mismatched files visible");
            Click(w,Center(q.save_filter_same));
            if(!Wait([&]{return Geometry(w,sender,q)&&!HasRow(w,(root/L"other-type.png").wstring());},state.done))return false;
            Check(true,"Save same-format chip filters out mismatched files");
            Click(w,Center(q.save_filter_all));
            if(!Wait([&]{return Geometry(w,sender,q)&&HasRow(w,(root/L"other-type.png").wstring());},state.done))return false;
            { pulse::ui::PickerJumpUiTest ep; ep.save_followup_endpoint = sender; Send(w,pulse::ui::kPickerJumpUiTestMessage,reinterpret_cast<WPARAM>(sender),reinterpret_cast<LPARAM>(&ep)); }
            if(!Wait([&]{return Geometry(w,sender,q)&&q.save_followup_available;},state.done))return false;
            Click(w,Center(q.save_notify_option));
            if(!Wait([&]{return Geometry(w,sender,q)&&q.save_notify;},state.done))return false;
            Click(w,Center(q.save_copy_option));
            if(!Wait([&]{return Geometry(w,sender,q)&&q.save_copy;},state.done))return false;
            pulse::ui::PickerPlacesTestPoint tray_pt;tray_pt.kind=pulse::ui::PickerPlacesTestPoint::Kind::Sidebar;tray_pt.path=q.save_to_tray_token;
            if(!Send(w,pulse::ui::kPickerPlacesTestPointMessage,0,reinterpret_cast<LPARAM>(&tray_pt)))return false;
            Click(w,tray_pt.point);
            if(!Wait([&]{return Geometry(w,sender,q)&&q.saving_to_tray;},state.done))return false;
            Check(Capture(w,output/L"save-to-tray.png"),"capture Save-to-Tray active destination preview");
            Click(w,tray_pt.point);
            if(!Wait([&]{return Geometry(w,sender,q)&&!q.saving_to_tray;},state.done))return false;
            return state.file_ok==0;
        },"Save mode same-format filter, post-save options, and Save-to-Tray toggle cleanly");
    }
    Check(api.Shutdown(session)==S_OK,"real picker session shuts down");CoUninitialize();
    std::printf("FAILURES=%u\n",failures.load());return failures?1:0;
}

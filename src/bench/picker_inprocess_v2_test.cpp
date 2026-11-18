#include "../picker_inprocess/picker_ui_api_v2.h"
#ifdef PULSE_REMOTE_PROVIDER_TEST
#include "../shell_bridge/file_dialog_remote_provider.h"
#endif
#include <objbase.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <wincodec.h>
#include <wrl/client.h>
#include <set>
#include <mutex>

namespace {
// Snapshot requests: the remote picker HWND lives in pulse.exe, so pass the
// path as a global atom; in-process pickers accept a same-process pointer.
HANDLE SnapshotRequest(const std::wstring& path) {
#ifdef PULSE_REMOTE_PROVIDER_TEST
    return reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(GlobalAddAtomW(path.c_str())));
#else
    return const_cast<wchar_t*>(path.c_str());
#endif
}
struct DpiApi {
    using GetThread = DPI_AWARENESS_CONTEXT (WINAPI*)();
    using SetThread = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);
    using Equal = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
    GetThread get_thread = nullptr;
    SetThread set_thread = nullptr;
    Equal equal = nullptr;
    DpiApi() {
        const HMODULE user = GetModuleHandleW(L"user32.dll");
        if (!user) return;
        get_thread = reinterpret_cast<GetThread>(GetProcAddress(user, "GetThreadDpiAwarenessContext"));
        set_thread = reinterpret_cast<SetThread>(GetProcAddress(user, "SetThreadDpiAwarenessContext"));
        equal = reinterpret_cast<Equal>(GetProcAddress(user, "AreDpiAwarenessContextsEqual"));
    }
    bool Available() const { return get_thread && set_thread && equal; }
};
DpiApi dpi;
unsigned checks = 0, failures = 0;
constexpr HRESULT kClose = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x432);
void Check(bool ok, const char* name) {
    ++checks; if (!ok) ++failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
}
PulsePickerUiV2Text Text(const std::wstring& value) {
    return {value.c_str(), static_cast<uint32_t>(value.size())};
}
struct OwnerReentry {
    PulsePickerUiV2Api* api = nullptr;
    PulsePickerUiV2Session session = 0;
    const PulsePickerUiV2Config* config = nullptr;
    const PulsePickerUiV2Callbacks* callbacks = nullptr;
    PulsePickerUiV2Result nested{};
    bool armed = false, fired = false;
    HRESULT shutdown = E_PENDING, show = E_PENDING;
};
LRESULT CALLBACK OwnerProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* state = reinterpret_cast<OwnerReentry*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_ENABLE && !wp && state && state->armed && !state->fired) {
        state->fired = true;
        state->shutdown = state->api->Shutdown(state->session);
        // If Shutdown wrongly succeeded, do not issue another call on its stale handle.
        if (state->shutdown == HRESULT_FROM_WIN32(ERROR_BUSY))
            state->show = state->api->Show(state->session, state->config, state->callbacks, &state->nested);
    }
    return DefWindowProcW(window, message, wp, lp);
}
struct ThreadOwner {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HWND window = nullptr;
    DWORD thread_id = 0;
    std::thread worker;
    void Start(const wchar_t* class_name, HDESK desktop) {
        if (!ready) return;
        worker = std::thread([this, class_name, desktop] {
            thread_id = GetCurrentThreadId();
            if (SetThreadDesktop(desktop)) {
                window = CreateWindowExW(0, class_name, L"Owned cross-thread proof owner", WS_OVERLAPPED,
                    0, 0, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            }
            SetEvent(ready);
            if (window) {
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                DestroyWindow(window);
            }
        });
        Check(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0, "cross-thread owner initialized within budget");
    }
    void Stop() {
        if (worker.joinable()) { PostThreadMessageW(thread_id, WM_QUIT, 0, 0); worker.join(); }
    }
    ~ThreadOwner() { Stop(); if (ready) CloseHandle(ready); }
};
// Observe the real HWND visibility transition and the compositor's pixel
// snapshot, rather than relying on an internal "first frame ready" flag.
struct StartupVisibility {
    inline static thread_local StartupVisibility* active = nullptr;
    HHOOK hook = nullptr;
    HWND window = nullptr;
    std::wstring snapshot;
    RECT expected{}, first{};
    bool requested = false, shown = false, moved_visible = false, pixels_before_show = false;
    bool folder_trigger = false;
    unsigned visible_positions = 0;
    ~StartupVisibility() {
        if (hook) UnhookWindowsHookEx(hook); if (active == this) active = nullptr;
#ifdef PULSE_REMOTE_PROVIDER_TEST
        if (observer.joinable()) { PostThreadMessageW(observer_thread, WM_QUIT, 0, 0); observer.join(); }
        if (remote == this) remote = nullptr;
#endif
    }
    bool Install(const std::wstring& path) {
        snapshot = path;
        active = this;
#ifdef PULSE_REMOTE_PROVIDER_TEST
        return InstallRemote();
#else
        hook = SetWindowsHookExW(WH_CALLWNDPROC, Observe, nullptr, GetCurrentThreadId());
        return hook != nullptr;
#endif
    }
    static bool SameRect(const RECT& left, const RECT& right) {
        return left.left == right.left && left.top == right.top && left.right == right.right && left.bottom == right.bottom;
    }
    void Verify() {
        Observed([this] { return shown; }, 2000);
#ifdef PULSE_REMOTE_PROVIDER_TEST
        std::lock_guard guard(lock);
#endif
        Check(requested && shown && visible_positions > 0, "record actual initial host request and first visible window position");
        Check(pixels_before_show, "first visible position already has covered translucent chrome and opaque rendered content");
        Check(SameRect(first, expected), "first visible rectangle equals host final requested placement");
        Check(!moved_visible, "picker never moves from an earlier visible startup position during initialization");
        std::printf("STARTUP_SCREENSHOT=%ls\n", snapshot.c_str());
    }
    bool HasRenderedPixels() const {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateDecoderFromFilename(snapshot.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
        UINT width = 0, height = 0;
        if (FAILED(converter->GetSize(&width, &height)) || width < 200 || height < 100 || width > 4096 || height > 4096) return false;
        std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
        if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) return false;
        size_t covered = 0, samples = 0, body_opaque = 0, body_samples = 0;
        std::set<unsigned> colors;
        // BeginSurface intentionally tints Acrylic at alpha 0.42 and Mica at
        // 0.76/0.82. Require full surface coverage at >=0.40 (quantization
        // margin), while the central file pane must still be truly opaque.
        // The fixed fixture's center-right region excludes sidebar and chrome.
        for (UINT y = 12; y + 12 < height; y += 3) for (UINT x = 12; x + 12 < width; x += 3) {
            const BYTE* pixel = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
            ++samples;
            if (pixel[3] >= 102) ++covered;
            if (x >= width * 4 / 10 && x < width * 9 / 10 && y >= height * 3 / 10 && y < height * 7 / 10) {
                ++body_samples;
                if (pixel[3] >= 250) ++body_opaque;
            }
            colors.insert(static_cast<unsigned>(pixel[0]) | (static_cast<unsigned>(pixel[1]) << 8) | (static_cast<unsigned>(pixel[2]) << 16));
        }
        std::printf("STARTUP pixels=%ux%u covered=%zu/%zu body-opaque=%zu/%zu colors=%zu\n",
            width, height, covered, samples, body_opaque, body_samples, colors.size());
        return samples && body_samples && covered * 100 >= samples * 98 &&
            body_opaque * 100 >= body_samples * 98 && colors.size() >= 16;
    }
    static LRESULT CALLBACK Observe(int code, WPARAM wp, LPARAM lp) {
        auto* state = active;
        if (code == HC_ACTION && state) {
            const auto* message = reinterpret_cast<const CWPSTRUCT*>(lp);
            if (!state->window) {
                wchar_t name[128]{};
                if (GetClassNameW(message->hwnd, name, 128) && wcscmp(name, L"PulseFolderPickerWindow") == 0)
                    state->window = message->hwnd;
            }
            if (state->window == message->hwnd && message->message == WM_WINDOWPOSCHANGED && IsWindowVisible(message->hwnd))
                state->Record(message->hwnd);
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    void Record(HWND hwnd) {
        const auto prior = dpi.set_thread(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        RECT rectangle{}; GetWindowRect(hwnd, &rectangle);
        if (prior) dpi.set_thread(prior);
        ++visible_positions;
        if (!shown) {
            shown = true;
            first = rectangle;
            pixels_before_show = HasRenderedPixels();
            std::printf("STARTUP first-visible=%ld,%ld,%ld,%ld snapshot=%d\n", rectangle.left, rectangle.top,
                rectangle.right, rectangle.bottom, pixels_before_show);
        } else if (!SameRect(first, rectangle)) moved_visible = true;
    }
#ifdef PULSE_REMOTE_PROVIDER_TEST
    // The remote picker HWND lives in pulse.exe, where a WH_CALLWNDPROC hook on
    // this thread cannot see it. Out-of-context WinEvents arrive in order on a
    // dedicated thread, so creation, first visibility and every later move are
    // still observed (rectangles are read when each event is delivered).
    inline static StartupVisibility* remote = nullptr;
    std::mutex lock;
    std::thread observer;
    DWORD observer_thread = 0;
    static void CALLBACK WinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
        auto* state = remote;
        if (!state || !hwnd || object != OBJID_WINDOW || child != CHILDID_SELF) return;
        std::lock_guard guard(state->lock);
        if (!state->window) {
            wchar_t name[128]{};
            if (GetClassNameW(hwnd, name, 128) && wcscmp(name, L"PulseFolderPickerWindow") == 0) state->window = hwnd;
        }
        if (state->window == hwnd && (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_LOCATIONCHANGE) && IsWindowVisible(hwnd))
            state->Record(hwnd);
    }
    bool InstallRemote() {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready) return false;
        remote = this;
        std::atomic<bool> installed{false};
        observer = std::thread([this, ready, &installed] {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            MSG message{};
            PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
            const HWINEVENTHOOK hook_handle = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, WinEvent,
                0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            observer_thread = GetCurrentThreadId();
            installed = hook_handle != nullptr;
            SetEvent(ready);
            while (hook_handle && GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
            if (hook_handle) UnhookWinEvent(hook_handle);
            if (SUCCEEDED(com)) CoUninitialize();
        });
        WaitForSingleObject(ready, INFINITE);
        CloseHandle(ready);
        return installed;
    }
#endif
    // Events reach the remote observer asynchronously; give them a bounded
    // moment before reading what was observed.
    template <class Done> bool Observed(Done done, DWORD timeout_ms) {
#ifdef PULSE_REMOTE_PROVIDER_TEST
        const auto deadline = GetTickCount64() + timeout_ms;
        for (;;) {
            { std::lock_guard guard(lock); if (done()) return true; }
            if (GetTickCount64() >= deadline) return false;
            Sleep(5);
        }
#else
        (void)timeout_ms; return done();
#endif
    }
};
struct State {
    PulsePickerUiV2Api* api = nullptr;
    PulsePickerUiV2Session session = 0;
    DWORD thread = 0;
    HWND owner = nullptr, window = nullptr;
    DPI_AWARENESS_CONTEXT caller_dpi = nullptr;
    HRESULT requested = kClose;
    bool property_edit = false, property_mutate = false, property_scroll = false;
    unsigned property_events = 0;
    std::wstring navigation_original;
    bool callback_navigation = false; unsigned navigation_completions = 0;
    bool external_navigation = false, external_veto = false;
    bool places = false, navigated = false;
    std::atomic<bool> place_initial_loaded{false}, place_click_started{false};
    std::wstring place_path;
    bool prominent = false;
    const PulsePickerUiV2Control* prominent_control = nullptr;
    const PulsePickerUiV2Control* prominent_parent = nullptr;
    bool separators = false;
    bool query = false, ready = false, custom = false, inspect = false, window_cancel = false;
    int escape_case = 0;
    bool escape_watchdog = false;
    StartupVisibility* startup = nullptr;
    bool mask_sequence = false;
    std::wstring mask_folder;
    std::atomic<unsigned> mask_type{99}, mask_results{0}, callbacks_active{0};
    std::vector<unsigned> mask_type_events;
    int startup_terminal = 0;
    unsigned startup_changing = 0, startup_changed = 0;
    bool folder_single = false;
    bool initial_empty = false, labels = false, labels_empty = false, multi = false, multi_close = false, open_missing = false, combo = false;
    bool list_ten = false;
    std::wstring title, screenshot;
    std::thread driver;
    unsigned item_events=0,file_ok_events=0;
    bool driver_ok=false;
    bool values=false;
    PulsePickerUiV2Control* value_controls=nullptr;
    unsigned check_events=0,edit_events=0;
    bool menus=false;
    unsigned activations=0,dropdown_events=0;
    std::atomic<bool> menu_selected{false};
    uint64_t initial_type_generation = 0;
    unsigned button_clicks = 0;
    uint64_t show = 0;
};
State* external_state = nullptr;
void CALLBACK NavigateTimer(HWND window, UINT, UINT_PTR timer, DWORD) {
    KillTimer(window, timer);
    auto& state = *external_state;
    auto snapshot = [&]() {
        PulsePickerUiV2Snapshot value{}; value.size = sizeof(value);
        wchar_t folder[32768]{}, name[32768]{}, selection[32768]{}, candidate[32768]{};
        value.folder={folder,32768,0}; value.filename={name,32768,0};
        value.selection={selection,32768,0}; value.candidate={candidate,32768,0};
        Check(state.api->Query(state.session,state.show,0,&value)==S_OK,"external navigation snapshot query");
        return std::make_pair(std::wstring(folder),std::wstring(selection));
    };
    const auto before=snapshot();
    state.external_veto=true;
    Check(state.api->Navigate(state.session,state.show,Text(state.place_path))==S_FALSE,"external navigation veto returned");
    Check(snapshot()==before,"veto preserves folder and selection");
    state.external_veto=false;
    const auto missing=state.place_path+L"-missing";
    Check(FAILED(state.api->Navigate(state.session,state.show,Text(missing))),"missing external destination fails");
    Check(snapshot()==before,"missing destination preserves folder and selection");
    Check(state.api->Navigate(state.session,state.show,Text(state.place_path))==S_OK,"external navigation commits valid destination");
    Check(snapshot().first==state.place_path,"external navigation publishes exact destination");
    state.api->Close(state.session,state.show,state.requested);
}
HRESULT ValueInputUi(State& state,const PulsePickerUiV2Event* event) {
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED) {++state.item_events;Check(event->control_id==206 && event->item_id==207,"radio selection skips disabled option");return S_OK;}
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_CHECK_TOGGLED) {++state.check_events;Check(event->control_id==210 && event->item_id==1,"checkbox Space publishes checked state");return S_OK;}
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_EDIT_CHANGED) {
        ++state.edit_events;const std::wstring text(event->control_text.data,event->control_text.units);
        Check(event->control_id==213,"painted edit exact custom id");
        if(text==L"xy") {
            state.value_controls[2].state=0;
            state.value_controls[2].text={text.data(),static_cast<uint32_t>(text.size())};
            Check(state.api->UpdateControl(state.session,event->show_id,&state.value_controls[2])==S_OK && !IsWindowVisible(GetDlgItem(event->window,2002)),"live edit hide removes input child");
            const HRESULT closed=state.api->Close(state.session,event->show_id,state.requested);
            Check(closed==S_OK,"Close succeeds from painted edit callback");
            std::printf("VALUE_CLOSE=%08lX\n",static_cast<unsigned long>(closed));std::fflush(stdout);
        }return S_OK;
    }
    if(event->kind!=PULSE_PICKER_UI_V2_EVENT_READY)return S_OK;
    state.ready=true;state.show=event->show_id;state.window=event->window;
    HWND input=GetDlgItem(event->window,2002);
    Check(input && !IsWindowVisible(input),"initially hidden edit exists without display");
    Check(input && GetPropW(input,L"Pulse.PaintedPlainEdit") && !GetPropW(input,L"Pulse.NativeEditFallback"),"custom input is authoritative Pulse painted child without native fallback");
    for(unsigned i=0;i<3;++i){state.value_controls[i].state=3;Check(state.api->UpdateControl(state.session,event->show_id,&state.value_controls[i])==S_OK,"reveal hidden value control");}
    state.driver=std::thread([&state,window=event->window] {
        if(!SetThreadDesktop(GetThreadDesktop(state.thread)))return;
        HWND filename=GetDlgItem(window,102);DWORD_PTR ignored=0;
        for(unsigned i=0;i<1000 && filename && !IsWindowEnabled(filename);++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto key=[&](HWND target,UINT msg,WPARAM value,LPARAM data=0){return SendMessageTimeoutW(target,msg,value,data,SMTO_ABORTIFHUNG,2000,&ignored)!=0;};
        if(!filename || !key(window,WM_SYSKEYDOWN,'N') || !key(filename,WM_KEYDOWN,VK_TAB) || !key(window,WM_KEYDOWN,VK_TAB))return;
        if(!key(window,WM_KEYDOWN,VK_SPACE) || !key(window,WM_KEYDOWN,VK_TAB) || !key(window,WM_KEYDOWN,VK_SPACE) || !key(window,WM_KEYDOWN,VK_TAB))return;
        HWND edit=GetDlgItem(window,2002);GUITHREADINFO info{sizeof(info)};
        if(!edit || !GetGUIThreadInfo(state.thread,&info) || info.hwndFocus!=edit || !IsWindowVisible(edit))return;
        state.driver_ok=key(edit,EM_SETSEL,0,-1) && key(edit,WM_CHAR,'x') && key(edit,WM_CHAR,'y');
        std::printf("VALUE_DRIVER_END=%u\n",state.driver_ok?1u:0u);std::fflush(stdout);
    });return S_OK;
}
HRESULT WINAPI Callback(void* opaque, const PulsePickerUiV2Event* event) {
    auto& state = *static_cast<State*>(opaque);
    struct Active { std::atomic<unsigned>& count; explicit Active(std::atomic<unsigned>& c) : count(c) { ++count; } ~Active() { --count; } } active{state.callbacks_active};
    try {
        if (state.callback_navigation) {
            auto folder = [&]() {
                PulsePickerUiV2Snapshot value{}; value.size=sizeof(value);
                wchar_t a[32768]{},b[32768]{},c[32768]{},d[32768]{};
                value.folder={a,32768,0};value.filename={b,32768,0};value.selection={c,32768,0};value.candidate={d,32768,0};
                Check(state.api->Query(state.session,event->show_id,0,&value)==S_OK,"queued navigation snapshot query");
                return std::wstring(a);
            };
            if(event->kind==PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGING && event->proposed_folder.data &&
                std::wstring(event->proposed_folder.data,event->proposed_folder.units)==state.place_path+L"-veto") return S_FALSE;

            if (event->kind==PULSE_PICKER_UI_V2_EVENT_READY) {
                state.ready=true;state.show=event->show_id;state.window=event->window;
                state.navigation_original=folder();
                const auto veto=state.place_path+L"-veto";
                Check(state.api->Navigate(state.session,state.show,Text(veto))==E_PENDING,"callback veto destination queued");
                const auto missing=state.place_path+L"-missing";
                Check(state.api->Navigate(state.session,state.show,Text(missing))==E_PENDING,"callback missing navigation queued");
                Check(state.api->Navigate(state.session,state.show,Text(state.place_path))==E_PENDING,"callback valid navigation queued");
                Check(folder()==state.navigation_original,"callback returns before folder changes");
            }
            if (event->kind==PULSE_PICKER_UI_V2_EVENT_NAVIGATION_COMPLETE) {
                ++state.navigation_completions;
                if(state.navigation_completions==1) {
                    Check(event->navigation_result==S_FALSE,"queued veto completes S_FALSE");
                    Check(folder()==state.navigation_original,"queued veto retains folder");
                } else if(state.navigation_completions==2) {
                    Check(FAILED(event->navigation_result),"queued missing destination completes failure");
                    Check(folder()==state.navigation_original,"queued missing destination retains folder");
                }
                else {
                    Check(event->navigation_result==S_OK,"queued valid destination completes success");
                    Check(folder()==state.place_path,"queued success publishes target folder");
                    state.api->Close(state.session,state.show,state.requested);
                }
            }
            return S_OK;
        }
        if (state.external_navigation) {
            if (event->kind==PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGING) return state.external_veto ? S_FALSE : S_OK;
            if (event->kind==PULSE_PICKER_UI_V2_EVENT_READY) {
                state.ready=true;state.show=event->show_id;state.window=event->window;
                external_state=&state;
                Check(SetTimer(event->window,991,1000,NavigateTimer)!=0,"schedule outside-callback navigation");
            }
            return S_OK;
        }

        Check(event && event->size == sizeof(*event), "callback event header");
        if (!event || event->size != sizeof(*event)) return E_INVALIDARG;
        if (state.startup_terminal) {
            state.show = event->show_id; state.window = event->window;
            if (event->kind == PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGING) {
                ++state.startup_changing;
                // The isolated directory is exclusively held. Its failed
                // admission must take the existing fallback branch; reject
                // that fallback so the terminal initialization error is tested.
                if (state.startup_terminal == 3 && state.startup_changing == 2) return E_ACCESSDENIED;
            }
            if (event->kind == PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGED) {
                ++state.startup_changed;
                if (state.startup_terminal == 1) {
                    Check(!IsWindowVisible(event->window) && !state.ready, "first verified FolderChanged can close before any visible UI");
                    Check(state.api->Close(state.session, event->show_id, kClose) == S_OK, "Close succeeds inside initial FolderChanged");
                    return S_OK;
                }
            }
            if (state.startup_terminal == 2 && event->kind == PULSE_PICKER_UI_V2_EVENT_TYPE_CHANGED)
                Check(PostMessageW(event->window, WM_CLOSE, 0, 0) != FALSE, "queue cancellation while real picker is still initializing");
            if (event->kind == PULSE_PICKER_UI_V2_EVENT_READY) { state.ready = true; return S_OK; }
        }
        if (state.mask_sequence && (event->kind == PULSE_PICKER_UI_V2_EVENT_TYPE_CHANGED || event->kind == PULSE_PICKER_UI_V2_EVENT_FILE_OK)) {
            PulsePickerUiV2Snapshot snapshot{}; snapshot.size=sizeof(snapshot);
            std::vector<wchar_t> folder(32768),name(32768),selection(32768),candidate(32768);
            snapshot.folder={folder.data(),32768,0};snapshot.filename={name.data(),32768,0};
            snapshot.selection={selection.data(),32768,0};snapshot.candidate={candidate.data(),32768,0};
            const HRESULT queried=state.api->Query(state.session,event->show_id,event->generation,&snapshot);
            Check(queried==S_OK,"mask sequence event exposes exact-generation snapshot");
            if(event->kind==PULSE_PICKER_UI_V2_EVENT_TYPE_CHANGED) {
                state.mask_type_events.push_back(snapshot.filter_index);
                state.mask_type=snapshot.filter_index;
                std::printf("MASK_TYPE index=%u\n",snapshot.filter_index);
                return S_OK;
            }
            const unsigned step=state.mask_results.load();
            const wchar_t* expected[]={L"literal.md",L"wildcard.txt",L"all.txt"};
            Check(step<3 && snapshot.filter_index==step+1 &&
                std::wstring(candidate.data())==(std::filesystem::path(state.mask_folder)/expected[step]).wstring(),
                "actual FileOk matches native literal.md then wildcard.txt then all.txt sequence");
            ++state.file_ok_events;state.mask_results=step+1;
            return step<2 ? S_FALSE : S_OK;
        }
        if(state.values)return ValueInputUi(state,event);
        Check(GetCurrentThreadId() == state.thread, "callback on Show STA");
        Check(dpi.equal(dpi.get_thread(), state.caller_dpi) != FALSE,
            "callback restores caller thread DPI context");
        if (state.property_edit && event->kind == PULSE_PICKER_UI_V2_EVENT_PROPERTY_CHANGED) {
            ++state.property_events;
            if (state.property_scroll) {
                const bool valid = event->control_id == 107 && event->property_value_count == 1 && event->property_values &&
                    std::wstring(event->property_values[0].data, event->property_values[0].units) == L"edited last scalar";
                Check(valid, "eight-field panel edits the exact final scalar after scrolling");
                SetPropW(event->window, L"Pulse.PickerTestSnapshot", SnapshotRequest(state.screenshot));
                SendMessageW(event->window, WM_PAINT, 0, 0);
                RemovePropW(event->window, L"Pulse.PickerTestSnapshot");
                std::printf("PROPERTY_SCROLL_SCREENSHOT=%ls\n", state.screenshot.c_str()); std::fflush(stdout);
                Check(state.api->Close(state.session, event->show_id, state.requested) == S_OK, "eight-field property edit cancels through Close");
                return S_OK;
            }
            if (state.property_mutate) {
                const uint32_t expected = state.property_events == 1 ? 1u : 2u;
                const bool valid = event->control_id == 42 && event->property_value_count == expected && event->property_values &&
                    std::wstring(event->property_values[0].data, event->property_values[0].units) == L"literal;semicolon" &&
                    (expected == 1 || event->property_values[1].units == 0);
                Check(valid, expected == 1 ? "remove vector item retains exact surviving semicolon value" : "add vector item appends explicit empty value without splitting");
                if (!valid || state.property_events == 2) state.api->Close(state.session, event->show_id, state.requested);
                return S_OK;
            }
            const bool count = event->control_id == 42 && event->property_value_count == 2 && event->property_values;
            Check(count && std::wstring(event->property_values[0].data, event->property_values[0].units) == L"edited metadata" &&
                std::wstring(event->property_values[1].data, event->property_values[1].units) == L"literal;semicolon", "property edit preserves vector boundaries and untouched semicolon value");
            const HRESULT closed = state.api->Close(state.session, event->show_id, state.requested);
            std::printf("PROPERTY_CLOSE=%08lX\n", static_cast<unsigned long>(closed)); std::fflush(stdout);
            Check(closed == S_OK, "property callback Close returns success");
            return S_OK;
        }
        if (state.places && event->kind == PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGED) {
            PulsePickerUiV2Snapshot snapshot{}; snapshot.size = sizeof(snapshot);
            std::vector<wchar_t> folder(32768), name(32768), selection(32768), candidate(32768);
            snapshot.folder = {folder.data(), 32768, 0}; snapshot.filename = {name.data(), 32768, 0};
            snapshot.selection = {selection.data(), 32768, 0}; snapshot.candidate = {candidate.data(), 32768, 0};
            const HRESULT queried = state.api->Query(state.session, event->show_id, event->generation, &snapshot);
            std::printf("PLACES folder-changed query=%08lX generation=%llu click=%d actual=%ls expected=%ls\n",
                static_cast<unsigned long>(queried), event->generation, state.place_click_started.load() ? 1 : 0,
                folder.data(), state.place_path.c_str());
            std::fflush(stdout);
            if (!state.place_click_started.load()) {
                Check(queried == S_OK, "initial place fixture commit publishes coherent folder");
                state.place_initial_loaded = true;
                return S_OK;
            }
            state.navigated = queried == S_OK && state.place_path == folder.data();
            Check(state.navigated, "sidebar place click commits exact requested navigation destination");
            if (state.navigated && !state.screenshot.empty()) {
                SetPropW(event->window, L"Pulse.PickerTestSnapshot", SnapshotRequest(state.screenshot));
                SendMessageW(event->window, WM_PAINT, 0, 0);
                RemovePropW(event->window, L"Pulse.PickerTestSnapshot");
                std::printf("COMPUTER_SCREENSHOT=%ls\n", state.screenshot.c_str());
                std::fflush(stdout);
            }
            state.api->Close(state.session, event->show_id, state.requested);
            return S_OK;
        }
        if(state.multi && event->kind==PULSE_PICKER_UI_V2_EVENT_FILE_OK) {
            ++state.file_ok_events;
            PulsePickerUiV2Snapshot snapshot{};snapshot.size=sizeof(snapshot);
            Check(state.api->Query(state.session,event->show_id,event->generation,&snapshot)==HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),"multi candidate sizing");
            std::vector<wchar_t> folder(snapshot.folder.units+1),filename(snapshot.filename.units+1),selection(snapshot.selection.units+1),candidate(snapshot.candidate.units+1),selections(snapshot.selection_paths.units+1),candidates(snapshot.candidate_paths.units+1);
            snapshot.folder={folder.data(),static_cast<uint32_t>(folder.size()),0};snapshot.filename={filename.data(),static_cast<uint32_t>(filename.size()),0};
            snapshot.selection={selection.data(),static_cast<uint32_t>(selection.size()),0};snapshot.candidate={candidate.data(),static_cast<uint32_t>(candidate.size()),0};
            snapshot.selection_paths={selections.data(),static_cast<uint32_t>(selections.size()),0};snapshot.candidate_paths={candidates.data(),static_cast<uint32_t>(candidates.size()),0};
            Check(state.api->Query(state.session,event->show_id,event->generation,&snapshot)==S_OK && snapshot.candidate_count==(state.list_ten ? 10u : state.folder_single ? 1u : 2u) &&
                std::wstring(candidate.data())==std::wstring(candidates.data()) && candidates[snapshot.candidate_paths.units]==0,"multi FileOk exposes complete same-generation candidates and first-item consistency");
            if(state.multi_close) Check(state.api->Close(state.session,event->show_id,state.requested)==S_OK,"multi candidate Close wins before committing any output array");
            return S_OK;
        }
        if(state.menus) {
            if(event->kind==PULSE_PICKER_UI_V2_EVENT_CONTROL_ACTIVATING) { ++state.activations; return S_OK; }
            if(event->kind==PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED) {
                ++state.item_events;
                Check(event->control_id==42 && event->item_id==0 && state.activations==1 && state.file_ok_events==0,"real menu selection follows activation and does not submit");state.menu_selected.store(true);return S_OK;
            }
            if(event->kind==PULSE_PICKER_UI_V2_EVENT_OPEN_DROPDOWN_SELECTED) {
                ++state.dropdown_events;
                Check(event->control_id==50 && event->item_id==0 && state.activations==2 && state.item_events==1,"real split selection synchronizes without public item event");return S_OK;
            }
            if(event->kind==PULSE_PICKER_UI_V2_EVENT_FILE_OK) {
                ++state.file_ok_events;
                Check(state.dropdown_events==1 && state.item_events==1,"split selection precedes actual FileOk");
                Check(state.api->Close(state.session,event->show_id,state.requested)==S_OK,"close owned menu fixture from FileOk");return S_OK;
            }
        }
        if(state.combo && event->kind==PULSE_PICKER_UI_V2_EVENT_ITEM_SELECTED) {
            ++state.item_events;
            Check(event->control_id==42 && event->item_id==0,"real combo event preserves control ID and selected item zero");
            Check(state.api->Close(state.session,event->show_id,state.requested)==S_OK,"Close allowed from actual combo event");
            return E_FAIL; // Notifications cannot veto or replace the explicit Close result.
        }
        if (state.startup && !state.startup->requested && event->kind == (state.startup->folder_trigger
            ? PULSE_PICKER_UI_V2_EVENT_FOLDER_CHANGED : PULSE_PICKER_UI_V2_EVENT_TYPE_CHANGED)) {
            auto& probe = *state.startup;
            probe.requested = true;
            if (probe.folder_trigger) Check(!state.ready, "initial FolderChanged host initialization precedes READY");
            HWND actual = nullptr, observed = nullptr;
            bool shown_before = true;
            probe.Observed([&] { observed = probe.window; return observed != nullptr; }, 2000);
            probe.Observed([&] { shown_before = probe.shown; return true; }, 0);
            Check(state.api->GetWindow(state.session, event->show_id, &actual) == S_OK && actual == event->window &&
                actual == observed, "initial host callback obtains the observed real picker HWND");
            Check(!IsWindowVisible(actual) && !shown_before, "picker is hidden when initial host callback begins");
            Check(SetPropW(actual, L"Pulse.PickerTestSnapshot", SnapshotRequest(probe.snapshot)) != FALSE,
                "request actual compositor snapshot before first visibility");
            const auto previous = dpi.set_thread(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            RECT initial{}; GetWindowRect(actual, &initial);
            MONITORINFO monitor{sizeof(monitor)};
            Check(GetMonitorInfoW(MonitorFromWindow(actual, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE, "query host placement monitor");
            probe.expected = {monitor.rcWork.left + 31, monitor.rcWork.top + 47,
                monitor.rcWork.left + 31 + initial.right - initial.left, monitor.rcWork.top + 47 + initial.bottom - initial.top};
            ShowWindow(actual, SW_SHOWNOACTIVATE);
            Check(!IsWindowVisible(actual), "host early ShowWindow cannot expose an unpainted shell");
            Check(SetWindowPos(actual, nullptr, probe.expected.left, probe.expected.top,
                probe.expected.right - probe.expected.left, probe.expected.bottom - probe.expected.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE, "host requests final placement and visibility during initialization");
            Check(!IsWindowVisible(actual), "host SetWindowPos show request stays hidden until rendering completes");
            if (previous) dpi.set_thread(previous);
            if (state.ready) {
                // Old components publish this callback only after READY. Keep
                // the test alive until it actually exercises that late move.
                probe.Verify();
                Check(state.api->Close(state.session, event->show_id, state.requested) == S_OK,
                    "close after late FolderChanged startup regression evidence");
                return S_OK;
            }
        }
        if (state.initial_empty && event->kind == PULSE_PICKER_UI_V2_EVENT_TYPE_CHANGED && !state.initial_type_generation) {
            Check(event->generation != 0, "empty initial filename publishes nonzero TypeChanged generation");
            state.initial_type_generation = event->generation;
            wchar_t folder[1]{L'X'}, filename[1]{L'X'}, selection[1]{L'X'}, candidate[1]{L'X'};
            PulsePickerUiV2Snapshot snapshot{}; snapshot.size = sizeof(snapshot);
            snapshot.folder = {folder, 1, 0}; snapshot.filename = {filename, 1, 0};
            snapshot.selection = {selection, 1, 0}; snapshot.candidate = {candidate, 1, 0};
            const HRESULT queried = state.api->Query(state.session, event->show_id, event->generation, &snapshot);
            Check(queried == S_OK && snapshot.generation == event->generation && snapshot.show_id == event->show_id &&
                snapshot.window == event->window, "initial TypeChanged and Query share the same published snapshot");
            Check(!snapshot.folder.units && !snapshot.filename.units && !snapshot.selection.units && !snapshot.candidate.units &&
                !folder[0] && !filename[0] && !selection[0] && !candidate[0], "initial empty snapshot has no invented directory or candidate");
            HWND actual = nullptr;
            Check(state.api->GetWindow(state.session, event->show_id, &actual) == S_OK && actual == event->window && IsWindow(actual),
                "initial TypeChanged exposes actual HWND before READY");
            Check(state.api->Query(state.session, event->show_id, event->generation + 1, &snapshot) == HRESULT_FROM_WIN32(ERROR_RETRY),
                "initial Query still rejects mismatched generation");
        }
        if (event->kind == PULSE_PICKER_UI_V2_EVENT_BUTTON_CLICKED) {
            ++state.button_clicks;
            Check(state.custom && event->control_id == 42 && event->window == state.window, "real button event exact control and HWND");
            Check(state.api->Close(state.session, event->show_id, state.requested) == S_OK, "button callback closes exact Show");
            return E_FAIL; // COM control callback HRESULT is ignored; explicit Close still wins.
        }
        if (event->kind != PULSE_PICKER_UI_V2_EVENT_READY) return S_OK;
        Check(!IsWindowEnabled(state.owner), "owner disabled during actual UI READY");
        Check(!state.ready, "exactly one READY"); state.ready = true; state.show = event->show_id;
        state.window = event->window;
        DWORD pid = 0;
        const DWORD tid = GetWindowThreadProcessId(event->window, &pid);
#ifdef PULSE_REMOTE_PROVIDER_TEST
        Check(event->window && IsWindowVisible(event->window) && pid != GetCurrentProcessId() && pid && tid,
            "READY carries visible remote process HWND");
#else
        Check(event->window && IsWindowVisible(event->window) && pid == GetCurrentProcessId() && tid == state.thread,
            "READY carries visible own process and STA HWND");
#endif
        HWND actual = nullptr;
        const HRESULT got = state.api->GetWindow(state.session, event->show_id, &actual);
        Check(got == S_OK && actual == event->window, "callback GetWindow returns actual HWND");
        if (state.startup) {
            auto& probe = *state.startup;
            if (probe.folder_trigger && !probe.requested) return S_OK;
            probe.Verify();
        }
        if (state.initial_empty) {
            Check(state.initial_type_generation != 0 && event->generation > state.initial_type_generation,
                "navigation advances rather than resets initial generation");
            PulsePickerUiV2Snapshot stale{}; stale.size = sizeof(stale);
            Check(state.api->Query(state.session, event->show_id, state.initial_type_generation, &stale) == HRESULT_FROM_WIN32(ERROR_RETRY),
                "READY rejects old initial TypeChanged generation");
        }
        if (state.query) {
            PulsePickerUiV2Snapshot size{}; size.size = sizeof(size);
            HRESULT hr = state.api->Query(state.session, event->show_id, 0, &size);
            std::printf("QUERY sizing=%08lX generation=%llu folder_units=%u filename_units=%u\n",
                static_cast<unsigned long>(hr), size.generation, size.folder.units, size.filename.units);
            Check(hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && size.folder.units > 0,
                "query sizing reports coherent required lengths");
            if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && size.folder.units > 0 &&
                size.folder.units <= PULSE_PICKER_UI_V2_MAX_UNITS && size.filename.units <= PULSE_PICKER_UI_V2_MAX_UNITS &&
                size.selection.units <= PULSE_PICKER_UI_V2_MAX_UNITS && size.candidate.units <= PULSE_PICKER_UI_V2_MAX_UNITS) {
                wchar_t folder_short[2]{L'F', L'X'}, name_short[2]{L'N', L'X'}, selection_short[2]{L'S', L'X'}, candidate_short[2]{L'C', L'X'};
                PulsePickerUiV2Snapshot short_query{}; short_query.size = sizeof(short_query);
                short_query.folder = {folder_short, 1, 0}; short_query.filename = {name_short, 1, 0};
                short_query.selection = {selection_short, 1, 0}; short_query.candidate = {candidate_short, 1, 0};
                hr = state.api->Query(state.session, event->show_id, size.generation, &short_query);
                Check(hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && folder_short[0] == L'F' && folder_short[1] == L'X' &&
                    name_short[0] == L'N' && name_short[1] == L'X' && selection_short[0] == L'S' && selection_short[1] == L'X' &&
                    candidate_short[0] == L'C' && candidate_short[1] == L'X', "short query writes no partial strings or canaries");
                std::vector<wchar_t> folder(size.folder.units + 1), name(size.filename.units + 1),
                    selection(size.selection.units + 1), candidate(size.candidate.units + 1);
                PulsePickerUiV2Snapshot full{}; full.size = sizeof(full);
                full.folder = {folder.data(), static_cast<uint32_t>(folder.size()), 0};
                full.filename = {name.data(), static_cast<uint32_t>(name.size()), 0};
                full.selection = {selection.data(), static_cast<uint32_t>(selection.size()), 0};
                full.candidate = {candidate.data(), static_cast<uint32_t>(candidate.size()), 0};
                hr = state.api->Query(state.session, event->show_id, size.generation, &full);
                Check(hr == S_OK && full.show_id == event->show_id && full.generation == size.generation && full.window == event->window,
                    "full callback query retains same show generation and HWND");
                PulsePickerUiV2Snapshot stale{}; stale.size = sizeof(stale);
                const uint64_t other_generation = size.generation == 1 ? 2 : 1;
                Check(state.api->Query(state.session, event->show_id, other_generation, &stale) == HRESULT_FROM_WIN32(ERROR_RETRY),
                    "stale generation rejected");
                Check(state.api->Query(state.session, event->show_id + 1, 0, &stale) == HRESULT_FROM_WIN32(ERROR_RETRY),
                    "stale show rejected");
            }
        }
        if (state.prominent) {
            const auto capture = [&](const wchar_t* suffix) {
                const auto path = std::filesystem::absolute(std::filesystem::path(L"bench_data") / ((state.separators ? L"picker-separators-" : L"picker-prominent-") + std::to_wstring(GetCurrentProcessId()) + suffix + L".png"));
                const auto text = path.wstring();
                SetPropW(event->window, L"Pulse.PickerTestSnapshot", SnapshotRequest(text));
                SendMessageW(event->window, WM_PAINT, 0, 0);
                RemovePropW(event->window, L"Pulse.PickerTestSnapshot");
                Check(std::filesystem::exists(path), "prominent layout screenshot captured");
                std::printf("PROMINENT_SCREENSHOT=%ls\n", text.c_str());
            };
            if (state.separators) {
                PulsePickerUiV2Control separator{}; separator.size = sizeof(separator);
                separator.kind = PULSE_PICKER_UI_V2_CONTROL_SEPARATOR; separator.id = 2027; separator.state = 0;
                Check(state.api->UpdateControl(state.session, event->show_id, &separator) == S_OK, "hide standalone separator during actual Show");
                separator.state = 3;
                Check(state.api->UpdateControl(state.session, event->show_id, &separator) == S_OK, "restore standalone separator during actual Show");
            }
            capture(L"-ordinary");
            auto group = *state.prominent_control;
            group.state |= PULSE_PICKER_UI_V2_CONTROL_PROMINENT;
            Check(state.api->UpdateControl(state.session, event->show_id, &group) == S_OK, "prominent group updated during show");
            capture(L"-wide");
            if (state.prominent_parent) {
                auto parent = *state.prominent_parent;
                parent.state = 0;
                Check(state.api->UpdateControl(state.session, event->show_id, &parent) == S_OK, "hide parent of prominent child");
                capture(L"-parent-hidden");
            }
            Check(SetWindowPos(event->window, nullptr, 0, 0, 540, 620, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "narrow prominent window resized");
            capture(L"-narrow");
        }
        if(state.labels) {
            wchar_t title[2048]{};GetWindowTextW(event->window,title,2048);
            Check(std::wstring(title)==state.title,"explicit caller title reaches actual picker HWND including empty");
            if(!state.screenshot.empty()) {
                SetPropW(event->window,L"Pulse.PickerTestSnapshot",SnapshotRequest(state.screenshot));
                SendMessageW(event->window,WM_PAINT,0,0);
                std::printf("LABEL_SCREENSHOT=%ls\n",state.screenshot.c_str());
                RemovePropW(event->window,L"Pulse.PickerTestSnapshot");
            }
        }
        if (state.property_edit) {
#ifndef PULSE_REMOTE_PROVIDER_TEST
            SetPropW(event->window, L"Pulse.PickerTestSnapshot", SnapshotRequest(state.screenshot));
            SendMessageW(event->window, WM_PAINT, 0, 0);
            RemovePropW(event->window, L"Pulse.PickerTestSnapshot");
#endif
            std::printf("PROPERTY_SCREENSHOT=%ls\n", state.screenshot.c_str()); std::fflush(stdout);
            state.driver = std::thread([&state] {
                Sleep(150);
                HWND edit = nullptr;
                auto read_text = [](HWND window, wchar_t* buffer, UINT capacity) {
                    DWORD_PTR ignored = 0;
                    return SendMessageTimeoutW(window, WM_GETTEXT, capacity, reinterpret_cast<LPARAM>(buffer), SMTO_ABORTIFHUNG, 2000, &ignored) != 0;
                };
                for (HWND child = GetWindow(state.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                    wchar_t text[128]{}; read_text(child, text, 128);
                    if (IsWindowVisible(child) && std::wstring_view(text) == L"original metadata") { edit = child; break; }
                }
                DWORD_PTR ignored = 0;
                if (state.property_scroll) {
                    if (!SetThreadDesktop(GetThreadDesktop(state.thread))) { PostMessageW(state.window, WM_CLOSE, 0, 0); return; }
                    using WindowContext = DPI_AWARENESS_CONTEXT(WINAPI*)(HWND);
                    const auto window_context = reinterpret_cast<WindowContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowDpiAwarenessContext"));
                    const auto previous = window_context ? dpi.set_thread(window_context(state.window)) : nullptr;
                    HWND last = nullptr;
                    for (HWND child = GetWindow(state.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                        wchar_t text[128]{}; read_text(child, text, 128);
                        if (std::wstring_view(text) == L"last scalar original") { last = child; break; }
                    }
                    const bool initially_hidden = last && !IsWindowVisible(last);
                    RECT first{}; if (edit) GetWindowRect(edit, &first);
                    const LPARAM wheel_point = MAKELPARAM((first.left + first.right) / 2, (first.top + first.bottom) / 2);
                    auto send = [&](HWND target, UINT message, WPARAM wp, LPARAM lp = 0) {
                        return target && SendMessageTimeoutW(target, message, wp, lp, SMTO_ABORTIFHUNG, 2000, &ignored) != 0;
                    };
                    bool scrolled = previous && edit && initially_hidden;
                    for (unsigned step = 0; scrolled && !IsWindowVisible(last) && step < 20; ++step)
                        scrolled = send(state.window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA * 3)), wheel_point);
                    RECT last_rect{}; if (last) GetClientRect(last, &last_rect);
                    const LPARAM click = MAKELPARAM(last_rect.right / 2, last_rect.bottom / 2);
                    const bool clicked = scrolled && IsWindowVisible(last) && IsWindowEnabled(last) &&
                        send(last, WM_LBUTTONDOWN, MK_LBUTTON, click) && send(last, WM_LBUTTONUP, 0, click);
                    GUITHREADINFO gui{}; gui.cbSize = sizeof(gui);
                    const bool focused = clicked && GetGUIThreadInfo(state.thread, &gui) && gui.hwndFocus == last;
                    state.driver_ok = focused && send(last, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"edited last scalar"));
                    std::printf("PROPERTY_SCROLL_DRIVER initial_hidden=%u result=%u\n", initially_hidden ? 1u : 0u, state.driver_ok ? 1u : 0u); std::fflush(stdout);
                    if (previous) dpi.set_thread(previous);
                    if (!state.driver_ok) PostMessageW(state.window, WM_CLOSE, 0, 0);
                    return;
                }
                if (state.property_mutate) {
                    using WindowContext = DPI_AWARENESS_CONTEXT(WINAPI*)(HWND);
                    const auto window_context = reinterpret_cast<WindowContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowDpiAwarenessContext"));
                    const auto previous = window_context ? dpi.set_thread(window_context(state.window)) : nullptr;
                    RECT rect{}; if (edit) GetClientRect(edit, &rect);
                    const LPARAM point = MAKELPARAM((rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2);
                    auto send = [&](HWND target, UINT message, WPARAM wp, LPARAM lp = 0) {
                        return target && SendMessageTimeoutW(target, message, wp, lp, SMTO_ABORTIFHUNG, 2000, &ignored) != 0;
                    };
                    state.driver_ok = previous && edit && send(edit, WM_LBUTTONDOWN, MK_LBUTTON, point) && send(edit, WM_LBUTTONUP, 0, point) &&
                        send(edit, WM_KEYDOWN, VK_TAB) && send(state.window, WM_KEYDOWN, VK_RETURN) &&
                        send(state.window, WM_KEYDOWN, VK_TAB) && send(state.window, WM_KEYDOWN, VK_RETURN);
                    if (previous) dpi.set_thread(previous);
                    if (!state.driver_ok) PostMessageW(state.window, WM_CLOSE, 0, 0);
                    return;
                }
                state.driver_ok = edit && SendMessageTimeoutW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"edited metadata"), SMTO_ABORTIFHUNG, 2000, &ignored) != 0;
                std::printf("PROPERTY_DRIVER_END=%u error=%lu\n", state.driver_ok ? 1u : 0u, GetLastError()); std::fflush(stdout);
                if (!state.driver_ok) PostMessageW(state.window, WM_CLOSE, 0, 0);
            });
            return S_OK;
        }
        if (state.places) {
            state.driver = std::thread([&state] {
                const ULONGLONG deadline = GetTickCount64() + 5000;
                while (!state.place_initial_loaded.load() && GetTickCount64() < deadline) Sleep(10);
                if (!state.place_initial_loaded.load()) { PostMessageW(state.window, WM_CLOSE, 0, 0); return; }
                using GetWindowContext = DPI_AWARENESS_CONTEXT(WINAPI*)(HWND);
                const auto get_context = reinterpret_cast<GetWindowContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowDpiAwarenessContext"));
                const auto previous = get_context ? dpi.set_thread(get_context(state.window)) : nullptr;
                struct RestoreDpi { DPI_AWARENESS_CONTEXT previous; ~RestoreDpi() { if (previous) dpi.set_thread(previous); } } restore{previous};
                if (!previous) { PostMessageW(state.window, WM_CLOSE, 0, 0); return; }
                using GetDpi = UINT(WINAPI*)(HWND);
                const auto get_dpi = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
                const UINT dpi_value = get_dpi ? get_dpi(state.window) : 96;
                // First quick-access row: title 40 + margin 4 + pad 8 + header 40 + half row 16 DIPs.
                const LPARAM point = MAKELPARAM(MulDiv(80, dpi_value, 96), MulDiv(108, dpi_value, 96));
                DWORD_PTR ignored = 0;
                state.place_click_started = true;
                std::printf("PLACES click client=(%d,%d) dpi=%u\n", MulDiv(80, dpi_value, 96), MulDiv(108, dpi_value, 96), dpi_value);
                std::fflush(stdout);
                state.driver_ok = SendMessageTimeoutW(state.window, WM_LBUTTONDOWN, MK_LBUTTON, point, SMTO_ABORTIFHUNG, 2000, &ignored) != 0 &&
                    PostMessageW(state.window, WM_LBUTTONUP, 0, point) != FALSE;
            });
            return S_OK;
        }
        if(state.menus) {
            SetPropW(event->window,L"Pulse.PickerTestSnapshot",SnapshotRequest(state.screenshot));
            SendMessageW(event->window,WM_PAINT,0,0);
            RemovePropW(event->window,L"Pulse.PickerTestSnapshot");
            std::printf("MENU_SCREENSHOT=%ls\n",state.screenshot.c_str());
            state.driver=std::thread([&state,window=event->window] {
                if(!SetThreadDesktop(GetThreadDesktop(state.thread)))return;
                // Popups belong to the picker HWND thread (pulse.exe for the remote provider).
                DWORD ui_pid=0;const DWORD ui_thread=GetWindowThreadProcessId(window,&ui_pid);
                HWND edit=nullptr;for(unsigned i=0;i<1000 && IsWindow(window);++i) {
                    edit=GetDlgItem(window,102);if(edit && IsWindowEnabled(edit))break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if(!edit)return;
                DWORD_PTR ignored=0;
                auto key=[&](HWND target,UINT message,WPARAM key) {return SendMessageTimeoutW(target,message,key,0,SMTO_ABORTIFHUNG,2000,&ignored)!=0;};
                if(!key(window,WM_SYSKEYDOWN,'N') || !key(edit,WM_KEYDOWN,VK_TAB) || !key(window,WM_KEYDOWN,VK_TAB))return;
                auto choose=[&]() {
                    if(!PostMessageW(window,WM_KEYDOWN,VK_RETURN,0))return false;
                    HWND menu=nullptr;
                    for(unsigned i=0;i<1000 && IsWindow(window);++i) {
                        EnumThreadWindows(ui_thread,[](HWND hwnd,LPARAM out)->BOOL {
                            wchar_t name[64]{};GetClassNameW(hwnd,name,64);
                            if(IsWindowVisible(hwnd) && std::wstring_view(name)==L"PulseFluentMenu") {*reinterpret_cast<HWND*>(out)=hwnd;return FALSE;}return TRUE;
                        },reinterpret_cast<LPARAM>(&menu));
                        if(menu)break;std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    DWORD pid=0;if(!menu || GetWindowThreadProcessId(menu,&pid)!=ui_thread || pid!=ui_pid)return false;
                    if(!PostMessageW(menu,WM_KEYDOWN,VK_RETURN,0))return false;
                    for(unsigned i=0;i<1000 && IsWindowVisible(menu);++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    return !IsWindowVisible(menu);
                };
                if(!choose())return;
                // The popup hides before TrackDropdown restores focus and publishes selection.
                // Wait for that notification, then start from an explicit filename focus.
                for(unsigned i=0;i<1000 && !state.menu_selected.load() && IsWindow(window);++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if(!state.menu_selected.load() || !key(window,WM_SYSKEYDOWN,'N') || !key(edit,WM_KEYDOWN,VK_TAB))return;
                for(unsigned i=0;i<4;++i)if(!key(window,WM_KEYDOWN,VK_TAB))return;
                state.driver_ok=choose();
            });
            return S_OK;
        }
        if (state.mask_sequence) {
            state.driver=std::thread([&state,window=event->window] {
                if(!SetThreadDesktop(GetThreadDesktop(state.thread)))return;
                // Popups belong to the picker HWND thread (pulse.exe for the remote provider).
                DWORD ui_pid=0;const DWORD ui_thread=GetWindowThreadProcessId(window,&ui_pid);
                const HWND edit=GetDlgItem(window,102);if(!edit)return;
                const auto send=[](HWND target,UINT message,WPARAM wp,LPARAM lp=0) {
                    DWORD_PTR ignored=0;return SendMessageTimeoutW(target,message,wp,lp,SMTO_ABORTIFHUNG,2000,&ignored)!=0;
                };
                const wchar_t* names[]={L"literal",L"wildcard",L"all"};
                for(unsigned step=0;step<3;++step) {
                    // In-process, an outstanding callback (READY, FileOk) blocks
                    // these sends. A remote host must still run cross-thread
                    // sends during a callback, so wait until none is active and
                    // then confirm Alt+N really focused the filename: the editor is
                    // disabled while a filter change reloads, and a Tab from the
                    // type field would otherwise land on Cancel.
                    for(unsigned retry=0;retry<500 && state.callbacks_active.load();++retry)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    bool focused=false;
                    for(unsigned retry=0;retry<500 && IsWindow(window) && !focused;++retry) {
                        if(IsWindowEnabled(edit) && send(window,WM_SYSKEYDOWN,'N')) {
                            GUITHREADINFO info{sizeof(info)};
                            focused=GetGUIThreadInfo(ui_thread,&info)!=FALSE && info.hwndFocus==edit;
                        }
                        if(!focused)std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    if(!focused || !send(edit,WM_KEYDOWN,VK_TAB) || !PostMessageW(window,WM_KEYDOWN,VK_RETURN,0))return;
                    HWND menu=nullptr;
                    for(unsigned retry=0;retry<500 && IsWindow(window);++retry) {
                        EnumThreadWindows(ui_thread,[](HWND candidate,LPARAM output)->BOOL {
                            wchar_t name[64]{};GetClassNameW(candidate,name,64);
                            if(IsWindowVisible(candidate) && std::wstring_view(name)==L"PulseFluentMenu") {
                                *reinterpret_cast<HWND*>(output)=candidate;return FALSE;
                            }return TRUE;
                        },reinterpret_cast<LPARAM>(&menu));
                        if(menu)break;std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    DWORD pid=0;
                    if(!menu || GetWindowThreadProcessId(menu,&pid)!=ui_thread || pid!=ui_pid)return;
                    RECT bounds{};if(!GetClientRect(menu,&bounds))return;
                    const LPARAM point=MAKELPARAM((bounds.right-bounds.left)/2,(bounds.bottom-bounds.top)*static_cast<int>(2*(step+1)+1)/8);
                    if(!send(menu,WM_MOUSEMOVE,0,point) || !send(menu,WM_LBUTTONDOWN,MK_LBUTTON,point) ||
                        !PostMessageW(menu,WM_LBUTTONUP,0,point))return;
                    for(unsigned retry=0;retry<500 && state.mask_type.load()!=step+1 && IsWindow(window);++retry)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if(state.mask_type.load()!=step+1)return;
                    // A real filter change reloads the folder. The filename
                    // field remains valid, but wait until the popup has closed.
                    for(unsigned retry=0;retry<500 && IsWindowVisible(menu);++retry)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if(!send(edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(names[step])))return;
                    // Submit when the worker finishes this type's listing.
                    bool submitted=false;
                    for(unsigned retry=0;retry<500 && IsWindow(window) && state.mask_results.load()==step;++retry) {
                        if(retry%20==0)submitted=PostMessageW(edit,WM_KEYDOWN,VK_RETURN,0)!=FALSE;
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    if(!submitted || state.mask_results.load()!=step+1)return;
                }
                state.driver_ok=true;
            });
            return S_OK;
        }
        if(state.multi || state.combo) {
            state.driver=std::thread([&state,window=event->window] {
                if(!SetThreadDesktop(GetThreadDesktop(state.thread))) return;
                HWND edit=nullptr;
                for(unsigned i=0;i<1000;++i) {
                    if(!IsWindow(window)) return;
                    edit=GetDlgItem(window,102);
                    if(edit && IsWindowEnabled(edit)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                DWORD pid=0;
                const DWORD window_thread = GetWindowThreadProcessId(window, &pid);
#ifdef PULSE_REMOTE_PROVIDER_TEST
                if(!edit || !IsWindowEnabled(edit) || !window_thread || !pid) return;
#else
                if(!edit || !IsWindowEnabled(edit) || window_thread!=state.thread || pid!=GetCurrentProcessId()) return;
#endif
                if(state.multi) {
                    DWORD_PTR ignored=0;
                    if(state.list_ten) {
                        const bool attached=AttachThreadInput(GetCurrentThreadId(),state.thread,TRUE)!=FALSE;
                        BYTE keys[256]{},saved[256]{};GetKeyboardState(saved);
                        SendMessageTimeoutW(window,WM_KEYDOWN,VK_HOME,0,SMTO_ABORTIFHUNG,2000,&ignored);
                        std::copy(std::begin(saved),std::end(saved),std::begin(keys));keys[VK_CONTROL]=0x80;
                        SetKeyboardState(keys);
                        const bool selected=SendMessageTimeoutW(window,WM_KEYDOWN,'A',0,SMTO_ABORTIFHUNG,2000,&ignored)!=0;
                        SetKeyboardState(saved);
                        if(attached)AttachThreadInput(GetCurrentThreadId(),state.thread,FALSE);
                        state.driver_ok=attached && selected && PostMessageW(window,WM_KEYDOWN,VK_RETURN,0);
                        return;
                    }
                    const wchar_t* names=state.folder_single ? L"owned.txt" : state.open_missing ? L"\"owned.txt\" \"missing-parent\\missing.txt\"" : L"\"owned.txt\" \"second.txt\"";
                    state.driver_ok=SendMessageTimeoutW(edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(names),SMTO_ABORTIFHUNG,2000,&ignored)!=0 && PostMessageW(edit,WM_KEYDOWN,VK_RETURN,0);
                } else {
                    DWORD_PTR ignored=0;
                    const bool focus_request=SendMessageTimeoutW(window,WM_SYSKEYDOWN,'N',0,SMTO_ABORTIFHUNG,2000,&ignored)!=0;
                    GUITHREADINFO info{sizeof(info)};
                    const bool focused=GetGUIThreadInfo(window_thread,&info)!=FALSE && info.hwndFocus==edit;
                    std::printf("COMBO_DRIVER filename request=%u focused=%u focus=%p edit=%p\n",focus_request?1u:0u,focused?1u:0u,info.hwndFocus,edit);std::fflush(stdout);
                    if(!focus_request || !focused) return;
                    const bool first=SendMessageTimeoutW(edit,WM_KEYDOWN,VK_TAB,0,SMTO_ABORTIFHUNG,2000,&ignored)!=0;
                    const bool second=first && SendMessageTimeoutW(window,WM_KEYDOWN,VK_TAB,0,SMTO_ABORTIFHUNG,2000,&ignored)!=0;
                    const bool opened=second && PostMessageW(window,WM_KEYDOWN,VK_RETURN,0)!=FALSE;
                    std::printf("COMBO_DRIVER tabs=%u/%u open_post=%u\n",first?1u:0u,second?1u:0u,opened?1u:0u);std::fflush(stdout);
                    if(!opened) return;
                    HWND menu=nullptr;
                    for(unsigned i=0;i<1000 && IsWindow(window);++i) {
                        EnumThreadWindows(window_thread,[](HWND hwnd,LPARAM value)->BOOL {
                            wchar_t name[64]{};GetClassNameW(hwnd,name,64);
                            if(IsWindowVisible(hwnd) && std::wstring_view(name)==L"PulseFluentMenu") { *reinterpret_cast<HWND*>(value)=hwnd;return FALSE; }return TRUE;
                        },reinterpret_cast<LPARAM>(&menu));
                        if(menu) break;std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    DWORD menu_pid=0;
                    const bool owned_menu=menu && GetWindowThreadProcessId(menu,&menu_pid)==window_thread && menu_pid==pid && IsWindowVisible(menu);
                    if(owned_menu) state.driver_ok=PostMessageW(menu,WM_KEYDOWN,VK_RETURN,0)!=FALSE;
                    std::printf("COMBO_DRIVER menu=%p owned=%u select_post=%u\n",menu,owned_menu?1u:0u,state.driver_ok?1u:0u);std::fflush(stdout);
                }
            });
            return S_OK;
        }
        if (state.custom) {
            const std::wstring label = L"Updated Pulse button - press Enter or click";
            PulsePickerUiV2Control control{sizeof(control), PULSE_PICKER_UI_V2_CONTROL_BUTTON, 42, 7, 1, 3, Text(label), {0, 0}};
            Check(state.api->UpdateControl(state.session, state.show, &control) == S_OK, "update existing control label and own state");
            control.parent_id = 8;
            Check(state.api->UpdateControl(state.session, state.show, &control) == E_INVALIDARG, "update cannot change control parent identity");
            control.parent_id = 7; control.id = 99;
            Check(state.api->UpdateControl(state.session, state.show, &control) == E_INVALIDARG, "update unknown ID fails");
            if (state.inspect) { std::printf("INSPECT own Pulse custom controls ready; button 42 closes with exact HRESULT; up to 180 seconds\n"); std::fflush(stdout); return S_OK; }
        }
        if (state.escape_case) {
            if (state.escape_case == 2) SendMessageW(event->window, WM_KEYDOWN, VK_F4, 0);
            if (state.escape_case == 3) SendMessageW(event->window, WM_KEYDOWN, VK_F3, 0);
            if (state.escape_case == 4) SetFocus(event->window);
            HWND focused = GetFocus();
            Check(focused == event->window || IsChild(event->window, focused), "Escape targets actual focused picker control");
            Check(PostMessageW(focused, WM_KEYDOWN, VK_ESCAPE, 0) != FALSE, "post one Escape key to real picker");
            state.driver = std::thread([&state, window = event->window] {
                const auto deadline = GetTickCount64() + 3000;
                while (IsWindow(window) && GetTickCount64() < deadline) Sleep(10);
                if (IsWindow(window)) {
                    state.escape_watchdog = true;
                    std::printf("[FAIL] single Escape did not close dialog within three seconds\n"); std::fflush(stdout);
                    PostMessageW(window, WM_CLOSE, 0, 0);
                }
            });
            return S_OK;
        }
        if (state.window_cancel) {
            Check(PostMessageW(event->window, WM_CLOSE, 0, 0) != FALSE, "post actual picker window cancellation");
            return S_OK;
        }
        const HRESULT close = state.api->Close(state.session, state.show, state.requested);
        std::printf("CALLBACK Close request=%08lX call=%08lX\n", static_cast<unsigned long>(state.requested), static_cast<unsigned long>(close));
        Check(close == S_OK, "Close allowed inside READY callback");
        return S_OK;
    } catch (...) { state.api->Close(state.session, event ? event->show_id : 0, E_UNEXPECTED); return E_UNEXPECTED; }
}
int Run(const wchar_t* dll, const wchar_t* selected_case, bool interactive) {
    const int escape_case = wcscmp(selected_case, L"escape-filename") == 0 ? 1 :
        wcscmp(selected_case, L"escape-address") == 0 ? 2 : wcscmp(selected_case, L"escape-search") == 0 ? 3 :
        wcscmp(selected_case, L"escape-list") == 0 ? 4 : 0;
    const bool separators = wcscmp(selected_case, L"separators") == 0 || wcscmp(selected_case, L"separators-dark") == 0;
    const bool property_scroll = wcscmp(selected_case, L"property-scroll") == 0;
    const bool property_mutate = wcscmp(selected_case, L"property-mutate") == 0;
    const bool property_edit = property_scroll || property_mutate || wcscmp(selected_case, L"property-edit") == 0;
    const bool callback_navigation = wcscmp(selected_case,L"navigate-callback")==0;
    const bool external_navigation = callback_navigation || wcscmp(selected_case,L"navigate-external")==0;
    const bool computer = wcscmp(selected_case, L"computer-navigate") == 0;
    const bool places = computer || wcscmp(selected_case, L"places-navigate") == 0;
    const bool mask_sequence=wcscmp(selected_case,L"open-mask-sequence")==0;
    const bool extension_channel=wcscmp(selected_case,L"open-extension")==0;
    const bool labels_empty=wcscmp(selected_case,L"labels-empty")==0;
    const bool labels=labels_empty || wcscmp(selected_case,L"labels")==0;
    const bool folder_single=wcscmp(selected_case,L"folder-links-accept")==0;
    const bool folder_links=folder_single || wcscmp(selected_case,L"folder-links-multi-accept")==0;
    const bool multi_close=wcscmp(selected_case,L"multi-close")==0;
    const bool cross_accept = wcscmp(selected_case, L"cross-thread-accept") == 0;
    const bool open_missing=wcscmp(selected_case,L"open-missing")==0;
    const bool multi=folder_links || open_missing || multi_close || cross_accept || wcscmp(selected_case,L"multi-accept")==0;
    const bool values=wcscmp(selected_case,L"value-controls")==0;
    const bool menus=wcscmp(selected_case,L"menu-open")==0;
    const bool combo=wcscmp(selected_case,L"combo-select")==0;
    const int startup_terminal = wcscmp(selected_case, L"startup-folder-close") == 0 ? 1 :
        wcscmp(selected_case, L"startup-folder-cancel") == 0 ? 2 : wcscmp(selected_case, L"startup-folder-error") == 0 ? 3 : 0;
    const bool startup_folder = wcscmp(selected_case, L"startup-folder-visibility") == 0;
    const bool startup_visibility = startup_folder || wcscmp(selected_case, L"startup-visibility") == 0;
    const bool initial_empty = wcscmp(selected_case, L"initial-empty") == 0;
    const bool prominent_nested = wcsncmp(selected_case, L"prominent-nested-", 17) == 0;
    const wchar_t* prominent_case = prominent_nested ? selected_case + 17 : selected_case + (wcsncmp(selected_case, L"prominent-", 10) == 0 ? 10 : 0);
    const uint32_t prominent_kind = wcscmp(prominent_case, L"check") == 0 ? PULSE_PICKER_UI_V2_CONTROL_CHECK :
        wcscmp(prominent_case, L"combo") == 0 ? PULSE_PICKER_UI_V2_CONTROL_COMBO :
        wcscmp(prominent_case, L"button") == 0 ? PULSE_PICKER_UI_V2_CONTROL_BUTTON :
        wcscmp(prominent_case, L"menu") == 0 ? PULSE_PICKER_UI_V2_CONTROL_MENU : PULSE_PICKER_UI_V2_CONTROL_GROUP;
    const bool prominent = separators || prominent_kind != PULSE_PICKER_UI_V2_CONTROL_GROUP || wcscmp(selected_case, L"custom-prominent") == 0;
    const bool inspect = wcscmp(selected_case, L"custom-inspect") == 0;
    const bool custom = inspect || wcscmp(selected_case, L"custom-update") == 0;
    if (inspect) interactive = true;
#ifdef PULSE_REMOTE_PROVIDER_TEST
    // The remote picker presents from pulse.exe and its readiness requires a
    // composed frame. DWM only composes the input desktop, so a private
    // desktop that is never switched to always reports DXGI occlusion.
    interactive = true;
#endif
    const bool query = wcscmp(selected_case, L"query") == 0;
    const bool reentrant = wcscmp(selected_case, L"reentrant-owner") == 0;
    const bool cross_cancel = wcscmp(selected_case, L"cross-thread-cancel") == 0;
    const bool cross_owner = cross_cancel || cross_accept || wcscmp(selected_case, L"cross-thread-owner") == 0;
    const bool close_ok = wcscmp(selected_case, L"close-ok") == 0;
    if (!escape_case && !external_navigation && !mask_sequence && !startup_terminal && !startup_visibility && !values && !property_edit && !menus && !places && !extension_channel && !labels && !multi && !combo && !initial_empty && !prominent && !custom && !query && !close_ok && !reentrant && !cross_owner && wcscmp(selected_case, L"ready-close") != 0) return 2;
    if (!std::filesystem::path(dll).is_absolute()) return 2;
    Check(dpi.Available(), "required caller DPI APIs dynamically available");
    if (!dpi.Available()) return 1;
    const auto directory = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"picker-v2-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(directory.parent_path());
    if (!CreateDirectoryW(directory.c_str(), nullptr)) return 2;
    const bool list_ten=GetEnvironmentVariableW(L"PULSE_TEST_PICKER_TEN_JPG",nullptr,0)!=0;
    const auto file = directory / (list_ten ? L"00.jpg" : L"owned.txt");
    HANDLE created = folder_links ? INVALID_HANDLE_VALUE : CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (folder_links) { if (!CreateDirectoryW(file.c_str(),nullptr)) return 2; }
    else { if (created == INVALID_HANDLE_VALUE) { RemoveDirectoryW(directory.c_str()); return 2; } CloseHandle(created); }
    std::vector<std::filesystem::path> mask_files;
    if(list_ten) for(int i=2;i<10;++i) {
        const auto image=directory/(L"0"+std::to_wstring(i)+L".jpg");
        HANDLE fixture=CreateFileW(image.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(fixture==INVALID_HANDLE_VALUE)return 2;CloseHandle(fixture);mask_files.push_back(image);
    }
    if(mask_sequence)for(const auto* stem:{L"literal",L"wildcard",L"all"})for(const auto* suffix:{L".txt",L".md",L".xlsx"}) {
        const auto path=directory/(std::wstring(stem)+suffix);
        HANDLE fixture=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(fixture==INVALID_HANDLE_VALUE)return 2;CloseHandle(fixture);mask_files.push_back(path);
    }
    const auto second_file=open_missing ? directory/L"missing-parent"/L"missing.txt" : directory/(list_ten ? L"01.jpg" : L"second.txt");
    if(folder_links) { if (!CreateDirectoryW(second_file.c_str(),nullptr)) return 2; }
    else if(multi && !open_missing) { HANDLE second=CreateFileW(second_file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(second==INVALID_HANDLE_VALUE) return 2;CloseHandle(second); }
    const auto desktop_name = L"PulseV2Proof-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HDESK previous_desktop = GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop = interactive ? nullptr : CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    const bool switched = interactive || (desktop && SetThreadDesktop(desktop));
    Check(switched, interactive ? "current interactive desktop" : "private desktop without SwitchDesktop");
    if (!switched) {
        if (desktop) CloseDesktop(desktop);
        DeleteFileW(file.c_str()); RemoveDirectoryW(directory.c_str()); return 2;
    }
    const DPI_AWARENESS_CONTEXT previous_dpi = dpi.set_thread(DPI_AWARENESS_CONTEXT_UNAWARE);
    Check(previous_dpi != nullptr, "capture distinct caller thread DPI context");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(com), "caller STA");
    HMODULE module = nullptr;
    bool unload_safe = true;
    if (SUCCEEDED(com)) {
#ifdef PULSE_REMOTE_PROVIDER_TEST
        std::shared_ptr<const pulse::shell_bridge::FileDialogInprocessProvider> remote_provider;
        const HRESULT api_hr = pulse::shell_bridge::LoadFileDialogRemoteProvider(dll, remote_provider, true);
        PulsePickerUiV2Api api = remote_provider ? remote_provider->api : PulsePickerUiV2Api{};
        Check(api_hr == S_OK && !GetModuleHandleW(L"PulsePickerUiProof.dll"), "remote provider without caller UI DLL");
#else
        module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        Check(module != nullptr, "load explicit absolute UI DLL");
        using GetApi = HRESULT (WINAPI*)(uint32_t, uint32_t, PulsePickerUiV2Api*);
        const auto get = module ? reinterpret_cast<GetApi>(GetProcAddress(module, "PulsePickerUiGetApiV2")) : nullptr;
        PulsePickerUiV2Api api{};
        const HRESULT api_hr = get ? get(PULSE_PICKER_UI_V2_VERSION, sizeof(api), &api) : E_NOINTERFACE;
#endif
        std::printf("API result=%08lX capabilities=%u; internal proof required_capabilities=0, NOT production certification\n",
            static_cast<unsigned long>(api_hr), api.capabilities);
        const bool valid = api_hr == S_OK && api.size == sizeof(api) && api.version == 2 && api.Initialize && api.ValidateConfig &&
            api.Show && api.GetWindow && api.Query && api.Close && api.Shutdown && api.UpdateControl;
        Check(valid, "complete v2 API table");
        if (valid) {
            PulsePickerUiV2Initialize init{}; init.size = sizeof(init); init.version = 2;
            HRESULT mta_initialize = S_OK;
            std::thread apartment_probe([&] {
                const HRESULT mta = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                PulsePickerUiV2Session other = 0;
                mta_initialize = SUCCEEDED(mta) ? api.Initialize(&init, &other) : mta;
                if (other) { const HRESULT stopped = api.Shutdown(other); if (FAILED(stopped)) unload_safe = false; }
                if (SUCCEEDED(mta)) CoUninitialize();
            });
            apartment_probe.join();
            std::printf("MTA Initialize=%08lX (before any STA session)\n", static_cast<unsigned long>(mta_initialize));
            Check(FAILED(mta_initialize), "MTA Initialize rejected before active session exists");
            PulsePickerUiV2Session session = 0;
            const HRESULT initialized = api.Initialize(&init, &session);
            std::printf("INITIALIZE=%08lX\n", static_cast<unsigned long>(initialized));
            Check(initialized == S_OK && session, "initialize session");
            if (initialized == S_OK && session) {
                unload_safe = false;
                HRESULT wrong_query = S_OK, wrong_shutdown = S_OK;
                std::thread foreign([&] {
                    const HRESULT mta = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    PulsePickerUiV2Snapshot snapshot{}; snapshot.size = sizeof(snapshot);
                    wrong_query = api.Query(session, 1, 0, &snapshot);
                    wrong_shutdown = api.Shutdown(session);
                    if (SUCCEEDED(mta)) CoUninitialize();
                });
                foreign.join();
                Check(wrong_query == RPC_E_WRONG_THREAD && wrong_shutdown == RPC_E_WRONG_THREAD, "foreign thread cannot query or destroy session");
                OwnerReentry reentry;
                WNDCLASSW owner_class{}; owner_class.lpfnWndProc = OwnerProc;
                owner_class.hInstance = GetModuleHandleW(nullptr); owner_class.lpszClassName = L"PulseV2ProofOwner";
                const ATOM registered = RegisterClassW(&owner_class);
                Check(registered != 0, "register own owner procedure");
                ThreadOwner threaded_owner;
                if (registered && cross_owner) threaded_owner.Start(owner_class.lpszClassName, GetThreadDesktop(GetCurrentThreadId()));
                HWND owner = cross_owner ? threaded_owner.window : registered ? CreateWindowExW(0, owner_class.lpszClassName, L"Owned v2 proof owner", WS_OVERLAPPED, 0, 0, 200, 100,
                    nullptr, nullptr, GetModuleHandleW(nullptr), &reentry) : nullptr;
                Check(owner != nullptr, "own STA owner created");
                if (owner) {
                    if (interactive) {
                        // A real visible host is required for composed frames;
                        // consume a launcher's SW_HIDE before showing it.
                        ShowWindow(owner, SW_HIDE);
                        ShowWindow(owner, SW_SHOWNOACTIVATE);
                        UpdateWindow(owner);
                    }
                    const std::wstring folder = directory.wstring(), filename = initial_empty ? L"" : L"owned.txt", title = custom ? L"Pulse Custom Controls Inspection" : L"Pulse internal v2 proof";
                    PulsePickerUiV2Config config{}; config.size = sizeof(config); config.version = 2;
                    config.required_capabilities = 0; config.owner = owner; config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
                    config.initial_folder = Text(folder); config.filename = Text(filename); config.title = Text(title);
                    const std::wstring first_property = L"original metadata", second_property = L"literal;semicolon", property_label = L"Test metadata";
                    PulsePickerUiV2Text property_values[] = {Text(first_property), Text(second_property)};
                    PulsePickerUiV2Property property{sizeof(PulsePickerUiV2Property), 42, PULSE_PICKER_UI_V2_PROPERTY_LIST, Text(property_label), property_values, 2};
                    std::vector<std::wstring> property_labels(8), property_text(8);
                    std::vector<std::vector<PulsePickerUiV2Text>> scroll_values(8);
                    std::vector<PulsePickerUiV2Property> scroll_properties;
                    if (property_scroll) {
                        for (uint32_t i = 0; i < 8; ++i) {
                            property_labels[i] = L"Metadata field " + std::to_wstring(i + 1);
                            property_text[i] = i == 7 ? L"last scalar original" : i == 0 ? L"original metadata" : L"scalar value";
                            scroll_values[i].push_back(Text(property_text[i]));
                            if (i < 3) scroll_values[i].push_back(Text(second_property));
                            scroll_properties.push_back({sizeof(PulsePickerUiV2Property), 100 + i,
                                i < 3 ? PULSE_PICKER_UI_V2_PROPERTY_LIST : PULSE_PICKER_UI_V2_PROPERTY_SCALAR,
                                Text(property_labels[i]), scroll_values[i].data(), static_cast<uint32_t>(scroll_values[i].size())});
                        }
                    }
                    if (startup_visibility || startup_terminal) {
                        config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
                        config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
                    }
                    if (property_edit) {
                        config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
                        config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
                        config.properties = property_scroll ? scroll_properties.data() : &property;
                        config.property_count = property_scroll ? 8u : 1u;
                    }

                    const std::wstring place_path = computer ? L"pulse:computer" : (directory / L"place-target").wstring();
                    PulsePickerUiV2Place place_navigation{sizeof(PulsePickerUiV2Place), PULSE_PICKER_UI_V2_PLACE_TOP, Text(title), Text(place_path)};
                    if (external_navigation) Check(CreateDirectoryW(place_path.c_str(),nullptr)!=FALSE,"create external destination");
                    if (places) {
                        if (!computer) Check(CreateDirectoryW(place_path.c_str(), nullptr) != FALSE, "create isolated place destination");
                        config.places = &place_navigation; config.place_count = 1;
                    }
                    const std::wstring group_label = L"Pulse application actions", button_label = L"Original action";
                    PulsePickerUiV2Control controls[] = {
                        {sizeof(PulsePickerUiV2Control), PULSE_PICKER_UI_V2_CONTROL_GROUP, 7, 0, 0, 3, Text(group_label), {0, 0}},
                        {sizeof(PulsePickerUiV2Control), PULSE_PICKER_UI_V2_CONTROL_BUTTON, 42, 7, 1, 3, Text(button_label), {0, 0}}};
                    if (custom) {
                        config.controls = controls; config.control_count = 2;
                        controls[1].id = 7;
                        Check(api.ValidateConfig(session, &config) == E_INVALIDARG, "duplicate custom ID rejected before UI");
                        controls[1].id = 42; controls[1].parent_id = 99;
                        Check(api.ValidateConfig(session, &config) == E_INVALIDARG, "missing parent rejected before UI");
                        controls[1].parent_id = 7; config.control_count = 17;
                        Check(api.ValidateConfig(session, &config) == E_INVALIDARG, "custom count bounded before reading array");
                        config.control_count = 2;
                    }
                    if (custom) {
                        PulsePickerUiV2Place place{sizeof(PulsePickerUiV2Place), PULSE_PICKER_UI_V2_PLACE_TOP, Text(title), Text(folder)};
                        config.places = &place; config.place_count = 1;
                        Check(api.ValidateConfig(session, &config) == S_OK, "filesystem place accepted before UI");
                        place.location = 2;
                        Check(api.ValidateConfig(session, &config) == E_INVALIDARG, "unknown place position rejected");
                        place.location = PULSE_PICKER_UI_V2_PLACE_BOTTOM; place.size = 0;
                        Check(api.ValidateConfig(session, &config) == E_INVALIDARG, "wrong place size rejected");
                        place.size = sizeof(place); config.place_count = PULSE_PICKER_UI_V2_MAX_PLACES + 1;
                        Check(api.ValidateConfig(session, &config) == E_INVALIDARG, "place count bounded before array read");
                        config.places = nullptr; config.place_count = 1;
                        Check(api.ValidateConfig(session, &config) == E_INVALIDARG, "missing place array rejected");
                        config.place_count = 0;
                    }
                    const std::wstring prominent_labels[] = {L"Additional action", L"Export options", L"Open exported document after saving"};
                    PulsePickerUiV2Control prominent_controls[3]{};
                    for (unsigned i = 0; i < 3; ++i) {
                        prominent_controls[i].size = sizeof(PulsePickerUiV2Control);
                        prominent_controls[i].kind = i == 0 ? PULSE_PICKER_UI_V2_CONTROL_BUTTON : i == 1 ? PULSE_PICKER_UI_V2_CONTROL_GROUP : PULSE_PICKER_UI_V2_CONTROL_CHECK;
                        prominent_controls[i].id = i == 0 ? 40 : i == 1 ? 7 : 42;
                        prominent_controls[i].state = 3;
                        prominent_controls[i].label = Text(prominent_labels[i]);
                    }
                    prominent_controls[2].has_parent = 1;
                    prominent_controls[2].parent_id = 7;
                    const std::wstring prominent_choice = L"Export current document";
                    PulsePickerUiV2ControlItem prominent_item{sizeof(PulsePickerUiV2ControlItem), 91, 3, 0, Text(prominent_choice)};
                    const unsigned prominent_index = prominent_nested ? 2 : 1;
                    if (prominent_kind != PULSE_PICKER_UI_V2_CONTROL_GROUP) {
                        prominent_controls[prominent_index].kind = prominent_kind;
                        if (prominent_kind == PULSE_PICKER_UI_V2_CONTROL_COMBO || prominent_kind == PULSE_PICKER_UI_V2_CONTROL_MENU) {
                            prominent_controls[prominent_index].items = &prominent_item;
                            prominent_controls[prominent_index].item_count = 1;
                            prominent_controls[prominent_index].has_selected = prominent_kind == PULSE_PICKER_UI_V2_CONTROL_COMBO;
                            prominent_controls[prominent_index].selected_item_id = prominent_controls[prominent_index].has_selected ? 91 : 0;
                        }
                    }
                    if (prominent) {
                        config.controls = prominent_controls;
                        config.control_count = prominent_nested || prominent_kind == PULSE_PICKER_UI_V2_CONTROL_GROUP ? 3 : 2;
                        Check((api.implemented_features & (prominent_kind == PULSE_PICKER_UI_V2_CONTROL_GROUP ? PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_GROUP : PULSE_PICKER_UI_V2_FEATURE_CUSTOM_PROMINENT_CONTROL)) != 0, "prominent group feature advertised");
                    }
                    PulsePickerUiV2Control separator_controls[5]{};
                    if (separators) {
                        for (unsigned i = 0; i < 5; ++i) {
                            auto& control = separator_controls[i];
                            control.size = sizeof(control); control.id = 2024 + i; control.state = 3;
                            control.kind = i == 0 ? PULSE_PICKER_UI_V2_CONTROL_GROUP :
                                i == 2 || i == 4 ? PULSE_PICKER_UI_V2_CONTROL_BUTTON : PULSE_PICKER_UI_V2_CONTROL_SEPARATOR;
                            if (i == 1 || i == 2) { control.has_parent = 1; control.parent_id = 2024; }
                        }
                        separator_controls[0].label = Text(group_label);
                        separator_controls[2].label = Text(button_label);
                        separator_controls[4].label = Text(prominent_labels[0]);
                        config.controls = separator_controls; config.control_count = 5;
                        config.appearance = wcscmp(selected_case, L"separators-dark") == 0 ? PULSE_PICKER_UI_V2_DARK : 0;
                        Check(api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_CUSTOM_SEPARATOR, "separator feature explicitly advertised");
                        separator_controls[1].state = 7;
                        Check(api.ValidateConfig(session, &config) == E_INVALIDARG, "separator rejects unsupported prominence");
                        separator_controls[1].state = 3;
                    }
                    const std::wstring mask_labels[]={L"Text",L"Literal",L"Extension mask",L"All"};
                    std::wstring mask_patterns[]={L"*.txt",L"*.md",L"*.xl*;*.xlsx",L"*.*"};
                    PulsePickerUiV2Filter mask_filters[4]{};
                    for(unsigned i=0;i<4;++i) {mask_filters[i].size=sizeof(mask_filters[i]);mask_filters[i].label=Text(mask_labels[i]);mask_filters[i].pattern=Text(mask_patterns[i]);}
                    const std::wstring open_extension=L"txt";
                    if(mask_sequence) {
                        config.filters=mask_filters;config.filter_count=4;config.default_extension=Text(open_extension);
                        config.open_options|=PULSE_PICKER_UI_V2_OPEN_USE_DEFAULT_EXTENSION;
                        Check(api.ValidateConfig(session,&config)==S_OK,"real UI accepts generic Open extension mask config");
                        const auto original=mask_patterns[2];
                        for(const auto* invalid:{L"report*.txt",L"C:\\*.xlsx",L"*.x?",L"*.a b"}) {
                            mask_patterns[2]=invalid;mask_filters[2].pattern=Text(mask_patterns[2]);
                            Check(api.ValidateConfig(session,&config)==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),"real UI rejects unsupported mask scope before Show");
                        }
                        // Native semicolon lists ignore empty segments (picker_open_filter.h).
                        mask_patterns[2]=L"*.xl*;";mask_filters[2].pattern=Text(mask_patterns[2]);
                        Check(api.ValidateConfig(session,&config)==S_OK,"real UI accepts trailing empty mask segment like native Open");
                        mask_patterns[2]=original;mask_filters[2].pattern=Text(mask_patterns[2]);
                        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;config.open_options=0;config.default_extension={};
                        Check(api.ValidateConfig(session,&config)==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),"real UI retains Save mask rejection");
                        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
                        config.open_options=PULSE_PICKER_UI_V2_OPEN_USE_DEFAULT_EXTENSION;config.default_extension=Text(open_extension);
                    }
                    if(extension_channel) {
                        Check((api.implemented_features&PULSE_PICKER_UI_V2_FEATURE_OPEN_DEFAULT_EXTENSION)!=0,"Open extension feature negotiated");
                        config.default_extension=Text(open_extension);
                        Check(api.ValidateConfig(session,&config)==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),"Open extension without explicit presence rejected");
                        config.open_options|=PULSE_PICKER_UI_V2_OPEN_USE_DEFAULT_EXTENSION;
                        Check(api.ValidateConfig(session,&config)==S_OK,"Open extension explicit presence accepted");
                        config.default_extension={};
                        Check(api.ValidateConfig(session,&config)==S_OK,"Open extension explicit empty remains supported");
                        config.default_extension=Text(open_extension);
                    }
                    std::wstring ok_label(180,L'W'),file_label(180,L'F');
                    PulsePickerUiV2ControlItem combo_items[]={{sizeof(PulsePickerUiV2ControlItem),0,3,0,Text(group_label)},{sizeof(PulsePickerUiV2ControlItem),77,3,0,Text(button_label)}};
                    if(labels) {
                        Check((api.implemented_features&PULSE_PICKER_UI_V2_FEATURE_CUSTOM_LABELS)!=0,"labels feature explicitly negotiated");
                        config.label_flags=PULSE_PICKER_UI_V2_LABEL_TITLE|PULSE_PICKER_UI_V2_LABEL_OK|PULSE_PICKER_UI_V2_LABEL_FILE_NAME;
                        if(labels_empty) {ok_label.clear();file_label.clear();config.title={};}
                        config.ok_button_label=Text(ok_label);config.file_name_label=Text(file_label);
                        config.label_flags|=8;
                        Check(api.ValidateConfig(session,&config)==E_INVALIDARG,"unknown label presence flag rejected");config.label_flags&=7;
                    }
                    if(combo) {
                        Check((api.implemented_features&PULSE_PICKER_UI_V2_FEATURE_CUSTOM_COMBOBOX)!=0,"combo feature explicitly negotiated");
                        controls[1].kind=PULSE_PICKER_UI_V2_CONTROL_COMBO;controls[1].items=combo_items;controls[1].item_count=2;
                        controls[1].has_selected=1;controls[1].selected_item_id=77;config.controls=controls;config.control_count=2;
                        controls[1].selected_item_id=99;Check(api.ValidateConfig(session,&config)==E_INVALIDARG,"unknown selected item rejected");controls[1].selected_item_id=77;
                    }
                    PulsePickerUiV2Control menu_controls[2]{};
                    if(menus) {
                        menu_controls[0]=controls[1];menu_controls[0].kind=PULSE_PICKER_UI_V2_CONTROL_MENU;menu_controls[0].has_parent=0;menu_controls[0].parent_id=0;
                        menu_controls[0].items=combo_items;menu_controls[0].item_count=2;
                        menu_controls[1]=menu_controls[0];menu_controls[1].kind=PULSE_PICKER_UI_V2_CONTROL_OPEN_DROPDOWN;menu_controls[1].id=50;
                        menu_controls[1].has_selected=1;menu_controls[1].selected_item_id=0;
                        config.controls=menu_controls;config.control_count=2;
                    }
                    if(multi && !folder_single) {Check((api.implemented_features&PULSE_PICKER_UI_V2_FEATURE_MULTI_SELECT)!=0,"multi feature explicitly negotiated");config.open_options|=PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT;}
                    if(folder_links) {
                        config.access_policy=PULSE_PICKER_UI_V2_ACCESS_FOLDER_V1;
                        config.open_options|=PULSE_PICKER_UI_V2_OPEN_NO_DEREFERENCE_LINKS;
                    }
                    if(open_missing) {
                        Check((api.implemented_features&PULSE_PICKER_UI_V2_FEATURE_OPEN_MISSING)!=0,"missing Open feature explicitly negotiated");
                        config.open_options|=PULSE_PICKER_UI_V2_OPEN_ALLOW_MISSING|PULSE_PICKER_UI_V2_OPEN_SKIP_ACCESS_VALIDATION;
                        Check(GetFileAttributesW(second_file.c_str())==INVALID_FILE_ATTRIBUTES &&
                            GetFileAttributesW(second_file.parent_path().c_str())==INVALID_FILE_ATTRIBUTES,"missing Open starts with absent leaf and parent");
                    }
                    PulsePickerUiV2Control value_controls[3]{};
                    PulsePickerUiV2ControlItem radio_items[]={{sizeof(PulsePickerUiV2ControlItem),207,3,0,Text(group_label)},{sizeof(PulsePickerUiV2ControlItem),208,2,0,Text(button_label)}};
                    const std::wstring initial_value=L"initial";
                    if(values) {
                        for(auto& control:value_controls)control.size=sizeof(control);
                        value_controls[0].kind=6;value_controls[0].id=206;value_controls[0].items=radio_items;value_controls[0].item_count=2;value_controls[0].has_selected=1;value_controls[0].selected_item_id=208;
                        value_controls[1].kind=7;value_controls[1].id=210;value_controls[1].label=Text(button_label);
                        value_controls[2].kind=8;value_controls[2].id=213;value_controls[2].text=Text(initial_value);
                        config.controls=value_controls;config.control_count=3;
                    }
                    const HRESULT validated = api.ValidateConfig(session, &config);
                    Check(validated == S_OK, "internal config validation");
                    if (validated == S_OK) {
                        StartupVisibility startup;
                        State state; state.api = &api; state.session = session; state.thread = GetCurrentThreadId(); state.owner = owner;
                        if (startup_visibility) {
                            state.startup = &startup;
                            startup.folder_trigger = startup_folder;
                            Check(startup.Install((directory.parent_path() / (directory.filename().wstring() + L"-startup.png")).wstring()),
                                "install real window transition observer before Show");
                        }
                        state.mask_sequence=mask_sequence;state.mask_folder=folder;
                        state.startup_terminal = startup_terminal;
                        state.window_cancel = cross_cancel;
                        state.escape_case = escape_case;
                        state.values=values;state.value_controls=value_controls;
                        state.menus=menus;
                        if(menus)state.screenshot=(directory.parent_path()/(L"picker-menu-open-"+std::to_wstring(GetCurrentProcessId())+L".png")).wstring();
                        state.property_edit = property_edit; state.property_mutate = property_mutate; state.property_scroll = property_scroll;
                        if (property_edit) state.screenshot = (directory.parent_path() / ((property_scroll ? L"picker-properties-scroll-" : L"picker-properties-") + std::to_wstring(GetCurrentProcessId()) + L".png")).wstring();
                        state.callback_navigation = callback_navigation;
                        state.external_navigation = external_navigation;
                        state.places = places; state.place_path = place_path;
                        if (computer) state.screenshot = (directory.parent_path() / (L"picker-computer-" + std::to_wstring(GetCurrentProcessId()) + L".png")).wstring();
                        state.list_ten=list_ten;state.folder_single=folder_single;state.labels=labels;state.labels_empty=labels_empty;state.multi=multi;state.multi_close=multi_close;state.open_missing=open_missing;state.combo=combo;state.title=labels_empty ? L"" : title;
                        if(labels) state.screenshot=(directory.parent_path()/(L"picker-labels-"+std::to_wstring(GetCurrentProcessId())+(labels_empty ? L"-empty.png" : L"-long.png"))).wstring();
                        state.separators = separators;
                        state.caller_dpi = dpi.get_thread(); state.prominent = prominent; state.prominent_control = &prominent_controls[prominent_index]; state.prominent_parent = prominent_nested ? &prominent_controls[1] : nullptr;
                        if (separators) {
                            state.prominent_control = &separator_controls[2];
                            state.prominent_parent = &separator_controls[0];
                        }
                        state.query = query; state.custom = custom; state.inspect = inspect; state.initial_empty = initial_empty; state.requested = close_ok ? S_OK : kClose;
                        PulsePickerUiV2Callbacks callbacks{}; callbacks.size = sizeof(callbacks); callbacks.callback = Callback; callbacks.context = &state;
                        std::vector<wchar_t> path(PULSE_PICKER_UI_V2_MAX_UNITS + 1, L'X');
                        PulsePickerUiV2Result result{}; result.size = sizeof(result); result.path = {path.data(), static_cast<uint32_t>(path.size()), 0};
                        std::vector<wchar_t> packed(multi ? PULSE_PICKER_UI_V2_MAX_PACKED_UNITS+1 : 0);
                        if(multi) result.paths={packed.data(),static_cast<uint32_t>(packed.size()),0};
                        std::vector<wchar_t> nested_path(PULSE_PICKER_UI_V2_MAX_UNITS + 1);
                        reentry.api = &api; reentry.session = session; reentry.config = &config; reentry.callbacks = &callbacks;
                        reentry.nested.size = sizeof(reentry.nested);
                        reentry.nested.path = {nested_path.data(), static_cast<uint32_t>(nested_path.size()), 0};
                        reentry.armed = reentrant;
                        HANDLE admission_lock = INVALID_HANDLE_VALUE;
                        if (startup_terminal == 3) {
                            admission_lock = CreateFileW(directory.c_str(), GENERIC_READ, 0, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                            Check(admission_lock != INVALID_HANDLE_VALUE, "exclusively hold isolated directory to force real admission failure");
                        }
                        const HRESULT shown = api.Show(session, &config, &callbacks, &result);
                        if (admission_lock != INVALID_HANDLE_VALUE) CloseHandle(admission_lock);
                        reentry.armed = false;
                        if (property_edit) { std::printf("PROPERTY_SHOW_RETURNED=%08lX before-driver-join\n", static_cast<unsigned long>(shown)); std::fflush(stdout); }
                        if(state.driver.joinable()) state.driver.join();
                        if (property_edit) Check(state.driver_ok && state.property_events == (property_mutate ? 2u : 1u), "real painted metadata edits emit complete property events before cancellation");
                        if (callback_navigation) Check(state.navigation_completions==3,"all queued requests complete exactly once");
                        if (external_navigation) Check(RemoveDirectoryW(place_path.c_str())!=FALSE,"remove external destination");
                        if (places) { Check(state.driver_ok && state.navigated, "own UI place navigates after actual sidebar click"); if (!computer) Check(RemoveDirectoryW(place_path.c_str()) != FALSE, "remove isolated place destination"); }
                        if(values)Check(state.driver_ok && state.item_events==1 && state.check_events==1 && state.edit_events>=2,"actual radio/check/painted edit input lifecycle");
                        if(menus) Check(state.driver_ok && state.activations==2 && state.item_events==1 && state.dropdown_events==1 && state.file_ok_events==1,"real menu and split keyboard sequence reaches correct callbacks");
                        if(combo) Check(state.driver_ok && state.item_events==1,"real popup selection delivered exactly one combo event");
                        if (inspect) Check(state.button_clicks == 1, "inspection completed through real custom button");
                        if (reentrant) {
                            std::printf("OWNER_REENTRY fired=%d Shutdown=%08lX Show=%08lX\n", reentry.fired,
                                static_cast<unsigned long>(reentry.shutdown), static_cast<unsigned long>(reentry.show));
                            Check(reentry.fired && reentry.shutdown == HRESULT_FROM_WIN32(ERROR_BUSY) &&
                                reentry.show == HRESULT_FROM_WIN32(ERROR_BUSY), "synchronous owner WM_ENABLE cannot shutdown or nest Show");
                        }
                        std::printf("SHOW=%08lX terminal=%08lX status=%u flags=%u path_units=%u\n", static_cast<unsigned long>(shown),
                            static_cast<unsigned long>(result.terminal_hr), result.status, result.flags, result.path.units);
                        if (mask_sequence) {
                            Check(state.driver_ok && state.file_ok_events==3 && state.mask_results==3 && shown==S_OK &&
                                result.status==PULSE_PICKER_UI_V2_ACCEPTED && std::wstring(path.data())==(directory/L"all.txt").wstring(),
                                "real Pulse filter sequence accepts original-default all.txt after three observed FileOk candidates");
                            Check(state.mask_type_events==std::vector<unsigned>{0,1,2,3},"real Pulse OnTypeChange confirms literal then wildcard then all selections exactly once");
                        } else if (startup_terminal) {
                            Check(!state.ready && (result.flags & PULSE_PICKER_UI_V2_UI_SHOWN) == 0,
                                "initial close, cancellation or error never reports READY or UI_SHOWN");
                            if (startup_terminal == 1)
                                Check(state.startup_changed == 1 && shown == kClose && result.terminal_hr == kClose &&
                                    result.status == PULSE_PICKER_UI_V2_CLOSED, "initial FolderChanged Close preserves exact HRESULT once");
                            else if (startup_terminal == 2)
                                Check(shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) && result.status == PULSE_PICKER_UI_V2_CANCELLED,
                                    "initial posted cancellation remains cancellation without a visible dialog");
                            else
                                Check(state.startup_changing == 2 && state.startup_changed == 0 && FAILED(shown) &&
                                    result.status != PULSE_PICKER_UI_V2_ACCEPTED, "real directory admission failure and rejected fallback produce explicit error");
                            Check(result.path.units == 0 && path[0] == 0, "failed initialization exposes no selected result");
                        } else if(list_ten) {
                            Check(state.driver_ok && state.file_ok_events==1 && shown==S_OK && result.status==PULSE_PICKER_UI_V2_ACCEPTED && result.path_count==10,"ten JPG list selections accepted after real worker FileOk");
                            size_t at=0;bool exact=true;
                            for(int i=0;i<10;++i) {
                                const auto expected=(directory/(L"0"+std::to_wstring(i)+L".jpg")).wstring();
                                if(at>=result.paths.units) {exact=false;break;}
                                const std::wstring actual=packed.data()+at;exact &= actual==expected;at+=actual.size()+1;
                            }
                            Check(exact && at==result.paths.units && std::wstring(path.data())==file.wstring(),"ten JPG packed results retain exact order and complete paths");
                        } else if(multi && !multi_close) {
                            Check(state.driver_ok && state.file_ok_events==1 && shown==S_OK && result.status==PULSE_PICKER_UI_V2_ACCEPTED && result.path_count==(folder_single ? 1u : 2u),"multi typed-path request accepted both candidates after real worker FileOk");
                            const std::wstring first=packed.empty() ? L"" : packed.data();
                            const std::wstring second=result.paths.units>first.size()+1 ? packed.data()+first.size()+1 : L"";
                            Check(first==file.wstring() && (folder_single || second==second_file.wstring()) && std::wstring(path.data())==first && result.paths.units==first.size()+(folder_single ? 1 : second.size()+2),"multi packed paths exact order and first item agree");
                            if(open_missing) Check(GetFileAttributesW(second_file.c_str())==INVALID_FILE_ATTRIBUTES &&
                                GetFileAttributesW(second_file.parent_path().c_str())==INVALID_FILE_ATTRIBUTES,
                                "real missing Open accepts both paths without creating file or parent");
                        } else if (cross_cancel || escape_case) {
                            if (escape_case) Check(state.ready && !state.escape_watchdog, "single Escape closes without watchdog WM_CLOSE");
                            Check(state.ready && shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
                                result.status == PULSE_PICKER_UI_V2_CANCELLED && result.path.units == 0 && path[0] == 0,
                                "cross-thread owner window cancellation returns no selection");
                        } else {
                            Check(state.ready && shown == state.requested && result.terminal_hr == state.requested, "Show preserves exact callback Close HRESULT");
                            Check(result.status == PULSE_PICKER_UI_V2_CLOSED && result.path.units == 0 && path[0] == 0,
                                "CLOSED has no accepted path even for Close S_OK");
                            if(multi_close) Check(state.driver_ok && state.file_ok_events==1 && result.path_count==0 && result.paths.units==0 && packed[0]==0,"Close discards the complete prior multi candidate array");
                        }
                        if (!startup_terminal) Check((result.flags & PULSE_PICKER_UI_V2_UI_SHOWN) != 0, "actual UI_SHOWN recorded");
                        Check(IsWindowEnabled(owner) && (!state.window || !IsWindow(state.window)), "owner restored and own UI destroyed");
                        Check(dpi.equal(dpi.get_thread(), state.caller_dpi), "Show restores caller DPI context");
                        Check(FAILED(api.Close(session, state.show, kClose)), "Close after Show rejected");
                    }
                    if (cross_owner) threaded_owner.Stop(); else DestroyWindow(owner);
                }
                if (registered) Check(UnregisterClassW(owner_class.lpszClassName, owner_class.hInstance) != FALSE, "unregister own owner class");
                const HRESULT shutdown = api.Shutdown(session);
                unload_safe = shutdown == S_OK;
                Check(unload_safe, "Shutdown session");
                HWND stale = nullptr;
                Check(FAILED(api.GetWindow(session, 1, &stale)), "old session rejected after Shutdown");
                Check(FAILED(api.Shutdown(session)), "second Shutdown rejected");
#ifdef PULSE_REMOTE_PROVIDER_TEST
                if (wcscmp(selected_case, L"ready-close") == 0) {
                    const auto previous_session = session;
                    session = 0;
                    Check(api.Initialize(&init, &session) == S_OK && session && session != previous_session,
                        "same provider reinitializes with fresh session");
                    if (session) {
                        Check(FAILED(api.Shutdown(previous_session)), "old token cannot shutdown new session");
                        HWND repeat_owner = CreateWindowW(L"STATIC", L"Remote repeat owner", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            100, 100, 300, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                        const auto folder = directory.wstring();
                        PulsePickerUiV2Config repeat{}; repeat.size = sizeof(repeat); repeat.version = 2;
                        repeat.owner = repeat_owner; repeat.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
                        repeat.initial_folder = Text(folder);
                        State repeat_state; repeat_state.api = &api; repeat_state.session = session;
                        repeat_state.owner = repeat_owner; repeat_state.thread = GetCurrentThreadId();
                        repeat_state.caller_dpi = dpi.get_thread();
                        PulsePickerUiV2Callbacks repeat_callbacks{}; repeat_callbacks.size = sizeof(repeat_callbacks);
                        repeat_callbacks.callback = Callback; repeat_callbacks.context = &repeat_state;
                        std::vector<wchar_t> repeat_path(PULSE_PICKER_UI_V2_MAX_UNITS + 1);
                        PulsePickerUiV2Result repeat_result{}; repeat_result.size = sizeof(repeat_result);
                        repeat_result.path = {repeat_path.data(), static_cast<uint32_t>(repeat_path.size()), 0};
                        Check(api.ValidateConfig(session, &repeat) == S_OK, "repeat configuration accepted");
                        const auto repeat_hr = api.Show(session, &repeat, &repeat_callbacks, &repeat_result);
                        Check(repeat_state.ready && repeat_hr == kClose && repeat_result.terminal_hr == kClose &&
                            repeat_result.status == PULSE_PICKER_UI_V2_CLOSED, "second actual remote Show closes exactly");
                        Check(IsWindowEnabled(repeat_owner), "repeat owner restored");
                        DestroyWindow(repeat_owner);
                        Check(api.Shutdown(session) == S_OK, "repeat session shuts down");
                    }
                }
#endif
            }
        }
        // The UI component may pin itself. Release only this test's explicit load.
        if (module && unload_safe) FreeLibrary(module);
        else if (module) std::printf("CLEANUP session shutdown unconfirmed; retaining UI module until process exit\n");
        CoUninitialize();
    }
    if (previous_dpi) dpi.set_thread(previous_dpi);
    if (!interactive) Check(SetThreadDesktop(previous_desktop) != FALSE, "restore caller desktop");
    if (!interactive) Check(CloseDesktop(desktop) != FALSE, "close private desktop");
    if(folder_links) Check(RemoveDirectoryW(second_file.c_str())!=FALSE,"delete own second directory");
    else if(multi && !open_missing) Check(DeleteFileW(second_file.c_str())!=FALSE,"delete own second file");
    for(const auto& path:mask_files)Check(DeleteFileW(path.c_str())!=FALSE,"delete exact owned mask fixture");
    Check((folder_links ? RemoveDirectoryW(file.c_str()) : DeleteFileW(file.c_str())) != FALSE, "delete own file or directory");
    Check(RemoveDirectoryW(directory.c_str()) != FALSE, "delete own empty fixture directory");
    return failures ? 1 : 0;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[2]) == L"folder-links-config") {
        if (!std::filesystem::path(argv[1]).is_absolute()) return 2;
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(apartment)) return 2;
        HMODULE module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Check(module != nullptr, "load actual production picker provider");
        const auto get = module ? reinterpret_cast<decltype(&PulsePickerUiGetApiV2)>(GetProcAddress(module, "PulsePickerUiGetApiV2")) : nullptr;
        PulsePickerUiV2Api api{}; api.size = sizeof(api);
        const bool available = get && get(2, sizeof(api), &api) == S_OK && api.Initialize && api.ValidateConfig && api.Shutdown;
        Check(available, "production provider exports actual v2 validation");
        bool safe = true;
        if (available) {
            PulsePickerUiV2Initialize init{}; init.size = sizeof(init); init.version = 2;
            PulsePickerUiV2Session session = 0;
            Check(api.Initialize(&init, &session) == S_OK && session, "initialize actual STA provider");
            if (session) {
                HWND owner = CreateWindowExW(0, L"STATIC", L"folder config fixture", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
                const std::wstring folder = std::filesystem::absolute(L"bench_data").wstring();
                PulsePickerUiV2Config config{}; config.size = sizeof(config); config.version = 2;
                config.owner = owner; config.initial_folder = Text(folder);
                config.access_policy = PULSE_PICKER_UI_V2_ACCESS_FOLDER_V1;
                // Match the proof-provider production session; do not claim native parity.
                config.required_capabilities = 0;
                for (const uint32_t multiple : {0u, uint32_t(PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT)}) {
                    config.open_options = PULSE_PICKER_UI_V2_OPEN_NO_DEREFERENCE_LINKS | multiple;
                    const HRESULT accepted = api.ValidateConfig(session, &config);
                    std::printf("FOLDER_CONFIG multi=%u hr=%08lX\n", multiple, static_cast<unsigned long>(accepted));
                    Check(accepted == S_OK, "actual folder provider accepts propagated no-dereference single/multi protocol");
                    config.required_capabilities = PULSE_PICKER_UI_V2_CAP_FOLDER_V1;
                    Check(FAILED(api.ValidateConfig(session, &config)), "proof provider does not advertise unverified native parity");
                    config.required_capabilities = 0;
                    for (const uint32_t invalid : {uint32_t(PULSE_PICKER_UI_V2_OPEN_SKIP_ACCESS_VALIDATION), uint32_t(PULSE_PICKER_UI_V2_OPEN_ALLOW_MISSING), uint32_t(PULSE_PICKER_UI_V2_OPEN_USE_DEFAULT_EXTENSION), 0x80000000u}) {
                        config.open_options |= invalid;
                        Check(FAILED(api.ValidateConfig(session, &config)), "actual folder provider rejects unrelated or unsafe option");
                        config.open_options &= ~invalid;
                    }
                    const std::wstring extension = L"txt";
                    config.default_extension = Text(extension);
                    Check(FAILED(api.ValidateConfig(session, &config)), "actual folder provider rejects file extension semantics");
                    config.default_extension = {};
                    config.filter_count = 1;
                    Check(FAILED(api.ValidateConfig(session, &config)), "actual folder provider rejects file filter semantics");
                    config.filter_count = 0;
                }
                if (owner) DestroyWindow(owner);
                safe = api.Shutdown(session) == S_OK;
                Check(safe, "shutdown actual provider config probe");
            }
        }
        if (module && safe) FreeLibrary(module);
        CoUninitialize();
        std::printf("Checks=%u failures=%u\n", checks, failures);
        return failures ? 1 : 0;
    }

    if (argc != 3 && !(argc == 4 && wcscmp(argv[3], L"--interactive") == 0)) { std::printf("Usage: picker_inprocess_v2_test ABSOLUTE_UI_DLL property-edit|property-mutate|property-scroll|menu-open|computer-navigate|places-navigate|startup-visibility|startup-folder-visibility|startup-folder-close|startup-folder-cancel|startup-folder-error|ready-close|cross-thread-owner|cross-thread-accept|cross-thread-cancel|close-ok|query|reentrant-owner|custom-update|custom-inspect|initial-empty|open-extension|open-mask-sequence|labels|labels-empty|combo-select|multi-accept|multi-close|open-missing [--interactive]\n"); return 2; }
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return 2;
    const DWORD watchdog_ms = wcscmp(argv[2], L"custom-inspect") == 0 ? 180000 : 30000;
    std::thread watchdog([done, watchdog_ms] {
        if (WaitForSingleObject(done, watchdog_ms) != WAIT_OBJECT_0) {
            std::printf("WATCHDOG own proof exceeded mode budget; no pass, owned fixture retained\n");
            std::fflush(stdout); TerminateProcess(GetCurrentProcess(), 99);
        }
    });
    int result = 2;
    try { result = Run(argv[1], argv[2], argc == 4); }
    catch (...) { Check(false, "exception; owned fixture retained for diagnosis"); }
    SetEvent(done); watchdog.join(); CloseHandle(done);
    std::printf("Checks=%u failures=%u result=%d\n", checks, failures, result); std::fflush(stdout);
    return result;
}

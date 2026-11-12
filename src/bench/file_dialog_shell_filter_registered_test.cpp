#include <windows.h>
#include <windowsx.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
using Microsoft::WRL::ComPtr;
unsigned checks = 0, failures = 0;
void Check(bool value, const char* text) {
    ++checks; failures += !value;
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", text);
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
bool ModuleLoaded(DWORD pid, const std::filesystem::path& path) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry{sizeof(entry)}; bool found = false;
    if (Module32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szExePath, path.c_str()) == 0) { found = true; break; }
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot); return found;
}
struct State {
    IFileDialog* dialog = nullptr;
    IUnknown* identity = nullptr;
    DWORD thread = GetCurrentThreadId(), ui_pid = 0;
    std::filesystem::path bundle, target;
    std::wstring title;
    unsigned overwrite = 0, file_ok = 0, clicks = 0;
    unsigned include_calls=0, enum_calls=0, owned_mask=0;bool filter_thread=true, filter_error_codes=false;
    std::filesystem::path folder;
    bool callback_window_bound=false;bool stress=false,triggered=false,navigated=false,filter_cancel=false,filter_close=false;
    unsigned stress_mode=0,bulk_calls=0;
    ULONGLONG first_include=0,last_include=0,max_gap=0,trigger_tick=0;
    std::filesystem::path navigation_folder;ComPtr<IShellItem> navigation_item;
    bool cancel = false, veto = false, seen = false, native_seen = false, luma = false, expired = false;
    bool identities = true, candidates = true, bytes_unchanged = true;
    ULONGLONG started = 0, first_seen = 0, next_click = 0;
};
thread_local State* active = nullptr;
class Sink final : public IFileDialogEvents {
    std::atomic<ULONG> refs_{1}; State& state_;
    void Identity(IFileDialog* dialog) {
        ComPtr<IUnknown> identity;
        state_.identities &= dialog && dialog->QueryInterface(IID_PPV_ARGS(&identity)) == S_OK &&
            identity.Get() == state_.identity && GetCurrentThreadId() == state_.thread;
    }
public:
    explicit Sink(State& state) : state_(state) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER; *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *output = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto value = --refs_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        Identity(dialog); ++state_.file_ok;
        ComPtr<IShellItem> result; PWSTR path = nullptr;
        const HRESULT hr = dialog->GetResult(&result);
        const HRESULT path_hr = result ? result->GetDisplayName(SIGDN_FILESYSPATH, &path) : E_NOINTERFACE;
        std::error_code error;
        const bool exact = hr == S_OK && path_hr == S_OK && path &&
            std::filesystem::equivalent(path, state_.target, error) && !error;
        CoTaskMemFree(path);
        state_.candidates &= exact;
        state_.bytes_unchanged &= Read(state_.target) == "owned-existing";
        const bool veto = state_.veto && state_.file_ok == 1;
        std::printf("FILE_OK count=%u veto=%u exact=%u identity=%u original_bytes=%u\n",
            state_.file_ok, veto, exact, state_.identities, state_.bytes_unchanged);
        state_.next_click = GetTickCount64() + 500;
        return veto ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog* dialog, IShellItem*, FDE_OVERWRITE_RESPONSE* answer) override {
        if (!answer) return E_POINTER;
        Identity(dialog); ++state_.overwrite; *answer = FDEOR_ACCEPT;
        std::printf("OVERWRITE count=%u\n", state_.overwrite); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* answer) override {
        if (!answer) return E_POINTER; *answer = FDESVR_DEFAULT; return S_OK;
    }
};
class Filter final : public IShellItemFilter {
    State& state_;std::atomic<ULONG> refs_{1};
public:
    explicit Filter(State& state):state_(state){}
    ULONG References() const{return refs_.load();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown&&iid!=IID_IShellItemFilter)return E_NOINTERFACE;*out=static_cast<IShellItemFilter*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}ULONG STDMETHODCALLTYPE Release()override{return --refs_;}
    HRESULT STDMETHODCALLTYPE IncludeItem(IShellItem* item)override{
        ++state_.include_calls;APTTYPE type{};APTTYPEQUALIFIER qualifier{};const HRESULT apartment=CoGetApartmentType(&type,&qualifier);
        state_.filter_thread&=GetCurrentThreadId()==state_.thread&&apartment==S_OK&&(type==APTTYPE_STA||type==APTTYPE_MAINSTA);
        PWSTR path=nullptr;if(item)item->GetDisplayName(SIGDN_FILESYSPATH,&path);HRESULT answer=S_OK;unsigned bit=0;
        if(path){const std::filesystem::path candidate(path);if(candidate==state_.target){bit=1;answer=state_.filter_error_codes?E_ACCESSDENIED:S_FALSE;}
            else if(candidate==state_.folder/L"visible.txt"){bit=2;answer=state_.filter_error_codes?HRESULT(2):S_OK;}
            else if(candidate==state_.folder/L"blockedDir"){bit=4;answer=S_FALSE;}
            else if(candidate==state_.folder/L"allowedDir"){bit=8;answer=S_OK;}}
        state_.owned_mask|=bit;if(bit)std::printf("INCLUDE item_bit=%u returned=%08lX caller_sta=%u count=%u\n",bit,static_cast<unsigned long>(answer),state_.filter_thread,state_.include_calls);
        if(state_.stress && path && std::filesystem::path(path).filename().wstring().starts_with(L"bulk-")){
            const auto now=GetTickCount64();if(!state_.first_include)state_.first_include=now;
            if(state_.last_include)state_.max_gap=std::max(state_.max_gap,now-state_.last_include);state_.last_include=now;++state_.bulk_calls;
            if(state_.bulk_calls==1){
                ComPtr<IOleWindow> ole;HWND window=nullptr;
                if(SUCCEEDED(state_.dialog->QueryInterface(IID_PPV_ARGS(&ole)))&&SUCCEEDED(ole->GetWindow(&window))){
                    DWORD pid=0;GetWindowThreadProcessId(window,&pid);HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);wchar_t image[32768]{};DWORD units=32768;
                    state_.callback_window_bound=process&&QueryFullProcessImageNameW(process,0,image,&units)&&_wcsicmp(image,(state_.bundle/L"pulse.exe").c_str())==0&&pid!=GetCurrentProcessId();if(process)CloseHandle(process);
                    state_.ui_pid=pid;state_.luma=ModuleLoaded(pid,state_.bundle/L"lumatext.dll");std::printf("STRESS_WINDOW authenticated=%u visible=%u remote_pid=%lu luma=%u\n",state_.callback_window_bound,IsWindowVisible(window)!=FALSE,pid,state_.luma);
                }
            }
            if(state_.bulk_calls==40 && state_.stress_mode>=2){
                state_.triggered=true;state_.trigger_tick=now;
                if(state_.stress_mode==3){const auto hr=state_.dialog->SetFolder(state_.navigation_item.Get());state_.navigated=hr==S_OK;state_.folder=state_.navigation_folder;state_.target=state_.folder/L"blocked.txt";std::printf("STRESS SetFolder hr=%08lX at_item=%u\n",static_cast<unsigned long>(hr),state_.bulk_calls);}
                else if(state_.stress_mode==4){state_.filter_close=state_.dialog->Close(E_ABORT)==S_OK;std::printf("STRESS Close at_item=%u\n",state_.bulk_calls);}
                else{
                    ComPtr<IOleWindow> ole;HWND window=nullptr;if(SUCCEEDED(state_.dialog->QueryInterface(IID_PPV_ARGS(&ole)))&&SUCCEEDED(ole->GetWindow(&window))){
                        DWORD pid=0;GetWindowThreadProcessId(window,&pid);state_.filter_cancel=PostMessageW(window,WM_CLOSE,0,0)!=FALSE;
                        std::printf("STRESS Cancel posted=%u remote_pid=%lu at_item=%u\n",state_.filter_cancel,pid,state_.bulk_calls);
                    }
                }
            }
        }
        CoTaskMemFree(path);return answer;
    }
    HRESULT STDMETHODCALLTYPE GetEnumFlagsForItem(IShellItem*,SHCONTF*)override{++state_.enum_calls;return E_NOTIMPL;}
};
BOOL CALLBACK Inspect(HWND window, LPARAM) {
    if (!active || !IsWindowVisible(window)) return TRUE;
    wchar_t title[256]{}; GetWindowTextW(window, title, 256);
    if (active->title != title) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    wchar_t name[128]{}; GetClassNameW(window, name, 128);
    if (pid == GetCurrentProcessId() && wcscmp(name, L"#32770") == 0) {
        active->native_seen = true; PostMessageW(window, WM_COMMAND, IDCANCEL, 0); return TRUE;
    }
    if (pid == GetCurrentProcessId() || wcscmp(name, L"PulseFolderPickerWindow")) return TRUE;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    wchar_t image[32768]{}; DWORD units = 32768;
    const bool expected = process && QueryFullProcessImageNameW(process, 0, image, &units) &&
        _wcsicmp(image, (active->bundle / L"pulse.exe").c_str()) == 0;
    if (process) CloseHandle(process);
    if (!expected) return TRUE;
    active->seen = true; active->ui_pid = pid;
    active->luma = ModuleLoaded(pid, active->bundle / L"lumatext.dll");
    const auto now = GetTickCount64();
    if (!active->first_seen) { active->first_seen = now; active->next_click = now + 1000; }
    const unsigned limit = active->veto ? 2u : 1u;
    if (!active->luma || now < active->next_click || active->clicks >= limit) return TRUE;
    if (active->veto && active->clicks == 1 && active->file_ok != 1) return TRUE;
    using Dpi = UINT(WINAPI*)(HWND);
    const auto dpi = reinterpret_cast<Dpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const float scale = dpi ? dpi(window) / 96.0f : 1.0f;
    RECT client{}; GetClientRect(window, &client);
    const int x = client.right - static_cast<int>((active->cancel ? 204.0f : 76.0f) * scale);
    const int y = client.bottom - static_cast<int>(30.0f * scale);
    ++active->clicks; active->next_click = now + 500;
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    PostMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    return TRUE;
}
void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    if (!active) return;
    EnumWindows(Inspect, 0);
    if (GetTickCount64() - active->started > 20000) {
        active->expired = true; active->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
}
DWORD WINAPI Watchdog(void* event) {
    if (WaitForSingleObject(event, 120000) == WAIT_TIMEOUT) ExitProcess(124);
    return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    const bool stress=argc==4&&std::wstring(argv[3])==L"--stress";
    if (argc != 3&&!stress) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using SetDpi = BOOL(WINAPI*)(HANDLE);
    const auto set_dpi = reinterpret_cast<SetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_dpi && !set_dpi(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) return 2;
    const std::filesystem::path bundle(argv[1]), root(argv[2]);
    if (!bundle.is_absolute() || !root.is_absolute() || std::filesystem::exists(root)) return 2;
    std::filesystem::create_directory(root);
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", root.c_str());
    const auto target = root / L"blocked.txt";
    std::filesystem::create_directory(root/L"allowedDir");std::filesystem::create_directory(root/L"blockedDir");
    {std::ofstream visible(root/L"visible.txt");visible<<"visible-owned";}
    { std::ofstream file(target, std::ios::binary); file << "owned-existing"; }
    GUID nonce{}; if (FAILED(CoCreateGuid(&nonce))) return 2;
    wchar_t nonce_text[64]{}; StringFromGUID2(nonce, nonce_text, 64);
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment)) return 2;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE watchdog = stop ? CreateThread(nullptr, 0, Watchdog, stop, 0, nullptr) : nullptr;
    if (!watchdog) return 2;
    std::printf("REGISTERED_SHELL_FILTER caller_pid=%lu arch=%zu cases=5 bundle=%ls\n", GetCurrentProcessId(), sizeof(void*) * 8, bundle.c_str());
    for (unsigned scenario = 0; scenario < 5u; ++scenario) {
        State state; state.bundle = bundle; state.target = target;state.folder=root;state.filter_error_codes=scenario==4;Filter filter(state);
        state.cancel = stress?scenario==2:(scenario == 1 || scenario==3); state.veto = false;
        const bool prompt = false;const bool save=!stress&&scenario==2;
        if(stress){
            state.stress=true;state.stress_mode=scenario;state.folder=root/(L"stress-"+std::to_wstring(scenario));state.target=state.folder/L"blocked.txt";
            std::filesystem::create_directory(state.folder);std::filesystem::create_directory(state.folder/L"allowedDir");std::filesystem::create_directory(state.folder/L"blockedDir");
            for(const auto* leaf:{L"blocked.txt",L"visible.txt"}){std::ofstream file(state.folder/leaf);file<<"owned-existing";}
            const unsigned count=scenario==0||scenario==3?300u:1000u;
            for(unsigned i=0;i<count;++i){std::ofstream file(state.folder/(L"bulk-"+std::to_wstring(i)+L".txt"));file<<"owned-bulk";}
            state.navigation_folder=state.folder/L"destination";std::filesystem::create_directory(state.navigation_folder);{std::ofstream file(state.navigation_folder/L"blocked.txt");file<<"owned-existing";}
            SHCreateItemFromParsingName(state.navigation_folder.c_str(),nullptr,IID_PPV_ARGS(&state.navigation_item));
        }
        ComPtr<IFileDialog> dialog; ComPtr<IShellItem> folder; ComPtr<IUnknown> identity;
        HRESULT hr = CoCreateInstance(save?CLSID_FileSaveDialog:CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        const auto shim = sizeof(void*) == 4 ? L"PulseDialogAuto32.dll" : L"PulseDialogAuto64.dll";
        Check(hr == S_OK && dialog && ModuleLoaded(GetCurrentProcessId(), bundle / shim), "ordinary CoCreateInstance activates exact registered architecture-matched bundle");
        if (FAILED(hr) || !dialog) break;
        hr = dialog->QueryInterface(IID_PPV_ARGS(&identity));
        if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(state.folder.c_str(), nullptr, IID_PPV_ARGS(&folder));
        const DWORD options = FOS_FORCEFILESYSTEM | (save?FOS_NOREADONLYRETURN:FOS_FILEMUSTEXIST) | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT | (prompt ? FOS_OVERWRITEPROMPT : 0u);
        if (SUCCEEDED(hr)) hr = dialog->SetOptions(options);
        if (SUCCEEDED(hr)) hr = dialog->SetFolder(folder.Get());
        if (SUCCEEDED(hr)) hr = dialog->SetFileName(L"blocked.txt");
        state.title = L"Pulse Registered ShellFilter " + std::to_wstring(GetCurrentProcessId()) + L" " + nonce_text + L" Case " + std::to_wstring(scenario);
        if (SUCCEEDED(hr)) hr = dialog->SetTitle(state.title.c_str());
        FILEOPENDIALOGOPTIONS actual{};
        if (SUCCEEDED(hr)) hr = dialog->GetOptions(&actual);
        Check(hr == S_OK && actual == options && static_cast<bool>(actual & FOS_OVERWRITEPROMPT) == prompt, "exact prompt/no-prompt flags survive ordinary wrapper setters");
        state.dialog = dialog.Get(); state.identity = identity.Get();
        ComPtr<IFileDialogEvents> sink; sink.Attach(new Sink(state)); DWORD cookie = 0;
        if (SUCCEEDED(hr)) hr = dialog->Advise(sink.Get(), &cookie);
        if(SUCCEEDED(hr))hr=dialog->SetFilter(&filter);
        Check(hr==S_OK&&filter.References()==2,"ordinary SetFilter retains exactly one native-owned filter reference");
        if(scenario==3&&!stress){hr=dialog->SetFilter(nullptr);Check(hr==S_OK&&filter.References()==1,"clear removes filter obligation before Show");}
        state.started = GetTickCount64(); active = &state;
        const UINT_PTR timer = SUCCEEDED(hr) ? SetTimer(nullptr, 0, 50, Tick) : 0;
        if (timer) hr = dialog->Show(nullptr);
        if (timer) KillTimer(nullptr, timer); active = nullptr;
        if(!stress&&scenario==1&&timer&&hr==HRESULT_FROM_WIN32(ERROR_CANCELLED)){
            state.seen=false;state.native_seen=false;state.luma=false;state.clicks=0;state.first_seen=0;state.next_click=0;state.expired=false;
            state.title+=L" Repeat";dialog->SetTitle(state.title.c_str());state.started=GetTickCount64();active=&state;
            const auto repeated=SetTimer(nullptr,0,50,Tick);hr=dialog->Show(nullptr);KillTimer(nullptr,repeated);active=nullptr;
            Check(hr==HRESULT_FROM_WIN32(ERROR_CANCELLED),"same ordinary COM object supports a second filtered remote Show");
        }
        if (cookie) dialog->Unadvise(cookie);
        ComPtr<IShellItem> result;
        const HRESULT result_hr = dialog->GetResult(&result);
        bool exact = false;
        if (result) {
            PWSTR path = nullptr;
            if (result->GetDisplayName(SIGDN_FILESYSPATH, &path) == S_OK && path) {
                std::error_code error; exact = std::filesystem::equivalent(path, state.target, error) && !error;
            }
            CoTaskMemFree(path);
        }
        std::printf("CASE scenario=%u prompt=%u cancel=%u veto=%u hr=%08lX result_hr=%08lX overwrite=%u file_ok=%u clicks=%u remote=%u ui_pid=%lu caller_pid=%lu luma=%u native=%u expired=%u exact=%u\n",
            scenario, prompt, state.cancel, state.veto, static_cast<unsigned long>(hr), static_cast<unsigned long>(result_hr), state.overwrite, state.file_ok,
            state.clicks, state.seen && state.ui_pid != GetCurrentProcessId(), state.ui_pid, GetCurrentProcessId(), state.luma, state.native_seen, state.expired, exact);
        Check(timer && (stress?state.callback_window_bound:state.seen) && state.ui_pid != GetCurrentProcessId() && state.luma && !state.native_seen && !state.expired,
            stress?"authenticated callback window binds real remote pulse.exe and matching-bundle lumatext":"unique title binds real remote pulse.exe and matching-bundle lumatext");
        Check(hr == (stress&&scenario==4?E_ABORT:state.cancel ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK) && state.overwrite == (prompt ? 1u : 0u) &&
            state.file_ok == ((stress&&scenario==4)||state.cancel ? 0u : state.veto ? 2u : 1u), "ordinary remote chain preserves no-prompt overwrite count, veto and cancellation");
        std::printf("FILTER_SUMMARY case=%u includes=%u enum=%u mask=%u caller_sta=%u refs=%lu error_codes=%u\n",scenario,state.include_calls,state.enum_calls,state.owned_mask,state.filter_thread,filter.References(),state.filter_error_codes);
        Check(state.filter_thread&&state.enum_calls==0&&(stress?state.bulk_calls>0:(scenario==3?state.include_calls==0:state.owned_mask==15)),"actual remote enumeration calls IncludeItem for owned files/folders on caller STA and never enum flags");
        Check(state.identities && state.candidates && state.bytes_unchanged && Read(state.target) == "owned-existing", "callbacks retain wrapper identity and selection never modifies existing bytes");
        Check(state.cancel || (stress&&scenario==4) ? FAILED(result_hr) && !result : result_hr == S_OK && exact, "ordinary remote result is exact owned fixture or cleared on cancellation");
        if (!state.cancel && hr == S_OK && exact) {
            { std::ofstream file(state.target, std::ios::binary | std::ios::trunc); file << "registered-host-roundtrip"; }
            Check(Read(state.target) == "registered-host-roundtrip", "host writes and reopens the exact selected file");
            { std::ofstream file(state.target, std::ios::binary | std::ios::trunc); file << "owned-existing"; }
        }
        if(stress){
            const auto returned=GetTickCount64();
            std::printf("STRESS_RESULT case=%u items=%u include_span_ms=%llu show_ms=%llu max_gap_ms=%llu trigger_ack_ms=%llu triggered=%u navigated=%u cancel=%u close=%u hr=%08lX\n",scenario,state.bulk_calls,state.last_include-state.first_include,returned-state.started,state.max_gap,state.trigger_tick?returned-state.trigger_tick:0,state.triggered,state.navigated,state.filter_cancel,state.filter_close,static_cast<unsigned long>(hr));
            Check(scenario<=1?state.bulk_calls==(scenario==0?300u:1000u):state.triggered,"actual remote listing traverses 300 items or delivers the in-filter interaction");
            if(scenario==3)Check(state.navigated&&exact&&hr==S_OK,"callback SetFolder discards stale batch and accepts only destination fixture");
            if(scenario==2||scenario==4)Check(returned-state.trigger_tick<3000,"actual remote cancel/Close completes promptly during 1000-item filtering");
        }
        identity.Reset();dialog.Reset();Check(filter.References()==1,"ordinary COM final Release balances filter lifetime");
    }
    SetEvent(stop); WaitForSingleObject(watchdog, INFINITE); CloseHandle(watchdog); CloseHandle(stop);
    CoUninitialize();
    Check(Read(target) == "owned-existing", "owned fixture bytes restored after host roundtrip");
    std::filesystem::remove(target);std::filesystem::remove(root/L"visible.txt");std::filesystem::remove(root/L"allowedDir");std::filesystem::remove(root/L"blockedDir");
    std::printf("REGISTERED_SHELL_FILTER Checks=%u failures=%u registration_mutations=0\n", checks, failures);
    return failures ? 1 : 0;
}

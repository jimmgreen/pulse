#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

// Observation only: programmatic WM_COMMAND is not physical keyboard input.
// OnFileOk S_FALSE retains the dialog; GetResult is valid inside OnFileOk:
// https://learn.microsoft.com/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifiledialogevents-onfileok
namespace {
using Microsoft::WRL::ComPtr;
constexpr HRESULT kCloseResult = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x321);
// One STA writer, immutable records after release publication. The watchdog
// reads only the acquire-published prefix; records are never overwritten.
struct Record { char text[2048]{}; };
Record records[2048];
std::atomic_flag tracing = ATOMIC_FLAG_INIT;
std::atomic<unsigned> published{0}, dropped{0};
std::atomic<unsigned> last_stage{0};
std::atomic<ULONGLONG> last_stage_tick{0};
void Trace(const char* format, ...) {
    if (tracing.test_and_set(std::memory_order_acquire)) { ++dropped; return; }
    struct Unlock { ~Unlock() { tracing.clear(std::memory_order_release); } } unlock;
    const unsigned index = published.load(std::memory_order_relaxed);
    if (index >= 2048) { ++dropped; return; }
    va_list args; va_start(args, format);
    const int count = vsnprintf(records[index].text, sizeof(records[index].text), format, args);
    va_end(args);
    if (count < 0 || static_cast<size_t>(count) >= sizeof(records[index].text)) ++dropped;
    published.store(index + 1, std::memory_order_release);
}
void Dump() {
    const unsigned count = published.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) std::fputs(records[i].text, stdout);
    std::printf("DIAGNOSTICS published=%u dropped_or_truncated=%u last_stage=%u stage_tick=%llu now=%llu\n",
        count, dropped.load(), last_stage.load(), last_stage_tick.load(), GetTickCount64());
    std::fflush(stdout);
}
// Stage IDs: 1 timer, 2 QI/window, 3 GetFolder, 4 GetCurrentSelection,
// 5 GetResult, 6 GetFileName, 7 GetFileTypeIndex, 8 GetDisplayName,
// 9 programmatic setters, 10 Close, 11 Show, 12 callback completed.
void Stage(unsigned stage) {
    last_stage_tick.store(GetTickCount64()); last_stage.store(stage);
    Trace("STAGE id=%u tick=%llu\n", stage, GetTickCount64());
}
int checks = 0, failures = 0;
void Check(bool ok, const char* name) {
    ++checks; if (!ok) ++failures;
    Trace("[%s] %s\n", ok ? "PASS" : "FAIL", name);
}
void Item(const char* label, HRESULT hr, IShellItem* item) {
    PWSTR path = nullptr;
    Stage(8); const ULONGLONG began = GetTickCount64();
    const HRESULT name_hr = item ? item->GetDisplayName(SIGDN_FILESYSPATH, &path) : E_POINTER;
    Trace(" getter_display_ms=%llu", GetTickCount64() - began);
    Trace(" %s_hr=%08lX name_hr=%08lX", label, static_cast<unsigned long>(hr), static_cast<unsigned long>(name_hr));
    if (path) {
        char utf8[4096]{};
        WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
        Trace(" path=[%s]", utf8);
    }
    CoTaskMemFree(path);
}
struct Case {
    IFileOpenDialog* dialog = nullptr;
    HWND owner = nullptr, first_window = nullptr, veto_window = nullptr;
    DWORD sta = 0;
    ULONGLONG start = 0, action_at = 0;
    std::wstring folder, expected;
    unsigned sequence = 0, file_ok = 0, folder_changing = 0, folder_change = 0, selection = 0, type = 0;
    int phase = 0;
    bool ticking = false;
    unsigned callback_depth = 0;
    bool close_case = false, timed_out = false, veto_retained = false, wrong_thread = false, candidate_ok = true;
    bool callback_close = false, close_attempted = false;
    HRESULT close_hr = E_PENDING, timeout_close_hr = E_PENDING;
    HWND Window() const {
        Stage(2);
        ComPtr<IOleWindow> ole;
        HWND window = nullptr;
        const ULONGLONG began = GetTickCount64();
        const HRESULT qi = dialog->QueryInterface(IID_PPV_ARGS(&ole));
        const HRESULT got = ole ? ole->GetWindow(&window) : E_NOINTERFACE;
        Trace("Window QI=%08lX GetWindow=%08lX ms=%llu\n", static_cast<unsigned long>(qi), static_cast<unsigned long>(got), GetTickCount64() - began);
        if (FAILED(qi) || FAILED(got)) return nullptr;
        DWORD pid = 0;
        const DWORD tid = GetWindowThreadProcessId(window, &pid);
        if (!window || pid != GetCurrentProcessId() || tid != sta || GetWindow(window, GW_OWNER) != owner) return nullptr;
        return window;
    }
    void Observe(const char* event, IShellItem* proposed = nullptr) {
        ++callback_depth;
        struct CallbackExit { Case* state; ~CallbackExit() { --state->callback_depth; Stage(12); } } callback_exit{this};
        Trace("CALLBACK enter depth=%u tick=%llu\n", callback_depth, GetTickCount64());
        wrong_thread |= GetCurrentThreadId() != sta;
        HWND hwnd = Window();
        ComPtr<IOleWindow> ole;
        HWND raw_window = nullptr;
        const HRESULT qi = dialog->QueryInterface(IID_PPV_ARGS(&ole));
        const HRESULT window_hr = ole ? ole->GetWindow(&raw_window) : E_NOINTERFACE;
        Trace("WINDOW qi=%08lX get=%08lX raw=%p identity_verified=%d\n",
            static_cast<unsigned long>(qi), static_cast<unsigned long>(window_hr),
            static_cast<void*>(raw_window), hwnd != nullptr);
        Trace("EVENT seq=%u tick=%llu name=%s thread=%lu show_sta=%lu hwnd=%p\n",
            ++sequence, GetTickCount64(), event, GetCurrentThreadId(), sta, static_cast<void*>(hwnd));
        ComPtr<IShellItem> item;
        Stage(3); const ULONGLONG folder_at = GetTickCount64();
        HRESULT hr = dialog->GetFolder(&item); Trace("GetFolder ms=%llu\n", GetTickCount64() - folder_at); Item("folder", hr, item.Get()); item.Reset();
        Stage(4); const ULONGLONG selection_at = GetTickCount64();
        hr = dialog->GetCurrentSelection(&item); Trace("GetCurrentSelection ms=%llu\n", GetTickCount64() - selection_at); Item("selection", hr, item.Get()); item.Reset();
        Stage(5); const ULONGLONG result_at = GetTickCount64();
        hr = dialog->GetResult(&item); Trace("GetResult ms=%llu\n", GetTickCount64() - result_at); Item("result", hr, item.Get());
        Stage(6); const ULONGLONG filename_at = GetTickCount64();
        PWSTR filename = nullptr; const HRESULT filename_hr = dialog->GetFileName(&filename);
        Trace("GetFileName ms=%llu\n", GetTickCount64() - filename_at);
        char utf8[4096]{};
        if (filename) WideCharToMultiByte(CP_UTF8, 0, filename, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
        Trace(" filename_hr=%08lX filename=[%s]", static_cast<unsigned long>(filename_hr), utf8);
        CoTaskMemFree(filename);
        Stage(7); const ULONGLONG index_at = GetTickCount64();
        UINT index = 0; hr = dialog->GetFileTypeIndex(&index);
        Trace("GetFileTypeIndex ms=%llu\n", GetTickCount64() - index_at);
        Trace(" index_hr=%08lX index=%u", static_cast<unsigned long>(hr), index);
        if (proposed) Item("proposed", S_OK, proposed);
        Trace("\n");
    }
    void Tick() {
        if (ticking || callback_depth) { Trace("TIMER reentry suppressed tick=%llu phase=%d callback_depth=%u\n", GetTickCount64(), phase, callback_depth); return; }
        ticking = true;
        struct TickExit { Case* state; ~TickExit() { state->ticking = false; Trace("TIMER exit tick=%llu phase=%d\n", GetTickCount64(), state->phase); } } tick_exit{this};
        Stage(1);
        Trace("TIMER enter tick=%llu elapsed=%llu phase=%d callback_depth=%u\n", GetTickCount64(), GetTickCount64() - start, phase, callback_depth);
        HWND hwnd = Window();
        Trace("TIMER Window returned tick=%llu hwnd=%p\n", GetTickCount64(), static_cast<void*>(hwnd));
        if (GetTickCount64() - start > 20000) {
            if (!timed_out) {
                timed_out = true; Stage(10);
                Trace("TIMEOUT Close begin tick=%llu\n", GetTickCount64());
                timeout_close_hr = dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
                Trace("TIMEOUT Close end tick=%llu hr=%08lX\n", GetTickCount64(), static_cast<unsigned long>(timeout_close_hr));
            }
            return;
        }
        if (!hwnd || !IsWindowVisible(hwnd)) return;
        if (!first_window) first_window = hwnd;
        if (hwnd != first_window) { timed_out = true; dialog->Close(E_UNEXPECTED); return; }
        if (close_case) {
            if (callback_close) return; // Only the real OnFolderChange callback drives this variant.
            if (phase++ == 0) {
                close_attempted = true;
                close_hr = dialog->Close(kCloseResult);
                Trace("ACTION Close specified=%08lX call_hr=%08lX\n", static_cast<unsigned long>(kCloseResult), static_cast<unsigned long>(close_hr));
            }
            return;
        }
        if (phase == 0) {
            phase = 1; action_at = GetTickCount64(); Stage(9);
            ComPtr<IShellItem> target;
            HRESULT hr = SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&target));
            if (SUCCEEDED(hr)) hr = dialog->SetFolder(target.Get());
            const HRESULT filter = dialog->SetFileTypeIndex(2);
            const HRESULT name = dialog->SetFileName(L"selected.txt");
            Trace("ACTION programmatic SetFolder=%08lX SetType2=%08lX SetName=%08lX\n",
                static_cast<unsigned long>(hr), static_cast<unsigned long>(filter), static_cast<unsigned long>(name));
            Check(SUCCEEDED(hr) && SUCCEEDED(filter) && SUCCEEDED(name), "programmatic navigation setup");
            action_at = GetTickCount64();
        } else if (phase == 1 && GetTickCount64() - action_at >= 500) {
            phase = 2;
            SetLastError(ERROR_SUCCESS);
            const BOOL posted = PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
            Trace("ACTION programmatic submit1 posted=%d error=%lu (not acceptance evidence)\n", posted, GetLastError());
        } else if (phase == 2 && file_ok == 1) {
            veto_retained = hwnd == veto_window && IsWindow(hwnd);
            phase = 3;
            SetLastError(ERROR_SUCCESS);
            const BOOL posted = PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
            Trace("ACTION programmatic submit2 same_veto_window=%d posted=%d error=%lu", veto_retained, posted, GetLastError());
            Trace("\n");
        }
    }
};
thread_local Case* active = nullptr;
void CALLBACK Timer(HWND, UINT, UINT_PTR, DWORD) {
    try { if (active) active->Tick(); }
    catch (...) { if (active) { active->timed_out = true; active->dialog->Close(E_UNEXPECTED); } }
}
class Events final : public IFileDialogEvents {
public:
    explicit Events(Case& state) : state_(state) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IFileDialogEvents) { *out = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG count = --refs_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        state_.Observe("FileOk"); ++state_.file_ok;
        ComPtr<IShellItem> item; PWSTR path = nullptr;
        const HRESULT hr = dialog->GetResult(&item);
        const HRESULT named = item ? item->GetDisplayName(SIGDN_FILESYSPATH, &path) : E_POINTER;
        state_.candidate_ok &= SUCCEEDED(hr) && SUCCEEDED(named) && path && _wcsicmp(path, state_.expected.c_str()) == 0;
        CoTaskMemFree(path);
        if (state_.file_ok == 1) { state_.veto_window = state_.Window(); Trace("RETURN FileOk S_FALSE\n"); return S_FALSE; }
        Trace("RETURN FileOk S_OK\n"); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem* item) override { ++state_.folder_changing; state_.Observe("FolderChanging", item); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* pfd) override {
        ++state_.callback_depth;
        struct CallbackExit { Case& state; ~CallbackExit() { --state.callback_depth; } } callback_exit{state_};
        ++state_.folder_change; state_.Observe("FolderChange");
        if (state_.callback_close && !state_.close_attempted) {
            const HWND window = state_.Window();
            DWORD pid = 0; const DWORD thread = GetWindowThreadProcessId(window, &pid);
            Trace("CALLBACK Close before tick=%llu hwnd=%p visible=%d enabled=%d owner=%p pid=%lu tid=%lu sta=%lu pfd=%p\n",
                GetTickCount64(), static_cast<void*>(window), IsWindowVisible(window), IsWindowEnabled(window),
                static_cast<void*>(GetWindow(window, GW_OWNER)), pid, thread, state_.sta, static_cast<void*>(pfd));
            if (window && pfd) {
                state_.first_window = window;
                state_.close_attempted = true; Stage(10);
                state_.close_hr = pfd->Close(kCloseResult);
                Trace("CALLBACK Close after tick=%llu specified=%08lX call_hr=%08lX\n", GetTickCount64(),
                    static_cast<unsigned long>(kCloseResult), static_cast<unsigned long>(state_.close_hr));
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { ++state_.selection; state_.Observe("SelectionChange"); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { ++state_.type; state_.Observe("TypeChange"); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem* item, FDE_SHAREVIOLATION_RESPONSE* response) override {
        state_.Observe("ShareViolation", item); if (!response) return E_POINTER; *response = FDESVR_DEFAULT; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem* item, FDE_OVERWRITE_RESPONSE* response) override {
        state_.Observe("Overwrite", item); if (!response) return E_POINTER; *response = FDEOR_DEFAULT; return S_OK;
    }
private:
    std::atomic<ULONG> refs_{1};
    Case& state_;
};
void Run(HWND owner, const std::wstring& initial, const std::wstring& destination, bool close_case, bool callback_close = false) {
    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    Check(SUCCEEDED(hr), "raw native CoCreateInstance"); if (FAILED(hr)) return;
    GUID guid{}; hr = CoCreateGuid(&guid);
    if (SUCCEEDED(hr)) hr = dialog->SetClientGuid(guid);
    const COMDLG_FILTERSPEC filters[]{{L"All files", L"*.*"}, {L"Text", L"*.txt"}};
    if (SUCCEEDED(hr)) hr = dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
    if (SUCCEEDED(hr)) hr = dialog->SetFileTypes(2, filters);
    if (SUCCEEDED(hr)) hr = dialog->SetFileTypeIndex(1);
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder));
    if (SUCCEEDED(hr)) hr = dialog->SetFolder(folder.Get());
    Trace("CASE %s setup_hr=%08lX\n", callback_close ? "close_callback_hresult" : close_case ? "close_hresult" : "veto_accept", static_cast<unsigned long>(hr));
    Check(SUCCEEDED(hr), "native configuration");
    if (SUCCEEDED(hr)) {
        Case state; state.dialog = dialog.Get(); state.owner = owner; state.sta = GetCurrentThreadId();
        state.folder = destination; state.expected = destination + L"\\selected.txt"; state.close_case = close_case; state.callback_close = callback_close;
        ComPtr<IFileDialogEvents> sink; sink.Attach(new Events(state));
        DWORD cookie = 0; hr = dialog->Advise(sink.Get(), &cookie);
        Check(SUCCEEDED(hr), "Advise");
        if (SUCCEEDED(hr)) {
            active = &state; state.start = GetTickCount64();
            const UINT_PTR timer = SetTimer(nullptr, 0, 100, Timer);
            Check(timer != 0, "STA timer");
            if (timer) {
                Stage(11);
                const HRESULT shown = dialog->Show(owner);
                KillTimer(nullptr, timer);
                Trace("SHOW result=%08lX timeout=%d\n", static_cast<unsigned long>(shown), state.timed_out);
                Check(!state.timed_out && state.first_window, "actual owned dialog observed within budget");
                Check(!state.wrong_thread, "callbacks execute on Show STA");
                Check(IsWindowEnabled(owner) != FALSE, "owner enabled after Show");
                ComPtr<IShellItem> final_item;
                const HRESULT final_hr = dialog->GetResult(&final_item);
                Item("after_Show_result", final_hr, final_item.Get()); Trace("\n");
                if (close_case) Check(FAILED(final_hr) && !final_item, "Close does not publish a result");
                if (close_case) Check(state.close_attempted && shown == kCloseResult && SUCCEEDED(state.close_hr), "Close HRESULT returned unchanged by Show");
                else {
                    Check(shown == S_OK && state.file_ok == 2 && state.candidate_ok, "veto then accept actual selected result");
                    Check(state.veto_retained && state.veto_window == state.first_window, "S_FALSE preserves same actual dialog");
                }
                Trace("OBSERVED FolderChanging=%u FolderChange=%u SelectionChange=%u TypeChange=%u FileOk=%u; zero means NOT_OBSERVED, order not asserted\n",
                    state.folder_changing, state.folder_change, state.selection, state.type, state.file_ok);
            }
            active = nullptr;
            Check(SUCCEEDED(dialog->Unadvise(cookie)), "Unadvise");
        }
    }
    const HRESULT cleared = dialog->ClearClientData();
    Trace("CLEANUP ClearClientData=%08lX\n", static_cast<unsigned long>(cleared));
    Check(SUCCEEDED(cleared), "clear unique client data");
}
}
int wmain(int argc, wchar_t** argv) {
    const bool close_only = argc == 2 && std::wstring(argv[1]) == L"--close";
    if (argc != 1 && !close_only) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return 2;
    std::thread watchdog([done] {
        if (WaitForSingleObject(done, 45000) != WAIT_OBJECT_0) {
            Dump();
            std::printf("WATCHDOG own process exceeded 45 seconds; fixture retained for diagnosis\n");
            std::fflush(stdout); TerminateProcess(GetCurrentProcess(), 99);
        }
    });
    int result = 2;
    try {
        const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") / (L"native-events-" + suffix));
        std::filesystem::create_directories(root.parent_path());
        if (!CreateDirectoryW(root.c_str(), nullptr)) throw GetLastError();
        const auto a = root / L"a", b = root / L"b", file = b / L"selected.txt";
        Check(CreateDirectoryW(a.c_str(), nullptr) != FALSE && CreateDirectoryW(b.c_str(), nullptr) != FALSE, "exclusive fixture directories");
        HANDLE fixture = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(fixture != INVALID_HANDLE_VALUE, "exclusive owned selection file");
        if (fixture != INVALID_HANDLE_VALUE) CloseHandle(fixture);
        HDESK previous = GetThreadDesktop(GetCurrentThreadId());
        HDESK desktop = CreateDesktopW((L"PulseNativeEvents-" + suffix).c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (desktop && SetThreadDesktop(desktop)) {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            Check(SUCCEEDED(com), "private desktop STA initialized (never SwitchDesktop)");
            if (SUCCEEDED(com)) {
                HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse owned events probe", WS_OVERLAPPED, 0, 0, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                Check(owner != nullptr, "private desktop owner");
                if (owner) {
                    if (close_only) Run(owner, a.wstring(), b.wstring(), true, true);
                    else { Run(owner, a.wstring(), b.wstring(), false); Run(owner, a.wstring(), b.wstring(), true); }
                    DestroyWindow(owner);
                }
                CoUninitialize();
            }
            Check(SetThreadDesktop(previous) != FALSE, "restore test thread desktop");
        } else Check(false, "private desktop creation/switch");
        if (desktop) Check(CloseDesktop(desktop) != FALSE, "close private desktop");
        Check(DeleteFileW(file.c_str()) != FALSE, "delete own fixture file");
        Check(RemoveDirectoryW(a.c_str()) && RemoveDirectoryW(b.c_str()) && RemoveDirectoryW(root.c_str()), "remove own empty fixture directories");
        result = failures ? 1 : 0;
    } catch (...) { Check(false, "probe setup exception; owned fixture retained"); }
    SetEvent(done); watchdog.join(); CloseHandle(done);
    Trace("Checks=%d failures=%d result=%d\n", checks, failures, result);
    Dump();
    return result;
}

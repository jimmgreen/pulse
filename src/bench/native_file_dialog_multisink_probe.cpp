#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

// Controlled WM_COMMAND is not physical keyboard evidence. One case per process.
namespace {
using Microsoft::WRL::ComPtr;
constexpr HRESULT kPending = E_PENDING;
unsigned functional_failures = 0, cleanup_failures = 0;
void Check(bool value, const char* label, bool cleanup = false) {
    if (!value) ++(cleanup ? cleanup_failures : functional_failures);
    std::printf("[%s] %s %s\n", value ? "PASS" : "FAIL", cleanup ? "cleanup" : "functional", label);
}
enum class Mode { FirstVeto, SecondVeto, SelfUnadvise, OtherUnadvise, NotImplFirst, NotImplSecond, TypeUnexpected, TypeFail };
bool NotImplMode(Mode mode) { return mode == Mode::NotImplFirst || mode == Mode::NotImplSecond; }
bool TypeFailureMode(Mode mode) { return mode == Mode::TypeUnexpected || mode == Mode::TypeFail; }
struct State {
    IFileOpenDialog* dialog = nullptr;
    HWND owner = nullptr, window = nullptr;
    DWORD thread = 0, cookies[2]{};
    bool advised[2]{}, timer_active = false, retained = false, timeout = false;
    bool identity_ok = true, candidate_ok = true, test_cancel = false;
    unsigned notifications[3][2]{}, first_type_sequence[2]{};
    bool visible_observed = false, cancel_posted = false;
    Mode mode = Mode::FirstVeto;
    unsigned attempt = 0, sequence = 0, callback_depth = 0, calls[2][2]{};
    HRESULT unadvise_hr = kPending;
    ULONGLONG started = 0, sent = 0;
    std::wstring expected;
    HWND Window() const {
        ComPtr<IOleWindow> ole; HWND hwnd = nullptr; DWORD pid = 0;
        if (FAILED(dialog->QueryInterface(IID_PPV_ARGS(&ole))) || FAILED(ole->GetWindow(&hwnd))) return nullptr;
        if (!hwnd || GetWindowThreadProcessId(hwnd, &pid) != thread || pid != GetCurrentProcessId() ||
            GetWindow(hwnd, GW_OWNER) != owner) return nullptr;
        return hwnd;
    }
    void Tick() {
        if (timer_active || callback_depth) return;
        timer_active = true;
        struct End { bool& value; ~End() { value = false; } } end{timer_active};
        HWND actual = Window();
        if (GetTickCount64() - started >= 20000) {
            if (!timeout) {
                timeout = true;
                const BOOL posted = actual && PostMessageW(actual, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                std::printf("TIMEOUT cancel_posted=%d tick=%llu (not success)\n", posted, GetTickCount64());
            }
            return;
        }
        if (!actual || !IsWindowVisible(actual) || !IsWindowEnabled(actual)) return;
        if (!window) window = actual;
        if (actual != window) { Check(false, "dialog HWND changed"); timeout = true; return; }
        if (TypeFailureMode(mode)) {
            visible_observed = true;
            // Notification evidence is independent of cancellation. Never submit
            // FileOk in these modes, and never treat the timeout as success.
            if (!test_cancel && notifications[2][0] && notifications[2][1]) {
                test_cancel = true;
                cancel_posted = PostMessageW(actual, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0) != FALSE;
                std::printf("TYPE_CANCEL visible=1 hwnd=%p posted=%d typeorder=[%u,%u]\n",
                    static_cast<void*>(actual), cancel_posted, first_type_sequence[0], first_type_sequence[1]);
                std::fflush(stdout);
            }
            return;
        }
        if (NotImplMode(mode) && attempt == 1) {
            if (!test_cancel && GetTickCount64() - sent >= 1000) {
                retained = true; test_cancel = true;
                const BOOL posted = PostMessageW(actual, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                std::printf("OBSERVATION notimpl remains-open; test_cancel_posted=%d (not acceptance)\n", posted);
                Check(posted != FALSE, "bounded cancellation of observed retained dialog");
            }
            return;
        }
        if (!attempt || (attempt == 1 && (calls[0][0] + calls[0][1]) && GetTickCount64() - sent >= 700)) {
            if (attempt == 1) retained = true;
            ++attempt; sent = GetTickCount64();
            SetLastError(ERROR_SUCCESS);
            const BOOL posted = PostMessageW(actual, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
            std::printf("SUBMIT attempt=%u posted=%d error=%lu hwnd=%p tick=%llu\n", attempt, posted,
                posted ? ERROR_SUCCESS : GetLastError(), static_cast<void*>(actual), sent);
            Check(posted != FALSE, "post to verified owned native dialog");
        }
    }
};
thread_local State* active = nullptr;
void CALLBACK Timer(HWND, UINT, UINT_PTR, DWORD) { if (active) active->Tick(); }
class Sink final : public IFileDialogEvents {
    std::atomic<ULONG> references_{1};
    State& state_;
    unsigned index_;
public:
    Sink(State& state, unsigned index) : state_(state), index_(index) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *output = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        ++state_.callback_depth;
        struct CallbackEnd { State& state; ~CallbackEnd() { --state.callback_depth; } } callback_end{state_};
        const unsigned attempt = state_.attempt;
        HWND window = state_.Window();
        state_.identity_ok &= GetCurrentThreadId() == state_.thread && window == state_.window && window != nullptr;
        if (attempt < 1 || attempt > 2) { Check(false, "FileOk without controlled submission"); return S_FALSE; }
        ++state_.calls[attempt - 1][index_];
        ComPtr<IShellItem> item; PWSTR path = nullptr;
        const HRESULT result = dialog->GetResult(&item);
        const HRESULT name = item ? item->GetDisplayName(SIGDN_FILESYSPATH, &path) : E_POINTER;
        state_.candidate_ok &= SUCCEEDED(result) && SUCCEEDED(name) && path && _wcsicmp(path, state_.expected.c_str()) == 0;
        CoTaskMemFree(path);
        HRESULT answer = S_OK;
        if (NotImplMode(state_.mode)) {
            answer = NotImplementedSink() ? E_NOTIMPL : S_OK;
        } else if (attempt == 1) {
            const unsigned veto = state_.mode == Mode::SecondVeto ? 1u : 0u;
            if (index_ == veto) answer = S_FALSE;
            if (index_ == 0 && (state_.mode == Mode::SelfUnadvise || state_.mode == Mode::OtherUnadvise)) {
                const unsigned removed = state_.mode == Mode::SelfUnadvise ? 0u : 1u;
                state_.unadvise_hr = dialog->Unadvise(state_.cookies[removed]);
                if (SUCCEEDED(state_.unadvise_hr)) state_.advised[removed] = false;
                std::printf("UNADVISE inside_sink=1 removed_sink=%u hr=%08lX\n", removed + 1,
                    static_cast<unsigned long>(state_.unadvise_hr));
            }
        }
        std::printf("FILEOK seq=%u attempt=%u sink=%u answer=%08lX hwnd=%p thread=%lu\n", ++state_.sequence,
            attempt, index_ + 1, static_cast<unsigned long>(answer), static_cast<void*>(window), GetCurrentThreadId());
        return answer;
    }
    bool NotImplementedSink() const {
        return (state_.mode == Mode::NotImplFirst && index_ == 0) ||
            (state_.mode == Mode::NotImplSecond && index_ == 1);
    }
    HRESULT Notification(unsigned kind, const char* name) {
        ++state_.notifications[kind][index_];
        state_.identity_ok &= GetCurrentThreadId() == state_.thread;
        HRESULT answer = NotImplementedSink() ? E_NOTIMPL : S_OK;
        if (kind == 2 && TypeFailureMode(state_.mode) && index_ == 0)
            answer = state_.mode == Mode::TypeUnexpected ? E_UNEXPECTED : E_FAIL;
        if (kind == 2 && !state_.first_type_sequence[index_]) state_.first_type_sequence[index_] = state_.sequence + 1;
        std::printf("NOTIFY seq=%u event=%s sink=%u answer=%08lX thread=%lu\n", ++state_.sequence,
            name, index_ + 1, static_cast<unsigned long>(answer), GetCurrentThreadId());
        std::fflush(stdout);
        return answer;
    }
    // FolderChanging is vetoable navigation, not a notification-only method.
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return Notification(0, "FolderChange"); }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return Notification(1, "SelectionChange"); }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return Notification(2, "TypeChange"); }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* value) override {
        if (!value) return E_POINTER; *value = FDESVR_DEFAULT; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* value) override {
        if (!value) return E_POINTER; *value = FDEOR_DEFAULT; return S_OK;
    }
};
void Run(HWND owner, const std::filesystem::path& root, Mode mode) {
    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    Check(SUCCEEDED(hr), "raw native dialog created"); if (FAILED(hr)) return;
    GUID guid{}; hr = CoCreateGuid(&guid);
    wchar_t guid_text[40]{}; StringFromGUID2(guid, guid_text, 40);
    const auto journal = root / L"client-guid.txt";
    HANDLE file = CreateFileW(journal.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const DWORD bytes = static_cast<DWORD>(wcslen(guid_text) * sizeof(wchar_t));
    const bool journal_ok = file != INVALID_HANDLE_VALUE && WriteFile(file, guid_text, bytes, &written, nullptr) && written == bytes && FlushFileBuffers(file);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    Check(journal_ok, "unique client GUID journal retained"); if (!journal_ok) return;
    bool guid_bound = false;
    if (SUCCEEDED(hr)) { hr = dialog->SetClientGuid(guid); guid_bound = SUCCEEDED(hr); }
    if (SUCCEEDED(hr)) hr = dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
    if (SUCCEEDED(hr) && (NotImplMode(mode) || TypeFailureMode(mode))) {
        const COMDLG_FILTERSPEC filters[]{{L"Text", L"*.txt"}, {L"All", L"*.*"}};
        hr = dialog->SetFileTypes(2, filters);
        if (SUCCEEDED(hr)) hr = dialog->SetFileTypeIndex(1);
    }
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(root.c_str(), nullptr, IID_PPV_ARGS(&folder));
    if (SUCCEEDED(hr)) hr = dialog->SetFolder(folder.Get());
    if (SUCCEEDED(hr)) hr = dialog->SetFileName(L"selected.txt");
    Check(SUCCEEDED(hr), "filesystem configuration");
    State state; state.dialog = dialog.Get(); state.owner = owner; state.thread = GetCurrentThreadId();
    state.mode = mode; state.expected = (root / L"selected.txt").wstring();
    ComPtr<IFileDialogEvents> sinks[2];
    if (SUCCEEDED(hr)) for (unsigned i = 0; i != 2; ++i) {
        sinks[i].Attach(new Sink(state, i)); hr = dialog->Advise(sinks[i].Get(), &state.cookies[i]);
        std::printf("ADVISE sink=%u hr=%08lX cookie=%lu\n", i + 1, static_cast<unsigned long>(hr), state.cookies[i]);
        Check(SUCCEEDED(hr), "Advise sink in declared order");
        if (FAILED(hr)) break;
        state.advised[i] = true;
    }
    if (SUCCEEDED(hr)) {
        active = &state; state.started = GetTickCount64();
        const UINT_PTR timer = SetTimer(nullptr, 0, 100, Timer);
        Check(timer != 0, "Show STA controller timer");
        if (timer) {
            const HRESULT shown = dialog->Show(owner);
            KillTimer(nullptr, timer);
            std::printf("SHOW hr=%08lX attempts=%u retained=%d timeout=%d calls=[%u,%u][%u,%u]\n",
                static_cast<unsigned long>(shown), state.attempt, state.retained, state.timeout,
                state.calls[0][0], state.calls[0][1], state.calls[1][0], state.calls[1][1]);
            if (TypeFailureMode(mode)) {
                std::printf("TYPE_RESULT hr=%08lX first_return=%08lX typecounts=[%u,%u] typeorder=[%u,%u] visible=%d cancel_posted=%d timeout=%d\n",
                    static_cast<unsigned long>(shown), static_cast<unsigned long>(mode == Mode::TypeUnexpected ? E_UNEXPECTED : E_FAIL),
                    state.notifications[2][0], state.notifications[2][1], state.first_type_sequence[0], state.first_type_sequence[1],
                    state.visible_observed, state.cancel_posted, state.timeout);
                Check(state.notifications[2][0] > 0 && state.notifications[2][1] > 0 &&
                    state.first_type_sequence[0] < state.first_type_sequence[1],
                    "failing first TypeChange notification still reaches second sink in Advise order");
                Check(state.identity_ok && state.visible_observed && !state.timeout && state.test_cancel && state.cancel_posted,
                    "native remains visibly usable after notifications until explicit owned-window cancellation");
                Check(state.attempt == 0 && state.calls[0][0] == 0 && state.calls[0][1] == 0,
                    "notification-only case never submits or exercises FileOk veto");
                ComPtr<IShellItem> item; const HRESULT got = dialog->GetResult(&item);
                Check(shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) && FAILED(got) && !item,
                    "explicit cancellation returns exact cancelled HRESULT without result");
            } else if (NotImplMode(mode)) {
                std::printf("OBSERVATION notimpl terminal=%08lX test_cancel=%d FileOk=[%u,%u] FolderChange=[%u,%u] SelectionChange=[%u,%u] TypeChange=[%u,%u]\n",
                    static_cast<unsigned long>(shown), state.test_cancel, state.calls[0][0], state.calls[0][1],
                    state.notifications[0][0], state.notifications[0][1], state.notifications[1][0], state.notifications[1][1],
                    state.notifications[2][0], state.notifications[2][1]);
                Check(!state.timeout && state.attempt == 1 && state.identity_ok && state.candidate_ok,
                    "notimpl controlled single attempt completed with valid callback identity/candidate");
                const unsigned chosen = mode == Mode::NotImplFirst ? 0u : 1u;
                Check(state.calls[0][chosen] == 1, "actual E_NOTIMPL FileOk return observed (unobserved is failure)");
                Check(state.notifications[0][chosen] > 0 && state.notifications[2][chosen] > 0,
                    "actual E_NOTIMPL FolderChange and TypeChange observed");
                ComPtr<IShellItem> item; const HRESULT got = dialog->GetResult(&item);
                if (shown == S_OK && !state.test_cancel) {
                    PWSTR path = nullptr;
                    const HRESULT named = item ? item->GetDisplayName(SIGDN_FILESYSPATH, &path) : E_POINTER;
                    Check(SUCCEEDED(got) && SUCCEEDED(named) && path && _wcsicmp(path, state.expected.c_str()) == 0,
                        "observed native acceptance has exact fixture result");
                    CoTaskMemFree(path);
                } else {
                    Check(FAILED(shown) && FAILED(got) && !item,
                        "observed native nonacceptance or test cancellation has no result");
                    if (state.test_cancel) Check(shown == HRESULT_FROM_WIN32(ERROR_CANCELLED), "bounded test cancellation returned cancellation");
                }
            } else {
            Check(!state.timeout && state.retained && state.attempt == 2, "first S_FALSE retains same window until second submission");
            Check(state.identity_ok && state.candidate_ok, "all callbacks have exact native HWND STA and fixture candidate");
            Check(state.calls[0][0] == 1, "first sink observed first FileOk exactly once");
            if (mode != Mode::OtherUnadvise) Check(state.calls[0][1] == 1, "second sink observed first FileOk (unobserved is failure)");
            else std::printf("OBSERVATION other-unadvise first-round removed-sink calls=%u (order-sensitive, no assumed value)\n", state.calls[0][1]);
            if (mode == Mode::SelfUnadvise || mode == Mode::OtherUnadvise) {
                Check(SUCCEEDED(state.unadvise_hr), "Unadvise from callback succeeded");
                const unsigned removed = mode == Mode::SelfUnadvise ? 0u : 1u;
                Check(state.calls[1][removed] == 0 && state.calls[1][1 - removed] == 1, "removed sink absent and remaining sink notified on next FileOk");
            } else Check(state.calls[1][0] == 1 && state.calls[1][1] == 1, "both sinks notified on accepting FileOk");
            ComPtr<IShellItem> item; PWSTR path = nullptr;
            const HRESULT got = dialog->GetResult(&item);
            const HRESULT named = item ? item->GetDisplayName(SIGDN_FILESYSPATH, &path) : E_POINTER;
            Check(shown == S_OK && SUCCEEDED(got) && SUCCEEDED(named) && path && _wcsicmp(path, state.expected.c_str()) == 0,
                "second submission returns exact fixture result");
            CoTaskMemFree(path);
            }
            Check(IsWindowEnabled(owner) != FALSE, "native Show restores owned owner");
        }
        active = nullptr;
    }
    for (unsigned i = 0; i != 2; ++i) if (state.advised[i]) Check(SUCCEEDED(dialog->Unadvise(state.cookies[i])), "remaining sink unadvised", true);
    const HRESULT cleared = guid_bound ? dialog->ClearClientData() : E_UNEXPECTED;
    std::printf("CLEANUP ClearClientData=%08lX\n", static_cast<unsigned long>(cleared));
    Check(SUCCEEDED(cleared), "unique client history cleanup", true);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    Mode mode;
    const std::wstring argument = argv[1];
    if (argument == L"--first-veto") mode = Mode::FirstVeto;
    else if (argument == L"--second-veto") mode = Mode::SecondVeto;
    else if (argument == L"--self-unadvise") mode = Mode::SelfUnadvise;
    else if (argument == L"--other-unadvise") mode = Mode::OtherUnadvise;
    else if (argument == L"--notimpl") mode = Mode::NotImplFirst;
    else if (argument == L"--notimpl-second") mode = Mode::NotImplSecond;
    else if (argument == L"--type-unexpected") mode = Mode::TypeUnexpected;
    else if (argument == L"--type-fail") mode = Mode::TypeFail;
    else return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return 2;
    std::thread watchdog([done] {
        if (WaitForSingleObject(done, 45000) != WAIT_OBJECT_0) {
            std::printf("[FAIL] functional watchdog exceeded 45s; only this process terminates; fixture retained\n");
            std::fflush(stdout); TerminateProcess(GetCurrentProcess(), 99);
        }
    });
    try {
        const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") / (L"native-multisink-" + suffix));
        std::filesystem::create_directories(root.parent_path());
        if (!CreateDirectoryW(root.c_str(), nullptr)) throw GetLastError();
        char fixture_utf8[4096]{};
        WideCharToMultiByte(CP_UTF8, 0, root.c_str(), -1, fixture_utf8, static_cast<int>(sizeof(fixture_utf8)), nullptr, nullptr);
        std::printf("FIXTURE %s\n", fixture_utf8);
        const auto path = root / L"selected.txt";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "exclusive fixture file created");
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        HDESK previous = GetThreadDesktop(GetCurrentThreadId());
        HDESK desktop = CreateDesktopW((L"PulseNativeMultisink-" + suffix).c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        const bool switched = desktop && SetThreadDesktop(desktop);
        Check(switched, "private non-active desktop selected");
        if (switched) {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            Check(SUCCEEDED(com), "native STA initialized");
            if (SUCCEEDED(com)) {
                HWND owner = CreateWindowW(L"STATIC", L"Pulse multisink owned owner", WS_OVERLAPPED,
                    0, 0, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                Check(owner != nullptr, "private owner created");
                if (owner && file != INVALID_HANDLE_VALUE) Run(owner, root, mode);
                if (owner) Check(DestroyWindow(owner) != FALSE, "owned owner destroyed", true);
                CoUninitialize();
            }
            Check(SetThreadDesktop(previous) != FALSE, "original desktop restored", true);
        }
        if (desktop) Check(CloseDesktop(desktop) != FALSE, "private desktop closed", true);
        if (file != INVALID_HANDLE_VALUE) Check(DeleteFileW(path.c_str()) != FALSE, "exact fixture file removed", true);
        std::printf("JOURNAL retained in unique fixture directory for exact history diagnosis\n");
    } catch (...) { Check(false, "setup exception; fixture retained"); }
    SetEvent(done); watchdog.join(); CloseHandle(done);
    std::printf("SUMMARY functional_failures=%u cleanup_failures=%u\n", functional_failures, cleanup_failures);
    return functional_failures || cleanup_failures ? 1 : 0;
}

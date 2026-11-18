#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

// Standard messages address only this process's verified private-desktop dialog.
// They are not evidence of physical keyboard input. One case per process.
namespace {
using Microsoft::WRL::ComPtr;
enum class Mode { NewVeto, Accept, Refuse, MultiDefaultAccept, MultiAcceptRefuse, MultiRefuseAccept, MultiNotImplAccept, OverwriteNotImpl };
bool Multi(Mode mode) { return mode == Mode::MultiDefaultAccept || mode == Mode::MultiAcceptRefuse || mode == Mode::MultiRefuseAccept || mode == Mode::MultiNotImplAccept; }
unsigned failures = 0, cleanup_failures = 0;
void Check(bool ok, const char* text, bool cleanup = false) {
    if (!ok) ++(cleanup ? cleanup_failures : failures);
    std::printf("[%s] %s %s\n", ok ? "PASS" : "FAIL", cleanup ? "cleanup" : "functional", text);
}
bool Absent(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
}
bool Exact(IShellItem* item, const std::wstring& path) {
    PWSTR name = nullptr;
    const HRESULT hr = item ? item->GetDisplayName(SIGDN_FILESYSPATH, &name) : E_POINTER;
    const bool ok = SUCCEEDED(hr) && name && _wcsicmp(name, path.c_str()) == 0;
    std::printf("ITEM hr=%08lX exact=%d\n", static_cast<unsigned long>(hr), ok);
    CoTaskMemFree(name); return ok;
}
bool ReadFileIdentity(const std::wstring& path, BY_HANDLE_FILE_INFORMATION& info) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char bytes[32]{}; DWORD read = 0;
    const bool ok = GetFileInformationByHandle(file, &info) && ReadFile(file, bytes, sizeof(bytes), &read, nullptr) &&
        read == 14 && std::memcmp(bytes, "owned-existing", 14) == 0;
    CloseHandle(file); return ok;
}
struct PromptSearch { HWND owner = nullptr, found = nullptr; DWORD sta = 0; };
BOOL CALLBACK FindOwnedPrompt(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<PromptSearch*>(parameter);
    DWORD pid = 0; wchar_t kind[32]{};
    if (GetWindowThreadProcessId(window, &pid) == search.sta && pid == GetCurrentProcessId() &&
        GetWindow(window, GW_OWNER) == search.owner && !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) &&
        IsWindowVisible(window) && IsWindowEnabled(window) && GetClassNameW(window, kind, 32) && wcscmp(kind, L"#32770") == 0)
        search.found = window;
    return TRUE;
}
struct NegativeButtonSearch {
    HWND prompt = nullptr, no = nullptr, cancel = nullptr;
    DWORD sta = 0; unsigned no_count = 0, cancel_count = 0;
};
bool OwnedButton(HWND button, HWND prompt, DWORD sta, int id) {
    DWORD pid = 0; wchar_t kind[32]{};
    return IsChild(prompt, button) && GetWindowThreadProcessId(button, &pid) == sta && pid == GetCurrentProcessId() &&
        GetClassNameW(button, kind, 32) && _wcsicmp(kind, L"Button") == 0 &&
        IsWindowVisible(button) && IsWindowEnabled(button) && GetDlgCtrlID(button) == id;
}
BOOL CALLBACK FindNegativeButton(HWND button, LPARAM parameter) {
    auto& search = *reinterpret_cast<NegativeButtonSearch*>(parameter);
    DWORD pid = 0; wchar_t kind[32]{};
    if (!IsChild(search.prompt, button) || GetWindowThreadProcessId(button, &pid) != search.sta || pid != GetCurrentProcessId() ||
        !GetClassNameW(button, kind, 32) || _wcsicmp(kind, L"Button") != 0) return TRUE;
    const int id = GetDlgCtrlID(button);
    std::printf("MODAL_BUTTON hwnd=%p id=%d visible=%d enabled=%d\n", static_cast<void*>(button), id,
        IsWindowVisible(button), IsWindowEnabled(button));
    if (OwnedButton(button, search.prompt, search.sta, id)) {
        if (id == IDNO) { search.no = button; ++search.no_count; }
        if (id == IDCANCEL) { search.cancel = button; ++search.cancel_count; }
    }
    return TRUE;
}
struct State {
    IFileSaveDialog* dialog = nullptr;
    HWND owner = nullptr, window = nullptr;
    DWORD sta = 0;
    Mode mode{};
    std::wstring path;
    unsigned sequence = 0, file_ok = 0, overwrite = 0, attempt = 0, depth = 0;
    unsigned overwrite_by_sink[3]{}, fileok_by_sink[3]{};
    bool timer_busy = false, timeout = false, retained = false, cancelled = false;
    bool prompt_observed = false, prompt_close_posted = false, prompt_buttons_checked = false;
    HWND prompt_window = nullptr;
    ULONGLONG prompt_close_tick = 0;
    ULONGLONG started = 0, submitted = 0;
    HWND Window() const {
        ComPtr<IOleWindow> ole; HWND hwnd = nullptr; DWORD pid = 0;
        if (FAILED(dialog->QueryInterface(IID_PPV_ARGS(&ole))) || FAILED(ole->GetWindow(&hwnd))) return nullptr;
        return hwnd && GetWindowThreadProcessId(hwnd, &pid) == sta && pid == GetCurrentProcessId() &&
            GetWindow(hwnd, GW_OWNER) == owner ? hwnd : nullptr;
    }
    void Tick() {
        if (timer_busy || depth) return;
        timer_busy = true; struct Reset { bool& v; ~Reset() { v = false; } } reset{timer_busy};
        HWND actual = Window();
        if (GetTickCount64() - started >= 20000) {
            if (!timeout) {
                timeout = true;
                const BOOL posted = actual && PostMessageW(actual, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                std::printf("TIMEOUT cancel_posted=%d (failure, never acceptance)\n", posted);
            }
            return;
        }
        if ((mode == Mode::OverwriteNotImpl || Multi(mode)) && overwrite && actual && !prompt_buttons_checked &&
            GetTickCount64() - submitted >= 500) {
            PromptSearch prompt{actual, nullptr, sta};
            EnumThreadWindows(sta, FindOwnedPrompt, reinterpret_cast<LPARAM>(&prompt));
            if (prompt.found) {
                prompt_observed = true;
                if (!prompt_close_tick) {
                    prompt_window = prompt.found;
                    prompt_close_tick = GetTickCount64();
                    // Keep the WM_CLOSE observation; a posted message is not completion.
                    prompt_close_posted = PostMessageW(prompt.found, WM_CLOSE, 0, 0) != FALSE;
                    std::printf("OBSERVATION owned-modal observed=1 visible=1 enabled=1 hwnd=%p owner=%p WM_CLOSE=%d (caption/meaning not inferred)\n",
                        static_cast<void*>(prompt.found), static_cast<void*>(actual), prompt_close_posted);
                    return;
                }
                if (GetTickCount64() - prompt_close_tick < 250) return;
                prompt_buttons_checked = true;
                std::printf("OBSERVATION modal persists after WM_CLOSE posted=%d same_hwnd=%d\n", prompt_close_posted, prompt.found == prompt_window);
                if (prompt.found != prompt_window) return;
                NegativeButtonSearch buttons{prompt.found, nullptr, nullptr, sta};
                EnumChildWindows(prompt.found, FindNegativeButton, reinterpret_cast<LPARAM>(&buttons));
                const HWND button = buttons.no_count == 1 ? buttons.no : buttons.cancel_count == 1 ? buttons.cancel : nullptr;
                const int id = buttons.no_count == 1 ? IDNO : IDCANCEL;
                // Recheck the modal and observed standard negative button immediately before posting.
                PromptSearch verified{actual, nullptr, sta};
                EnumThreadWindows(sta, FindOwnedPrompt, reinterpret_cast<LPARAM>(&verified));
                const bool safe = button && verified.found == prompt.found && OwnedButton(button, prompt.found, sta, id);
                const BOOL posted = safe && PostMessageW(button, BM_CLICK, 0, 0);
                std::printf("MODAL_NEGATIVE no_count=%u cancel_count=%u button=%p id=%d reverified=%d BM_CLICK=%d (not completion evidence)\n",
                    buttons.no_count, buttons.cancel_count, static_cast<void*>(button), id, safe, posted);
                return;
            }
        }
        if (!actual || !IsWindowVisible(actual) || !IsWindowEnabled(actual)) return;
        if (!window) window = actual;
        if (window != actual) { Check(false, "same native HWND retained"); return; }
        if ((mode == Mode::Refuse || Multi(mode) || mode == Mode::OverwriteNotImpl) && overwrite && !cancelled && GetTickCount64() - submitted >= 500) {
            retained = true; cancelled = true;
            std::printf("OBSERVATION main-still-open visible=1 enabled=1 elapsed=%llu; explicit test cancellation follows (not native refusal classification)\n",
                GetTickCount64() - submitted);
            ComPtr<IShellItem> item; const HRESULT hr = dialog->GetResult(&item);
            Check(FAILED(hr) && !item, "retained dialog has no available result before explicit cancellation");
            Check(PostMessageW(actual, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0) != FALSE,
                "explicit cancellation posted to retained owned dialog");
            return;
        }
        if (!attempt || (mode == Mode::NewVeto && attempt == 1 && file_ok == 1 && GetTickCount64() - submitted >= 500)) {
            if (attempt) retained = true;
            ++attempt; submitted = GetTickCount64();
            if (mode == Mode::NewVeto) Check(Absent(path), "new target remains absent before submission");
            const BOOL posted = PostMessageW(actual, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
            std::printf("SUBMIT attempt=%u hwnd=%p visible=1 posted=%d\n", attempt, static_cast<void*>(actual), posted);
            Check(posted != FALSE, "submit exact owned native dialog");
        }
    }
};
thread_local State* active = nullptr;
void CALLBACK Timer(HWND, UINT, UINT_PTR, DWORD) { if (active) active->Tick(); }
class Sink final : public IFileDialogEvents {
    std::atomic<ULONG> refs_{1}; State& s_; unsigned index_;
public:
    explicit Sink(State& state, unsigned index = 0) : s_(state), index_(index) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *out = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        ++s_.depth; struct End { State& s; ~End() { --s.depth; } } end{s_};
        ++s_.file_ok; ++s_.fileok_by_sink[index_];
        Check(GetCurrentThreadId() == s_.sta && s_.Window() == s_.window && s_.window, "FileOk exact STA and native HWND");
        ComPtr<IShellItem> item; const HRESULT got = dialog->GetResult(&item);
        Check(SUCCEEDED(got) && Exact(item.Get(), s_.path), "FileOk GetResult returns exact candidate");
        if (s_.mode == Mode::NewVeto) Check(Absent(s_.path), "new candidate absent inside FileOk");
        const HRESULT answer = s_.mode == Mode::NewVeto && s_.file_ok == 1 ? S_FALSE : S_OK;
        std::printf("EVENT seq=%u sink=%u FileOk attempt=%u return=%08lX GetResult=%08lX\n", ++s_.sequence,
            index_ + 1, s_.attempt, static_cast<unsigned long>(answer), static_cast<unsigned long>(got));
        return answer;
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem* item, FDE_OVERWRITE_RESPONSE* response) override {
        if (!response) return E_POINTER;
        ++s_.overwrite; ++s_.overwrite_by_sink[index_];
        unsigned incoming = 0; static_assert(sizeof(incoming) == sizeof(*response));
        std::memcpy(&incoming, response, sizeof(incoming));
        Check(GetCurrentThreadId() == s_.sta && s_.Window() == s_.window && s_.window, "Overwrite exact STA and native HWND");
        Check(Exact(item, s_.path), "Overwrite supplies exact existing target");
        *response = s_.mode == Mode::Refuse ? FDEOR_REFUSE : FDEOR_ACCEPT;
        if (Multi(s_.mode)) {
            *response = FDEOR_DEFAULT;
            if ((s_.mode == Mode::MultiDefaultAccept || s_.mode == Mode::MultiNotImplAccept) && index_ == 1) *response = FDEOR_ACCEPT;
            if (s_.mode == Mode::MultiAcceptRefuse && index_ < 2) *response = index_ == 0 ? FDEOR_ACCEPT : FDEOR_REFUSE;
            if (s_.mode == Mode::MultiRefuseAccept && index_ < 2) *response = index_ == 0 ? FDEOR_REFUSE : FDEOR_ACCEPT;
        }
        const HRESULT answer = s_.mode == Mode::OverwriteNotImpl || (s_.mode == Mode::MultiNotImplAccept && index_ == 0) ? E_NOTIMPL : S_OK;
        if (s_.mode == Mode::OverwriteNotImpl) *response = FDEOR_DEFAULT;
        std::printf("EVENT seq=%u sink=%u Overwrite attempt=%u incoming_raw=%u response=%u return=%08lX\n", ++s_.sequence,
            index_ + 1, s_.attempt, incoming, static_cast<unsigned>(*response), static_cast<unsigned long>(answer));
        return answer;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override {
        if (!response) return E_POINTER; *response = FDESVR_DEFAULT; return S_OK;
    }
};
void Run(HWND owner, const std::filesystem::path& root, Mode mode) {
    const auto path = (root / (mode == Mode::NewVeto ? L"new.txt" : L"existing.txt")).wstring();
    BY_HANDLE_FILE_INFORMATION before{}, after{};
    if (mode != Mode::NewVeto) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, "owned-existing", 14, &written, nullptr) && written == 14;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        Check(ok && ReadFileIdentity(path, before), "exclusive existing fixture contents and identity captured");
        if (!ok) return;
    } else Check(Absent(path), "new fixture starts absent");
    ComPtr<IFileSaveDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    Check(SUCCEEDED(hr), "native Save COM object"); if (FAILED(hr)) return;
    GUID guid{}; hr = CoCreateGuid(&guid); if (FAILED(hr)) { Check(false, "unique GUID"); return; }
    wchar_t text[40]{}; StringFromGUID2(guid, text, 40);
    HANDLE journal = CreateFileW((root / L"client-guid.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0; const DWORD bytes = static_cast<DWORD>(wcslen(text) * sizeof(wchar_t));
    const bool recorded = journal != INVALID_HANDLE_VALUE && WriteFile(journal, text, bytes, &written, nullptr) && written == bytes && FlushFileBuffers(journal);
    if (journal != INVALID_HANDLE_VALUE) CloseHandle(journal);
    Check(recorded, "exact client GUID journal retained"); if (!recorded) return;
    hr = dialog->SetClientGuid(guid); const bool bound = SUCCEEDED(hr);
    if (SUCCEEDED(hr)) hr = dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
        FOS_DONTADDTORECENT | FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN);
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(root.c_str(), nullptr, IID_PPV_ARGS(&folder));
    if (SUCCEEDED(hr)) hr = dialog->SetFolder(folder.Get());
    if (SUCCEEDED(hr)) hr = dialog->SetFileName(mode == Mode::NewVeto ? L"new.txt" : L"existing.txt");
    Check(SUCCEEDED(hr), "Save native configuration");
    State state; state.dialog = dialog.Get(); state.owner = owner; state.sta = GetCurrentThreadId(); state.mode = mode; state.path = path;
    ComPtr<IFileDialogEvents> sinks[3]; DWORD cookies[3]{}; unsigned advised_count = 0;
    const unsigned expected_sinks = Multi(mode) ? 3u : 1u;
    for (unsigned i = 0; SUCCEEDED(hr) && i < expected_sinks; ++i) {
        sinks[i].Attach(new Sink(state, i)); hr = dialog->Advise(sinks[i].Get(), &cookies[i]);
        std::printf("ADVISE sink=%u hr=%08lX cookie=%lu\n", i + 1, static_cast<unsigned long>(hr), cookies[i]);
        if (SUCCEEDED(hr)) ++advised_count;
    }
    const bool advised = SUCCEEDED(hr) && advised_count == expected_sinks; Check(advised, "Save event sinks advised in declared order");
    if (advised) {
        active = &state; state.started = GetTickCount64(); const UINT_PTR timer = SetTimer(nullptr, 0, 100, Timer);
        Check(timer != 0, "bounded STA controller");
        if (timer) {
            const HRESULT shown = dialog->Show(owner); KillTimer(nullptr, timer);
            ComPtr<IShellItem> result; const HRESULT got = dialog->GetResult(&result);
            std::printf("SHOW hr=%08lX GetResult=%08lX attempts=%u FileOk=%u Overwrite=%u retained=%d cancel=%d timeout=%d\n",
                static_cast<unsigned long>(shown), static_cast<unsigned long>(got), state.attempt, state.file_ok,
                state.overwrite, state.retained, state.cancelled, state.timeout);
            Check(!state.timeout, "no timeout substituted for semantic evidence");
            if (Multi(mode) || mode == Mode::OverwriteNotImpl) {
                std::printf("OBSERVATION prompt_observed=%d prompt_close_posted=%d; no default-prompt meaning inferred\n",
                    state.prompt_observed, state.prompt_close_posted);
                std::printf("MULTI observed Overwrite=[%u,%u,%u] FileOk=[%u,%u,%u] outcome=%s\n",
                    state.overwrite_by_sink[0], state.overwrite_by_sink[1], state.overwrite_by_sink[2],
                    state.fileok_by_sink[0], state.fileok_by_sink[1], state.fileok_by_sink[2],
                    shown == S_OK && !state.cancelled ? "native-accepted" : state.retained && state.cancelled ? "observed-retained-then-test-cancel" : "unproven");
                Check(state.attempt == 1 && state.overwrite > 0, "actual multi-sink overwrite notification observed after one submission");
                if (shown == S_OK && !state.cancelled) {
                    Check(SUCCEEDED(got) && Exact(result.Get(), path) && state.file_ok > 0,
                        "observed native acceptance has exact target and actual FileOk");
                } else {
                    Check(state.retained && state.cancelled && !state.timeout && shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) && FAILED(got) && !result,
                        "observed usable retained dialog explicitly cancelled without result; no aggregate rule assumed");
                }
            } else if (mode == Mode::Refuse) {
                Check(state.overwrite == 1 && state.retained && state.cancelled && state.attempt == 1,
                    "REFUSE retains usable dialog without terminal acceptance (FileOk order recorded)");
                Check(shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) && FAILED(got) && !result, "explicit cancellation has no result");
            } else {
                Check(shown == S_OK && SUCCEEDED(got) && Exact(result.Get(), path), "native acceptance returns exact target");
                if (mode == Mode::NewVeto) Check(state.retained && state.attempt == 2 && state.file_ok == 2 && state.overwrite == 0,
                    "new target FileOk veto retains same window then accepts second attempt");
                else Check(state.overwrite == 1 && state.file_ok == 1 && state.attempt == 1,
                    "ACCEPT invokes actual Overwrite and FileOk once (order recorded, not assumed)");
            }
            Check(IsWindowEnabled(owner) != FALSE, "native restores owner");
        }
        active = nullptr;
    }
    for (unsigned i = 0; i < advised_count; ++i)
        Check(SUCCEEDED(dialog->Unadvise(cookies[i])), "sink unadvised", true);
    if (mode == Mode::NewVeto) Check(Absent(path), "new target remains absent after Show");
    else {
        Check(ReadFileIdentity(path, after) && before.dwVolumeSerialNumber == after.dwVolumeSerialNumber &&
            before.nFileIndexHigh == after.nFileIndexHigh && before.nFileIndexLow == after.nFileIndexLow &&
            CompareFileTime(&before.ftCreationTime, &after.ftCreationTime) == 0 &&
            CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0,
            "existing target identity contents and last-write time unchanged");
        Check(DeleteFileW(path.c_str()) != FALSE, "exact owned fixture removed", true);
    }
    const HRESULT cleared = bound ? dialog->ClearClientData() : E_UNEXPECTED;
    std::printf("CLEANUP ClearClientData=%08lX\n", static_cast<unsigned long>(cleared));
    Check(SUCCEEDED(cleared), "only test GUID history cleared", true);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    Mode mode;
    const std::wstring arg = argv[1];
    if (arg == L"--new-veto") mode = Mode::NewVeto;
    else if (arg == L"--overwrite-accept") mode = Mode::Accept;
    else if (arg == L"--overwrite-refuse") mode = Mode::Refuse;
    else if (arg == L"--multi-default-accept") mode = Mode::MultiDefaultAccept;
    else if (arg == L"--multi-accept-refuse") mode = Mode::MultiAcceptRefuse;
    else if (arg == L"--multi-refuse-accept") mode = Mode::MultiRefuseAccept;
    else if (arg == L"--multi-notimpl-accept") mode = Mode::MultiNotImplAccept;
    else if (arg == L"--overwrite-notimpl") mode = Mode::OverwriteNotImpl;
    else return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr); if (!done) return 2;
    std::thread watchdog([done] { if (WaitForSingleObject(done, 45000) != WAIT_OBJECT_0) {
        std::printf("[FAIL] watchdog45s; own process only terminates; evidence retained\n");
        TerminateProcess(GetCurrentProcess(), 99);
    }});
    try {
        const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") / (L"native-save-events-" + suffix));
        std::filesystem::create_directories(root.parent_path());
        if (!CreateDirectoryW(root.c_str(), nullptr)) throw GetLastError();
        std::printf("FIXTURE %ls\n", root.c_str());
        HDESK previous = GetThreadDesktop(GetCurrentThreadId());
        HDESK desktop = CreateDesktopW((L"PulseSaveEvents-" + suffix).c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        const bool switched = desktop && SetThreadDesktop(desktop); Check(switched, "private non-active desktop selected");
        if (switched) {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); Check(SUCCEEDED(com), "STA initialized");
            if (SUCCEEDED(com)) {
                HWND owner = CreateWindowW(L"STATIC", L"Pulse Save events owner", WS_OVERLAPPED,
                    0, 0, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                Check(owner != nullptr, "owned owner created");
                if (owner) Run(owner, root, mode);
                if (owner) Check(DestroyWindow(owner) != FALSE, "owner destroyed", true);
                CoUninitialize();
            }
            Check(SetThreadDesktop(previous) != FALSE, "original desktop restored", true);
        }
        if (desktop) Check(CloseDesktop(desktop) != FALSE, "private desktop closed", true);
        std::printf("JOURNAL retained for exact GUID cleanup diagnostics\n");
    } catch (...) { Check(false, "setup exception; evidence retained"); }
    SetEvent(done); watchdog.join(); CloseHandle(done);
    std::printf("SUMMARY functional_failures=%u cleanup_failures=%u\n", failures, cleanup_failures);
    return failures || cleanup_failures ? 1 : 0;
}

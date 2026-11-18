#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <atomic>
#include <fstream>
#include <vector>
#include <aclapi.h>
#include <sddl.h>
#include <mutex>
#include <cstring>
#include <regex>
#include "../shell_bridge/file_dialog_wrapper.h"
#pragma comment(lib, "advapi32.lib")

namespace {
using Microsoft::WRL::ComPtr;
int failures = 0;
bool native_options_probe = false;
HRESULT NativeDialog(bool save, IFileDialog** output) {
    using Entry = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
    static HMODULE module = LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto entry = module ? reinterpret_cast<Entry>(GetProcAddress(module, "DllGetClassObject")) : nullptr;
    ComPtr<IClassFactory> factory;
    const HRESULT hr = entry ? entry(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, IID_PPV_ARGS(&factory)) : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    return FAILED(hr) ? hr : factory->CreateInstance(nullptr, IID_PPV_ARGS(output));
}
std::atomic<int> cleanup_failures{0};
constexpr UINT kDrive = WM_APP + 71;
void Check(bool condition, const char* label) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (!condition) ++failures;
}

void CleanupCheck(bool condition, const char* label) {
    std::printf("[%s] cleanup: %s\n", condition ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (!condition) ++cleanup_failures;
}
struct OpenValidationDrive {
    const wchar_t* input = nullptr;
    bool (*emergency_cleanup)(void*) = nullptr;
    void* cleanup_context = nullptr;
    const std::filesystem::path* journal_directory = nullptr;
    bool accept_owned_overwrite = false;
};
struct RunState {
    IFileDialog* dialog = nullptr; // borrowed and accessed only by its creating STA
    ULONGLONG started = 0;
    bool cancel_case = false;
    bool visible = false;
    bool submitted = false;
    bool bounded_close = false;
    bool sta_close_attempted = false;
    HRESULT close_result = E_PENDING;
    std::atomic<HWND> native_window{nullptr};
    std::atomic<bool> test_cancel_sent{false};
    const OpenValidationDrive* validation = nullptr;
    bool folder_probe = false, folder_probed = false;
    bool input_attempted = false, input_confirmed = false;
    unsigned input_attempts = 0;
    std::atomic<ULONGLONG> submitted_tick{0};
    std::atomic<bool> prompt_observed{false}, remained_open{false};
    bool affirmative_attempted = false, affirmative_posted = false, affirmative_refused = false;
    unsigned affirmative_scans = 0;
    ULONGLONG affirmative_last_scan = 0;
    std::string affirmative_trace;
};

bool OwnedBy(HWND window, HWND root) {
    for (int depth = 0; window && depth < 32; ++depth) {
        window = GetWindow(window, GW_OWNER);
        if (window == root) return true;
    }
    return false;
}

struct PromptScan {
    HWND root;
    bool found = false;
    unsigned dismissed = 0;
    bool dismiss = false;
    bool visible_found = false;
    bool diagnose = false;
};
struct NativeCancel { HWND root; bool close; bool posted = false; };
BOOL CALLBACK CancelNative(HWND hwnd, LPARAM context) {
    auto& request = *reinterpret_cast<NativeCancel*>(context);
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    wchar_t name[64]{};
    GetClassNameW(hwnd, name, 64);
    if (hwnd != request.root || process != GetCurrentProcessId() ||
        std::wstring_view(name) != L"#32770" || !IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd)) return TRUE;
    std::printf("[FACT] controller posting native %s hwnd=%p\n", request.close ? "WM_CLOSE" : "IDCANCEL", static_cast<void*>(hwnd));
    std::fflush(stdout);
    request.posted = PostMessageW(hwnd, request.close ? WM_CLOSE : WM_COMMAND,
        request.close ? 0 : MAKEWPARAM(IDCANCEL, BN_CLICKED), 0) != FALSE;
    std::printf("[FACT] controller native cancel posted=%d\n", request.posted);
    std::fflush(stdout);
    return FALSE;
}
BOOL CALLBACK DiagnoseWindow(HWND hwnd, LPARAM) {
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(hwnd, &process);
    if (process != GetCurrentProcessId()) return TRUE;
    wchar_t name[128]{};
    GetClassNameW(hwnd, name, 128);
    std::printf("[WINDOW] hwnd=%p owner=%p thread=%lu class=%ls visible=%d enabled=%d\n",
        static_cast<void*>(hwnd), static_cast<void*>(GetWindow(hwnd, GW_OWNER)), thread,
        name, IsWindowVisible(hwnd), IsWindowEnabled(hwnd));
    return TRUE;
}
std::string UnicodeEscapes(std::wstring_view text) {
    std::string out;
    for (const wchar_t unit : text) {
        if (unit >= 0x20 && unit <= 0x7e && unit != L'\\') out.push_back(static_cast<char>(unit));
        else {
            char escaped[8]{};
            std::snprintf(escaped, sizeof(escaped), "\\u%04X", static_cast<unsigned>(unit));
            out += escaped;
        }
    }
    return out;
}
bool ExplicitAffirmativeButton(const std::wstring& text) {
    for (const auto* label : {L"Yes", L"&Yes", L"是", L"是(&Y)"})
        if (_wcsicmp(text.c_str(), label) == 0) return true;
    return false;
}
bool ReadPromptButton(HWND hwnd, std::wstring& text) {
    wchar_t name[32]{}, value[256]{};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if (std::wstring_view(name) != L"Button") return false;
    DWORD_PTR length = 0;
    if (!SendMessageTimeoutW(hwnd, WM_GETTEXT, ARRAYSIZE(value), reinterpret_cast<LPARAM>(value),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &length) || !length || length >= ARRAYSIZE(value) - 1) return false;
    text.assign(value, static_cast<size_t>(length));
    return true;
}
bool ExplicitNegativeButton(const std::wstring& text) {
    // Exact observed semantics only; no position, ordinal or ID-zero inference.
    for (const auto* label : {L"No", L"&No", L"Cancel", L"&Cancel", L"否", L"否(&N)",
                             L"取消", L"取消(&C)"})
        if (_wcsicmp(text.c_str(), label) == 0) return true;
    return false;
}
struct NegativeButtons {
    HWND dialog;
    HWND match = nullptr;
    std::wstring text;
    unsigned matches = 0;
    bool complete = true;
    bool affirmative = false;
};
BOOL CALLBACK FindNegativeButton(HWND hwnd, LPARAM context) {
    auto& buttons = *reinterpret_cast<NegativeButtons*>(context);
    wchar_t name[32]{};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if (std::wstring_view(name) != L"Button" || !IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd)) return TRUE;
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(hwnd, &process);
    std::wstring text;
    if (process != GetCurrentProcessId() || thread != GetWindowThreadProcessId(buttons.dialog, nullptr) ||
        !IsChild(buttons.dialog, hwnd) || !ReadPromptButton(hwnd, text)) {
        buttons.complete = false;
        return TRUE;
    }
    if (buttons.affirmative ? ExplicitAffirmativeButton(text) : ExplicitNegativeButton(text)) {
        ++buttons.matches;
        buttons.match = hwnd;
        buttons.text = std::move(text);
    }
    return TRUE;
}
BOOL CALLBACK DiagnosePromptChild(HWND hwnd, LPARAM context) {
    auto& count = *reinterpret_cast<unsigned*>(context);
    if (count++ >= 64) return FALSE;
    wchar_t name[128]{};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    std::printf("[PROMPT_CONTROL] hwnd=%p parent=%p id=%d class=%ls visible=%d enabled=%d\n",
        static_cast<void*>(hwnd), static_cast<void*>(GetParent(hwnd)), GetDlgCtrlID(hwnd), name,
        IsWindowVisible(hwnd), IsWindowEnabled(hwnd));
    if (std::wstring_view(name) == L"Button") {
        std::wstring text;
        const bool read = ReadPromptButton(hwnd, text);
        std::printf("[PROMPT_BUTTON_TEXT] hwnd=%p read=%d negative=%d text=%s\n",
            static_cast<void*>(hwnd), read, read && ExplicitNegativeButton(text), UnicodeEscapes(text).c_str());
    }
    return TRUE;
}
struct AffirmativePrompt {
    HWND root;
    HWND dialog = nullptr;
    unsigned matches = 0;
    std::string candidates;
};
BOOL CALLBACK FindAffirmativePrompt(HWND hwnd, LPARAM context) {
    auto& prompt = *reinterpret_cast<AffirmativePrompt*>(context);
    DWORD pid = 0;
    const DWORD thread = GetWindowThreadProcessId(hwnd, &pid);
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    if (hwnd != prompt.root && pid == GetCurrentProcessId() && GetWindow(hwnd, GW_OWNER) == prompt.root &&
        std::wstring_view(cls) == L"#32770") {
        char candidate[192]{};
        std::snprintf(candidate, sizeof(candidate), " hwnd=%p visible=%d enabled=%d thread=%lu same_sta=%d;",
            static_cast<void*>(hwnd), IsWindowVisible(hwnd), IsWindowEnabled(hwnd), thread,
            thread == GetWindowThreadProcessId(prompt.root, nullptr));
        prompt.candidates += candidate;
    }
    if (hwnd != prompt.root && pid == GetCurrentProcessId() &&
        thread == GetWindowThreadProcessId(prompt.root, nullptr) && GetWindow(hwnd, GW_OWNER) == prompt.root &&
        std::wstring_view(cls) == L"#32770" && IsWindowVisible(hwnd) && IsWindowEnabled(hwnd)) {
        ++prompt.matches;
        prompt.dialog = hwnd;
    }
    return TRUE;
}
void AcceptOwnedOverwrite(HDESK desktop, RunState& state) {
    if (state.affirmative_attempted) return;
    ++state.affirmative_scans;
    state.affirmative_last_scan = GetTickCount64();
    AffirmativePrompt prompt{state.native_window.load()};
    const bool root_enabled = prompt.root && IsWindowEnabled(prompt.root);
    BOOL enumerated = FALSE;
    DWORD enumeration_error = ERROR_SUCCESS;
    if (prompt.root) {
        SetLastError(ERROR_SUCCESS);
        enumerated = EnumDesktopWindows(desktop, FindAffirmativePrompt, reinterpret_cast<LPARAM>(&prompt));
        if (!enumerated) enumeration_error = GetLastError();
    }
    const auto trace = std::string("root_present=") + (prompt.root ? "1" : "0") + " root_enabled=" +
        (root_enabled ? "1" : "0") + " enum_ok=" + std::to_string(enumerated) + " error=" +
        std::to_string(enumeration_error) + " eligible=" + std::to_string(prompt.matches) + " candidates:" + prompt.candidates;
    if (trace != state.affirmative_trace) {
        std::printf("[AFFIRMATIVE_SCAN] elapsed=%llu submitted_age=%llu scan=%u root=%p %s\n",
            state.affirmative_last_scan - state.started, state.affirmative_last_scan - state.submitted_tick.load(),
            state.affirmative_scans, static_cast<void*>(prompt.root), trace.c_str());
        std::fflush(stdout);
        state.affirmative_trace = trace;
    }
    // Diagnostics may inspect an unready prompt, but affirmative permission is
    // unchanged and must still be within the original four-second action window.
    if (!prompt.root || root_enabled || !enumerated || !prompt.matches || GetTickCount64() - state.started >= 4000) return;
    state.affirmative_attempted = true;
    NegativeButtons buttons{prompt.dialog};
    buttons.affirmative = true;
    if (prompt.matches == 1) EnumChildWindows(prompt.dialog, FindNegativeButton, reinterpret_cast<LPARAM>(&buttons));
    std::wstring verified;
    DWORD pid = 0, prompt_pid = 0, root_pid = 0;
    const DWORD thread = GetWindowThreadProcessId(buttons.match, &pid);
    const DWORD prompt_thread = GetWindowThreadProcessId(prompt.dialog, &prompt_pid);
    const DWORD root_thread = GetWindowThreadProcessId(prompt.root, &root_pid);
    const bool allowed = GetTickCount64() - state.started < 4000 && prompt.matches == 1 && buttons.complete && buttons.matches == 1 &&
        IsWindowVisible(prompt.dialog) && IsWindowEnabled(prompt.dialog) && !IsWindowEnabled(prompt.root) &&
        GetWindow(prompt.dialog, GW_OWNER) == prompt.root && IsChild(prompt.dialog, buttons.match) &&
        pid == GetCurrentProcessId() && prompt_pid == pid && root_pid == pid &&
        thread == root_thread && prompt_thread == root_thread &&
        IsWindowVisible(buttons.match) && IsWindowEnabled(buttons.match) && ReadPromptButton(buttons.match, verified) &&
        verified == buttons.text && ExplicitAffirmativeButton(verified);
    DWORD error = ERROR_SUCCESS;
    if (allowed) {
        state.affirmative_posted = PostMessageW(buttons.match, BM_CLICK, 0, 0) != FALSE;
        if (!state.affirmative_posted) error = GetLastError();
    }
    state.affirmative_refused = !allowed || !state.affirmative_posted;
    std::printf("[PROMPT_AFFIRMATIVE] root=%p dialog=%p button=%p prompts=%u complete=%d matches=%u caption=%s allowed=%d posted=%d error=%lu; post alone is not acceptance\n",
        static_cast<void*>(prompt.root), static_cast<void*>(prompt.dialog), static_cast<void*>(buttons.match),
        prompt.matches, buttons.complete, buttons.matches, UnicodeEscapes(verified.empty() ? buttons.text : verified).c_str(),
        allowed, state.affirmative_posted, error);
    unsigned controls = 0;
    if (!allowed) EnumChildWindows(prompt.dialog, DiagnosePromptChild, reinterpret_cast<LPARAM>(&controls));
    std::fflush(stdout);
}
BOOL CALLBACK ScanPrompt(HWND hwnd, LPARAM context) {
    auto& scan = *reinterpret_cast<PromptScan*>(context);
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(hwnd, &process);
    wchar_t class_name[64]{};
    GetClassNameW(hwnd, class_name, 64);
    if (hwnd == scan.root || process != GetCurrentProcessId() ||
        std::wstring_view(class_name) != L"#32770" || !OwnedBy(hwnd, scan.root)) return TRUE;
    const bool visible = IsWindowVisible(hwnd) != FALSE;
    // A disabled native root may be blocked by an owned dialog that has not
    // become visible on the private desktop. Unwind only its own enabled,
    // same-STA direct popup; never reinterpret this as a visible rejection.
    const bool hidden_modal = !visible && !IsWindowEnabled(scan.root) && IsWindowEnabled(hwnd) &&
        GetWindow(hwnd, GW_OWNER) == scan.root &&
        thread == GetWindowThreadProcessId(scan.root, nullptr) && !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD);
    if (scan.diagnose) {
        std::printf("[OWNED_PROMPT] hwnd=%p root=%p visible=%d enabled=%d root_enabled=%d same_sta=%d hidden_unwind=%d cancel=%p no=%p ok=%p yes=%p\n",
            static_cast<void*>(hwnd), static_cast<void*>(scan.root), visible, IsWindowEnabled(hwnd), IsWindowEnabled(scan.root),
            thread == GetWindowThreadProcessId(scan.root, nullptr), hidden_modal,
            static_cast<void*>(GetDlgItem(hwnd, IDCANCEL)), static_cast<void*>(GetDlgItem(hwnd, IDNO)),
            static_cast<void*>(GetDlgItem(hwnd, IDOK)), static_cast<void*>(GetDlgItem(hwnd, IDYES)));
        unsigned controls = 0;
        EnumChildWindows(hwnd, DiagnosePromptChild, reinterpret_cast<LPARAM>(&controls));
        std::printf("[OWNED_PROMPT] tick=%llu descendants=%u last_active_popup=%p\n", GetTickCount64(), controls,
            static_cast<void*>(GetLastActivePopup(scan.root)));
    }
    if (!visible && !hidden_modal) return TRUE;
    scan.found = true;
    scan.visible_found = scan.visible_found || visible;
    if (scan.dismiss) {
        // Only the current test's owned prompt, never a user desktop window.
        const HWND cancel = GetDlgItem(hwnd, IDCANCEL);
        const HWND no = GetDlgItem(hwnd, IDNO);
        BOOL posted = FALSE;
        const char* action = "WM_CLOSE";
        if (cancel && IsWindowEnabled(cancel)) action = "IDCANCEL";
        else if (no && IsWindowEnabled(no)) action = "IDNO";
        if (cancel && IsWindowEnabled(cancel)) posted = PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), reinterpret_cast<LPARAM>(cancel));
        else if (no && IsWindowEnabled(no)) posted = PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), reinterpret_cast<LPARAM>(no));
        else {
            NegativeButtons buttons{hwnd};
            if (visible) EnumChildWindows(hwnd, FindNegativeButton, reinterpret_cast<LPARAM>(&buttons));
            std::wstring verified;
            DWORD button_process = 0;
            const DWORD button_thread = GetWindowThreadProcessId(buttons.match, &button_process);
            if (buttons.complete && buttons.matches == 1 && IsChild(hwnd, buttons.match) &&
                button_process == GetCurrentProcessId() && button_thread == thread &&
                IsWindowVisible(buttons.match) && IsWindowEnabled(buttons.match) && OwnedBy(hwnd, scan.root) &&
                ReadPromptButton(buttons.match, verified) && verified == buttons.text && ExplicitNegativeButton(verified)) {
                // BM_CLICK routes BN_CLICKED to the button's actual parent,
                // including ID-zero buttons nested below DirectUI sinks.
                // https://learn.microsoft.com/windows/win32/controls/bm-click
                action = "BM_CLICK-explicit-negative";
                posted = PostMessageW(buttons.match, BM_CLICK, 0, 0);
                std::printf("[PROMPT_NEGATIVE] hwnd=%p button=%p unique=1 text=%s posted=%d\n",
                    static_cast<void*>(hwnd), static_cast<void*>(buttons.match), UnicodeEscapes(verified).c_str(), posted);
            } else {
                if (scan.diagnose) std::printf("[PROMPT_NEGATIVE] complete=%d matches=%u; no semantic button activated\n", buttons.complete, buttons.matches);
                posted = PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
        }
        const DWORD post_error = posted ? ERROR_SUCCESS : GetLastError();
        if (scan.diagnose) {
            std::printf("[PROMPT_CANCEL_POST] hwnd=%p action=%s posted=%d error=%lu tick=%llu; queue acceptance is not proof of dismissal\n",
                static_cast<void*>(hwnd), action, posted, post_error, GetTickCount64());
            std::fflush(stdout);
        }
        if (posted) ++scan.dismissed;
    }
    return TRUE;
}

bool AppendJournal(const std::filesystem::path& root, const std::string& record) {
    HANDLE file = CreateFileW((root / L"native-client-guids.tsv").c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = record.size() <= MAXDWORD && WriteFile(file, record.data(),
        static_cast<DWORD>(record.size()), &written, nullptr) && written == record.size() && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

struct NativeWindowSearch { HWND owner; HWND result = nullptr; };
BOOL CALLBACK FindOwnedNative(HWND hwnd, LPARAM context) {
    auto& search = *reinterpret_cast<NativeWindowSearch*>(context);
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    wchar_t name[64]{};
    GetClassNameW(hwnd, name, 64);
    if (process == GetCurrentProcessId() && GetWindow(hwnd, GW_OWNER) == search.owner &&
        IsWindowVisible(hwnd) && std::wstring_view(name) == L"#32770") search.result = hwnd;
    return TRUE;
}

struct EditSearch { HWND found = nullptr; unsigned matches = 0; bool diagnose = false; };
BOOL CALLBACK FindSeedEdit(HWND hwnd, LPARAM context) {
    auto& search = *reinterpret_cast<EditSearch*>(context);
    wchar_t cls[64]{}, value[256]{}; DWORD_PTR ignored = 0;
    GetClassNameW(hwnd, cls, 64);
    DWORD pid = 0;
    const DWORD thread = GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || thread != GetCurrentThreadId()) return TRUE;
    const bool is_edit = _wcsicmp(cls, L"Edit") == 0;
    const bool read = is_edit && SendMessageTimeoutW(hwnd, WM_GETTEXT, 256,
        reinterpret_cast<LPARAM>(value), SMTO_ABORTIFHUNG, 250, &ignored);
    if (search.diagnose) {
        std::printf("[CONTROL] hwnd=%p parent=%p id=%d class=%ls visible=%d enabled=%d edit_read=%d text=%ls\n",
            static_cast<void*>(hwnd), static_cast<void*>(GetParent(hwnd)), GetDlgCtrlID(hwnd), cls,
            IsWindowVisible(hwnd), IsWindowEnabled(hwnd), read, value);
    }
    if (read && IsWindowVisible(hwnd) && IsWindowEnabled(hwnd) &&
        std::wstring_view(value) == L"ordinary.txt") { search.found = hwnd; ++search.matches; }
    return TRUE;
}
bool EnterValidationFilename(HWND native, const wchar_t* input, bool diagnose) {
    EditSearch search; search.diagnose = diagnose; EnumChildWindows(native, FindSeedEdit, reinterpret_cast<LPARAM>(&search));
    if (search.matches != 1) { std::printf("[FACT] filename EDIT seed matches=%u; refusing ambiguous input\n", search.matches); return false; }
    DWORD_PTR ignored = 0; wchar_t value[256]{};
    const bool entered = SendMessageTimeoutW(search.found, EM_SETSEL, 0, -1, SMTO_ABORTIFHUNG, 250, &ignored) &&
        SendMessageTimeoutW(search.found, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(input), SMTO_ABORTIFHUNG, 250, &ignored) &&
        SendMessageTimeoutW(search.found, WM_GETTEXT, 256, reinterpret_cast<LPARAM>(value), SMTO_ABORTIFHUNG, 250, &ignored) &&
        std::wstring_view(value) == input;
    std::printf("[FACT] manual filename EDIT hwnd=%p input=%ls readback=%ls confirmed=%d\n", static_cast<void*>(search.found), input, value, entered);
    std::fflush(stdout); return entered;
}
void EmergencyCleanup(const RunState& state) {
    if (!state.validation || !state.validation->emergency_cleanup) return;
    const bool restored = state.validation->emergency_cleanup(state.validation->cleanup_context);
    std::printf("[FACT] emergency ACL restoration before test-process termination=%d\n", restored);
    if (!restored) ++cleanup_failures;
    std::fflush(stdout);
}
LRESULT CALLBACK OwnerProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<RunState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == kDrive && state && state->dialog) {
        ComPtr<IOleWindow> window;
        HWND native = nullptr;
        if (SUCCEEDED(state->dialog->QueryInterface(IID_PPV_ARGS(&window)))) window->GetWindow(&native);
        if (!native) {
            // The wrapper intentionally does not expose the inner IOleWindow.
            NativeWindowSearch search{hwnd};
            EnumThreadWindows(GetCurrentThreadId(), FindOwnedNative, reinterpret_cast<LPARAM>(&search));
            native = search.result;
        }
        if (native && IsWindowVisible(native)) {
            DWORD process = 0;
            const DWORD thread = GetWindowThreadProcessId(native, &process);
            state->visible = process == GetCurrentProcessId() && thread == GetCurrentThreadId();
            if (state->visible) state->native_window = native;
            if (state->visible && !state->submitted && GetTickCount64() - state->started >= 300) {
                if (state->cancel_case) {
                    state->submitted = true;
                    state->sta_close_attempted = true;
                    std::puts("[FACT] original STA entering explicit cancel Close");
                    std::fflush(stdout);
                    state->close_result = state->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
                    std::printf("[FACT] original STA explicit cancel Close returned=0x%08lX\n", static_cast<unsigned long>(state->close_result));
                    std::fflush(stdout);
                } else {
                    if (state->validation && !state->input_confirmed && state->input_attempts < 10) {
                        // Readiness is asynchronous. Retry only the exact seed, never guess an EDIT.
                        state->input_attempted = true;
                        ++state->input_attempts;
                        state->input_confirmed = EnterValidationFilename(native, state->validation->input,
                            state->input_attempts == 1 || state->input_attempts == 10);
                    }
                    const HWND accept = GetDlgItem(native, IDOK);
                    if ((!state->validation || state->input_confirmed) && accept && IsWindowEnabled(accept))
                        state->submitted = PostMessageW(native, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
                            reinterpret_cast<LPARAM>(accept)) != FALSE;
                    if (state->submitted && state->validation) state->submitted_tick = GetTickCount64();
                }
            }
        }
        if (state->folder_probe && state->submitted && !state->folder_probed && GetTickCount64() - state->started >= 1500) {
            state->folder_probed = true;
            ComPtr<IShellItem> current; PWSTR path = nullptr, name = nullptr;
            if (SUCCEEDED(state->dialog->GetFolder(&current))) current->GetDisplayName(SIGDN_FILESYSPATH, &path);
            state->dialog->GetFileName(&name);
            std::printf("[FACT] folder after first submit path=%ls name=%ls\n", path ? path : L"null", name ? name : L"null");
            CoTaskMemFree(path); CoTaskMemFree(name);
            if (native) { const HWND accept = GetDlgItem(native, IDOK); PostMessageW(native, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(accept)); }
        }
        if (GetTickCount64() - state->started >= 4000) {
            state->bounded_close = true;
            PromptScan prompts{state->native_window.load()};
            if (prompts.root) EnumThreadWindows(GetCurrentThreadId(), ScanPrompt, reinterpret_cast<LPARAM>(&prompts));
            // A separate HWND-only driver dismisses nested prompts first. Calling
            // COM Close from inside their validation stack may otherwise block.
            if (!state->sta_close_attempted && !prompts.found && prompts.root && IsWindowEnabled(prompts.root)) {
                state->sta_close_attempted = true;
                std::puts("[FACT] original STA entering IFileDialog::Close");
                std::fflush(stdout);
                state->close_result = state->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
                std::printf("[FACT] original STA Close returned=0x%08lX\n", static_cast<unsigned long>(state->close_result));
                std::fflush(stdout);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

// Test-only native callback: allows the readonly case to reach readonly handling
// without stopping at a preceding overwrite prompt. It does not write the file.
class NativeEvents final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileDialogEvents> {
public:
    explicit NativeEvents(bool accept_overwrite, IUnknown* expected = nullptr, int event_mode = 0)
        : accept_overwrite_(accept_overwrite), expected_(expected), event_mode_(event_mode) {}
    unsigned overwrite_calls = 0;
    unsigned file_ok_calls = 0;
    bool identity_matches = true;
    HRESULT callback_close = E_PENDING;
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        ++file_ok_calls;
        if (!event_mode_) return S_OK;
        ComPtr<IUnknown> actual;
        identity_matches = identity_matches && dialog &&
            SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&actual))) && actual.Get() == expected_.Get();
        if (event_mode_ == 2) {
            callback_close = dialog ? dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED)) : E_POINTER;
            std::printf("[FACT] OnFileOk wrapper Close=0x%08lX\n", static_cast<unsigned long>(callback_close));
            std::fflush(stdout);
        }
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override {
        if (!response) return E_POINTER;
        *response = FDESVR_DEFAULT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override {
        if (!response) return E_POINTER;
        ++overwrite_calls;
        *response = accept_overwrite_ ? FDEOR_ACCEPT : FDEOR_DEFAULT;
        return S_OK;
    }
private:
    bool accept_overwrite_;
    ComPtr<IUnknown> expected_;
    int event_mode_;
};

struct Observation {
    HRESULT show = E_UNEXPECTED;
    HRESULT result = E_PENDING;
    std::wstring path;
    bool submitted = false;
    bool bounded_close = false;
    bool input_confirmed = false, prompt_observed = false, remained_open = false;
    FILEOPENDIALOGOPTIONS actual_options = 0;
    bool affirmative_attempted = false, affirmative_posted = false, affirmative_refused = false;
};

Observation Run(HWND owner, const std::filesystem::path& root, const wchar_t* filename,
                bool save, FILEOPENDIALOGOPTIONS options, const char* label,
                bool cancel_case = false, bool readonly_case = false, int wrapper_events = 0,
                const OpenValidationDrive* validation = nullptr) {
    Observation observation;
    const auto& journal_directory = validation && validation->journal_directory ? *validation->journal_directory : root;
    ComPtr<IFileDialog> dialog;
    unsigned bridge_calls = 0;
    pulse::shell_bridge::FileDialogBridgeServices services;
    services.run_bridge = [&](const auto&, HWND, HANDLE) { ++bridge_calls; return pulse::ipc::picker::Message{}; };
    // validation_verified and show_native deliberately remain absent: real
    // native Show with the production default validation gate closed.
    HRESULT hr = wrapper_events ? pulse::shell_bridge::CreatePulseFileDialog(
        save ? pulse::shell_bridge::DialogFactory::Save : pulse::shell_bridge::DialogFactory::Open,
        services, IID_PPV_ARGS(&dialog)) : CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if ((options & FOS_PICKFOLDERS) || native_options_probe) {
        using Entry = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
        static HMODULE module = LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto entry = module ? reinterpret_cast<Entry>(GetProcAddress(module, "DllGetClassObject")) : nullptr;
        ComPtr<IClassFactory> factory;
        dialog.Reset(); hr = entry ? entry(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, IID_PPV_ARGS(&factory)) : E_FAIL;
        if (SUCCEEDED(hr)) hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
    }
    if (FAILED(hr)) { Check(false, "native dialog created"); return observation; }
    GUID guid{};
    hr = CoCreateGuid(&guid);
    wchar_t guid_text[40]{};
    if (SUCCEEDED(hr) && !StringFromGUID2(guid, guid_text, 40)) hr = E_FAIL;
    std::string guid_ascii;
    for (const wchar_t ch : std::wstring_view(guid_text)) guid_ascii.push_back(static_cast<char>(ch));
    if (SUCCEEDED(hr) && !AppendJournal(journal_directory, std::string(label) + "\t" + (save ? "Save" : "Open") +
        "\t" + guid_ascii + "\tallocated-before-SetClientGuid\r\n")) hr = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    if (FAILED(hr)) { Check(false, "native history GUID persisted before use"); return observation; }
    if (SUCCEEDED(hr)) hr = dialog->SetClientGuid(guid);
    std::printf("[FACT] %s ClientGuid=%s SetClientGuid HRESULT=0x%08lX\n", label, guid_ascii.c_str(), static_cast<unsigned long>(hr));
    if (FAILED(hr)) { Check(false, "unique native history GUID installed"); return observation; }
    // SetClientGuid must precede other configuration. Clear only this test GUID.
    // https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifiledialog-clearclientdata
    struct ClearHistory {
        IFileDialog* dialog;
        const std::filesystem::path& root;
        const char* label;
        std::string guid;
        bool validation_mode;
        bool save;
        ~ClearHistory() {
            HRESULT cleared = dialog->ClearClientData();
            if (native_options_probe && FAILED(cleared)) {
                ComPtr<IFileDialog> fresh;
                GUID id{};
                const std::wstring wide(guid.begin(), guid.end());
                HRESULT retried = CLSIDFromString(wide.c_str(), &id);
                if (SUCCEEDED(retried)) retried = NativeDialog(save, &fresh);
                if (SUCCEEDED(retried)) retried = fresh->SetClientGuid(id);
                if (SUCCEEDED(retried)) retried = fresh->ClearClientData();
                std::printf("[FACT] fresh native GUID cleanup original=0x%08lX retry=0x%08lX\n",
                    static_cast<unsigned long>(cleared), static_cast<unsigned long>(retried));
                cleared = retried;
            }
            char code[32]{};
            std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(cleared));
            std::printf("[FACT] %s ClientGuid=%s ClearClientData HRESULT=%s\n", label, guid.c_str(), code);
            const bool persisted = AppendJournal(root, std::string(label) + "\t" + guid + "\tClearClientData\t" + code + "\r\n");
            if (validation_mode) {
                CleanupCheck(persisted, "native GUID cleanup result persisted");
                CleanupCheck(SUCCEEDED(cleared), "test GUID dialog history cleared");
            } else {
                Check(persisted, "native GUID cleanup result persisted");
                Check(SUCCEEDED(cleared), "test GUID dialog history cleared");
            }
        }
    } clear{dialog.Get(), journal_directory, label, guid_ascii, validation != nullptr, save};
    ComPtr<IShellItem> folder;
    hr = SHCreateItemFromParsingName(root.c_str(), nullptr, IID_PPV_ARGS(&folder));
    if (SUCCEEDED(hr)) hr = dialog->SetFolder(folder.Get());
    if (SUCCEEDED(hr) && (options & FOS_PICKFOLDERS)) hr = dialog->SetOptions(options);
    if (SUCCEEDED(hr) && !validation) hr = dialog->SetFileName(filename);
    if (SUCCEEDED(hr) && !(options & FOS_PICKFOLDERS)) hr = dialog->SetOptions(options);
    if (validation && SUCCEEDED(hr)) {
        const COMDLG_FILTERSPEC filters[] = {{L"Text", L"*.txt"}};
        hr = dialog->SetFileTypes(1, filters);
        if (SUCCEEDED(hr)) hr = dialog->SetFileTypeIndex(1);
        // Set the seed after type configuration, which may update filename state.
        if (SUCCEEDED(hr)) hr = dialog->SetFileName(filename);
        PWSTR seed = nullptr;
        const HRESULT seed_hr = SUCCEEDED(hr) ? dialog->GetFileName(&seed) : hr;
        const bool seed_ok = SUCCEEDED(seed_hr) && seed && std::wstring_view(seed) == filename;
        std::printf("[FACT] configured filename seed HRESULT=0x%08lX value=%ls exact=%d\n",
            static_cast<unsigned long>(seed_hr), seed ? seed : L"<null>", seed_ok);
        CoTaskMemFree(seed);
        Check(seed_ok, "native filename seed read back before Show");
        if (!seed_ok && SUCCEEDED(hr)) hr = E_FAIL;
    }
    FILEOPENDIALOGOPTIONS actual{};
    if (SUCCEEDED(hr)) hr = dialog->GetOptions(&actual);
    std::printf("[FACT] %s configuration HRESULT=0x%08lX requested=0x%08lX actual=0x%08lX\n",
        label, static_cast<unsigned long>(hr), static_cast<unsigned long>(options), static_cast<unsigned long>(actual));
    if (FAILED(hr)) { Check(false, "native candidate options configured"); return observation; }
    observation.actual_options = actual;
    if (validation) Check(actual == options, "validation sampler preserves exact native options");
    ComPtr<IUnknown> expected;
    if (wrapper_events) Check(SUCCEEDED(dialog.As(&expected)), "wrapper canonical identity obtained");
    auto events = Microsoft::WRL::Make<NativeEvents>(readonly_case, expected.Get(), wrapper_events);
    DWORD cookie = 0;
    if ((readonly_case || wrapper_events) && (!events || FAILED(dialog->Advise(events.Get(), &cookie)))) {
        Check(false, "readonly native observation event installed");
        return observation;
    }
    RunState state{dialog.Get(), GetTickCount64(), cancel_case};
    state.folder_probe = (options & FOS_PICKFOLDERS) != 0;
    state.validation = validation;
    SetWindowLongPtrW(owner, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!finished) { SetWindowLongPtrW(owner, GWLP_USERDATA, 0); if (cookie) dialog->Unadvise(cookie); return observation; }
    const HDESK isolated_desktop = GetThreadDesktop(GetCurrentThreadId());
    unsigned dismissed = 0;
    std::thread watchdog([finished, isolated_desktop, owner, &state, &dismissed, &root, label] {
        if (!SetThreadDesktop(isolated_desktop)) {
            std::printf("[FAIL] controller SetThreadDesktop error=%lu\n", GetLastError());
            std::fflush(stdout);
            if (state.validation) {
                std::printf("[FACT] watchdog case=%s fixture_directory=%ls input=%ls; ACL recovery journal remains in this directory when applicable\n",
                    label, root.c_str(), state.validation->input);
                std::fflush(stdout);
            }
            EmergencyCleanup(state);
            TerminateProcess(GetCurrentProcess(), 98);
            return;
        }
        bool diagnosed = false, cancel_attempted = false, close_attempted = false;
        unsigned prompt_diagnostic_stage = 0;
        while (WaitForSingleObject(finished, 50) == WAIT_TIMEOUT) {
            PostMessageW(owner, kDrive, 0, 0);
            const auto elapsed = GetTickCount64() - state.started;
            const auto submitted = state.submitted_tick.load();
            if (state.validation && submitted && elapsed < 4000) {
                PromptScan prompts{state.native_window.load()};
                if (prompts.root) EnumDesktopWindows(isolated_desktop, ScanPrompt, reinterpret_cast<LPARAM>(&prompts));
                if (prompts.visible_found) state.prompt_observed = true;
                if (state.validation->accept_owned_overwrite) AcceptOwnedOverwrite(isolated_desktop, state);
                if (prompts.root && IsWindowVisible(prompts.root) && GetTickCount64() - submitted >= 500) state.remained_open = true;
            }
            if (elapsed >= 4000 && !diagnosed) {
                if (state.validation && state.validation->accept_owned_overwrite) {
                    std::printf("[AFFIRMATIVE_DEADLINE] elapsed=%llu scans=%u last_scan_elapsed=%llu submitted_age=%llu attempted=%d posted=%d trace=%s\n",
                        elapsed, state.affirmative_scans, state.affirmative_last_scan ? state.affirmative_last_scan - state.started : 0,
                        submitted ? GetTickCount64() - submitted : 0, state.affirmative_attempted, state.affirmative_posted,
                        state.affirmative_trace.c_str());
                }
                EnumDesktopWindows(isolated_desktop, DiagnoseWindow, 0);
                std::fflush(stdout);
                diagnosed = true;
            }
            if (GetTickCount64() - state.started >= 4000) {
                PromptScan prompts{state.native_window.load(), false, 0, true};
                const unsigned stage = elapsed >= 10000 ? 3u : elapsed >= 6000 ? 2u : 1u;
                prompts.diagnose = stage > prompt_diagnostic_stage;
                if (prompts.root) EnumDesktopWindows(isolated_desktop, ScanPrompt, reinterpret_cast<LPARAM>(&prompts));
                prompt_diagnostic_stage = stage;
                dismissed += prompts.dismissed;
            }
            if ((elapsed >= 4000 && !cancel_attempted) || (elapsed >= 6000 && !close_attempted)) {
                const bool close = cancel_attempted;
                NativeCancel request{state.native_window.load(), close};
                // Enumeration proves desktop membership, callback checks PID,
                // exact captured native HWND and class before posting cancel.
                if (request.root) EnumDesktopWindows(isolated_desktop, CancelNative, reinterpret_cast<LPARAM>(&request));
                if (request.posted) state.test_cancel_sent = true;
                if (request.posted) {
                    if (close) close_attempted = true; else cancel_attempted = true;
                }
            }
            if (GetTickCount64() - state.started < 12000) continue;
            std::puts("[FAIL] native modal Close did not finish within 12 seconds; ending only this test process. Fixture/history cleanup may be incomplete.");
            std::printf("[FACT] modal timeout negative_prompt_posts=%u native_cancel_posted=%d root=%p root_enabled=%d\n",
                dismissed, state.test_cancel_sent.load(), static_cast<void*>(state.native_window.load()),
                IsWindowEnabled(state.native_window.load()));
            std::fflush(stdout);
            if (state.validation) {
                std::printf("[FACT] watchdog case=%s fixture_directory=%ls input=%ls; ACL recovery journal remains in this directory when applicable\n",
                    label, root.c_str(), state.validation->input);
                std::fflush(stdout);
            }
            EmergencyCleanup(state);
            TerminateProcess(GetCurrentProcess(), 99);
        }
    });
    observation.show = dialog->Show(owner);
    SetEvent(finished);
    watchdog.join();
    CloseHandle(finished);
    std::printf("[FACT] %s nested owned prompt dismissal posts=%u\n", label, dismissed);
    SetWindowLongPtrW(owner, GWLP_USERDATA, 0);
    MSG pending{};
    while (PeekMessageW(&pending, owner, kDrive, kDrive, PM_REMOVE)) {}
    if (cookie) Check(SUCCEEDED(dialog->Unadvise(cookie)), "native event subscription removed");
    observation.submitted = state.submitted;
    observation.input_confirmed = state.input_confirmed;
    observation.affirmative_attempted = state.affirmative_attempted;
    observation.affirmative_posted = state.affirmative_posted;
    observation.affirmative_refused = state.affirmative_refused;
    observation.prompt_observed = state.prompt_observed.load(); observation.remained_open = state.remained_open.load();
    if (validation) std::printf("[FACT] %s input_verified=%d prompt_after_submit=%d visible_after_submit_500ms=%d\n",
        label, observation.input_confirmed, observation.prompt_observed, observation.remained_open);
    observation.bounded_close = state.bounded_close || state.test_cancel_sent.load();
    ComPtr<IShellItem> item;
    observation.result = dialog->GetResult(&item);
    if (SUCCEEDED(observation.result) && item) {
        PWSTR path = nullptr;
        observation.result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        if (SUCCEEDED(observation.result) && path) observation.path = path;
        CoTaskMemFree(path);
    }
    const bool cancelled = observation.show == HRESULT_FROM_WIN32(ERROR_CANCELLED);
    const char* disposition = SUCCEEDED(observation.show) ?
        (SUCCEEDED(observation.result) && !observation.path.empty() ? "accepted" : "show-succeeded-invalid-result") :
        cancelled ? (observation.bounded_close ? "still-open-then-test-cancel" :
            cancel_case ? "test-cancelled" : "native-cancelled") : "native-failure";
    // Driver actions can race a successful modal return. Report them separately
    // rather than overriding the final native HRESULT/result with driver intent.
    std::printf("[FACT] %s timeout_reached=%d sta_close_attempted=%d native_cancel_posted=%d\n",
        label, state.bounded_close, state.sta_close_attempted, state.test_cancel_sent.load());
    std::printf("[FACT] %s state=%s visible=%d submitted=%d Show=0x%08lX GetResult=0x%08lX Close=0x%08lX overwrite_callbacks=%u\n",
        label, disposition, state.visible, state.submitted, static_cast<unsigned long>(observation.show),
        static_cast<unsigned long>(observation.result), static_cast<unsigned long>(state.close_result), events ? events->overwrite_calls : 0);
    Check(state.visible && state.submitted, "native dialog was driven on its own non-active desktop");
    if (wrapper_events) {
        Check(bridge_calls == 0, "closed validation gate and events use real native Show");
        Check(events->file_ok_calls > 0 && events->identity_matches, "real OnFileOk exposes wrapper IUnknown identity");
        Check(observation.show == HRESULT_FROM_WIN32(ERROR_CANCELLED) && FAILED(observation.result) && observation.path.empty(),
            "OnFileOk S_FALSE never yields an accepted result");
        if (wrapper_events == 1) Check(observation.bounded_close, "S_FALSE leaves dialog open until bounded test cancellation");
        if (wrapper_events == 2) Check(SUCCEEDED(events->callback_close) && !observation.bounded_close,
            "real callback Close ends Show before timeout cancellation");
    }
    return observation;
}

bool Matches(const Observation& result, const std::filesystem::path& path) {
    return SUCCEEDED(result.show) && SUCCEEDED(result.result) &&
        CompareStringOrdinal(result.path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool MakeFile(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, "native-fixture", 14, &written, nullptr) && written == 14;
    CloseHandle(file);
    return ok;
}


// This guard only receives a CREATE_NEW fixture in this run's exclusive directory.
class IsolatedFileAcl {
public:
    explicit IsolatedFileAcl(const std::filesystem::path& path) : path_(path) {}
    ~IsolatedFileAcl() {
        Restore();
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        if (original_) LocalFree(original_);
    }
    bool Install(bool allow_read_data = false, bool directory = false) {
        file_ = CreateFileW(path_.c_str(), READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (file_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file_, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            (((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) ||
            (!directory && info.nNumberOfLinks != 1)) return false;
        if (GetSecurityInfo(file_, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
            &original_acl_, nullptr, &original_) != ERROR_SUCCESS) return false;
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(original_, &control_, &revision)) return false;
        LPWSTR sddl = nullptr;
        if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(original_, SDDL_REVISION_1,
            DACL_SECURITY_INFORMATION, &sddl, nullptr)) return false;
        // Persist recovery evidence before changing the new file's DACL.
        const auto journal_path = path_.parent_path() / L"acl-recovery.txt";
        HANDLE journal = CreateFileW(journal_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        const std::wstring record = L"fixture=" + path_.wstring() + L"\r\noriginal_dacl=" + sddl + L"\r\n";
        LocalFree(sddl);
        DWORD written = 0;
        const DWORD bytes = static_cast<DWORD>(record.size() * sizeof(wchar_t));
        const bool persisted = journal != INVALID_HANDLE_VALUE && WriteFile(journal, record.data(), bytes, &written, nullptr) && written == bytes && FlushFileBuffers(journal);
        if (journal != INVALID_HANDLE_VALUE) CloseHandle(journal);
        if (!persisted) return false;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        DWORD needed = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        std::vector<BYTE> user(needed);
        const bool token_ok = GetTokenInformation(token, TokenUser, user.data(), needed, &needed) != FALSE;
        CloseHandle(token);
        if (!token_ok) return false;
        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = FILE_READ_ATTRIBUTES | SYNCHRONIZE | READ_CONTROL | WRITE_DAC | DELETE |
            (allow_read_data ? FILE_READ_DATA : 0) |
            (directory ? FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_EA : 0);
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
        entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
        PACL restricted = nullptr;
        if (SetEntriesInAclW(1, &entry, nullptr, &restricted) != ERROR_SUCCESS) return false;
        const DWORD result = SetSecurityInfo(file_, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, restricted, nullptr);
        LocalFree(restricted);
        changed_ = result == ERROR_SUCCESS;
        std::printf("[FACT] ACL fixture=%ls SetSecurityInfo=%lu directory=%d; current SID retains attributes/WRITE_DAC/DELETE file-read-data-granted=%d directory-create-not-granted=%d\n",
            path_.c_str(), result, directory, allow_read_data, directory);
        std::fflush(stdout);
        return changed_;
    }
    bool Restore() {
        std::lock_guard lock(mutex_);
        if (!changed_) return true;
        const DWORD result = SetSecurityInfo(file_, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION |
            ((control_ & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
            nullptr, nullptr, original_acl_, nullptr);
        PSECURITY_DESCRIPTOR verified = nullptr;
        PACL acl = nullptr;
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        const DWORD queried = GetSecurityInfo(file_, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &acl, nullptr, &verified);
        const bool same_acl = queried == ERROR_SUCCESS &&
            ((!acl && !original_acl_) || (acl && original_acl_ && acl->AclSize == original_acl_->AclSize &&
                std::memcmp(acl, original_acl_, acl->AclSize) == 0));
        const bool ok = result == ERROR_SUCCESS && same_acl && GetSecurityDescriptorControl(verified, &control, &revision) &&
            (control & SE_DACL_PROTECTED) == (control_ & SE_DACL_PROTECTED);
        if (verified) LocalFree(verified);
        if (ok) changed_ = false;
        std::printf("[FACT] ACL restore fixture=%ls set=%lu query=%lu exact_dacl_and_protection=%d\n",
            path_.c_str(), result, queried, ok);
        std::fflush(stdout);
        return ok;
    }
    static bool Emergency(void* self) { return static_cast<IsolatedFileAcl*>(self)->Restore(); }
private:
    std::filesystem::path path_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    PSECURITY_DESCRIPTOR original_ = nullptr;
    PACL original_acl_ = nullptr;
    SECURITY_DESCRIPTOR_CONTROL control_ = 0;
    bool changed_ = false;
    std::mutex mutex_;
};

int RunOpenValidation(HWND owner, const std::filesystem::path& root, bool no_validate = false) {
    const char* mode = no_validate ? "open-novalidate" : "open-validation";
    const FILEOPENDIALOGOPTIONS options = FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT |
        FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | (no_validate ? FOS_NOVALIDATE : 0);
    std::printf("[FACT] mode=%s requested_options=0x%08lX only_difference_from_validation=0x%08lX\n",
        mode, static_cast<unsigned long>(options), static_cast<unsigned long>(no_validate ? FOS_NOVALIDATE : 0));
    const wchar_t* names[] = {L"ordinary.txt", L"locked.txt", L"manual.bin", L"attributes.txt"};
    bool created[4]{};
    for (size_t i = 0; i < 4; ++i) {
        HANDLE file = CreateFileW((root / names[i]).c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        created[i] = file != INVALID_HANDLE_VALUE;
        DWORD written = 0;
        const bool ready = created[i] && WriteFile(file, "native-fixture", 14, &written, nullptr) && written == 14;
        if (created[i]) CloseHandle(file);
        Check(ready, "CREATE_NEW isolated validation fixture");
        if (!ready) {
            for (size_t j = 0; j <= i; ++j) if (created[j])
                CleanupCheck(DeleteFileW((root / names[j]).c_str()) != FALSE, "partial fixture creation cleaned up");
            std::printf("[SUMMARY] %s functional_failures=%d cleanup_failures=%d\n", mode, failures, cleanup_failures.load());
            return 1;
        }
    }
    auto sample = [&](const wchar_t* name, const char* label, bool must_accept, bool must_reject,
                      IsolatedFileAcl* acl = nullptr) {
        const std::string case_label = no_validate ? std::string("novalidate-") + label : label;
        label = case_label.c_str();
        OpenValidationDrive drive{name, acl ? IsolatedFileAcl::Emergency : nullptr, acl};
        const auto result = Run(owner, root, L"ordinary.txt", false, options, label, false, false, 0, &drive);
        const bool accepted = Matches(result, root / name);
        const bool no_result = FAILED(result.show) && FAILED(result.result) && result.path.empty();
        const bool evidenced_rejection = no_result && result.prompt_observed;
        const bool native_failure = no_result && !result.bounded_close && result.show != HRESULT_FROM_WIN32(ERROR_CANCELLED);
        std::printf("[SAMPLE] %s fixture=%ls options=0x%08lX filter=*.txt index=1 outcome=%s\n", label,
            (root / name).c_str(), static_cast<unsigned long>(result.actual_options), accepted ? "accepted-exact" :
            evidenced_rejection ? "owned-prompt-nonacceptance" : native_failure ? "native-failure" : "inconclusive-harness-cancel");
        Check(result.input_confirmed && result.submitted, "sampler typed and submitted exact filename");
        Check(accepted || evidenced_rejection || native_failure, "sampler obtains terminal or prompt-backed evidence");
        Check(accepted || no_result, "sampler returns exact target or no result");
        if (result.show == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            Check(FAILED(result.result) && result.path.empty(), "sampler cancellation has no result");
        // Preserve established default-mode assertions. NOVALIDATE is a fresh
        // observation, including its interaction with FILEMUSTEXIST.
        if (!no_validate && must_accept) Check(accepted, "ordinary Open accepts exact target");
        if (!no_validate && must_reject) Check(no_result, "missing target is not accepted");
    };
    if (created[0]) sample(names[0], "validation-ordinary", true, false);
    if (created[0] && created[1]) {
        const auto path = root / names[1];
        HANDLE exclusive = CreateFileW(path.c_str(), FILE_READ_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE probe = CreateFileW(path.c_str(), FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD error = probe == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
        const bool ready = exclusive != INVALID_HANDLE_VALUE && error == ERROR_SHARING_VIOLATION && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        std::printf("[FACT] lock probe FILE_READ_DATA error=%lu (does not infer native access/share masks)\n", error);
        Check(ready, "exclusive fixture independently denies read-data sharing");
        if (ready) sample(names[1], "validation-exclusive", false, false);
        if (exclusive != INVALID_HANDLE_VALUE) CloseHandle(exclusive);
    }
    const auto missing = root / L"missing.txt";
    const bool absent = GetFileAttributesW(missing.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
    Check(absent, "missing fixture proven absent before Show");
    if (created[0] && absent) sample(L"missing.txt", "validation-missing", false, true);
    Check(GetFileAttributesW(missing.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
        "Open did not create missing fixture");
    if (created[0] && created[2]) sample(names[2], "validation-outside-filter", false, false);
    bool restored = true;
    if (created[3]) {
        const auto path = root / names[3];
        IsolatedFileAcl acl(path);
        const bool installed = acl.Install();
        Check(installed, "owned ACL fixture installed with recovery journal");
        if (installed) {
            WIN32_FILE_ATTRIBUTE_DATA info{};
            const bool attributes = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) != FALSE;
            HANDLE attr = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            HANDLE data = CreateFileW(path.c_str(), FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            const DWORD error = data == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
            const bool ready = attributes && attr != INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED;
            std::printf("[FACT] ACL GetFileAttributesEx=%d FILE_READ_ATTRIBUTES=%d FILE_READ_DATA error=%lu\n", attributes, attr != INVALID_HANDLE_VALUE, error);
            if (attr != INVALID_HANDLE_VALUE) CloseHandle(attr);
            if (data != INVALID_HANDLE_VALUE) CloseHandle(data);
            Check(ready, "ACL precondition attributes readable and read-data denied");
            if (ready && created[0]) sample(names[3], "validation-attributes-only", false, false, &acl);
        }
        restored = acl.Restore();
        CleanupCheck(restored, "original owned-file DACL and protection restored and verified");
    }
    for (size_t i = 0; i < 4; ++i) {
        if (created[i] && (i != 3 || restored)) {
            HANDLE file = CreateFileW((root / names[i]).c_str(), FILE_READ_DATA, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            char content[15]{};
            DWORD read = 0;
            const bool unchanged = file != INVALID_HANDLE_VALUE && ReadFile(file, content, 15, &read, nullptr) &&
                read == 14 && std::memcmp(content, "native-fixture", 14) == 0;
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            Check(unchanged, "Open left owned fixture bytes unchanged");
        }
        if (created[i] && (i != 3 || restored)) CleanupCheck(DeleteFileW((root / names[i]).c_str()) != FALSE, "owned fixture removed; journals retained");
    }
    std::printf("[SUMMARY] %s functional_failures=%d cleanup_failures=%d fixture_directory=%ls\n",
        mode, failures, cleanup_failures.load(), root.c_str());
    return failures || cleanup_failures.load() ? 1 : 0;
}


// Probe only the explicitly requested access; never read bytes or infer native masks.
DWORD ProbeExistingAccess(const std::filesystem::path& path, DWORD access) {
    HANDLE file = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD error = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return error;
}

int RunOpenSharing(HWND owner, const std::filesystem::path& root) {
    const FILEOPENDIALOGOPTIONS options = FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT |
        FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
    // One owned file suffices: each holder is closed and each mutation restored
    // before the next observation. Native dialogs use a new GUID every time.
    const auto file = root / L"ordinary.txt";
    HANDLE created = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (created == INVALID_HANDLE_VALUE) {
        Check(false, "sharing fixture exclusively created");
        return 1;
    }
    DWORD written = 0;
    const bool written_ok = WriteFile(created, "native-fixture", 14, &written, nullptr) && written == 14;
    CloseHandle(created);
    Check(written_ok, "sharing fixture initialized");
    if (!written_ok) {
        CleanupCheck(DeleteFileW(file.c_str()) != FALSE, "partial sharing fixture removed");
        return 1;
    }
    auto sample = [&](const char* label, IsolatedFileAcl* acl = nullptr) {
        const DWORD before = ProbeExistingAccess(file, FILE_READ_DATA);
        std::printf("[FACT] %s probe-before desired_access=0x%08lX share=0x00000007 disposition=OPEN_EXISTING error=%lu fixture=%ls\n",
            label, static_cast<unsigned long>(FILE_READ_DATA), before, file.c_str());
        OpenValidationDrive drive{L"ordinary.txt", acl ? IsolatedFileAcl::Emergency : nullptr, acl};
        const auto result = Run(owner, root, L"ordinary.txt", false, options, label, false, false, 0, &drive);
        const DWORD after = ProbeExistingAccess(file, FILE_READ_DATA);
        const bool accepted = Matches(result, file);
        const bool no_result = FAILED(result.show) && FAILED(result.result) && result.path.empty();
        const bool prompted = no_result && result.prompt_observed;
        const bool native_failure = no_result && !result.bounded_close && result.show != HRESULT_FROM_WIN32(ERROR_CANCELLED);
        const char* outcome = accepted ? "accepted-exact" : prompted ? "owned-prompt-nonacceptance" :
            native_failure ? "native-failure" : "inconclusive-harness-cancel";
        std::printf("[SAMPLE] %s native=%s read-data-probe-before=%lu after=%lu options=0x%08lX filter=*.txt index=1 comparison=%s\n",
            label, outcome, before, after, static_cast<unsigned long>(result.actual_options),
            accepted ? (before == ERROR_SUCCESS ? "both-accept" : "native-accepts-probe-fails") :
            prompted ? (before == ERROR_SUCCESS ? "native-prompts-probe-succeeds" : "native-prompts-probe-fails") : "unclassified");
        Check(result.input_confirmed && result.submitted, "sharing sampler typed and submitted exact filename");
        Check(accepted || no_result, "sharing sampler returns exact fixture or no result");
        Check(accepted || prompted || native_failure, "sharing sampler has terminal or prompt-backed evidence");
        Check(before == after, "probe evidence stable across the observed native dialog");
    };

    const bool readonly = SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE;
    const DWORD readonly_attributes = GetFileAttributesW(file.c_str());
    const bool readonly_verified = readonly && readonly_attributes != INVALID_FILE_ATTRIBUTES &&
        (readonly_attributes & FILE_ATTRIBUTE_READONLY) != 0;
    Check(readonly_verified, "readonly sharing fixture attribute verified");
    if (readonly_verified) sample("sharing-readonly");
    const bool attr_restored = SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE;
    CleanupCheck(attr_restored && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES &&
        !(GetFileAttributesW(file.c_str()) & FILE_ATTRIBUTE_READONLY), "sharing readonly attribute restored");

    struct HolderCase { const char* label; DWORD access; DWORD share; };
    const HolderCase cases[] = {
        {"sharing-reader-share-read", FILE_READ_DATA, FILE_SHARE_READ},
        {"sharing-reader-share-write", FILE_READ_DATA, FILE_SHARE_WRITE},
        {"sharing-reader-share-all", FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE},
        {"sharing-writer-share-read", FILE_WRITE_DATA, FILE_SHARE_READ},
        {"sharing-writer-share-write", FILE_WRITE_DATA, FILE_SHARE_WRITE},
        {"sharing-writer-share-all", FILE_WRITE_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE},
    };
    if (attr_restored) for (const auto& test : cases) {
        HANDLE holder = CreateFileW(file.c_str(), test.access, test.share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD error = holder == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        std::printf("[FACT] %s holder access=0x%08lX share=0x%08lX error=%lu\n", test.label,
            static_cast<unsigned long>(test.access), static_cast<unsigned long>(test.share), error);
        Check(holder != INVALID_HANDLE_VALUE, "sharing fixture holder acquired with exact requested flags");
        if (holder != INVALID_HANDLE_VALUE) {
            sample(test.label);
            CleanupCheck(CloseHandle(holder) != FALSE, "sharing fixture holder released");
            Check(ProbeExistingAccess(file, FILE_READ_DATA) == ERROR_SUCCESS, "read-data access succeeds after holder release");
        }
    }

    bool acl_restored = true;
    {
        IsolatedFileAcl acl(file);
        const bool installed = acl.Install(true);
        Check(installed, "read-data-only ACL fixture installed with original DACL journal");
        if (installed) {
            const DWORD read_data = ProbeExistingAccess(file, FILE_READ_DATA);
            const DWORD generic_read = ProbeExistingAccess(file, GENERIC_READ);
            const DWORD read_ea = ProbeExistingAccess(file, FILE_READ_EA);
            std::printf("[FACT] ACL distinction FILE_READ_DATA=%lu GENERIC_READ=%lu FILE_READ_EA=%lu fixture=%ls\n",
                read_data, generic_read, read_ea, file.c_str());
            const bool ready = read_data == ERROR_SUCCESS && generic_read == ERROR_ACCESS_DENIED && read_ea == ERROR_ACCESS_DENIED;
            Check(ready, "ACL independently permits read-data but denies generic-read extra rights");
            if (ready) sample("sharing-read-data-without-read-ea", &acl);
        }
        acl_restored = acl.Restore();
        CleanupCheck(acl_restored, "sharing original DACL and protection restored and verified");
    }
    if (acl_restored) {
        // The exclusive opener also proves the sampler leaked no probe/holder handles.
        HANDLE verify = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        char bytes[15]{};
        DWORD read = 0;
        const bool unchanged = verify != INVALID_HANDLE_VALUE && ReadFile(verify, bytes, 15, &read, nullptr) &&
            read == 14 && std::memcmp(bytes, "native-fixture", 14) == 0;
        if (verify != INVALID_HANDLE_VALUE) CloseHandle(verify);
        Check(unchanged, "exclusive post-test verification finds unchanged fixture and no leaked handles");
        CleanupCheck(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "final owned-file attribute cleanup");
        CleanupCheck(DeleteFileW(file.c_str()) != FALSE, "sharing fixture removed; recovery and GUID journals retained");
    }
    std::printf("[SUMMARY] open-sharing functional_failures=%d cleanup_failures=%d fixture_directory=%ls\n",
        failures, cleanup_failures.load(), root.c_str());
    return failures || cleanup_failures.load() ? 1 : 0;
}



int RunSaveOverwriteAccept(HWND owner, const std::filesystem::path& root) {
    constexpr FILEOPENDIALOGOPTIONS options = 0x0200884A;
    static_assert(options == (FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT |
        FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN));
    const auto target = root / L"ordinary.txt";
    HANDLE created = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (created == INVALID_HANDLE_VALUE) { Check(false, "overwrite-accept exclusively creates its own fixture"); return 1; }
    DWORD written = 0;
    const bool initialized = WriteFile(created, "native-fixture", 14, &written, nullptr) && written == 14;
    CloseHandle(created);
    Check(initialized, "overwrite-accept fixture initialized");
    struct FileObservation { FILE_ID_INFO identity{}; char contents[15]{}; DWORD bytes = 0; };
    const auto observe = [&](FileObservation& observation) {
        // Exclusive content verification is outside Show. It also detects leaked
        // test/native handles, without imposing a sharing constraint on Show.
        HANDLE file = CreateFileW(target.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        const bool read = GetFileInformationByHandleEx(file, FileIdInfo, &observation.identity, sizeof(observation.identity)) &&
            ReadFile(file, observation.contents, sizeof(observation.contents), &observation.bytes, nullptr);
        const bool closed = CloseHandle(file) != FALSE;
        return read && closed && observation.bytes == 14;
    };
    FileObservation before, after;
    const bool before_ok = initialized && observe(before);
    Check(before_ok && std::memcmp(before.contents, "native-fixture", 14) == 0,
        "overwrite-accept records existing file identity and contents before native Show");
    if (before_ok) {
        OpenValidationDrive drive{L"ordinary.txt", nullptr, nullptr, &root, true};
        const auto result = Run(owner, root, L"ordinary.txt", true, options,
            "save-overwrite-accept", false, false, 0, &drive);
        std::printf("[SAMPLE] save-overwrite-accept requested=0x%08lX actual=0x%08lX affirmative_attempted=%d posted=%d refused=%d prompt=%d Show=0x%08lX GetResult=0x%08lX\n",
            static_cast<unsigned long>(options), static_cast<unsigned long>(result.actual_options),
            result.affirmative_attempted, result.affirmative_posted, result.affirmative_refused, result.prompt_observed,
            static_cast<unsigned long>(result.show), static_cast<unsigned long>(result.result));
        Check(result.input_confirmed && result.submitted, "overwrite-accept typed and submitted exact owned target");
        Check(result.prompt_observed && result.affirmative_attempted && result.affirmative_posted && !result.affirmative_refused,
            "overwrite-accept acts on one explicit visible same-owner STA Yes button");
        Check(result.show == S_OK && result.result == S_OK && Matches(result, target) && !result.bounded_close,
            "native Save actually accepts exact existing target after explicit overwrite consent");
        NativeWindowSearch windows{owner};
        EnumThreadWindows(GetCurrentThreadId(), FindOwnedNative, reinterpret_cast<LPARAM>(&windows));
        CleanupCheck(IsWindow(owner) && IsWindowEnabled(owner) && !windows.result,
            "overwrite-accept restores owner and leaves no visible native dialog");
        const bool after_ok = observe(after);
        Check(after_ok, "overwrite-accept exclusive post-Show file handle succeeds and closes");
        Check(after_ok && before.identity.VolumeSerialNumber == after.identity.VolumeSerialNumber &&
            std::memcmp(before.identity.FileId.Identifier, after.identity.FileId.Identifier, sizeof(before.identity.FileId.Identifier)) == 0,
            "overwrite-accept preserves exact owned file identity");
        Check(after_ok && before.bytes == after.bytes && std::memcmp(before.contents, after.contents, before.bytes) == 0,
            "overwrite-accept preserves existing contents without truncation");
    }
    CleanupCheck(DeleteFileW(target.c_str()) != FALSE, "overwrite-accept owned file removed; GUID journal retained");
    std::printf("[SUMMARY] save-overwrite-accept functional_failures=%d cleanup_failures=%d fixture_directory=%ls\n",
        failures, cleanup_failures.load(), root.c_str());
    return failures || cleanup_failures.load() ? 1 : 0;
}

int RunSaveValidation(HWND owner, const std::filesystem::path& root) {
    constexpr FILEOPENDIALOGOPTIONS options = 0x0200884A;
    static_assert(options == (FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT |
        FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN));
    std::printf("[FACT] save-validation requested_options=0x%08lX; no NOVALIDATE/NOTESTFILECREATE/NODEREFERENCELINKS\n",
        static_cast<unsigned long>(options));
    const auto existing = root / L"ordinary.txt";
    HANDLE created = CreateFileW(existing.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (created == INVALID_HANDLE_VALUE) { Check(false, "Save fixture exclusively created"); return 1; }
    DWORD written = 0;
    const bool ready = WriteFile(created, "native-fixture", 14, &written, nullptr) && written == 14;
    CloseHandle(created);
    Check(ready, "Save fixture initialized");
    if (!ready) { CleanupCheck(DeleteFileW(existing.c_str()) != FALSE, "partial Save fixture removed"); return 1; }
    auto sample = [&](const std::filesystem::path& directory, const wchar_t* leaf, const char* label,
                      IsolatedFileAcl* acl = nullptr) {
        const auto target = directory / leaf;
        const DWORD before = GetFileAttributesW(target.c_str());
        const DWORD before_error = before == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        std::printf("[FACT] %s before_attributes=0x%08lX before_error=%lu target=%ls\n", label, before, before_error, target.c_str());
        OpenValidationDrive drive{leaf, acl ? IsolatedFileAcl::Emergency : nullptr, acl, &root};
        const auto result = Run(owner, directory, L"ordinary.txt", true, options, label, false, false, 0, &drive);
        const bool accepted = Matches(result, target);
        const bool no_result = FAILED(result.show) && FAILED(result.result) && result.path.empty();
        const bool prompt = no_result && result.prompt_observed;
        const bool native_failure = no_result && !result.bounded_close && result.show != HRESULT_FROM_WIN32(ERROR_CANCELLED);
        std::printf("[SAMPLE] %s outcome=%s actual_options=0x%08lX; owned-prompt may be overwrite confirmation, not proof of access rejection\n",
            label, accepted ? "accepted-exact" : prompt ? "owned-prompt-nonacceptance" : native_failure ? "native-failure" :
            "inconclusive-harness-cancel", static_cast<unsigned long>(result.actual_options));
        Check(result.input_confirmed && result.submitted, "Save sampler typed and submitted exact filename");
        Check(accepted || no_result, "Save sampler yields exact target or no result");
        Check(accepted || prompt || native_failure, "Save sampler has terminal or prompt-backed evidence");
        if (result.show == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            Check(FAILED(result.result) && result.path.empty(), "Save cancellation has no result");
        NativeWindowSearch windows{owner};
        EnumThreadWindows(GetCurrentThreadId(), FindOwnedNative, reinterpret_cast<LPARAM>(&windows));
        CleanupCheck(IsWindow(owner) && IsWindowEnabled(owner), "Save native Show restored test owner");
        CleanupCheck(!windows.result, "Save native dialog left no visible owned dialog");
        const DWORD after = GetFileAttributesW(target.c_str());
        const DWORD after_error = after == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        std::printf("[FACT] %s after_attributes=0x%08lX after_error=%lu target=%ls\n", label, after, after_error, target.c_str());
        if (before == INVALID_FILE_ATTRIBUTES && before_error == ERROR_FILE_NOT_FOUND) {
            std::printf("[FACT] %s initially-absent-target present-after-Show=%d absence-confirmed=%d\n", label,
                after != INVALID_FILE_ATTRIBUTES, after == INVALID_FILE_ATTRIBUTES && after_error == ERROR_FILE_NOT_FOUND);
            if (after != INVALID_FILE_ATTRIBUTES) CleanupCheck(DeleteFileW(target.c_str()) != FALSE, "native-created owned Save target removed");
        }
    };
    sample(root, L"new-save.txt", "save-validation-new");
    sample(root, L"ordinary.txt", "save-validation-existing");
    const bool readonly = SetFileAttributesW(existing.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE;
    Check(readonly, "Save readonly fixture installed");
    if (readonly) sample(root, L"ordinary.txt", "save-validation-readonly");
    const bool attr_restored = SetFileAttributesW(existing.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE;
    CleanupCheck(attr_restored && GetFileAttributesW(existing.c_str()) != INVALID_FILE_ATTRIBUTES &&
        !(GetFileAttributesW(existing.c_str()) & FILE_ATTRIBUTE_READONLY), "Save readonly attribute restored");
    if (attr_restored) {
        HANDLE exclusive = CreateFileW(existing.c_str(), FILE_READ_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD probe = ProbeExistingAccess(existing, FILE_READ_DATA);
        Check(exclusive != INVALID_HANDLE_VALUE && probe == ERROR_SHARING_VIOLATION, "Save exclusive holder independently verified");
        if (exclusive != INVALID_HANDLE_VALUE) {
            if (probe == ERROR_SHARING_VIOLATION) sample(root, L"ordinary.txt", "save-validation-exclusive");
            CleanupCheck(CloseHandle(exclusive) != FALSE, "Save exclusive holder closed");
        }
    }
    const auto denied_directory = root / L"no-create";
    const bool directory_created = CreateDirectoryW(denied_directory.c_str(), nullptr) != FALSE;
    Check(directory_created, "Save ACL directory exclusively created");
    bool restored = true;
    if (directory_created) {
        IsolatedFileAcl acl(denied_directory);
        const bool installed = acl.Install(false, true);
        Check(installed, "Save directory ACL installed with recovery journal");
        if (installed) {
            const auto probe_path = denied_directory / L"creation-probe.tmp";
            HANDLE probe = CreateFileW(probe_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            const DWORD error = probe == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
            std::printf("[FACT] Save directory CREATE_NEW probe error=%lu path=%ls\n", error, probe_path.c_str());
            Check(error == ERROR_ACCESS_DENIED, "Save ACL independently denies creation");
            if (probe != INVALID_HANDLE_VALUE) {
                CloseHandle(probe);
                CleanupCheck(DeleteFileW(probe_path.c_str()) != FALSE, "unexpected owned creation probe removed");
            }
            if (error == ERROR_ACCESS_DENIED) sample(denied_directory, L"denied-save.txt", "save-validation-create-denied", &acl);
        }
        restored = acl.Restore();
        CleanupCheck(restored, "Save directory original DACL and protection exactly restored");
    }
    if (directory_created && restored) {
        CleanupCheck(RemoveDirectoryW(denied_directory.c_str()) != FALSE,
            "Save owned ACL directory removed after exact restoration; journals retained in parent");
    }
    HANDLE verify = CreateFileW(existing.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    char contents[15]{}; DWORD read = 0;
    Check(verify != INVALID_HANDLE_VALUE && ReadFile(verify, contents, 15, &read, nullptr) && read == 14 &&
        std::memcmp(contents, "native-fixture", 14) == 0, "Save existing fixture unchanged; no remaining test handles");
    if (verify != INVALID_HANDLE_VALUE) CloseHandle(verify);
    CleanupCheck(SetFileAttributesW(existing.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE && DeleteFileW(existing.c_str()) != FALSE,
        "Save existing fixture removed; recovery and GUID journals retained");
    std::printf("[SUMMARY] save-validation functional_failures=%d cleanup_failures=%d fixture_directory=%ls\n",
        failures, cleanup_failures.load(), root.c_str());
    return failures || cleanup_failures.load() ? 1 : 0;
}

int IsolationProbe() {
    // This proves only predefined-HKCU routing in this process, not all Shell
    // persistence. Never call Show here or use this probe as permission to do so.
    // https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regoverridepredefkey
    GUID nonce{};
    wchar_t nonce_text[40]{};
    if (FAILED(CoCreateGuid(&nonce)) || !StringFromGUID2(nonce, nonce_text, 40)) return 2;
    const auto name = L"PulseNativeContract-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    const std::wstring leaf = name + L"-" + nonce_text;
    const auto root = std::filesystem::absolute(std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        L"bench_data" / name);
    if (!CreateDirectoryW(root.c_str(), nullptr)) return 2;
    std::string leaf_ascii;
    for (const wchar_t ch : leaf) leaf_ascii.push_back(static_cast<char>(ch));
    std::string marker_ascii;
    for (const wchar_t ch : std::wstring_view(nonce_text)) marker_ascii.push_back(static_cast<char>(ch));
    const auto journal_path = root / L"registry-isolation.tsv";
    HANDLE journal = CreateFileW(journal_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (journal == INVALID_HANDLE_VALUE) return 2;
    const std::string record = "HKCU\\Software\\" + leaf_ascii + "\tPulseNativeContractMarker\t" + marker_ascii + "\r\n";
    DWORD written = 0;
    const bool persisted = WriteFile(journal, record.data(), static_cast<DWORD>(record.size()), &written, nullptr) &&
        written == record.size() && FlushFileBuffers(journal);
    CloseHandle(journal);
    if (!persisted) return 2;
    std::printf("[FACT] registry probe recovery journal=%ls\n", journal_path.c_str());
    std::fflush(stdout);
    struct Key {
        HKEY value = nullptr;
        ~Key() { if (value) RegCloseKey(value); }
    } parent, scratch, probe;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software", 0, KEY_READ | KEY_WRITE, &parent.value);
    if (status != ERROR_SUCCESS) return 2;
    DWORD disposition = 0;
    status = RegCreateKeyExW(parent.value, leaf.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_ALL_ACCESS, nullptr, &scratch.value, &disposition);
    if (status != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) return 2;
    const DWORD marker_bytes = static_cast<DWORD>((wcslen(nonce_text) + 1) * sizeof(wchar_t));
    status = RegSetValueExW(scratch.value, L"PulseNativeContractMarker", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(nonce_text), marker_bytes);
    const auto owned = [&] {
        wchar_t actual[40]{};
        DWORD bytes = sizeof(actual), type = 0;
        return RegQueryValueExW(scratch.value, L"PulseNativeContractMarker", nullptr, &type,
            reinterpret_cast<BYTE*>(actual), &bytes) == ERROR_SUCCESS && type == REG_SZ &&
            bytes == marker_bytes && std::memcmp(actual, nonce_text, marker_bytes) == 0;
    };
    if (status != ERROR_SUCCESS || !owned()) return 2;
    const LSTATUS attached = RegOverridePredefKey(HKEY_CURRENT_USER, scratch.value);
    Check(attached == ERROR_SUCCESS, "probe HKCU attached to exclusively created marked scratch");
    if (attached == ERROR_SUCCESS) {
        status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Probe", 0, nullptr, 0,
            KEY_READ | KEY_WRITE, nullptr, &probe.value, nullptr);
        if (status == ERROR_SUCCESS) status = RegSetValueExW(probe.value, L"RoutedMarker", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(nonce_text), marker_bytes);
        Check(status == ERROR_SUCCESS, "probe writes marker through predefined HKCU");
        wchar_t actual[40]{};
        DWORD bytes = sizeof(actual);
        status = RegGetValueW(scratch.value, L"Probe", L"RoutedMarker", RRF_RT_REG_SZ, nullptr, actual, &bytes);
        Check(status == ERROR_SUCCESS && bytes == marker_bytes && std::memcmp(actual, nonce_text, marker_bytes) == 0,
            "direct retained scratch handle observes redirected marker");
        if (probe.value) { RegCloseKey(probe.value); probe.value = nullptr; }
        const LSTATUS detached = RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        Check(detached == ERROR_SUCCESS, "probe restores predefined HKCU mapping");
        if (detached != ERROR_SUCCESS) return 2;
    }
    if (!owned()) { Check(false, "scratch ownership verified before cleanup"); return 2; }
    // Delete contents only via the retained, freshly-created, marker-verified
    // scratch handle; delete its exact leaf via the original retained parent.
    status = RegDeleteTreeW(scratch.value, nullptr);
    Check(status == ERROR_SUCCESS, "probe removes only owned scratch contents");
    if (status == ERROR_SUCCESS) {
        RegCloseKey(scratch.value); scratch.value = nullptr;
        status = RegDeleteKeyW(parent.value, leaf.c_str());
        Check(status == ERROR_SUCCESS, "probe removes only its exact scratch leaf");
        HKEY residual = nullptr;
        const LSTATUS absent = RegOpenKeyExW(parent.value, leaf.c_str(), 0, KEY_READ, &residual);
        if (residual) RegCloseKey(residual);
        Check(absent == ERROR_FILE_NOT_FOUND, "probe scratch is absent after cleanup");
    }
    std::puts("[LIMIT] Probe verifies only this process's predefined HKCU access. Native Shell persistence isolation remains unproven; no Show was called.");
    return failures ? 1 : 0;
}

int CleanupJournal(const wchar_t* argument) {
    // Accept only a direct, non-reparse test journal under this checkout's bench_data.
    const auto bench = std::filesystem::absolute(std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / L"bench_data").lexically_normal();
    const auto journal = std::filesystem::absolute(argument).lexically_normal();
    const auto root = journal.parent_path();
    if (journal.filename() != L"native-client-guids.tsv" || root.parent_path() != bench ||
        !std::regex_match(root.filename().wstring(), std::wregex(L"PulseNativeContract-[0-9]+-[0-9]+"))) {
        std::puts("[FAIL] cleanup requires this checkout's bench_data/PulseNativeContract-<pid>-<tick>/native-client-guids.tsv");
        return 2;
    }
    for (auto path = journal; !path.empty(); path = path.parent_path()) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            std::puts("[FAIL] cleanup journal path is missing or traverses a reparse point");
            return 2;
        }
        if (path == path.parent_path()) break;
    }
    struct Entry { std::string label; bool save; GUID guid; std::string text; };
    std::vector<Entry> entries;
    std::ifstream input(journal);
    if (!input) return 2;
    std::string line;
    const std::regex allocation("([^\\t]+)\\t(Open|Save)\\t(\\{[0-9A-Fa-f-]{36}\\})\\tallocated-before-SetClientGuid");
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find("allocated-before-SetClientGuid") == std::string::npos) continue;
        std::smatch match;
        if (!std::regex_match(line, match, allocation)) return 2;
        const std::string text = match[3].str();
        const std::wstring wide(text.begin(), text.end());
        GUID guid{};
        if (FAILED(CLSIDFromString(wide.c_str(), &guid))) return 2;
        entries.push_back({match[1].str(), match[2].str() == "Save", guid, text});
    }
    if (!input.eof() || entries.empty()) return 2;
    input.close();
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::printf("[FACT] cleanup CoInitializeEx(STA) HRESULT=0x%08lX\n", static_cast<unsigned long>(initialized));
    if (FAILED(initialized)) return 2;
    for (const auto& entry : entries) {
        ComPtr<IFileDialog> dialog;
        const HRESULT created = NativeDialog(entry.save, &dialog);
        const HRESULT configured = SUCCEEDED(created) ? dialog->SetClientGuid(entry.guid) : E_PENDING;
        const HRESULT cleared = SUCCEEDED(configured) ? dialog->ClearClientData() : E_PENDING;
        const HRESULT hr = FAILED(created) ? created : FAILED(configured) ? configured : cleared;
        const auto stage = [&](const char* name, HRESULT result, bool called) {
            char stage_code[32]{};
            if (called) std::snprintf(stage_code, sizeof(stage_code), "0x%08lX", static_cast<unsigned long>(result));
            else std::snprintf(stage_code, sizeof(stage_code), "not-called");
            std::printf("[FACT] exact cleanup GUID=%s %s=%s\n", entry.text.c_str(), name, stage_code);
            Check(AppendJournal(root, entry.label + "\t" + entry.text + "\t" + name + "\t" + stage_code + "\r\n"),
                "cleanup stage journaled");
        };
        stage("CoCreateInstance", created, true);
        stage("SetClientGuid", configured, SUCCEEDED(created));
        stage("ClearClientData", cleared, SUCCEEDED(configured));
        char code[32]{};
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
        std::printf("[FACT] exact journal cleanup %s GUID=%s HRESULT=%s\n", entry.label.c_str(), entry.text.c_str(), code);
        Check(AppendJournal(root, entry.label + "\t" + entry.text + "\tExplicitCleanup\t" + code + "\r\n"), "cleanup result journaled");
        Check(SUCCEEDED(hr), "exact test GUID cleanup succeeded");
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--isolation-probe") return IsolationProbe();
    if (argc == 3 && std::wstring_view(argv[1]) == L"--cleanup-journal") return CleanupJournal(argv[2]);
    const bool folder_names = argc == 2 && std::wstring_view(argv[1]) == L"--folder-names";
    const bool wrapper_events = argc == 2 && std::wstring_view(argv[1]) == L"--wrapper-events";
    const bool open_validation = argc == 2 && std::wstring_view(argv[1]) == L"--open-validation";
    const bool open_sharing = argc == 2 && std::wstring_view(argv[1]) == L"--open-sharing";
    const bool native_options = argc == 2 && std::wstring_view(argv[1]) == L"--native-options";
    const bool open_novalidate = argc == 2 && std::wstring_view(argv[1]) == L"--open-novalidate";
    const bool save_validation = argc == 2 && std::wstring_view(argv[1]) == L"--save-validation";
    const bool save_overwrite_accept = argc == 2 && std::wstring_view(argv[1]) == L"--save-overwrite-accept";
    if (argc != 1 && !folder_names && !wrapper_events && !open_validation && !open_sharing && !native_options && !open_novalidate && !save_validation && !save_overwrite_accept) return 2;
    const auto desktop_name = L"PulseNativeContract-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) { if (desktop) CloseDesktop(desktop); return 2; }
    // Never SwitchDesktop: every HWND and driver belongs to this isolated desktop.
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    WNDCLASSW cls{};
    cls.lpfnWndProc = OwnerProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"PulseNativeContractOwner";
    RegisterClassW(&cls);
    HWND owner = CreateWindowW(cls.lpszClassName, L"native contract", WS_OVERLAPPED, 0, 0, 300, 200,
        nullptr, nullptr, cls.hInstance, nullptr);
    if (!owner) { CoUninitialize(); return 2; }
    const auto bench = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / L"bench_data";
    const auto root = std::filesystem::absolute(bench / desktop_name);
    if (!CreateDirectoryW(root.c_str(), nullptr)) { DestroyWindow(owner); CoUninitialize(); return 2; }
    std::printf("[NOTE] Exact fixture and GUID journal directory: %ls\n", root.c_str());
    std::fflush(stdout);
    if (native_options) {
        native_options_probe = true;
        const auto outside = root / L"ordinary.bin";
        Check(MakeFile(outside), "strict-filter native fixture created");
        OpenValidationDrive drive{L"ordinary.bin"};
        const auto flags = FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT |
            FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_STRICTFILETYPES;
        const auto control = Run(owner, root, L"ordinary.txt", false, flags & ~FOS_STRICTFILETYPES, "advisory-filter-open", false, false, 0, &drive);
        Check(Matches(control, outside), "native Open advisory control accepts typed outside-filter file");
        const auto open = Run(owner, root, L"ordinary.txt", false, flags, "strict-filter-open", false, false, 0, &drive);
        Check(open.input_confirmed && open.submitted, "strict native Open typed outside-filter filename");
        Check(Matches(open, outside), "native Open STRICTFILETYPES keeps typed filters advisory");
        CleanupCheck(DeleteFileW(outside.c_str()) != FALSE, "strict-filter fixture removed");
        DestroyWindow(owner); CoUninitialize();
        std::printf("native-options failures=%d cleanup_failures=%d\n", failures, cleanup_failures.load());
        return failures || cleanup_failures.load() ? 1 : 0;
    }
    if (folder_names) {
        const auto selected = root / L"named-folder";
        Check(CreateDirectoryW(selected.c_str(), nullptr) != FALSE, "named folder fixture");
        const auto result = Run(owner, root, L"named-folder", false, static_cast<FILEOPENDIALOGOPTIONS>(0x1060 | FOS_DONTADDTORECENT), "folder-initial-name");
        Check(result.show == S_OK && result.path == selected.wstring(), "native nonempty initial folder name selects named child");
        CleanupCheck(RemoveDirectoryW(selected.c_str()) != FALSE, "owned named folder removed");
        DestroyWindow(owner); CoUninitialize(); return failures ? 1 : 0;
    }
    if (open_validation || open_sharing || open_novalidate || save_validation || save_overwrite_accept) {
        const int result = save_overwrite_accept ? RunSaveOverwriteAccept(owner, root) :
            save_validation ? RunSaveValidation(owner, root) :
            open_sharing ? RunOpenSharing(owner, root) : RunOpenValidation(owner, root, open_novalidate);
        DestroyWindow(owner);
        CoUninitialize();
        return result;
    }
    const auto file = root / L"ordinary.txt";
    const auto readonly = root / L"readonly.txt";
    const auto shortcut = root / L"ordinary.lnk";
    Check(MakeFile(file) && MakeFile(readonly), "isolated native fixtures created");
    if (wrapper_events) {
        const FILEOPENDIALOGOPTIONS options = FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT |
            FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOVALIDATE | FOS_NODEREFERENCELINKS;
        Run(owner, root, L"ordinary.txt", false, options, "wrapper-OnFileOk-S_FALSE", false, false, 1);
        Run(owner, root, L"ordinary.txt", false, options, "wrapper-OnFileOk-Close", false, false, 2);
        Check(DeleteFileW(file.c_str()) && DeleteFileW(readonly.c_str()), "wrapper event fixture files removed; journal retained");
        DestroyWindow(owner);
        CoUninitialize();
        return failures ? 1 : 0;
    }
    {
        ComPtr<IShellLinkW> link;
        ComPtr<IPersistFile> persistence;
        Check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
            SUCCEEDED(link->SetPath(file.c_str())) && SUCCEEDED(link.As(&persistence)) &&
            SUCCEEDED(persistence->Save(shortcut.c_str(), TRUE)), "isolated shortcut fixture created");
    }
    const FILEOPENDIALOGOPTIONS common = FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT;
    const FILEOPENDIALOGOPTIONS open = common | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NODEREFERENCELINKS;
    const FILEOPENDIALOGOPTIONS candidate_open = open | FOS_NOVALIDATE;
    const FILEOPENDIALOGOPTIONS save = common | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN |
        FOS_NOTESTFILECREATE | FOS_NOVALIDATE | FOS_NODEREFERENCELINKS;
    auto observation = Run(owner, root, L"ordinary.txt", false, candidate_open, "ordinary-open-candidate");
    Check(Matches(observation, file), "ordinary native Open returns requested fixture");
    HANDLE exclusive = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(exclusive != INVALID_HANDLE_VALUE, "exclusive lock fixture acquired");
    if (exclusive != INVALID_HANDLE_VALUE) {
        observation = Run(owner, root, L"ordinary.txt", false, candidate_open, "locked-open-NOVALIDATE");
        Check(FAILED(observation.show) || Matches(observation, file), "locked Open cannot return an unrelated target");
        CloseHandle(exclusive);
    }
    observation = Run(owner, root, L"ordinary.lnk", false, candidate_open, "shortcut-NODEREFERENCELINKS");
    Check(Matches(observation, shortcut), "native no-dereference returns shortcut itself");
    const auto new_file = root / L"new.txt";
    observation = Run(owner, root, L"new.txt", true, save, "new-save-candidate");
    Check(Matches(observation, new_file), "native Save returns new target");
    Check(GetFileAttributesW(new_file.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
        "native Save with NOTESTFILECREATE did not create target");
    const bool readonly_set = SetFileAttributesW(readonly.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE;
    Check(readonly_set, "readonly fixture attribute installed");
    if (readonly_set) {
        observation = Run(owner, root, L"readonly.txt", true, save, "readonly-save-candidate", false, true);
        Check(FAILED(observation.show) || Matches(observation, readonly), "readonly native observation cannot return unrelated target");
        Check(FAILED(observation.show) && FAILED(observation.result) && observation.path.empty(),
            "readonly Save must not accept a result");
        std::printf("[FACT] readonly candidate accepted=%d; this observation is not approval of wrapper parity.\n", SUCCEEDED(observation.show));
    }
    SetFileAttributesW(readonly.c_str(), FILE_ATTRIBUTE_NORMAL);
    observation = Run(owner, root, L"ordinary.txt", false, candidate_open, "explicit-cancel", true);
    Check(observation.show == HRESULT_FROM_WIN32(ERROR_CANCELLED) && FAILED(observation.result) && observation.path.empty(),
        "native cancellation has no accepted result");
    // The non-candidate validation control runs last so a native deadlock cannot hide candidate observations.
    exclusive = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(exclusive != INVALID_HANDLE_VALUE, "validation control exclusive lock acquired");
    if (exclusive != INVALID_HANDLE_VALUE) {
        observation = Run(owner, root, L"ordinary.txt", false, open, "locked-open-with-validation");
        Check(FAILED(observation.show) || Matches(observation, file), "validation control cannot return unrelated target");
        Check(FAILED(observation.show) && FAILED(observation.result) && observation.path.empty(),
            "locked Open with validation must not accept a result");
        std::puts("[FACT] Validation control checks non-acceptance of this exclusively locked fixture; bounded test cancellation does not establish complete native validation parity.");
        CloseHandle(exclusive);
    }
    WIN32_FILE_ATTRIBUTE_DATA info{};
    Check(GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow == 14 &&
        GetFileAttributesExW(readonly.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow == 14,
        "native selection did not truncate existing fixtures");
    Check(DeleteFileW(shortcut.c_str()) && DeleteFileW(file.c_str()) && DeleteFileW(readonly.c_str()),
        "native fixture files removed; GUID cleanup journal retained");
    std::printf("[NOTE] Retained exact test GUID cleanup journal: %ls\\native-client-guids.tsv\n", root.c_str());
    DestroyWindow(owner);
    CoUninitialize();
    std::puts("[NOTE] Random ClientGuid cleanup was attempted with each HRESULT recorded; DONTADDTORECENT was set. No existing user GUID/configuration was queried or edited; undocumented OS bookkeeping is not asserted absent.");
    return failures ? 1 : 0;
}

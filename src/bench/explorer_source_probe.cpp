#include <windows.h>
#include <shlobj.h>
#include <exdisp.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include "../app/explorer_source_scope.h"
#include "../app/explorer_source_observation.h"

// Standalone source feasibility probe. No Pulse observer, injection, association
// changes, Explorer termination, or automatic fallback to another desktop.
namespace {
using Microsoft::WRL::ComPtr;
std::string HResultText(HRESULT value) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(value));
    return text;
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct SourceOwner {
    HWND window = nullptr;
    uint64_t value = 0;
    ~SourceOwner() {
        if (value && reinterpret_cast<uint64_t>(GetPropW(window, pulse::app::kExplorerScopeOwner)) == value)
            RemovePropW(window, pulse::app::kExplorerScopeOwner);
    }
};
std::wstring Canonical(const std::wstring& path) {
    Handle file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return {};
    std::wstring result(32768, 0);
    const DWORD count = GetFinalPathNameByHandleW(file.value, result.data(), static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED);
    if (!count || count >= result.size()) return {};
    result.resize(count); return result;
}
bool Same(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) return "<invalid-utf16>";
    std::string result(static_cast<size_t>(count), 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}
std::wstring DesktopName(DWORD tid) {
    wchar_t name[256]{}; DWORD count = 0;
    const auto desktop = GetThreadDesktop(tid);
    return desktop && GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &count) ? name : L"<unavailable>";
}
std::set<HWND> Windows(HDESK desktop, bool* complete = nullptr, std::string* diagnostics = nullptr) {
    std::set<HWND> result;
    const bool ok = EnumDesktopWindows(desktop, [](HWND window, LPARAM data) -> BOOL {
            reinterpret_cast<std::set<HWND>*>(data)->insert(window); return TRUE;
        }, reinterpret_cast<LPARAM>(&result)) != FALSE;
    if (complete) *complete = ok;
    if (!ok && diagnostics) *diagnostics += "BASELINE_ERROR\tstage=EnumDesktopWindows\terror=" + std::to_string(GetLastError()) + "\n";
    if (!ok) result.clear();
    return result;
}
struct Source {
    HWND window = nullptr;
    DWORD pid = 0, tid = 0;
    uint64_t created = 0;
    ComPtr<IWebBrowser2> browser;
};
bool ProcessIdentity(Source& source, const std::wstring& system_explorer) {
    source.tid = GetWindowThreadProcessId(source.window, &source.pid);
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, source.pid)};
    if (!source.tid || !process.value) return false;
    wchar_t image[32768]{}; DWORD size = ARRAYSIZE(image);
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!QueryFullProcessImageNameW(process.value, 0, image, &size) ||
        !GetProcessTimes(process.value, &created, &exited, &kernel, &user) ||
        !Same(Canonical(image), system_explorer)) return false;
    source.created = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    return true;
}
std::vector<Source> Sources(IShellWindows* windows, bool* complete = nullptr, std::string* diagnostics = nullptr) {
    std::vector<Source> result;
    if (complete) *complete = false;
    long count = 0;
    const HRESULT count_hr = windows->get_Count(&count);
    if (count_hr != S_OK || count < 0 || count > 4096) {
        if (diagnostics) *diagnostics += "BASELINE_ERROR\tstage=get_Count\thresult=" + HResultText(count_hr) +
            "\tcount=" + std::to_string(count) + "\n";
        return result;
    }
    bool ok = true;
    for (long i = 0; i < count; ++i) {
        VARIANT index{}; index.vt = VT_I4; index.lVal = i;
        ComPtr<IDispatch> dispatch;
        Source source; SHANDLE_PTR handle = 0;
        const auto failed = [&](const char* stage, HRESULT hr) {
            ok = false;
            if (diagnostics) *diagnostics += std::string("BASELINE_ERROR\tstage=") + stage +
                "\thresult=" + HResultText(hr) + "\tindex=" + std::to_string(i) + "\tcount=" + std::to_string(count) + "\n";
        };
        HRESULT item_hr = windows->Item(index, &dispatch);
        // Item documents S_FALSE/null as "window was not found", not a failed
        // COM call. This identifies no source and does not prove a pending item.
        // https://learn.microsoft.com/windows/win32/api/exdisp/nf-exdisp-ishellwindows-item
        if (item_hr == S_FALSE && !dispatch) {
            if (diagnostics) *diagnostics += "ITEM_ABSENT\thresult=" + HResultText(item_hr) +
                "\tindex=" + std::to_string(i) + "\tcount=" + std::to_string(count) + "\n";
            continue;
        }
        if (item_hr != S_OK || !dispatch) { failed("Item_or_null_dispatch", item_hr); continue; }
        item_hr = dispatch.As(&source.browser);
        if (FAILED(item_hr) || !source.browser) { failed("QueryInterface_IWebBrowser2", item_hr); continue; }
        item_hr = source.browser->get_HWND(&handle);
        if (FAILED(item_hr) || !handle) { failed("get_HWND_or_null", item_hr); continue; }
        source.window = reinterpret_cast<HWND>(handle);
        if (source.window) result.push_back(std::move(source));
    }
    if (complete) *complete = ok;
    return result;
}
HRESULT PrepareFixtureSelection(const Source& source) {
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> folder_view;
    ComPtr<IShellFolder> folder;
    HRESULT hr = source.browser.As(&provider);
    if (SUCCEEDED(hr)) hr = provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
    if (SUCCEEDED(hr)) hr = browser->QueryActiveShellView(&view);
    if (SUCCEEDED(hr) && !view) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = view.As(&folder_view);
    if (SUCCEEDED(hr)) hr = folder_view->GetFolder(IID_PPV_ARGS(&folder));
    PIDLIST_RELATIVE child = nullptr;
    wchar_t name[] = L"selected.txt";
    if (SUCCEEDED(hr)) hr = folder->ParseDisplayName(nullptr, nullptr, name, nullptr, &child, nullptr);
    if (SUCCEEDED(hr) && child)
        hr = view->SelectItem(child, SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED);
    else if (SUCCEEDED(hr)) hr = E_FAIL;
    CoTaskMemFree(child);
    return hr;
}
bool Read(const Source& source, std::wstring& path, std::vector<std::wstring>& names) {
    ComPtr<IServiceProvider> provider;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> folder_view;
    ComPtr<IPersistFolder2> folder;
    if (FAILED(source.browser.As(&provider)) ||
        FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view)) || !view || FAILED(view.As(&folder_view)) ||
        FAILED(folder_view->GetFolder(IID_PPV_ARGS(&folder)))) return false;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(folder->GetCurFolder(&pidl)) || !pidl) return false;
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &raw);
    CoTaskMemFree(pidl);
    if (FAILED(hr) || !raw) return false;
    path = Canonical(raw); CoTaskMemFree(raw);
    if (path.empty()) return false;
    int selected = 0;
    if (FAILED(folder_view->ItemCount(SVGIO_SELECTION, &selected))) return false;
    if (!selected) return true;
    ComPtr<IShellItemArray> items; DWORD count = 0;
    if (folder_view->GetSelection(FALSE, &items) != S_OK || !items || FAILED(items->GetCount(&count))) return false;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item; PWSTR name = nullptr;
        if (FAILED(items->GetItemAt(i, &item)) || FAILED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &name)) || !name)
            return false;
        names.emplace_back(name); CoTaskMemFree(name);
    }
    return true;
}
int TabCount(HWND window) {
    int result = 0;
    EnumChildWindows(window, [](HWND child, LPARAM value) -> BOOL {
        wchar_t name[80]{};
        if (GetClassNameW(child, name, ARRAYSIZE(name)) && wcscmp(name, L"ShellTabWindowClass") == 0)
            ++*reinterpret_cast<int*>(value);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}
bool ExactSource(const Source& source, IShellWindows* windows, HDESK desktop,
                 const std::set<HWND>& baseline, const std::wstring& image, const std::wstring& fixture,
                 std::string* diagnostics = nullptr) {
    const auto reject = [&](const char* reason) {
        if (diagnostics) *diagnostics += std::string("\treject=") + reason;
        return false;
    };
    const bool old_window = baseline.contains(source.window);
    bool desktop_complete = false;
    const bool desktop_member = Windows(desktop, &desktop_complete).contains(source.window);
    const bool root = GetAncestor(source.window, GA_ROOT) == source.window;
    if (diagnostics) *diagnostics += "\tbaseline=" + std::to_string(old_window) + "\tdesktop_member=" + std::to_string(desktop_member) +
        "\tdesktop_complete=" + std::to_string(desktop_complete) + "\troot=" + std::to_string(root);
    if (old_window) return reject("baseline-HWND");
    if (!desktop_member) return reject(desktop_complete ? "wrong-desktop" : "desktop-enumeration-failed");
    if (!root) return reject("not-root-HWND");
    Source current; current.window = source.window;
    const bool process_identity = ProcessIdentity(current, image) && current.pid == source.pid && current.tid == source.tid &&
        current.created == source.created;
    if (diagnostics) *diagnostics += "\tprocess_identity=" + std::to_string(process_identity);
    if (!process_identity) return reject("process-identity-changed-or-unreadable");
    SHANDLE_PTR handle = 0;
    const HRESULT hwnd_hr = source.browser->get_HWND(&handle);
    const bool com_hwnd = SUCCEEDED(hwnd_hr) && reinterpret_cast<HWND>(handle) == source.window;
    if (diagnostics) *diagnostics += "\tcom_hwnd=" + std::to_string(com_hwnd) + "\tcom_hwnd_hr=" + HResultText(hwnd_hr);
    if (!com_hwnd) return reject("COM-HWND");
    bool complete = false;
    const auto sources = Sources(windows, &complete);
    if (diagnostics) *diagnostics += "\tcom_enumeration_complete=" + std::to_string(complete);
    if (!complete) return reject("COM-enumeration-incomplete");
    size_t views = 0; bool same_com = false;
    ComPtr<IUnknown> expected; source.browser.As(&expected);
    for (const auto& item : sources) if (GetAncestor(item.window, GA_ROOT) == source.window) {
        ++views; ComPtr<IUnknown> identity; item.browser.As(&identity);
        same_com = same_com || (expected && identity.Get() == expected.Get());
    }
    // Fail closed on platforms where this private class cannot prove one tab.
    const int tabs = TabCount(source.window);
    if (diagnostics) *diagnostics += "\tcom_views=" + std::to_string(views) + "\tsame_com=" + std::to_string(same_com) +
        "\ttabs=" + std::to_string(tabs);
    if (views != 1) return reject("COM-view-count");
    if (!same_com) return reject("COM-identity");
    if (tabs != 1) return reject("tab-count");
    std::wstring path; std::vector<std::wstring> names;
    const bool read = Read(source, path, names);
    if (diagnostics) *diagnostics += "\tfinal_read=" + std::to_string(read) + "\tfinal_fixture_match=" + std::to_string(Same(path, fixture));
    if (!read) return reject("view-or-selection-unreadable");
    if (!Same(path, fixture)) return reject("folder-changed");
    return true;
}
void Pump(DWORD milliseconds) {
    MsgWaitForMultipleObjectsEx(0, nullptr, milliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
std::vector<wchar_t> ChildEnvironment(const std::wstring& fixture) {
    std::vector<std::wstring> values;
    if (auto inherited = GetEnvironmentStringsW()) {
        for (const wchar_t* entry = inherited; *entry; entry += wcslen(entry) + 1)
            if (_wcsnicmp(entry, L"PULSE_TEST_", 11) != 0) values.emplace_back(entry);
        FreeEnvironmentStringsW(inherited);
    }
    values.push_back(L"PULSE_TEST_SCOPED_TAKEOVER=1");
    values.push_back(L"PULSE_TEST_DATA_DIR=" + fixture);
    std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (const auto& value : values) { block.insert(block.end(), value.begin(), value.end()); block.push_back(0); }
    block.push_back(0); return block;
}
bool RunHandoff(const Source& source, const std::filesystem::path& control_root, const std::wstring& pulse,
        bool busy, bool crash, bool resume, bool external, uint64_t owner, std::string& report, bool& stopped) {
    if (crash) report += "CASE\tcontrolled-owned-Pulse-crash\n";
    const auto before = pulse::app::ObserveExplorerSource(source.window);
    if (!pulse::app::UnchangedAvailableSource(before, before)) return false;
    pulse::app::ExplorerScopeToken token;
    token.hwnd = reinterpret_cast<uint64_t>(source.window); token.pid = source.pid; token.tid = source.tid;
    token.created = source.created; token.busy = crash ? 2u : resume ? 3u : busy ? 1u : 0u;
    token.owner = owner;
    token.reserved = external ? pulse::app::kExplorerScopeExternalEmptySelection : 0;
    Handle close_authorized{CreateEventW(nullptr, TRUE, FALSE, pulse::app::ExplorerScopeCloseEventName(token).c_str())};
    const DWORD close_event_error = GetLastError();
    if (!close_authorized.value || close_event_error == ERROR_ALREADY_EXISTS) {
        report += "FAIL\tclose authorization event creation/collision\terror=" + std::to_string(close_event_error) + "\n";
        return false;
    }
    Handle config{CreateFileW((control_root / L"scope-token.bin").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    DWORD written = 0;
    if (config.value == INVALID_HANDLE_VALUE || !WriteFile(config.value, &token, sizeof(token), &written, nullptr) || written != sizeof(token)) return false;
    CloseHandle(config.value); config.value = nullptr;
    const auto start = control_root / L"start";
    if (!CreateDirectoryW(start.c_str(), nullptr)) return false;
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value) {
        report += "PROCESS\tcreate-job=0\terror=" + std::to_string(GetLastError()) + "\n"; return false;
    }
    const BOOL configured = SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    const DWORD configure_error = configured ? ERROR_SUCCESS : GetLastError();
    report += "PROCESS\tconfigure-job=" + std::to_string(configured != FALSE) + "\terror=" + std::to_string(configure_error) + "\n";
    if (!configured) return false;
    auto environment = ChildEnvironment(control_root.wstring());
    std::wstring command = L"\"" + pulse + L"\" --test-instance \"" + start.wstring() + L"\"";
    wchar_t desktop[] = L"Default";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.lpDesktop = desktop;
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION child{};
    const BOOL created = CreateProcessW(pulse.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        environment.data(), control_root.c_str(), &startup, &child);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    report += "PROCESS\tcreated=" + std::to_string(created != FALSE) + "\terror=" + std::to_string(create_error) +
        "\tchild_pid=" + std::to_string(child.dwProcessId) + "\tchild_tid=" + std::to_string(child.dwThreadId) + "\n";
    if (!created) return false;
    Handle process{child.hProcess}, thread{child.hThread};
    const auto wait_owned = [&](const char* phase, DWORD milliseconds) {
        const auto before_wait = GetTickCount64();
        const DWORD result = WaitForSingleObject(process.value, milliseconds);
        const DWORD error = result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        report += std::string("PROCESS_WAIT\tphase=") + phase + "\trequested_ms=" + std::to_string(milliseconds) +
            "\tresult=" + std::to_string(result) + "\terror=" + std::to_string(error) +
            "\ttick=" + std::to_string(GetTickCount64()) + "\telapsed_ms=" + std::to_string(GetTickCount64() - before_wait) + "\n";
        return result;
    };
    const auto terminate_job = [&](const char* phase, UINT code) {
        const BOOL result = TerminateJobObject(job.value, code);
        const DWORD error = result ? ERROR_SUCCESS : GetLastError();
        report += std::string("PROCESS_KILL_JOB\tphase=") + phase + "\tresult=" + std::to_string(result != FALSE) +
            "\terror=" + std::to_string(error) + "\tcode=" + std::to_string(code) + "\n";
        return result != FALSE;
    };
    const auto exit_status = [&](const char* phase, DWORD& code) {
        const BOOL result = GetExitCodeProcess(process.value, &code);
        const DWORD error = result ? ERROR_SUCCESS : GetLastError();
        report += std::string("PROCESS_EXIT\tphase=") + phase + "\tresult=" + std::to_string(result != FALSE) +
            "\terror=" + std::to_string(error) + "\tcode=" + std::to_string(code) + "\n";
    };
    const BOOL assigned = AssignProcessToJobObject(job.value, process.value);
    const DWORD assign_error = assigned ? ERROR_SUCCESS : GetLastError();
    report += "PROCESS\tassigned-job=" + std::to_string(assigned != FALSE) + "\terror=" + std::to_string(assign_error) + "\n";
    if (!assigned) {
        const BOOL killed = TerminateProcess(process.value, 1); // own suspended Pulse only
        const DWORD error = killed ? ERROR_SUCCESS : GetLastError();
        report += "PROCESS_KILL_SUSPENDED\tresult=" + std::to_string(killed != FALSE) + "\terror=" + std::to_string(error) + "\n";
        wait_owned("assignment-failure", 5000);
        DWORD code = 1; exit_status("assignment-failure", code); return false;
    }
    BOOL in_job = FALSE;
    const BOOL queried = IsProcessInJob(process.value, job.value, &in_job);
    const DWORD query_error = queried ? ERROR_SUCCESS : GetLastError();
    report += "PROCESS\tquery-job=" + std::to_string(queried != FALSE) + "\terror=" + std::to_string(query_error) +
        "\tin-owned-job=" + std::to_string(in_job != FALSE) + "\n";
    const DWORD resumed = ResumeThread(thread.value);
    const DWORD resume_error = resumed == static_cast<DWORD>(-1) ? GetLastError() : ERROR_SUCCESS;
    report += "PROCESS\tresume-previous-count=" + std::to_string(resumed) + "\terror=" + std::to_string(resume_error) + "\n";
    if (resumed == static_cast<DWORD>(-1)) {
        terminate_job("resume-failure", 1); wait_owned("resume-failure", 5000);
        DWORD code = 1; exit_status("resume-failure", code); return false;
    }
    const auto deadline = GetTickCount64() + 45000;
    bool crash_performed = false;
    pulse::app::ExplorerSourceSamples observations;
    const auto& sampled_unchanged = observations.unchanged;
    const auto& samples = observations.before_close;
    while (wait_owned("running-poll", 0) == WAIT_TIMEOUT && GetTickCount64() < deadline) {
        const bool authorized_before = WaitForSingleObject(close_authorized.value, 0) == WAIT_OBJECT_0;
        const auto observed = pulse::app::ObserveExplorerSource(source.window);
        const bool authorized_after = WaitForSingleObject(close_authorized.value, 0) == WAIT_OBJECT_0;
        observations.Record(authorized_before, authorized_after, pulse::app::UnchangedAvailableSource(before, observed));
        if (crash) {
            std::ifstream ready(control_root / L"takeover-ready.tsv", std::ios::binary);
            const std::string ready_text((std::istreambuf_iterator<char>(ready)), std::istreambuf_iterator<char>());
            if (ready_text == "READY\tWatcher request received by UI handoff\n") {
                report += "CRASH_TRIGGER\towned request ready; terminating only owned Pulse job\n";
                crash_performed = terminate_job("controlled-crash", 77);
                if (crash_performed) { wait_owned("controlled-crash", 5000); break; }
            }
        }
        Pump(100);
    }
    if (wait_owned("before-unwind", 0) == WAIT_TIMEOUT) {
        // Ask only this child UI to unwind before resorting to its own job.
        const BOOL posted = PostThreadMessageW(child.dwThreadId, WM_QUIT, 0, 0);
        const DWORD post_error = posted ? ERROR_SUCCESS : GetLastError();
        report += "PROCESS\tpost-quit=" + std::to_string(posted != FALSE) + "\terror=" + std::to_string(post_error) + "\n";
        const auto unwind = GetTickCount64() + 10000;
        while (wait_owned("unwind-poll", 0) == WAIT_TIMEOUT && GetTickCount64() < unwind) Pump(100);
        if (wait_owned("before-watchdog-kill", 0) == WAIT_TIMEOUT) {
            terminate_job("watchdog", 1); wait_owned("after-watchdog-kill", 5000);
        }
        report += "FAIL\towned Pulse watchdog expired; no Explorer process terminated\n";
    }
    DWORD exit_code = 1; exit_status("final", exit_code);
    std::ifstream input(control_root / L"takeover-report.tsv", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    report += text;
    const bool clean_stop = text.find("STOPPED\t1\n") != std::string::npos;
    stopped = wait_owned("final-stop-confirmation", 0) == WAIT_OBJECT_0;
    const auto after = pulse::app::ObserveExplorerSource(source.window);
    const bool source_unchanged = pulse::app::UnchangedAvailableSource(before, after);
    const bool close_signal = WaitForSingleObject(close_authorized.value, 0) == WAIT_OBJECT_0;
    report += "OBSERVATION\tsamples=" + std::to_string(samples) + "\tsampled_unchanged=" + std::to_string(sampled_unchanged) +
        "\tclose_authorized=" + std::to_string(close_signal) + "\ttransition_samples=" + std::to_string(observations.transition) +
        "\tauthorized_samples=" + std::to_string(observations.authorized) +
        "\tafter_available_unchanged=" + std::to_string(source_unchanged) + "\tbefore_style=" + std::to_string(before.style) +
        "\tbefore_alpha=" + std::to_string(before.alpha) + "\tafter_style=" + std::to_string(after.style) +
        "\tafter_alpha=" + std::to_string(after.alpha) + "\tafter_visible=" + std::to_string(after.visible) +
        "\tafter_enabled=" + std::to_string(after.enabled) + "\nLIMIT\tEndpoint and periodic samples do not observe every millisecond; cross-signal samples have uncertain ordering within the authorized close interval.\n";
    if (crash) {
        const bool pass = crash_performed && stopped && samples && sampled_unchanged && source_unchanged && !close_signal;
        report += pass ? "PASS\tcontrolled owned Pulse crash preserves available source\n" : "FAIL\tcontrolled crash source preservation\n";
        return pass;
    }
    if (!clean_stop) report += "FAIL\tno graceful Watcher stop report; no source style mutation attempted\n";
    return exit_code == 0 && clean_stop && samples && sampled_unchanged && (busy ? source_unchanged && !close_signal : close_signal) &&
        text.find("PASS\tproduction Watcher background-prepare/source-close/actual-present") != std::string::npos;

}
int Probe(const std::filesystem::path& control_root, HDESK desktop, HDESK original,
          const std::wstring& desktop_name, std::string& report, const std::wstring& pulse, bool busy, bool crash, bool resume,
          const std::wstring& external_folder) {
    wchar_t system[32768]{};
    if (!GetWindowsDirectoryW(system, ARRAYSIZE(system))) return 1;
    const auto explorer = (std::filesystem::path(system) / L"explorer.exe").wstring();
    const auto fixture_path = external_folder.empty() ? control_root / L"source" : std::filesystem::path(external_folder);
    const auto image = Canonical(explorer), fixture = Canonical(fixture_path.wstring());
    if (image.empty() || fixture.empty()) return 1;
    // Keep this uniquely created directory from being renamed/replaced during
    // observation and cleanup. Children remain readable by Explorer.
    Handle fixture_lock{CreateFileW(fixture_path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (fixture_lock.value == INVALID_HANDLE_VALUE) return 1;
    ComPtr<IShellWindows> windows;
    const HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows));
    if (FAILED(hr)) {
        report += "INCONCLUSIVE\tShellWindows unavailable\thresult=" + HResultText(hr) + "\n"; return 2;
    }
    bool desktop_complete = false, shell_complete = false;
    std::set<HWND> baseline;
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        report += "BASELINE_ATTEMPT\t" + std::to_string(attempt) + "\n";
        auto snapshot = Windows(original, &desktop_complete, &report);
        for (const auto& item : Sources(windows.Get(), &shell_complete, &report)) {
            const HWND root = GetAncestor(item.window, GA_ROOT);
            if (!root) {
                shell_complete = false;
                report += "BASELINE_ERROR\tstage=GetAncestor\terror=" + std::to_string(GetLastError()) +
                    "\thwnd=" + std::to_string(reinterpret_cast<uintptr_t>(item.window)) + "\n";
            } else snapshot.insert(root);
        }
        if (desktop_complete && shell_complete) { baseline = std::move(snapshot); break; }
        if (attempt < 3) Pump(100);
    }
    if (!desktop_complete || !shell_complete) {
        report += "INCONCLUSIVE\tincomplete baseline; Explorer not launched\n"; return 2;
    }
    report += "BASELINE\t" + std::to_string(baseline.size()) + "\n";
    if (external_folder.empty()) {
        std::wstring command = L"\"" + explorer + L"\" /select,\"" + (fixture_path / L"selected.txt").wstring() + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        auto desktop_arg = desktop_name; startup.lpDesktop = desktop_arg.data();
        startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWNOACTIVATE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(explorer.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                fixture_path.c_str(), &startup, &process)) {
            report += "FAIL\tCreateProcess error=" + std::to_string(GetLastError()) + "\n"; return 1;
        }
        // The launcher may hand off to an existing process. Never terminate it.
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        report += "LAUNCH\tpid=" + std::to_string(process.dwProcessId) + "\n";
    } else {
        report += "EXTERNAL\texpected_selection=empty\tfolder=" + Utf8(fixture) + "\n";
        std::cout << "WAITING\tbaseline recorded; trigger external application now\tfolder=" << Utf8(fixture) << std::endl;
    }
    std::set<HWND> reported;
    std::set<HWND> selection_prepared;
    const auto discovery_started = GetTickCount64();
    const auto budget = external_folder.empty() ? 15000u : 90000u;
    const auto deadline = discovery_started + budget;
    unsigned scan_number = 0;
    report += "DISCOVERY\tstarted=" + std::to_string(discovery_started) + "\tdeadline=" + std::to_string(deadline) + "\tbudget_ms=" + std::to_string(budget) + "\n";
    while (GetTickCount64() < deadline) {
        const auto scan_started = GetTickCount64();
        ++scan_number;
        bool scan_complete = false;
        const auto sources = Sources(windows.Get(), &scan_complete);
        unsigned matched = 0, known_paths = 0;
        for (auto source : sources) {
            const bool process_identity = ProcessIdentity(source, image);
            std::wstring path; std::vector<std::wstring> names;
            const bool read = Read(source, path, names);
            if (!path.empty()) ++known_paths;
            if (!Same(path, fixture)) continue;
            ++matched;
            if (reported.insert(source.window).second) {
                report += "FIXTURE_SOURCE\tfound_tick=" + std::to_string(GetTickCount64()) +
                    "\tsince_discovery_ms=" + std::to_string(GetTickCount64() - discovery_started) +
                    "\thwnd=" + std::to_string(reinterpret_cast<uintptr_t>(source.window)) +
                    "\tpid=" + std::to_string(source.pid) + "\ttid=" + std::to_string(source.tid) +
                    "\tcreated=" + std::to_string(source.created) + "\tdesktop=" + Utf8(DesktopName(source.tid)) +
                    "\tbaseline=" + std::to_string(baseline.contains(source.window)) +
                    "\ttabs=" + std::to_string(TabCount(source.window)) + "\n";
            }
            std::string detail = "FIXTURE_CHECK\tscan=" + std::to_string(scan_number) + "\ttick=" + std::to_string(GetTickCount64()) +
                "\thwnd=" + std::to_string(reinterpret_cast<uintptr_t>(source.window)) + "\tread=" + std::to_string(read) +
                "\tprocess_identity=" + std::to_string(process_identity);
            bool exact = false;
            if (!read) detail += "\treject=initial-selection-read";
            else if (!process_identity) detail += "\treject=initial-process-identity";
            else exact = ExactSource(source, windows.Get(), desktop, baseline, image, fixture, &detail);
            detail += "\taccepted=" + std::to_string(exact) + "\tscan_elapsed_ms=" + std::to_string(GetTickCount64() - scan_started) + "\n";
            report += detail;
            if (!exact) continue;
            const bool selection = external_folder.empty() ? names.size() == 1 && names.front() == L"selected.txt" : names.empty();
            if (!selection && external_folder.empty() && names.empty() &&
                GetTickCount64() - discovery_started >= 1000 && selection_prepared.insert(source.window).second) {
                // /select can be ignored while Explorer restores its view. Set
                // up only this verified new fixture; the next scan must read
                // the real selection back before Pulse is even launched.
                const HRESULT selected = PrepareFixtureSelection(source);
                report += "FIXTURE_PREPARE_SELECTION\thresult=" + HResultText(selected) + "\n";
                continue;
            }
            if (!selection && GetTickCount64() + 1000 < deadline) {
                report += "FIXTURE_WAIT\treason=expected-selection-pending\ttick=" + std::to_string(GetTickCount64()) + "\n";
                continue;
            }
            report += "SOURCE_SCAN\tnumber=" + std::to_string(scan_number) + "\tstart=" + std::to_string(scan_started) +
                "\telapsed_ms=" + std::to_string(GetTickCount64() - scan_started) + "\taccepted_fixture=1\n";
            report += "SELECTION\tcount=" + std::to_string(names.size());
            for (const auto& name : names) report += "\t" + Utf8(name);
            report += "\n";
            report += selection ? "PASS\treal COM fixture selection read\n" : "FAIL\tfixture selection differs\n";
            bool handoff_pass = true;
            SourceOwner owner;
            if (!pulse.empty()) {
                if (!selection) { report += "FAIL\thandoff precondition selection\n"; return 1; }
                const auto owner_value = static_cast<uintptr_t>((static_cast<uint64_t>(GetCurrentProcessId()) << 16) ^ GetTickCount64());
                if (!owner_value || GetPropW(source.window, pulse::app::kExplorerScopeOwner) ||
                    !SetPropW(source.window, pulse::app::kExplorerScopeOwner, reinterpret_cast<HANDLE>(owner_value))) {
                    report += "FAIL\tsource ownership marker unavailable\n"; return 1;
                }
                owner.window = source.window; owner.value = owner_value;
                bool stopped = false;
                handoff_pass = RunHandoff(source, control_root, pulse, busy, crash, resume, !external_folder.empty(), owner.value, report, stopped);
                if (!stopped) {
                    report += "FAIL\tWatcher stop not confirmed; no driver Quit issued\n"; return 1;
                }
                if (!IsWindow(source.window)) {
                    report += "SOURCE\tproduction watcher closed exact source\n";
                    return handoff_pass && !busy && !crash ? 0 : 1;
                }
                report += "CLEANUP\tWatcher stopped before driver source revalidation\n";
                if (!external_folder.empty()) {
                    report += "SOURCE\texternal native source retained; no driver cleanup Quit\n";
                    return handoff_pass && (busy || crash) ? 0 : 1;
                }
            }
            // Recheck immediately before the only mutation, including PID age,
            // COM identity, desktop membership, canonical folder and tab count.
            if (!ExactSource(source, windows.Get(), desktop, baseline, image, fixture) ||
                (owner.value && reinterpret_cast<uint64_t>(GetPropW(source.window, pulse::app::kExplorerScopeOwner)) != owner.value)) {
                report += "INCONCLUSIVE\tidentity changed before cleanup; source retained\n"; return 2;
            }
            const HRESULT quit = source.browser->Quit();
            const auto close_deadline = GetTickCount64() + 4000;
            while (IsWindow(source.window) && GetTickCount64() < close_deadline) Pump(50);
            if (FAILED(quit) || IsWindow(source.window)) {
                report += "FAIL\texact fixture Quit did not close source\n"; return 1;
            }
            report += "PASS\texact required-desktop fixture source closed\n";
            return selection && handoff_pass ? 0 : 1;
        }
        report += "SOURCE_SCAN\tnumber=" + std::to_string(scan_number) + "\tstart=" + std::to_string(scan_started) +
            "\telapsed_ms=" + std::to_string(GetTickCount64() - scan_started) + "\tcom_complete=" + std::to_string(scan_complete) +
            "\titems=" + std::to_string(sources.size()) + "\tknown_paths=" + std::to_string(known_paths) +
            "\tfixture_matches=" + std::to_string(matched) + "\n";
        Pump(100);
    }
    report += "INCONCLUSIVE\tno safely claimable required-desktop source; no Quit issued; any reused or other-desktop fixture window retained\n";
    return 2;
}
}

int wmain(int argc, wchar_t** argv) {
    bool current_desktop = false, busy = false, crash = false, resume = false;
    std::wstring pulse, external_folder;
    for (int index = 1; index < argc; ++index) {
        if (wcscmp(argv[index], L"--current-desktop") == 0) current_desktop = true;
        else if (wcscmp(argv[index], L"--busy") == 0) busy = true;
        else if (wcscmp(argv[index], L"--busy-resume") == 0) resume = true;
        else if (wcscmp(argv[index], L"--crash") == 0) crash = true;
        else if (wcscmp(argv[index], L"--handoff") == 0 && index + 1 < argc) pulse = argv[++index];
        else if (wcscmp(argv[index], L"--external-folder") == 0 && index + 1 < argc) external_folder = argv[++index];
        else { std::cerr << "Usage: pulse_explorer_source_probe [--current-desktop [--handoff <absolute pulse.exe> [--busy|--busy-resume|--crash] [--external-folder <absolute fixture>]]]\n"; return 1; }
    }
    if ((!pulse.empty() && (!current_desktop || !std::filesystem::path(pulse).is_absolute() || Canonical(pulse).empty())) ||
        ((busy || crash || resume) && pulse.empty()) || (static_cast<unsigned>(busy) + static_cast<unsigned>(crash) + static_cast<unsigned>(resume) > 1)) {
        std::cerr << "FAIL: handoff requires explicit current desktop and an existing absolute Pulse executable\n"; return 1;
    }
    const auto observer_desktop = DesktopName(GetCurrentThreadId());
    if (current_desktop && observer_desktop != L"Default") {
        std::cout << "FAIL\t--current-desktop requires Default; actual=" << Utf8(observer_desktop)
            << "; Explorer not launched\n";
        return 1;
    }
    std::string report = pulse.empty() ? "LIMIT\tSource discovery, COM selection read and guarded cleanup only; not production handoff validation.\n" :
        "MODE\tproduction Watcher to owned queue to UI handoff to successful presented/close; cleanup only after Watcher stop\n";
    report += current_desktop ? "MODE\tcurrent-desktop-explicit\n" : "MODE\tprivate-desktop-default\n";
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return 1;
    const auto bench = std::filesystem::path(executable).parent_path().parent_path() / L"bench_data";
    if (!external_folder.empty()) {
        const auto canonical = Canonical(external_folder);
        const DWORD attributes = GetFileAttributesW(external_folder.c_str());
        if (!current_desktop || pulse.empty() || !std::filesystem::path(external_folder).is_absolute() ||
            canonical.empty() || !Same(Canonical(std::filesystem::path(canonical).parent_path().wstring()), Canonical(bench.wstring())) ||
            attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            std::cerr << "FAIL: external folder requires current-desktop handoff and an existing direct bench_data directory without reparse points\n";
            return 1;
        }
        external_folder = canonical;
    }
    const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto fixture = bench / (L"explorer-source-" + suffix);
    if (!CreateDirectoryW(bench.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return 1;
    if (!CreateDirectoryW(fixture.c_str(), nullptr)) return 1;
    if (external_folder.empty()) {
        const auto source_directory = fixture / L"source";
        if (!CreateDirectoryW(source_directory.c_str(), nullptr)) return 1;
        Handle selected{CreateFileW((source_directory / L"selected.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (selected.value == INVALID_HANDLE_VALUE) return 1;
    } else {
        Handle path_file{CreateFileW((fixture / L"external-folder.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        DWORD written = 0;
        const auto bytes = static_cast<DWORD>(external_folder.size() * sizeof(wchar_t));
        if (path_file.value == INVALID_HANDLE_VALUE || !WriteFile(path_file.value, external_folder.data(), bytes, &written, nullptr) || written != bytes) return 1;
    }
    const auto original = GetThreadDesktop(GetCurrentThreadId());
    const auto desktop_name = current_desktop ? observer_desktop : L"PulseExplorerSourceProbe." + suffix;
    const auto desktop = current_desktop ? original :
        CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    const DWORD desktop_error = desktop ? ERROR_SUCCESS : GetLastError();
    int result = 1;
    // The read-only observer can use the existing Shell COM activation context.
    // Only the launched Explorer receives lpDesktop; ExactSource still requires
    // membership of the explicitly required desktop before Quit, without fallback.
    report += "DESKTOP\tobserver=" + Utf8(observer_desktop) +
        "\tsource_required=" + Utf8(desktop_name) + "\n";
    if (desktop) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(initialized)) {
            result = Probe(fixture, desktop, original, desktop_name, report, pulse, busy, crash, resume, external_folder);
            CoUninitialize();
        } else report += "FAIL\tCOM initialization failed\thresult=" + HResultText(initialized) + "\n";
    } else report += "FAIL\trequired desktop unavailable\terror=" + std::to_string(desktop_error) + "\n";
    if (!current_desktop && desktop && !CloseDesktop(desktop)) { report += "FAIL\tclose probe desktop handle\n"; result = 1; }
    report += "RESULT\t" + std::to_string(result) + "\t0=supported 2=inconclusive 1=failed\n";
    Handle output{CreateFileW((fixture / L"source-report.tsv").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    DWORD written = 0;
    if (output.value == INVALID_HANDLE_VALUE || !WriteFile(output.value, report.data(),
        static_cast<DWORD>(report.size()), &written, nullptr) || written != report.size()) result = 1;
    std::cout << "Fixture: " << Utf8(fixture.wstring()) << '\n' << report;
    return result;
}

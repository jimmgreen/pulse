#include "../common/native_browse_intent.h"
#include <cstdio>
#include <thread>

namespace nb = pulse::native_browse_intent;
namespace {
unsigned passed = 0, failed = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (ok) ++passed; else ++failed;
}
struct Window {
    HWND value = CreateWindowExW(0, L"STATIC", L"native-intent-hidden-test", WS_POPUP,
        0, 0, 20, 20, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~Window() { if (value) DestroyWindow(value); }
};
struct Pair {
    nb::Observer observer;
    nb::Publisher publisher;
    explicit Pair(const std::wstring& name) {
        std::puts("[STEP] observer StartForTest");
        const auto started = observer.StartForTest(name);
        std::printf("[STEP] observer result=%lu\n", started);
        const auto opened = publisher.OpenForTest(name);
        Check(started == 0 && opened == 0, "private observer/publisher connection");
        if (started || opened) ExitProcess(20);
    }
};
DWORD ProcessIntegrity(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return MAXDWORD;
    alignas(TOKEN_MANDATORY_LABEL) BYTE data[1024]{}; DWORD bytes = 0;
    DWORD result = MAXDWORD;
    if (GetTokenInformation(token, TokenIntegrityLevel, data, sizeof(data), &bytes)) {
        const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(data)->Label.Sid;
        if (IsValidSid(sid)) result = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
    }
    CloseHandle(token); return result;
}
DWORD ObjectIntegrity(HANDLE object) {
    DWORD bytes = 0;
    GetKernelObjectSecurity(object, LABEL_SECURITY_INFORMATION, nullptr, 0, &bytes);
    if (!bytes) return MAXDWORD;
    std::vector<BYTE> data(bytes);
    if (!GetKernelObjectSecurity(object, LABEL_SECURITY_INFORMATION, data.data(), bytes, &bytes)) return MAXDWORD;
    PACL sacl = nullptr; BOOL present = FALSE, defaulted = FALSE;
    if (!GetSecurityDescriptorSacl(data.data(), &present, &sacl, &defaulted) || !present || !sacl) return MAXDWORD;
    for (DWORD i = 0; i < sacl->AceCount; ++i) {
        void* ace = nullptr;
        if (GetAce(sacl, i, &ace) && static_cast<ACE_HEADER*>(ace)->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE) {
            auto* sid = &static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace)->SidStart;
            if (IsValidSid(sid)) return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
        }
    }
    return MAXDWORD;
}
DWORD last_child_integrity = MAXDWORD;
bool Child(const std::wstring& executable, const std::wstring& mode, const std::wstring& name) {
    auto command = L"\"" + executable + L"\" " + mode + L" " + name;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &child)) return false;
    CloseHandle(child.hThread);
    last_child_integrity = ProcessIntegrity(child.hProcess);
    const DWORD wait = WaitForSingleObject(child.hProcess, 10000);
    DWORD code = STILL_ACTIVE; GetExitCodeProcess(child.hProcess, &code); CloseHandle(child.hProcess);
    std::printf("[CHILD] pid=%lu integrity=%lu wait=%lu exit=%lu\n", child.dwProcessId, last_child_integrity, wait, code);
    return wait == WAIT_OBJECT_0 && code == 0;
}
}
int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("[START] pid=%lu argc=%d integrity=%lu\n", GetCurrentProcessId(), argc, ProcessIntegrity(GetCurrentProcess()));
    if (argc == 3 && !wcscmp(argv[1], L"--publish-exit")) {
        nb::Publisher publisher;
        if (publisher.OpenForTest(argv[2])) return 10;
        const auto result = publisher.Publish({L"C:\\owned\\cross-process", L"file.txt", 10000});
        if (result.error || publisher.CommitLaunch(result.ticket)) return 11;
        // Return normally: no persistent publisher process or handle remains.
        return 0;
    }
    if (argc == 3 && !wcscmp(argv[1], L"--abandon")) {
        nb::Observer observer;
        if (observer.StartForTest(argv[2])) return 12;
        if (WaitForSingleObject(observer.ChannelForTest()->mutex, 1000) != WAIT_OBJECT_0) return 13;
        ExitProcess(0);
    }
    wchar_t image[32768]{}; GetModuleFileNameW(nullptr, image, ARRAYSIZE(image));
    const std::wstring child_executable = argc == 3 && !wcscmp(argv[1], L"--publisher-path") ? argv[2] : image;
    const auto prefix = L"intent-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    {
        nb::Publisher absent;
        Check(absent.OpenForTest(prefix + L"-absent") != 0, "publisher cannot create an orphan ledger without an observer");
        Check(absent.Publish({L"C:\\owned", {}, 1000}).error == ERROR_NOT_READY, "disconnected publication explicitly fails");
    }
    {
        const auto name = prefix + L"-security";
        Pair p(name);
        const DWORD mutex_label = ObjectIntegrity(p.observer.ChannelForTest()->mutex);
        const DWORD mapping_label = ObjectIntegrity(p.observer.ChannelForTest()->mapping);
        std::printf("[SECURITY] observer_integrity=%lu mutex_label=%lu mapping_label=%lu version=%u\n",
            ProcessIntegrity(GetCurrentProcess()), mutex_label, mapping_label, nb::detail::kVersion);
        Check(mutex_label == SECURITY_MANDATORY_LOW_RID && mapping_label == SECURITY_MANDATORY_LOW_RID,
            "both same-user passive ledger objects have an explicit Low mandatory label");
        Check(Child(child_executable, L"--publish-exit", name), "old-v2-compatible publisher opens and publishes through Low-labelled objects");
        const auto peer_integrity = last_child_integrity;
        if (ProcessIntegrity(GetCurrentProcess()) >= SECURITY_MANDATORY_MEDIUM_RID && peer_integrity == SECURITY_MANDATORY_LOW_RID) {
            PSECURITY_DESCRIPTOR medium = nullptr;
            const BOOL descriptor = ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;;NW;;;ME)", SDDL_REVISION_1, &medium, nullptr);
            const bool labelled = descriptor && SetKernelObjectSecurity(p.observer.ChannelForTest()->mutex, LABEL_SECURITY_INFORMATION, medium) &&
                SetKernelObjectSecurity(p.observer.ChannelForTest()->mapping, LABEL_SECURITY_INFORMATION, medium);
            if (medium) LocalFree(medium);
            Check(labelled && ObjectIntegrity(p.observer.ChannelForTest()->mutex) == SECURITY_MANDATORY_MEDIUM_RID &&
                  ObjectIntegrity(p.observer.ChannelForTest()->mapping) == SECURITY_MANDATORY_MEDIUM_RID,
                "private fixture models old Medium-labelled v2 objects without changing any production namespace");
            Check(!Child(child_executable, L"--publish-exit", name) && last_child_integrity == SECURITY_MANDATORY_LOW_RID,
                "real Low publisher reproduces write-up failure against old Medium v2 objects");
            nb::Observer replacement;
            Check(replacement.StartForTest(name) == 0 && ObjectIntegrity(replacement.ChannelForTest()->mutex) == SECURITY_MANDATORY_LOW_RID &&
                  ObjectIntegrity(replacement.ChannelForTest()->mapping) == SECURITY_MANDATORY_LOW_RID,
                "new observer validates and relabels existing compatible v2 objects without a schema bump");
            Check(Child(child_executable, L"--publish-exit", name) && last_child_integrity == SECURITY_MANDATORY_LOW_RID,
                "unchanged Low publisher succeeds after observer-side label correction");
        } else std::puts("[SKIP] old Medium-label write-up regression requires a Medium-or-higher observer and a real Low peer; not counted as passed");
        HANDLE source = nullptr, restricted = nullptr;
        BYTE restricted_sid[SECURITY_MAX_SID_SIZE]{}; DWORD sid_size = sizeof(restricted_sid);
        bool made = OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &source) &&
            CreateWellKnownSid(WinRestrictedCodeSid, nullptr, restricted_sid, &sid_size);
        SID_AND_ATTRIBUTES restriction{restricted_sid, 0};
        made = made && CreateRestrictedToken(source, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 1, &restriction, &restricted);
        Check(made && IsTokenRestricted(restricted), "create real restricted token with an additional restricting SID");
        HANDLE restricted_mutex = nullptr, restricted_map = nullptr;
        DWORD mutex_error = 0, map_error = 0;
        bool impersonated = false, reverted = false;
        if (made && ImpersonateLoggedOnUser(restricted)) {
            impersonated = true;
            const auto object = L"Local\\Pulse.NativeBrowseIntent.v" + std::to_wstring(nb::detail::kVersion) + L"." + std::to_wstring(p.observer.ChannelForTest()->scope);
            restricted_mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (object + L".mutex").c_str()); mutex_error = GetLastError();
            restricted_map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, (object + L".map").c_str()); map_error = GetLastError();
            reverted = RevertToSelf() != FALSE;
        }
        Check(impersonated && reverted && !restricted_mutex && !restricted_map && mutex_error == ERROR_ACCESS_DENIED && map_error == ERROR_ACCESS_DENIED,
            "same-user DACL still rejects real restricted-token mutex and mapping access despite Low labels");
        if (restricted_mutex) CloseHandle(restricted_mutex); if (restricted_map) CloseHandle(restricted_map);
        if (restricted) CloseHandle(restricted); if (source) CloseHandle(source);
    }
    {
        Pair p(prefix + L"-lifetime");
        Window existing;
        const auto publication = p.publisher.Publish({L"c:/owned/./folder/", L"FILE.TXT", 200});
        Check(existing.value && publication.error == 0, "publish complete normalized target before launch");
        Check(p.observer.ShouldWait(existing.value, L"C:\\owned\\folder", false).state == nb::State::None,
            "existing prelaunch HWND is never claimed by new request");
        const auto slower = p.publisher.Publish({L"C:\\owned\\folder", L"FILE.TXT", 5000});
        Check(!slower.error, "same-target second ticket keeps its independent original TTL");
        Window fresh;
        Check(fresh.value && !IsWindowVisible(fresh.value), "test creates hidden ordinary HWND only, no Explorer");
        Check(p.observer.ShouldWait(fresh.value, {}, false).state == nb::State::Wait, "initial Home/unknown folder delays veil before launch commit");
        Check(p.observer.MatchAndRetain(fresh.value, L"C:\\OWNED\\FOLDER", {L"file.txt"}, false).state == nb::State::Wait,
            "selection not ready cannot prematurely bind a reveal");
        Check(p.observer.MatchAndRetain(fresh.value, L"C:\\OWNED\\FOLDER", {L"wrong.txt"}, true).state == nb::State::Wait,
            "wrong selection does not bind and preserves pending target wait");
        Check(p.observer.IsRetained(fresh.value).state == nb::State::None, "wrong-selection window remains unretained");
        Check(p.publisher.CommitLaunch(publication.ticket) == 0, "successful native launch commits existing ticket");
        Check(p.observer.MatchAndRetain(fresh.value, L"C:\\owned\\folder", {L"file.txt"}, true).state == nb::State::Retained,
            "matching path and selection binds full HWND/process identity without SetProp");
        size_t pending_same_window = 0;
        for (const auto& c : p.observer.ChannelForTest()->candidates->entries)
            if (c.state == 1 && c.identity.window == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fresh.value))) ++pending_same_window;
        Check(pending_same_window == 0 && p.observer.ChannelForTest()->data->slots[slower.ticket.slot].state == 1,
            "retaining one HWND retires its other provisional candidates without consuming the sibling ticket");
        Sleep(450);
        Window sibling;
        Check(p.observer.MatchAndRetain(sibling.value, L"C:\\owned\\folder", {L"file.txt"}, true).state == nb::State::Retained,
            "slow same-target sibling binds the second ticket after the first ticket expires");
        Check(p.observer.IsRetained(fresh.value).state == nb::State::Retained, "retained HWND persists after pending ticket expires");
        Check(p.observer.ForgetWindow(fresh.value) == 0 && p.observer.IsRetained(fresh.value).state == nb::State::None,
            "DESTROY/CREATE ForgetWindow prevents same-process HWND identity reuse");
    }
    {
        Pair p(prefix + L"-cancel");
        const auto a = p.publisher.Publish({L"C:\\owned\\cancel", {}, 5000});
        Check(a.error == 0 && p.publisher.Cancel(a.ticket) == 0, "definitively failed launch cancels pending ticket");
        Window fresh;
        Check(p.observer.ShouldWait(fresh.value, {}, false).state == nb::State::None, "cancelled launch leaves no broad pending exemption");
        const auto b = p.publisher.Publish({L"C:\\owned\\other", {}, 80});
        Check(b.error == 0 && p.publisher.Cancel(a.ticket) == ERROR_NOT_FOUND, "reused ring slot rejects stale generation ticket");
        auto forged = b.ticket; ++forged.epoch;
        Check(p.publisher.CommitLaunch(forged) == ERROR_INVALID_HANDLE, "ticket from a different ledger lifetime is rejected");
        Sleep(100);
        Check(p.observer.ShouldWait(fresh.value, L"C:\\owned\\other", false).state == nb::State::None, "unclaimed ticket expires at bounded deadline");
        Check(p.observer.MatchAndRetain(fresh.value, L"C:/owned/other", {}, true).state == nb::State::None,
            "expired ticket without any observed candidate cannot claim even the exact target");
    }
    {
        Pair p(prefix + L"-concurrency");
        auto a = p.publisher.Publish({L"C:\\owned\\alpha", L"a.txt", 5000});
        auto b = p.publisher.Publish({L"C:\\owned\\beta", L"b.txt", 5000});
        Window alpha, beta, unrelated;
        Check(!a.error && !b.error, "two independent target tickets coexist");
        Sleep(1550);
        Check(p.observer.ShouldWait(unrelated.value, L"C:\\owned\\other", true).state == nb::State::None,
            "unrelated folder is released after bounded initial settle, not globally exempted");
        Check(p.observer.ShouldWait(alpha.value, L"C:\\owned\\alpha", false).state == nb::State::Wait,
            "matching folder with unfinished selection waits beyond initial settle");
        Check(p.observer.MatchAndRetain(alpha.value, L"C:\\owned\\alpha", {L"b.txt"}, true).state != nb::State::Retained,
            "different target selections never cross-bind");
        Check(p.observer.MatchAndRetain(beta.value, L"C:\\owned\\beta", {L"b.txt"}, true).state == nb::State::Retained &&
              p.observer.MatchAndRetain(alpha.value, L"C:\\owned\\alpha", {L"a.txt"}, true).state == nb::State::Retained,
            "concurrent targets bind their own matching windows");
    }
    {
        const auto desktop_name = prefix + L"-desktop";
        HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        Check(desktop != nullptr, "create private non-visible desktop without switching the interactive desktop");
        if (desktop) {
            HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr), finish = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HWND other_window = nullptr;
            DWORD publish_empty = ERROR_GEN_FAILURE, publish_empty_again = ERROR_GEN_FAILURE;
            BOOL empty_result = TRUE, one_result = FALSE, final_result = TRUE;
            DWORD empty_error = 1, one_error = 1, final_error = 1;
            unsigned empty_count = 0, one_count = 0, final_count = 0, one_owned = 0, current_owned = 0;
            bool thread_ready = false;
            std::thread worker([&] {
                const auto enumerate = [&](unsigned& count, DWORD& error) {
                    struct Counts { unsigned& total; unsigned& owned; HWND target; } counts{count, current_owned, other_window};
                    current_owned = 0;
                    SetLastError(ERROR_SUCCESS);
                    const BOOL result = EnumDesktopWindows(GetThreadDesktop(GetCurrentThreadId()), [](HWND hwnd, LPARAM value) -> BOOL {
                        auto& counts = *reinterpret_cast<Counts*>(value);
                        ++counts.total; if (hwnd == counts.target) ++counts.owned;
                        char name[128]{}; GetClassNameA(hwnd, name, sizeof(name));
                        std::printf("[PRIVATE_DESKTOP_WINDOW] owned=%d class=%s\n", hwnd == counts.target, name);
                        return TRUE;
                    }, reinterpret_cast<LPARAM>(&counts));
                    error = GetLastError(); return result;
                };
                if (SetThreadDesktop(desktop)) {
                    nb::Observer observer; nb::Publisher publisher;
                    thread_ready = !observer.StartForTest(prefix + L"-private") && !publisher.OpenForTest(prefix + L"-private");
                    empty_result = enumerate(empty_count, empty_error);
                    publish_empty = publisher.Publish({L"C:/owned/empty", {}, 3000}).error;
                    other_window = CreateWindowExW(0, L"STATIC", L"hidden-private-ledger-test", WS_POPUP, 0, 0, 20, 20,
                        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                    one_result = enumerate(one_count, one_error); one_owned = current_owned;
                    SetEvent(ready); WaitForSingleObject(finish, INFINITE);
                    if (other_window) DestroyWindow(other_window);
                    final_result = enumerate(final_count, final_error);
                    publish_empty_again = publisher.Publish({L"C:/owned/empty-again", {}, 3000}).error;
                } else SetEvent(ready);
            });
            WaitForSingleObject(ready, INFINITE);
            Pair local(prefix + L"-private");
            Check(other_window && local.observer.ShouldWait(other_window, {}, false).state == nb::State::None,
                "window on another desktop is outside this observer's scope");
            SetEvent(finish); worker.join(); CloseHandle(ready); CloseHandle(finish); CloseDesktop(desktop);
            std::printf("[EMPTY_DESKTOP] initial_result=%d error=%lu count=%u; one_result=%d error=%lu count=%u; final_result=%d error=%lu count=%u\n",
                empty_result, empty_error, empty_count, one_result, one_error, one_count, final_result, final_error, final_count);
            Check(thread_ready && !empty_result && !empty_error && empty_count == 0 && one_result && one_count >= 1 && one_owned == 1 &&
                  !final_result && !final_error && final_count == 0, "real Win32 empty/one-owned-hidden/destroyed-empty desktop enumeration contract (IME auxiliaries counted separately)");
            Check(publish_empty == 0 && publish_empty_again == 0, "empty desktop publication succeeds without adding a dummy window");
        }
    }
    {
        Pair p(prefix + L"-paths");
        for (const auto& path : std::vector<std::wstring>{L"relative", L"C:relative", L"C:\\..\\escape", L"\\\\server", L"\\\\.\\C:", L"C:\\bad\"name", L"C:\\trailing. ", std::wstring(L"C:\\a\0b", 7), std::wstring(32768, L'x')})
            Check(p.publisher.Publish({path, {}, 1000}).error == ERROR_INVALID_NAME, "malformed/ambiguous/oversized path rejected without truncation");
        Check(p.publisher.Publish({L"C:\\", L"bad\\name", 1000}).error == ERROR_INVALID_NAME &&
              p.publisher.Publish({L"C:\\", std::wstring(L"x\0y", 3), 1000}).error == ERROR_INVALID_NAME,
            "selection must be one complete component, no embedded NUL");
        auto root = p.publisher.Publish({L"C:/", {}, 3000});
        auto unc = p.publisher.Publish({L"\\\\?\\unc\\server\\share\\dir\\", {}, 3000});
        Window a, b;
        Check(!root.error && p.observer.MatchAndRetain(a.value, L"c:\\", {}, true).state == nb::State::Retained, "drive root survives normalization");
        Check(!unc.error && p.observer.MatchAndRetain(b.value, L"\\\\SERVER\\SHARE\\DIR", {}, true).state == nb::State::Retained,
            "UNC/extended UNC case and slash normalization agree");
    }
    {
        Pair p(prefix + L"-long-identity");
        std::wstring long_path = L"C:/";
        for (unsigned i = 0; i < 130; ++i) { long_path += std::wstring(200, L'x'); long_path += L'/'; }
        const auto published = p.publisher.Publish({long_path, L"中文.txt", 3000});
        Window target;
        Check(!published.error && p.observer.MatchAndRetain(target.value, long_path, {L"中文.txt"}, true).state == nb::State::Retained,
            "long Unicode target is stored and matched without truncation or ACP loss");
        nb::detail::deny_identity_for_test = target.value;
        const auto denied = p.observer.IsRetained(target.value);
        Check(denied.state == nb::State::Unavailable && denied.error == ERROR_ACCESS_DENIED,
            "injected identity-access failure for a real live HWND is Unavailable, never unprotected None");
        nb::detail::deny_identity_for_test = nullptr;
        Check(p.observer.IsRetained(target.value).state == nb::State::Retained, "identity query failure does not erase an existing retained record");
        auto* channel = p.observer.ChannelForTest();
        { nb::detail::Lock lock(*channel);
          for (auto& retained : channel->data->retained) if (retained.generation) { retained.identity.created ^= 1; break; } }
        Check(p.observer.IsRetained(target.value).state == nb::State::None, "same HWND/PID/TID with wrong process creation time cannot inherit retention");
    }
    {
        const auto name = prefix + L"-cross";
        nb::Observer observer; Check(observer.StartForTest(name) == 0, "observer owns cross-process ledger lifetime");
        Window existing;
        Check(Child(child_executable, L"--publish-exit", name), "real child publisher exits successfully after publishing and committing");
        Window fresh;
        Check(observer.ShouldWait(existing.value, {}, false).state == nb::State::None, "child snapshot excludes parent's existing HWND");
        Check(observer.MatchAndRetain(fresh.value, L"C:\\owned\\cross-process", {L"file.txt"}, true).state == nb::State::Retained,
            "observer retains matching window after publisher process and all its handles exit");
    }
    {
        Pair p(prefix + L"-capacity"); std::vector<nb::Ticket> tickets;
        for (size_t i = 0; i < nb::detail::kRequests; ++i) {
            const auto value = p.publisher.Publish({L"C:\\owned\\capacity-" + std::to_wstring(i), {}, 30000});
            if (!value.error) tickets.push_back(value.ticket);
        }
        const auto full = p.publisher.Publish({L"C:\\owned\\overflow", {}, 80});
        Check(tickets.size() == nb::detail::kRequests && full.error == ERROR_BUSY, "full request ring reports capacity without overwriting live tickets");
        Window overflow_window;
        auto overloaded = p.observer.ShouldWait(overflow_window.value, L"C:\\owned\\overflow", true);
        Check(overloaded.state == nb::State::Unavailable && overloaded.error == ERROR_BUSY,
            "full ring publishes bounded observable fail-open for a new native window");
        Pair independent(prefix + L"-independent-overload");
        Check(independent.observer.ShouldWait(overflow_window.value, {}, false).state == nb::State::None,
            "capacity fail-open never extends another ledger scope");
        Sleep(100);
        Check(p.observer.ShouldWait(overflow_window.value, L"C:\\owned\\overflow", true).state != nb::State::Unavailable,
            "capacity fail-open automatically expires back to ordinary matching");
        unsigned native_attempts = 0;
        if (full.error) ++native_attempts; // Caller policy: failure to reserve must not consume the original operation.
        Check(native_attempts == 1 && !tickets.empty() && p.publisher.Cancel(tickets.front()) == 0 &&
              p.publisher.Publish({L"C:\\owned\\after-cancel", {}, 1000}).error == 0,
            "caller can still attempt native once on full-ring status; cancelled slot is reusable");
    }
    {
        Pair p(prefix + L"-same-path-burst");
        std::vector<nb::Ticket> tickets;
        for (size_t i = 0; i < nb::detail::kRequests; ++i) {
            const auto value = p.publisher.Publish({L"C:/owned/same-burst", {}, 30000});
            if (!value.error) tickets.push_back(value.ticket);
        }
        Check(tickets.size() == nb::detail::kRequests, "64 same-target tickets all reserve independently before any view arrives");
        std::vector<std::unique_ptr<Window>> windows;
        size_t retained = 0;
        for (size_t i = 0; i < tickets.size(); ++i) {
            windows.push_back(std::make_unique<Window>());
            if (p.observer.MatchAndRetain(windows.back()->value, L"C:/owned/same-burst", {}, true).state == nb::State::Retained) ++retained;
        }
        Check(retained == nb::detail::kRequests, "64 same-target windows each consume a distinct ticket, not the first ticket repeatedly");
        const auto next = p.publisher.Publish({L"C:/owned/next-burst", {}, 3000});
        Check(!next.error && !tickets.empty() && p.publisher.CommitLaunch(tickets.front()) == 0 &&
              p.publisher.Cancel(tickets.front()) == 0 && p.publisher.CommitLaunch(next.ticket) == 0,
            "bound slots are reusable and late commit/cancel cannot corrupt a new slot generation");
    }

    {
        const auto name = prefix + L"-late-view"; Pair p(name); p.observer.CandidateLeaseForTest(900);
        Window existing;
        const auto ticket = p.publisher.Publish({L"C:/owned/late-view", L"target.txt", 1800});
        Check(!ticket.error && !p.publisher.CommitLaunch(ticket.ticket), "late-view fixture publishes before native launch");
        Sleep(1550);
        Window observed;
        Check(p.observer.ShouldWait(observed.value, {}, false).state == nb::State::Wait,
            "SHOW after publish settle but before TTL registers unknown Home candidate before veil");
        Check(p.observer.ShouldWait(existing.value, {}, false).state == nb::State::None,
            "prelaunch HWND never becomes a late-view candidate");
        Sleep(300);
        Check(p.observer.ShouldWait(observed.value, L"C:/owned/late-view", false).state == nb::State::Wait,
            "observed folder waits for selection after original admission expiry");
        Window late;
        Check(p.observer.MatchAndRetain(late.value, L"C:/owned/late-view", {L"target.txt"}, true).state == nb::State::None,
            "pin does not extend admission to a different HWND first observed after original TTL");
        nb::Observer peer;
        Check(peer.StartForTest(name) == 0 && peer.MatchAndRetain(observed.value, L"C:/owned/late-view", {L"target.txt"}, true).state == nb::State::Retained,
            "second observer matches folder and selection after original TTL using shared candidate");
        Sleep(650);
        Check(p.observer.IsRetained(observed.value).state == nb::State::Retained,
            "same HWND remains native beyond original TTL and readiness bound instead of HIDE/DESTROY");
    }
    {
        Pair p(prefix + L"-candidate-terminal"); p.observer.CandidateLeaseForTest(180);
        auto ticket = p.publisher.Publish({L"C:/owned/terminal", L"item.txt", 60}); Window window;
        Check(!ticket.error && p.observer.ShouldWait(window.value, {}, false).state == nb::State::Wait,
            "terminal fixture observes candidate before short launch TTL");
        Sleep(230);
        auto result = p.observer.ShouldWait(window.value, {}, false);
        Check(result.state == nb::State::Retained && result.error == ERROR_TIMEOUT,
            "readiness deadline ends waiting as exact-window retained with timeout diagnostic");
        Sleep(50);
        Check(p.observer.IsRetained(window.value).state == nb::State::Retained && p.observer.MatchAndRetain(window.value, L"C:/owned/terminal", {L"item.txt"}, true).error == ERROR_TIMEOUT,
            "terminal preserve does not turn into takeover while folder remains unreadable");
        for (auto& candidate : p.observer.ChannelForTest()->candidates->entries) candidate.state = 0;
        Check(p.observer.IsRetained(window.value).error == ERROR_TIMEOUT,
            "conservative-retain diagnostic survives candidate record reclamation in observer sidecar");
        Window other;
        Check(p.observer.ShouldWait(other.value, {}, false).state == nb::State::None,
            "terminal preserve is not a global exemption for another new window");
    }

    for (bool reverse : {false, true}) {
        Pair p(prefix + (reverse ? L"-candidate-two-ba" : L"-candidate-two-ab"));
        p.observer.CandidateLeaseForTest(180);
        auto ticket = p.publisher.Publish({L"C:/owned/two-unknown", L"target.txt", 60}); Window a, b;
        Check(!ticket.error && p.observer.ShouldWait(a.value, {}, false).state == nb::State::Wait &&
              p.observer.ShouldWait(b.value, {}, false).state == nb::State::Wait,
            "one ticket admits two unknown HWND candidates before original TTL");
        Sleep(230);
        const auto first = p.observer.ShouldWait(reverse ? b.value : a.value, {}, false);
        const auto second = p.observer.ShouldWait(reverse ? a.value : b.value, {}, false);
        Check(first.state == nb::State::Retained && first.error == ERROR_TIMEOUT &&
              second.state == nb::State::Retained && second.error == ERROR_TIMEOUT,
            reverse ? "B then A terminal preservation does not consume the sibling candidate" :
                      "A then B terminal preservation does not consume the sibling candidate");
        Check(p.observer.IsRetained(a.value).state == nb::State::Retained &&
              p.observer.MatchAndRetain(b.value, L"C:/owned/two-unknown", {L"target.txt"}, true).state == nb::State::Retained,
            "both same-generation candidates stay native when the true target becomes readable later");
    }
    {
        const auto name = prefix + L"-candidate-stop-two"; Pair p(name);
        auto ticket = p.publisher.Publish({L"C:/owned/stop-two", {}, 1000}); Window a, b;
        Check(!ticket.error && p.observer.ShouldWait(a.value, {}, false).state == nb::State::Wait &&
              p.observer.ShouldWait(b.value, {}, false).state == nb::State::Wait,
            "graceful Stop fixture admits two unknown candidates for one generation");
        p.observer.Stop(); nb::Observer restarted;
        Check(restarted.StartForTest(name) == 0 && restarted.IsRetained(a.value).state == nb::State::Retained &&
              restarted.IsRetained(b.value).state == nb::State::Retained,
            "normal Stop and restart persist both same-ticket unknown identities, not just the first");
    }
    {
        Pair p(prefix + L"-candidate-precise-consume");
        auto ticket = p.publisher.Publish({L"C:/owned/precise-once", {}, 1000}); Window a, b;
        Check(!ticket.error && p.observer.ShouldWait(a.value, {}, false).state == nb::State::Wait &&
              p.observer.ShouldWait(b.value, {}, false).state == nb::State::Wait,
            "precise-consumption fixture has two provisional candidates");
        Check(p.observer.MatchAndRetain(a.value, L"C:/owned/precise-once", {}, true).state == nb::State::Retained &&
              p.observer.ShouldWait(b.value, {}, false).state == nb::State::None,
            "a genuine precise match still consumes its ticket and excludes other provisional candidates");
    }
    {
        Pair p(prefix + L"-candidate-cancel"); p.observer.CandidateLeaseForTest(500);
        auto ticket = p.publisher.Publish({L"C:/owned/cancel-late", {}, 60}); Window window;
        Check(!ticket.error && p.observer.ShouldWait(window.value, {}, false).state == nb::State::Wait,
            "cancel fixture establishes a provisional candidate");
        Sleep(90);
        Check(p.publisher.Cancel(ticket.ticket) == 0 && p.observer.MatchAndRetain(window.value, L"C:/owned/cancel-late", {}, true).state == nb::State::None,
            "v2 Cancel after original TTL but before readiness bound revokes pinned candidate");
        auto replacement = p.publisher.Publish({L"C:/owned/reused", {}, 1000});
        Check(!replacement.error && replacement.ticket.slot == ticket.ticket.slot &&
              p.observer.MatchAndRetain(window.value, L"C:/owned/cancel-late", {}, true).state == nb::State::None,
            "slot reuse cannot reinterpret old candidate with a new target/generation");
    }
    {
        Pair p(prefix + L"-candidate-mismatch");
        auto ticket = p.publisher.Publish({L"C:/owned/match-only", {}, 1000}); Window wrong;
        Check(!ticket.error && p.observer.ShouldWait(wrong.value, {}, false).state == nb::State::Wait,
            "mismatch fixture observes unknown candidate during admission");
        Sleep(1550);
        Check(p.observer.MatchAndRetain(wrong.value, L"C:/owned/different", {}, true).state == nb::State::None,
            "known different folder exits candidate after finite settle even after original TTL");
        Check(p.observer.IsRetained(wrong.value).state == nb::State::None,
            "excluded known mismatch is not permanently retained");
        nb::detail::Lock lock(*p.observer.ChannelForTest());
        Check(!p.observer.ChannelForTest()->data->slots[ticket.ticket.slot].state,
            "excluded last candidate releases original expired ticket rather than occupying ring until pin bound");
    }

    {
        Pair p(prefix + L"-candidate-selection"); p.observer.CandidateLeaseForTest(160);
        auto ticket = p.publisher.Publish({L"C:/owned/selection", L"wanted.txt", 60}); Window wrong;
        Check(!ticket.error && p.observer.MatchAndRetain(wrong.value, L"C:/owned/selection", {L"wrong.txt"}, true).state == nb::State::Wait,
            "ready wrong selection initially waits but is not a precise retain");
        Sleep(210);
        Check(p.observer.IsRetained(wrong.value).state == nb::State::None &&
              p.observer.MatchAndRetain(wrong.value, L"C:/owned/selection", {L"wrong.txt"}, true).state == nb::State::None,
            "known wrong ready selection at bound excludes candidate rather than claiming a successful match");
    }
    {
        const auto name = prefix + L"-candidate-stop"; Pair p(name);
        auto ticket = p.publisher.Publish({L"C:/owned/restart", {}, 1000}); Window window;
        Check(!ticket.error && p.observer.ShouldWait(window.value, {}, false).state == nb::State::Wait,
            "graceful replacement fixture holds provisional candidate");
        p.observer.Stop(); nb::Observer replacement;
        Check(replacement.StartForTest(name) == 0 && replacement.IsRetained(window.value).state == nb::State::Retained,
            "normal Stop persists exact candidate through restart while publisher holds v2");
    }
    {
        const auto name = prefix + L"-candidate-lost-sidecar"; Pair p(name);
        auto ticket = p.publisher.Publish({L"C:/owned/lost-sidecar", {}, 1000}); Window window;
        Check(!ticket.error && p.observer.ShouldWait(window.value, {}, false).state == nb::State::Wait,
            "lost-sidecar fixture has provisional claimed marker in original v2 layout");
        auto* channel = p.observer.ChannelForTest();
        UnmapViewOfFile(channel->candidates); channel->candidates = nullptr;
        CloseHandle(channel->candidates_mapping); channel->candidates_mapping = nullptr;
        p.observer.Stop(); nb::Observer replacement;
        Check(replacement.StartForTest(name) == ERROR_INVALID_DATA && replacement.IsRetained(window.value).state == nb::State::Unavailable,
            "lost sidecar never reinterprets pin as fresh admission or silently enables closing");
    }
    {
        Pair p(prefix + L"-retained-capacity"); p.observer.RetainedCapacityForTest(2);
        const auto published = p.publisher.Publish({L"C:\\owned\\retained", {}, 10000});
        const auto second = p.publisher.Publish({L"C:/owned/retained", {}, 10000});
        const auto third = p.publisher.Publish({L"C:/owned/retained", {}, 10000});
        Check(!second.error && !third.error, "retained capacity fixture reserves one complete ticket per window");
        Window a, b, c;
        Check(!published.error && p.observer.MatchAndRetain(a.value, L"C:\\owned\\retained", {}, true).state == nb::State::Retained &&
              p.observer.MatchAndRetain(b.value, L"C:\\owned\\retained", {}, true).state == nb::State::Retained,
            "retained capacity fixture fills without losing earlier identities");
        nb::detail::deny_identity_for_test = a.value;
        const auto full = p.observer.MatchAndRetain(c.value, L"C:\\owned\\retained", {}, true);
        nb::detail::deny_identity_for_test = nullptr;
        Check(full.state == nb::State::Unavailable && full.error == ERROR_NOT_ENOUGH_MEMORY &&
              p.observer.IsRetained(a.value).state == nb::State::Retained, "retained capacity error is explicit; identity-denied live HWND is not evicted");
        p.observer.ForgetWindow(a.value);
        Check(p.observer.MatchAndRetain(c.value, L"C:\\owned\\retained", {}, true).state == nb::State::Retained,
            "destroy notification releases retained capacity");
    }

    {
        const auto name = prefix + L"-stop-full-retained"; Pair p(name);
        p.observer.RetainedCapacityForTest(1);
        const auto bound = p.publisher.Publish({L"C:/owned/stop-full", {}, 1000}); Window retained;
        Check(!bound.error && p.observer.MatchAndRetain(retained.value, L"C:/owned/stop-full", {}, true).state == nb::State::Retained,
            "Stop cost fixture fills retained capacity with one live HWND");
        const auto first = p.publisher.Publish({L"C:/owned/stop-pending-a", {}, 1000});
        const auto second = p.publisher.Publish({L"C:/owned/stop-pending-b", {}, 1000}); Window a, b;
        Check(!first.error && !second.error && p.observer.ShouldWait(a.value, {}, false).state == nb::State::Wait &&
              p.observer.ShouldWait(b.value, {}, false).state == nb::State::Wait,
            "Stop cost fixture has four pending candidate records over a full retained table");
        const auto before = nb::detail::identity_reads_for_test;
        p.observer.Stop();
        const auto reads = nb::detail::identity_reads_for_test - before;
        std::printf("[STOP_COST] identity_reads=%zu retained_capacity=1 pending_candidates=4\n", reads);
        Check(reads == 1, "Stop stops retrying the same live full retained table after its first capacity failure");
        nb::Observer replacement;
        Check(replacement.StartForTest(name) == ERROR_INVALID_DATA && replacement.ShouldWait(a.value, {}, false).state == nb::State::Unavailable,
            "Stop capacity failure leaves unresolved claimed candidates observable rather than pretending persistence succeeded");
    }
    {
        Pair p(prefix + L"-mutex"); Window window;
        HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr), release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread holder([&] { WaitForSingleObject(p.observer.ChannelForTest()->mutex, INFINITE); SetEvent(entered);
            WaitForSingleObject(release, INFINITE); ReleaseMutex(p.observer.ChannelForTest()->mutex); });
        WaitForSingleObject(entered, INFINITE);
        const auto publication = p.publisher.Publish({L"C:\\owned\\locked", {}, 1000});
        const auto query = p.observer.ShouldWait(window.value, {}, false);
        Check(publication.error == ERROR_TIMEOUT && query.state == nb::State::Unavailable && query.error == ERROR_TIMEOUT,
            "bounded lock contention returns observable failure rather than unprotected None");
        SetEvent(release); holder.join(); CloseHandle(entered); CloseHandle(release);
        Check(p.publisher.Publish({L"C:\\owned\\unlocked", {}, 1000}).error == 0, "ordinary lock timeout does not poison a healthy ledger");
        Check(p.observer.ShouldWait(window.value, {}, false).state == nb::State::Unavailable, "lock timeout emergency remains observable after lock release");
        Sleep(1050);
        Check(p.observer.ShouldWait(window.value, {}, false).state != nb::State::Unavailable, "lock timeout emergency expires without changing takeover settings");
    }
    {
        Pair p(prefix + L"-malformed"); Window window;
        auto published = p.publisher.Publish({L"C:\\owned\\protocol", {}, 1000});
        auto* channel = p.observer.ChannelForTest();
        { nb::detail::Lock lock(*channel); ++channel->data->version; }
        Check(p.observer.ShouldWait(window.value, {}, false).state == nb::State::Unavailable &&
              p.publisher.CommitLaunch(published.ticket) == ERROR_INVALID_DATA, "unknown shared protocol version is rejected");
        { nb::detail::Lock lock(*channel); --channel->data->version; ++channel->data->bytes; }
        Check(p.publisher.Publish({L"C:\\owned\\bad-size", {}, 1000}).error == ERROR_INVALID_DATA, "malformed fixed layout size rejected");
        { nb::detail::Lock lock(*channel); --channel->data->bytes; channel->data->slots[published.ticket.slot].folder[2] = 0; }
        Check(p.observer.ShouldWait(window.value, {}, false).error == ERROR_INVALID_DATA, "embedded protocol NUL cannot shorten the intended target");
        { nb::detail::Lock lock(*channel); channel->data->slots[published.ticket.slot].folder[2] = L'\\';
          channel->data->slots[published.ticket.slot].state = 99; }
        Check(p.observer.ShouldWait(window.value, {}, false).error == ERROR_INVALID_DATA, "unknown slot state rejected");
        { nb::detail::Lock lock(*channel); auto& slot = channel->data->slots[published.ticket.slot]; slot.state = 1; slot.expires = slot.started + nb::detail::kMaxTtlMs + 1; }
        Check(p.observer.ShouldWait(window.value, {}, false).error == ERROR_INVALID_DATA, "overlong shared request TTL rejected");
        { nb::detail::Lock lock(*channel); auto& slot = channel->data->slots[published.ticket.slot]; slot.expires = slot.started + 1000; slot.folder_length = nb::detail::kPath; }
        Check(p.observer.ShouldWait(window.value, {}, false).error == ERROR_INVALID_DATA, "out-of-range shared path length rejected before indexing");
        { nb::detail::Lock lock(*channel); channel->data->slots[published.ticket.slot].state = 0;
          channel->data->overload_started = GetTickCount64(); channel->data->overload_until = GetTickCount64() + nb::detail::kMaxTtlMs + 5000; channel->data->overload_error = ERROR_BUSY; }
        Check(p.observer.ShouldWait(window.value, {}, false).error == ERROR_INVALID_DATA, "malformed overload deadline cannot claim an unbounded preservation period");
    }
    {
        const auto name = prefix + L"-abandoned"; Pair p(name); Window window;
        Check(Child(child_executable, L"--abandon", name), "real writer exits while holding shared mutex");
        Check(p.observer.ShouldWait(window.value, {}, false).state == nb::State::Unavailable &&
              p.publisher.Publish({L"C:\\owned\\abandoned", {}, 1000}).error == ERROR_INVALID_DATA,
            "abandoned writer poisons potentially torn protocol instead of trusting partial state");
    }
    {
        const auto name = prefix + L"-release";
        DWORD before = 0, after = 0; GetProcessHandleCount(GetCurrentProcess(), &before);
        for (unsigned i = 0; i < 20; ++i) { Pair p(name + std::to_wstring(i)); }
        GetProcessHandleCount(GetCurrentProcess(), &after);
        Check(after == before, "observer/publisher teardown releases every mapping and mutex handle");
        nb::Publisher gone; Check(gone.OpenForTest(name + L"19") != 0, "named ledger disappears after last owner handle closes");
    }
    std::printf("SUMMARY passed=%u failed=%u sizeof_layout=%zu pointer_bits=%zu\n", passed, failed, sizeof(nb::detail::Layout), sizeof(void*) * 8);
    return failed ? 1 : 0;
}

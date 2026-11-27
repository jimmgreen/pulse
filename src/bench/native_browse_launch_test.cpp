#include "../common/native_browse_launch.h"
#include <cstdio>
#include <thread>
#include <vector>

namespace nb = pulse::native_browse;
namespace intent = pulse::native_browse_intent;
using pulse::browse::Intent;
using pulse::browse::Request;

namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
}
struct Window {
    HWND value = nullptr;
    bool Create() {
        value = CreateWindowExW(0, L"STATIC", L"Launch private desktop fixture", WS_POPUP,
            0, 0, 20, 20, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        return value != nullptr;
    }
    ~Window() { if (value) DestroyWindow(value); }
};
bool Start(intent::Observer& observer) {
    const DWORD error = observer.Start();
    Check(!error, "production Observer::Start connects on isolated desktop");
    if (error) std::printf("[DETAIL] Observer error=%lu\n", error);
    return !error;
}
DWORD Prepare(nb::Launch& launch, const Request& request) {
    const DWORD error = launch.Prepare(request);
    const DWORD saved = GetLastError();
    if (error) std::printf("[RESERVATION] error=%lu depth=%u\n", error, nb::depth);
    SetLastError(saved);
    return error;
}

void Run() {
    const Request folder{Intent::OpenFolder, L"C:\\native-launch-fixture\\folder"};
    Check(!nb::Active() && nb::depth == 0, "initial recursion depth is inactive");
    {
        unsigned calls = 0;
        DWORD prepare_error = 0, returned_error = 0;
        BOOL result = TRUE;
        SetLastError(0x12345);
        {
            nb::Launch launch;
            Check(GetLastError() == 0x12345 && nb::Active(), "Launch constructor preserves LastError and enables guard");
            prepare_error = Prepare(launch, folder);
            Check(prepare_error != ERROR_SUCCESS && GetLastError() == 0x12345,
                "absent observer reservation explicitly fails without changing LastError");
            ++calls; result = FALSE; SetLastError(ERROR_ACCESS_DENIED);
            launch.Complete(result != FALSE);
            returned_error = GetLastError();
        }
        Check(calls == 1 && !result && returned_error == ERROR_ACCESS_DENIED && GetLastError() == ERROR_ACCESS_DENIED,
            "absent observer still executes native once and returns its actual failure");
        Check(!nb::Active() && nb::depth == 0, "failed unprotected launch unwinds recursion guard");
    }
    {
        intent::Observer observer;
        if (!Start(observer)) return;
        Window before, native;
        Check(before.Create(), "preexisting hidden STATIC created on private desktop");
        Check(observer.MatchAndRetain(before.value, folder.path, {}, true).state == intent::State::None,
            "no launch intent matches before Prepare");
        unsigned calls = 0;
        DWORD after_complete = 0;
        {
            nb::Launch launch;
            SetLastError(0x31415);
            const auto reserved = Prepare(launch, folder);
            Check(!reserved && GetLastError() == 0x31415, "Prepare publishes before native and preserves caller error");
            Check(observer.MatchAndRetain(before.value, folder.path, {}, true).state == intent::State::None,
                "Prepare snapshot never retains a preexisting HWND");
            ++calls;
            const BOOL result = native.Create();
            SetLastError(0x27182);
            launch.Complete(result != FALSE);
            after_complete = GetLastError();
        }
        const DWORD after_destructor = GetLastError();
        Check(calls == 1 && native.value && !IsWindowVisible(native.value), "original native callback runs exactly once and creates only a hidden STATIC");
        Check(after_complete == 0x27182 && after_destructor == 0x27182,
            "successful Complete and Launch destructor preserve native LastError");
        Check(observer.MatchAndRetain(native.value, folder.path, {}, true).state == intent::State::Retained &&
            observer.IsRetained(native.value).state == intent::State::Retained,
            "successful ticket survives guard and publisher destruction until asynchronous observer matching");
    }
    {
        intent::Observer observer;
        if (!Start(observer)) return;
        unsigned calls = 0;
        unsigned enumerated_windows = 0;
        SetLastError(ERROR_SUCCESS);
        const BOOL enumerated = EnumDesktopWindows(GetThreadDesktop(GetCurrentThreadId()), [](HWND, LPARAM context) -> BOOL {
            ++*reinterpret_cast<unsigned*>(context); return TRUE;
        }, reinterpret_cast<LPARAM>(&enumerated_windows));
        std::printf("[EMPTY_DESKTOP] result=%d error=%lu windows=%u\n", enumerated, GetLastError(), enumerated_windows);
        {
            nb::Launch launch;
            Check(!Prepare(launch, folder), "failure case reserves a ticket");
            ++calls; SetLastError(ERROR_SHARING_VIOLATION);
            launch.Complete(false);
            Check(GetLastError() == ERROR_SHARING_VIOLATION, "failed Complete preserves native failure code");
        }
        Check(GetLastError() == ERROR_SHARING_VIOLATION && calls == 1, "failure destructor does not replace native LastError");
        Window later; Check(later.Create(), "post-failure hidden window created");
        Check(observer.MatchAndRetain(later.value, folder.path, {}, true).state == intent::State::None,
            "Complete(false) withdraws ticket before a later same-folder window");
    }
    {
        intent::Observer observer;
        if (!Start(observer)) return;
        bool caught = false;
        try {
            nb::Launch launch;
            Check(!Prepare(launch, folder), "exception case reserves a ticket");
            SetLastError(ERROR_CANCELLED);
            throw 7;
        } catch (int) { caught = true; }
        Check(caught && GetLastError() == ERROR_CANCELLED && !nb::Active() && nb::depth == 0,
            "exception destructor preserves error and unwinds guard");
        Window later; Check(later.Create(), "post-exception hidden window created");
        Check(observer.MatchAndRetain(later.value, folder.path, {}, true).state == intent::State::None,
            "exception without Complete withdraws pending ticket");
    }
    {
        intent::Observer observer;
        if (!Start(observer)) return;
        intent::Publisher filler;
        Check(!filler.Open(), "capacity probe uses production publisher naming");
        unsigned calls = 0;
        {
            nb::Launch outer;
            Check(!Prepare(outer, folder) && nb::depth == 1, "outer Launch reserves one ticket at depth one");
            ++calls;
            {
                nb::Launch nested;
                SetLastError(0x42424);
                Check(!Prepare(nested, folder) && nb::Active() && nb::depth == 2 && GetLastError() == 0x42424,
                    "nested Launch bypasses publication while retaining recursion protection");
                nested.Complete(true);
            }
            Check(nb::Active() && nb::depth == 1 && GetLastError() == 0x42424,
                "nested completion restores outer depth and native LastError");
            unsigned added = 0;
            std::vector<intent::Ticket> tickets;
            for (unsigned i = 0; i < 63; ++i) {
                const auto publication = filler.Publish({L"C:\\native-launch-fixture\\capacity", {}, 30000});
                if (!publication.error) { ++added; tickets.push_back(publication.ticket); }
            }
            // Filling the remaining 63 slots proves that the inner Launch
            // reserved none. Do not intentionally trigger overload here; its
            // desktop-wide fail-closed behavior has a separate test below.
            std::printf("[CAPACITY] remaining_slots_filled=%u\n", added);
            Check(added == 63,
                "nested Launch did not create a second ticket in the 64-slot ledger");
            for (const auto& ticket : tickets) filler.Cancel(ticket);
            outer.Complete(true);
        }
        Check(calls == 1 && !nb::Active() && nb::depth == 0,
            "outer native invocation stays single and final guard depth returns to zero");
        Window later; Check(later.Create(), "nested launch asynchronous window created");
        Check(observer.MatchAndRetain(later.value, folder.path, {}, true).state == intent::State::Retained,
            "outer committed ticket survives nested completion and destruction");
    }
    {
        intent::Observer observer;
        if (!Start(observer)) return;
        intent::Publisher filler;
        Check(!filler.Open(), "full-ledger publisher opens production channel");
        unsigned occupied = 0;
        for (unsigned i = 0; i < 64; ++i)
            occupied += filler.Publish({L"C:\\native-launch-fixture\\occupied", {}, 30000}).error == ERROR_SUCCESS;
        Check(occupied == 64, "all 64 real ledger slots are occupied");
        unsigned calls = 0;
        BOOL result = TRUE;
        DWORD returned_error = 0;
        {
            nb::Launch launch;
            SetLastError(0x51515);
            Check(Prepare(launch, folder) == ERROR_BUSY && GetLastError() == 0x51515,
                "full ledger reports reservation failure without contaminating LastError");
            ++calls; result = FALSE; SetLastError(ERROR_BAD_PATHNAME);
            launch.Complete(result != FALSE);
            returned_error = GetLastError();
        }
        Check(calls == 1 && !result && returned_error == ERROR_BAD_PATHNAME && GetLastError() == ERROR_BAD_PATHNAME,
            "full ledger never suppresses native call or replaces its actual failure with success");
    }
    for (const auto& request : std::vector<Request>{{Intent::Reveal, L"C:\\项目\\资料\\选择文件.txt"},
            {Intent::Reveal, L"C:\\根文件.txt"}, {Intent::OpenFolder, L"C:\\"}}) {
        intent::Observer observer;
        if (!Start(observer)) return;
        std::wstring directory = request.path, selected;
        if (request.intent == Intent::Reveal) {
            const auto slash = directory.find_last_of(L'\\');
            selected = directory.substr(slash + 1);
            directory.resize(slash == 2 ? 3 : slash);
        }
        {
            nb::Launch launch;
            Check(!Prepare(launch, request), "root or Unicode request reserves successfully");
            launch.Complete(true);
        }
        Window later; Check(later.Create(), "root or Unicode hidden window created");
        if (!selected.empty())
            Check(observer.MatchAndRetain(later.value, directory, {L"wrong.txt"}, true).state != intent::State::Retained,
                "file-select decomposition rejects a different selected item");
        const std::vector<std::wstring> names = selected.empty() ? std::vector<std::wstring>{} : std::vector<std::wstring>{selected};
        Check(observer.MatchAndRetain(later.value, directory, names, true).state == intent::State::Retained,
            "root and Unicode folder/selection decomposition matches actual observer");
    }
    {
        intent::Observer observer;
        if (!Start(observer)) return;
        // Warm CRT/security initialization before measuring handles; retain all
        // iterations and report exact counts rather than selecting a low sample.
        { nb::Launch warmup; warmup.Prepare(folder); warmup.Complete(false); }
        DWORD before = 0, after = 0;
        const BOOL measured_before = GetProcessHandleCount(GetCurrentProcess(), &before);
        unsigned successful = 0;
        for (unsigned i = 0; i < 128; ++i) {
            nb::Launch launch;
            successful += launch.Prepare(folder) == ERROR_SUCCESS;
            launch.Complete(false);
        }
        const BOOL measured_after = GetProcessHandleCount(GetCurrentProcess(), &after);
        std::printf("[HANDLES] iterations=128 prepared=%u before=%lu after=%lu\n", successful, before, after);
        Check(measured_before && measured_after && successful == 128 && before == after,
            "repeated Launch Prepare/Complete/destruction has no handle growth");
    }
}
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const auto name = L"Pulse.NativeLaunchTest." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    std::puts("[SETUP] creating an isolated desktop; input desktop is not switched");
    const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
        DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
    Check(desktop != nullptr, "private desktop created without switching input desktop");
    if (!desktop) { std::printf("[DETAIL] CreateDesktop error=%lu\n", GetLastError()); return 1; }
    std::thread worker([&] {
        const HDESK previous = GetThreadDesktop(GetCurrentThreadId());
        const BOOL attached = SetThreadDesktop(desktop);
        Check(attached != FALSE, "dedicated test thread attaches private desktop");
        if (!attached) { std::printf("[DETAIL] SetThreadDesktop error=%lu\n", GetLastError()); return; }
        Check(intent::detail::ObjectName(GetThreadDesktop(GetCurrentThreadId())) == name,
            "production ledger identity is scoped to the private desktop");
        Run();
        Check(SetThreadDesktop(previous) != FALSE, "test thread restores desktop after all fixtures are destroyed");
    });
    worker.join();
    Check(CloseDesktop(desktop) != FALSE, "private desktop closed after test thread exits");
    std::printf("RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}

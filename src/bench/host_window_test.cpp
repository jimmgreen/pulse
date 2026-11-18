// host_window_test.cpp - Multi-window M2: the hidden host window owns the
// single-instance endpoint. A second process forwards an open request through
// the real SingleInstanceCoordinator to the host; the host stays hidden, out of
// the taskbar, survives WM_CLOSE, and (unlike a message-only window) receives
// broadcasts such as TaskbarCreated. The routing table forwards only
// process-level notifications, and forwarding keeps the posting order.
#include "../app/host_routing.h"
#include "../app/host_window.h"
#include "../app/single_instance_coordinator.h"

#include <cstdio>
#include <string>
#include <iterator>
#include <thread>
#include <vector>

namespace {

using Coordinator = pulse::app::SingleInstanceCoordinator;
using pulse::app::HostWindow;

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

void Pump(DWORD ms) {
    const auto deadline = GetTickCount64() + ms;
    do {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(2);
    } while (GetTickCount64() < deadline);
}

HANDLE Spawn(const std::wstring& arguments) {
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    std::wstring command = L"\"" + std::wstring(exe) + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &process))
        return nullptr;
    CloseHandle(process.hThread);
    return process.hProcess;
}

// Pumps the host while the child forwards; true when the child exits with 0.
bool WaitChild(HANDLE process) {
    if (!process) return false;
    const auto deadline = GetTickCount64() + 10000;
    while (WaitForSingleObject(process, 0) == WAIT_TIMEOUT && GetTickCount64() < deadline) Pump(5);
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    if (code == STILL_ACTIVE) {
        TerminateProcess(process, 2);
        WaitForSingleObject(process, 2000);
    }
    CloseHandle(process);
    return code == 0;
}

UINT g_broadcast = 0;
int g_message_only_broadcasts = 0;
std::vector<UINT> g_window_order;
LRESULT CALLBACK OrderWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message >= WM_APP && message < 0xC000) {
        g_window_order.push_back(message);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
LRESULT CALLBACK MessageOnlyProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == g_broadcast) ++g_message_only_broadcasts;
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--sender") {
        Coordinator sender;
        if (sender.Acquire(argv[2]) != Coordinator::AcquireResult::Existing) return 2;
        return sender.ForwardOpenPath(argv[3], 5000) ? 0 : 3;
    }

    const std::wstring endpoint = L"PulseHostWindowTest-" + std::to_wstring(GetCurrentProcessId());
    Coordinator primary;
    Check(primary.Acquire(endpoint) == Coordinator::AcquireResult::Primary, "test process owns a private endpoint");

    int opens = 0, consumed = 0, broadcasts = 0;
    std::wstring opened;
    g_broadcast = RegisterWindowMessageW(L"PulseHostWindowTest.Broadcast");
    HostWindow host;
    const bool created = host.Create(GetModuleHandleW(nullptr),
        [&](HWND hwnd, UINT message, WPARAM, LPARAM lparam) -> std::optional<LRESULT> {
            if (message == g_broadcast) ++broadcasts;
            if (message == WM_APP + 7) { ++consumed; return 42; }
            if (message != WM_COPYDATA) return std::nullopt;
            if (!primary.OwnsEndpoint(hwnd)) return FALSE;
            Coordinator::OpenRequest request;
            if (!Coordinator::DecodeOpenRequest(reinterpret_cast<COPYDATASTRUCT*>(lparam), request)) return FALSE;
            const auto accepted = primary.AcceptOpenRequest(request, GetTickCount64());
            if (accepted == Coordinator::OpenAcceptance::Invalid) return FALSE;
            if (accepted == Coordinator::OpenAcceptance::New) { ++opens; opened = request.path; }
            return TRUE;
        });
    Check(created && host.Hwnd() != nullptr, "host window created");
    const HWND hwnd = host.Hwnd();
    Check(!host.Create(GetModuleHandleW(nullptr), {}), "a second Create on the same host is refused");

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    Check(!IsWindowVisible(hwnd) && (style & WS_VISIBLE) == 0, "host is never visible");
    Check((ex_style & WS_EX_TOOLWINDOW) != 0 && (ex_style & WS_EX_APPWINDOW) == 0,
          "host stays out of the taskbar and Alt+Tab");
    Check(GetAncestor(hwnd, GA_PARENT) == GetDesktopWindow(), "host is a top-level window, not HWND_MESSAGE");

    Check(SendMessageW(hwnd, WM_APP + 7, 0, 0) == 42 && consumed == 1, "handler result is returned");
    Check(SendMessageW(hwnd, WM_APP + 8, 0, 0) == 0, "unhandled messages fall through to DefWindowProc");
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    Check(IsWindow(hwnd) != FALSE, "WM_CLOSE does not destroy the host");

    // Broadcast reach: the host gets it, a message-only window does not.
    WNDCLASSW cls{};
    cls.lpfnWndProc = MessageOnlyProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"PulseHostWindowTestMessageOnly";
    RegisterClassW(&cls);
    const HWND message_only = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                            nullptr, cls.hInstance, nullptr);
    PostMessageW(HWND_BROADCAST, g_broadcast, 0, 0);
    Pump(300);
    Check(broadcasts == 1, "host receives HWND_BROADCAST messages (TaskbarCreated)");
    Check(message_only && g_message_only_broadcasts == 0,
          "control: a message-only window misses the same broadcast");
    if (message_only) DestroyWindow(message_only);

    Check(primary.PublishEndpoint(hwnd), "endpoint published on the host");
    Check(Coordinator::FindPrimaryWindow(endpoint) == hwnd, "second launches resolve the host window");
    Check(primary.OwnsEndpoint(hwnd), "host owns the endpoint");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    std::wstring folder = temp;
    while (folder.size() > 3 && folder.back() == L'\\') folder.pop_back();  // "x\" would escape the quote
    Check(WaitChild(Spawn(L"--sender " + endpoint + L" \"" + folder + L"\"")),
          "second process forwards an open request and gets acceptance");
    std::wstring expected;
    Coordinator::NormalizeLaunchPath(folder, expected);
    Check(opens == 1 && opened == expected, "host handler received the forwarded path once");

    host.Destroy();
    Check(host.Hwnd() == nullptr && !IsWindow(hwnd), "Destroy removes the host window");
    Check(Coordinator::FindPrimaryWindow(endpoint) == nullptr, "a destroyed host no longer resolves as the endpoint");
    Check(!WaitChild(Spawn(L"--sender " + endpoint + L" \"" + folder + L"\"")) && opens == 1,
          "forwarding fails after the host is gone (no stray delivery)");

    // Routing table (host_routing.h).
    using namespace pulse;
    bool unique = true;
    for (size_t i = 0; i < std::size(app::kHostRoutedMessages); ++i)
        for (size_t j = i + 1; j < std::size(app::kHostRoutedMessages); ++j)
            unique = unique && app::kHostRoutedMessages[i] != app::kHostRoutedMessages[j];
    Check(unique, "routing table has no duplicate ids");
    bool services = true;
    for (UINT m : {WM_WORKER_RESULT, WM_OPS_NOTIFY, WM_INDEX_NOTIFY, WM_INDEX_SEARCH, WM_NETWORK_INDEX_NOTIFY,
                   WM_NETWORK_INDEX_SEARCH, WM_CONTENT_SEARCH, WM_DUPLICATE_SCAN, WM_ASSOC_CHANGED,
                   WM_SHELL_CACHE_INVALIDATE, WM_SHELL_VERB_SEED, WM_CTX_LAYOUT_SEED, WM_SHELL_VERBS,
                   WM_RECYCLE_INFO, WM_NETWORK_LOCATIONS, WM_UPDATE_RESULT, WM_UPDATE_DOWNLOADED,
                   WM_CHANGE_TRACKING, WM_TAG_ADS_WARNING, WM_TAG_ADS_DISCOVERED, WM_SETTINGS_TASK_RESULT,
                   app::TrayController::kCallbackMessage, UINT(WM_HOTKEY)})
        services = services && app::IsHostRoutedMessage(m);
    Check(services, "service, tray and hotkey notifications retargeted in M2b are routed");
    Check(app::IsHostRoutedMessage(WM_NET_PROBE) && app::IsHostRoutedMessage(WM_BITLOCKER_UNLOCK) &&
              app::IsHostRoutedMessage(WM_EXPLORER_TAKEOVER) && app::IsHostRoutedMessage(WM_NETWORK_CONNECT),
          "shared navigation results (M3e) are routed");
    Check(app::IsHostRoutedMessage(WM_SHELLCTX_ITEMS), "shell context menu rows reach the active window (M4a)");
    Check(app::IsHostRoutedMessage(WM_NETWORK_LIVE_SEARCH),
          "a tab's live network search is routed: the tab may move to another window (M6f)");
    Check(app::IsHostRoutedMessage(WM_RECYCLE_SEARCH), "recycle search results reach a Pulse window");
    Check(!app::IsHostRoutedMessage(WM_DUP_VOLUMES), "per-dialog results stay on their window");
    bool window_local = true;
    for (UINT m : {WM_FRAME_PUMP, WM_QUICK_PREVIEW_NAVIGATE, WM_QUICK_PREVIEW_OPEN, WM_QUICK_PREVIEW_COMMAND,
                   WM_DETAILS_META, WM_SHOW_RELEASE_NOTES, WM_CONTENT_SELECTION,
                   WM_UPDATE_INSTALL, WM_SEARCH_HISTORY, WM_SHELL_SELECT, WM_NEW_WINDOW,
                   WM_REOPEN_CLOSED_WINDOW, WM_OPEN_SESSION_WINDOWS, WM_MOVE_TAB_TO_NEW_WINDOW,
                   WM_CONVERT_EXTERNAL_OPEN, WM_ADOPT_OPEN_POLICY, WM_REOPEN_CLOSED_TABS})
        window_local = window_local && !app::IsHostRoutedMessage(m);
    Check(window_local, "window-level messages are not routed through the host");
    bool broadcasts_local = true;
    for (UINT m : {UINT(WM_SETTINGCHANGE), UINT(WM_DEVICECHANGE), UINT(WM_ENDSESSION), UINT(WM_QUERYENDSESSION),
                   UINT(WM_DISPLAYCHANGE), UINT(WM_COPYDATA), UINT(WM_CLOSE)})
        broadcasts_local = broadcasts_local && !app::IsHostRoutedMessage(m);
    Check(broadcasts_local, "system broadcasts are not forwarded (no double handling)");

    // M3c: the host handles these broadcasts once for every window.
    using app::SystemBroadcast;
    using app::ClassifySystemBroadcast;
    const auto settings = [](const wchar_t* area) {
        return ClassifySystemBroadcast(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(area));
    };
    Check(settings(nullptr) == SystemBroadcast::Theme && settings(L"ImmersiveColorSet") == SystemBroadcast::Theme &&
              settings(L"HighContrast") == SystemBroadcast::Theme,
          "theme setting changes are host broadcasts");
    Check(ClassifySystemBroadcast(WM_THEMECHANGED, 0, 0) == SystemBroadcast::Theme &&
              ClassifySystemBroadcast(WM_SETTINGCHANGE, SPI_SETHIGHCONTRAST,
                  reinterpret_cast<LPARAM>(L"Accessibility")) == SystemBroadcast::Theme,
          "theme and explicit high contrast broadcasts refresh the snapshot");
    Check(settings(L"Environment") == SystemBroadcast::None && settings(L"intl") == SystemBroadcast::None,
          "other setting changes are ignored");
    DEV_BROADCAST_HDR volume{};
    volume.dbch_size = sizeof(volume);
    volume.dbch_devicetype = DBT_DEVTYP_VOLUME;
    DEV_BROADCAST_HDR device_interface = volume;
    device_interface.dbch_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    const auto device = [](WPARAM event, const DEV_BROADCAST_HDR* header) {
        return ClassifySystemBroadcast(WM_DEVICECHANGE, event, reinterpret_cast<LPARAM>(header));
    };
    Check(device(DBT_DEVICEARRIVAL, &volume) == SystemBroadcast::Volumes &&
              device(DBT_DEVICEREMOVECOMPLETE, &volume) == SystemBroadcast::Volumes &&
              device(DBT_DEVICEARRIVAL, nullptr) == SystemBroadcast::Volumes,
          "volume arrival and removal rebuild the drive list");
    DEV_BROADCAST_DEVICEINTERFACE_W phone{};
    phone.dbcc_size = sizeof(phone);
    phone.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    phone.dbcc_classguid = app::kPortableDeviceInterfaceClass;
    DEV_BROADCAST_DEVICEINTERFACE_W other_interface = phone;
    other_interface.dbcc_classguid = GUID{0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};
    const auto* phone_header = reinterpret_cast<const DEV_BROADCAST_HDR*>(&phone);
    Check(device(DBT_DEVICEARRIVAL, phone_header) == SystemBroadcast::Devices &&
              device(DBT_DEVICEREMOVECOMPLETE, phone_header) == SystemBroadcast::Devices,
          "portable device (phone) arrival and removal refresh the device list");
    Check(device(DBT_DEVICEARRIVAL, &device_interface) == SystemBroadcast::None &&
              device(DBT_DEVICEARRIVAL, reinterpret_cast<const DEV_BROADCAST_HDR*>(&other_interface)) ==
                  SystemBroadcast::None &&
              device(DBT_DEVICEQUERYREMOVE, phone_header) == SystemBroadcast::None &&
              device(DBT_DEVICEQUERYREMOVE, &volume) == SystemBroadcast::None &&
              device(DBT_DEVNODES_CHANGED, nullptr) == SystemBroadcast::None,
          "other device events are ignored");
    Check(ClassifySystemBroadcast(WM_QUERYENDSESSION, 0, 0) == SystemBroadcast::QueryEndSession &&
              ClassifySystemBroadcast(WM_ENDSESSION, TRUE, 0) == SystemBroadcast::EndSession,
          "session end is a host broadcast (saved once)");
    bool disjoint = true;
    for (const UINT routed : app::kHostRoutedMessages)
        disjoint = disjoint && ClassifySystemBroadcast(routed, 0, 0) == SystemBroadcast::None;
    disjoint = disjoint && ClassifySystemBroadcast(WM_DISPLAYCHANGE, 0, 0) == SystemBroadcast::None &&
               ClassifySystemBroadcast(WM_COPYDATA, 0, 0) == SystemBroadcast::None;
    Check(disjoint, "forwarded notifications and host broadcasts do not overlap");

    // Forwarding keeps order: one thread queue serves both windows, so posts
    // from a worker reach the window in posting order whichever window they
    // were addressed to.
    WNDCLASSW order_cls{};
    order_cls.lpfnWndProc = OrderWindowProc;
    order_cls.hInstance = GetModuleHandleW(nullptr);
    order_cls.lpszClassName = L"PulseHostWindowTestOrder";
    RegisterClassW(&order_cls);
    const HWND window = CreateWindowW(order_cls.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr,
                                      nullptr, order_cls.hInstance, nullptr);
    HostWindow router;
    router.Create(GetModuleHandleW(nullptr),
        [window](HWND, UINT message, WPARAM wparam, LPARAM lparam) -> std::optional<LRESULT> {
            if (!app::IsHostRoutedMessage(message)) return std::nullopt;
            return SendMessageW(window, message, wparam, lparam);
        });
    const HWND router_hwnd = router.Hwnd();
    std::thread([router_hwnd, window] {
        PostMessageW(router_hwnd, WM_INDEX_NOTIFY, 0, 0);
        PostMessageW(router_hwnd, WM_RECYCLE_SEARCH, 0, 0);
        PostMessageW(window, WM_FRAME_PUMP, 0, 0);
        PostMessageW(router_hwnd, WM_WORKER_RESULT, 0, 0);
        PostMessageW(window, WM_DETAILS_META, 0, 0);
        PostMessageW(router_hwnd, WM_OPS_NOTIFY, 0, 0);
        PostMessageW(router_hwnd, WM_FRAME_PUMP, 0, 0);  // not routed: never reaches the window
    }).join();
    Pump(100);
    const std::vector<UINT> expected_order{WM_INDEX_NOTIFY, WM_RECYCLE_SEARCH, WM_FRAME_PUMP, WM_WORKER_RESULT, WM_DETAILS_META,
                                           WM_OPS_NOTIFY};
    Check(window && router_hwnd && g_window_order == expected_order,
          "routed and direct posts arrive in posting order; unrouted ids stay on the host");
    router.Destroy();
    if (window) DestroyWindow(window);

    std::printf("host_window_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

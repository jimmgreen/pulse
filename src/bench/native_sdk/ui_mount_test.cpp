#include "../../app/extensions/native/ui_adapter.h"
#include "../../common/windows_compat.h"
#include "ui_model_fixture.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <objbase.h>
#include <string>
#include <thread>

using namespace native_ui_fixture;
using pulse::extensions::native::UiAdapter;
static_assert(sizeof(PulseUiMountInfo) == 40);
namespace {
int failures = 0;
void Check(bool pass, const char *label) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
    failures += !pass;
}
void Pump() {
    MSG message{};
    unsigned count = 0;
    while (count++ < 1000 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message != WM_QUIT) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}
struct FakeHost {
    struct Compositor {
        int width = 1200, height = 800;
        int Width() const { return width; }
        int Height() const { return height; }
    } compositor;
    struct Renderer {
        bool sidebar = false;
        unsigned layout_changes = 0;
        D2D1_RECT_F ContentRect(float width, float height) const {
            return {20, 60, width - (sidebar ? 320.0f : 20.0f), height - 20};
        }
        D2D1_RECT_F ExtensionDockRect(float width, float height) const {
            return width >= 700 && height > 100 ? D2D1_RECT_F{width - 300, 60, width - 20, height - 20}
                                               : D2D1_RECT_F{};
        }
        void SetExtensionDockVisible(bool visible) { sidebar = visible; ++layout_changes; }
    } renderer;
    HWND hwnd = nullptr;
    bool darkMode = false;
    D2D1_COLOR_F accentColor = D2D1::ColorF(0x0078d4);
    float scale = 1;
    unsigned clamped = 0;
    FakeHost() {
        hwnd = CreateWindowExW(0, L"STATIC", L"Native mount fixture", WS_POPUP | WS_CLIPCHILDREN,
            0, 0, compositor.width, compositor.height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (hwnd) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    ~FakeHost() { if (IsWindow(hwnd)) DestroyWindow(hwnd); }
    void Resize(int width, int height) {
        compositor.width = width;
        compositor.height = height;
        MoveWindow(hwnd, 0, 0, width, height, FALSE);
    }
};
struct Fixture {
    PulseUiModelNode root = Node(PULSE_UI_MODEL_STACK, "root");
    std::array<PulseUiModelNode, 2> children{Node(PULSE_UI_MODEL_TEXT_INPUT, "input"),
        Node(PULSE_UI_MODEL_BUTTON, "run")};
    PulseUiState text{S("text"), S("initial")};
    PulseStringView command = S("run");
    PulseUiModelTree tree{};
    Fixture() {
        root.children = {1, children.data(), static_cast<uint32_t>(children.size())};
        children[0].text = T("Name");
        children[0].state = T("text");
        children[1].text = T("Run");
        children[1].command = T("run");
        tree.size = sizeof(tree);
        tree.root = &root;
        tree.states = &text;
        tree.state_count = 1;
        tree.commands = &command;
        tree.command_count = 1;
    }
};
struct Events {
    unsigned calls = 0, depth = 0, maximum_depth = 0;
    uint64_t panel = 0;
    PulseUiMountInfo last{};
    std::function<void(uint64_t)> action;
    static void PULSE_CALL Receive(void *context, uint64_t panel, const PulseUiMountInfo *info) {
        auto &self = *static_cast<Events *>(context);
        ++self.calls;
        ++self.depth;
        self.maximum_depth = (std::max)(self.maximum_depth, self.depth);
        self.panel = panel;
        self.last = *info;
        if (self.action) self.action(panel);
        --self.depth;
    }
};
struct Attachment {
    FakeHost &host;
    bool alive = true;
    UiAdapter adapter;
    PulseUiModelV1 ui{};
    PulseUiModelV1 model{};
    PulseUiMountV1 mount{};
    uint64_t panel = 0;
    explicit Attachment(FakeHost &value)
        : host(value), adapter(value.hwnd, value.darkMode, value.accentColor, value.scale, [this] { return alive; }) {
        Check(adapter.QueryModel(PULSE_UI_MODEL_VERSION, &ui, sizeof(ui)) == PULSE_OK &&
            adapter.QueryModel(PULSE_UI_MODEL_VERSION, &model, sizeof(model)) == PULSE_OK &&
            adapter.QueryMount(PULSE_UI_MOUNT_VERSION_1, &mount, sizeof(mount)) == PULSE_OK,
            "query real UI model and mount v1 tables");
        Sync();
    }
    void Sync() { adapter.SyncHost(host, [target = &host] { ++target->clamped; }); }
    bool Create() {
        Fixture fixture;
        return model.create(model.context, S("Native mount test"), &fixture.tree, &panel) == PULSE_OK && panel;
    }
    PulseStatus Set(uint32_t placement) { return mount.set(mount.context, panel, placement); }
    PulseUiMountInfo Info() {
        PulseUiMountInfo info{};
        info.size = sizeof(info);
        Check(mount.get(mount.context, panel, &info) == PULSE_OK, "get actual live panel mount information");
        return info;
    }
    uint64_t Subscribe(Events &events) {
        uint64_t token = 0;
        Check(mount.subscribe(mount.context, panel, Events::Receive, &events, &token) == PULSE_OK && token,
            "subscribe mount changes");
        return token;
    }
    std::string Text() {
        std::array<char, 128> buffer{};
        uint32_t needed = 0;
        if (ui.get_state(ui.context, panel, S("text"), buffer.data(), static_cast<uint32_t>(buffer.size()), &needed) !=
            PULSE_OK || needed > buffer.size()) return {};
        return std::string(buffer.data(), needed);
    }
};
bool ChildOf(HWND window, HWND owner) {
    return IsWindow(window) && GetParent(window) == owner && (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) != 0 &&
        (GetWindowLongPtrW(window, GWL_STYLE) & WS_POPUP) == 0;
}
bool OwnedBy(HWND window, HWND owner) {
    return IsWindow(window) && GetWindow(window, GW_OWNER) == owner &&
        (GetWindowLongPtrW(window, GWL_STYLE) & WS_POPUP) != 0 &&
        (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) == 0;
}
void AbiAndSubscriptionChecks(Attachment &a) {
    PulseUiMountV1 table{};
    Check(a.adapter.QueryMount(2, &table, sizeof(table)) == PULSE_ERROR_ABI &&
        a.adapter.QueryMount(1, nullptr, sizeof(table)) == PULSE_ERROR_ABI &&
        a.adapter.QueryMount(1, &table, sizeof(table) - 1) == PULSE_ERROR_ABI,
        "mount query rejects unknown version null table and wrong table size");
    PulseUiMountInfo invalid{};
    uint64_t invalid_token = 42;
    Check(a.mount.get(a.mount.context, a.panel, &invalid) == PULSE_ERROR_ARGUMENT &&
        a.mount.get(a.mount.context, a.panel, nullptr) == PULSE_ERROR_ARGUMENT &&
        a.Set(3) == PULSE_ERROR_ARGUMENT &&
        a.mount.subscribe(a.mount.context, a.panel, nullptr, nullptr, &invalid_token) == PULSE_ERROR_ARGUMENT &&
        invalid_token == 0 && a.mount.unsubscribe(a.mount.context, 0) == PULSE_ERROR_STATE,
        "mount validates info size placement callback and unknown token");
    std::array<Events, 17> listeners{};
    std::array<uint64_t, 16> tokens{};
    for (size_t i = 0; i < tokens.size(); ++i) tokens[i] = a.Subscribe(listeners[i]);
    uint64_t excess = 42;
    Check(a.mount.subscribe(a.mount.context, a.panel, Events::Receive, &listeners.back(), &excess) == PULSE_ERROR_STATE &&
        excess == 0, "mount subscription cap is exactly sixteen");
    Pump();
    Check(std::all_of(listeners.begin(), listeners.end(), [](const Events &events) { return !events.calls; }),
        "initial mount event waits for a later host turn even when ordinary messages are pumped");
    Check(a.mount.unsubscribe(a.mount.context, tokens[0]) == PULSE_OK, "unsubscribe pending initial event");
    a.Sync();
    Check(!listeners[0].calls && std::all_of(listeners.begin() + 1, listeners.end() - 1,
        [&](const Events &events) { return events.calls == 1 && events.panel == a.panel && events.last.revision > 0; }),
        "cancelled initial event is suppressed and all retained listeners receive one snapshot");
    a.Sync();
    Check(listeners[1].calls == 1, "unchanged host turn emits no duplicate mount snapshot");
    for (size_t i = 1; i < tokens.size(); ++i)
        Check(a.mount.unsubscribe(a.mount.context, tokens[i]) == PULSE_OK, "release retained subscription token");
    Events foreign;
    std::array<PulseStatus, 5> wrong_thread{};
    std::thread worker([&] {
        PulseUiMountInfo info{}; info.size = sizeof(info);
        uint64_t token = 0;
        wrong_thread[0] = a.adapter.QueryMount(1, &table, sizeof(table));
        wrong_thread[1] = a.mount.get(a.mount.context, a.panel, &info);
        wrong_thread[2] = a.mount.set(a.mount.context, a.panel, PULSE_UI_MOUNT_SIDEBAR);
        wrong_thread[3] = a.mount.subscribe(a.mount.context, a.panel, Events::Receive, &foreign, &token);
        wrong_thread[4] = a.mount.unsubscribe(a.mount.context, tokens[0]);
    });
    worker.join();
    Check(std::all_of(wrong_thread.begin(), wrong_thread.end(), [](PulseStatus value) {
        return value == PULSE_ERROR_STATE;
    }), "query get set subscribe and unsubscribe reject non-owner thread");
}
void MigrationChecks(Attachment &a) {
    const HWND window = a.adapter.Window(), input = a.adapter.NodeWindow("input");
    Check(window && input && OwnedBy(window, a.host.hwnd), "create actual owned window and editable child HWND");
    SetWindowTextW(input, L"preserved input");
    Pump();
    Check(a.Text() == "preserved input", "real editable input commits to panel state");
    struct Clicks {
        unsigned calls = 0;
        static void PULSE_CALL Receive(void *context, uint64_t, PulseStringView) {
            ++static_cast<Clicks *>(context)->calls;
        }
    } clicks;
    uint64_t click_token = 0;
    Check(a.ui.subscribe_command(a.ui.context, a.panel, S("run"), Clicks::Receive, &clicks, &click_token) == PULSE_OK,
        "subscribe existing button before moving its panel");
    Events changes;
    const auto token = a.Subscribe(changes);
    a.Sync();
    const auto first_revision = changes.last.revision;
    const auto sidebar_status = a.Set(PULSE_UI_MOUNT_SIDEBAR);
    if (sidebar_status != PULSE_OK || !a.adapter.IsSidebar() || !ChildOf(window, a.host.hwnd) || !a.host.renderer.sidebar)
        std::printf("[INFO] sidebar status=%u placement=%u child=%u parent=%p owner=%p style=%llx visible=%u reserved=%u\n",
            sidebar_status, a.adapter.MountInfo().placement, ChildOf(window, a.host.hwnd), GetParent(window), a.host.hwnd,
            static_cast<unsigned long long>(GetWindowLongPtrW(window, GWL_STYLE)), IsWindowVisible(window), a.host.renderer.sidebar);
    Check(sidebar_status == PULSE_OK && a.adapter.IsSidebar() &&
        ChildOf(window, a.host.hwnd) && a.host.renderer.sidebar, "sidebar is a real child and reserves host right dock");
    Check(a.Set(PULSE_UI_MOUNT_WORKSPACE) == PULSE_OK && a.adapter.IsWorkspace() &&
        ChildOf(window, a.host.hwnd) && !a.host.renderer.sidebar,
        "workspace moves the existing child into host content and releases sidebar layout");
    Check(a.Set(PULSE_UI_MOUNT_WINDOW) == PULSE_OK && OwnedBy(window, a.host.hwnd),
        "window remount restores owned popup styles and owner relationship");
    Check(a.adapter.Window() == window && a.adapter.NodeWindow("input") == input && a.Text() == "preserved input",
        "all three placements retain the same panel HWND control HWND and edited state");
    SendMessageW(a.adapter.NodeWindow("run"), BM_CLICK, 0, 0);
    Check(clicks.calls == 1, "pre-migration button subscription still calls through real control");
    Check(changes.calls == 1, "multiple synchronous moves do not synchronously deliver mount events");
    a.Sync();
    Check(changes.calls == 2 && changes.last.placement == PULSE_UI_MOUNT_WINDOW &&
        changes.last.revision > first_revision, "moves coalesce to final placement with a newer revision");
    Events cancelled;
    const auto cancelled_token = a.Subscribe(cancelled);
    a.Sync();
    Check(a.Set(PULSE_UI_MOUNT_SIDEBAR) == PULSE_OK &&
        a.mount.unsubscribe(a.mount.context, cancelled_token) == PULSE_OK, "cancel listener with a queued placement delta");
    a.Sync();
    Check(cancelled.calls == 1, "unsubscribe suppresses a queued placement delta after initial delivery");
    Check(a.Set(PULSE_UI_MOUNT_WORKSPACE) == PULSE_OK, "mount workspace for geometry theme and focus checks");
    a.Sync();
    a.host.Resize(1000, 700);
    a.host.darkMode = true;
    a.host.scale = 1.5f;
    SetFocus(a.host.hwnd);
    a.Sync();
    const auto info = a.Info();
    RECT client{}; GetClientRect(window, &client);
    RECT location{}; GetWindowRect(window, &location);
    MapWindowPoints(nullptr, a.host.hwnd, reinterpret_cast<POINT *>(&location), 2);
    Check(info.dark == 1 && info.dpi == 144 && !info.focused &&
        info.width_dip == static_cast<uint32_t>(std::lround(static_cast<float>(client.right) / 1.5f)) &&
        info.height_dip == static_cast<uint32_t>(std::lround(static_cast<float>(client.bottom) / 1.5f)),
        "mount info reports current client DIP dimensions host DPI theme and focus");
    Check(location.left == 20 && location.top == 60 && location.right == 980 && location.bottom == 680,
        "workspace child actually occupies the host content rectangle");
    SetFocus(input);
    a.Sync();
    Check(a.Info().focused == 1, "focus in a retained descendant marks the mounted panel focused");
    a.host.darkMode = false;
    a.host.scale = 1;
    a.host.Resize(1200, 800);
    Check(a.Set(PULSE_UI_MOUNT_WINDOW) == PULSE_OK, "release host area before owner contention checks");
    a.Sync();
    Check(a.mount.unsubscribe(a.mount.context, token) == PULSE_OK &&
        a.ui.unsubscribe(a.ui.context, click_token) == PULSE_OK, "both mount and original UI tokens survive migration");
}
void ContentionAndStopChecks(Attachment &a, Attachment &b) {
    PulseUiMountInfo info{}; info.size = sizeof(info);
    Events foreign;
    uint64_t token = 42;
    Check(a.mount.get(a.mount.context, b.panel, &info) == PULSE_ERROR_STATE &&
        a.mount.set(a.mount.context, b.panel, PULSE_UI_MOUNT_SIDEBAR) == PULSE_ERROR_STATE &&
        a.mount.subscribe(a.mount.context, b.panel, Events::Receive, &foreign, &token) == PULSE_ERROR_STATE && !token,
        "foreign panel handles reject get set and subscribe without affecting their owner");
    Check(a.Set(PULSE_UI_MOUNT_SIDEBAR) == PULSE_OK && b.Set(PULSE_UI_MOUNT_SIDEBAR) == PULSE_ERROR_STATE &&
        a.adapter.IsSidebar() && OwnedBy(b.adapter.Window(), b.host.hwnd),
        "second attachment cannot steal occupied sidebar and remains in its owned window");
    Check(b.Set(PULSE_UI_MOUNT_WORKSPACE) == PULSE_OK && a.Set(PULSE_UI_MOUNT_WORKSPACE) == PULSE_ERROR_STATE &&
        a.adapter.IsSidebar() && b.adapter.IsWorkspace(), "workspace slot is independently exclusive per owner");
    {
        FakeHost independent;
        Check(independent.hwnd != nullptr, "create independent owner while both first-owner host slots are occupied");
        if (independent.hwnd) {
            Attachment c(independent);
            Check(c.Create() && c.Set(PULSE_UI_MOUNT_SIDEBAR) == PULSE_OK && c.adapter.IsSidebar() &&
                a.adapter.IsSidebar() && b.adapter.IsWorkspace(),
                "sidebar reservations are keyed by owner rather than a process-wide singleton");
        }
    }
    Check(a.Set(PULSE_UI_MOUNT_WINDOW) == PULSE_OK && b.Set(PULSE_UI_MOUNT_SIDEBAR) == PULSE_OK &&
        a.Set(PULSE_UI_MOUNT_WORKSPACE) == PULSE_OK, "moving out releases slots for the other attachment");
    Check(b.ui.close(b.ui.context, b.panel) == PULSE_OK && a.Set(PULSE_UI_MOUNT_SIDEBAR) == PULSE_OK,
        "closing one attachment releases its slot without reconstructing another attachment");
    Check(a.Set(PULSE_UI_MOUNT_WINDOW) == PULSE_OK, "release native sidebar before legacy claim");
    UiAdapter::SetLegacySidebar(a.host.hwnd, true);
    Check(a.Set(PULSE_UI_MOUNT_SIDEBAR) == PULSE_ERROR_STATE, "legacy declared sidebar blocks native acquisition");
    UiAdapter::SetLegacySidebar(a.host.hwnd, false);
    Check(a.Set(PULSE_UI_MOUNT_SIDEBAR) == PULSE_OK, "native sidebar becomes available after legacy release");
    Events stopped;
    a.Subscribe(stopped);
    const auto old_panel = a.panel;
    a.adapter.Stop();
    a.Sync();
    Check(!stopped.calls && !UiAdapter::SidebarOccupied(a.host.hwnd) && !a.host.renderer.sidebar &&
        a.mount.get(a.mount.context, old_panel, &info) == PULSE_ERROR_STATE,
        "stop cancels pending events closes original panel and restores host sidebar layout");
    Check(b.Create() && b.Set(PULSE_UI_MOUNT_SIDEBAR) == PULSE_OK,
        "another live attachment reuses sidebar after owner attachment stops");
    Check(a.host.clamped > 0, "host geometry callback runs when sidebar occupancy changes");
}
void ReentrantAndFallbackChecks(Attachment &b) {
    Check(b.Set(PULSE_UI_MOUNT_WINDOW) == PULSE_OK, "begin reentrant event case in owned window");
    Events mover, after_move;
    bool inside = false;
    PulseStatus moved = PULSE_ERROR_STATE;
    mover.action = [&](uint64_t panel) {
        inside = b.adapter.InCallback();
        if (mover.calls == 1) {
            moved = b.mount.set(b.mount.context, panel, PULSE_UI_MOUNT_WORKSPACE);
            b.Sync();
        }
    };
    const auto mover_token = b.Subscribe(mover), later_token = b.Subscribe(after_move);
    b.Sync();
    Check(moved == PULSE_OK && inside && mover.calls == 1 && !after_move.calls && mover.maximum_depth == 1,
        "callback move and nested host sync do not reenter or deliver stale placement to later listeners");
    b.Sync();
    Check(mover.calls == 2 && after_move.calls == 1 && mover.maximum_depth == 1 &&
        mover.last.placement == PULSE_UI_MOUNT_WORKSPACE && !b.adapter.InCallback(),
        "callback move produces fresh placement on a later turn and exits callback unload gate");
    mover.action = {};
    Check(b.Set(PULSE_UI_MOUNT_SIDEBAR) == PULSE_OK, "mount sidebar before narrow host fallback");
    b.Sync();
    const HWND window = b.adapter.Window(), input = b.adapter.NodeWindow("input");
    b.host.Resize(650, 700);
    b.Sync();
    Check(b.Info().placement == PULSE_UI_MOUNT_WINDOW && OwnedBy(window, b.host.hwnd) &&
        !b.host.renderer.sidebar && !UiAdapter::SidebarOccupied(b.host.hwnd) &&
        b.adapter.Window() == window && b.adapter.NodeWindow("input") == input &&
        mover.last.placement == PULSE_UI_MOUNT_WINDOW,
        "narrow host falls back to usable owned window reports event and retains actual controls");
    Check(b.Set(PULSE_UI_MOUNT_SIDEBAR) == PULSE_OK && b.Info().placement == PULSE_UI_MOUNT_WINDOW,
        "explicit sidebar request in narrow host reports its actual window fallback");
    RECT suggested{50, 50, 770, 680};
    SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&suggested));
    b.Sync();
    Check(b.Info().dpi == 144 && b.Set(PULSE_UI_MOUNT_WINDOW) == PULSE_OK && b.Info().dpi == 144,
        "injected WINDOW DPI change survives host sync and idempotent window mount");
    b.host.Resize(1200, 800);
    Check(b.Set(PULSE_UI_MOUNT_SIDEBAR) == PULSE_OK, "return from window fallback to available host sidebar");
    b.Sync();
    Check(b.Info().dpi == 96 && b.adapter.Window() == window && b.adapter.NodeWindow("input") == input,
        "docked DPI follows host again without replacing panel or input");
    Check(b.mount.unsubscribe(b.mount.context, mover_token) == PULSE_OK &&
        b.mount.unsubscribe(b.mount.context, later_token) == PULSE_OK, "reentrant and fallback listeners stay cancellable");
    Events closer, after_close;
    closer.action = [&](uint64_t panel) {
        inside = b.adapter.InCallback();
        b.ui.close(b.ui.context, panel);
        b.Sync();
    };
    b.Subscribe(closer);
    b.Subscribe(after_close);
    b.Sync();
    Check(closer.calls == 1 && inside && !after_close.calls && closer.maximum_depth == 1 &&
        !b.adapter.InCallback() && !b.adapter.Window() && !UiAdapter::SidebarOccupied(b.host.hwnd),
        "callback close cancels later listeners releases slot and safely returns through unload gate");
}
} // namespace
int wmain() {
    pulse::compat::EnableDpiAwareness();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) { std::printf("[FAIL] initialize real COM apartment: 0x%08lx\n", static_cast<unsigned long>(com)); return 2; }
    {
        FakeHost host;
        Check(host.hwnd != nullptr, "create real owner HWND for fake geometry host");
        if (host.hwnd) {
            Attachment a(host), b(host);
            Check(a.Create() && b.Create(), "multiple attachments create distinct real owned windows");
            if (a.panel && b.panel) {
                AbiAndSubscriptionChecks(a);
                MigrationChecks(a);
                ContentionAndStopChecks(a, b);
                ReentrantAndFallbackChecks(b);
            }
            b.adapter.Stop();
            FakeHost second_host;
            Check(second_host.hwnd != nullptr, "create independent owner for owner teardown case");
            if (second_host.hwnd) {
                Attachment c(second_host);
                Check(c.Create() && c.Set(PULSE_UI_MOUNT_SIDEBAR) == PULSE_OK,
                    "independent owner can acquire its own sidebar");
                Events owner_closed;
                c.Subscribe(owner_closed);
                const HWND old_owner = second_host.hwnd;
                DestroyWindow(old_owner);
                second_host.hwnd = nullptr;
                c.Sync();
                Check(!c.adapter.Window() && !UiAdapter::SidebarOccupied(old_owner) && !owner_closed.calls,
                    "destroying owner closes original child releases registry and cancels pending delivery");
            }
        }
    }
    CoUninitialize();
    return failures ? 1 : 0;
}

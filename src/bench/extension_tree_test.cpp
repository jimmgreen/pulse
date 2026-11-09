#include "../common/localization.h"
#include "../ui/extension_tree.h"
#include <algorithm>
#include <commctrl.h>
#include <cstdio>
#include <cwchar>
#include <objbase.h>
#include <oleacc.h>
#include <thread>
namespace {
enum class PopupAction { SelectSecond, Cancel, CloseView, Recursive };
struct PopupProbe {
    pulse::ui::ExtensionTreeView *view{};
    HWND select{}, owner{}, root{};
    PopupAction action{};
    ULONGLONG started{}, last_step{};
    int steps = 0;
    bool saw_fluent = false, saw_native = false, dispatched = false, timed_out = false;
};
PopupProbe *active_popup = nullptr;
BOOL CALLBACK FindOwnedFluentMenu(HWND window, LPARAM context) {
    auto *result = reinterpret_cast<HWND *>(context);
    DWORD process = 0;
    DWORD thread = GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || thread != GetCurrentThreadId() || !IsWindowVisible(window))
        return TRUE;
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    HWND owner = GetWindow(window, GW_OWNER);
    if (std::wcscmp(name, L"PulseFluentMenu") == 0 && active_popup &&
        (owner == active_popup->owner || owner == active_popup->root)) {
        *result = window;
        return FALSE;
    }
    return TRUE;
}
void CALLBACK DrivePopup(HWND, UINT, UINT_PTR timer, DWORD) {
    auto *probe = active_popup;
    if (!probe) {
        KillTimer(nullptr, timer);
        return;
    }
    if (GetTickCount64() - probe->started > 5000) {
        probe->timed_out = true;
        probe->view->Close();
        KillTimer(nullptr, timer);
        return;
    }
    if (probe->dispatched && probe->action != PopupAction::Recursive)
        return;
    HWND popup = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindOwnedFluentMenu, reinterpret_cast<LPARAM>(&popup));
    if (!popup)
        return;
    probe->saw_fluent = true;
    COMBOBOXINFO info{sizeof(info)};
    probe->saw_native = SendMessageW(probe->select, CB_GETDROPPEDSTATE, 0, 0) != 0 ||
                        (GetComboBoxInfo(probe->select, &info) && IsWindowVisible(info.hwndList));
    if (probe->action == PopupAction::Recursive) {
        if (GetTickCount64() - probe->last_step < 250)
            return;
        probe->last_step = GetTickCount64();
        WPARAM keys[] = {VK_RETURN, VK_ESCAPE, VK_RETURN, VK_RETURN, VK_RETURN};
        if (probe->steps < 5)
            PostMessageW(popup, WM_KEYDOWN, keys[probe->steps++], 0);
        return;
    }
    probe->dispatched = true;
    if (probe->action == PopupAction::CloseView) {
        probe->view->Close();
        return;
    }
    PostMessageW(popup, WM_KEYDOWN, VK_DOWN, 0);
    PostMessageW(popup, WM_KEYDOWN, probe->action == PopupAction::Cancel ? VK_ESCAPE : VK_RETURN, 0);
}
PopupProbe ExercisePopup(pulse::ui::ExtensionTreeView &view, PopupAction action, const char *id = "select") {
    PopupProbe probe;
    probe.view = &view;
    probe.select = view.NodeWindow(id);
    probe.owner = view.Hwnd();
    probe.root = GetAncestor(probe.owner, GA_ROOT);
    probe.action = action;
    probe.started = GetTickCount64();
    active_popup = &probe;
    UINT_PTR timer = SetTimer(nullptr, 0, 50, DrivePopup);
    if (timer) {
        SendMessageW(probe.select, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(12, 12));
        if (action == PopupAction::Recursive && IsWindow(probe.select))
            SendMessageW(probe.select, WM_LBUTTONUP, 0, MAKELPARAM(12, 12));
    } else
        probe.timed_out = true;
    if (timer)
        KillTimer(nullptr, timer);
    active_popup = nullptr;
    return probe;
}
} // namespace
int main() {
    struct ComScope {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~ComScope() {
            if (SUCCEEDED(result))
                CoUninitialize();
        }
    } com;
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en");
    using namespace pulse;
    extensions::UiTree tree;
    tree.state.push_back({"name", "hello"});
    extensions::UiNode input;
    input.type = extensions::UiNodeType::TextInput;
    input.id = "input";
    input.state = "name";
    tree.root.children.push_back(input);
    extensions::UiNode toggle;
    toggle.id = "switch";
    toggle.type = extensions::UiNodeType::Switch;
    toggle.text = "Enable feature";
    toggle.state = "switch_state";
    tree.state.push_back({"switch_state", "false"});
    tree.root.children.push_back(toggle);

    extensions::UiNode select;
    select.id = "select";
    select.type = extensions::UiNodeType::Select;
    select.options = {"A", "B"};
    select.state = "selection";
    tree.state.push_back({"selection", "1"});
    tree.root.children.push_back(select);
    extensions::UiNode list;
    list.id = "list";
    list.type = extensions::UiNodeType::List;
    list.options = {"First", "Second", "Third"};
    list.state = "list_selection";
    tree.state.push_back({"list_selection", "0"});
    tree.root.children.push_back(list);

    extensions::UiNode progress;
    progress.id = "progress";
    progress.type = extensions::UiNodeType::Progress;
    progress.binding = "state.progress";
    tree.state.push_back({"progress", "72"});
    tree.root.children.push_back(progress);
    extensions::UiNode tabs;
    tabs.id = "tabs";
    tabs.type = extensions::UiNodeType::Tabs;
    tabs.state = "tab";
    tree.state.push_back({"tab", "0"});
    for (int k = 0; k < 2; ++k) {
        extensions::UiNode tab;
        tab.type = extensions::UiNodeType::Tab;
        tab.id = "tab" + std::to_string(k);
        tab.text = tab.id;
        extensions::UiNode text;
        text.id = "tabtext" + std::to_string(k);
        text.type = extensions::UiNodeType::Text;
        text.text = "Content";
        tab.children.push_back(text);
        tabs.children.push_back(tab);
    }
    tree.root.children.push_back(tabs);
    extensions::UiNode button;
    button.type = extensions::UiNodeType::Button;
    button.id = "close";
    button.command = "close";
    tree.root.children.push_back(button);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    HWND parent = CreateWindowExW(0, L"STATIC", L"test", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    ui::ExtensionTreeView view;
    ui::ExtensionTreeCallbacks callbacks;
    bool enabled = false;
    callbacks.command_enabled = [&](const std::string &) { return enabled; };
    callbacks.on_command = [&](const std::string &) { view.Close(); };
    int failures = 0;
    auto Check = [&](bool condition, const char *name) {
        std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition)
            ++failures;
    };
    Check(view.Create(parent, tree, callbacks, false, {}, 1), "create native host");
    view.SetBounds({0, 0, 400, 300});
    HDC capture = CreateCompatibleDC(nullptr);
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = 400;
    bitmap_info.bmiHeader.biHeight = -300;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    void *capture_pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(capture, &bitmap_info, DIB_RGB_COLORS, &capture_pixels, nullptr, 0);
    auto previous = SelectObject(capture, bitmap);
    Check(view.PaintTo(capture, 0, 0), "shared Painter and Pulse EditHost frame captured");
    unsigned min_channel = 255, max_channel = 0;
    if (capture_pixels) {
        auto *bytes = static_cast<unsigned char *>(capture_pixels);
        for (int index = 0; index < 400 * 300 * 4; ++index)
            if (index % 4 != 3) {
                min_channel = std::min(min_channel, static_cast<unsigned>(bytes[index]));
                max_channel = std::max(max_channel, static_cast<unsigned>(bytes[index]));
            }
    }
    Check(max_channel > min_channel + 8, "shared compositor capture has rendered content");
    SelectObject(capture, previous);
    DeleteObject(bitmap);
    DeleteDC(capture);
    HWND original = view.NodeWindow("input");
    Check(original != nullptr, "native EDIT exists");
    Check(view.StateValue("name") == "hello", "initial local state");
    const auto initial_state_revision = view.StateRevision();
    Check(view.SetState("name", "hello") && view.StateRevision() == initial_state_revision,
          "unchanged state does not invalidate an outstanding script snapshot");
    view.Refresh();
    Check(view.StateRevision() == initial_state_revision, "refresh without state changes keeps script revision");
    Check(view.SetState("name", "changed"), "set declared state");
    Check(view.StateRevision() > initial_state_revision, "actual state edit invalidates outstanding script snapshot");
    const auto changed_revision = view.StateRevision();
    Check(!view.SetStates({{"name", "partial"}, {"missing", "invalid"}}) &&
              view.StateRevision() == changed_revision && view.StateValue("name") == "changed",
          "rejected script batch neither changes state nor advances revision");
    view.SetBounds({0, 0, 300, 250});
    Check(original == view.NodeWindow("input"), "resize retains HWND identity");
    Check(view.StateValue("name") == "changed", "resize retains state");
    const auto before_native_revision = view.StateRevision();
    SetWindowTextW(original, L"native");
    Check(view.StateValue("name") == "native", "native EDIT notification updates state");
    Check(view.StateRevision() > before_native_revision, "native EDIT invalidates script snapshot");
    const auto before_noop_tree = view.StateRevision();
    Check(view.UpdateTree(tree) && view.StateRevision() == before_noop_tree,
          "equivalent tree update preserves actual state revision");
    auto changed_structure = tree;
    ++changed_structure.root.gap;
    Check(view.UpdateTree(changed_structure) && view.StateRevision() > before_noop_tree,
          "changed tree structure invalidates old script results even when state values match");
    Check(view.UpdateTree(tree), "restore tree structure after revision regression");
    Check(SendMessageW(view.NodeWindow("select"), CB_GETCURSEL, 0, 0) == 1, "Select initializes index");
    Check(SendMessageW(view.NodeWindow("progress"), PBM_GETPOS, 0, 0) == 72, "Progress reads bound state");
    Check(!(GetWindowLongPtrW(view.NodeWindow("tabtext1"), GWL_STYLE) & WS_VISIBLE), "inactive tab hidden");
    RECT tab_rect{};
    GetClientRect(view.NodeWindow("tabs"), &tab_rect);
    SendMessageW(view.NodeWindow("tabs"), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(tab_rect.right * 3 / 4, 12));
    SendMessageW(view.NodeWindow("tabs"), WM_LBUTTONUP, 0, MAKELPARAM(tab_rect.right * 3 / 4, 12));
    Check(view.StateValue("tab") == "1", "click drawn second tab selects second page");
    Check(SendMessageW(view.NodeWindow("list"), LB_GETITEMHEIGHT, 0, 0) == 32,
          "list input row height matches Painter rows");
    SendMessageW(view.NodeWindow("list"), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 48));
    SendMessageW(view.NodeWindow("list"), WM_LBUTTONUP, 0, MAKELPARAM(20, 48));
    Check(view.StateValue("list_selection") == "1", "click drawn second list row selects second item");
    Check(view.SetState("tab", "1"), "set tab index");
    Check(GetWindowLongPtrW(view.NodeWindow("tabtext1"), GWL_STYLE) & WS_VISIBLE, "selected tab visible");
    Check(!(GetWindowLongPtrW(view.NodeWindow("tabtext0"), GWL_STYLE) & WS_VISIBLE), "previous tab hidden");
    Check(!view.SetState("name", std::string(1025, 'x')), "reject oversized state");
    Check(!view.SetState("selection", "2"), "reject selection overflow");
    Check(!view.SetState("selection", "-1"), "reject negative index");
    Check(!view.SetState("tab", "01"), "reject noncanonical index");
    Check(!view.SetState("progress", "101"), "reject progress overflow");
    Check(!view.SetState("progress", "-1"), "reject negative progress");
    Check(!view.SetState("name", std::string("a\0b", 3)), "reject embedded NUL state");
    Check(view.SetState("switch_state", "true"), "Switch accepts boolean state");
    Check(SendMessageW(view.NodeWindow("switch"), BM_GETCHECK, 0, 0) == BST_CHECKED,
          "Switch state synchronizes input model");
    Check(!view.SetState("switch_state", "1"), "Switch rejects nonboolean state");
    Check(!view.SetState("missing", "x"), "reject undeclared state");
    Check(!view.StateValue("missing"), "unknown state absent");
    Check(!view.NodeWindow("missing"), "unknown node absent");
    Check(view.SetState("selection", "0"), "update selection state");
    Check(SendMessageW(view.NodeWindow("select"), CB_GETCURSEL, 0, 0) == 0, "selection refresh applied");
    Check(view.SetState("progress", "0"), "progress lower boundary");
    Check(view.SetState("progress", "100"), "progress upper boundary");
    Check(SendMessageW(view.NodeWindow("progress"), PBM_GETPOS, 0, 0) == 100, "progress refresh applied");
    SendMessageW(view.Hwnd(), WM_COMMAND, MAKEWPARAM(0, BN_CLICKED), (LPARAM)view.NodeWindow("close"));
    Check(view.Hwnd() != nullptr, "disabled command rejected");
    enabled = true;
    view.Refresh();
    Check(IsWindowEnabled(view.NodeWindow("close")), "authorized command enabled");
    SendMessageW(view.Hwnd(), WM_COMMAND, MAKEWPARAM(0, BN_CLICKED), (LPARAM)view.NodeWindow("close"));
    Check(!view.Hwnd(), "callback closes view synchronously");
    Check(!IsWindow(original), "close destroys child HWND");
    extensions::UiTree bound;
    bound.root.type = extensions::UiNodeType::Text;
    bound.root.id = "bound";
    bound.root.binding = "context.secret";
    bool readable = true;
    ui::ExtensionTreeCallbacks read_callbacks;
    read_callbacks.read_binding = [&](const std::string &) -> std::optional<std::string> {
        return readable ? std::optional<std::string>("visible") : std::nullopt;
    };
    Check(view.Create(parent, bound, read_callbacks, false, {}, 1), "recreate after close");
    Check(GetWindowTextLengthW(view.NodeWindow("bound")) == 7, "context binding readable");
    readable = false;
    view.Refresh();
    Check(GetWindowTextLengthW(view.NodeWindow("bound")) == 0, "failed binding clears stale data");
    view.Close();
    read_callbacks.read_binding = [&](const std::string &) -> std::optional<std::string> {
        view.Close();
        return "obsolete";
    };
    Check(!view.Create(parent, bound, read_callbacks, false, {}, 1), "binding callback close revokes creation");
    Check(!view.Hwnd(), "closed binding callback leaves no HWND");
    extensions::UiTree disabled;
    disabled.root.enabled = false;
    extensions::UiNode action;
    action.id = "disabled_action";
    action.type = extensions::UiNodeType::Button;
    action.command = "action";
    disabled.root.children.push_back(action);
    int calls = 0;
    ui::ExtensionTreeCallbacks action_callbacks;
    action_callbacks.on_command = [&](const std::string &) { ++calls; };
    Check(view.Create(parent, disabled, action_callbacks, false, {}, 1), "disabled ancestor tree creates");
    Check(!IsWindowEnabled(view.NodeWindow("disabled_action")), "ancestor disabled propagates");
    SendMessageW(view.Hwnd(), WM_COMMAND, MAKEWPARAM(0, BN_CLICKED), (LPARAM)view.NodeWindow("disabled_action"));
    Check(calls == 0, "disabled ancestor rejects forged command");
    view.Close();
    disabled.root.enabled = true;
    disabled.root.visible = false;
    Check(view.Create(parent, disabled, action_callbacks, false, {}, 1), "hidden ancestor tree creates");
    SendMessageW(view.Hwnd(), WM_COMMAND, MAKEWPARAM(0, BN_CLICKED), (LPARAM)view.NodeWindow("disabled_action"));
    Check(calls == 0, "hidden ancestor rejects forged command");
    view.Close();
    extensions::UiTree scrolling;
    for (int index = 0; index < 20; ++index) {
        extensions::UiNode row;
        row.id = "scroll_" + std::to_string(index);
        row.type = index == 0 ? extensions::UiNodeType::Checkbox : extensions::UiNodeType::Text;
        row.text = "Scrollable row";
        scrolling.root.children.push_back(row);
    }
    for (float dpi_scale : {1.0f, 1.5f}) {
        Check(view.Create(parent, scrolling, {}, false, {}, dpi_scale), "create shared scrollbar test surface");
        view.SetBounds({0, 0, 300, 200});
        HWND scrolling_host = view.Hwnd();
        POINT gutter_point{290, 18};
        HWND gutter_child = ChildWindowFromPointEx(scrolling_host, gutter_point, CWP_SKIPINVISIBLE);
        Check(gutter_child == scrolling_host, "overflow scrollbar gutter is outside child input proxy");
        RECT child_bounds{};
        GetWindowRect(view.NodeWindow("scroll_0"), &child_bounds);
        MapWindowPoints(nullptr, scrolling_host, reinterpret_cast<POINT *>(&child_bounds), 2);
        Check(child_bounds.right <= 300 - static_cast<int>(14 * dpi_scale),
              "overflow children end before scrollbar hit gutter");
        POINT child_point = gutter_point;
        MapWindowPoints(scrolling_host, gutter_child, &child_point, 1);
        SendMessageW(gutter_child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(child_point.x, child_point.y));
        Check(GetCapture() == scrolling_host, "reserved gutter pointer captures shared scrollbar");
        SendMessageW(scrolling_host, WM_LBUTTONUP, 0, MAKELPARAM(gutter_point.x, gutter_point.y));

        SendMessageW(scrolling_host, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(296, 8));
        Check(GetCapture() == scrolling_host, "shared scrollbar thumb captures own window");
        SendMessageW(scrolling_host, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(296, 10000));
        RECT last{};
        GetWindowRect(view.NodeWindow("scroll_19"), &last);
        MapWindowPoints(nullptr, scrolling_host, reinterpret_cast<POINT *>(&last), 2);
        Check(last.bottom <= 200 && last.bottom >= 190, "scrollbar drag clamps to content bottom at scale");
        SendMessageW(scrolling_host, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(296, -10000));
        RECT first{};
        GetWindowRect(view.NodeWindow("scroll_0"), &first);
        MapWindowPoints(nullptr, scrolling_host, reinterpret_cast<POINT *>(&first), 2);
        Check(first.top == 8, "scrollbar drag clamps to content top at scale");
        SendMessageW(scrolling_host, WM_LBUTTONUP, 0, MAKELPARAM(296, 0));
        Check(GetCapture() != scrolling_host, "scrollbar release drops pointer capture");
        SendMessageW(scrolling_host, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(296, 190));
        GetWindowRect(view.NodeWindow("scroll_0"), &first);
        MapWindowPoints(nullptr, scrolling_host, reinterpret_cast<POINT *>(&first), 2);
        Check(first.top < 0, "shared scrollbar track click moves content");
        view.Close();
        Check(GetCapture() != scrolling_host, "closing scrolling view releases capture");
    }
    extensions::UiTree horizontal;
    horizontal.root.orientation = "horizontal";
    for (int index = 0; index < 3; ++index) {
        extensions::UiNode button_node;
        button_node.id = "horizontal_" + std::to_string(index);
        button_node.type = extensions::UiNodeType::Button;
        button_node.text = "Responsive action";
        horizontal.root.children.push_back(button_node);
    }
    Check(view.Create(parent, horizontal, {}, false, {}, 1), "create horizontal stack layout");
    view.SetBounds({0, 0, 200, 400});
    RECT first_horizontal{}, second_horizontal{};
    GetWindowRect(view.NodeWindow("horizontal_0"), &first_horizontal);
    GetWindowRect(view.NodeWindow("horizontal_1"), &second_horizontal);
    Check(second_horizontal.top >= first_horizontal.bottom, "horizontal stack wraps at narrow width");
    view.SetBounds({0, 0, 600, 200});
    GetWindowRect(view.NodeWindow("horizontal_0"), &first_horizontal);
    GetWindowRect(view.NodeWindow("horizontal_1"), &second_horizontal);
    Check(second_horizontal.top == first_horizontal.top && second_horizontal.left >= first_horizontal.right,
          "horizontal stack reflows into wide row");
    view.Close();
    extensions::UiTree menu_tree;
    menu_tree.state.push_back({"selection", "0"});
    menu_tree.root.children.push_back(select);
    Check(view.Create(parent, menu_tree, {}, false, {}, 1), "create tree for real Select popup interaction");
    view.SetBounds({0, 0, 400, 200});
    auto selected_popup = ExercisePopup(view, PopupAction::SelectSecond);
    Check(selected_popup.saw_fluent && !selected_popup.timed_out, "Select opens actual thread-owned FluentMenu");
    Check(!selected_popup.saw_native, "Select never opens native ComboBox dropdown");
    Check(view.StateValue("selection") == "1", "FluentMenu Down and Enter update selection state");
    auto cancelled_popup = ExercisePopup(view, PopupAction::Cancel);
    Check(cancelled_popup.saw_fluent && !cancelled_popup.timed_out, "Select popup receives Escape through modal loop");
    Check(view.StateValue("selection") == "1", "FluentMenu cancellation preserves selection");
    auto closing_popup = ExercisePopup(view, PopupAction::CloseView);
    Check(closing_popup.saw_fluent && !closing_popup.timed_out,
          "close view while real FluentMenu is open returns safely");
    Check(!view.Hwnd(), "closing popup owner revokes tree and child controls");
    extensions::UiTree expanded;
    expanded.state = {{"query", ""},       {"level", "0"},   {"radio", "false"}, {"segment", "0"},
                      {"toggle", "false"}, {"show", "true"}, {"enable", "true"}, {"ratio", "50"}};
    extensions::UiNode search;
    search.id = "search";
    search.type = extensions::UiNodeType::CommandSearch;
    search.text = "Search";
    search.placeholder = "Find items";
    search.state = "query";
    expanded.root.children.push_back(search);
    extensions::UiNode slider;
    slider.id = "slider";
    slider.type = extensions::UiNodeType::Slider;
    slider.stops = 5;
    slider.state = "level";
    expanded.root.children.push_back(slider);
    extensions::UiNode radio;
    radio.id = "radio";
    radio.type = extensions::UiNodeType::Radio;
    radio.text = "Choice";
    radio.state = "radio";
    expanded.root.children.push_back(radio);
    extensions::UiNode segmented;
    segmented.id = "segmented";
    segmented.type = extensions::UiNodeType::Segmented;
    segmented.options = {"One", "Two"};
    segmented.state = "segment";
    expanded.root.children.push_back(segmented);
    extensions::UiNode toggle_button;
    toggle_button.id = "toggle";
    toggle_button.type = extensions::UiNodeType::Button;
    toggle_button.text = "Toggle";
    toggle_button.button_kind = "toggle";
    toggle_button.state = "toggle";
    toggle_button.command = "toggle_action";
    expanded.root.children.push_back(toggle_button);
    extensions::UiNode dynamic_group;
    dynamic_group.id = "dynamic_group";
    dynamic_group.type = extensions::UiNodeType::Group;
    dynamic_group.text = "Dynamic";
    dynamic_group.visible_binding = "state.show";
    dynamic_group.enabled_binding = "state.enable";
    extensions::UiNode dynamic_action;
    dynamic_action.id = "dynamic_action";
    dynamic_action.type = extensions::UiNodeType::Button;
    dynamic_action.text = "Run";
    dynamic_action.command = "allowed";
    dynamic_group.children.push_back(dynamic_action);
    expanded.root.children.push_back(dynamic_group);
    extensions::UiNode info;
    info.id = "info";
    info.type = extensions::UiNodeType::InfoBar;
    info.text = "Information";
    info.message = "Actions use the declared command gate";
    info.close_command = "close_info";
    extensions::UiMenuItem allowed_action;
    allowed_action.id = "allowed_action";
    allowed_action.text = "Allowed";
    allowed_action.command = "allowed";
    extensions::UiMenuItem denied_action = allowed_action;
    denied_action.id = "denied_action";
    denied_action.text = "Denied";
    denied_action.command = "denied";
    info.actions = {allowed_action, denied_action};
    expanded.root.children.push_back(info);
    extensions::UiNode split;
    split.id = "split";
    split.type = extensions::UiNodeType::SplitPane;
    split.orientation = "horizontal";
    split.state = "ratio";
    for (int i = 0; i < 2; ++i) {
        extensions::UiNode pane;
        pane.id = "pane_" + std::to_string(i);
        pane.type = extensions::UiNodeType::Text;
        pane.text = "Pane content";
        split.children.push_back(pane);
    }
    expanded.root.children.push_back(split);
    extensions::UiNode context_text;
    context_text.id = "context_text";
    context_text.type = extensions::UiNodeType::Text;
    context_text.binding = "context.value";
    expanded.root.children.push_back(context_text);
    std::vector<std::string> commands;
    int reads = 0;
    ui::ExtensionTreeCallbacks expanded_callbacks;
    expanded_callbacks.on_command = [&](const std::string &command) { commands.push_back(command); };
    expanded_callbacks.command_enabled = [](const std::string &command) { return command != "denied"; };
    expanded_callbacks.read_binding = [&](const std::string &) -> std::optional<std::string> {
        ++reads;
        return "context";
    };
    Check(view.Create(parent, expanded, expanded_callbacks, false, {}, 1), "create expanded shared control models");
    view.SetBounds({0, 0, 600, 900});
    reads = 0;
    Check(!view.SetStates({{"show", "false"}, {"level", "99"}}), "state transaction rejects one invalid member");
    Check(view.StateValue("show") == "true" && view.StateValue("level") == "0" && reads == 0,
          "failed transaction makes no state or binding changes");
    Check(!view.SetStates({{"show", "false"}, {"show", "true"}}), "state transaction rejects duplicate keys");
    reads = 0;
    Check(
        view.SetStates({{"show", "false"}, {"enable", "false"}, {"level", "4"}, {"query", "\xE4\xB8\xAD\xE6\x96\x87"}}),
        "valid state batch commits all members");
    Check(reads == 1, "state batch refreshes external bindings once");
    Check(!(GetWindowLongPtrW(view.NodeWindow("dynamic_action"), GWL_STYLE) & WS_VISIBLE),
          "dynamic visible binding hides descendants");
    Check(!IsWindowEnabled(view.NodeWindow("dynamic_action")), "dynamic enabled binding disables descendants");
    SendMessageW(view.Hwnd(), WM_COMMAND, MAKEWPARAM(0, BN_CLICKED), (LPARAM)view.NodeWindow("dynamic_action"));
    Check(commands.empty(), "dynamic hidden disabled node rejects forged command");
    wchar_t query[16]{};
    GetWindowTextW(view.NodeWindow("search"), query, 16);
    Check(std::wstring(query) == L"\x4E2D\x6587", "CommandSearch state uses real Unicode EditHost model");
    Check(view.SetStates({{"show", "true"}, {"enable", "true"}}), "dynamic conditions can be restored atomically");
    Check(IsWindowEnabled(view.NodeWindow("dynamic_action")), "restored dynamic condition re-enables control");
    SendMessageW(view.NodeWindow("slider"), WM_KEYDOWN, VK_HOME, 0);
    Check(view.StateValue("level") == "0", "shared slider Home commits minimum");
    SendMessageW(view.NodeWindow("slider"), WM_KEYDOWN, VK_END, 0);
    Check(view.StateValue("level") == "4", "shared slider End commits maximum");
    SendMessageW(view.NodeWindow("radio"), BM_CLICK, 0, 0);
    Check(view.StateValue("radio") == "true", "real radio activation commits checked state");
    RECT segment_rect{};
    GetClientRect(view.NodeWindow("segmented"), &segment_rect);
    SendMessageW(view.NodeWindow("segmented"), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(segment_rect.right * 3 / 4, 16));
    SendMessageW(view.NodeWindow("segmented"), WM_LBUTTONUP, 0, MAKELPARAM(segment_rect.right * 3 / 4, 16));
    Check(view.StateValue("segment") == "1", "segmented pointer hit matches shared segment geometry");
    SendMessageW(view.NodeWindow("toggle"), BM_CLICK, 0, 0);
    Check(view.StateValue("toggle") == "true" && !commands.empty() && commands.back() == "toggle_action",
          "toggle button commits state and authorized command");
    size_t before_actions = commands.size();
    SendMessageW(view.NodeWindow("info"), WM_KEYDOWN, VK_RETURN, 0);
    Check(commands.size() == before_actions + 1 && commands.back() == "allowed",
          "InfoBar keyboard action follows command gate");
    SendMessageW(view.NodeWindow("info"), WM_KEYDOWN, VK_RIGHT, 0);
    SendMessageW(view.NodeWindow("info"), WM_KEYDOWN, VK_RETURN, 0);
    Check(commands.size() == before_actions + 1, "InfoBar denied action does not dispatch");
    SendMessageW(view.NodeWindow("info"), WM_KEYDOWN, VK_RIGHT, 0);
    SendMessageW(view.NodeWindow("info"), WM_KEYDOWN, VK_RETURN, 0);
    Check(commands.back() == "close_info", "InfoBar close dispatches declared command only");
    Check(view.RevealNode("split"), "RevealNode accepts split container");
    RECT split_rect{};
    GetClientRect(view.NodeWindow("split"), &split_rect);
    SendMessageW(view.NodeWindow("split"), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(split_rect.right / 2, 16));
    SendMessageW(view.NodeWindow("split"), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(split_rect.right - 40, 16));
    SendMessageW(view.NodeWindow("split"), WM_LBUTTONUP, 0, MAKELPARAM(split_rect.right - 40, 16));
    Check(std::stoi(*view.StateValue("ratio")) > 80, "split pane divider drag commits percent state");
    Check(view.RevealNode("dynamic_group"), "RevealNode accepts ordinary container");
    view.Close();
    extensions::UiTree nested;
    extensions::UiNode outer;
    outer.id = "outer";
    outer.type = extensions::UiNodeType::ScrollView;
    outer.min_height = 180;
    extensions::UiNode outer_header;
    outer_header.id = "outer_header";
    outer_header.type = extensions::UiNodeType::Text;
    outer_header.text = "Outer header";
    outer.children.push_back(outer_header);
    extensions::UiNode inner;
    inner.id = "inner";
    inner.type = extensions::UiNodeType::ScrollView;
    inner.min_height = 80;
    for (int i = 0; i < 12; ++i) {
        extensions::UiNode row;
        row.id = "inner_" + std::to_string(i);
        row.type = extensions::UiNodeType::Text;
        row.text = "Nested row";
        inner.children.push_back(row);
    }
    outer.children.push_back(inner);
    for (int i = 0; i < 8; ++i) {
        auto row = outer_header;
        row.id = "outer_" + std::to_string(i);
        outer.children.push_back(row);
    }
    nested.root.children.push_back(outer);
    extensions::UiNode footer = outer_header;
    footer.id = "footer";
    nested.root.children.push_back(footer);
    Check(view.Create(parent, nested, {}, false, {}, 1), "create nested scroll viewports");
    view.SetBounds({0, 0, 400, 300});
    RECT nested_before{}, footer_before{}, outer_before{};
    GetWindowRect(view.NodeWindow("inner_0"), &nested_before);
    GetWindowRect(view.NodeWindow("footer"), &footer_before);
    GetWindowRect(view.NodeWindow("outer_header"), &outer_before);
    POINT wheel_point{20, 70};
    ClientToScreen(view.Hwnd(), &wheel_point);
    SendMessageW(view.Hwnd(), WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)),
                 MAKELPARAM(wheel_point.x, wheel_point.y));
    RECT nested_after{}, footer_after{}, outer_after{};
    GetWindowRect(view.NodeWindow("inner_0"), &nested_after);
    GetWindowRect(view.NodeWindow("footer"), &footer_after);
    GetWindowRect(view.NodeWindow("outer_header"), &outer_after);
    Check(nested_after.top < nested_before.top && footer_after.top == footer_before.top &&
              outer_after.top == outer_before.top,
          "wheel scrolls deepest viewport without moving siblings or outer content");
    Check(view.RevealNode("inner_11"), "RevealNode scrolls nested ancestors to target");
    HRGN visible_region = CreateRectRgn(0, 0, 0, 0);
    GetWindowRgn(view.NodeWindow("inner_11"), visible_region);
    RECT region_bounds{};
    Check(GetRgnBox(visible_region, &region_bounds) != NULLREGION,
          "revealed nested target retains nonempty clipped native input region");
    DeleteObject(visible_region);
    Check(view.RevealNode("outer") && view.RevealNode("inner"), "RevealNode supports nested scroll containers");
    view.Close();
    extensions::UiTree gutter_data;
    extensions::UiNode table;
    table.id = "gutter_table";
    table.text = "Table rows";
    table.type = extensions::UiNodeType::DataTable;
    table.data_columns = {{"name", "Name", 180}};
    for (int i = 0; i < 30; ++i) {
        extensions::UiDataRow row;
        row.id = "data_" + std::to_string(i);
        row.text = "Row " + std::to_string(i);
        row.cells = {row.text};
        table.rows.push_back(row);
    }
    gutter_data.root.children.push_back(table);
    extensions::UiNode tall_footer;
    tall_footer.id = "tall_footer";
    tall_footer.text = "Footer";
    tall_footer.type = extensions::UiNodeType::Text;
    tall_footer.min_height = 600;
    gutter_data.root.children.push_back(tall_footer);
    Check(view.Create(parent, gutter_data, {}, false, {}, 1), "create data viewport inside overflowing tree");
    view.SetBounds({0, 0, 400, 300});
    HWND data_window = view.NodeWindow("gutter_table");
    RECT data_client{}, footer_start{}, footer_end{};
    GetClientRect(data_window, &data_client);
    GetWindowRect(view.NodeWindow("tall_footer"), &footer_start);
    SendMessageW(data_window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(data_client.right - 4, 44));
    Check(GetCapture() == data_window, "data scrollbar capture is not stolen by outer tree gutter");
    SendMessageW(data_window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(data_client.right - 4, 5000));
    SendMessageW(data_window, WM_LBUTTONUP, 0, MAKELPARAM(data_client.right - 4, 5000));
    GetWindowRect(view.NodeWindow("tall_footer"), &footer_end);
    Check(footer_start.top == footer_end.top, "dragging data scrollbar does not scroll outer siblings");
    IAccessible *data_access = nullptr;
    HRESULT data_hr = AccessibleObjectFromWindow(data_window, static_cast<DWORD>(OBJID_CLIENT), IID_IAccessible,
                                                 reinterpret_cast<void **>(&data_access));
    Check(SUCCEEDED(data_hr) && data_access, "data input proxy exposes real virtual-row MSAA");
    if (data_access) {
        VARIANT child{};
        child.vt = VT_I4;
        child.lVal = 1;
        VARIANT flags{};
        HRESULT state_hr = data_access->get_accState(child, &flags);
        Check(SUCCEEDED(state_hr) && flags.vt == VT_I4 && (flags.lVal & STATE_SYSTEM_OFFSCREEN),
              "data drag moves first virtual row offscreen");
        VariantClear(&flags);
        data_access->Release();
    }
    view.Close();
    extensions::UiTree accessible_tree;
    accessible_tree.root.id = "accessible_tree";
    accessible_tree.root.text = "Accessible hierarchy";
    accessible_tree.root.type = extensions::UiNodeType::Tree;
    extensions::UiDataRow branch_row;
    branch_row.id = "parent";
    branch_row.text = "Parent";
    extensions::UiDataRow child_row;
    child_row.id = "child";
    child_row.text = "Child";
    branch_row.children.push_back(child_row);
    accessible_tree.root.rows.push_back(branch_row);
    Check(view.Create(parent, accessible_tree, {}, false, {}, 1), "create accessible tree parent action surface");
    view.SetBounds({0, 0, 400, 300});
    IAccessible *branch_access = nullptr;
    HRESULT branch_hr = AccessibleObjectFromWindow(view.NodeWindow("accessible_tree"), static_cast<DWORD>(OBJID_CLIENT),
                                                   IID_IAccessible, reinterpret_cast<void **>(&branch_access));
    Check(SUCCEEDED(branch_hr) && branch_access, "tree rows expose custom accessibility provider");
    if (branch_access) {
        VARIANT child{};
        child.vt = VT_I4;
        child.lVal = 1;
        long before = 0, after = 0;
        branch_access->get_accChildCount(&before);
        HRESULT action_hr = branch_access->accDoDefaultAction(child);
        branch_access->get_accChildCount(&after);
        VARIANT flags{};
        branch_access->get_accState(child, &flags);
        Check(action_hr == S_OK && before == 2 && after == 1,
              "MSAA parent default action succeeds and collapses visible children");
        Check(flags.vt == VT_I4 && (flags.lVal & STATE_SYSTEM_COLLAPSED),
              "MSAA parent expanded state updates after default action");
        VariantClear(&flags);
        branch_access->Release();
    }
    view.Close();
    extensions::UiTree recursive;
    extensions::UiNode menu_button;
    menu_button.id = "menu_button";
    menu_button.type = extensions::UiNodeType::MenuButton;
    menu_button.text = "Nested menu";
    extensions::UiMenuItem branch;
    branch.id = "branch";
    branch.text = "Level one";
    extensions::UiMenuItem sub;
    sub.id = "sub";
    sub.text = "Level two";
    extensions::UiMenuItem leaf;
    leaf.id = "leaf";
    leaf.text = "Run nested action";
    leaf.command = "allowed";
    sub.children.push_back(leaf);
    branch.children.push_back(sub);
    menu_button.menu.push_back(branch);
    recursive.root.children.push_back(menu_button);
    Check(view.Create(parent, recursive, expanded_callbacks, false, {}, 1), "create recursive real menu button");
    view.SetBounds({0, 0, 400, 200});
    size_t before_menu = commands.size();
    auto recursive_probe = ExercisePopup(view, PopupAction::Recursive, "menu_button");
    Check(recursive_probe.saw_fluent && !recursive_probe.timed_out && recursive_probe.steps == 5,
          "real recursive FluentMenu supports Escape back to parent level");
    Check(commands.size() == before_menu + 1 && commands.back() == "allowed",
          "nested menu leaf executes authorized command once");
    view.Close();
    extensions::UiTree local;
    local.state = {{"show", "true"}, {"name", "before"}};
    local.actions = {{"hide", {{"show", "false"}, {"name", "after"}}},
                     {"invalid", {{"name", "broken"}, {"show", "invalid"}}}};
    extensions::UiNode local_group;
    local_group.id = "local_group";
    local_group.enabled_binding = "state.show";
    extensions::UiNode local_button;
    local_button.id = "local_button";
    local_button.type = extensions::UiNodeType::Button;
    local_button.command = "hide";
    local_button.text = "Hide locally";
    local_group.children.push_back(local_button);
    local.root.children.push_back(local_group);
    local_button.id = "invalid_button";
    local_button.command = "invalid";
    local.root.children.push_back(local_button);
    Check(view.Create(parent, local, {}, false, {}, 1), "local actions work without host command callbacks");
    view.SetBounds({0, 0, 400, 240});
    SendMessageW(view.NodeWindow("invalid_button"), BM_CLICK, 0, 0);
    Check(view.StateValue("name") == "before" && view.StateValue("show") == "true",
          "invalid local action batch rolls back every update");
    SendMessageW(view.NodeWindow("local_button"), BM_CLICK, 0, 0);
    Check(view.StateValue("name") == "after" && view.StateValue("show") == "false",
          "local action atomically updates state without host privileges");
    view.SetState("name", "blocked");
    SendMessageW(view.NodeWindow("local_button"), BM_CLICK, 0, 0);
    Check(view.StateValue("name") == "blocked", "disabled ancestor rejects forged local action click");
    view.Close();
    extensions::UiTree dynamic;
    dynamic.root.id = "dynamic_root";
    dynamic.state = {{"filter", ""},    {"sort", "nameAsc"}, {"status", "idle"}, {"error", ""},
                     {"progress", "0"}, {"selection", ""},   {"form", ""},       {"submitted", "false"}};
    extensions::UiNode query_node;
    query_node.id = "dynamic_query";
    query_node.type = extensions::UiNodeType::TextInput;
    query_node.text = "Filter";
    query_node.state = "filter";
    extensions::UiNode results;
    results.id = "dynamic_results";
    results.type = extensions::UiNodeType::VirtualList;
    results.state = "selection";
    results.min_height = 160;
    extensions::UiDataSource source;
    source.filter_state = "filter";
    source.sort_state = "sort";
    source.status_state = "status";
    source.error_state = "error";
    source.progress_state = "progress";
    source.page_size = 2;
    results.data_source = source;
    extensions::UiNode form;
    form.id = "form";
    form.type = extensions::UiNodeType::TextInput;
    form.text = "Name";
    form.state = "form";
    form.validation = extensions::UiValidation{true, 2, 4, "Enter two to four characters"};
    form.max_height = 32;
    extensions::UiNode submit;
    submit.id = "submit";
    submit.type = extensions::UiNodeType::Button;
    submit.text = "Submit";
    submit.command = "submit";
    extensions::UiNode next = submit;
    next.id = "next";
    next.command = "next";
    dynamic.root.children = {query_node, results, form, submit, next};
    extensions::UiStateAction submit_action;
    submit_action.id = "submit";
    submit_action.updates = {{"submitted", "true"}};
    submit_action.validate = {"form"};
    extensions::UiStateAction next_action;
    next_action.id = "next";
    next_action.data_operation = extensions::UiDataOperation{"dynamic_results", "nextPage"};
    dynamic.actions = {submit_action, next_action};
    std::vector<ui::UiDataRequest> requests;
    std::vector<ui::UiDataCompletion> completions;
    int cancelled = 0;
    ui::ExtensionTreeCallbacks dynamic_callbacks;
    dynamic_callbacks.request_data = [&](const ui::UiDataRequest &request, ui::UiDataCompletion completion) {
        requests.push_back(request);
        completions.push_back(std::move(completion));
        return [&] { ++cancelled; };
    };
    auto drain = [&] {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    };
    Check(view.Create(parent, dynamic, dynamic_callbacks, false, {}, 1), "dynamic source view creates");
    view.SetBounds({0, 0, 500, 700});
    Check(requests.size() == 1 && view.StateValue("status") == "loading", "source begins one asynchronous request");
    ui::ExtensionTreeView isolated_view;
    ui::UiDataCompletion isolated_completion;
    ui::ExtensionTreeCallbacks isolated_callbacks;
    isolated_callbacks.request_data = [&](const ui::UiDataRequest &, ui::UiDataCompletion completion) {
        isolated_completion = std::move(completion);
        return [] {};
    };
    Check(isolated_view.Create(parent, dynamic, isolated_callbacks, true, {}, 1),
          "second package owns a separate source subscription for identical node IDs");
    isolated_view.SetBounds({0, 0, 500, 700});
    ui::UiDataPage idle;
    idle.status = "idle";
    idle.progress = 0;
    completions.front()(idle);
    drain();
    Check(view.StateValue("status") == "idle", "provider idle task status reaches declared state");
    HWND results_window = view.NodeWindow("dynamic_results");
    ui::UiDataPage first;
    first.rows = {{"row1", "First"}, {"row2", "Second"}};
    first.next_cursor = "page2";
    first.revision = "snapshot1";
    std::thread complete_worker([&] { completions.front()(first); });
    complete_worker.join();
    drain();
    Check(view.StateValue("status") == "ready" && view.SetState("selection", "row2"),
          "worker completion marshals rows and selectable IDs to UI thread");
    Check(!view.SetState("selection", "unknown"), "dynamic selection rejects IDs absent from actual rows");
    Check(isolated_view.StateValue("status") == "loading" && !isolated_view.SetState("selection", "row2"),
          "one package completion cannot populate another package rows or status");
    isolated_completion(first);
    drain();
    Check(isolated_view.SetState("selection", "row2"), "independent source accepts its own rows");
    isolated_completion(idle);
    drain();
    Check(isolated_view.StateValue("status") == "idle" && isolated_view.StateValue("selection") == "" &&
              !isolated_view.SetState("selection", "row2"),
          "idle clears previously displayed task rows and selection");
    isolated_view.Close();
    isolated_completion(first);
    drain();

    SendMessageW(view.NodeWindow("next"), BM_CLICK, 0, 0);
    Check(requests.size() == 2 && requests.back().cursor == "page2", "next-page action passes opaque cursor");
    auto second = first;
    second.rows = {{"row2", "Updated second"}, {"row3", "Third"}};
    second.next_cursor = "page3";
    completions.back()(second);
    drain();
    Check(view.StateValue("selection") == "row2" && view.NodeWindow("dynamic_results") == results_window,
          "page append preserves selection and input window");
    IAccessible *paged_access = nullptr;
    AccessibleObjectFromWindow(results_window, static_cast<DWORD>(OBJID_CLIENT), IID_IAccessible,
                               reinterpret_cast<void **>(&paged_access));
    long page_rows = 0;
    if (paged_access) {
        paged_access->get_accChildCount(&page_rows);
        paged_access->Release();
    }
    Check(page_rows == 3, "overlapping pages deduplicate row IDs");
    auto frozen = view.SnapshotTree();
    Check(frozen.root.children[1].rows.size() == 3 && !frozen.root.children[1].data_source,
          "read-only snapshot exports current dynamic rows without provider declaration");
    ui::ExtensionTreeView frozen_view;
    size_t before_snapshot_requests = requests.size();
    Check(frozen_view.Create(parent, frozen, {}, false, {}, 1), "read-only dynamic snapshot creates with frozen rows");
    frozen_view.SetBounds({0, 0, 500, 700});
    frozen_view.CopySnapshotViewState(view);
    Check(requests.size() == before_snapshot_requests && frozen_view.StateValue("selection") == "row2",
          "snapshot preserves selection without starting another data subscription");
    frozen_view.Close();

    SendMessageW(view.NodeWindow("next"), BM_CLICK, 0, 0);
    auto changed_revision = second;
    changed_revision.revision = "snapshot2";
    completions.back()(changed_revision);
    drain();
    Check(view.StateValue("status") == "error" && view.StateValue("error") == "revision_changed",
          "append rejects a different listing revision with an explicit reason");
    size_t before_invalid_cursor = requests.size();
    SendMessageW(view.NodeWindow("next"), BM_CLICK, 0, 0);
    Check(requests.size() == before_invalid_cursor, "revision mismatch invalidates pagination cursor until refresh");
    auto stale = completions.back();
    view.SetState("filter", "new");
    first.status = "error";
    first.error = "stale";
    stale(first);
    drain();
    Check(view.StateValue("status") == "loading" && view.StateValue("error") == "",
          "superseded completion cannot overwrite new query_node status");
    Check(cancelled >= 2 && requests.back().filter == "new", "new query_node cancels previous subscription");
    RECT input_before_validation{};
    GetWindowRect(view.NodeWindow("form"), &input_before_validation);
    SendMessageW(view.NodeWindow("submit"), BM_CLICK, 0, 0);
    Check(view.StateValue("submitted") == "false", "required validation blocks entire action transaction");
    RECT invalid_input{}, following_submit{};
    GetWindowRect(view.NodeWindow("form"), &invalid_input);
    GetWindowRect(view.NodeWindow("submit"), &following_submit);
    Check(invalid_input.bottom - invalid_input.top == input_before_validation.bottom - input_before_validation.top &&
          following_submit.top - invalid_input.bottom > 12,
          "bounded input preserves readable height and reserves separate validation feedback space");
    view.SetState("form", "ab");
    SendMessageW(view.NodeWindow("form"), WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(view.NodeWindow("submit"), BM_CLICK, 0, 0);
    Check(view.StateValue("submitted") == "false", "IME composition blocks early form submission");
    SendMessageW(view.NodeWindow("form"), WM_IME_ENDCOMPOSITION, 0, 0);
    SendMessageW(view.NodeWindow("submit"), BM_CLICK, 0, 0);
    Check(view.StateValue("submitted") == "true", "valid committed form executes action");
    IAccessible *form_access = nullptr;
    AccessibleObjectFromWindow(view.NodeWindow("form"), static_cast<DWORD>(OBJID_CLIENT), IID_IAccessible,
                               reinterpret_cast<void **>(&form_access));
    VARIANT form_child{};
    form_child.vt = VT_I4;
    form_child.lVal = CHILDID_SELF;
    BSTR form_name = nullptr;
    if (form_access)
        form_access->get_accName(form_child, &form_name);
    Check(form_name && std::wcscmp(form_name, L"Name") == 0, "successful validation restores input accessible name");
    if (form_name)
        SysFreeString(form_name);
    if (form_access)
        form_access->Release();
    HWND query_window = view.NodeWindow("dynamic_query");
    SetFocus(query_window);
    std::swap(dynamic.root.children[0], dynamic.root.children[1]);
    Check(view.UpdateTree(dynamic) && view.NodeWindow("dynamic_query") == query_window && GetFocus() == query_window,
          "tree reorder preserves stable input identity and keyboard focus");
    auto invalid_tree = dynamic;
    invalid_tree.root.children[1].id = invalid_tree.root.children[0].id;
    Check(!view.UpdateTree(invalid_tree) && view.NodeWindow("dynamic_query") == query_window,
          "invalid patch leaves live tree unchanged");
    ui::UiDataPage long_error;
    long_error.status = "error";
    for (int index = 0; index < 600; ++index)
        long_error.error += "\xe8\xb7\xaf";
    completions.back()(long_error);
    drain();
    auto bounded_error = view.StateValue("error").value_or("");
    Check(view.StateValue("status") == "error" && bounded_error.size() <= 1024 && !bounded_error.empty() &&
              MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bounded_error.data(),
                                  static_cast<int>(bounded_error.size()), nullptr, 0) > 0,
          "long Unicode provider error truncates safely without blocking terminal status");
    auto after_close = completions.back();
    int before_close = cancelled;
    view.Close();
    after_close(second);
    drain();
    Check(cancelled > before_close && !view.Hwnd(), "close cancels subscription and late completion is discarded");
    ui::UiDataCompletion capped_completion;
    ui::UiDataPage capped_result;
    ui::ExtensionDataController capped_controller(
        [&](const ui::UiDataRequest &, ui::UiDataCompletion completion) {
            capped_completion = std::move(completion);
            return [] {};
        },
        [] {}, [&](const ui::UiDataPage &page) { capped_result = page; });
    capped_controller.Request({});
    ui::UiDataPage oversized;
    oversized.next_cursor = "more";
    for (int index = 0; index < 4100; ++index)
        oversized.rows.push_back({std::to_string(index), "Row"});
    capped_completion(std::move(oversized));
    capped_controller.Drain();
    Check(capped_result.rows.empty() && capped_controller.NextCursor().empty() &&
              capped_result.status == "error" && capped_result.error == "row_limit_exceeded",
          "dynamic row budget rejects oversized data and invalidates continuation cursor");
    ui::UiDataPage empty_error;
    empty_error.status = "error";
    empty_error.error = "cursor_expired";
    capped_controller.Request({});
    ui::UiDataPage cursor_page;
    cursor_page.rows = {{"one", "One"}};
    cursor_page.next_cursor = "continue";
    capped_completion(cursor_page);
    capped_controller.Drain();
    capped_completion(empty_error);
    capped_controller.Drain();
    Check(capped_controller.NextCursor().empty() && capped_result.status == "error",
          "empty provider error invalidates an existing continuation cursor");
    capped_controller.Close();
    extensions::UiTree flexible;
    flexible.root.id = "flex_root";
    extensions::UiNode row;
    row.id = "flex_row";
    row.orientation = "horizontal";
    extensions::UiNode a;
    a.id = "flex_a";
    a.type = extensions::UiNodeType::Button;
    a.text = "First";
    a.command = "allowed";
    a.min_width = 80;
    a.flex = 1;
    auto b = a;
    b.id = "flex_b";
    b.text = "Second";
    b.flex = 2;
    b.max_width = 240;
    auto c = a;
    c.id = "flex_c";
    c.text = "Fixed";
    c.flex = 0;
    c.min_width = 100;
    c.max_width = 100;
    row.children = {a, b, c};
    extensions::UiNode column;
    column.id = "flex_column";
    column.min_height = 400;
    auto top = a;
    top.id = "flex_top";
    top.min_width = 0;
    top.max_height = 80;
    auto bottom = b;
    bottom.id = "flex_bottom";
    bottom.min_width = 0;
    bottom.max_width = 0;
    column.children = {top, bottom};
    extensions::UiNode limited_scroll;
    limited_scroll.id = "limited_scroll";
    limited_scroll.type = extensions::UiNodeType::ScrollView;
    limited_scroll.min_height = 80;
    limited_scroll.max_height = 100;
    auto tall = a;
    tall.id = "limited_child";
    tall.min_height = 300;
    tall.flex = 0;
    limited_scroll.children = {tall};
    flexible.root.children = {row, column, limited_scroll};
    Check(view.Create(parent, flexible, expanded_callbacks, false, {}, 1), "create constrained real control hierarchy");
    view.SetBounds({0, 0, 600, 700});
    auto node_rect = [&](const char *id) {
        RECT rectangle{};
        GetWindowRect(view.NodeWindow(id), &rectangle);
        MapWindowPoints(nullptr, view.Hwnd(), reinterpret_cast<POINT *>(&rectangle), 2);
        return rectangle;
    };
    auto ar = node_rect("flex_a"), br = node_rect("flex_b"), cr = node_rect("flex_c");
    Check(ar.right - ar.left == 220 && br.right - br.left == 240 && cr.right - cr.left == 100,
          "horizontal flex redistributes remaining width after maximum cap");
    Check(ar.right <= br.left && br.right <= cr.left && cr.right <= 592,
          "constrained horizontal siblings do not overlap or escape viewport");
    auto tr = node_rect("flex_top"), bottom_rect = node_rect("flex_bottom");
    Check(tr.bottom - tr.top == 80 && bottom_rect.bottom - bottom_rect.top == 308,
          "vertical flex uses measured minimum and redistributes capped growth");
    HWND stable_a = view.NodeWindow("flex_a");
    SetFocus(stable_a);
    view.SetBounds({0, 0, 220, 700});
    ar = node_rect("flex_a");
    br = node_rect("flex_b");
    cr = node_rect("flex_c");
    Check(ar.bottom <= br.top && br.bottom <= cr.top && br.right <= 212,
          "minimum widths exceeding narrow viewport fall back to usable vertical flow");
    Check(GetFocus() == stable_a && view.NodeWindow("flex_a") == stable_a,
          "responsive rearrangement preserves native input identity and focus");
    POINT hit{(ar.left + ar.right) / 2, (ar.top + ar.bottom) / 2};
    Check(ChildWindowFromPointEx(view.Hwnd(), hit, CWP_SKIPINVISIBLE) == stable_a,
          "narrow displayed button bounds match actual child hit target");
    size_t flex_commands = commands.size();
    SendMessageW(stable_a, BM_CLICK, 0, 0);
    Check(commands.size() == flex_commands + 1, "responsive control still dispatches authorized command");
    view.SetTheme(true, {}, 1.5f);
    view.SetBounds({0, 0, 900, 1100});
    ar = node_rect("flex_a");
    br = node_rect("flex_b");
    cr = node_rect("flex_c");
    Check(br.right - br.left == 360 && cr.right - cr.left == 150 && ar.right <= br.left,
          "layout constraints scale with DPI and survive theme change");
    view.Close();
    extensions::UiTree short_viewport;
    short_viewport.root.id = "short_viewport";
    short_viewport.root.type = extensions::UiNodeType::ScrollView;
    short_viewport.root.min_height = 2000;
    for (int index = 0; index < 30; ++index) {
        auto entry = a;
        entry.id = "short_" + std::to_string(index);
        entry.flex = 0;
        short_viewport.root.children.push_back(entry);
    }
    Check(view.Create(parent, short_viewport, expanded_callbacks, false, {}, 1),
          "create root scroll view with minimum taller than client");
    view.SetBounds({0, 0, 300, 180});
    Check(view.RevealNode("short_29"), "short root viewport can reveal last content item");
    RECT last_short = node_rect("short_29");
    Check(last_short.top >= 8 && last_short.bottom <= 172,
          "actual client viewport wins over root minHeight and scrolling reaches content bottom");
    view.Close();
    DestroyWindow(parent);
    return failures ? 1 : 0;
}

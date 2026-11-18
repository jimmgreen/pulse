#include "../common/localization.h"
#include "../ui/extension_tree.h"
#include <commctrl.h>
#include <oleacc.h>
#include <cstdio>
#include <string>
using namespace pulse;
namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
extensions::UiNode Node(extensions::UiNodeType type, const char* id, const char* state = "") {
    extensions::UiNode n; n.type = type; n.id = id; n.text = id; n.state = state; return n;
}
void Key(HWND window, WPARAM key) { SendMessageW(window, WM_KEYDOWN, key, 0); }
void Click(HWND window) { SendMessageW(window, BM_CLICK, 0, 0); }
}
int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en");
    HWND parent = CreateWindowExW(0, L"STATIC", L"catalog integration", WS_OVERLAPPEDWINDOW,
        0, 0, 700, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(parent != nullptr, "create parent");
    if (!parent) return 1;
    ui::ExtensionTreeView view;
    extensions::UiTree model;
    model.root = Node(extensions::UiNodeType::Stack, "root");
    model.state = {{"radio","false"},{"step","1"},{"segment","0"},{"toggle","false"},
        {"query",""},{"show","true"},{"enable","true"},{"ratio","50"}};
    model.root.children.push_back(Node(extensions::UiNodeType::Radio,"radio","radio"));
    auto slider = Node(extensions::UiNodeType::Slider,"slider","step"); slider.stops = 5;
    model.root.children.push_back(slider);
    auto segment = Node(extensions::UiNodeType::Segmented,"segment","segment"); segment.options = {"One","Two","Three"};
    model.root.children.push_back(segment);
    auto toggle = Node(extensions::UiNodeType::Button,"toggle","toggle"); toggle.button_kind = "toggle";
    model.root.children.push_back(toggle);
    model.root.children.push_back(Node(extensions::UiNodeType::CommandSearch,"query","query"));
    auto group = Node(extensions::UiNodeType::Group,"conditional");
    group.visible_binding = "state.show"; group.enabled_binding = "state.enable";
    auto action = Node(extensions::UiNodeType::Button,"action"); action.command = "close";
    group.children.push_back(action); model.root.children.push_back(group);
    auto split = Node(extensions::UiNodeType::SplitPane,"split","ratio"); split.orientation = "horizontal";
    split.min_height = 100; split.children = {Node(extensions::UiNodeType::Text,"left"),Node(extensions::UiNodeType::Text,"right")};
    model.root.children.push_back(split);
    int commands = 0;
    ui::ExtensionTreeCallbacks callbacks;
    callbacks.command_enabled = [](const std::string&) { return true; };
    callbacks.on_command = [&](const std::string&) { ++commands; view.Close(); };
    Check(view.Create(parent, model, callbacks, false, {}, 1), "create primitives");
    view.SetBounds({0,0,680,680}); ShowWindow(view.Hwnd(), SW_SHOW);
    Click(view.NodeWindow("radio")); Check(view.StateValue("radio") == "true", "radio click selects");
    Click(view.NodeWindow("radio")); Check(view.StateValue("radio") == "true", "radio repeated click remains selected");
    HWND sh = view.NodeWindow("slider");
    Key(sh,VK_END); Check(view.StateValue("step") == "4", "slider End reaches last stop");
    Key(sh,VK_HOME); Check(view.StateValue("step") == "0", "slider Home reaches first stop");
    RECT bounds{}; GetClientRect(sh,&bounds);
    SendMessageW(sh,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(2,12));
    SendMessageW(sh,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(bounds.right + 100,12));
    SendMessageW(sh,WM_LBUTTONUP,0,MAKELPARAM(bounds.right + 100,12));
    Check(view.StateValue("step") == "4", "slider drag clamps to last stop");
    Key(view.NodeWindow("segment"),VK_END); Check(view.StateValue("segment") == "2", "segmented End updates local selection");
    Click(view.NodeWindow("toggle")); Check(view.StateValue("toggle") == "true", "toggle changes local state");
    SetWindowTextW(view.NodeWindow("query"),L"中文检索");
    Check(view.StateValue("query") == "中文检索", "command search preserves Chinese text");
    Check(view.SetStates({{"step","2"},{"toggle","false"}}), "valid state batch accepted");
    Check(!view.SetState("query", std::string(1, static_cast<char>(0xff))), "runtime state rejects invalid UTF-8 using the same manifest validator");
    Check(view.StateValue("step") == "2" && view.StateValue("toggle") == "false", "valid state batch committed");
    Check(!view.SetStates({{"step","3"},{"toggle","invalid"}}), "invalid state batch rejected");
    Check(view.StateValue("step") == "2" && view.StateValue("toggle") == "false", "invalid state batch rolls back all entries");
    Check(view.SetStates({{"show","false"},{"enable","false"}}), "ancestor visibility state update");
    Check((GetWindowLongPtrW(view.NodeWindow("action"),GWL_STYLE)&WS_VISIBLE)==0, "ancestor hides child window");
    Check(view.SetState("show","true"), "restore ancestor visibility");
    Check(!IsWindowEnabled(view.NodeWindow("action")), "ancestor disables child action");
    Click(view.NodeWindow("action")); Check(commands == 0 && view.Hwnd(), "disabled child cannot invoke command");
    HWND split_hwnd = view.NodeWindow("split");
    Key(split_hwnd,VK_HOME); Check(view.StateValue("ratio") == "0", "split Home clamps at zero");
    Key(split_hwnd,VK_END); Check(view.StateValue("ratio") == "100", "split End clamps at hundred");
    view.SetState("ratio","50"); GetClientRect(split_hwnd,&bounds);
    SendMessageW(split_hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(bounds.right/2,bounds.bottom/2));
    SendMessageW(split_hwnd,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(bounds.right+100,bounds.bottom/2));
    SendMessageW(split_hwnd,WM_LBUTTONUP,0,MAKELPARAM(bounds.right+100,bounds.bottom/2));
    Check(view.StateValue("ratio") == "100", "split drag clamps at hundred");
    view.Close();

    extensions::UiTree local;
    local.root = Node(extensions::UiNodeType::Stack, "local-root");
    local.state = {{"notice", "true"}, {"local-choice", "0"}};
    local.actions = {{"dismiss-local", {{"notice", "false"}, {"local-choice", "1"}}}};
    auto local_button = Node(extensions::UiNodeType::Button, "local-action");
    local_button.command = "dismiss-local";
    auto notice = Node(extensions::UiNodeType::Text, "local-notice");
    notice.visible_binding = "state.notice";
    auto local_select = Node(extensions::UiNodeType::Select, "local-select", "local-choice");
    local_select.options = {"Before", "After"};
    local.root.children = {local_button, notice, local_select};
    Check(view.Create(parent, local, callbacks, false, {}, 1), "create local action fixture");
    view.SetBounds({0, 0, 680, 300});
    Click(view.NodeWindow("local-action"));
    Check(view.StateValue("notice") == "false" && view.StateValue("local-choice") == "1", "local action commits multiple state updates");
    Check(commands == 0 && view.Hwnd(), "local action never invokes host capability callback");
    Check((GetWindowLongPtrW(view.NodeWindow("local-notice"), GWL_STYLE) & WS_VISIBLE) == 0, "local action hides bound notice");
    view.Close();

    for (auto type : {extensions::UiNodeType::VirtualList,extensions::UiNodeType::Tree,extensions::UiNodeType::DataTable}) {
        std::printf("[CASE] dataset type=%s\n", std::string(extensions::UiNodeTypeName(type)).c_str());
        extensions::UiTree data;
        data.root = Node(type,"data","selected"); data.state = {{"selected",""}};
        extensions::UiDataRow first; first.id="first"; first.text="First row"; first.cells={"First row"};
        extensions::UiDataRow second; second.id="second"; second.text="Second row"; second.cells={"Second row"};
        if (type != extensions::UiNodeType::DataTable) { first.cells.clear(); second.cells.clear(); }
        data.root.rows={first,second};
        if (type == extensions::UiNodeType::DataTable) data.root.data_columns.push_back({"name","Name",160});
        Check(view.Create(parent,data,{},false,{},1), "create dataset view"); view.SetBounds({0,0,680,300});
        HWND window = view.NodeWindow("data");
        Key(window,VK_HOME); Check(view.StateValue("selected")=="first", "dataset Home selects first row");
        Key(window,VK_END); Check(view.StateValue("selected")=="second", "dataset End selects last row");
        IAccessible* accessible = nullptr;
        HRESULT hr = AccessibleObjectFromWindow(window,static_cast<DWORD>(OBJID_CLIENT),IID_IAccessible,reinterpret_cast<void**>(&accessible));
        Check(SUCCEEDED(hr) && accessible, "real MSAA provider available");
        if (accessible) {
            long count=0; Check(SUCCEEDED(accessible->get_accChildCount(&count)) && count>=2, "MSAA exposes row children");
            VARIANT child{}; child.vt=VT_I4; child.lVal=1; BSTR name=nullptr;
            hr=accessible->get_accName(child,&name); Check(SUCCEEDED(hr)&&name&&SysStringLen(name)>0, "MSAA row name nonempty"); SysFreeString(name);
            VARIANT selection{}; VariantInit(&selection); hr=accessible->get_accSelection(&selection);
            Check(SUCCEEDED(hr)&&selection.vt!=VT_EMPTY, "MSAA exposes selection"); VariantClear(&selection);
            view.Close(); count=0; hr=accessible->get_accChildCount(&count);
            Check(FAILED(hr), "old MSAA provider disconnects after close"); accessible->Release();
        } else view.Close();
    }
    extensions::UiTree scroll;
    scroll.root=Node(extensions::UiNodeType::ScrollView,"outer");
    auto inner=Node(extensions::UiNodeType::ScrollView,"inner"); inner.min_height=120;
    for(int i=0;i<12;++i) { auto row=Node(extensions::UiNodeType::Button,("scroll-"+std::to_string(i)).c_str()); row.command="close"; inner.children.push_back(row); }
    scroll.root.children.push_back(inner);
    Check(view.Create(parent,scroll,callbacks,false,{},1), "create nested scroll views"); view.SetBounds({0,0,400,200});
    Check(view.RevealNode("scroll-11"), "RevealNode reaches nested last control");
    RECT last{}; GetWindowRect(view.NodeWindow("scroll-11"),&last); MapWindowPoints(nullptr,view.Hwnd(),reinterpret_cast<POINT*>(&last),2);
    Check(last.top>=0 && last.bottom<=200, "revealed nested control lies inside outer viewport");
    RECT before_wheel{}; GetWindowRect(view.NodeWindow("scroll-11"),&before_wheel);
    POINT wheel_point{20,60}; ClientToScreen(view.Hwnd(),&wheel_point);
    SendMessageW(view.Hwnd(),WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),MAKELPARAM(wheel_point.x,wheel_point.y));
    RECT after_wheel{}; GetWindowRect(view.NodeWindow("scroll-11"),&after_wheel);
    Check(after_wheel.top > before_wheel.top, "wheel hit scrolls nested viewport toward first row");
    Check(view.RevealNode("scroll-0"), "reveal after nested wheel remains functional");
    Click(view.NodeWindow("scroll-0")); Check(commands==1 && !view.Hwnd(), "command callback can safely close view");
    DestroyWindow(parent); if(SUCCEEDED(com)) CoUninitialize();
    std::printf("catalog integration failures: %d\n",failures); return failures ? 1 : 0;
}

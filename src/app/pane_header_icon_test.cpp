#include "../ui/shortcut_help.h"
#if defined(PULSE_WITH_SELFTEST) || defined(PULSE_PANE_HEADER_STANDALONE)
#include "../ui/ui_renderer_internal.h"
#include "../ui/pane_header_icons.h"
#include "../common/windows_compat.h"
#include "pane_header_animation.h"
#include "../ui/toolbar_layout.h"
#include "../ui/address_search_layout.h"
#include "context_menu.h"
#include <filesystem>
#include <fstream>

namespace pulse::ui {
struct PaneHeaderIconTest {
    static bool Toolbar(Compositor& compositor, MainRenderer& renderer, std::ofstream& log) {
        bool ok=true;
        auto check=[&](bool passed,const char* name) { log << (passed ? "[PASS] " : "[FAIL] ") << name << '\n'; ok &= passed; };
        app::BackgroundViewOptions options;
        options.sort_column=SortColumn::Size;
        options.indexed_search=true;
        options.show_path=true;
        const auto menu=app::BuildSortMenu(options);
        bool selected=false,restricted=false;
        for (const auto& item:menu) {
            if (item.command==app::CmdSortSize) selected=item.radio;
            if (item.command==app::CmdSortType) restricted=!item.enabled;
        }
        check(selected && restricted,"toolbar sorting preserves selected order and indexed-search restrictions");
        for (float scale:{1.0f,1.5f}) for (bool dark:{false,true}) for (int width:{1200,800,560,320}) {
            compositor.Resize(static_cast<UINT>(width*scale),static_cast<UINT>(230*scale));
            compositor.RecreateTextFormats(scale);
            renderer.SetScale(scale);
            const auto theme=MakeTheme(dark,HexColor(0x8265B2));
            renderer.UpdateBrushes(theme);
            renderer.text_background_=theme.bg;
            renderer.painter_.BeginFrame(theme,false);
            WindowViewModel vm;
            vm.dark=dark;vm.can_go_back=true;vm.pane.can_go_up=true;
            vm.pane.path=L"C:\\Users\\Desktop";vm.pane.header_text=L"Desktop";
            vm.pane.selected_count=1;
            vm.pane_slots.resize(2);
            const auto window=D2D1::RectF(0,0,width*scale,700*scale);
            const float left=renderer.EffectiveSidebarWidth(window.right);
            for (bool expanded:{false,true}) {
                vm.address_searching=width==320 && expanded;
                vm.pane.filter_expand=expanded ? 1.0f : 0.0f;
                vm.pane.filter_text=expanded ? L"report" : L"";
                const auto layout=MakeToolbarLayout(window.right,scale,renderer.TitleBarHeight(),renderer.Margin(),
                    renderer.NewButtonWidthPx(window.right-left < 600*scale),left,vm.pane.filter_expand);
                check(layout.filter.right<=window.right-renderer.Margin(),"toolbar filter stays inside window");
                const auto hit=[&](D2D1_RECT_F rc) { return renderer.HitTest(vm,window,(rc.left+rc.right)*0.5f,(rc.top+rc.bottom)*0.5f).region; };
                if (vm.address_searching) {
                    const auto search=LayoutAddressSearch(renderer.SearchBarRect(window.right),scale);
                    check(search.input.right-search.input.left>=64*scale,"compact search expands to usable input");
                    check(hit(search.input)==HitTestResult::AddressSearchInput && hit(search.close)==HitTestResult::AddressSearchClose,
                        "compact search overlay routes input and close before navigation");
                } else {
                    check(layout.address.right <= layout.navigation[3].left &&
                        layout.navigation[3].right <= layout.search.left,
                        "refresh sits between address and search without overlap");
                    check(hit(layout.navigation[3]) == HitTestResult::NavRefresh,
                        "relocated refresh routes to refresh action");
                    check(layout.address.right+2*scale<=layout.search.left,"address and search do not overlap");
                    check(hit(layout.search)==HitTestResult::AddressSearch,"search entry remains accessible");
                }
                check(hit(layout.sort)==HitTestResult::ToolbarSort,"toolbar sort hit target");
                check(hit(layout.filter)==HitTestResult::FilterBox,"toolbar filter hit target");
                if (layout.overflow.right>layout.overflow.left) {
                    check(layout.filter.right+4*scale<=layout.overflow.left,"compact filter and overflow do not overlap");
                    check(hit(layout.overflow)==HitTestResult::ToolbarMore,"compact overflow remains accessible");
                } else {
                    check(layout.filter.right+4*scale<=layout.commands[6].left,"filter and layout controls do not overlap");
                    check(hit(layout.commands[5])==HitTestResult::Terminal,"terminal hit target");
                    check(hit(layout.commands[6])==HitTestResult::SplitButton,"layout hit target");
                    check(hit(layout.commands[7])==HitTestResult::DetailsToggle,"preview hit target");
                    check(hit(layout.commands[8])==HitTestResult::PaneColumnLayout,"column view has its own toolbar hit target");
                    check(layout.commands[8].right<=window.right-renderer.Margin(),"column view stays inside toolbar");
                }
                if (expanded) {
                    const auto edit=renderer.FilterEditRect(window,1,true);
                    const auto clear=renderer.FilterClearRect(window,1);
                    check(edit.right<clear.left && edit.right-edit.left>=32*scale,"filter text and clear button have separate usable bounds");
                    check(hit(clear)==HitTestResult::FilterClear,"global filter clear hit target");
                }
                auto* dc=compositor.Dc();
                dc->BeginDraw();dc->Clear(theme.bg);
                renderer.DrawTitleBar(vm,window,theme);
                renderer.DrawToolbar(vm,window,theme);
                auto bounds=renderer.ContentRect(window.right,window.bottom);
                vm.pane.header_controls_opacity=1;
                renderer.DrawSinglePane(vm,vm.pane,bounds,0,true,false,theme);
                const bool drawn=SUCCEEDED(dc->EndDraw());
                const auto file=std::filesystem::path(L"bench_data/pane-header-icons") /
                    (L"toolbar-"+std::to_wstring(width)+L"-"+std::to_wstring(static_cast<int>(scale*100))+
                    (dark ? L"-dark" : L"-light")+(expanded ? L"-filter.png" : L".png"));
                check(drawn && compositor.SaveSnapshot(file.c_str()),"toolbar render and capture");
            }
        }
        for (bool chinese:{false,true}) for (bool dark:{false,true}) {
            l10n::SetLanguage(chinese ? L"zh-CN" : L"en-US");
            const auto hints=app::BuildShortcutHints();
            check(hints.children.empty(), "shortcut help opens a dialog instead of a nested menu");
            compositor.Resize(884,945);
            auto* dc=compositor.Dc();
            dc->BeginDraw();dc->Clear(HexColor(0xB5BBC7));
            DrawShortcutHelp(compositor,dark,HexColor(0x6269CD),1,0);
            const bool drawn=SUCCEEDED(dc->EndDraw());
            const auto path=std::filesystem::path(L"bench_data/pane-header-icons") /
                ((chinese ? std::wstring(L"shortcuts-zh") : std::wstring(L"shortcuts-en"))+(dark ? L"-dark.png" : L"-light.png"));
            check(drawn && compositor.SaveSnapshot(path.c_str()),"shortcut hints render in both languages and themes");
        }
        {
            const auto roomy=LayoutShortcutHelp(884,945,1);
            check(roomy.max_scroll==0 && !roomy.narrow &&
                  roomy.card.bottom-roomy.card.top<760 &&
                  roomy.card.right-roomy.card.left<=820,
                  "shortcut help card hugs its content in a normal window");
            const auto narrow=LayoutShortcutHelp(560,900,1);
            check(narrow.narrow && narrow.max_scroll>0,
                  "shortcut help stacks into one scrolling column when narrow");
            const auto short_window=LayoutShortcutHelp(600,360,1.5f);
            check(short_window.max_scroll>0 && short_window.card.top>=0 &&
                  short_window.close.left>short_window.card.left &&
                  short_window.close.right<=short_window.card.right,
                  "shortcut help scrolls when the window is short");
        }
        l10n::SetLanguage(L"zh-CN");
        return ok;
    }
    static bool Run() {
        const std::filesystem::path root = L"bench_data/pane-header-icons";
        std::filesystem::create_directories(root);
        std::ofstream log(root / "results.log");
        bool ok = true;
        auto check = [&](bool passed, const char* name) {
            log << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
            ok &= passed;
        };
        app::PaneHeaderAnimation animation;
        animation.Tick(false,100);
        check(animation.opacity == 0, "inactive pane starts hidden");
        animation.Tick(true,200);
        animation.Tick(true,260);
        check(animation.opacity > 0 && animation.opacity < 1, "focus reveals controls gradually");
        const float interrupted = animation.opacity;
        animation.Tick(false,260);
        check(animation.opacity == interrupted, "rapid focus reversal has no opacity jump");
        animation.Tick(false,320);
        check(animation.opacity > 0 && animation.opacity < interrupted, "reversed fade moves toward hidden");
        animation.Tick(false,440);
        check(animation.opacity == 0 && !animation.Tick(false,500), "fade ends and stops requesting frames");
        animation.Tick(true,600);
        animation.Tick(true,780);
        check(animation.opacity == 1, "focused controls reach full opacity");
        const HWND hwnd = CreateWindowExW(0, L"STATIC", L"Toolbar icon review", WS_POPUP,
            0, 0, 800, 420, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        {
            Compositor compositor;
            if (!hwnd || !compositor.Init(hwnd)) {
                if (hwnd) DestroyWindow(hwnd);
                return false;
            }
            MainRenderer renderer;
            renderer.SetCompositor(&compositor);
            for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
                for (bool dark : {false, true}) {
                    for (int width : {800, 440}) {
                        compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(540 * scale));
                        compositor.RecreateTextFormats(scale);
                        renderer.SetScale(scale);
                        const auto theme = MakeTheme(dark, HexColor(0x0078D4));
                        auto* dc = compositor.Dc();
                        renderer.UpdateBrushes(theme);
                        renderer.text_background_ = theme.bg;
                        renderer.icon_cache_.SetDeviceContext(dc);
                        renderer.painter_.BeginFrame(theme, false);
                        dc->BeginDraw();
                        dc->Clear(theme.bg);
                        for (int i = 0; i < 7; ++i) {
                            renderer.DrawPaneHeaderIcon(D2D1::RectF((20 + 48.0f * i) * scale, 12 * scale,
                                (52 + 48.0f * i) * scale, 44 * scale), static_cast<PaneHeaderIcon>(i), theme.text);
                        }
                        WindowViewModel vm;
                        vm.dark = dark;
                        vm.pane_slots.resize(2);
                        PaneViewModel pane;
                        pane.path = L"C:\\Pictures";
                        pane.header_text = L"Pictures";
                        pane.can_go_forward = pane.can_go_up = true;
                        for (int row = 0; row < 4; ++row) {
                            pane.header_controls_opacity = row == 1 ? 0.45f : row == 2 ? 0.0f : 1.0f;
                            pane.header_text = row == 0 ? L"Focused" : row == 1 ? L"Fading out" : row == 2 ? L"Inactive" : L"Focused filter";
                            pane.filter_expand = row == 3 ? 1.0f : 0.0f;
                            pane.filter_text = row == 3 ? L"image" : L"";
                            pane.view_mode = row == 1 ? ViewMode::MediumIcons : ViewMode::Details;
                            vm.hover_region = row == 1 ? static_cast<int>(HitTestResult::PaneViewButton) : -1;
                            vm.hover_control_index = 0;
                            renderer.DrawSinglePane(vm, pane, D2D1::RectF(8 * scale, (64 + row * 116.0f) * scale,
                                (width - 8.0f) * scale, (174 + row * 116.0f) * scale), 0, row == 0 || row == 3, false, theme);
                        }
                        const bool drawn = SUCCEEDED(dc->EndDraw());
                        const auto file = root / (std::to_wstring(width) + L"-" +
                            std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark.png" : L"-light.png"));
                        const bool saved = drawn && compositor.SaveSnapshot(file.c_str());
                        log << (saved ? "[PASS] " : "[FAIL] ") << width << " scale=" << scale
                            << " dark=" << dark << " render and capture toolbar states\n";
                        ok &= saved;
                        const auto window = D2D1::RectF(0,0,(width+400.0f)*scale,900*scale);
                        auto paneRect = renderer.ContentRect(window.right,window.bottom);
                        paneRect.right = paneRect.left + width*scale;
                        auto& slot = vm.pane_slots[0];
                        slot.rect = paneRect;
                        slot.pane = pane;
                        slot.pane.filter_expand = 0;
                        slot.pane.filter_text.clear();
                        const D2D1_RECT_F buttons[] = {
                            renderer.PaneDetailsRect(paneRect,0), renderer.PaneMediumIconsRect(paneRect,0),
                            renderer.PaneViewButtonRect(paneRect,0)};
                        const HitTestResult::Region actions[] = {HitTestResult::PaneDetails,
                            HitTestResult::PaneMediumIcons,HitTestResult::PaneViewButton};
                        for (bool focused : {false,true}) for (float opacity : {0.0f,0.45f,1.0f}) {
                            slot.focused = focused;
                            slot.pane.header_controls_opacity = opacity;
                            for (size_t i=0;i<std::size(buttons);++i) {
                                const auto& button=buttons[i];
                                const auto hit=renderer.HitTest(vm,window,(button.left+button.right)*0.5f,
                                    (button.top+button.bottom)*0.5f);
                                check(hit.region == (focused && opacity > 0.05f ? actions[i] : HitTestResult::PaneHeader),
                                    "only visible focused header actions accept clicks");
                            }
                        }
                    }
                }
            }
            ok &= Toolbar(compositor,renderer,log);
        }
        DestroyWindow(hwnd);
        return ok;
    }
};
}
namespace pulse::app {
bool RunPaneHeaderIconTest() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return false;
    compat::EnableDpiAwareness();
    const bool ok = ui::PaneHeaderIconTest::Run();
    CoUninitialize();
    return ok;
}
}
#ifdef PULSE_PANE_HEADER_STANDALONE
int wmain() {
    pulse::l10n::Initialize(GetModuleHandleW(nullptr),L"zh-CN");
    return pulse::app::RunPaneHeaderIconTest() ? 0 : 1;
}
#endif
#endif

#include "app_internal.h"
#include "../common/app_runtime_paths.h"
#include "../ui/ui_renderer.h"
#include "app_input.h"
#include "app_ops_ui.h"
#include "global_search_controller.h"
#include "../ui/ui_renderer_internal.h"
#include "../ui/tab_pager_layout.h"
#include <fstream>
#include <filesystem>
#include <string_view>
#include <objbase.h>

namespace pulse { bool TestDialogManagementBackgroundVm(AppState&); }

int RunDialogManagementUiTest(pulse::AppState& s,const wchar_t* output) {
    using namespace pulse;
    using I=l10n::StringId;using H=ui::HitTestResult;
    std::ofstream log{std::filesystem::path(output)};
    int failures=0;
    auto check=[&](bool ok,const char* label) {log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<'\n';if(!ok) ++failures;};
    const auto language=l10n::preference();const float original_scale=s.scale;
    const int original_width=s.compositor.Width(),original_height=s.compositor.Height();
    const auto original_management=s.core.dialogManagement;
    ui::fluent::Painter painter(&s.compositor);
    auto fixture=[&](int variant) {
        auto vm=BuildVm(s,false);vm.settings_open=true;vm.settings_page=11;vm.settings_scroll=0;
        vm.settings_dialog_available=true;vm.settings_dialog_busy=variant==1;
        vm.settings_dialog_enabled=variant==2;vm.settings_dialog_conflict=variant==3;
        vm.settings_dialog_failed=variant==4 || variant==5;vm.settings_dialog_scope_expanded=false;
        vm.settings_dialog_status=l10n::Get(variant==1 ? I::StartupDialogsChecking : variant==2 ? I::StartupDialogsOn : variant==3 ? I::StartupDialogsConflict : variant==4 ? I::StartupDialogsMissing : variant==5 ? I::StartupDialogsPathMismatch : I::StartupDialogsOff);
        vm.settings_integration_enabled=true;vm.settings_integration_state=1;
        return vm;
    };
    bool bounds=true,hits=true,wrapped=true,old_hidden=true;
    bool shortcut_bounds=true,shortcut_hits=true,shortcut_hidden=true;
    for(const auto* locale:{L"zh-CN",L"zh-TW",L"en-US"}) {
        l10n::SetLanguage(locale);painter.InvalidateTypography();
        for(float scale:{1.0f,1.5f,2.0f}) {
            s.compositor.RecreateTextFormats(scale);s.renderer.SetScale(scale);painter.SetScale(scale);
            for(float width:{620.0f,1280.0f}) for(int variant=0;variant<6;++variant) {
                auto vm=fixture(variant);const auto window=D2D1::RectF(0,0,width*scale,1100*scale);
                const auto l=ui::MakeSettingsLayout(vm,window,scale,s.renderer.TitleBarHeight(),28*scale,&painter);
                const D2D1_RECT_F controls[]={l.dialog_toggle_row,l.dialog_retry_button,l.dialog_export_button,l.dialog_scope_button,l.startup_restore_button};
                const int actions[]={10,11,12,13,15};
                for(int i=0;i<5;++i) {
                    const auto r=controls[i];if(r.bottom<=r.top) continue;
                    bounds&=r.right>r.left && r.left>=l.content.left && r.right<=l.content.right;
                    auto shown=vm;shown.settings_scroll=r.top-l.content.top-20*scale;
                    const auto hit=s.renderer.HitTest(shown,window,(r.left+r.right)/2,(r.top+r.bottom)/2-shown.settings_scroll);
                    hits&=(hit.region==H::SettingsDialogManagement && hit.index==actions[i])==(i==3 || !vm.settings_dialog_busy);
                }
                for(int j=0;j<2;++j) {
                    const auto r=j ? l.dialog_jump_auto_row : l.dialog_jump_row;
                    bounds&=r.right>r.left && r.left>=l.content.left && r.right<=l.content.right;
                    auto shown=vm;shown.settings_dialog_jump_bar=true;
                    shown.settings_scroll=r.top-l.content.top-20*scale;
                    const auto hit=s.renderer.HitTest(shown,window,(r.left+r.right)/2,(r.top+r.bottom)/2-shown.settings_scroll);
                    hits&=hit.region==H::SettingsToggle && hit.index==41+j;
                    const auto title=j ? I::DialogJumpAuto : I::DialogJumpBar;
                    const auto desc=j ? I::DialogJumpAutoDesc : I::DialogJumpBarDesc;
                    const float text_width=(std::max)(40*scale,r.right-r.left-126*scale);
                    wrapped&=30*scale+painter.MeasureWrappedBodyHeight(l10n::Get(title),text_width)+painter.MeasureWrappedCaptionHeight(l10n::Get(desc),text_width)<=r.bottom-r.top+0.5f;
                }
                if(l.dialog_management_intro.bottom>l.dialog_management_intro.top) {
                    const auto& info=l.startup_dialog_info;
                    for(int j=0;j<4;++j) {
                        const auto r=info.notes[j];
                        wrapped&=ui::startup_info::BlockHeight(&painter,ui::startup_info::note_titles[j],ui::startup_info::note_bodies[j],r.right-r.left-26*scale,scale)<=r.bottom-r.top+0.5f;
                        bounds&=r.left>=l.content.left && r.right<=l.content.right && r.bottom<=info.bottom;
                    }
                    for(int j=0;j<2;++j) {
                        const auto r=info.scope[j];
                        wrapped&=ui::startup_info::BlockHeight(&painter,ui::startup_info::scope_titles[j],ui::startup_info::scope_bodies[j],r.right-r.left-28*scale,scale)<=r.bottom-r.top+0.5f;
                    }
                }
                bounds&=(l.dialog_scope_button.bottom>l.dialog_scope_button.top)==vm.settings_dialog_enabled;
                bounds&=(l.dialog_application_row.bottom>l.dialog_application_row.top)==(vm.settings_dialog_enabled || vm.settings_dialog_conflict || vm.settings_dialog_failed);
                old_hidden&=l.dialog_bundle_button.bottom==0 && l.dialog_launch_button.bottom==0 && l.dialog_disable_buttons.empty();
                if(variant==0) for(int mode=0;mode<5;++mode) {
                    auto quick=vm;
                    quick.settings_integration_restore_busy=false;
                    quick.settings_integration_experimental=mode<3;
                    quick.settings_integration_folders_compat=mode==4;
                    quick.settings_keep_running=mode==1 || mode==2;
                    quick.settings_launch_on_startup=mode==2;
                    const auto q=ui::MakeSettingsLayout(quick,window,scale,s.renderer.TitleBarHeight(),28*scale,&painter);
                    const auto r=q.integration_hint_action;
                    const bool visible=r.bottom>r.top;
                    shortcut_hidden&=visible==(mode<2 || mode==4);
                    if(!visible) continue;
                    shortcut_bounds&=r.right>r.left && r.left>=q.content.left && r.right<=q.content.right &&
                        r.top>=q.integration_hint_text.bottom && r.bottom<=q.integration_hint.bottom;
                    quick.settings_scroll=r.top-q.content.top-20*scale;
                    const auto h=s.renderer.HitTest(quick,window,(r.left+r.right)/2,(r.top+r.bottom)/2-quick.settings_scroll);
                    shortcut_hits&=h.region==H::SettingsIntegration && h.index==(mode==1 ? 11 : 10);
                }
            }
        }
    }
    check(bounds,"formal controls fit at 620/1280 DIP and 100/150/200 percent");
    check(hits,"formal action hit regions and busy-state disabling match rendering");
    check(wrapped,"scope explanation fits in all three languages");
    check(old_hidden,"bundle/application/launch/session testing controls removed");
    check(TestDialogManagementBackgroundVm(s),"background dialog refresh preserves visible state and does not enter busy layout");
    check(shortcut_bounds,"integration enable shortcuts stay within wide/narrow footer bounds without overlapping text");
    check(shortcut_hits,"integration enable shortcuts hit keep-running 10 then launch-at-sign-in 11");
    check(shortcut_hidden,"integration shortcuts hide when both settings are enabled or runtime integration is disabled");
    const auto base=std::filesystem::path(output).parent_path();
    for(bool dark:{false,true}) for(bool narrow:{false,true}) for(int section=0;section<7;++section) {
        const float scale=narrow ? 1.5f : 1.0f;
        l10n::SetLanguage(narrow ? L"en-US" : L"zh-CN");painter.InvalidateTypography();
        s.compositor.RecreateTextFormats(scale);s.renderer.SetScale(scale);painter.SetScale(scale);
        const int width=static_cast<int>((narrow ? 620 : 1280)*scale),height=static_cast<int>(1000*scale);
        s.compositor.Resize(width,height);
        auto vm=fixture(section==6 ? 5 : section==1 ? 3 : section==2 ? 0 : 2);vm.settings_dialog_scope_expanded=section==4;vm.dark=dark;vm.window_effect=ui::WindowEffect::None;
        vm.background_image.clear();vm.backdrop_enabled=false;vm.wallpaper_look=0;vm.wallpaper_blur=0;
        if(section==0) {vm.settings_integration_experimental=true;vm.settings_keep_running=false;}
        const auto window=D2D1::RectF(0,0,static_cast<float>(width),static_cast<float>(height));
        const auto l=ui::MakeSettingsLayout(vm,window,scale,s.renderer.TitleBarHeight(),28*scale,&painter);
        vm.settings_scroll=section==5 ? l.dialog_jump_row.top-l.content.top-12*scale : section==0 ? 0 : (section==1 || section==4 || section==6 ? l.dialog_management_section.top : section==2 ? l.startup_dialog_info.heading.top : l.section[2].top)-l.content.top-12*scale;
        auto* dc=s.compositor.Dc();dc->BeginDraw();s.renderer.Render(vm,window,ui::MakeTheme(dark,s.accentColor));
        check(dc->EndDraw()==S_OK,"startup integration fixture rendered");
        const auto name=std::wstring(L"startup-")+(dark ? L"dark-" : L"light-")+(narrow ? L"narrow-" : L"wide-")+std::to_wstring(section)+L".png";
        check(s.compositor.SaveSnapshot((base/name).c_str()),"screenshot captured for visual review");
    }
    for(int index:{10,11,12,15}) {H action{};action.region=H::SettingsDialogManagement;action.index=index;if(s.isolatedTest) HandleSettingsControl(s,action);}
    check(s.isolatedTest && s.core.dialogManagement==original_management,"isolated mutating actions cannot initialize controllers or touch configuration");
    l10n::SetLanguage(l10n::LanguageId(language));s.compositor.Resize(original_width,original_height);
    s.compositor.RecreateTextFormats(original_scale);s.renderer.SetScale(original_scale);
    log<<"failures="<<failures<<std::endl;return failures ? 1 : 0;
}


int RunGuardedDialogManagementUiTest(pulse::AppState& state) {
    const bool workspace_test = GetEnvironmentVariableW(L"PULSE_TEST_WORKSPACE_UI", nullptr, 0) > 0;
    struct DirectoryHandle { HANDLE value=INVALID_HANDLE_VALUE; ~DirectoryHandle(){if(value!=INVALID_HANDLE_VALUE) CloseHandle(value);} } held;
    try {
        wchar_t enabled[8]{},output[32768]{},executable[32768]{};
        if(!state.isolatedTest || __argc!=2 || !__wargv[1] || wcscmp(__wargv[1],L"--test-instance")!=0 ||
            GetEnvironmentVariableW(workspace_test ? L"PULSE_TEST_WORKSPACE_UI" : L"PULSE_TEST_DIALOG_MANAGEMENT_UI",enabled,ARRAYSIZE(enabled))!=1 || enabled[0]!=L'1') return 2;
        const DWORD length=GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_FLOW",output,ARRAYSIZE(output));
        const DWORD exe_length=GetModuleFileNameW(nullptr,executable,ARRAYSIZE(executable));
        if(!length || length>=ARRAYSIZE(output) || !exe_length || exe_length>=ARRAYSIZE(executable)) return 2;
        const std::filesystem::path requested(output);
        if(!requested.is_absolute() || requested.filename()!=L"management.log") return 2;
        const auto leaf=requested.parent_path().filename().wstring();
        constexpr std::wstring_view prefix=L"management-ui-";
        if(!leaf.starts_with(prefix) || leaf.size()!=prefix.size()+38) return 2;
        const auto nonce=leaf.substr(prefix.size());GUID guid{};wchar_t formatted[40]{};
        if(FAILED(CLSIDFromString(nonce.c_str(),&guid)) || !StringFromGUID2(guid,formatted,ARRAYSIZE(formatted)) || _wcsicmp(nonce.c_str(),formatted)) return 2;
        auto canonical=[](const std::filesystem::path& path) {
            HANDLE file=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
            if(file==INVALID_HANDLE_VALUE) return std::wstring{};
            std::wstring result(32768,0);
            const DWORD count=GetFinalPathNameByHandleW(file,result.data(),static_cast<DWORD>(result.size()),FILE_NAME_NORMALIZED);
            CloseHandle(file);
            if(!count || count>=result.size()) return std::wstring{};
            result.resize(count);return result;
        };
        const auto bench=canonical(std::filesystem::path(executable).parent_path().parent_path()/L"bench_data");
        const auto supplied_bench=canonical(requested.parent_path().parent_path());
        if(bench.empty() || supplied_bench.empty() || _wcsicmp(bench.c_str(),supplied_bench.c_str())) return 2;
        const auto fixture=std::filesystem::path(bench)/leaf;
        // Only a freshly created direct fixture is allowed; never reuse a directory.
        if(!CreateDirectoryW(fixture.c_str(),nullptr)) return 2;
        held.value=CreateFileW(fixture.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if(held.value==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(held.value,&info) ||
            !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
            canonical(fixture)!=fixture.wstring()) return 2;
        if (workspace_test) {
            extern int RunWorkspaceUiTest(pulse::AppState&, const wchar_t*);
            return RunWorkspaceUiTest(state,(fixture/L"management.log").c_str());
        }
        return RunDialogManagementUiTest(state,(fixture/L"management.log").c_str());
    } catch(...) { return 2; }
}

int RunDoubleCtrlSettingsTest(pulse::AppState& s, const wchar_t* output) {
    using namespace pulse;
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    const auto check = [&](bool okay, const char* label) { log << (okay ? "[PASS] " : "[FAIL] ") << label << std::endl; if (!okay) ++failures; };
    s.appPrefs.persist = false;
    s.appPrefs.search_enabled = s.appPrefs.global_search_enabled = s.appPrefs.global_search_double_ctrl = true;
    ApplyGlobalSearchSettings(s);
    check(s.core.doubleCtrl.Running(), "double Ctrl input watcher starts when enabled");
    OpenSettingsTab(s, 1);
    ui::fluent::Painter painter(&s.compositor);
    const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    const auto layout = [&] { return ui::MakeSettingsLayout(BuildVm(s, false), bounds, s.scale, s.renderer.TitleBarHeight(), 28*s.scale, &painter); };
    for (bool expected : {false, true, false}) {
        s.settings.SetScroll(0, 0);
        const auto initial = layout();
        s.settings.SetScroll(initial.global_search_double_ctrl_row.top - initial.content.top,
            s.renderer.SettingsMaxScroll(BuildVm(s, false), bounds.right, bounds.bottom));
        const auto row = layout().global_search_double_ctrl_row;
        const int x = static_cast<int>(row.right - 36*s.scale), y = static_cast<int>((row.top+row.bottom)/2);
        const auto hit = s.renderer.HitTest(BuildVm(s, false), bounds, static_cast<float>(x), static_cast<float>(y));
        check(hit.region == ui::HitTestResult::SettingsToggle && hit.index == 40, "double Ctrl switch hit targets setting 40");
        SendMessageW(s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        SendMessageW(s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        check(s.appPrefs.global_search_double_ctrl == expected, "mouse click updates double Ctrl preference");
        check(BuildVm(s, false).settings_global_search_double_ctrl == expected, "displayed switch follows double Ctrl preference");
        check(s.core.doubleCtrl.Running() == expected, "input watcher follows actual settings click");
    }
    return failures ? 1 : 0;
}

int RunDialogJumpToggleTest(pulse::AppState& s, const wchar_t* output) {
    using namespace pulse;
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    s.appPrefs.persist = false;
    OpenSettingsTab(s, 11);
    ui::fluent::Painter painter(&s.compositor);
    const auto window = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    auto layout = [&] { return ui::MakeSettingsLayout(BuildVm(s, false), window, s.scale, s.renderer.TitleBarHeight(), 28*s.scale, &painter); };
    auto search_vm=BuildVm(s,false);search_vm.settings_page=1;
    const auto search_layout=ui::MakeSettingsLayout(search_vm,window,s.scale,s.renderer.TitleBarHeight(),28*s.scale,&painter);
    check(search_layout.dialog_jump_row.bottom==0 && search_layout.dialog_jump_auto_row.bottom==0,"jump toggles removed from search settings");
    const bool recent = s.appPrefs.network_recent_enabled, hidden = s.appPrefs.show_hidden_shares;
    auto click = [&](bool automatic) {
        s.settings.SetScroll(0, 0);
        const auto initial = layout();
        const auto row = automatic ? initial.dialog_jump_auto_row : initial.dialog_jump_row;
        s.settings.SetScroll(row.top-initial.content.top, s.renderer.SettingsMaxScroll(BuildVm(s, false), window.right, window.bottom));
        const auto visible = layout();
        const auto target = automatic ? visible.dialog_jump_auto_row : visible.dialog_jump_row;
        const int x = static_cast<int>((target.left+target.right)/2), y = static_cast<int>((target.top+target.bottom)/2);
        const auto hit = s.renderer.HitTest(BuildVm(s, false), window, static_cast<float>(x), static_cast<float>(y));
        check(hit.region == ui::HitTestResult::SettingsToggle && hit.index == (automatic ? 42 : 41), "dialog jump row hits its toggle");
        SendMessageW(s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x,y));
        SendMessageW(s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x,y));
    };
    s.appPrefs.dialog_jump_bar = true;
    s.appPrefs.dialog_jump_auto = false;
    click(false); check(!s.appPrefs.dialog_jump_bar, "mouse disables dialog jump bar");
    {
        auto vm=BuildVm(s,false);vm.settings_scroll=0;
        const auto l=ui::MakeSettingsLayout(vm,window,s.scale,s.renderer.TitleBarHeight(),28*s.scale,&painter);
        const auto r=l.dialog_jump_auto_row;
        vm.settings_scroll=r.top-l.content.top;
        const auto hit=s.renderer.HitTest(vm,window,(r.left+r.right)/2,(r.top+r.bottom)/2-vm.settings_scroll);
        check(hit.region!=ui::HitTestResult::SettingsToggle || hit.index!=42,"automatic jump is inert when jump bar is disabled");
    }
    click(false); check(s.appPrefs.dialog_jump_bar, "mouse enables dialog jump bar");
    click(true); check(s.appPrefs.dialog_jump_auto, "mouse enables automatic jump");
    click(true); check(!s.appPrefs.dialog_jump_auto, "mouse disables automatic jump");
    check(s.appPrefs.network_recent_enabled == recent && s.appPrefs.show_hidden_shares == hidden, "jump toggles preserve network preferences");
    OpenSettingsTab(s, 1);
    s.appPrefs.search_pinyin = true;
    for (bool expected : {false, true}) {
        s.settings.SetScroll(0, 0);
        const auto initial = layout();
        s.settings.SetScroll(initial.search_pinyin_row.top-initial.content.top,
            s.renderer.SettingsMaxScroll(BuildVm(s, false), window.right, window.bottom));
        const auto row = layout().search_pinyin_row;
        const int x = static_cast<int>((row.left+row.right)/2), y = static_cast<int>((row.top+row.bottom)/2);
        const auto hit = s.renderer.HitTest(BuildVm(s, false), window, static_cast<float>(x), static_cast<float>(y));
        check(hit.region == ui::HitTestResult::SettingsToggle && hit.index == 9,
            "filename pinyin row hits its own toggle");
        SendMessageW(s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x,y));
        SendMessageW(s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x,y));
        check(s.appPrefs.search_pinyin == expected && BuildVm(s, false).settings_search_pinyin == expected,
            expected ? "mouse enables filename pinyin and view reflects it" : "mouse disables filename pinyin and view reflects it");
    }
    log.flush();
    return failures;
}

int RunAssistantToggleHitTest(const wchar_t* output) {
    using namespace pulse;
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    for (const auto stage : {ui::AsstStage::Pick, ui::AsstStage::Setup, ui::AsstStage::Connected}) {
        ui::WindowViewModel vm{};
        vm.settings_assistant.stage = stage;
        ui::SettingsLayout layout{};
        layout.assistant.enabled_toggle = D2D1::RectF(100, 20, 142, 44);
        const bool hit = ui::HitTestSettingsAssistant(layout, vm, 121, 32) == ui::kAsstEnabled;
        const bool outside = ui::HitTestSettingsAssistant(layout, vm, 143, 32) != ui::kAsstEnabled;
        log << (hit && outside ? "[PASS] " : "[FAIL] ")
            << "assistant enable toggle hit bounds, stage " << static_cast<int>(stage) << '\n';
        if (!hit || !outside) ++failures;
    }
    return failures ? 1 : 0;
}

#ifdef PULSE_WITH_SELFTEST
namespace pulse { bool TestSettingsFilter(const std::wstring&,l10n::StringId); }
int RunShortcutFlowTest(pulse::AppState& s, const wchar_t* output);
using namespace pulse;
int RunSettingsWidthBorderTest(AppState& s, const wchar_t* output) {
    using H = ui::HitTestResult;
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    check((s.settingsExpanded & ui::kSettingsContextExpandedMask) == 0,
        "third-party context-menu groups start collapsed");
    const float original_scale = s.scale;
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        s.compositor.RecreateTextFormats(scale);
        s.renderer.SetScale(scale);
        for (float width : {760.0f, 1280.0f, 3045.0f}) {
            auto vm = BuildVm(s, false);
            vm.settings_open = true;
            vm.settings_scroll = 0;
            const auto window = D2D1::RectF(0, 0, width*scale, 1464*scale);
            for (int page = 0; page < 5; ++page) {
                vm.settings_page = page;
                const auto l = ui::MakeSettingsLayout(vm, window, scale,
                    s.renderer.TitleBarHeight(), 28*scale, nullptr);
                check(l.content.left == l.nav.right && l.content.right == window.right,
                    "settings use available width at every window size and DPI");
                const auto hit = s.renderer.HitTest(vm, window,
                    (l.nav_row[page].left+l.nav_row[page].right)/2,
                    (l.nav_row[page].top+l.nav_row[page].bottom)/2);
                check(hit.region == H::SettingsNav && hit.index == page,
                    "page navigation matches the resized layout");
            }
        }
    }
    s.compositor.RecreateTextFormats(original_scale);
    s.renderer.SetScale(original_scale);
    OpenSettingsTab(s, 2);
    const auto group = ipc::CtxMenuGroup::Software;
    const bool enabled = s.ctxMenuPrefs.GroupEnabled(group);
    H hit; hit.region = H::SettingsDisclosure; hit.index = 8;
    HandleSettingsControl(s, hit);
    check((s.settingsExpanded & (1u<<8)) != 0, "collapsed group can be expanded");
    HandleSettingsControl(s, hit);
    check((s.settingsExpanded & ui::kSettingsContextExpandedMask) == 0 &&
        s.ctxMenuPrefs.GroupEnabled(group) == enabled, "expansion preserves context-menu preferences");

    // Render the real switch painter at fractional offsets and common DPI scales.
    s.compositor.Resize(640, 420);
    auto* dc = s.compositor.Dc();
    dc->BeginDraw();
    const auto theme = ui::MakeTheme(true, s.accentColor);
    dc->Clear(theme.bg);
    ui::fluent::Painter painter(&s.compositor);
    int row = 0;
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        s.compositor.RecreateTextFormats(scale);
        painter.SetScale(scale);
        painter.BeginFrame(theme);
        const float top = 20.25f + row*94.0f;
        painter.DrawText(std::to_wstring(static_cast<int>(scale*100)) + L"%",
            D2D1::RectF(16, top, 100, top+40), s.compositor.TextFormat(), theme.text);
        for (int state = 0; state < 4; ++state) {
            ui::fluent::ControlState control{};
            control.checked = state==1;
            control.hovered = state==2;
            control.enabled = state!=3;
            painter.DrawSwitch(D2D1::RectF(120.25f+state*126,top,232.0f+state*126.0f,top+40*scale),L"",control);
        }
        ++row;
    }
    check(SUCCEEDED(dc->EndDraw()), "switch states render without Direct2D errors");
    const auto png = std::filesystem::path(output).parent_path()/L"switch-borders.png";
    check(s.compositor.SaveSnapshot(png.c_str()), "switch border DPI comparison captured");
    log << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}

void Render(pulse::AppState&);
int RunSettingsDropdownTest(AppState& s,const wchar_t* output) {
    using H=ui::HitTestResult;
    std::ofstream log{std::filesystem::path(output)};int failures=0;
    auto check=[&](bool ok,const char* label) {log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<std::endl;if(!ok)++failures;};
    const auto base=std::filesystem::path(output).parent_path();
    s.appPrefs.persist=false;
    OpenSettingsTab(s,0);s.settings.SetScroll(0,0);
    s.settings.WindowEffect(ui::WindowEffectId(ui::WindowEffect::None));
    s.settings.Language(L"en-US");
    Render(s);check(s.compositor.SaveSnapshot((base/L"closed.png").c_str()),"closed dropdown screenshot captured");
    for(int index=0;index<2;++index) {
        const auto vm=BuildVm(s,false);
        const auto control=s.renderer.SettingsDropdownBounds(vm,index,
            static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
        const auto hit=s.renderer.HitTest(vm,D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height())),
            control.left+4*s.scale,(control.top+control.bottom)/2);
        check(hit.region==H::SettingsDropdown && hit.index==index,"dropdown hit target matches the rendered control");
        POINT origin{static_cast<LONG>(std::lround(control.left)),static_cast<LONG>(std::lround(control.bottom))};
        ClientToScreen(s.hwnd,&origin);
        static RECT popup{};popup={};
        static int down=0;down=index;
        const auto png=base/(index==0 ? L"effect-menu.png" : L"language-menu.png");
        SetEnvironmentVariableW(L"PULSE_TEST_DROPDOWN_SHOT",png.c_str());
        SetTimer(s.hwnd,0x5347,350,[](HWND hwnd,UINT,UINT_PTR id,DWORD) {
            KillTimer(hwnd,id);
            EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM)->BOOL {
                wchar_t title[64]{};GetWindowTextW(window,title,64);
                if(IsWindowVisible(window) && wcscmp(title,L"PulseMenu")==0) GetWindowRect(window,&popup);
                return TRUE;
            },0);
            for(int i=0;i<down;++i) PostThreadMessageW(GetCurrentThreadId(),WM_KEYDOWN,VK_DOWN,0);
            PostThreadMessageW(GetCurrentThreadId(),WM_KEYDOWN,VK_RETURN,0);
        });
        HandleSettingsControl(s,hit);
        SetEnvironmentVariableW(L"PULSE_TEST_DROPDOWN_SHOT",nullptr);
        log<<"control left="<<origin.x<<" bottom="<<origin.y<<" popup="<<popup.left<<","<<popup.top<<","<<popup.right<<","<<popup.bottom<<std::endl;
        check(popup.left+ui::FluentMenu::kShadowMargin==origin.x,"popup left edge aligns with control instead of click position");
        check(popup.top+ui::FluentMenu::kShadowMargin==origin.y+static_cast<int>(std::lround(4*s.scale)),"popup opens below control with a consistent gap");
        check(popup.right-popup.left-2*ui::FluentMenu::kShadowMargin>=static_cast<int>(std::lround(control.right-control.left)),"popup is at least as wide as the control");
    }
    check(s.appPrefs.language==L"zh-CN","language selection still applies");
    check(s.appPrefs.window_effect==ui::WindowEffectId(ui::WindowEffect::None),"window effect selection still applies");
    Render(s);check(s.compositor.SaveSnapshot((base/L"closed-zh.png").c_str()),"Chinese selected value screenshot captured");
    log<<"failures="<<failures<<std::endl;return failures ? 1 : 0;
}

int RunSearchColumnsTest(AppState& s,const wchar_t* output) {
    using H=ui::HitTestResult;
    std::ofstream log{std::filesystem::path(output)};int failures=0;
    auto check=[&](bool ok,const char* label) {log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<std::endl;if(!ok)++failures;};
    const auto base=std::filesystem::path(output).parent_path();
    check((s.settingsExpanded&2u)!=0,"storage and maintenance starts expanded");
    OpenSettingsTab(s,1);
    H disclosure;disclosure.region=H::SettingsDisclosure;disclosure.index=1;
    HandleSettingsControl(s,disclosure);check(!(s.settingsExpanded&2u),"storage can still collapse");
    HandleSettingsControl(s,disclosure);check((s.settingsExpanded&2u)!=0,"storage can reopen");
    Render(s);check(s.compositor.SaveSnapshot((base/L"storage-expanded.png").c_str()),"default expanded storage screenshot captured");
    // Pre-1.0.39 sessions stored divider ratios; they read back as automatic widths.
    const std::array<float,4> saved{0.31f,0.43f,0.62f,0.82f};
    using K=ui::MainRenderer::ColumnKind;
    for(float scale:{1.0f,1.25f,1.5f,2.0f}) {
        s.renderer.SetScale(scale);
        bool compact=true,positive=true,filled=true,dragged=true,legacy=true;
        for(float width:{500.0f,800.0f,1440.0f,2400.0f}) {
            const auto pane=D2D1::RectF(0,0,width*scale,600*scale);
            const auto automatic=s.renderer.DetailsColumns(pane,{},true,{});
            for(const auto& dividers:{std::array<float,4>{},saved}) {
                const auto c=s.renderer.DetailsColumns(pane,{},true,dividers);
                compact &= c.Width(K::Date)<=176*scale+0.01f && c.Width(K::Type)<=196*scale+0.01f && c.Width(K::Size)<=112*scale+0.01f;
                float sum=0;for(int i=0;i<c.count;++i){positive &= c.widths[i]>0;sum+=c.widths[i];}
                filled &= std::abs(sum-(c.right-c.left))<0.05f;
                legacy &= c.count==automatic.count && std::abs(c.widths[0]-automatic.widths[0])<0.05f;
                if(c.Has(K::Path)) {
                    const auto moved=s.renderer.ResizeSearchColumnDivider(pane,dividers,0,c.DividerX(0)-40*scale);
                    const auto after=s.renderer.DetailsColumns(pane,{},true,moved);
                    dragged &= std::abs(after.widths[1]-c.widths[1]-40*scale)<0.05f;
                }
                if(width>=1440) compact &= c.widths[0]+c.widths[1]>c.Width(K::Date)+c.Width(K::Type)+c.Width(K::Size);
            }
        }
        check(compact,"fitted metadata columns stay compact and leave room for name/path");
        check(positive && filled,"columns stay positive and fill the available width");
        check(dragged,"name/path divider drag keeps its requested width");
        check(legacy,"legacy divider ratios fall back to fitted widths");
    }
    s.renderer.SetScale(s.scale);
    ui::WindowViewModel vm;vm.dark=s.darkMode;vm.window_effect=ui::WindowEffect::None;
    vm.tabs.push_back({L"Search results",true});
    const float width=static_cast<float>(s.compositor.Width()),height=static_cast<float>(s.compositor.Height());
    ui::PaneSlotView slot;slot.rect=D2D1::RectF(220*s.scale,100*s.scale,width-8*s.scale,height-32*s.scale);slot.focused=true;
    slot.pane.header_text=L"Search results";slot.pane.is_search=true;slot.pane.search_query=L"setup";
    slot.pane.search_column_dividers=saved;
    for(int i=0;i<5;++i) {
        ui::ListEntryView row;row.name=L"setup-diagnostic-025d13-project-report-"+std::to_wstring(i)+L".log";
        row.path=L"C:\\Program Files\\Pulse\\Diagnostics\\September\\"+row.name;
        row.type_text=L"Text document";row.date_text=L"2026-09-11 12:50";row.size_text=L"539 B";
        slot.pane.entries.push_back(std::move(row));
    }
    vm.pane_slots.push_back(std::move(slot));
    auto* dc=s.compositor.Dc();dc->BeginDraw();
    s.renderer.Render(vm,D2D1::RectF(0,0,width,height),ui::MakeTheme(s.darkMode,s.accentColor));
    check(SUCCEEDED(dc->EndDraw()),"search result columns render successfully");
    check(s.compositor.SaveSnapshot((base/L"search-columns.png").c_str()),"long name/path and compact metadata screenshot captured");
    log<<"failures="<<failures<<std::endl;return failures ? 1 : 0;
}

int RunAdaptiveColumnsTest(AppState& s, const wchar_t* output) {
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    const auto base = std::filesystem::path(output).parent_path();
    const float original_scale = s.scale;
    if (auto* tab = ActiveTab(s)) {
        const auto saved_path = tab->current_path;
        const auto saved_view = tab->view_mode;
        const auto saved_details = tab->details_column_dividers;
        const auto saved_search = tab->search_column_dividers;
        // Reuse the isolated application's tab; no navigation or disk fixture is needed.
        tab->current_path = L"C:\\";
        tab->view_mode = ui::ViewMode::Details;
        tab->details_column_dividers = {0.48f, 0.70f, 0.86f};
        tab->search_column_dividers = {0.28f, 0.48f, 0.70f, 0.86f};
        const auto vm = BuildVm(s, false);
        const auto window = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
            static_cast<float>(s.compositor.Height()));
        bool exercised = false;
        for (size_t i = 0; i < vm.pane_slots.size(); ++i) {
            if (PaneAtSlot(s, static_cast<int>(i)) != s.pane) continue;
            const auto& slot = vm.pane_slots[i];
            const auto columns = s.renderer.DetailsColumns(slot.rect, slot.pane);
            const auto list = s.renderer.PaneListRect(slot.pane, slot.rect);
            const int x = static_cast<int>(std::lround(columns.DividerX(0)));
            const int y = static_cast<int>(std::lround(list.top - 8 * s.scale));
            const auto hit = s.renderer.HitTest(vm, window, static_cast<float>(x), static_cast<float>(y));
            check(hit.region == ui::HitTestResult::ColumnDivider && hit.pane_index == static_cast<int>(i),
                "double-click fixture hits the active tab's actual column divider");
            if (hit.region != ui::HitTestResult::ColumnDivider) break;
            s.columnResizing = true;
            s.columnResizeIndex = hit.index;
            s.columnResizePane = hit.pane_index;
            SetCapture(s.hwnd);
            check(GetCapture() == s.hwnd, "double-click fixture begins with mouse capture");
            HandleLButtonDblClk(&s, s.hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(x, y));
            check(tab->details_column_dividers[0] == 0.0f &&
                  tab->search_column_dividers == std::array<float, 4>{0.28f, 0.48f, 0.70f, 0.86f},
                "actual divider double-click refits adjacent columns without changing the other layout");
            check(!s.columnResizing && s.columnResizeIndex == -1 && s.columnResizePane == -1 &&
                  GetCapture() != s.hwnd,
                "actual divider double-click stops resizing and releases mouse capture");
            exercised = true;
            break;
        }
        check(exercised, "actual divider double-click regression was exercised");
        if (GetCapture() == s.hwnd) ReleaseCapture();
        s.columnResizing = false;
        s.columnResizeIndex = -1;
        s.columnResizePane = -1;
        tab->current_path = saved_path;
        tab->view_mode = saved_view;
        tab->details_column_dividers = saved_details;
        tab->search_column_dividers = saved_search;
    } else {
        check(false, "isolated application supplies a tab for the double-click regression");
    }
    auto fixture = [](bool search, bool long_type) {
        ui::PaneViewModel pane;
        pane.is_search = search;
        pane.header_text = search ? L"搜索结果 · 自适应列宽" : L"文件夹 · 自适应列宽";
        const wchar_t* types[] = { L"文件", L"应用程序", L"压缩文件",
            L"Microsoft PowerPoint Presentation", L"带有较长中文类型名称的项目归档文件" };
        for (int i = 0; i < (long_type ? 5 : 3); ++i) {
            ui::ListEntryView row;
            row.name = L"2026-项目资料-Quarterly-report-" + std::to_wstring(i) + L".dat";
            row.path = L"C:\\Projects\\季度资料\\Archive\\" + row.name;
            row.type_text = types[i];
            row.date_text = L"2026-09-21 07:44";
            row.size_text = i == 0 ? L"321.9 MB" : (i == 1 ? L"321.9MB" : L"999.99 GB");
            pane.entries.push_back(std::move(row));
        }
        return pane;
    };
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        s.compositor.RecreateTextFormats(scale);
        s.renderer.SetScale(scale);
        auto measure = [&](const std::wstring& value) {
            ui::ComPtr<IDWriteTextLayout> layout;
            const HRESULT hr = s.compositor.DwriteFactory()->CreateTextLayout(value.c_str(),
                static_cast<UINT32>(value.size()), s.compositor.TextFormat(),
                10000.0f * scale, 100.0f * scale, &layout);
            DWRITE_TEXT_METRICS metrics{};
            if (FAILED(hr) || !layout.get() || FAILED(layout->GetMetrics(&metrics))) return 100000.0f;
            float luma = 0.0f;
            s.compositor.MeasureLumaText(value, s.compositor.TextFormat(), luma);
            return std::max(metrics.widthIncludingTrailingWhitespace, luma);
        };
        for (bool search : {false, true}) {
            log << "scale=" << scale << " search=" << search << std::endl;
            for (float width : {280.0f, 480.0f, 600.0f, 800.0f, 1440.0f}) {
                const auto bounds = D2D1::RectF(0, 0, width * scale, 600 * scale);
                auto pane = fixture(search, true);
                const auto columns = s.renderer.DetailsColumns(bounds, pane);
                float sum = 0.0f;
                bool positive = true;
                for (int i = 0; i < columns.count; ++i) {
                    positive &= std::isfinite(columns.widths[i]) && columns.widths[i] > 0;
                    sum += columns.widths[i];
                }
                check(positive && columns.Has(ui::MainRenderer::ColumnKind::Name) &&
                    columns.Has(ui::MainRenderer::ColumnKind::Size),
                    "normal and search columns remain positive at narrow widths and every DPI");
                check(std::abs(sum - (columns.right - columns.left)) < 0.1f,
                    "adaptive columns exactly fill the available pane width");
                if (width >= 600 && columns.Has(ui::MainRenderer::ColumnKind::Date)) {
                    bool compact_fields_fit = true;
                    for (const auto& row : pane.entries) {
                        compact_fields_fit &= measure(row.date_text) + 20 * scale <=
                            columns.Width(ui::MainRenderer::ColumnKind::Date) + 0.1f;
                        compact_fields_fit &= measure(row.size_text) + 20 * scale <=
                            columns.widths[columns.count - 1] + 0.1f;
                    }
                    check(compact_fields_fit, "long type descriptions do not squeeze dates or sizes");
                }
                if (width >= 800 && columns.Has(ui::MainRenderer::ColumnKind::Type)) {
                    bool fits = true;
                    for (const auto& row : pane.entries) {
                        const std::wstring* values[] = { &row.date_text, &row.type_text, &row.size_text };
                        for (int i = 0; i < 3; ++i)
                            fits &= measure(*values[i]) + 20 * scale <=
                                columns.widths[columns.count - 3 + i] + 0.1f;
                    }
                    check(fits, "actual rendered font fits full dates, sizes, and long Chinese/English types");
                }
                const auto short_columns = s.renderer.DetailsColumns(bounds, fixture(search, false));
                if (width >= 800 && columns.Has(ui::MainRenderer::ColumnKind::Type))
                    check(columns.widths[columns.count - 2] > short_columns.widths[columns.count - 2],
                        "long type descriptions expand the type column beyond short labels");
                if (width >= 800 && columns.Has(ui::MainRenderer::ColumnKind::Type)) {
                    auto captured = pane;
                    using K = ui::MainRenderer::ColumnKind;
                    captured.details_column_dividers = {
                        columns.Width(K::Date) / scale, columns.Width(K::Type) / scale,
                        columns.Width(K::Size) / scale};
                    captured.search_column_dividers = {
                        columns.Has(K::Path) ? columns.Width(K::Name) / scale : 0.0f,
                        columns.Width(K::Date) / scale, columns.Width(K::Type) / scale,
                        columns.Width(K::Size) / scale};
                    const auto restored = s.renderer.DetailsColumns(bounds, captured);
                    bool stable = true;
                    for (int i = 0; i < columns.count; ++i)
                        stable &= std::abs(restored.widths[i] - columns.widths[i]) < 0.1f;
                    check(stable, "capturing measured layout as manual DIP widths preserves every column without a jump");
                }
                pane.details_column_dividers = {140.0f, 150.0f, 90.0f};
                pane.search_column_dividers = {200.0f, 140.0f, 150.0f, 90.0f};
                const auto manual = s.renderer.DetailsColumns(bounds, pane);
                const auto expected = s.renderer.DetailsColumns(bounds, pane.details_column_dividers,
                    search, pane.search_column_dividers);
                bool preserved = true;
                for (int i = 0; i < manual.count; ++i)
                    preserved &= std::abs(manual.widths[i] - expected.widths[i]) < 0.01f;
                check(preserved, "explicit manual divider layout overrides content measurement");
            }
        }
        for (bool dark : {false, true}) {
            for (bool narrow : {false, true}) {
                const float width = (narrow ? 760.0f : 1500.0f) * scale;
                const float height = 820.0f * scale;
                s.compositor.Resize(static_cast<UINT>(width), static_cast<UINT>(height));
                ui::WindowViewModel vm;
                vm.dark = dark;
                vm.window_effect = ui::WindowEffect::None;
                vm.tabs.push_back({L"自适应列宽 / Adaptive columns", true});
                for (int i = 0; i < 2; ++i) {
                    ui::PaneSlotView slot;
                    slot.rect = D2D1::RectF(220 * scale, (100 + i * 338) * scale,
                        width - 12 * scale, (420 + i * 338) * scale);
                    slot.focused = i == 0;
                    slot.pane = fixture(i == 1, true);
                    vm.pane_slots.push_back(std::move(slot));
                }
                auto* dc = s.compositor.Dc();
                dc->BeginDraw();
                s.renderer.Render(vm, D2D1::RectF(0, 0, width, height), ui::MakeTheme(dark, s.accentColor));
                check(SUCCEEDED(dc->EndDraw()), "adaptive columns render without Direct2D errors");
                const auto name = std::wstring(L"adaptive-columns-") + (dark ? L"dark-" : L"light-") +
                    (narrow ? L"narrow-" : L"wide-") + std::to_wstring(static_cast<int>(scale * 100)) + L".png";
                check(s.compositor.SaveSnapshot((base / name).c_str()), "adaptive column theme/DPI screenshot captured");
                auto& strip = vm.pane_slots[0].pane.column_strip;
                strip.enabled = strip.eligible = strip.has_child = true;
                auto entries = std::make_shared<std::vector<fs::DirEntry>>(3);
                auto rows = std::make_shared<std::vector<int>>(std::initializer_list<int>{0, 1, 2});
                for (size_t i = 0; i < entries->size(); ++i) {
                    (*entries)[i].name = L"项目文件夹 / Folder " + std::to_wstring(i + 1);
                    (*entries)[i].is_dir = true;
                }
                ui::ColumnStripColumnView column;
                column.title = L"父目录 / Parent";
                column.snapshot = entries;
                column.rows = rows;
                column.highlight_row = 1;
                strip.ancestors = {column, column, column};
                strip.child = column;
                strip.child.title = L"子目录 / Child";
                dc->BeginDraw();
                s.renderer.Render(vm, D2D1::RectF(0, 0, width, height), ui::MakeTheme(dark, s.accentColor));
                check(SUCCEEDED(dc->EndDraw()), "ancestor/current/child columns render across themes and DPI");
                check(s.compositor.SaveSnapshot((base / (L"strip-" + name)).c_str()),
                    "column strip theme/DPI screenshot captured");
            }
        }
    }
    s.compositor.RecreateTextFormats(original_scale);
    s.renderer.SetScale(original_scale);
    log << "failures=" << failures << std::endl;
    return failures ? 1 : 0;
}

#endif
#if defined(PULSE_WITH_SELFTEST) || defined(PULSE_WITH_TEST_HOOKS)
namespace pulse { bool TestSettingsFilter(const std::wstring&, l10n::StringId); }
int RunSettingsIntegrationUiTest(pulse::AppState& s, const wchar_t* output) {
    using namespace pulse;
    using H = ui::HitTestResult;
    using I = l10n::StringId;
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    const auto language = l10n::preference();
    const float original_scale = s.scale;
    const int original_width = s.compositor.Width(), original_height = s.compositor.Height();
    ui::fluent::Painter painter(&s.compositor);
    bool labels = true, separate = true, pill_fits = true, tiles = true, choices_hit = true;
    if (GetEnvironmentVariableW(L"PULSE_TEST_WIN_E_ACTIVATE_ONLY", nullptr, 0)) {
        const auto saved_prefs = s.appPrefs;
        s.appPrefs.persist = false;
        s.appPrefs.integration_enabled = true;
        s.appPrefs.integration_win_e = true;
        l10n::SetLanguage(L"zh-CN");
        for (float scale : {1.0f, 1.5f}) {
            s.renderer.SetScale(scale); s.compositor.RecreateTextFormats(scale); painter.SetScale(scale);
            painter.InvalidateTypography();
            for (int dip_width : {620, 1280}) {
                const int width = static_cast<int>(dip_width * scale), height = static_cast<int>(1100 * scale);
                s.compositor.Resize(width, height);
                auto vm = BuildVm(s, false);
                vm.settings_open = true; vm.settings_page = 11; vm.settings_scroll = 0;
                vm.settings_integration_enabled = vm.settings_integration_win_e = true;
                vm.settings_open_win_e = 2;
                vm.dark = dip_width == 620; vm.window_effect = ui::WindowEffect::None;
                vm.background_image.clear(); vm.backdrop_enabled = false;
                vm.wallpaper_look = 0; vm.wallpaper_blur = 0;
                const auto window = D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height));
                auto layout = ui::MakeSettingsLayout(vm, window, scale, s.renderer.TitleBarHeight(), 28 * scale, &painter);
                vm.settings_scroll = layout.integration_cover[2].top - layout.content.top - 24 * scale;
                layout = ui::MakeSettingsLayout(vm, window, scale, s.renderer.TitleBarHeight(), 28 * scale, &painter);
                bool hit_all = true, fits = true;
                for (int index = 0; index < 4; ++index) {
                    const auto cell = layout.integration_segments[2][index];
                    const auto row = layout.integration_cover[2];
                    fits &= cell.left >= row.left && cell.right <= row.right && cell.top >= row.top && cell.bottom <= row.bottom;
                    const auto hit = s.renderer.HitTest(vm, window, (cell.left + cell.right) / 2, (cell.top + cell.bottom) / 2);
                    hit_all &= hit.region == H::SettingsIntegration && hit.index == 26 + index;
                    if (index == 3 && hit.region == H::SettingsIntegration && hit.index == 29) {
                        s.appPrefs.open_win_e = 0;
                        HandleSettingsControl(s, hit);
                        check(s.appPrefs.open_win_e == 2, "activate click updates Win+E mode without integration registration");
                    }
                }
                check(hit_all, "all four Win+E choices have distinct hit targets");
                check(fits, "four choices fit the wide or wrapped narrow row");
                auto* dc = s.compositor.Dc(); dc->BeginDraw();
                s.renderer.Render(vm, window, ui::MakeTheme(vm.dark, s.accentColor));
                check(SUCCEEDED(dc->EndDraw()), "Win+E fixture renders");
                const auto name = L"win-e-activate-" + std::to_wstring(dip_width) + L"-" + std::to_wstring(static_cast<int>(scale * 100)) + L".png";
                check(s.compositor.SaveSnapshot((std::filesystem::path(output).parent_path() / name).c_str()), "Chinese Win+E screenshot captured");
            }
        }
        s.appPrefs = saved_prefs;
        l10n::SetLanguage(l10n::LanguageId(language));
        s.compositor.Resize(original_width, original_height);
        s.compositor.RecreateTextFormats(original_scale); s.renderer.SetScale(original_scale);
        log << "failures=" << failures << std::endl;
        return failures ? 1 : 0;
    }
    bool extras = true, action_hits = true, bar_fits = true, foot = true, search = true;
    auto nonempty = [](D2D1_RECT_F r) { return r.right > r.left && r.bottom > r.top; };
    auto inside = [](D2D1_RECT_F inner, D2D1_RECT_F outer) {
        return inner.left >= outer.left-0.5f && inner.right <= outer.right+0.5f &&
            inner.top >= outer.top-0.5f && inner.bottom <= outer.bottom+0.5f;
    };
    auto overlap = [](D2D1_RECT_F a, D2D1_RECT_F b) {
        return a.left < b.right-0.5f && b.left < a.right-0.5f && a.top < b.bottom-0.5f && b.top < a.bottom-0.5f;
    };
    // Variants: 0 off, 1 on, 2 changed back, 3 failed, 4 another Pulse copy, 5 just turned on.
    auto fixture = [&](int state) {
        auto vm = BuildVm(s, false);
        vm.settings_open = true; vm.settings_page = 11; vm.settings_scroll = 0;
        vm.settings_expanded = 0;
        vm.settings_integration_state = state == 0 ? 0 : state == 4 ? 2 : state == 5 ? 1 : state;
        vm.settings_integration_enabled = state != 0;
        vm.settings_integration_folders = vm.settings_integration_win_e = vm.settings_integration_this_pc = true;
        // State 1 runs 兼容打开方式: no folder verb, yet the tile still reads as covered.
        vm.settings_integration_folders_compat = state == 1;
        vm.settings_integration_folders_active = state != 0 && state != 1;
        vm.settings_integration_win_e_active = state != 0;
        vm.settings_integration_this_pc_active = state != 0 && state != 2;
        vm.settings_integration_recycle_bin = state == 1;
        vm.settings_integration_experimental = state == 1 || state == 3;
        vm.settings_integration_can_retry = state == 2 || state == 3 || state == 4;
        vm.settings_integration_can_restore = state != 0;
        vm.settings_integration_just_enabled = state == 5;
        vm.settings_integration_other_exe = state == 4 ? L"D:\\Tools\\Pulse\\pulse.exe" : L"";
        vm.settings_launch_on_startup = vm.settings_keep_running = false;
        vm.settings_integration_summary = state == 3 ? std::wstring(l10n::Pick(
            L"没能设置：桌面上的「此电脑」（写入后被改回，可能被安全软件拦截）。详情见 %LOCALAPPDATA%\\Pulse\\logs\\integration.log。",
            L"Could not apply: This PC on the desktop (it was undone right after writing, possibly by security software). Details: %LOCALAPPDATA%\\Pulse\\logs\\integration.log."))
            : std::wstring{};
        return vm;
    };
    for (const auto* locale : {L"zh-CN", L"zh-TW", L"en-US"}) {
        l10n::SetLanguage(locale);
        painter.InvalidateTypography();
        auto missing = [&](int id) {
            const bool found = !l10n::Get(static_cast<I>(id)).empty();
            if (!found) log << "Missing integration resource: locale=" <<
                (std::wstring(locale)==L"zh-CN" ? "zh-CN" : std::wstring(locale)==L"zh-TW" ? "zh-TW" : "en-US") <<
                " id=" << id << '\n';
            labels &= found;
        };
        for (int id = IDS_SETTINGS_INTEGRATION; id <= IDS_INTEGRATION_REAPPLY; ++id) missing(id);
        for (int id = IDS_INTEGRATION_OPTIONS; id <= IDS_INTEGRATION_USE_THIS; ++id) missing(id);
        for (I id : {I::SettingsIntegration, I::SettingsWinE}) {
            const auto& query = l10n::Get(id);
            const bool found = !query.empty() && TestSettingsFilter(query, id);
            if (!found) log << "Missing integration search destination: id=" << static_cast<int>(id) << '\n';
            search &= found;
        }
        for (float scale : {1.0f, 1.5f, 2.0f}) {
            s.compositor.RecreateTextFormats(scale);
            s.renderer.SetScale(scale);
            painter.SetScale(scale);
            for (float width : {620.0f, 760.0f, 1280.0f}) {
                const auto window = D2D1::RectF(0, 0, width*scale, 1000*scale);
                for (int state = 0; state < 6; ++state) {
                    const auto vm = fixture(state);
                    auto hit = [&](D2D1_RECT_F r) {
                        auto shown = vm;
                        shown.settings_scroll = r.top - s.renderer.TitleBarHeight() - 20*scale;
                        return s.renderer.HitTest(shown, window, (r.left+r.right)/2,
                            (r.top+r.bottom)/2 - shown.settings_scroll);
                    };
                    auto is_action = [&](D2D1_RECT_F r, int action) {
                        const auto h = hit(r); return h.region == H::SettingsIntegration && h.index == action;
                    };
                    const auto l = ui::MakeSettingsLayout(vm, window, scale, s.renderer.TitleBarHeight(), 28*scale, &painter);
                    const auto card = l.integration_card;
                    separate &= l.integration_section.bottom <= card.top && card.bottom <= l.dialog_management_section.top &&
                        l.group[2].top == l.startup_row[0].top && l.default_manager_row.top == card.top;
                    // Master row: the pill never collides with the switch and the description fits.
                    const auto m = l.default_manager_row;
                    const auto badge = l.integration_badge;
                    const bool stacked = badge.top >= m.top+35*scale;
                    const float master_h = painter.MeasureWrappedCaptionHeight(l10n::Get(I::IntegrationMasterDesc),
                        l.integration_text_right-m.left-54*scale);
                    pill_fits &= nonempty(badge) && inside(badge, m) && badge.right <= m.right-60*scale+0.5f &&
                        (stacked || badge.left >= l.integration_text_right) &&
                        m.top+35*scale+master_h <= (stacked ? badge.top : m.bottom-12*scale)+0.5f;
                    action_hits &= is_action(m, 0);
                    // Integration choice rows stay inside the card and obey the master switch.
                    for (int i = 0; i < 3; ++i) {
                        const auto t = l.integration_cover[i];
                        tiles &= nonempty(t) && inside(t, card) && t.top >= l.integration_list_head.bottom-0.5f &&
                            t.bottom <= l.integration_options_head.top+0.5f;
                        for (int j = 0; j < i; ++j) tiles &= !overlap(t, l.integration_cover[j]);
                        const auto choice=l.integration_segments[i][0];
                        tiles &= nonempty(choice) && inside(choice,t);
                        const auto track=l.integration_choice[i];
                        tiles &= inside(track,t);
                        for(int cell=0;cell<ui::IntegrationChoiceCount(i);++cell) {
                            const auto segment=l.integration_segments[i][cell];
                            tiles &= nonempty(segment) && inside(segment,track);
                            if(cell>0) tiles &= !overlap(segment,l.integration_segments[i][cell-1]);
                        }
                        const auto choice_hit=hit(choice);
                        choices_hit &= vm.settings_integration_enabled
                            ? choice_hit.region==H::SettingsIntegration && choice_hit.index==20+i*3
                            : choice_hit.region!=H::SettingsIntegration;
                    }
                    // The extras (recycle bin, takeover, 兼容打开方式): their descriptions fit,
                    // they never overlap, and they are only editable while the switch is on.
                    const D2D1_RECT_F rows[] = {l.recycle_bin_row, l.explorer_windows_row, l.folders_compat_row};
                    const I descriptions[] = {I::SettingsRecycleBinDesc, I::IntegrationExperimentalDesc,
                                              I::IntegrationCompatDesc};
                    const int row_actions[] = {7, 4, 9};
                    for (int i = 0; i < 3; ++i) {
                        const float text_left = rows[i].left+112*scale;
                        const float height = painter.MeasureWrappedCaptionHeight(l10n::Get(descriptions[i]),
                            rows[i].right-16*scale-text_left);
                        extras &= nonempty(rows[i]) && inside(rows[i], card) &&
                            rows[i].top+31*scale+height <= rows[i].bottom-12*scale+0.5f;
                        action_hits &= is_action(rows[i], row_actions[i]) == vm.settings_integration_enabled;
                        for (int j = 0; j < i; ++j) extras &= !overlap(rows[i], rows[j]);
                    }
                    const bool problem = state >= 2 && state <= 4;
                    action_hits &= is_action(l.integration_retry, 5) == problem &&
                        is_action(l.integration_restore, 6) == problem;
                    if (problem) {
                        const auto bar = l.integration_bar;
                        const float msg_h = painter.MeasureWrappedCaptionHeight(ui::IntegrationProblemMessage(vm), bar.right-bar.left-60*scale);
                        const auto retry = l.integration_retry, restore = l.integration_restore;
                        bar_fits &= inside(bar, card) && bar.top >= m.bottom && bar.bottom <= l.integration_list_head.top &&
                            bar.top+32*scale+msg_h <= (std::min)(retry.top, restore.top)+0.5f &&
                            inside(retry, bar) && inside(restore, bar) &&
                            (retry.right <= restore.left || retry.bottom <= restore.top);
                        bar_fits &= (state == 4) == (ui::IntegrationRetryLabel(vm) == I::IntegrationUseThis);
                    } else {
                        bar_fits &= !nonempty(l.integration_bar) && !nonempty(l.integration_retry) && !nonempty(l.integration_restore);
                    }
                    // Footer: the try-it button only while on and healthy; the sentence fits.
                    const auto h = l.integration_hint, t = l.integration_hint_text;
                    const bool try_it = vm.settings_integration_enabled && !problem;
                    foot &= nonempty(h) && h.top >= l.folders_compat_row.bottom-0.5f && h.bottom <= card.bottom+0.5f &&
                        inside(t, h) && nonempty(l.integration_try) == try_it && is_action(l.integration_try, 8) == try_it &&
                        t.top+painter.MeasureWrappedCaptionHeight(ui::IntegrationHint(vm), t.right-t.left) <= h.bottom+0.5f;
                    if (try_it) foot &= inside(l.integration_try, h) && !overlap(l.integration_try, t);
                }
            }
        }
    }
    check(labels, "integration resources exist in all three languages");
    check(search, "nonempty localized integration queries find their search destinations");
    check(separate, "integration owns a titled card before startup settings");
    check(pill_fits, "status pill fits beside or below the master text without touching the switch");
    check(tiles, "coverage tiles fit inside the card without overlapping at every width and DPI");
    check(choices_hit, "integration dropdowns follow master enabled state");
    check(extras, "the three extras fit narrow and high-DPI layouts without overlapping");
    check(action_hits, "switch always hits; extras only while on; retry and restore only for problems");
    check(bar_fits, "problem bar and its actions fit; healthy states have no bar or action targets");
    check(foot, "footer sentence fits; try-it appears only while on and healthy");
    // Render fixture view models only: never call integration actions or change registry/preferences.
    const auto base = std::filesystem::path(output).parent_path();
    for (const auto* locale : {L"zh-CN", L"zh-TW", L"en-US"}) {
        l10n::SetLanguage(locale); painter.InvalidateTypography();
        s.compositor.RecreateTextFormats(1.0f); s.renderer.SetScale(1.0f); painter.SetScale(1.0f);
        for (int state = 0; state < 6; ++state) {
            const bool narrow = state == 2 || state == 3;
            const int width = narrow ? 620 : 1280, height = 1100;
            s.compositor.Resize(width, height);
            auto vm = fixture(state);
            vm.dark = state != 0 && state != 2; vm.window_effect = ui::WindowEffect::None;
            vm.background_image.clear();
            vm.backdrop_enabled = false; vm.wallpaper_look = 0; vm.wallpaper_blur = 0;
            const auto window = D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height));
            const auto l = ui::MakeSettingsLayout(vm, window, 1, s.renderer.TitleBarHeight(), 28, &painter);
            vm.settings_scroll = l.integration_section.top-l.content.top-12;
            auto* dc = s.compositor.Dc();
            dc->BeginDraw();
            s.renderer.Render(vm, window, ui::MakeTheme(vm.dark, s.accentColor));
            check(SUCCEEDED(dc->EndDraw()), "integration fixture renders");
            const auto name = L"integration-" + std::wstring(locale) + L"-" + std::to_wstring(state) + L".png";
            check(s.compositor.SaveSnapshot((base/name).c_str()), "integration fixture screenshot captured");
        }
    }
    l10n::SetLanguage(l10n::LanguageId(language));
    s.compositor.Resize(original_width, original_height);
    s.compositor.RecreateTextFormats(original_scale); s.renderer.SetScale(original_scale);
    log << "failures=" << failures << std::endl;
    return failures ? 1 : 0;
}


int RunSettingsSearchTest(pulse::AppState& s,const wchar_t* output) {
        using namespace pulse;
        std::ofstream log{std::filesystem::path(output)};
        int failures = 0;
        auto check = [&](bool ok, const char* label) { log << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl; if (!ok) ++failures; };
        using I = l10n::StringId;
        for (const auto* language : {L"zh-CN", L"zh-TW", L"en-US"}) {
            l10n::SetLanguage(language);
            check(TestSettingsFilter(l10n::Get(I::SettingsTheme)+L"   "+l10n::Get(I::SettingsAppearance), I::SettingsTheme), "AND title and category");
            check(TestSettingsFilter(l10n::Get(I::SettingsThemeDesc), I::SettingsTheme), "translated description");
            check(!TestSettingsFilter(l10n::Get(I::SettingsTheme)+L" no-such-setting-123", I::SettingsTheme), "missing AND term rejected");
            ui::WindowViewModel vm;
            s.settingsSearchQuery = l10n::Get(I::SettingsTheme);
            FillSettingsSearchVm(s, vm);
            check(!vm.settings_search_results.empty(), "results supplied to view model");
            for (float width : {600.0f, 900.0f, 1200.0f}) {
                auto lay = ui::MakeSettingsLayout(vm, D2D1::RectF(0,0,width,800), 1, 40, 24, nullptr);
                check(lay.search_box.bottom <= lay.nav_row[0].top, "search field does not overlap navigation");
                check(lay.search_results.size() == vm.settings_search_results.size(), "each result has shared hit geometry");
                check(lay.search_results.empty() || lay.search_results[0].left >= lay.content.left, "result stays inside content");
            }
        }
        l10n::SetLanguage(L"zh-CN");
        const auto base=std::filesystem::path(output).parent_path();
        for(int scenario=0;scenario<8;++scenario) {
            const float width=scenario==1 || scenario==4 ? 620.0f : 1200.0f;
            const float height=900;
            s.compositor.Resize(static_cast<int>(width),static_cast<int>(height));
            const float fixture_scale=scenario>=6 ? 1.5f : 1.0f;
            s.scale=fixture_scale;
            if(scenario==6) l10n::SetLanguage(L"en-US");
            if(scenario==7) l10n::SetLanguage(L"zh-TW");
            s.compositor.RecreateTextFormats(fixture_scale);s.renderer.SetScale(fixture_scale);
            auto vm=BuildVm(s,false);vm.settings_open=true;vm.settings_page=10;vm.settings_scroll=0;
            vm.settings_vertical_tabs=false;vm.dark=scenario%2!=0;vm.backdrop_enabled=false;
            vm.settings_search_query.clear();vm.settings_search_results.clear();
            vm.tab_max_rows=3;
            vm.tab_layout=scenario>=2 && scenario!=5 ? (scenario==3 ? 2 : 1) : 0;
            if(vm.tab_layout) {
                vm.tabs.clear();
                for(int i=0;i<24;++i) {ui::TabView tab;tab.title=L"项目文件夹 "+std::to_wstring(i+1);tab.active=i==8;tab.pinned=i<2;vm.tabs.push_back(tab);}
            }
            if(scenario<2 || scenario==5) {
                s.settingsSearchQuery=scenario==5 ? L"no-setting-xyz" : L"标签";
                FillSettingsSearchVm(s,vm);
            }
            if(vm.tab_layout) {
                s.renderer.UpdateTabLayout(vm,1600);
                s.renderer.UpdateTabLayout(vm,width);
                D2D1_RECT_F active{};
                check(s.renderer.TabItemRect(vm,width,8,&active) && active.bottom>active.top && active.top<s.renderer.TitleBarHeight(),"resize keeps active tab visible");
                s.renderer.ScrollTabStripPage(vm,width,1);
                D2D1_RECT_F pinned{};s.renderer.TabItemRect(vm,width,0,&pinned);
                check(pinned.top<44*fixture_scale,"paging leaves first row fixed");
                s.renderer.ScrollTabStripPage(vm,width,-1);
            }
            s.renderer.UpdateTabLayout(vm,width);
            check(std::fabs(s.renderer.TitleControlsHeight()-44*fixture_scale)<0.1f,"single and multi row title controls share height");
            if(!vm.tab_layout) check(std::fabs(s.renderer.TitleBarHeight()-44*fixture_scale)<0.1f,"single row title bar uses compact row height");
            if(vm.tab_layout) vm.settings_scroll=s.renderer.SettingsDestinationOffset(vm,static_cast<int>(I::SettingsTabLayout),width,height);
            const auto window=D2D1::RectF(0,0,width,height);
            if (vm.tab_layout) {
                const int rows=static_cast<int>(std::lround(s.renderer.TitleBarHeight()/(44*fixture_scale)));
                const auto pager=ui::MakeTabPagerLayout(width,44*fixture_scale,rows,fixture_scale);
                auto hit_pager=[&](const D2D1_RECT_F& r) {return s.renderer.HitTest(vm,window,(r.left+r.right)/2,(r.top+r.bottom)/2);};
                s.renderer.ScrollTabStripPage(vm,width,-999);
                check(hit_pager(pager.up).region!=ui::HitTestResult::TabScroll,"first page disables previous arrow");
                check(hit_pager(pager.down).region==ui::HitTestResult::TabScroll && hit_pager(pager.down).index==1,"next arrow uses drawn bounds");
                check(hit_pager(pager.label).region!=ui::HitTestResult::TabScroll,"page count does not act as arrow");
                s.renderer.ScrollTabStripPage(vm,width,999);
                check(hit_pager(pager.down).region!=ui::HitTestResult::TabScroll,"last page disables next arrow");
                check(hit_pager(pager.up).region==ui::HitTestResult::TabScroll && hit_pager(pager.up).index==-1,"previous arrow uses drawn bounds");
                s.renderer.ScrollTabStripPage(vm,width,-999);
            }
            auto* dc=s.compositor.Dc();dc->BeginDraw();
            s.renderer.Render(vm,window,ui::MakeTheme(vm.dark,s.accentColor));
            check(SUCCEEDED(dc->EndDraw()),"search/tab fixture renders");
            check(s.compositor.SaveSnapshot((base/(L"tab-settings-"+std::to_wstring(scenario)+L".png")).c_str()),"application screenshot saved");
            if(scenario==3) {
                for(auto& tab:vm.tabs) tab.active=false;
                vm.tabs.back().active=true;vm.dark=false;s.renderer.UpdateTabLayout(vm,width);
                D2D1_RECT_F last{};s.renderer.TabItemRect(vm,width,static_cast<int>(vm.tabs.size()-1),&last);
                check(last.bottom+12*fixture_scale>=s.renderer.TitleBarHeight(),"last active tab joins content edge");
                dc->BeginDraw();s.renderer.Render(vm,window,ui::MakeTheme(vm.dark,s.accentColor));
                check(SUCCEEDED(dc->EndDraw()),"joined last-row fixture renders");
                check(s.compositor.SaveSnapshot((base/L"last-row-joined.png").c_str()),"joined last-row screenshot saved");
            }
            if(scenario==2 || scenario==6) {
                ui::fluent::Painter measured(&s.compositor);measured.SetScale(fixture_scale);
                const auto layout=ui::MakeSettingsLayout(vm,window,fixture_scale,s.renderer.TitleBarHeight(),28*fixture_scale,&measured);
                check(layout.tab_layout_card.top<layout.home_folder_card.top,"tab settings precede window settings");
                check(layout.open_pulse_icon_row[0].right-layout.open_pulse_icon_row[0].left < 150*fixture_scale,"short segment is not stretched");
                for(int shot=0;shot<2;++shot) {
                    vm.settings_scroll=shot==0 ? 0 : s.renderer.SettingsDestinationOffset(vm,static_cast<int>(I::SettingsOpenPulseIcon),width,height);
                    dc->BeginDraw();s.renderer.Render(vm,window,ui::MakeTheme(vm.dark,s.accentColor));
                    check(SUCCEEDED(dc->EndDraw()),"ordered settings and compact segments render");
                    const auto name=(shot==0 ? L"tabs-first-" : L"compact-segments-")+std::to_wstring(scenario)+L".png";
                    check(s.compositor.SaveSnapshot((base/name).c_str()),"settings layout screenshot saved");
                }
            }
            for(size_t i=0;i<vm.tabs.size();++i) {
                D2D1_RECT_F tab{};
                if(s.renderer.TabItemRect(vm,width,static_cast<int>(i),&tab) && tab.right>tab.left && tab.bottom>tab.top) {
                    {
                        check(std::fabs((tab.bottom-tab.top)-40*fixture_scale)<0.1f,"all tab rows have equal tab height");
                        check(std::fabs(std::remainder((tab.top-4*fixture_scale)/fixture_scale,44.0f))<0.1f,"all rows use the same compact pitch");
                    }
                    const auto hit=s.renderer.HitTest(vm,window,(tab.left+tab.right)/2,(tab.top+tab.bottom)/2);
                    check(hit.region==ui::HitTestResult::Tab && hit.index==static_cast<int>(i),"visible tab center hit matches drawn slot");
                }
            }
        }
        OpenSettingsTab(s,10);
        StartSettingsSearch(s);
        check(s.hwndSettingsSearchEdit!=nullptr,"native search editor created");
        SetWindowTextW(s.hwndSettingsSearchEdit,L"多排");
        check(s.settingsSearchQuery==L"多排","edit change updates query");
        SendMessageW(s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1));
        SendMessageW(s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(1, 1));
        check(GetFocus()!=s.hwndSettingsSearchEdit && !IsWindowVisible(s.hwndSettingsSearchEdit),"outside click blurs and closes search editor");
        check(s.settingsSearchQuery==L"多排","outside click preserves search query");
        StartSettingsSearch(s);
        check(GetFocus()==s.hwndSettingsSearchEdit,"search editor can regain focus");
        SendMessageW(s.hwndSettingsSearchEdit, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(4, 4));
        SendMessageW(s.hwndSettingsSearchEdit, WM_LBUTTONUP, 0, MAKELPARAM(4, 4));
        check(GetFocus()==s.hwndSettingsSearchEdit,"inside click keeps search focus");
        SendMessageW(s.hwndSettingsSearchEdit,WM_KEYDOWN,VK_DOWN,0);
        check(s.settingsSearchSelected==1,"down selects next result");
        SendMessageW(s.hwndSettingsSearchEdit,WM_KEYDOWN,VK_RETURN,0);
        check(s.settingsSearchQuery.empty() && !IsWindowVisible(s.hwndSettingsSearchEdit),"enter opens and clears search");
        check(s.settingsSearchTarget!=0,"result target is highlighted");
        StartSettingsSearch(s);SetWindowTextW(s.hwndSettingsSearchEdit,L"missing");
        ui::HitTestResult clear_hit;clear_hit.region=ui::HitTestResult::SettingsSearchClear;
        HandleSettingsControl(s,clear_hit);
        check(s.settingsSearchQuery.empty() && GetFocus()==s.hwndSettingsSearchEdit,"clear search returns focus to shared search field");
        SetWindowTextW(s.hwndSettingsSearchEdit,L"missing");
        SendMessageW(s.hwndSettingsSearchEdit,WM_KEYDOWN,VK_ESCAPE,0);
        check(s.settingsSearchQuery.empty() && !IsWindowVisible(s.hwndSettingsSearchEdit),"escape clears and closes editor");
        s.settingsSearchQuery.clear();
        l10n::SetLanguage(s.appPrefs.language);
        log << "failures=" << failures << std::endl;
        return failures ? 1 : 0;
}
int RunStableEntryRegistrationTest(const wchar_t* output) {
    using namespace pulse;

        std::ofstream log{std::filesystem::path(output)};
        int failures=0;
        auto check=[&](bool ok,const char* label){log<<(ok?"[PASS] ":"[FAIL] ")<<label<<'\n';if(!ok)++failures;};
        const auto key_name=L"Software\\PulseStableEntryTest_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64());
        HKEY isolated=nullptr;
        const auto created=RegCreateKeyExW(HKEY_CURRENT_USER,key_name.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&isolated,nullptr);
        check(created==ERROR_SUCCESS,"create isolated registry fixture");
        if(created!=ERROR_SUCCESS)return 1;
        const auto redirected=RegOverridePredefKey(HKEY_CURRENT_USER,isolated);
        check(redirected==ERROR_SUCCESS,"redirect process registry into fixture");
        if(redirected==ERROR_SUCCESS) {
            app::AppPrefs prefs;
            prefs.persist=true;
            HKEY fixture_run=nullptr;
            const auto run_created=RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_ALL_ACCESS,nullptr,&fixture_run,nullptr);
            check(run_created==ERROR_SUCCESS,"create isolated startup key");
            if(fixture_run)RegCloseKey(fixture_run);
            check(prefs.ApplyLaunchOnStartup(true),"write actual startup registration");
            HKEY run=nullptr;wchar_t value[32768]{};DWORD bytes=sizeof(value),type=0;
            const auto opened=RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_QUERY_VALUE,&run);
            const bool read=opened==ERROR_SUCCESS && RegQueryValueExW(run,L"Pulse",nullptr,&type,reinterpret_cast<BYTE*>(value),&bytes)==ERROR_SUCCESS;
            if(run)RegCloseKey(run);
            const auto stable=app_runtime_paths::StableEntryPath();
            check(read && type==REG_SZ && std::wstring(value).starts_with(L"\""+stable+L"\""),"startup command uses stable entry");
            const auto runtime=app_runtime_paths::RuntimeDirectory();
            const bool versioned=std::filesystem::path(runtime).parent_path().filename()==L"DialogBridge";
            check(versioned && std::filesystem::path(stable).parent_path()==std::filesystem::path(runtime).parent_path().parent_path(),"test runs inside versioned runtime and registers installation root");
            check(prefs.ApplyLaunchOnStartup(false),"remove isolated startup registration");
            RegOverridePredefKey(HKEY_CURRENT_USER,nullptr);
        }
        RegCloseKey(isolated);RegDeleteTreeW(HKEY_CURRENT_USER,key_name.c_str());
        log<<"failures="<<failures<<std::endl;return failures?1:0;
}

#endif
#ifdef PULSE_WITH_SELFTEST
int RunSettingsFlowTest(AppState& s,const wchar_t* output) {
    if (GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_SEARCH", nullptr, 0)) return RunSettingsSearchTest(s, output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_ASSISTANT_TOGGLE_HIT", nullptr, 0))
        return RunAssistantToggleHitTest(output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_DIALOG_JUMP_TOGGLES", nullptr, 0))
        return RunDialogJumpToggleTest(s, output);
    if(GetEnvironmentVariableW(L"PULSE_TEST_DIALOG_MANAGEMENT_UI",nullptr,0))
        return RunDialogManagementUiTest(s,output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_INTEGRATION", nullptr, 0) ||
        GetEnvironmentVariableW(L"PULSE_TEST_WIN_E_ACTIVATE_ONLY", nullptr, 0))
        return RunSettingsIntegrationUiTest(s, output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_ADAPTIVE_COLUMNS", nullptr, 0))
        return RunAdaptiveColumnsTest(s, output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_SEARCH_COLUMNS", nullptr, 0))
        return RunSearchColumnsTest(s, output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_DROPDOWN", nullptr, 0))
        return RunSettingsDropdownTest(s, output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_WIDTH_BORDER", nullptr, 0))
        return RunSettingsWidthBorderTest(s, output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_SHORTCUTS", nullptr, 0))
        return RunShortcutFlowTest(s, output);
    using H=ui::HitTestResult;using I=l10n::StringId;
    std::ofstream log{std::filesystem::path(output)};int failures=0;
    auto check=[&](bool ok,const char* label){log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<'\n';if(!ok)++failures;};
    s.appPrefs.persist=false;
    app::AppPrefs prefs; prefs.persist=false;
    const auto color_close=[](float a,float b) { return std::fabs(a-b)<0.001f; };
    auto pine=ResolveAccentColor(prefs,false);
    check(color_close(pine.r,82/255.0f) && color_close(pine.g,125/255.0f) && color_close(pine.b,112/255.0f),
        "light theme defaults to concept pine green");
    const auto charcoal=ui::MakeTheme(true,pine);
    check(color_close(charcoal.bg.r,26/255.0f) && color_close(charcoal.bg.r,charcoal.bg.g) &&
        color_close(charcoal.surface_sheet.r,charcoal.surface_sheet.b),"dark theme restores neutral charcoal backgrounds");
    const auto warm=ui::MakeTheme(false,pine);
    check(color_close(warm.bg.r,243/255.0f) && color_close(warm.bg.g,245/255.0f) && color_close(warm.bg.b,241/255.0f),
        "light theme uses concept warm canvas");
    prefs.accent_follow_system=true;
    app::AppPrefs accent_roundtrip; accent_roundtrip.persist=false;
    check(accent_roundtrip.FromJson(prefs.ToJson()) && accent_roundtrip.accent_follow_system,
        "explicit Windows accent survives preference round trip");
    prefs.accent_rgb=L"8861AA";
    auto custom=ResolveAccentColor(prefs,false);
    check(color_close(custom.r,136/255.0f) && color_close(custom.b,170/255.0f),"custom accent overrides theme palette");
    prefs.FromJson(L"{\"language\":\"en-US\"}");check(prefs.theme_mode==-1,"old profiles retain session theme");
    for(int mode=0;mode<3;++mode) {
        prefs.theme_mode=mode;const auto json=prefs.ToJson();app::AppPrefs loaded;loaded.persist=false;
        check(loaded.FromJson(json) && loaded.theme_mode==mode,"theme preference round trip");
    }
    prefs.FromJson(L"{\"theme_mode\":99}");check(prefs.theme_mode==-1,"invalid theme preference has legacy fallback");
    for(const auto* language:{L"zh-CN",L"en-US"}) {
        l10n::SetLanguage(language);bool complete=true;
        for(int id=1900;id<=1929;++id) complete &= !l10n::Get(static_cast<I>(id)).empty();
        check(complete,"new settings labels exist in both languages");
        check(TestSettingsFilter(l10n::Get(I::SettingsWallpaper),I::SettingsWallpaper),"settings search finds hidden advanced settings");
        check(TestSettingsFilter(l10n::Get(I::SettingsSearchIndex),I::IndexLocation),"settings search finds page and subsettings");
        check(!TestSettingsFilter(L"no-such-setting-123",I::SettingsTheme),"settings search has empty results");
        check(TestSettingsFilter(l10n::Get(I::SettingsTheme)+L"   "+l10n::Get(I::SettingsAppearance),I::SettingsTheme),"settings search AND matches title and category across whitespace");
        check(TestSettingsFilter(l10n::Get(I::SettingsThemeDesc),I::SettingsTheme),"settings search matches translated descriptions");
        check(!TestSettingsFilter(l10n::Get(I::SettingsTheme)+L" no-such-setting-123",I::SettingsTheme),"settings search rejects a missing AND term");
        bool apps_text=true;
        for(I id:{I::AboutMoreApps,I::AboutMoreAppsDesc,I::AboutLumenPdfDesc,I::AboutLumaShotDesc}) apps_text &= !l10n::Get(id).empty();
        check(apps_text,"recommended apps texts exist in both languages");
    }
    l10n::SetLanguage(L"zh-CN");
    const float original_scale=s.scale;
    ui::fluent::Painter painter(&s.compositor);
    for(float scale:{1.0f,1.5f,2.0f}) {
        s.compositor.RecreateTextFormats(scale);s.renderer.SetScale(scale);painter.SetScale(scale);
        for(float width:{720.0f,820.0f,1280.0f,1920.0f}) {
            const auto window=D2D1::RectF(0,0,width*scale,1000*scale);
            auto vm=BuildVm(s,false);vm.settings_open=true;vm.settings_scroll=0;vm.settings_bloom=&s.bloom_accent;
            vm.settings_content_folders={{LR"(C:\Projects\Very long project folder name)",L"Ready",false},{LR"(D:\Unavailable)",L"Unavailable (3)",true}};
            for(int page=0;page<5;++page) {
                vm.settings_page=page;vm.settings_expanded=0;
                const auto layout=ui::MakeSettingsLayout(vm,window,scale,s.renderer.TitleBarHeight(),28*scale,&painter);
                auto hit=[&](D2D1_RECT_F r){return s.renderer.HitTest(vm,window,(r.left+r.right)/2,(r.top+r.bottom)/2);};
                bool nav=true;for(int i=0;i<5;++i) {auto h=hit(layout.nav_row[i]);nav &= h.region==H::SettingsNav && h.index==i;}
                check(nav,"fixed navigation preserves all page destinations across widths and DPI");
                check(layout.nav_row[0].top <= layout.nav.top + 48*scale,
                    "settings navigation starts without a search-box gap");
                check(layout.nav_row[4].bottom<layout.nav_row[3].top,"about is pinned below primary navigation");
                if(page==0) {
                    check(hit(layout.theme_tile[0]).region==H::SettingsTheme && hit(layout.theme_tile[0]).index==1,"light theme preview hit target");
                    check(hit(layout.accent_picker).region==H::SettingsAccent,"original color wheel remains interactive");
                    check(layout.effect_choice.right == 0 && hit(layout.language_choice).index==1,"window effect has no duplicate dropdown; language dropdown remains");
                    auto scrolled=vm;
                    scrolled.settings_scroll=layout.density_card.top-layout.content.top;
                    const auto density_layout=ui::MakeSettingsLayout(scrolled,window,scale,s.renderer.TitleBarHeight(),28*scale,&painter);
                    const auto density=density_layout.density_row[2];
                    check(s.renderer.HitTest(scrolled,window,(density.left+density.right)/2,(density.top+density.bottom)/2).region==H::SettingsDensity,"density segments remain reachable after scrolling into view");
                    check(layout.wallpaper_card.bottom==0,"collapsed advanced settings have no invisible hit targets");
                    check(layout.integration_cover[0].bottom>layout.integration_cover[0].top && layout.explorer_windows_row.bottom>layout.explorer_windows_row.top,"default file manager choices are visible without expanding");
                    {
                        const auto m=layout.default_manager_row;const auto mh=hit(m);
                        check(mh.region==H::SettingsIntegration && mh.index==0 &&
                              m.top==layout.integration_card.top && layout.integration_card.bottom<=layout.section[1].top &&
                              layout.group[1].top==layout.startup_row[0].top,
                              "integration master switch lives in its own card before startup settings");
                    }
                    for(const auto* locale:{L"zh-CN",L"en-US"}) {
                        l10n::SetLanguage(locale);bool fits=true;
                        const I labels[]={I::SettingsDensityCompact,I::SettingsDensityStandard,I::SettingsDensityRoomy,I::SettingsTraySmall,I::SettingsTrayStandard,I::SettingsTrayLarge};
                        for(I id:labels) {
                            const auto& label=l10n::Get(id);ui::ComPtr<IDWriteTextLayout> measured;
                            s.compositor.DwriteFactory()->CreateTextLayout(label.c_str(),static_cast<UINT32>(label.size()),s.compositor.TextFormat(),2000*scale,100*scale,&measured);
                            DWRITE_TEXT_METRICS metrics{};if(measured.get()) measured->GetMetrics(&metrics);
                            fits &= measured.get() && metrics.widthIncludingTrailingWhitespace<=layout.density_row[0].right-layout.density_row[0].left-6*scale;
                        }
                        check(fits,"density and tray labels fit their segments in both languages");
                    }
                    l10n::SetLanguage(L"zh-CN");
                }
                if(page==2) {
                    // No pivot: the 系统右键菜单 layer card and the hint row in the
                    // left card both open page 6; the Pulse card is the page itself.
                    auto layer=hit(layout.ctx_layer[1]),hint=hit(layout.context_system_hint),own=hit(layout.ctx_layer[0]);
                    check(layer.region==H::SettingsCtxPivot && layer.index==6 &&
                          hint.region==H::SettingsNav && hint.index==6 &&
                          own.region!=H::SettingsCtxPivot && own.region!=H::SettingsNav &&
                          layout.ctx_layer[0].top>=layout.ctx_intro.bottom && layout.pulse_card.top>=layout.ctx_layer[0].bottom &&
                          layout.context_system_hint.top>=layout.pulse_tab[2].bottom &&
                          layout.context_system_hint.bottom<=layout.pulse_tabs.bottom &&
                          layout.pulse_tabs.right<=layout.pulse_card.left && layout.pulse_tabs.top==layout.pulse_card.top,
                          "context page: layer cards above the columns, hint in the left card, both open the system page");
                    bool hidden=true;for(const auto& r:layout.context_rows) hidden &= r.bottom==0;
                    check(hidden,"collapsed context entries do not receive clicks");
                }
                if(page==3) {
                    bool apps=true;
                    for(int i=0;i<2;++i) {auto h=hit(layout.apps_row[i]);apps &= h.region==H::SettingsAboutAction && h.index==3+i;}
                    check(apps,"recommended app rows open their own project pages");
                    check(layout.apps_card.top>=layout.about_card.bottom && layout.apps_card.bottom<=layout.diagnostics_card.top &&
                          layout.apps_row[0].top>=layout.apps_card.top && layout.apps_row[0].bottom<=layout.apps_row[1].top &&
                          layout.apps_row[1].bottom<=layout.apps_card.bottom,
                          "recommended apps card sits between about and diagnostics without overlap");
                }
                if(page==1) {
                    check(layout.index_action[0].bottom==0 && layout.index_volume_rows.empty(),"collapsed maintenance has no invisible hit targets");
                    check(hit(layout.disclosure[1]).region==H::SettingsDisclosure,"maintenance disclosure hit target");
                    check(hit(layout.content_header).region!=H::SettingsContentAction,"shared scope header has no maintenance hit target");
                    check(hit(layout.content_options).region==H::SettingsContentAction && hit(layout.content_options).index==2,"shared text encoding hit target");
                    check(layout.content_rebuild.bottom<=layout.group[1].bottom && layout.section[2].bottom==0,"rebuild lives inside content card without a redundant more-options section");
                }
            }
        }
    }
    s.compositor.RecreateTextFormats(original_scale);s.renderer.SetScale(original_scale);painter.SetScale(original_scale);
    OpenSettingsTab(s,0);s.settingsExpanded=0;s.settings.SetScroll(0,0);
    auto window=D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
    auto layout=[&] {return ui::MakeSettingsLayout(BuildVm(s,false),window,s.scale,s.renderer.TitleBarHeight(),28*s.scale,&painter);};
    auto click=[&](D2D1_RECT_F r) {
        const int x=static_cast<int>((r.left+r.right)/2),y=static_cast<int>((r.top+r.bottom)/2);
        SendMessageW(s.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        SendMessageW(s.hwnd,WM_LBUTTONUP,0,MAKELPARAM(x,y));
    };
    click(layout().theme_tile[0]);check(!s.darkMode && s.appPrefs.theme_mode==1,"mouse click applies and saves light theme");
    click(layout().theme_tile[1]);check(s.darkMode && s.appPrefs.theme_mode==2,"mouse click applies and saves dark theme");
    click(layout().theme_tile[2]);check(s.themeOverride==ui::ThemeMode::Auto && s.appPrefs.theme_mode==0,"system theme is a persistent explicit selection");
    OpenSettingsTab(s,9);s.settings.SetScroll(0,0);
    s.settings.SetScroll(layout().density_card.top-layout().content.top,
        s.renderer.SettingsMaxScroll(BuildVm(s,false),window.right,window.bottom));
    click(layout().density_row[0]);check(s.appPrefs.row_height==28,"mouse click changes density through existing controller");
    // Layout rectangles are scroll-relative: measure from the top so the target does not depend on the density step.
    OpenSettingsTab(s,10);s.settings.SetScroll(0.0f,0.0f);
    s.settings.SetScroll(layout().close_last_tab_row.top-layout().content.top,
        s.renderer.SettingsMaxScroll(BuildVm(s,false),window.right,window.bottom));
    {
        const auto tab_layout=layout();
        check(tab_layout.close_last_tab_row.top>=tab_layout.new_tab_open_card.bottom &&
              tab_layout.close_last_tab_row.bottom<=tab_layout.group[1].bottom,
              "close-with-last-tab row sits in the tabs card");
        const bool was=s.appPrefs.close_window_with_last_tab;
        click(tab_layout.close_last_tab_row);
        const bool flipped=s.appPrefs.close_window_with_last_tab!=was;
        click(layout().close_last_tab_row);
        check(flipped && s.appPrefs.close_window_with_last_tab==was,
              "mouse click toggles closing the window with the last tab");
    }
    OpenSettingsTab(s,12);s.settings.SetScroll(0.0f,0.0f);
    s.settings.SetScroll(layout().confirm_delete_row.top-layout().content.top,
        s.renderer.SettingsMaxScroll(BuildVm(s,false),window.right,window.bottom));
    {
        const auto list_layout=layout();
        check(list_layout.confirm_delete_row.top>=list_layout.folder_sort_card.bottom &&
              list_layout.confirm_delete_row.bottom<=list_layout.group[0].bottom,
              "confirm-before-delete row sits in the delete card");
        const bool was=s.appPrefs.confirm_recycle_delete;
        click(list_layout.confirm_delete_row);
        const bool flipped=s.appPrefs.confirm_recycle_delete!=was;
        click(layout().confirm_delete_row);
        check(flipped && s.appPrefs.confirm_recycle_delete==was,
              "mouse click toggles confirming before deleting");
        const auto one=BuildRecycleDeleteConfirm({L"C:\\fx\\a.txt"});
        const auto many=BuildRecycleDeleteConfirm({L"C:\\fx\\a.txt",L"C:\\fx\\b.txt",L"C:\\fx\\c.txt"});
        check(!one.danger && one.items.size()==1 &&
              one.message==l10n::Get(l10n::StringId::RecycleConfirmOne) &&
              many.items.size()==3 && many.message.find(L"3")!=std::wstring::npos &&
              !many.confirm_text.empty() && !many.danger,
              "recycle delete confirmation lists the items without the danger style");
    }
    OpenSettingsTab(s,0);s.settingsExpanded&=~32u;s.settings.SetScroll(0,0);
    auto vm=BuildVm(s,false);
    const auto collapsed_max=s.renderer.SettingsMaxScroll(vm,window.right,window.bottom);
    H toggle;toggle.region=H::SettingsDisclosure;toggle.index=0;
    H accent_custom;accent_custom.region=H::SettingsAccentCustom;accent_custom.index=0;HandleSettingsControl(s,accent_custom);
    vm=BuildVm(s,false);const auto expanded_max=s.renderer.SettingsMaxScroll(vm,window.right,window.bottom);
    check((s.settingsExpanded&32u) && expanded_max>collapsed_max,"opening the custom accent updates scroll range");
    {
        // 文件列表 › 显示哪些项目 owns both the hidden-files switch and the protected
        // operating system files switch; both need a reachable row and fitting text.
        OpenSettingsTab(s,9);s.settings.SetScroll(0,0);
        auto advanced=BuildVm(s,false);advanced.settings_scroll=0;
        const auto lay=ui::MakeSettingsLayout(advanced,window,s.scale,s.renderer.TitleBarHeight(),28*s.scale,&painter);
        check(lay.protected_files_row.top>=lay.hidden_files_row.bottom &&
            lay.protected_files_row.bottom-lay.protected_files_row.top>=64*s.scale-1.0f &&
            lay.protected_files_row.bottom<=lay.footer.top,
            "protected system files row sits between the hidden row and the footer");
        const float text_width=lay.hidden_files_row.right-lay.hidden_files_row.left-126*s.scale;
        for(const auto* locale:{L"zh-CN",L"en-US"}) {
            l10n::SetLanguage(locale);bool fits=true;
            // An out-of-range id resolves to an empty string, which would also fit.
            check(!l10n::Get(I::SettingsShowProtected).empty() &&
                !l10n::Get(I::SettingsShowProtectedDesc).empty(),
                "protected system files labels exist in both languages");
            const I descriptions[]={I::SettingsShowHiddenDesc,I::SettingsShowProtectedDesc};
            for(I id:descriptions) {
                const auto& value=l10n::Get(id);ui::ComPtr<IDWriteTextLayout> measured;
                s.compositor.DwriteFactory()->CreateTextLayout(value.c_str(),static_cast<UINT32>(value.size()),
                    s.compositor.SmallFormat(),4000*s.scale,100*s.scale,&measured);
                DWRITE_TEXT_METRICS metrics{};if(measured.get()) measured->GetMetrics(&metrics);
                fits &= measured.get() && metrics.widthIncludingTrailingWhitespace<=text_width;
            }
            check(fits,"hidden and protected descriptions fit the row in both languages");
        }
        l10n::SetLanguage(L"zh-CN");
        // Scrolled into view, both rows must answer with their own toggle target.
        s.settings.SetScroll(lay.hidden_files_row.top-lay.content.top,10000);
        auto scrolled=BuildVm(s,false);
        const auto visible=ui::MakeSettingsLayout(scrolled,window,s.scale,s.renderer.TitleBarHeight(),28*s.scale,&painter);
        auto row_hit=[&](D2D1_RECT_F r) {
            return s.renderer.HitTest(scrolled,window,(r.left+r.right)/2,(r.top+r.bottom)/2); };
        const auto hidden=row_hit(visible.hidden_files_row);
        const auto guarded=row_hit(visible.protected_files_row);
        check(hidden.region==H::SettingsToggle && hidden.index==5,"hidden files row keeps its toggle target");
        check(guarded.region==H::SettingsToggle && guarded.index==16,"protected system files row exposes its own toggle");
        Render(s);
        check(s.compositor.SaveSnapshot((std::filesystem::path(output).parent_path()/L"settings-advanced.png").c_str()),
            "advanced settings screenshot captured");
    }
    OpenSettingsTab(s,0);s.settings.SetScroll(expanded_max,expanded_max);HandleSettingsControl(s,accent_custom);
    check(s.settings.scroll()<=collapsed_max,"closing the custom accent clamps existing scroll");
    OpenSettingsTab(s,1);s.settings.SetScroll(0,0);
    const bool before=s.appPrefs.search_pinyin;click(layout().search_pinyin_row);
    check(s.appPrefs.search_pinyin!=before,"mouse click changes pinyin setting");
    toggle.index=1;HandleSettingsControl(s,toggle);vm=BuildVm(s,false);
    const float offset=s.renderer.SettingsDestinationOffset(vm,static_cast<int>(I::IndexLocation),window.right,window.bottom);
    check(offset>0 && (s.settingsExpanded&2u),"search destination locates expanded index storage section");
    // Pulse 右键菜单 in the 系统右键菜单 column style: three columns when wide,
    // no preview column below 960 dip.
    for(int w:{1400,900}) {
        SetWindowPos(s.hwnd,nullptr,0,0,static_cast<int>(w*s.scale),static_cast<int>(900*s.scale),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        OpenSettingsTab(s,2);s.settings.SetScroll(0,0);Render(s);
        const auto png=std::filesystem::path(output).parent_path()/(L"settings-context-"+std::to_wstring(w)+L".png");
        check(s.compositor.SaveSnapshot(png.c_str()),"context menu page screenshot captured");
    }
    log<<"failures="<<failures<<'\n';return failures ? 1 : 0;
}
#endif

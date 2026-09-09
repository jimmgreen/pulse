#include "../common/windows_compat.h"
#include "../ui/fluent_components.h"

#include <windows.h>

namespace {

using pulse::ui::Compositor;
using pulse::ui::HexColor;
using pulse::ui::MakeHighContrastTheme;
using pulse::ui::MakeTheme;
using pulse::ui::Theme;
using namespace pulse::ui::fluent;

LRESULT CALLBACK GalleryWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

D2D1_RECT_F Rect(float x, float y, float width, float height) {
    return D2D1::RectF(x, y, x + width, y + height);
}

ControlState State(bool hovered = false, bool pressed = false, bool enabled = true) {
    ControlState state{};
    state.hovered = hovered;
    state.pressed = pressed;
    state.enabled = enabled;
    return state;
}

bool DrawGallery(Compositor& compositor, bool dark, bool high_contrast,
                  const wchar_t* output, float canvas_scale = 1.0f) {
    Theme theme = high_contrast ? MakeHighContrastTheme()
                                : MakeTheme(dark, HexColor(0x0078D4));
    Painter painter(&compositor);
    painter.SetScale(1.0f);

    auto* dc = compositor.Dc();
    D2D1_MATRIX_3X2_F old_transform{};
    dc->GetTransform(&old_transform);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(canvas_scale, canvas_scale));
    dc->BeginDraw();
    dc->Clear(theme.bg);
    painter.BeginFrame(theme, high_contrast);
    painter.DrawText(high_contrast ? L"QFluent visual contract - high contrast"
                                   : dark ? L"QFluent visual contract - dark"
                                          : L"QFluent visual contract - light",
                     Rect(32, 18, 520, 32), compositor.HeaderFormat(), theme.text);

    const wchar_t* labels[] = {L"Normal", L"Hover", L"Pressed", L"Disabled"};
    for (int i = 0; i < 4; ++i) {
        painter.DrawText(labels[i], Rect(32.0f + i * 154.0f, 58, 138, 24),
                         compositor.SmallFormat(), theme.text_secondary);
        const ControlState state = i == 1 ? State(true) : i == 2 ? State(false, true)
                                                       : i == 3 ? State(false, false, false)
                                                                : State();
        painter.DrawButton({Rect(32.0f + i * 154.0f, 84, 138, 34), L"Button", L"\xE8FB",
                            ButtonKind::Standard, state});
        painter.DrawButton({Rect(32.0f + i * 154.0f, 128, 138, 34), L"Primary", L"\xE8FB",
                            ButtonKind::Primary, state});
        painter.DrawButton({Rect(32.0f + i * 154.0f, 172, 138, 34), L"Transparent", L"\xE713",
                            ButtonKind::Transparent, state});
    }

    TextFieldSpec input{Rect(32, 226, 288, 36), L"C:\\Projects\\Pulse", L"Address",
                        L"\xE8B7", L"\xE8BB", State()};
    painter.DrawTextField(input);
    input.bounds = Rect(334, 226, 288, 36);
    input.text = {};
    input.placeholder = L"Focused input";
    input.state.focused = true;
    input.state.keyboard_focus = true;
    painter.DrawTextField(input);

    ControlState checked{};
    checked.checked = true;
    ControlState checked_hover = checked;
    checked_hover.hovered = true;
    ControlState disabled_checked = checked;
    disabled_checked.enabled = false;
    painter.DrawCheckBox(Rect(32, 282, 170, 28), L"Unchecked", State());
    painter.DrawCheckBox(Rect(210, 282, 170, 28), L"Checked", checked);
    painter.DrawCheckBox(Rect(388, 282, 170, 28), L"Hover", checked_hover);
    painter.DrawCheckBox(Rect(566, 282, 170, 28), L"Disabled", disabled_checked);

    painter.DrawRadioButton(Rect(32, 322, 170, 28), L"Unchecked", State());
    painter.DrawRadioButton(Rect(210, 322, 170, 28), L"Checked", checked);
    ControlState pressed = State(false, true);
    painter.DrawRadioButton(Rect(388, 322, 170, 28), L"Pressed", pressed);
    painter.DrawRadioButton(Rect(566, 322, 170, 28), L"Disabled", disabled_checked);

    ControlState switch_off{};
    switch_off.check_progress = 0.0f;
    ControlState switch_on = checked;
    switch_on.check_progress = 1.0f;
    ControlState switch_middle = checked;
    switch_middle.check_progress = EvaluateMotion(motion::SwitchSlide, 60.0f);
    painter.DrawSwitch(Rect(32, 362, 180, 28), L"Off", switch_off);
    painter.DrawSwitch(Rect(220, 362, 180, 28), L"On", switch_on);
    painter.DrawSwitch(Rect(408, 362, 220, 28), L"120 ms transition", switch_middle);

    TabSpec tab{Rect(32, 414, 176, 36), L"Selected tab", L"\xE8B7", checked, true};
    tab.state.selected = true;
    painter.DrawTab(tab);
    tab.bounds = Rect(212, 414, 176, 36);
    tab.text = L"Hover tab";
    tab.state = State(true);
    painter.DrawTab(tab);
    tab.bounds = Rect(392, 414, 176, 36);
    tab.text = L"Pressed tab";
    tab.state = State(false, true);
    painter.DrawTab(tab);

    painter.DrawMenuSurface(Rect(662, 58, 250, 222));
    painter.DrawMenuItem({.bounds = Rect(662, 68, 250, 42), .text = L"Open",
                          .shortcut = L"Enter", .glyph = L"\xE8E5", .state = State(true)});
    painter.DrawMenuItem({.bounds = Rect(662, 110, 250, 42), .text = L"Copy",
                          .shortcut = L"Ctrl+C", .glyph = L"\xE8C8", .state = State()});
    MenuItemSpec selected_item{.bounds = Rect(662, 152, 250, 42), .text = L"View",
                               .glyph = L"\xE8A9", .state = State()};
    selected_item.state.selected = true;
    selected_item.has_submenu = true;
    painter.DrawMenuItem(selected_item);
    MenuItemSpec disabled_item{.bounds = Rect(662, 194, 250, 42),
                               .text = L"Unavailable", .glyph = L"\xE711",
                               .state = State()};
    disabled_item.state.enabled = false;
    disabled_item.separator_after = true;
    painter.DrawMenuItem(disabled_item);
    painter.DrawTooltip(Rect(676, 292, 218, 34), L"Tooltip fades in over 150 ms");

    painter.DrawProgressBar({Rect(32, 478, 280, 12), 0.68f});
    ProgressSpec indeterminate{Rect(32, 500, 280, 12)};
    indeterminate.indeterminate = true;
    indeterminate.animation_progress = 0.58f;
    painter.DrawProgressBar(indeterminate);
    ProgressSpec paused{Rect(32, 522, 280, 12), 0.45f};
    paused.paused = true;
    painter.DrawProgressBar(paused);
    ProgressSpec error{Rect(32, 544, 280, 12), 0.82f};
    error.error = true;
    painter.DrawProgressBar(error);

    ProgressSpec ring{Rect(342, 474, 64, 64), 0.72f};
    painter.DrawProgressRing(ring);
    ProgressSpec busy_ring{Rect(432, 474, 64, 64)};
    busy_ring.indeterminate = true;
    busy_ring.animation_progress = 0.34f;
    painter.DrawProgressRing(busy_ring);

    ListRowSpec row{Rect(32, 584, 590, 32)};
    row.state.hovered = true;
    painter.DrawListRowBackground(row);
    painter.DrawText(L"Hover row", Rect(50, 584, 260, 32), compositor.TextFormat(), theme.text);
    row.bounds = Rect(32, 620, 590, 32);
    row.state.hovered = false;
    row.state.selected = true;
    row.state.keyboard_focus = true;
    painter.DrawListRowBackground(row);
    painter.DrawText(L"Selected + keyboard focus", Rect(50, 620, 320, 32),
                     compositor.TextFormat(), theme.text);

    painter.DrawSidebarItem({.bounds = Rect(662, 352, 250, 38), .text = L"Projects",
                             .detail = L"12 folders", .glyph = L"\xE8B7",
                             .state = State(true), .badge_count = 12});
    painter.DrawTrayCard({Rect(662, 406, 250, 76), L"3 staged files", L"Release to current folder",
                          3, false, State(true)});

    ScrollbarSpec scrollbar{Rect(934, 58, 16, 594), 320, 260, 1200, 1.0f};
    painter.DrawScrollbar(scrollbar);
    scrollbar.viewport = Rect(960, 58, 16, 594);
    scrollbar.expand_progress = 0.0f;
    painter.DrawScrollbar(scrollbar);

    painter.DrawTagDot(D2D1::Point2F(690, 522), 4, HexColor(0xE74856));
    painter.DrawTagDot(D2D1::Point2F(706, 522), 4, HexColor(0xFFB900));
    painter.DrawTagDot(D2D1::Point2F(722, 522), 4, HexColor(0x00B7C3));

    painter.DrawTitleBarButton(Rect(662, 548, 46, 44), TitleBarButtonRole::Minimize,
                               {}, State());
    painter.DrawTitleBarButton(Rect(708, 548, 46, 44), TitleBarButtonRole::Maximize,
                               {}, State());
    painter.DrawTitleBarButton(Rect(754, 548, 46, 44), TitleBarButtonRole::Close,
                               {}, State());
    painter.DrawTitleBarButton(Rect(800, 548, 46, 44), TitleBarButtonRole::Close,
                               {}, State(true));

    dc->SetTransform(old_transform);
    if (FAILED(dc->EndDraw())) {
        return false;
    }
    return compositor.SaveSnapshot(output);
}

bool DrawCompositeGallery(Compositor& compositor, bool dark, bool high_contrast,
                           const wchar_t* output, float canvas_scale = 1.0f) {
    Theme theme = high_contrast ? MakeHighContrastTheme()
                                : MakeTheme(dark, HexColor(0x0078D4));
    Painter painter(&compositor);
    painter.SetScale(1.0f);

    auto* dc = compositor.Dc();
    D2D1_MATRIX_3X2_F old_transform{};
    dc->GetTransform(&old_transform);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(canvas_scale, canvas_scale));
    dc->BeginDraw();
    dc->Clear(theme.bg);
    painter.BeginFrame(theme, high_contrast);
    const D2D1_COLOR_F toolbar_fill = high_contrast ? theme.bg
                                      : dark ? HexColor(0x171717) : HexColor(0xF3F3F3);
    const D2D1_COLOR_F sidebar_fill = high_contrast ? theme.bg
                                      : dark ? HexColor(0x181818) : HexColor(0xF3F3F3);

    painter.FillRoundedRect(Rect(0, 0, 1000, 44), 0, theme.header_bg);
    painter.DrawText(L"Pulse", Rect(16, 0, 86, 44), compositor.HeaderFormat(), theme.text);
    ControlState selected{};
    selected.selected = true;
    painter.DrawTab({Rect(104, 4, 180, 40), L"Project Alpha", L"\xE8B7", selected, true});
    painter.DrawTab({Rect(288, 4, 142, 40), L"Downloads", L"\xE896", State(true), true});
    painter.DrawButton({Rect(434, 7, 32, 32), {}, L"\xE710", ButtonKind::Transparent,
                        State(), true});
    painter.DrawCommandSearchBox({Rect(616, 7, 232, 32), L"Command / Search", L"Ctrl+K",
                                  State()});
    painter.DrawTitleBarButton(Rect(862, 0, 46, 44), TitleBarButtonRole::Minimize,
                               {}, State());
    painter.DrawTitleBarButton(Rect(908, 0, 46, 44), TitleBarButtonRole::Maximize,
                               {}, State());
    painter.DrawTitleBarButton(Rect(954, 0, 46, 44), TitleBarButtonRole::Close,
                               {}, State());

    painter.FillRoundedRect(Rect(0, 44, 1000, 44), 0, toolbar_fill);
    painter.FillRoundedRect(Rect(0, 87, 1000, 1), 0, theme.stroke_divider);
    painter.DrawButton({Rect(12, 50, 32, 32), {}, L"\xE72B", ButtonKind::Transparent,
                        State(), true});
    painter.DrawButton({Rect(48, 50, 32, 32), {}, L"\xE72A", ButtonKind::Transparent,
                        State(), true});
    painter.DrawButton({Rect(84, 50, 32, 32), {}, L"\xE74A", ButtonKind::Transparent,
                        State(), true});
    painter.FillRoundedRect(Rect(120, 49, 418, 34), 6, theme.address_bg);
    painter.StrokeRoundedRect(Rect(120, 49, 418, 34), 6, theme.stroke_card);
    painter.DrawBreadcrumbSegment({Rect(124, 50, 66, 32), L"D:", L"\xE7F8",
                                    State(), false, true, false});
    painter.DrawBreadcrumbSegment({Rect(190, 50, 110, 32), L"Workspaces", {},
                                    State(), false, true, false});
    painter.DrawBreadcrumbSegment({Rect(300, 50, 122, 32), L"Design Assets", {},
                                    State(true), false, true, false});
    painter.DrawBreadcrumbSegment({Rect(422, 50, 112, 32), L"Project A", {},
                                    State(), true, false, true});
    painter.DrawSplitButton({Rect(548, 50, 112, 32), L"New", L"\xE710", State(), State()});
    painter.DrawButton({Rect(668, 50, 32, 32), {}, L"\xE8C6", ButtonKind::Transparent,
                        State(), true});
    painter.DrawButton({Rect(704, 50, 32, 32), {}, L"\xE8C8", ButtonKind::Transparent,
                        State(), true});
    painter.DrawButton({Rect(740, 50, 32, 32), {}, L"\xE74D", ButtonKind::Transparent,
                        State(), true});
    ControlState detail_view{};
    detail_view.selected = true;
    painter.DrawSegmentedItem({Rect(800, 50, 42, 32), {}, L"\xE8EF", detail_view,
                               SegmentPosition::First});
    painter.DrawSegmentedItem({Rect(841, 50, 42, 32), {}, L"\xF0E2", State(),
                               SegmentPosition::Middle});
    painter.DrawSegmentedItem({Rect(882, 50, 42, 32), {}, L"\xECA5", State(),
                               SegmentPosition::Last});
    ControlState details_toggle{};
    details_toggle.checked = true;
    painter.DrawButton({Rect(940, 50, 32, 32), {}, L"\xE89F",
                        ButtonKind::TransparentToggle, details_toggle, true});

    painter.FillRoundedRect(Rect(0, 88, 236, 558), 0, sidebar_fill);
    painter.FillRoundedRect(Rect(235, 88, 1, 558), 0, theme.stroke_divider);
    painter.DrawSidebarSectionHeader({Rect(10, 96, 216, 26), L"WORKSPACES", State(), true});
    painter.DrawSidebarItem({.bounds = Rect(10, 124, 216, 38), .text = L"Project Alpha",
                             .glyph = L"\xE8B7", .badge_text = L"Git", .state = selected,
                             .icon_color = HexColor(0x34D399)});
    painter.DrawSidebarSectionHeader({Rect(10, 170, 216, 26), L"QUICK ACCESS", State(), true});
    painter.DrawSidebarItem({.bounds = Rect(10, 198, 216, 34), .text = L"Starred",
                             .glyph = L"\xE735", .icon_color = HexColor(0xFBBF24)});
    painter.DrawSidebarItem({.bounds = Rect(10, 234, 216, 34), .text = L"Recent",
                             .glyph = L"\xE823", .icon_color = HexColor(0x60A5FA)});
    painter.DrawSidebarSectionHeader({Rect(10, 274, 216, 26), L"DRIVES", State(), true});
    painter.DrawDriveSidebarItem({.bounds = Rect(10, 302, 216, 54), .name = L"System (C:)",
                                  .detail = L"120G / 512G", .glyph = L"\xE7F8", .capacity = 0.24f,
                                  .icon_color = HexColor(0x60A5FA)});
    painter.DrawDriveSidebarItem({.bounds = Rect(10, 360, 216, 54), .name = L"Data (D:)",
                                  .detail = L"640G / 1TB", .glyph = L"\xE7F8", .capacity = 0.64f,
                                  .state = State(true), .icon_color = HexColor(0x34D399)});
    painter.DrawSidebarSectionHeader({Rect(10, 422, 216, 26), L"TAGS", State(), true});
    painter.DrawBadge({Rect(18, 454, 8, 8), {}, BadgeKind::Danger,
                       HexColor(0xE74856), HexColor(0xFFFFFF), true, true});
    painter.DrawText(L"Urgent fix", Rect(38, 444, 130, 28), compositor.TextFormat(), theme.text);
    painter.DrawBadge({Rect(18, 482, 8, 8), {}, BadgeKind::Warning,
                       HexColor(0xFFB900), HexColor(0x000000), true, true});
    painter.DrawText(L"Design review", Rect(38, 472, 130, 28), compositor.TextFormat(), theme.text);

    painter.DrawStagingTrayPanel({.bounds = Rect(10, 510, 216, 126), .title = L"Staging tray",
                                  .helper = L"Drop files here to collect", .count_label = L"2",
                                  .item_count = 2, .expanded = true});
    painter.DrawStagingItem({Rect(20, 560, 196, 28), L"hero_banner.png", State(), false});
    painter.DrawStagingItem({Rect(20, 594, 196, 28), L"schema_v2.json", State(true), false});

    painter.DrawPaneHeader({Rect(236, 88, 764, 48), L"Workspace A", L"14 items, 1 selected",
                            {}, State(), State()});
    painter.FillRoundedRect(Rect(236, 135, 764, 1), 0, theme.stroke_divider);
    painter.DrawColumnHeader({Rect(248, 136, 354, 32), L"Name", State(),
                              SortDirection::Ascending});
    painter.DrawColumnHeader({Rect(602, 136, 150, 32), L"Modified", State()});
    painter.DrawColumnHeader({Rect(752, 136, 132, 32), L"Status", State()});
    painter.DrawColumnHeader({Rect(884, 136, 106, 32), L"Size", State(),
                              SortDirection::None, HorizontalAlignment::Right});

    FileRowContentSpec file{};
    file.bounds = Rect(244, 172, 746, 36);
    file.name = L"01_Design_Prototypes";
    file.modified = L"2026-08-14 14:20";
    file.type = L"Folder";
    file.glyph = L"\xE8B7";
    file.tag_colors[0] = HexColor(0xFFB900);
    file.tag_count = 1;
    painter.DrawFileRowContent(file);
    file.bounds = Rect(244, 210, 746, 36);
    file.name = L"src_components";
    file.modified = L"2026-08-16 09:12";
    file.status = L"3 staged";
    file.status_kind = BadgeKind::Warning;
    file.tag_count = 0;
    painter.DrawFileRowContent(file);
    file.bounds = Rect(244, 248, 746, 36);
    file.name = L"AppLayout.tsx";
    file.modified = L"Today 09:45";
    file.size = L"14.2 KB";
    file.glyph = L"\xE943";
    file.status = L"Modified";
    file.status_kind = BadgeKind::Warning;
    file.state = selected;
    file.tag_colors[0] = HexColor(0xE74856);
    file.tag_count = 1;
    painter.DrawFileRowContent(file);
    file.bounds = Rect(244, 286, 746, 36);
    file.name = L"ui_concept_v3.png";
    file.modified = L"Yesterday 18:30";
    file.size = L"2.4 MB";
    file.glyph = L"\xEB9F";
    file.status = L"Untracked";
    file.status_kind = BadgeKind::Neutral;
    file.state = State(true);
    file.tag_colors[0] = HexColor(0xB146C2);
    painter.DrawFileRowContent(file);
    file.bounds = Rect(244, 324, 746, 36);
    file.name = L"Requirements_Doc.md";
    file.modified = L"2026-08-10 11:05";
    file.size = L"8.5 KB";
    file.glyph = L"\xE8A5";
    file.status = L"Committed";
    file.status_kind = BadgeKind::Success;
    file.state = State();
    file.tag_count = 0;
    painter.DrawFileRowContent(file);

    painter.DrawInfoBar({Rect(256, 392, 720, 42), L"Network snapshot",
                         L"Showing cached results while the share refreshes.", {},
                         InfoBarKind::Informational, State(), true});
    painter.DrawStatusBar({Rect(0, 646, 1000, 24), L"Ready | 1 selected (14.2 KB)",
                           L"14 files and folders", L"main | UTF-8 | Fluent mode", true});

    dc->SetTransform(old_transform);
    if (FAILED(dc->EndDraw())) {
        return false;
    }
    return compositor.SaveSnapshot(output);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) {
        return 3;
    }
    pulse::compat::EnableDpiAwareness();
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = GalleryWndProc;
    window_class.lpszClassName = L"PulseFluentGallery";
    RegisterClassW(&window_class);

    HWND hwnd = CreateWindowExW(0, window_class.lpszClassName, L"Pulse Fluent Gallery",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                1024, 720, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        CoUninitialize();
        return 1;
    }

    Compositor compositor;
    if (!compositor.Init(hwnd)) {
        DestroyWindow(hwnd);
        CoUninitialize();
        return 2;
    }
    compositor.RecreateTextFormats(1.0f);
    const bool dark_ok = DrawGallery(compositor, true, false,
                                     L"build-fluent-verify\\fluent_gallery_dark.png");
    const bool light_ok = DrawGallery(compositor, false, false,
                                      L"build-fluent-verify\\fluent_gallery_light.png");
    const bool high_contrast_ok = DrawGallery(
        compositor, false, true,
        L"build-fluent-verify\\fluent_gallery_high_contrast.png");
    const bool composite_dark_ok = DrawCompositeGallery(
        compositor, true, false,
        L"build-fluent-verify\\fluent_composite_dark.png");
    const bool composite_light_ok = DrawCompositeGallery(
        compositor, false, false,
        L"build-fluent-verify\\fluent_composite_light.png");
    const bool composite_high_contrast_ok = DrawCompositeGallery(
        compositor, false, true,
        L"build-fluent-verify\\fluent_composite_high_contrast.png");
    compositor.Resize(1536, 1080);
    const bool dark_150_ok = DrawGallery(
        compositor, true, false,
        L"build-fluent-verify\\fluent_gallery_dark_150.png", 1.5f);
    const bool light_150_ok = DrawGallery(
        compositor, false, false,
        L"build-fluent-verify\\fluent_gallery_light_150.png", 1.5f);
    const bool composite_dark_150_ok = DrawCompositeGallery(
        compositor, true, false,
        L"build-fluent-verify\\fluent_composite_dark_150.png", 1.5f);
    const bool composite_light_150_ok = DrawCompositeGallery(
        compositor, false, false,
        L"build-fluent-verify\\fluent_composite_light_150.png", 1.5f);
    compositor.Shutdown();
    DestroyWindow(hwnd);
    CoUninitialize();
    return dark_ok && light_ok && high_contrast_ok && composite_dark_ok &&
                   composite_light_ok && composite_high_contrast_ok &&
                   dark_150_ok && light_150_ok && composite_dark_150_ok &&
                   composite_light_150_ok ? 0 : 4;
}

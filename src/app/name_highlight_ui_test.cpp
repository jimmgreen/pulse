#ifdef PULSE_WITH_SELFTEST
#include "name_highlight_ui_test.h"
#include "../common/windows_compat.h"
#include "../ui/ui_renderer.h"
#include "../ui/ui_renderer_internal.h"
#include <cstdio>
#include <filesystem>

namespace pulse::app {
namespace {
using namespace ui;
bool Check(bool value, const char* label) {
    FILE* log = nullptr;
    if (fopen_s(&log, "bench_data/name-highlight/results.log", "a") == 0 && log) {
        std::fprintf(log, "[%s] %s\n", value ? "PASS" : "FAIL", label);
        std::fclose(log);
    }
    return value;
}

bool MatchTests() {
    bool ok = true;
    auto terms = NameHighlightTerms(L"", L"report ext:pdf size:>1kb !draft path:C:\\archive content:secret");
    auto ranges = NameMatchRanges(L"Report-report.pdf", terms);
    ok &= Check(ranges.size() == 2 && ranges[0].start == 0 && ranges[1].start == 7,
        "search highlights repeated case-insensitive filename terms");
    ok &= Check(NameMatchRanges(L"ext size draft secret archive.pdf", terms).empty(),
        "search syntax, exclusions, content and path filters are not filename highlights");
    terms = NameHighlightTerms(L"#work annual report", L"");
    ranges = NameMatchRanges(L"annual report - annual report.pdf", terms);
    ok &= Check(ranges.size() == 2 && ranges[0].length == 13,
        "pane filter keeps its phrase semantics and excludes tag selectors");
    terms = NameHighlightTerms(L"*report?.txt", L"");
    ranges = NameMatchRanges(L"report1.txt", terms);
    ok &= Check(ranges.size() == 2 && ranges[0].length == 6 && ranges[1].start == 7,
        "wildcards highlight literal parts, not wildcard-matched characters");
    ok &= Check(NameMatchRanges(L"report12.txt", terms).empty(), "nonmatching wildcard does not highlight");
    terms = NameHighlightTerms(L"report", L"");
    const auto visible = VisibleNameMatchRanges(L"report-report.pdf", L"rep\x2026pdf",
        NameMatchRanges(L"report-report.pdf", terms));
    ok &= Check(visible.size() == 1 && visible[0].start == 0 && visible[0].length == 3,
        "truncated filename maps only real visible characters and excludes ellipsis");
    terms = NameHighlightTerms(L"\x62a5\x544a", L"");
    ok &= Check(NameMatchRanges(L"\x62a5\x544a-\x62a5\x544a.docx", terms).size() == 2,
        "repeated Unicode terms preserve UTF-16 positions");
    terms = NameHighlightTerms(L"", L"zhongguo");
    ranges = NameMatchRanges(L"2026中国报告.txt", terms);
    ok &= Check(ranges.size() == 1 && ranges[0].start == 4 && ranges[0].length == 2,
        "pinyin highlight keeps the original Chinese character span");
    return ok;
}

WindowViewModel Fixture(float scale, bool search) {
    WindowViewModel vm;
    vm.window_effect = WindowEffect::None;
    vm.tabs.push_back({L"Filename match highlights", true});
    PaneSlotView slot;
    slot.rect = D2D1::RectF(220 * scale, 94 * scale, 1092 * scale, 700 * scale);
    slot.focused = true;
    auto& pane = slot.pane;
    pane.header_text = L"Documents";
    pane.filter_expand = 1.0f;
    pane.filter_text = search ? L"" : L"report";
    pane.is_search = search;
    pane.search_query = search ? L"report ext:pdf !draft content:secret" : L"";
    pane.selected_index = 0;
    pane.selected_count = 1;
    pane.hover_index = 1;
    const wchar_t* names[] = {L"Report-report.pdf", L"FANTAI_report", L"Annual report archive - final report with a long filename.pdf",
        L"report-report-report.docx", L"ext size draft secret.pdf"};
    auto map = std::make_shared<PaneViewModel::FilterMap>();
    for (int i = 0; i < 5; ++i) {
        ListEntryView entry;
        entry.name = names[i];
        entry.is_dir = i == 1;
        entry.attrs = entry.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        entry.type_text = entry.is_dir ? L"Folder" : L"Document";
        entry.date_text = L"2026-09-11 14:32";
        pane.entries.push_back(entry);
        map->push_back(i);
    }
    pane.filter_map = map;
    pane.change_badges[1] = {L"Just now", L"", 1, false, 0};
    auto tags = std::make_shared<PaneViewModel::TagDots>();
    (*tags)[1] = {HexColor(0x28B463)};
    pane.tag_dots = tags;
    vm.pane_slots.push_back(std::move(slot));
    return vm;
}
}

bool RunNameHighlightUiTest() {
    std::filesystem::create_directories(L"bench_data/name-highlight");
    FILE* log = nullptr;
    if (fopen_s(&log, "bench_data/name-highlight/results.log", "w") == 0 && log) std::fclose(log);
    bool ok = MatchTests();
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return false;
    pulse::compat::EnableDpiAwareness();
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseNameHighlightUiTest";
    RegisterClassW(&wc);
    const auto hwnd = CreateWindowExW(0, wc.lpszClassName, L"Filename match test", WS_OVERLAPPEDWINDOW,
        0, 0, 1440, 1080, nullptr, nullptr, instance, nullptr);
    {
        Compositor compositor;
        if (!hwnd || !compositor.Init(hwnd)) return false;
        MainRenderer renderer;
        renderer.SetCompositor(&compositor);
        compositor.Resize(1100, 740);
        auto* wallpaper_dc = compositor.Dc();
        ComPtr<ID2D1SolidColorBrush> wallpaper_brush;
        wallpaper_dc->CreateSolidColorBrush(HexColor(0xC79552), &wallpaper_brush);
        wallpaper_dc->BeginDraw();
        wallpaper_dc->Clear(HexColor(0x163543));
        wallpaper_dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(600, 240), 420, 260), wallpaper_brush.get());
        wallpaper_brush->SetColor(HexColor(0xF0E5CE));
        wallpaper_dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(900, 450), 220, 370), wallpaper_brush.get());
        wallpaper_dc->EndDraw();
        const std::wstring wallpaper = L"bench_data/name-highlight/contrast-background.png";
        ok &= Check(compositor.SaveSnapshot(wallpaper.c_str()), "contrast wallpaper fixture captured");
        for (const float scale : {1.0f, 1.5f, 2.0f}) {
            compositor.Resize(static_cast<int>(1100 * scale), static_cast<int>(740 * scale));
            compositor.RecreateTextFormats(scale);
            renderer.SetScale(scale);
            ComPtr<IDWriteTextLayout> padded;
            const std::wstring sample = L"20report20.jpg";
            const auto padding_matches = NameMatchRanges(sample, NameHighlightTerms(L"", L"20"));
            compositor.DwriteFactory()->CreateTextLayout(sample.c_str(), static_cast<UINT32>(sample.size()),
                compositor.TextFormat(), 1000 * scale, 40 * scale, &padded);
            DWRITE_TEXT_METRICS before{}, after{};
            padded->GetMetrics(&before);
            ApplyNameHighlightPadding(padded.get(), padding_matches, scale);
            padded->GetMetrics(&after);
            ok &= Check(std::abs(after.width - before.width - 8 * scale) < 0.1f,
                "two matches reserve exactly two DIP per side without changing internal letter spacing");
            for (float budget : {40.0f * scale, 85.0f * scale, 180.0f * scale}) {
                const auto shown = FitHighlightedFileName(&compositor, compositor.DwriteFactory(),
                    compositor.TextFormat(), sample, budget, padding_matches, scale);
                const float measured = MeasureLayoutText(&compositor, compositor.DwriteFactory(),
                    compositor.TextFormat(), shown) + HighlightPaddingWidth(sample, shown, padding_matches, scale);
                ok &= Check(measured <= budget + 0.1f, "ellipsis fitting reserves highlight gutters inside the name column");
                ok &= Check(shown == FitHighlightedFileName(&compositor, compositor.DwriteFactory(),
                    compositor.TextFormat(), sample, measured, padding_matches, scale),
                    "highlight name fitting stays stable when layout supplies its measured width");
            }
            const auto bounds = D2D1::RectF(0, 0, 1100 * scale, 740 * scale);
            for (const bool dark : {false, true}) for (int variant = 0; variant < 7; ++variant) {
                auto vm = Fixture(scale, variant == 1);
                vm.dark = dark;
                if (variant >= 4) {
                    vm.background_image = wallpaper;
                    vm.backdrop_enabled = true;
                    auto& pane = vm.pane_slots.front().pane;
                    pane.filter_text.clear();
                    pane.is_search = true;
                    pane.search_query = variant == 6 ? L"zhongguo" : L"20";
                    const wchar_t* numeric[] = {L"20e066dac2b122caf93a721-long-folder", L"20fa2d4c858a873ca262d",
                        L"20.jpg", L"2020-report-20.pdf", L"20.mp4"};
                    for (size_t i = 0; i < pane.entries.size(); ++i) {
                        pane.entries[i].name = variant == 6 ? L"2026中国报告-中国.txt" : numeric[i];
                        pane.entries[i].is_dir = i < 2;
                    }
                    if (variant == 5) pane.view_mode = ViewMode::MediumIcons;
                }
                if (variant == 2) vm.pane_slots.front().pane.view_mode = ViewMode::MediumIcons;
                if (variant == 3) vm.pane_slots.front().rect.right = 620 * scale;
                if (variant == 3) {
                    const auto& slot = vm.pane_slots.front();
                    const auto list = renderer.PaneListRect(slot.rect, 0, ViewMode::Details);
                    const auto columns = renderer.DetailsColumns(list, slot.pane);
                    ViewLayout positions(ViewMode::Details, list, slot.pane.EntryCount(), 0, 0,
                        scale, renderer.ListRowHeightDip(slot.pane, list));
                    for (const int row : {0, 1}) {
                        const auto name_rect = positions.NameRect(row);
                        const auto cell = positions.ItemRect(row);
                        const auto& entry = slot.pane.entries[static_cast<size_t>(row)];
                        const float badge_width = row == 1
                            ? ChangeBadgeWidth(slot.pane.change_badges.at(1), scale, &compositor) : 0;
                        const auto make_trail = [&](bool hover) {
                            return LayoutNameTrail(name_rect.left, name_rect.top,
                                name_rect.bottom - name_rect.top, columns.DividerX(0) - 4 * scale,
                                cell.top, cell.bottom, scale, entry.name, row == 1 ? 1 : 0,
                                badge_width, hover, hover && entry.is_dir, hover,
                                &compositor, compositor.DwriteFactory(), compositor.TextFormat(),
                                row == 1, entry.is_dir ? 3 : 2);
                        };
                        const auto idle = make_trail(false), hovered = make_trail(true);
                        const auto fitted = FitFileName(&compositor, compositor.DwriteFactory(),
                            compositor.TextFormat(), entry.name, 36 * scale);
                        const auto fitted_width = MeasureLayoutText(&compositor, compositor.DwriteFactory(),
                            compositor.TextFormat(), fitted);
                        ok &= Check(fitted == FitFileName(&compositor, compositor.DwriteFactory(),
                            compositor.TextFormat(), entry.name, fitted_width),
                            "fitting a measured name does not lose another character to ellipsis kerning");
                        ok &= Check(idle.name_w > 0 && idle.name_w == hovered.name_w &&
                            idle.tag_x0 == hovered.tag_x0 && !hovered.show_star &&
                            !hovered.show_more && !hovered.show_new_tab,
                            "real narrow columns keep names and tag positions stable instead of drawing actions");
                        ok &= Check(hovered.tag_n == 0 ||
                            (hovered.tag_x0 >= hovered.name_x + hovered.name_w &&
                             hovered.tag_x0 + 2 * hovered.tag_r + (hovered.tag_n - 1) * hovered.tag_step <=
                                columns.DividerX(0) - 8 * scale + 0.5f),
                            "narrow name tags remain inside their column without action overlap");
                    }
                }
                auto* dc = compositor.Dc();
                dc->BeginDraw();
                renderer.Render(vm, bounds, MakeTheme(dark, HexColor(0x0078D4)));
                const bool drawn = SUCCEEDED(dc->EndDraw());
                const auto path = L"bench_data/name-highlight/matches-" + std::to_wstring(static_cast<int>(scale * 100)) +
                    (dark ? L"-dark-" : L"-light-") + std::to_wstring(variant) + L".png";
                ok &= Check(drawn && compositor.SaveSnapshot(path.c_str()), "filename highlight renderer screenshot");
                compositor.Present();
            }
        }
    }
    DestroyWindow(hwnd);
    CoUninitialize();
    return ok;
}
}
#endif

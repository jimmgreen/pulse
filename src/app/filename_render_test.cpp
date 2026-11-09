#ifdef PULSE_WITH_SELFTEST
#include "name_highlight_ui_test.h"
#include "../common/windows_compat.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/ui_renderer_internal.h"
#include "../ui/typography.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace pulse::ui {
struct FilenameRenderTest {
    static bool BenchmarkDetailsList() {
        const std::filesystem::path root = L"bench_data/filename-render";
        std::filesystem::create_directories(root);
        std::ofstream log(root / "details-list-perf.log");
        (void)SystemHighContrastState();
        const auto high_contrast_queries = SystemHighContrastState().QueryCount();
        auto check = [&](bool pass, const char* label) {
            log << (pass ? "[PASS] " : "[FAIL] ") << label << std::endl;
            return pass;
        };
        HWND hwnd = CreateWindowExW(0, L"STATIC", L"Details list performance",
            WS_OVERLAPPEDWINDOW, 0, 0, 900, 650, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) return check(false, "create isolated benchmark window");
        Compositor compositor;
        if (!compositor.Init(hwnd)) {
            DestroyWindow(hwnd);
            return check(false, "initialize isolated benchmark compositor");
        }
        MainRenderer renderer;
        renderer.SetCompositor(&compositor);
        renderer.SetDetailsColumns(kDetailsColumnModified | kDetailsColumnType | kDetailsColumnSize);
        constexpr float scale = 1.5f;
        compositor.Resize(1350, 975);
        compositor.RecreateTextFormats(scale);
        renderer.SetScale(scale);
        PaneViewModel vm;
        vm.path = L"C:\\PulseBench";
        vm.view_mode = ViewMode::Details;
        vm.entries.reserve(16);
        for (int i = 0; i < 16; ++i) {
            ListEntryView entry;
            entry.name = L"Drawing-Project-2026-" + std::to_wstring(i + 1) + L"-final.dwg";
            entry.path = vm.path + L"\\" + entry.name;
            entry.type_text = L"AutoCAD Drawing";
            entry.size_text = L"1.24 MB";
            entry.date_text = L"2026/11/06 13:45";
            entry.size_value = 1300234;
            vm.entries.push_back(std::move(entry));
        }
        const Theme theme = MakeTheme(false, HexColor(0x0078D4));
        auto* dc = compositor.Dc();
        auto* filename = compositor.FileNameFormat();
        if (!dc || !filename) {
            compositor.Shutdown();
            DestroyWindow(hwnd);
            return check(false, "initialize Details list resources");
        }
        dc->BeginDraw();
        dc->Clear(theme.bg);
        dc->EndDraw();
        const auto* stats = compositor.GetLumaTextStats();
        const uint64_t misses_before = stats ? stats->surface_cache_misses : 0;
        const uint64_t fallbacks_before = stats ? stats->fallback_draws : 0;
        std::vector<uint64_t> list_ms;
        list_ms.reserve(9);
        uint64_t rows_drawn = 0;
        uint64_t names_drawn = 0;
        for (int frame = 0; frame < 9; ++frame) {
            dc->BeginDraw();
            renderer.ResetFramePerf();
            const auto start = std::chrono::steady_clock::now();
            renderer.DrawList(vm, 24.0f * scale, 90.0f * scale, 850.0f * scale, 480.0f * scale,
                theme, -1, -1, true, 0);
            const auto end = std::chrono::steady_clock::now();
            dc->EndDraw();
            rows_drawn += renderer.LastFramePerf().list_rows;
            names_drawn += renderer.LastFramePerf().name_draws;
            list_ms.push_back(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                end - start).count()));
        }
        const auto* after = compositor.GetLumaTextStats();
        const uint64_t misses = after && after->surface_cache_misses >= misses_before
            ? after->surface_cache_misses - misses_before : 0;
        const uint64_t fallbacks = after && after->fallback_draws >= fallbacks_before
            ? after->fallback_draws - fallbacks_before : 0;
        const auto sorted = list_ms;
        const uint64_t cold_us = sorted.front();
        std::vector<uint64_t> warm(sorted.begin() + 1, sorted.end());
        std::sort(warm.begin(), warm.end());
        log << "luma_enabled=" << compositor.LumaTextEnabled() << " scale=1.5 entries=16"
            << " visible_rows_total=" << rows_drawn << " names_drawn_total=" << names_drawn
            << " surface_misses=" << misses << " fallbacks=" << fallbacks << std::endl;
        if (after) {
            log << "luma_layout_us=" << after->layout_time_us
                << " luma_draw_us=" << after->draw_time_us
                << " luma_end_us=" << after->frame_end_time_us << std::endl;
        }
        log << "list_frame_us=";
        for (size_t i = 0; i < list_ms.size(); ++i) log << (i ? "," : "") << list_ms[i];
        log << " cold_us=" << cold_us << " warm_median_us=" << warm[warm.size() / 2] << std::endl;
        vm.hover_index = 0;
        ++renderer.motion_frame_;
        renderer.motion_now_ = motion::NowMs();
        dc->BeginDraw();
        renderer.DrawList(vm, 24.0f * scale, 90.0f * scale, 850.0f * scale, 480.0f * scale,
            theme, -1, -1, true, 0);
        dc->EndDraw();
        vm.hover_index = 8;
        std::vector<uint64_t> hover_us;
        std::vector<float> hover_y;
        hover_us.reserve(14);
        hover_y.reserve(14);
        for (int frame = 0; frame < 14; ++frame) {
            ++renderer.motion_frame_;
            renderer.motion_now_ += 16;
            dc->BeginDraw();
            renderer.ResetFramePerf();
            const auto start = std::chrono::steady_clock::now();
            renderer.DrawList(vm, 24.0f * scale, 90.0f * scale, 850.0f * scale, 480.0f * scale,
                theme, -1, -1, true, 0);
            const auto end = std::chrono::steady_clock::now();
            dc->EndDraw();
            hover_us.push_back(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                end - start).count()));
            hover_y.push_back(renderer.HoverMotionPositionForTest().top);
        }
        auto hover_sorted = hover_us;
        std::sort(hover_sorted.begin(), hover_sorted.end());
        const float hover_travel = hover_y.empty() ? 0.0f : hover_y.back() - hover_y.front();
        log << "hover_plate_frame_us=";
        for (size_t i = 0; i < hover_us.size(); ++i) log << (i ? "," : "") << hover_us[i];
        log << " hover_median_us=" << hover_sorted[hover_sorted.size() / 2]
            << " plate_y_first=" << hover_y.front() << " plate_y_last=" << hover_y.back()
            << " plate_travel=" << hover_travel << std::endl;
        bool ok = check(list_ms.size() == 9 && warm.size() == 8 && names_drawn > 0 &&
            hover_us.size() == 14 && hover_travel > 1.0f,
            "run real Details list and hover-motion frames");
        for (int scenario = 0; scenario < 6; ++scenario) {
            typography::SetTextRenderMode(scenario == 5 ? typography::TextRenderMode::Sharp : typography::TextRenderMode::Auto);
            typography::SetUiFontScale(scenario == 4 ? 125 : 100);
            compositor.RecreateTextFormats(scale);
            WindowViewModel window;
            window.pane = vm;
            window.pane.filter_text = scenario == 2 ? L"设计" : L"";
            if (scenario == 2) {
                auto matches = std::make_shared<PaneViewModel::FilterMap>();
                for (int i = 0; i < 16; ++i) matches->push_back(i);
                window.pane.filter_map = std::move(matches);
            }
            for (size_t i = 0; i < window.pane.entries.size(); ++i) {
                if (scenario > 0) window.pane.entries[i].name = L"结构设计施工图-中文长文件名-第" + std::to_wstring(i) + L"版-审核修改最终版本.dwg";
            }
            for (int frame = 0; frame < 12; ++frame) {
                window.pane.hover_index = frame % 12;
                if (frame == 6) window.pane.path = L"C:\\PulseBenchSecond";
                if (frame == 0 || frame == 6) {
                    renderer.SetFrameDamage(nullptr);
                    dc->BeginDraw();
                    renderer.Render(window, D2D1::RectF(0, 0, 1350, 975), theme);
                    ok = SUCCEEDED(dc->EndDraw()) && ok;
                    Sleep(350); // Let navigation's zero-alpha entrance finish before measuring visible rows.
                }
                const D2D1_RECT_F damage = D2D1::RectF(0, 200, 1350, 500);
                renderer.SetFrameDamage(scenario == 3 && frame > 0 ? &damage : nullptr);
                const auto before = *compositor.GetLumaTextStats();
                dc->BeginDraw();
                renderer.Render(window, D2D1::RectF(0, 0, 1350, 975), theme);
                const HRESULT draw_result = dc->EndDraw();
                const auto& perf = renderer.LastFramePerf();
                const auto& luma = *compositor.GetLumaTextStats();
                log << "scenario=" << scenario << " frame=" << frame << " render_us=" << perf.render_us
                    << " list_us=" << perf.list_us << " prepare_us=" << perf.row_prepare_us
                    << " icons_us=" << perf.row_icons_us << " text_us=" << perf.row_text_us
                    << " columns_us=" << perf.row_columns_us << " name_fit_us=" << perf.name_fit_us
                    << " name_draw_us=" << perf.name_draw_us << " names=" << perf.name_draws
                    << " row_fallbacks=" << (perf.row_prepare_fallbacks + perf.row_icons_fallbacks + perf.row_text_fallbacks + perf.row_columns_fallbacks)
                    << " all_fallbacks=" << luma.fallback_draws - before.fallback_draws
                    << " input_fail=" << luma.fail_input - before.fail_input
                    << " font_fail=" << luma.fail_font - before.fail_font
                    << " bounds_fail=" << luma.fail_bounds - before.fail_bounds
                    << " layout_fail=" << luma.fail_layout - before.fail_layout
                    << " backend_fail=" << luma.fail_backend - before.fail_backend
                    << " hr=" << static_cast<unsigned long>(draw_result) << std::endl;
                ok = SUCCEEDED(draw_result) && perf.name_draws > 0 && ok;
            }
        }
        typography::SetTextRenderMode(typography::TextRenderMode::Auto);
        typography::SetUiFontScale(100);
        ok = check(ok, "full Render matrix: Unicode, highlighting, partial, font scale, DirectWrite, navigation") && ok;
        ok = check(SystemHighContrastState().QueryCount() == high_contrast_queries,
            "all render frames perform zero high-contrast system queries") && ok;
        compositor.Shutdown();
        DestroyWindow(hwnd);
        return ok;
    }

    static bool Run() {
        const std::filesystem::path root = L"bench_data/filename-render";
        std::filesystem::create_directories(root);
        std::ofstream log(root / "results.log");
        int failures = 0;
        auto check = [&](bool pass, const char* label) {
            log << (pass ? "[PASS] " : "[FAIL] ") << label << std::endl;
            failures += !pass;
        };
        const HWND hwnd = CreateWindowExW(0, L"STATIC", L"Filename rendering regression",
            WS_OVERLAPPEDWINDOW, 0, 0, 900, 650, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        {
            Compositor compositor;
            if (!hwnd || !compositor.Init(hwnd)) {
                if (hwnd) DestroyWindow(hwnd);
                check(false, "initialize isolated renderer");
                return false;
            }
            MainRenderer renderer;
            renderer.SetCompositor(&compositor);
            log << "LumaText=" << compositor.LumaTextEnabled() << std::endl;
            const wchar_t* names[] = {L"Drawing1.dwg", L"Drawing2.dwg", L"图纸设计2.dwg",
                L"office-final.pdf", L"README", L".gitignore", L"archive.tar.gz",
                L"Drawing1234567890-very-long-file-name.dwg"};
            for (const wchar_t* language : {L"zh-CN", L"en-US"}) {
                l10n::Initialize(GetModuleHandleW(nullptr), language);
                for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
                    compositor.Resize(static_cast<UINT>(900 * scale), static_cast<UINT>(650 * scale));
                    compositor.RecreateTextFormats(scale);
                    renderer.SetScale(scale);
                    auto* dc = compositor.Dc();
                    auto* fmt = compositor.FileNameFormat();
                    auto* factory = compositor.DwriteFactory();
                    for (bool dark : {false, true}) {
                        const auto theme = MakeTheme(dark, HexColor(0x0078D4));
                        ComPtr<ID2D1SolidColorBrush> normal, dim;
                        dc->CreateSolidColorBrush(theme.text, &normal);
                        dc->CreateSolidColorBrush(WithAlpha(theme.text, dark ? .58f : .62f), &dim);
                        dc->BeginDraw();
                        dc->Clear(theme.bg);
                        for (int row = 0; row < 16; ++row) {
                            const std::wstring name = names[row % 8];
                            const float budget = (row < 8 ? 380.0f : 90.0f) * scale;
                            const float y = (10.0f + row * 39.0f) * scale;
                            const float height = 34.0f * scale;
                            const std::vector<NameMatchRange> matches = row == 11
                                ? std::vector<NameMatchRange>{{0, 3}} : std::vector<NameMatchRange>{};
                            const auto shown = FitHighlightedFileName(&compositor, factory, fmt, name, budget, matches, scale);
                            const float name_width = std::min(budget, MeasureLayoutText(&compositor, factory, fmt, shown) +
                                HighlightPaddingWidth(name, shown, matches, scale));
                            if (row < 8) check(shown == name, "wide filename retains every character before drawing");
                            const size_t dot = shown.find_last_of(L'.');
                            const bool has_extension = dot != std::wstring::npos && dot > 0 &&
                                shown.size() - dot <= 8 && shown.find(L'\u2026', dot) == std::wstring::npos;
                            const float phase = row % 2 ? 0.375f : 0.0f;
                            const bool reported = renderer.DrawTruncatedName(
                                name, 20 * scale + phase, y, name_width, height, theme, false, matches, true);
                            // The row tooltip relies on this (B站 #15).
                            check(reported == (shown != name), "drawn name reports exactly when it was shortened");
                            // Independent reference: shape the complete visible name once,
                            // then color the extension by UTF-16 range. No substring widths.
                            const auto rc = D2D1::RectF(460 * scale + phase, y, 460 * scale + phase + name_width, y + height);
                            const auto visible = VisibleNameMatchRanges(name, shown, matches);
                            // LumaText colours the extension of its single shaped line by
                            // clipping at the caret of the dot in that same layout.
                            const bool split_drawn = has_extension && visible.empty() &&
                                compositor.DrawLumaTextSplit(shown, fmt, rc, theme.text,
                                    static_cast<std::uint32_t>(dot), dim->GetColor(), theme.bg);
                            if (!split_drawn && (has_extension || !visible.empty())) {
                                ComPtr<IDWriteTextLayout> layout;
                                factory->CreateTextLayout(shown.c_str(), static_cast<UINT32>(shown.size()), fmt,
                                    name_width, height, &layout);
                                if (!layout.get()) { check(false, "create complete filename reference"); continue; }
                                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                                layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                                layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                                if (has_extension) layout->SetDrawingEffect(dim.get(),
                                    {static_cast<UINT32>(dot), static_cast<UINT32>(shown.size() - dot)});
                                // Production puts the baseline on a whole pixel row, as LumaText does.
                                float top = rc.top;
                                DWRITE_TEXT_METRICS text_metrics{};
                                DWRITE_LINE_METRICS line_metrics{};
                                UINT32 line_count = 0;
                                if (SUCCEEDED(layout->GetMetrics(&text_metrics)) &&
                                    SUCCEEDED(layout->GetLineMetrics(&line_metrics, 1, &line_count)) && line_count > 0) {
                                    const float baseline = rc.top + text_metrics.top + line_metrics.baseline;
                                    top += std::round(baseline) - baseline;
                                }
                                DrawNameHighlightBackground(&compositor, layout.get(), {rc.left, top}, rc, visible, theme, scale);
                                compositor.DrawTextLayout({rc.left, top}, layout.get(), normal.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                            } else if (!split_drawn && !compositor.DrawLumaText(shown, fmt, rc, theme.text, theme.bg)) {
                                dc->DrawText(shown.c_str(), static_cast<UINT32>(shown.size()), fmt, rc,
                                    normal.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                            }
                        }
                        check(SUCCEEDED(dc->EndDraw()), "draw production and full-name reference");
                        const auto file = root / (std::wstring(language) + L"-" + std::to_wstring(static_cast<int>(scale * 100)) +
                            (dark ? L"-dark.png" : L"-light.png"));
                        check(compositor.SaveSnapshot(file.c_str()), "save production/reference comparison");
                        ComPtr<ID2D1Image> image;
                        dc->GetTarget(&image);
                        ComPtr<ID2D1Bitmap1> target, readback;
                        HRESULT hr = image->QueryInterface(__uuidof(ID2D1Bitmap1), reinterpret_cast<void**>(&target.p));
                        if (SUCCEEDED(hr)) {
                            auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                target->GetPixelFormat(), 96, 96);
                            hr = dc->CreateBitmap(target->GetPixelSize(), nullptr, 0, &props, &readback);
                        }
                        if (SUCCEEDED(hr)) hr = readback->CopyFromBitmap(nullptr, target.get(), nullptr);
                        D2D1_MAPPED_RECT mapped{};
                        if (SUCCEEDED(hr)) hr = readback->Map(D2D1_MAP_OPTIONS_READ, &mapped);
                        check(SUCCEEDED(hr), "read rendered glyph pixels");
                        if (SUCCEEDED(hr)) {
                            for (int row = 0; row < 16; ++row) {
                                size_t differences = 0, ink = 0;
                                const UINT left = static_cast<UINT>(20 * scale), right = static_cast<UINT>(460 * scale);
                                const UINT top = static_cast<UINT>((10 + row * 39) * scale);
                                for (UINT y = top; y < static_cast<UINT>((44 + row * 39) * scale); ++y) {
                                    const auto* pixels = reinterpret_cast<const uint32_t*>(mapped.bits + y * mapped.pitch);
                                    for (UINT x = 0; x < static_cast<UINT>(390 * scale); ++x) {
                                        // WARP (no-GPU CI) antialiases glyph edges differently depending on absolute x.
                                        const uint32_t a = pixels[left + x], b = pixels[right + x];
                                        bool close_enough = true;
                                        for (int shift = 0; shift < 32; shift += 8) {
                                            const int delta = static_cast<int>((a >> shift) & 0xFF) - static_cast<int>((b >> shift) & 0xFF);
                                            close_enough = close_enough && delta <= 16 && delta >= -16;
                                        }
                                        differences += !close_enough;
                                        ink += pixels[left + x] != pixels[0];
                                    }
                                }
                                log << "[INFO] scale=" << scale << " dark=" << dark << " row=" << row
                                    << " different_pixels=" << differences << " ink_pixels=" << ink << std::endl;
                                // A few stray edge pixels are noise; a wrong glyph or color differs on far more.
                                check(ink > 0 && differences <= ink / 200, "production preserves full shaped glyphs and extension color");
                            }
                            readback->Unmap();
                        }
                    }
                }
            }
            // Icon views wrap the name over lines; cut-off lines count as shortened.
            {
                const float scale = 1.0f;
                compositor.Resize(900, 650);
                compositor.RecreateTextFormats(scale);
                renderer.SetScale(scale);
                const auto theme = MakeTheme(false, HexColor(0x0078D4));
                auto* dc = compositor.Dc();
                bool short_cut = true, long_cut = false;
                dc->BeginDraw();
                dc->Clear(theme.bg);
                renderer.DrawCenteredIconName(L"README", D2D1::RectF(20, 20, 220, 60), theme.text, theme, {}, &short_cut);
                renderer.DrawCenteredIconName(std::wstring(L"Drawing1234567890-very-long-file-name-") + std::wstring(60, L'x') + L".dwg",
                    D2D1::RectF(20, 80, 120, 120), theme.text, theme, {}, &long_cut);
                check(SUCCEEDED(dc->EndDraw()), "draw icon-view names");
                check(!short_cut, "icon-view name that fits is not reported as shortened");
                check(long_cut, "icon-view name cut off after its wrapped lines is reported as shortened");
            }
        }
        DestroyWindow(hwnd);
        log << "failures=" << failures << std::endl;
        return failures == 0;
    }
};
}
namespace pulse::app {
bool RunFilenameRenderTest() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return false;
    compat::EnableDpiAwareness();
    const bool ok = ui::FilenameRenderTest::Run();
    CoUninitialize();
    return ok;
}

bool RunDetailsListPerfTest() {
    return ui::FilenameRenderTest::BenchmarkDetailsList();
}
}
#endif

#include "../ui/ui_renderer_internal.h"
#include "../common/windows_compat.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace pulse::ui {
struct ColumnResizeUiTest {
    static bool Run(bool media_only = false) {
        const std::filesystem::path root = L"bench_data/column-resize";
        std::filesystem::create_directories(root);
        std::ofstream log(root / L"results.log");
        if (!log) return false;
        bool ok = true;
        auto check = [&](bool pass, const char* message) {
            log << (pass ? "[PASS] " : "[FAIL] ") << message << std::endl; ok &= pass;
        };
        HWND hwnd = CreateWindowExW(0, L"STATIC", L"Column resize", WS_POPUP,
            0, 0, 1200, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        {
            Compositor compositor;
            if (!hwnd || !compositor.Init(hwnd)) { if (hwnd) DestroyWindow(hwnd); return false; }
            MainRenderer renderer; renderer.SetCompositor(&compositor);
            if (media_only) {
                WindowViewModel settings; settings.settings_open = true; settings.settings_page = 9;
                const auto settings_bounds = D2D1::RectF(0, 0, 1200, 4000);
                const auto layout = MakeSettingsLayout(settings, settings_bounds, 1.0f,
                    renderer.title_bar_height_, renderer.status_height_, &renderer.painter_);
                const auto row = layout.list_style_row[6];
                const auto hit = renderer.HitTest(settings, settings_bounds,
                    (row.left + row.right) * 0.5f, (row.top + row.bottom) * 0.5f);
                check(row.bottom > row.top && hit.region == HitTestResult::SettingsToggle && hit.index == 80,
                      "media settings row hits its dedicated toggle id 80");
                for (float scale : {1.0f, 1.5f, 2.0f}) for (int media : {1, 2, 3})
                for (int width : {540, 1200, 2000}) {
                    renderer.SetScale(scale); compositor.RecreateTextFormats(scale);
                    compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(700 * scale));
                    const auto theme = MakeTheme(false, HexColor(0x0078D4));
                    renderer.UpdateBrushes(theme); renderer.text_background_ = theme.bg;
                    renderer.painter_.BeginFrame(theme, false);
                    PaneViewModel pane; pane.view_mode = ViewMode::Details; pane.media_columns = static_cast<uint8_t>(media);
                    pane.path = L"C:\\media-test";
                    for (int item_index = 0; item_index < 5; ++item_index) {
                        ListEntryView entry; entry.name = L"Photo-" + std::to_wstring(item_index) + L".jpg";
                        entry.type_text = L"JPEG"; entry.date_text = L"2026-11-04";
                        entry.created_text = entry.accessed_text = entry.date_text;
                        entry.size_text = L"1.6 MB"; pane.entries.push_back(entry);
                    }
                    pane.media_cell = [](int, SortColumn col) -> std::wstring {
                        if (col == SortColumn::MediaDims) return L"4000 × 2242";
                        if (col == SortColumn::MediaTaken) return L"2026-11-04";
                        return L"Camera";
                    };
                    const auto bounds = D2D1::RectF(0, 0, width * scale, 700 * scale);
                    const auto body = renderer.PaneBodyBounds(pane, bounds);
                    renderer.SetDetailsColumns(0);
                    const auto unchecked = renderer.DetailsColumns(body, pane);
                    check(!unchecked.Has(MainRenderer::ColumnKind::Date) && !unchecked.Has(MainRenderer::ColumnKind::Size),
                          "media layout respects unchecked ordinary columns");
                    renderer.SetDetailsColumns(kDetailsColumnsAll);
                    const auto before = renderer.DetailsColumns(body, pane);
                    if (width >= 1200) check(before.Has(MainRenderer::ColumnKind::Date) &&
                        before.Has(MainRenderer::ColumnKind::Created) && before.Has(MainRenderer::ColumnKind::Accessed) &&
                        before.Has(MainRenderer::ColumnKind::Type), "selected ordinary columns survive media layout");
                    for (int divider = 0; divider < before.count - 1; ++divider) {
                        log << "media=" << media << " width=" << width << " scale=" << scale << " divider=" << divider << std::endl;
                        auto resized = pane;
                        resized.media_column_widths[media - 1] = renderer.ResizeMediaColumnDivider(
                            body, pane, divider, before.DividerX(divider) + 12 * scale);
                        resized.details_column_dividers = renderer.ResizeMediaDetailsColumnDivider(
                            body, pane, divider, before.DividerX(divider) + 12 * scale);
                        const auto after = renderer.DetailsColumns(body, resized);
                        check(after.count == before.count && after.kinds == before.kinds,
                              "media resize keeps visible column identity at narrow width and DPI");
                        check(std::abs(after.DividerX(divider) - before.DividerX(divider) - 12 * scale) < 0.1f,
                              "media divider follows pointer using media kind mapping");

                    }
                    pane.media_column_widths[media - 1] = renderer.ResizeMediaColumnDivider(
                        body, pane, 0, before.DividerX(0) + 12 * scale);
                    WindowViewModel vm;
                    auto* dc = compositor.Dc(); dc->BeginDraw(); dc->Clear(theme.bg);
                    renderer.DrawSinglePane(vm, pane, bounds, 0, true, false, theme);
                    check(SUCCEEDED(dc->EndDraw()), "render media columns after resize");
                    const auto shot = root / (L"media-" + std::to_wstring(media) + L"-" +
                        std::to_wstring(width) + L"-" + std::to_wstring(static_cast<int>(scale * 100)) + L".png");
                    check(compositor.SaveSnapshot(shot.c_str()), "save media renderer screenshot");
                }
                DestroyWindow(hwnd);
                return ok;
            }
#ifdef PULSE_WITH_SELFTEST
            for (float scale : {1.0f, 1.5f, 2.0f}) for (bool dark : {false, true}) for (int width : {540, 760}) {
                renderer.SetScale(scale); compositor.RecreateTextFormats(scale);
                compositor.Resize(static_cast<UINT>(width*scale), static_cast<UINT>(700*scale));
                const auto theme = MakeTheme(dark, HexColor(0x0078D4));
                renderer.UpdateBrushes(theme); renderer.text_background_ = theme.bg;
                renderer.icon_cache_.SetDeviceContext(compositor.Dc()); renderer.painter_.BeginFrame(theme, false);
                PaneViewModel pane; pane.view_mode = ViewMode::Details; pane.path = L"C:\\资料";
                pane.header_text = L"列宽拖动：原始 / 向右 / 向左";
                for (const wchar_t* name : {L"季度数据.xlsx", L"Drawing2.dwg", L"设计说明.pdf"}) {
                    ListEntryView entry; entry.name = name; entry.path = pane.path + L"\\" + name;
                    entry.type_text = L"设计文档"; entry.date_text = L"2026-09-27 18:20"; entry.size_text = L"128.5 KB";
                    pane.entries.push_back(entry);
                }
                WindowViewModel vm;
                auto* dc = compositor.Dc(); dc->BeginDraw(); dc->Clear(theme.bg);
                for (int phase = 0; phase < 3; ++phase) {
                    const auto bounds = D2D1::RectF(8*scale, (8+phase*232.0f)*scale, (width-8.0f)*scale, (232+phase*232.0f)*scale);
                    const auto body = renderer.PaneBodyBounds(pane, bounds);
                    const auto initial = renderer.DetailsColumns(body, pane);
                    for (int i = 0; i < initial.count; ++i) {
                        using K = MainRenderer::ColumnKind;
                        const int slot = initial.kinds[i] == K::Date ? 0 : initial.kinds[i] == K::Type ? 1 : initial.kinds[i] == K::Size ? 2 : -1;
                        if (slot >= 0) pane.details_column_dividers[slot] = initial.widths[i] / scale;
                    }
                    const int divider = initial.count >= 3 ? 1 : 0;
                    const float cursor = initial.DividerX(divider) + (phase == 1 ? 60.0f : phase == 2 ? -120.0f : 0.0f)*scale;
                    pane.details_column_dividers = renderer.ResizeDetailsColumnDivider(body, pane.details_column_dividers, divider, cursor);
                    const auto after = renderer.DetailsColumns(body, pane);
                    check(initial.kinds == after.kinds && initial.count == after.count, "measured production columns retain visibility through dragging");
                    renderer.DrawSinglePane(vm, pane, bounds, 0, true, false, theme);
                }
                check(SUCCEEDED(dc->EndDraw()), "render before/after column resize");
                const auto shot = root / (std::to_wstring(width) + L"-" + std::to_wstring(static_cast<int>(scale*100)) + (dark ? L"-dark.png" : L"-light.png"));
                check(compositor.SaveSnapshot(shot.c_str()), "save actual column renderer screenshot");
            }
            const auto old_mode = typography::CurrentTextRenderMode();
            const int old_font = typography::UiFontScalePercent();
            const auto old_language = l10n::preference();
            for (float scale : {1.0f, 1.5f}) for (int font : {100, 125, 90})
            for (const auto mode : {typography::TextRenderMode::Auto, typography::TextRenderMode::Sharp, typography::TextRenderMode::Smooth})
            for (const wchar_t* language : {L"en-US", L"zh-CN"}) {
                const std::wstring sample = L"2026-12-30 23:59 XLSX 路径 final-report.xlsx";
                (void)renderer.CellTextWidth(sample);
                (void)renderer.CellTextWidth(sample, true);
                (void)renderer.AutoColumnWidths();
                check(!renderer.cell_text_widths_.empty(), "prewarm real independent list text cache");
                typography::SetUiFontScale(font); typography::SetTextRenderMode(mode); l10n::SetLanguage(language);
                renderer.SetScale(scale); compositor.RecreateTextFormats(scale);
                renderer.InvalidateTypography();
                check(renderer.cell_text_widths_.empty() && renderer.auto_widths_scale_ < 0,
                      "production typography invalidation clears both independent list caches");
                MainRenderer fresh; fresh.SetCompositor(&compositor); fresh.SetScale(scale);
                const auto a = renderer.AutoColumnWidths(), b = fresh.AutoColumnWidths();
                auto close = [](float left, float right) { return std::abs(left - right) < 0.1f; };
                check(close(renderer.CellTextWidth(sample), fresh.CellTextWidth(sample)) &&
                      close(renderer.CellTextWidth(sample, true), fresh.CellTextWidth(sample, true)) &&
                      close(a.date, b.date) && close(a.type, b.type) && close(a.size, b.size) &&
                      close(a.created, b.created) && close(a.accessed, b.accessed),
                      "warmed renderer matches fresh baseline after same-DPI font backend language changes");
                if (!((font == 125 && mode == typography::TextRenderMode::Auto && std::wstring_view(language) == L"en-US") ||
                      (font == 90 && mode == typography::TextRenderMode::Smooth && std::wstring_view(language) == L"zh-CN"))) continue;
                for (bool dark : {false, true}) {
                    compositor.Resize(static_cast<UINT>(760 * scale), static_cast<UINT>(320 * scale));
                    const auto theme = MakeTheme(dark, HexColor(0x0078D4));
                    renderer.UpdateBrushes(theme); renderer.text_background_ = theme.bg;
                    renderer.painter_.BeginFrame(theme, false);
                    PaneViewModel pane; pane.view_mode = ViewMode::Details; pane.is_search = true;
                    pane.path = L"C:\\private-typography-fixture";
                    pane.header_text = L"Typography cache regression";
                    ListEntryView entry; entry.name = L"final-report-季度数据.xlsx";
                    entry.path = pane.path + L"\\a-long-parent-folder\\another-parent\\" + entry.name;
                    entry.type_text = L"Excel document"; entry.date_text = L"2026-12-30 23:59"; entry.size_text = L"1023.9 KB";
                    pane.entries.push_back(entry);
                    WindowViewModel vm;
                    auto* dc = compositor.Dc(); dc->BeginDraw(); dc->Clear(theme.bg);
                    renderer.DrawSinglePane(vm, pane, {8 * scale, 8 * scale, 752 * scale, 312 * scale}, 0, true, false, theme);
                    check(SUCCEEDED(dc->EndDraw()), "draw resized typography with real path metadata layout");
                    const auto shot = root / (L"typography-" + std::to_wstring(font) + L"-" +
                        std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark.png" : L"-light.png"));
                    check(compositor.SaveSnapshot(shot.c_str()), "save real typography layout screenshot");
                }
            }
            // Type chip labels sit centered in every text rendering mode. User report:
            // with Text rendering set to Sharp / Smooth (DirectWrite) the extension was
            // drawn against the chip's left edge; Auto (LumaText) centered it.
            {
                typography::SetUiFontScale(100);
                for (float scale : {1.0f, 1.5f}) {
                    float auto_cy = -1.0f;
                    for (const auto mode : {typography::TextRenderMode::Auto, typography::TextRenderMode::Sharp,
                                            typography::TextRenderMode::Smooth}) {
                        typography::SetTextRenderMode(mode);
                        renderer.SetScale(scale); compositor.RecreateTextFormats(scale);
                        renderer.InvalidateTypography();
                        compositor.Resize(static_cast<UINT>(200 * scale), static_cast<UINT>(100 * scale));
                        const auto theme = MakeTheme(false, HexColor(0x0078D4));
                        renderer.UpdateBrushes(theme); renderer.text_background_ = theme.bg;
                        const std::wstring chip = L"DWG";
                        const float origin = std::round(40.0f * scale);
                        const auto box = D2D1::RectF(origin, origin,
                            origin + (renderer.TypeChipWidthDip(chip) - kTypeChipGapDip) * scale,
                            origin + 17.0f * scale);
                        auto* dc = compositor.Dc(); dc->BeginDraw(); dc->Clear(theme.bg);
                        renderer.DrawTypeChipLabel(dc, chip, box, HexColor(0x202020), theme.bg);
                        check(SUCCEEDED(dc->EndDraw()), "draw a type chip label");
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
                        check(SUCCEEDED(hr), "read type chip label pixels");
                        if (FAILED(hr)) continue;
                        // Ink = anything that is not the cleared background, searched a few
                        // pixels beyond the box so a label outside it would still be seen.
                        const uint32_t bg = reinterpret_cast<const uint32_t*>(mapped.bits)[0];
                        // Blended backgrounds may round by a step or two; only real ink counts.
                        const auto differs = [](uint32_t a, uint32_t b) {
                            for (int shift = 0; shift < 24; shift += 8) {
                                const int ca = static_cast<int>((a >> shift) & 0xFF), cb = static_cast<int>((b >> shift) & 0xFF);
                                if (std::abs(ca - cb) > 24) return true;
                            }
                            return false;
                        };
                        int min_x = INT_MAX, max_x = -1, min_y = INT_MAX, max_y = -1;
                        const int x0 = static_cast<int>(box.left - 6 * scale), x1 = static_cast<int>(box.right + 6 * scale);
                        const int y0 = static_cast<int>(box.top - 6 * scale), y1 = static_cast<int>(box.bottom + 6 * scale);
                        for (int y = y0; y < y1; ++y) {
                            const auto* row = reinterpret_cast<const uint32_t*>(mapped.bits + static_cast<size_t>(y) * mapped.pitch);
                            for (int x = x0; x < x1; ++x) {
                                if (!differs(row[x], bg)) continue;
                                min_x = std::min(min_x, x); max_x = std::max(max_x, x);
                                min_y = std::min(min_y, y); max_y = std::max(max_y, y);
                            }
                        }
                        readback->Unmap();
                        const bool found = max_x >= 0;
                        const float ink_cx = (static_cast<float>(min_x) + static_cast<float>(max_x) + 1.0f) * 0.5f;
                        const float ink_cy = (static_cast<float>(min_y) + static_cast<float>(max_y) + 1.0f) * 0.5f;
                        const float box_cx = (box.left + box.right) * 0.5f;
                        log << "[INFO] chip mode=" << static_cast<int>(mode) << " scale=" << scale
                            << " ink_x=" << min_x << ".." << max_x << " ink_y=" << min_y << ".." << max_y
                            << " box_x=" << box.left << ".." << box.right << std::endl;
                        check(found && std::abs(ink_cx - box_cx) <= 1.5f * scale,
                              "type chip label is centered horizontally in every text rendering mode");
                        if (mode == typography::TextRenderMode::Auto) auto_cy = found ? ink_cy : -1.0f;
                        else check(found && auto_cy >= 0.0f && std::abs(ink_cy - auto_cy) <= 1.5f * scale,
                                   "type chip label sits at the same height as with LumaText");
                    }
                }
            }
            typography::SetUiFontScale(old_font); typography::SetTextRenderMode(old_mode);
            l10n::SetLanguage(l10n::LanguageId(old_language));
#endif
        }
        DestroyWindow(hwnd); return ok;
    }
};
}
namespace pulse::app {
bool RunMediaColumnResizeCapture() {
    return ui::ColumnResizeUiTest::Run(true);
}
#ifdef PULSE_WITH_SELFTEST
bool RunColumnResizeUiTest() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return false;
    compat::EnableDpiAwareness(); l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const bool ok = ui::ColumnResizeUiTest::Run();
    CoUninitialize(); return ok;
}
#endif
}

#include "app_internal.h"
#include "../ui/ui_renderer.h"
#include "app_input.h"
#include "../ui/ui_renderer_internal.h"
#include "../ui/typography.h"
#include <filesystem>
#include <fstream>

#ifdef PULSE_WITH_SELFTEST
namespace pulse { bool TestSettingsFilter(const std::wstring&, l10n::StringId); }
int RunQuickLookSettingsTest(pulse::AppState& s, const wchar_t* output) {
    using namespace pulse;
    using H = ui::HitTestResult;
    const auto folder = std::filesystem::path(output).parent_path();
    std::filesystem::create_directories(folder);
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        log << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!ok) ++failures;
    };
    const auto original_font = ui::typography::UiFontScalePercent();
    const auto original_scale = s.scale;
    const auto original_language = l10n::preference();
    const auto original_expanded = s.settingsExpanded;
    const auto original_page = s.settings.page();
    const auto original_scroll = s.settings.scroll();
    auto* active = ActiveTab(s);
    const auto original_path = active ? active->current_path : std::wstring{};
    check((ui::kSettingsDefaultExpandedMask & 4u) != 0, "format disclosure starts expanded by default");
    if (active) {
        active->current_path = L"pulse:settings:packs";
        s.settings.SelectPage(5); s.settingsExpanded |= 4u;
        ui::fluent::Painter click_painter(&s.compositor); click_painter.SetScale(s.scale);
        const auto click_rect = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
        auto layout = [&] {
            return ui::MakeSettingsLayout(BuildVm(s, false), click_rect, s.scale,
                s.renderer.TitleBarHeight(), 28*s.scale, &click_painter);
        };
        const auto expanded = layout();
        auto click_header = [&](const auto& current) {
            const auto r = current.disclosure[2];
            const LPARAM point = MAKELPARAM(static_cast<int>((r.left + r.right)/2), static_cast<int>((r.top + r.bottom)/2));
            HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
            HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, point);
        };
        click_header(expanded);
        const auto collapsed = layout();
        check(!(s.settingsExpanded & 4u) && collapsed.preview_formats.bottom == 0 &&
              collapsed.preview_codec_section.top < expanded.preview_codec_section.top,
              "real header click collapses formats and moves following sections up");
        click_header(collapsed);
        check((s.settingsExpanded & 4u) && layout().preview_formats.bottom > layout().preview_formats.top,
              "second real header click restores the format overview");
    } else check(false, "isolated window has a settings tab for disclosure interaction");
    for (const auto* language : {L"zh-CN", L"zh-TW", L"en-US"}) {
        l10n::SetLanguage(language);
        const std::wstring queries[] = {
            l10n::Get(l10n::StringId::QuickPreview),
            l10n::Pick(L"预览增强包", L"Preview packs"),
            l10n::Pick(L"系统扩展", L"System extensions"),
            l10n::Pick(L"支持的格式", L"Supported formats")};
        for (const auto& query : queries) {
            const bool found = TestSettingsFilter(query, l10n::StringId::QuickPreview);
            const auto utf8 = [](std::wstring_view value) {
                const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
                std::string result(static_cast<size_t>(length), '\0');
                if (length) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
                return result;
            };
            log << "search language=" << utf8(language) << " query=" << utf8(query) << " found=" << found << '\n';
            check(found, "settings search exposes Quick Look and its format, pack, and system headings");
        }
    }
    ui::fluent::Painter painter(&s.compositor);
    for (const auto* language : {L"zh-CN", L"en-US"}) for (const int font : {100, 125})
    for (const float scale : {1.0f, 1.5f, 2.0f}) for (const int width : {760, 1280}) {
        l10n::SetLanguage(language);
        ui::typography::SetUiFontScale(font);
        s.compositor.RecreateTextFormats(scale); s.renderer.SetScale(scale); painter.SetScale(scale);
        const auto rect = D2D1::RectF(0, 0, width*scale, 960*scale);
        s.compositor.Resize(static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom));
        ui::WindowViewModel vm;
        vm.settings_open = true; vm.settings_page = 5; vm.settings_ui_font_scale = font;
        vm.settings_version = L"test";
        vm.settings_pack_root = L"C:\\Users\\Example\\AppData\\Local\\Pulse\\packs";
        vm.settings_pack_media_available = vm.settings_pack_images_available = true;
        vm.settings_pack_raw_available = true;
        log << "case language=" << (language[0] == L'z' ? "zh" : "en") << " font=" << font
            << " dpi=" << scale << " width=" << width << '\n';
        for (const bool installed : {false, true}) {
            vm.settings_preview_codecs = ui::kPreviewCodecsDetected | (installed ? 15u : 0u);
            vm.settings_pack_images_installed = vm.settings_pack_images_enabled = installed;
            vm.settings_pack_raw_installed = vm.settings_pack_raw_enabled = installed;
            vm.settings_pack_media_installed = vm.settings_pack_ffmpeg_enabled = installed;
            vm.settings_pack_ffmpeg = installed ? 1 : 0;
            vm.settings_pack_bytes = installed ? 100 * 1024 * 1024 : 0;
            vm.settings_pack_installed = installed ? 3 : 0;
            vm.settings_pack_images_version = vm.settings_pack_raw_version = L"1.0.0";
            const auto lay = ui::MakeSettingsLayout(vm, rect, scale, s.renderer.TitleBarHeight(), 28*scale, &painter);
            check(lay.preview_formats.bottom > lay.preview_formats.top && lay.disclosure[2].bottom > lay.disclosure[2].top,
                "Quick Look format overview has an expanded interactive header");
            check(lay.preview_group.bottom < lay.preview_codec_section.top &&
                  lay.preview_codec_group.bottom < lay.pack_section.top && lay.pack_section.bottom <= lay.pack_summary.top,
                "formats and system extensions precede preview packs");
            check(lay.pack_card.bottom <= lay.pack_source_group.top && lay.pack_source_group.bottom < lay.pack_images_card.top &&
                  lay.pack_images_card.bottom < lay.pack_raw_card.top,
                "all three pack managers remain separately laid out");
            {   // 高级: background updates above the uninstall choice, each row its own switch.
                const auto a = lay.pack_auto_update_row, r = lay.pack_remove_row;
                check(a.bottom > a.top && a.top >= lay.pack_group.top && a.bottom <= r.top && r.bottom <= lay.pack_group.bottom,
                      "automatic pack updates share the advanced card, above the uninstall choice");
                auto scrolled = vm;
                scrolled.settings_scroll = lay.pack_group.top - lay.content.top - 10*scale;
                const auto visible = ui::MakeSettingsLayout(scrolled, rect, scale, s.renderer.TitleBarHeight(), 28*scale, &painter);
                const auto at = [&](D2D1_RECT_F row) {
                    return s.renderer.HitTest(scrolled, rect, row.right - 37*scale, (row.top + row.bottom)/2);
                };
                const auto auto_hit = at(visible.pack_auto_update_row), remove_hit = at(visible.pack_remove_row);
                check(auto_hit.region == H::SettingsPackAction && auto_hit.index == static_cast<int>(ui::PackAction::AutoUpdate) &&
                      remove_hit.region == H::SettingsPackAction &&
                      remove_hit.index == static_cast<int>(ui::PackAction::RemoveOnUninstall),
                      "the automatic update and uninstall switches hit their own actions");
            }
            std::vector<ui::PreviewFormatChip> chips;
            std::vector<ui::PreviewFormatRow> rows;
            ui::LayoutPreviewFormats(lay.preview_formats, scale, &chips, &rows,
                [&](std::wstring_view value, float note_width) { return painter.MeasureWrappedCaptionHeight(value, note_width); });
            const auto& groups = ui::PreviewFormatGroups();
            for (size_t group = 0; group < rows.size() && group < groups.size(); ++group) {
                if (groups[group].note.empty()) continue;
                const auto note = rows[group].note;
                ui::ComPtr<IDWriteTextLayout> wrapped;
                HRESULT measured = s.compositor.DwriteFactory()->CreateTextLayout(
                    groups[group].note.c_str(), static_cast<UINT32>(groups[group].note.size()),
                    s.compositor.SmallFormat(), (std::max)(1.0f, note.right - note.left), 100000.0f, &wrapped);
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(measured)) measured = wrapped->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                if (SUCCEEDED(measured)) measured = wrapped->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                if (SUCCEEDED(measured)) measured = wrapped->GetMetrics(&metrics);
                const bool fits = SUCCEEDED(measured) && note.right > note.left &&
                    metrics.height <= note.bottom - note.top + 0.5f;
                if (!fits) log << "note group=" << group << " measured_height=" << metrics.height
                    << " allocated_height=" << note.bottom - note.top << " width=" << note.right - note.left
                    << " HRESULT=" << measured << '\n';
                check(fits, "actual wrapped category description fits its allocated rectangle");
            }
            bool contained = true, disjoint = true;
            for (size_t i = 0; i < chips.size(); ++i) {
                const auto a = chips[i].rect;
                contained &= a.left >= lay.preview_formats.left && a.right <= lay.preview_formats.right &&
                             a.top >= lay.preview_formats.top && a.bottom <= lay.preview_formats.bottom;
                if (i) {
                    const auto b = chips[i - 1].rect;
                    disjoint &= a.left >= b.right || a.top >= b.bottom;
                }
            }
            check(contained && disjoint && chips.size() == ui::PreviewFormatCount(), "all format chips stay inside card without overlapping");
            bool controls_ok = true;
            for (int i = 0; i < ui::kPreviewCodecCount; ++i) {
                const auto row = lay.preview_codec_row[i], text = lay.preview_codec_text[i], badge = lay.preview_codec_badge[i];
                controls_ok &= badge.left >= row.left && badge.right <= row.right && badge.bottom <= row.bottom;
                controls_ok &= text.right <= badge.left || text.bottom <= badge.top;
            }
            check(controls_ok, "codec labels and status controls never overlap");
            if (installed) {
                // Current packs show no 更新到; older ones show it left of 卸载, with its own target.
                check(lay.pack_update.right <= lay.pack_update.left && lay.pack_images_update.right <= lay.pack_images_update.left &&
                      lay.pack_raw_update.right <= lay.pack_raw_update.left,
                      "current packs offer no update");
                auto older = vm;
                older.settings_pack_version = older.settings_pack_images_version = older.settings_pack_raw_version = L"1.0.1";
                older.settings_pack_update_version = older.settings_pack_images_update_version =
                    older.settings_pack_raw_update_version = L"1.0.2";
                const auto u = ui::MakeSettingsLayout(older, rect, scale, s.renderer.TitleBarHeight(), 28*scale, &painter);
                using Lay = ui::SettingsLayout;
                struct UpdateCard { D2D1_RECT_F Lay::* card; D2D1_RECT_F Lay::* update; D2D1_RECT_F Lay::* primary; ui::PackAction action; };
                const UpdateCard cards[] = {
                    {&Lay::pack_card, &Lay::pack_update, &Lay::pack_primary, ui::PackAction::Update},
                    {&Lay::pack_images_card, &Lay::pack_images_update, &Lay::pack_images_primary, ui::PackAction::ImagesUpdate},
                    {&Lay::pack_raw_card, &Lay::pack_raw_update, &Lay::pack_raw_primary, ui::PackAction::RawUpdate}};
                bool placed = true, targets = true;
                for (const auto& c : cards) {
                    const auto card = u.*c.card, update = u.*c.update, primary = u.*c.primary;
                    placed &= update.right > update.left && update.right < primary.left &&
                              update.top == primary.top && update.bottom == primary.bottom &&
                              update.left >= card.left + 54*scale && update.bottom <= card.bottom;
                    auto scrolled = older;
                    scrolled.settings_scroll = card.top - u.content.top - 10*scale;
                    const auto visible = ui::MakeSettingsLayout(scrolled, rect, scale, s.renderer.TitleBarHeight(), 28*scale, &painter);
                    const auto r = visible.*c.update;
                    const auto hit = s.renderer.HitTest(scrolled, rect, (r.left + r.right)/2, (r.top + r.bottom)/2);
                    targets &= hit.region == H::SettingsPackAction && hit.index == static_cast<int>(c.action);
                }
                check(placed, "update button sits left of the primary button inside every pack card");
                check(targets, "each update button hits its own pack's update action");
                auto busy = older;
                busy.settings_pack_images_installing = true;
                busy.settings_pack_images_progress = 0.42f;
                const auto b = ui::MakeSettingsLayout(busy, rect, scale, s.renderer.TitleBarHeight(), 28*scale, &painter);
                check(b.pack_images_update.right <= b.pack_images_update.left && b.pack_raw_update.right > b.pack_raw_update.left &&
                      ui::pack_text::ImagesBadge(busy).text == l10n::Pick(L"正在更新", L"Updating"),
                      "a downloading update hides only its own button and says it is updating");
                if (language[0] == L'z' && scale == 1 && ((width == 1280 && font == 100) || (width == 760 && font == 125))) {
                    for (int section = 0; section < 2; ++section) {
                        auto shot = section == 0 ? older : busy;
                        shot.dark = false;
                        shot.settings_scroll = (section == 0 ? u.pack_card.top : u.pack_images_card.top) - u.content.top - 40*scale;
                        const auto theme = ui::MakeTheme(false, ui::HexColor(0x4F7766));
                        s.compositor.Dc()->BeginDraw(); s.renderer.Render(shot, rect, theme); s.compositor.Dc()->EndDraw();
                        const auto name = L"quicklook-" + std::to_wstring(width) + L"-font" + std::to_wstring(font) +
                            (section == 0 ? L"-update.png" : L"-updating.png");
                        check(s.compositor.SaveSnapshot((folder / name).c_str()), "Quick Look screenshot saved");
                    }
                }
            }
            if (!installed) {
                for (const bool expanded : {true, false}) {
                auto scrolled = vm;
                scrolled.settings_expanded = expanded ? vm.settings_expanded | 4u : vm.settings_expanded & ~4u;
                const auto initial = ui::MakeSettingsLayout(scrolled, rect, scale, s.renderer.TitleBarHeight(), 28*scale, &painter);
                check(expanded || (initial.preview_formats.bottom == 0 && initial.preview_legend.bottom == 0),
                    "collapsed disclosure removes format and legend geometry");
                scrolled.settings_scroll = initial.preview_codec_group.top - initial.content.top - 10*scale;
                const auto visible = ui::MakeSettingsLayout(scrolled, rect, scale, s.renderer.TitleBarHeight(), 28*scale, &painter);
                const auto get = visible.preview_codec_button[0];
                const auto hit = s.renderer.HitTest(scrolled, rect, (get.left + get.right)/2, (get.top + get.bottom)/2);
                check(hit.region == H::SettingsPreviewStore && hit.index == 0, "system extension Get retains its working hit target after disclosure changes");
                }
            }
            if (language[0] == L'z' && scale == 1 && ((width == 1280 && font == 100) || (width == 760 && font == 125))) {
                for (const bool dark : {false, true}) for (int section = 0; section < 4; ++section) {
                    vm.dark = dark;
                    auto shot = vm;
                    const float tops[] = {0, lay.preview_codec_section.top, lay.pack_section.top, lay.pack_advanced_section.top};
                    shot.settings_scroll = section == 0 ? 0 : tops[section] - lay.content.top - 10*scale;
                    const auto theme = ui::MakeTheme(dark, ui::HexColor(0x4F7766));
                    s.compositor.Dc()->BeginDraw(); s.renderer.Render(shot, rect, theme); s.compositor.Dc()->EndDraw();
                    const auto name = L"quicklook-" + std::to_wstring(width) + L"-font" + std::to_wstring(font) +
                        (installed ? L"-enabled" : L"-missing") + (dark ? L"-dark-" : L"-light-") + std::to_wstring(section) + L".png";
                    check(s.compositor.SaveSnapshot((folder / name).c_str()), "Quick Look screenshot saved");
                }
            }
        }
        vm.settings_page = 0; vm.settings_scroll = 0;
        const auto general = ui::MakeSettingsLayout(vm, rect, scale, s.renderer.TitleBarHeight(), 28*scale, &painter);
        check(general.preview_group.bottom == 0 && general.preview_codec_group.bottom == 0, "General no longer duplicates Quick Look");
    }
    if (active) active->current_path = original_path;
    s.settings.SelectPage(original_page); s.settingsExpanded = original_expanded;
    s.settings.SetScroll(original_scroll, original_scroll);
    ui::typography::SetUiFontScale(original_font);
    s.compositor.RecreateTextFormats(original_scale); s.renderer.SetScale(original_scale);
    l10n::SetLanguage(l10n::LanguageId(original_language));
    return failures ? 1 : 0;
}
#endif

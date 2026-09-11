#ifdef PULSE_WITH_SELFTEST
#include "change_tracking_ui_test.h"
#include "../common/windows_compat.h"
#include "../ui/ui_renderer.h"
#include "../ui/ui_renderer_internal.h"
#include <cstdio>
#include <filesystem>

using namespace pulse::ui;

namespace {
bool Check(bool value, const char* name) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
    FILE* log = nullptr;
    if (fopen_s(&log, "bench_data/change-ui/results.log", "a") == 0 && log) {
        std::fprintf(log, "[%s] %s\n", value ? "PASS" : "FAIL", name);
        std::fclose(log);
    }
    return value;
}

WindowViewModel Fixture(float scale, float width) {
    WindowViewModel vm;
    vm.window_title = L"Change tracking visual verification";
    vm.window_effect = WindowEffect::None;
    vm.tabs.push_back({L"Changes", true});
    PaneSlotView slot;
    slot.rect = D2D1::RectF(220 * scale, 94 * scale, width - 8 * scale, 700 * scale);
    slot.focused = true;
    slot.pane.header_text = L"Project files";
    slot.pane.title_change_badge = {L"Today", L"Includes descendants", 6, true, 0};
    auto tags = std::make_shared<PaneViewModel::TagDots>();
    (*tags)[0] = {HexColor(0x28B463), HexColor(0xA569BD)};
    slot.pane.tag_dots = tags;
    const wchar_t* names[] = {L"FANTAI", L"Project_archive", L"DB.Browser.for.SQLite-v3.13.1-win64", L"Documents", L"Downloads", L"Music", L"Pictures", L"Videos", L"workspace"};
    (*tags)[2] = {HexColor(0x28B463)};
    for (int i = 0; i < 9; ++i) {
        ListEntryView entry;
        entry.name = names[i];
        entry.is_dir = true;
        entry.record_only = false;
        entry.attrs = entry.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        entry.path = L"";
        entry.date_text = L"2026-09-11 14:32";
        entry.type_text = L"Folder";
        slot.pane.entries.push_back(entry);
        if (i < 2) slot.pane.change_badges[i] = {
            i == 0 ? L"Just now" : L"Yesterday", L"", 3, i == 1, i};
    }
    vm.pane_slots.push_back(std::move(slot));
    return vm;
}

bool DrawShot(Compositor& compositor, MainRenderer& renderer, WindowViewModel& vm,
              D2D1_RECT_F bounds, const wchar_t* path, bool dark) {
    vm.dark = dark;
    auto* dc = compositor.Dc();
    dc->BeginDraw();
    renderer.Render(vm, bounds, MakeTheme(dark, HexColor(0x0078D4)));
    if (FAILED(dc->EndDraw())) return false;
    const bool saved = compositor.SaveSnapshot(path);
    compositor.Present();
    return saved;
}
}

bool pulse::app::RunChangeTrackingUiTest() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return false;
    pulse::compat::EnableDpiAwareness();
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseChangeTrackingUiTest";
    RegisterClassW(&wc);
    const auto hwnd = CreateWindowExW(0, wc.lpszClassName, L"Tracking test", WS_OVERLAPPEDWINDOW,
        0, 0, 1440, 1080, nullptr, nullptr, instance, nullptr);
    bool ok = hwnd != nullptr;
    {
        Compositor compositor;
        if (!hwnd || !compositor.Init(hwnd)) return false;
        MainRenderer renderer;
        renderer.SetCompositor(&compositor);
        std::filesystem::create_directories(L"bench_data/change-ui");
        FILE* log = nullptr;
        if (fopen_s(&log, "bench_data/change-ui/results.log", "w") == 0 && log) std::fclose(log);
        ok &= Check(!pulse::l10n::Get(pulse::l10n::StringId::ChangeView).empty() &&
            !pulse::l10n::Get(pulse::l10n::StringId::SettingsChangeTracking).empty(),
            "tracking action and settings labels are available");
        for (const float scale : {1.0f, 1.5f, 2.0f}) {
            compositor.Resize(static_cast<int>(1100 * scale), static_cast<int>(740 * scale));
            compositor.RecreateTextFormats(scale);
            renderer.SetScale(scale);
            const auto bounds = D2D1::RectF(0, 0, 1100 * scale, 740 * scale);
            auto vm = Fixture(scale, bounds.right);
            const auto& pane = vm.pane_slots.front();
            const auto title = ChangeTitleRect(pane.rect,
                renderer.PaneNavBackRect(pane.rect, 0).left - 8 * scale,
                renderer.PaneHeaderHeight(), pane.pane.title_change_badge, scale, &compositor, pane.pane.header_text);
            const auto hit = renderer.HitTest(vm, bounds, (title.left + title.right) / 2,
                (title.top + title.bottom) / 2);
            ok &= Check(hit.region == HitTestResult::ChangeBadge && hit.index == -1 && hit.pane_index == 0,
                "title badge hit preserves pane and title index");
            const float row_y = pane.rect.top + renderer.PaneHeaderHeight() +
                renderer.ColumnHeaderHeight() + renderer.RowHeight() * 0.5f;
            bool row_hit = false;
            for (float x = pane.rect.left; x < pane.rect.right; x += scale) {
                const auto row = renderer.HitTest(vm, bounds, x, row_y);
                if (row.region == HitTestResult::ChangeBadge && row.index == 0 && row.pane_index == 0)
                    row_hit = true;
            }
            ok &= Check(row_hit, "row badge has an actionable hit region");
            vm.pane_slots.front().pane.change_badges[2] = {};
            vm.pane_slots.front().pane.change_badges[4] = {L"Unavailable", L"", 0, false, 2};
            bool empty_hit = false;
            for (const int row_index : {2, 4, 5}) {
                for (float x = pane.rect.left; x < pane.rect.right; x += scale) {
                    const auto row = renderer.HitTest(vm, bounds, x, row_y + row_index * renderer.RowHeight());
                    empty_hit |= row.region == HitTestResult::ChangeBadge;
                }
            }
            ok &= Check(!empty_hit, "ordinary folders and stale empty status entries have no badge hit");
            vm.pane_slots.front().pane.change_badges.erase(2);
            vm.pane_slots.front().pane.change_badges.erase(4);
            vm.change_popover = {true, bounds.right - 100 * scale, bounds.bottom - 50 * scale,
                L"Last 7 days\nLatest: today 14:32\nCreated 2\nModified 3\nDeleted 1\nRenamed 1\nIncludes descendants", 0, 0};
            const auto popup = ChangePopoverRect(vm.change_popover, bounds, scale);
            ok &= Check(popup.right <= bounds.right && popup.bottom <= bounds.bottom &&
                popup.bottom - popup.top >= (7 * 24 + 54) * scale - 1,
                "seven-line summary fits with separate action footer");
            const auto action = renderer.HitTest(vm, bounds, popup.left + 20 * scale, popup.bottom - 20 * scale);
            ok &= Check(action.region == HitTestResult::ChangeOpen && action.index == 0 && action.pane_index == 0,
                "popover action preserves source row");
            const float yesterday_y = row_y + renderer.RowHeight();
            bool yesterday_hit = false;
            for (float x = pane.rect.left; x < pane.rect.right; x += scale) {
                const auto row = renderer.HitTest(vm, bounds, x, yesterday_y);
                if (row.region != HitTestResult::ChangeBadge || row.index != 1) continue;
                vm.change_popover.x = x + 12 * scale;
                vm.change_popover.y = yesterday_y;
                vm.change_popover.row_index = 1;
                vm.change_popover.summary = L"Last 7 days\nLatest: yesterday 14:32\nModified 3\nIncludes descendants\nTracking has a gap";
                yesterday_hit = true;
                break;
            }
            ok &= Check(yesterday_hit, "preview popup anchored to Yesterday badge");
            for (const bool dark : {false, true}) {
                const auto path = L"bench_data/change-ui/badges-" + std::to_wstring(static_cast<int>(scale * 100)) +
                    (dark ? L"-dark.png" : L"-light.png");
                ok &= Check(DrawShot(compositor, renderer, vm, bounds, path.c_str(), dark), "badge screenshot");
            }
            vm.change_popover.visible = false;
            for (const bool dark : {false, true}) {
                const auto path = L"bench_data/change-ui/names-" + std::to_wstring(static_cast<int>(scale * 100)) +
                    (dark ? L"-dark.png" : L"-light.png");
                vm.pane_slots.front().pane.hover_index = 2;
                ok &= Check(DrawShot(compositor, renderer, vm, bounds, path.c_str(), dark), "long name with adaptive row actions screenshot");
            }
            vm.pane_slots.front().pane.hover_index = -1;
            for (const bool dark : {false, true}) {
                const auto path = L"bench_data/change-ui/names-idle-" + std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark.png" : L"-light.png");
                ok &= Check(DrawShot(compositor, renderer, vm, bounds, path.c_str(), dark), "ordinary folder idle screenshot");
            }
            vm.pane_slots.front().pane.view_mode = ViewMode::MediumIcons;
            const auto icons_path = L"bench_data/change-ui/icons-" + std::to_wstring(static_cast<int>(scale * 100)) + L".png";
            ok &= Check(DrawShot(compositor, renderer, vm, bounds, icons_path.c_str(), true), "icon view screenshot");
            const auto icons_light = L"bench_data/change-ui/icons-light-" + std::to_wstring(static_cast<int>(scale * 100)) + L".png";
            ok &= Check(DrawShot(compositor, renderer, vm, bounds, icons_light.c_str(), false), "light icon view screenshot");
            vm.settings_open = true;
            vm.settings_change_tracking = false;
            vm.settings_change_days = 7;
            fluent::Painter painter(&compositor);
            painter.SetScale(scale);
            const auto settings = MakeSettingsLayout(vm, bounds, scale, renderer.TitleBarHeight(), 28 * scale, &painter);
            vm.settings_scroll = std::max(0.0f, settings.change_tracking_row.top - 260 * scale);
            const auto settings_path = L"bench_data/change-ui/settings-" + std::to_wstring(static_cast<int>(scale * 100)) + L".png";
            ok &= Check(DrawShot(compositor, renderer, vm, bounds, settings_path.c_str(), true), "settings screenshot");
            vm.settings_open = false;
            vm.pane_slots.front().pane.view_mode = ViewMode::Details;
            auto& changes = vm.pane_slots.front().pane;
            changes.is_changes = true;
            changes.is_search = true;
            changes.header_text = L"Project files - changes";
            changes.change_time_label = pulse::l10n::Get(pulse::l10n::StringId::ChangeLast7Days);
            changes.change_type_label = pulse::l10n::Get(pulse::l10n::StringId::ChangeAll);
            changes.change_has_more = true;
            changes.change_badges.clear();
            changes.title_change_badge = {};
            changes.banner_message = pulse::l10n::Get(pulse::l10n::StringId::ChangeScope);
            changes.change_status_text = pulse::l10n::Get(pulse::l10n::StringId::ChangeRefreshFailed);
            changes.entries[3].name = L"deleted-report.docx";
            changes.entries[3].record_only = true;
            changes.entries[3].is_dir = false;
            changes.entries[3].type_text = pulse::l10n::Get(pulse::l10n::StringId::ChangeDeleted);
            changes.entries[3].path = L"Z:\\tracking-fixture\\deleted-report.docx";
            const auto results_path = L"bench_data/change-ui/results-" + std::to_wstring(static_cast<int>(scale * 100)) + L".png";
            ok &= Check(DrawShot(compositor, renderer, vm, bounds, results_path.c_str(), true), "change results screenshot");
            changes.change_status_text.clear();
            const auto saved_entries = changes.entries;
            changes.entries.clear();
            changes.change_empty_text = pulse::l10n::Get(pulse::l10n::StringId::ChangeReadFailed);
            vm.pane_slots.front().rect.right = vm.pane_slots.front().rect.left + 330 * scale;
            const float wrapped = PaneBannerHeight(changes, 330 * scale, scale, &compositor);
            ok &= Check(wrapped > 36 * scale, "narrow changes scope wraps and grows above controls");
            const auto empty_path = L"bench_data/change-ui/empty-error-" + std::to_wstring(static_cast<int>(scale * 100)) + L".png";
            ok &= Check(DrawShot(compositor, renderer, vm, bounds, empty_path.c_str(), true), "change read failure empty state screenshot");
            changes.loading = true;
            changes.change_empty_text = pulse::l10n::Get(pulse::l10n::StringId::ChangeScanning);
            const auto loading_path = L"bench_data/change-ui/empty-loading-" + std::to_wstring(static_cast<int>(scale * 100)) + L".png";
            ok &= Check(DrawShot(compositor, renderer, vm, bounds, loading_path.c_str(), true), "first change load shows scanning text without loading skeleton");
            changes.loading = false;
            changes.entries = saved_entries;
            changes.is_changes = false;
            changes.is_search = false;
            changes.banner_message.clear();
            vm.pane_slots.front().rect.right = vm.pane_slots.front().rect.left + 330 * scale;
            const auto narrow_path = L"bench_data/change-ui/narrow-" + std::to_wstring(static_cast<int>(scale * 100)) + L".png";
            ok &= Check(DrawShot(compositor, renderer, vm, bounds, narrow_path.c_str(), true), "narrow pane screenshot");
            const auto narrow_light = L"bench_data/change-ui/narrow-light-" + std::to_wstring(static_cast<int>(scale * 100)) + L".png";
            ok &= Check(DrawShot(compositor, renderer, vm, bounds, narrow_light.c_str(), false), "light narrow pane screenshot");
            const ChangeBadge badge{L"Yesterday", L"", 1, false, 1};
            const ChangeBadge long_status{L"Location not covered", L"", 0, false, 2};
            ok &= Check(ChangeBadgeWidth(long_status, scale, &compositor) == 0 &&
                ChangeBadgeWidth(ChangeBadge{}, scale, &compositor) == 0,
                "status and empty badges reserve no space");
            const auto trail = LayoutNameTrail(0, 0, 24 * scale, 80 * scale, 0, 30 * scale,
                scale, L"A very long folder name", 0, ChangeBadgeWidth(badge, scale, &compositor),
                false, false, false, &compositor, compositor.DwriteFactory(), compositor.TextFormat(), true);
            ok &= Check(trail.badge.right == trail.badge.left && trail.name_w <= 80 * scale,
                "narrow names hide badge without overlap");
            const auto stable = [&](bool hover) {
                return LayoutNameTrail(0, 0, 24 * scale, 420 * scale, 0, 30 * scale,
                    scale, L"A very long folder name that must stay still", 2,
                    ChangeBadgeWidth(badge, scale, &compositor), hover, hover, hover,
                    &compositor, compositor.DwriteFactory(), compositor.TextFormat(), true, 3);
            };
            const auto idle_trail = stable(false), hover_trail = stable(true);
            ok &= Check(idle_trail.name_w == hover_trail.name_w &&
                idle_trail.badge.left == hover_trail.badge.left && idle_trail.tag_x0 == hover_trail.tag_x0,
                "hover actions do not move long folder names, tags or change badge");
            ok &= Check(idle_trail.more.right == 0 && idle_trail.new_tab.right == 0,
                "reserved action space does not expose invisible hit targets");
            const auto ordinary = [&](bool hover) {
                return LayoutNameTrail(0, 0, 24 * scale, 420 * scale, 0, 30 * scale,
                    scale, L"DB.Browser.for.SQLite-v3.13.1-win64", 2, 0,
                    hover, hover, hover, &compositor, compositor.DwriteFactory(), compositor.TextFormat(), false, 3);
            };
            const auto ordinary_idle = ordinary(false), ordinary_hover = ordinary(true);
            ok &= Check(ordinary_idle.name_w == ordinary_hover.name_w &&
                ordinary_idle.tag_x0 == ordinary_hover.tag_x0 && ordinary_idle.badge.right == 0 &&
                ordinary_hover.badge.right == 0 && ordinary_idle.star.right == 0,
                "ordinary long folder name and tags stay still without empty badge or invisible actions");
            const std::wstring full_name = L"DB.Browser.for.SQLite-v3.13.1-win64";
            const float full_name_width = MeasureLayoutText(&compositor, compositor.DwriteFactory(), compositor.TextFormat(), full_name);
            const float badge_width = ChangeBadgeWidth(badge, scale, &compositor);
            const float column_width = full_name_width + badge_width + 64 * scale;
            const auto adaptive = LayoutNameTrail(0, 0, 24 * scale, column_width, 0, 30 * scale,
                scale, full_name, 0, badge_width, true, true, true,
                &compositor, compositor.DwriteFactory(), compositor.TextFormat(), true);
            ok &= Check(adaptive.name_w >= full_name_width - 0.5f && adaptive.show_star && adaptive.show_more &&
                !adaptive.show_new_tab && adaptive.new_tab.right == 0 && adaptive.badge.right <= adaptive.star.left,
                "crowded column reclaims new-tab slot to show complete filename without overlapping actions");
        }
    }
    DestroyWindow(hwnd);
    CoUninitialize();
    return ok;
}

#endif

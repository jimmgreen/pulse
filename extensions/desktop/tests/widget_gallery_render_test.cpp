// Renders every live widget at the sizes it offers, with fixed sample data, to one PNG per kind and theme.
// Used to review the widget faces (docs/design/desktop_widgets_redesign.html) before and after a change.
// Usage: pulse_desktop_widget_gallery_render_test.exe <output-dir> [photo.png]
#include "host/render/desktop_renderer.h"
#include "host/overlays/widget_icons.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pulse::desktop;

namespace {

int Span(int units) { return units * model::kTileUnitPx + (units - 1) * model::kTileGapPx; }

struct Variant {
    model::TileSize size;
    bool analog = true;
    int note_color = 0;
};

render::LiveTileRuntimeData SampleData(const std::wstring& photo) {
    render::LiveTileRuntimeData live;
    if (!photo.empty()) live.photo_paths = {photo};
    live.photo_total_count = 128;
    const int64_t now_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
    live.download_items = {
        {L"C:\\fixture\\季度报告 Q3.pdf", L"季度报告 Q3.pdf", L".pdf", false, 2'400'000, now_ms - 600'000},
        {L"C:\\fixture\\PulseSetup-1.4.2.exe", L"PulseSetup-1.4.2.exe", L".exe", false, 38'600'000, now_ms - 3'600'000},
        {L"C:\\fixture\\IMG_2048.heic", L"IMG_2048.heic", L".heic", false, 3'100'000, now_ms - 86'400'000},
        {L"C:\\fixture\\素材包.zip", L"素材包.zip", L".zip", false, 812'000'000, now_ms - 90'000'000},
        {L"C:\\fixture\\发布会录像.mp4", L"发布会录像.mp4", L".mp4", false, 1'200'000'000, now_ms - 200'000'000},
        {L"C:\\fixture\\预算 2027.xlsx", L"预算 2027.xlsx", L".xlsx", false, 86'000, now_ms - 210'000'000},
    };
    live.recent_items = live.download_items;
    live.recycle_bin = {12, 22'400'000};
    live.drives = {{L"C:\\", 128ull << 30, 512ull << 30}, {L"D:\\", 1100ull << 30, 1863ull << 30}};
    live.tray_entries = {{1, 812'000'000, false, false, L"C:\\fixture\\素材包.zip", L"素材包.zip"},
                         {2, 2'400'000, false, false, L"C:\\fixture\\季度报告 Q3.pdf", L"季度报告 Q3.pdf"},
                         {3, 3'100'000, false, false, L"C:\\fixture\\IMG_2048.heic", L"IMG_2048.heic"}};
    live.tray_total_bytes = 817'500'000;

    auto& w = live.weather;
    w.valid = true; w.city = L"上海"; w.temp_c = 10; w.code = 0; w.hi_c = 17; w.lo_c = 9;
    w.feels_c = 8; w.humidity_pct = 62; w.wind_kmh = 11; w.is_day = false; w.has_sun = true;
    w.updated = L"23:40";
    const int hour_codes[6] = {0, 0, 1, 1, 2, 3};
    for (int i = 0; i < 12; ++i)
        w.hours.push_back({i == 0 ? L"现在" : std::to_wstring((i) % 24) + L"时", hour_codes[i % 6], 10 - (i % 3), false});
    w.days = {{L"明天", 3, 9, 15}, {L"周六", 61, 8, 12}, {L"周日", 3, 6, 11}, {L"周一", 0, 5, 13}, {L"周二", 1, 7, 14}};

    auto& s = live.system;
    s.valid = true; s.cpu_pct = 27; s.cpu_pct_exact = 27.4f; s.mem_used = 19ull << 30; s.mem_total = 32ull << 30;
    s.mem_pct_exact = 61.0f; s.disk_used_pct = 75; s.disk_free = 128ull << 30; s.net_down_bps = 2'400'000;
    s.net_up_bps = 180'000; s.physical_cores = 16; s.logical_cores = 24; s.has_vram = true; s.vram_pct = 34.0f;
    s.vram_used_gb = 4.1f; s.vram_total_gb = 12.0f; s.gpu_name = L"NVIDIA GeForce RTX 4070";
    float c = 22.0f, m = 61.0f;
    for (int i = 0; i < 48; ++i) {
        c = std::clamp(c + static_cast<float>((i * 37) % 17) - 8.0f, 4.0f, 96.0f);
        m = std::clamp(m + static_cast<float>((i * 13) % 5) * 0.25f - 0.5f, 58.0f, 66.0f);
        s.cpu_history.push_back(c);
        s.mem_history.push_back(m);
        s.net_history.push_back(400.0f + static_cast<float>((i * 53) % 2600));
    }

    live.pomodoro.running = false;
    live.pomodoro.focus_minutes = 25;
    live.pomodoro.total_sec = 25 * 60;
    live.pomodoro.remaining_sec = 18 * 60 + 42;
    live.pomodoro.completed_today = 2;

    live.media.active = true; live.media.playing = true;
    live.media.title = L"海边的星期三"; live.media.artist = L"Lumen Quartet"; live.media.album = L"Slow Tide";
    live.media.source_app = L"网易云音乐";

    auto& b = live.battery;
    b.valid = true; b.has_battery = true; b.battery_pct = 76; b.charging = true; b.ac_online = true;
    b.uptime_sec = (3 * 24 + 4) * 3600; b.power_plan = L"平衡";
    b.bt_devices = {{L"耳机", 80, 1}, {L"鼠标", 45, 2}, {L"键盘", 15, 3}};
    return live;
}

std::vector<Variant> VariantsFor(model::TileKind kind) {
    std::vector<Variant> out;
    for (const auto& sz : model::AllowedTileSizesFor(kind)) {
        if (sz.rows > 4) continue;  // keep each sheet a reasonable height
        out.push_back({sz});
    }
    if (kind == model::TileKind::Clock) {
        for (const model::TileSize sz : {model::TileSize{2, 1}, model::TileSize{2, 2}}) out.push_back({sz, false});
    }
    if (kind == model::TileKind::StickyNote) {
        for (int c = 1; c < 6; ++c) out.push_back({{2, 2}, true, c});
    }
    return out;
}

const wchar_t* KindName(model::TileKind k) {
    switch (k) {
    case model::TileKind::Photos: return L"photos";
    case model::TileKind::Downloads: return L"downloads";
    case model::TileKind::Recent: return L"recent";
    case model::TileKind::RecycleBin: return L"recycle";
    case model::TileKind::ThisPC: return L"thispc";
    case model::TileKind::DiskRing: return L"disk";
    case model::TileKind::PulseLauncher: return L"launcher";
    case model::TileKind::Clock: return L"clock";
    case model::TileKind::StickyNote: return L"note";
    case model::TileKind::StagingTray: return L"staging";
    case model::TileKind::Weather: return L"weather";
    case model::TileKind::Calendar: return L"calendar";
    case model::TileKind::Todo: return L"todo";
    case model::TileKind::System: return L"system";
    case model::TileKind::Pomodoro: return L"pomodoro";
    case model::TileKind::MediaPlayer: return L"media";
    case model::TileKind::Countdown: return L"countdown";
    case model::TileKind::Battery: return L"battery";
    default: return nullptr;
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 3;
    const std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    const std::wstring photo = argc >= 3 ? argv[2] : L"";
    const std::wstring only = argc >= 4 ? argv[3] : L"";
    int failures = 0;
    {
        auto live = SampleData(photo);
        // PULSE_WIDGET_EMPTY=1: every source empty or unavailable, to check the empty and error states.
        if (char e[4]{}; GetEnvironmentVariableA("PULSE_WIDGET_EMPTY", e, 4) > 0 && e[0] == '1') {
            live.photo_paths.clear(); live.download_items.clear(); live.recent_items.clear();
            live.recycle_bin = {}; live.tray_entries.clear(); live.tray_total_bytes = 0;
            live.weather = {}; live.weather.status = L"连不上天气服务";
            live.system = {}; live.media = {}; live.battery = {};
        }
        for (int k = 0; k <= static_cast<int>(model::kLastTileKind); ++k) {
            const auto kind = static_cast<model::TileKind>(k);
            const wchar_t* name = KindName(kind);
            if (!name || (!only.empty() && only != name)) continue;
            const auto variants = VariantsFor(kind);
            // Lay the sizes out left to right, wrapping at 1400 DIP.
            int x = 24, y = 24, row_h = 0, width = 0;
            std::vector<model::DesktopTile> tiles;
            for (size_t i = 0; i < variants.size(); ++i) {
                const auto& v = variants[i];
                const int w = Span(v.size.cols), h = Span(v.size.rows);
                if (x + w > 1400) { x = 24; y += row_h + 24; row_h = 0; }
                model::DesktopTile t{};
                t.id = "g" + std::to_string(i);
                t.kind = kind;
                t.size = v.size;
                t.free_placed = true;
                t.x = x; t.y = y; t.w = w; t.h = h;
                t.analog_clock = v.analog;
                t.note_color = v.note_color;
                t.live_content = true;
                t.note_body = L"周五前寄发票\n顺丰到付，单号写在快递单背面。";
                tiles.push_back(t);
                x += w + 24;
                row_h = std::max(row_h, h);
                width = std::max(width, x);
            }
            const int height = y + row_h + 24;
            render::DesktopRenderer renderer;
            if (!renderer.InitializeOffscreen(static_cast<uint32_t>(width), static_cast<uint32_t>(height))) { ++failures; continue; }
            for (const bool dark : {true, false}) {
                model::DesktopModelState state;
                state.settings.organize_enabled = true;
                state.settings.tiles_enabled = true;
                state.settings.dark_mode = dark;
                state.settings.wallpaper_preset = dark ? model::WallpaperPreset::Dusk : model::WallpaperPreset::Forest;
                state.settings.weather_city = L"上海";
                // PULSE_WIDGET_SCHEME=1/2/3 renders 浅色 / 深色 / 着色 (小组件配色); unset: 自动.
                if (char sc[8]{}; GetEnvironmentVariableA("PULSE_WIDGET_SCHEME", sc, 8) > 0) state.settings.widgets.scheme = static_cast<uint8_t>(std::atoi(sc) & 3);
                state.tiles = tiles;
                state.todos = {{L"给王老师回邮件", false}, {L"15:00 项目评审", false}, {L"续费域名", false},
                               {L"订周五的会议室", false}, {L"交周报", true}, {L"整理桌面截图", true}};
                const auto file = output / (std::wstring(name) + (dark ? L"-dark.png" : L"-light.png"));
                const bool ok = renderer.RenderCompositePng(file.wstring(), state, {}, {}, live, {});
                std::printf("[%s] %ls\n", ok ? "PASS" : "FAIL", file.filename().c_str());
                failures += !ok;
            }
        }
    }
    {
        // The 添加小组件 drawer's preview path: one tile through RenderTilePixels, written raw (w, h, BGRA).
        render::DesktopRenderer pr;
        if (pr.InitializeOffscreen(440, 440)) {
            model::DesktopModelState st;
            st.settings.dark_mode = true;
            st.settings.tiles_enabled = true;
            st.settings.wallpaper_preset = model::WallpaperPreset::Dusk;
            model::DesktopTile t{};
            t.id = "p";
            t.kind = model::TileKind::Pomodoro;
            t.size = {2, 2};
            t.live_content = true;
            std::vector<uint8_t> px;
            uint32_t w = 0, h = 0;
            const bool ok = pr.RenderTilePixels(t, st, SampleData(photo), px, w, h);
            if (ok) {
                std::ofstream f(output / L"preview-pomodoro.raw", std::ios::binary);
                f.write(reinterpret_cast<const char*>(&w), 4);
                f.write(reinterpret_cast<const char*>(&h), 4);
                f.write(reinterpret_cast<const char*>(px.data()), static_cast<std::streamsize>(px.size()));
            }
            std::printf("[%s] preview pixels %ux%u\n", ok ? "PASS" : "FAIL", w, h);
            failures += !ok;
        }
    }
    {
        // The 添加小组件 icons: every kind at the gallery's 32 DIP and at 64 DIP, on a dark and a light wall.
        const model::TileKind kinds[] = {model::TileKind::Photos, model::TileKind::Weather, model::TileKind::Calendar,
            model::TileKind::Todo, model::TileKind::Clock, model::TileKind::StickyNote, model::TileKind::Apps,
            model::TileKind::Recent, model::TileKind::Downloads, model::TileKind::System, model::TileKind::StagingTray,
            model::TileKind::DiskRing, model::TileKind::Pomodoro, model::TileKind::MediaPlayer, model::TileKind::Countdown,
            model::TileKind::Battery};
        Microsoft::WRL::ComPtr<IDWriteFactory> dw;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
        if (dw) dw->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-CN", &fmt);
        render::DesktopRenderer renderer;
        if (renderer.InitializeOffscreen(16 * 84 + 24, 200)) {
            for (const bool dark : {true, false}) {
                model::DesktopModelState state;
                state.settings.dark_mode = dark;
                state.settings.tiles_enabled = false;
                state.settings.icons_hidden = true;
                state.settings.wallpaper_preset = dark ? model::WallpaperPreset::Dusk : model::WallpaperPreset::Forest;
                const auto file = output / (dark ? L"icons-dark.png" : L"icons-light.png");
                const bool ok = renderer.RenderCompositePng(file.wstring(), state, {}, {}, {}, {}, [&](ID2D1DeviceContext* dc) {
                    for (size_t i = 0; i < std::size(kinds); ++i) {
                        const float x = 24.0f + 84.0f * static_cast<float>(i);
                        overlays::DrawWidgetIcon(dc, kinds[i], D2D1::RectF(x + 16.0f, 24.0f, x + 48.0f, 56.0f), 8.5f, fmt.Get());
                        overlays::DrawWidgetIcon(dc, kinds[i], D2D1::RectF(x, 96.0f, x + 64.0f, 160.0f), 17.0f, fmt.Get());
                    }
                });
                std::printf("[%s] %ls\n", ok ? "PASS" : "FAIL", file.filename().c_str());
                failures += !ok;
            }
        }
    }
    CoUninitialize();
    return failures ? 1 : 0;
}

#include "../../glass_control_labels.h"
#include "host/overlays/desktop_overlays.h"
#include "host/overlays/widget_icons.h"
#include "host/render/vector_glyphs.h"
#include "host/render/liquid_dock_bar.h"
#include "../../../../src/common/localization.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <filesystem>

namespace pulse::desktop::overlays {
namespace {
const wchar_t* WidgetName(model::TileKind k) {
    switch (k) {
    case model::TileKind::Photos: return L"照片";
    case model::TileKind::Weather: return L"天气";
    case model::TileKind::Calendar: return L"日历";
    case model::TileKind::Todo: return L"待办";
    case model::TileKind::Clock: return L"时钟";
    case model::TileKind::StickyNote: return L"便笺";
    case model::TileKind::Apps: return L"应用坞";
    case model::TileKind::Recent: return L"最近使用";
    case model::TileKind::Downloads: return L"下载";
    case model::TileKind::System: return L"系统";
    case model::TileKind::StagingTray: return L"暂存区";
    case model::TileKind::DiskRing: return L"磁盘";
    case model::TileKind::RecycleBin: return L"回收站";
    case model::TileKind::ThisPC: return L"此电脑";
    case model::TileKind::PulseLauncher: return L"Pulse";
    case model::TileKind::Pomodoro: return L"番茄专注";
    case model::TileKind::MediaPlayer: return L"正在播放";
    case model::TileKind::Countdown: return L"倒数日";
    case model::TileKind::Battery: return L"电池设备";
    default: return L"磁贴";
    }
}
wchar_t WidgetGlyph(model::TileKind k) {
    switch (k) {
    case model::TileKind::Photos: return L'\xEB9F';
    case model::TileKind::Weather: return L'\xE706';
    case model::TileKind::Calendar: return L'\xE787';
    case model::TileKind::Todo: return L'\xE73A';
    case model::TileKind::Clock: return L'\xE823';
    case model::TileKind::StickyNote: return L'\xE70F';
    case model::TileKind::Apps: return L'\xE71D';
    case model::TileKind::Recent: return L'\xE81C';
    case model::TileKind::Downloads: return L'\xE896';
    case model::TileKind::System: return L'\xE9D9';
    case model::TileKind::StagingTray: return L'\xE7B8';
    case model::TileKind::DiskRing: return L'\xEDA2';
    case model::TileKind::RecycleBin: return L'\xE74D';
    case model::TileKind::ThisPC: return L'\xE977';
    case model::TileKind::PulseLauncher: return L'\xEC50';
    case model::TileKind::Pomodoro: return L'\xE916';
    case model::TileKind::MediaPlayer: return L'\xE8D6';
    case model::TileKind::Countdown: return L'\xE787';
    case model::TileKind::Battery: return L'\xE83F';
    default: return L'\xE8A5';
    }
}

const wchar_t* WidgetDesc(model::TileKind k) {
    switch (k) {
    case model::TileKind::Photos: return L"照片文件夹里的图片轮流显示；点开在 Pulse 里看全部。";
    case model::TileKind::Weather: return L"当前位置的温度、今天接下来几个小时和未来三天；位置用 Windows 的设置。";
    case model::TileKind::Calendar: return L"今天的日期和接下来的日程，来自 Windows 日历里登录的账户。";
    case model::TileKind::Todo: return L"直接在桌面上勾掉；和 Microsoft To Do 同步。";
    case model::TileKind::Clock: return L"表盘或数字，跟随系统时间和时区。";
    case model::TileKind::StickyNote: return L"随手记一句，自动保存，演示时隐藏。";
    case model::TileKind::Apps: return L"macOS 风格液态玻璃程序坞，支持悬停放大、运行状态点与点击弹跳。";
    case model::TileKind::StagingTray: return L"和 Pulse 的暂存区是同一份，桌面和 Pulse 之间拖进拖出。";
    case model::TileKind::Downloads: return L"最新下载的文件排在最前，拖出来就能用。";
    case model::TileKind::Recent: return L"Pulse 和应用里最近打开的文件。";
    case model::TileKind::System: return L"处理器、内存、磁盘和网速；点开任务管理器。";
    case model::TileKind::DiskRing: return L"剩余空间，快满时变色提醒。";
    case model::TileKind::RecycleBin: return L"显示回收站占用空间，右键一键清空。";
    case model::TileKind::ThisPC: return L"快速打开此电脑与系统存储管理。";
    case model::TileKind::PulseLauncher: return L"一键唤起 Pulse 主窗口与全盘秒搜。";
    case model::TileKind::Pomodoro: return L"桌面直接开始/暂停番茄钟，支持 25/45 分钟专注与休息切换。";
    case model::TileKind::MediaPlayer: return L"实时读取系统正在播放的音乐封面与歌名，桌面直接切歌和暂停。";
    case model::TileKind::Countdown: return L"重要日程倒数天数与今日/本周/本月/今年时间进度条，右键可改目标。";
    case model::TileKind::Battery: return L"本机电量、电源模式、运行时间以及已连接蓝牙外设电量一览。";
    default: return L"固定文件夹或常用项目到桌面磁贴区。";
    }
}

struct GalleryWidgetDef {
    model::TileKind kind;
    const wchar_t* name;
    wchar_t glyph;
    uint32_t badge_rgb;
    std::initializer_list<model::TileSize> sizes;
};

static const GalleryWidgetDef kGalleryWidgets[16] = {
    {model::TileKind::Photos,      L"照片",     L'\xEB9F', 0xFFE08A3Cu, {{2, 2}, {4, 2}, {4, 4}}},
    {model::TileKind::Weather,     L"天气",     L'\xE706', 0xFF3D9BE9u, {{1, 1}, {2, 1}, {4, 1}, {2, 2}, {4, 2}}},
    {model::TileKind::Calendar,    L"日历",     L'\xE787', 0xFFE5484Du, {{1, 1}, {2, 1}, {4, 1}, {2, 2}, {4, 2}}},
    {model::TileKind::Todo,        L"待办",     L'\xE73A', 0xFF30A46Cu, {{1, 1}, {2, 1}, {4, 1}, {2, 2}, {4, 2}, {4, 4}}},
    {model::TileKind::Clock,       L"时钟",     L'\xE823', 0xFFF08A3Cu, {{1, 1}, {2, 1}, {4, 1}, {2, 2}}},
    {model::TileKind::StickyNote,  L"便笺",     L'\xE70B', 0xFFD9A400u, {{1, 1}, {2, 2}, {4, 2}, {4, 4}}},
    {model::TileKind::Apps,        L"应用坞",   L'\xE71D', 0xFF5B6BE0u, {{4, 1}, {2, 1}, {4, 2}, {2, 2}, {1, 1}}},
    {model::TileKind::Recent,      L"最近使用", L'\xE81C', 0xFF7C66DCu, {{1, 1}, {2, 2}, {4, 2}}},
    {model::TileKind::Downloads,   L"下载",     L'\xE896', 0xFF2F8FE8u, {{1, 1}, {2, 2}, {4, 2}}},
    {model::TileKind::System,      L"系统",     L'\xE9D9', 0xFF5A72E8u, {{1, 1}, {2, 1}, {4, 1}, {2, 2}, {4, 2}, {4, 4}}},
    {model::TileKind::StagingTray, L"暂存区",   L'\xE7B8', 0xFF9A6BDBu, {{1, 1}, {2, 1}, {4, 1}, {2, 2}, {4, 2}}},
    {model::TileKind::DiskRing,    L"磁盘",     L'\xEDA2', 0xFFD4952Au, {{1, 1}}},
    {model::TileKind::Pomodoro,    L"番茄专注", L'\xE916', 0xFFFF5A52u, {{1, 1}, {2, 1}, {2, 2}, {4, 2}}},
    {model::TileKind::MediaPlayer, L"正在播放", L'\xE8D6', 0xFFFF4D8Du, {{2, 1}, {4, 1}, {2, 2}, {4, 2}}},
    {model::TileKind::Countdown,   L"倒数日",   L'\xE787', 0xFF6B7BFFu, {{1, 1}, {2, 1}, {2, 2}, {4, 2}}},
    {model::TileKind::Battery,     L"电池设备", L'\xE83F', 0xFF30D158u, {{1, 1}, {2, 1}, {2, 2}, {4, 2}}},
};

const GalleryWidgetDef& FindGalleryWidgetDef(model::TileKind k) noexcept {
    for (const auto& w : kGalleryWidgets) {
        if (w.kind == k) return w;
    }
    return kGalleryWidgets[1];
}

const wchar_t* SizeShortLabel(model::TileSize sz) noexcept {
    if (sz.cols == 1 && sz.rows == 1) return L"小 1×1";
    if (sz.cols == 2 && sz.rows == 1) return L"窄条 2×1";
    if (sz.cols == 4 && sz.rows == 1) return L"宽条 4×1";
    if (sz.cols == 2 && sz.rows == 2) return L"中 2×2";
    if (sz.cols == 4 && sz.rows == 2) return L"宽 4×2";
    return L"大 4×4";
}

std::wstring SizeDims(model::TileSize sz) {
    return std::to_wstring(sz.cols) + L"×" + std::to_wstring(sz.rows);
}

std::wstring StripMenuMnemonic(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if ((s[i] == L'(' || s[i] == L'（') && i + 3 < s.size() && s[i + 1] == L'&' &&
            (s[i + 3] == L')' || s[i + 3] == L'）')) {
            i += 3;
            continue;
        }
        if ((s[i] == L'(' || s[i] == L'（') && i + 2 < s.size() &&
            ((s[i + 1] >= L'A' && s[i + 1] <= L'Z') || (s[i + 1] >= L'a' && s[i + 1] <= L'z')) &&
            (s[i + 2] == L')' || s[i + 2] == L'）')) {
            i += 2;
            continue;
        }
        if (s[i] == L'&') continue;
        out.push_back(s[i]);
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'\t' || out.back() == L'.' || out.back() == L'…')) {
        out.pop_back();
    }
    size_t lead = 0;
    while (lead < out.size() && (out[lead] == L' ' || out[lead] == L'\t')) ++lead;
    if (lead > 0) out.erase(0, lead);
    return out;
}

bool EqualsIgnoreCaseW(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

bool IsShellNewSubmenuHeader(const ShellVerbEntry& sv) {
    if (sv.depth != 0) return false;
    if (_wcsicmp(sv.handler.c_str(), L"New") == 0 || _wcsicmp(sv.verb.c_str(), L"New") == 0) return true;
    const std::wstring clean = StripMenuMnemonic(sv.label);
    return clean == L"新建" || EqualsIgnoreCaseW(clean, L"new");
}

bool IsDuplicateShellNewChild(std::wstring_view label) {
    const std::wstring clean = StripMenuMnemonic(label);
    return clean == L"文件夹" || EqualsIgnoreCaseW(clean, L"folder") ||
           EqualsIgnoreCaseW(clean, L"new folder") ||
           clean == L"文本文档" || clean == L"新建文本文档" ||
           EqualsIgnoreCaseW(clean, L"text document") ||
           EqualsIgnoreCaseW(clean, L"markdown") || clean == L"Markdown 文档";
}

bool IsDuplicateBlankDesktopShellVerb(const ShellVerbEntry& sv) {
    if (sv.depth != 0) return false;
    const std::wstring clean = StripMenuMnemonic(sv.label);
    const std::wstring& v = sv.verb;
    if (clean == L"查看" || EqualsIgnoreCaseW(clean, L"view") || EqualsIgnoreCaseW(v, L"view")) return true;
    if (clean == L"排序方式" || EqualsIgnoreCaseW(clean, L"sort by") || EqualsIgnoreCaseW(v, L"arrange")) return true;
    if (clean == L"刷新" || EqualsIgnoreCaseW(clean, L"refresh") || EqualsIgnoreCaseW(v, L"refresh")) return true;
    if (clean == L"撤销" || clean.starts_with(L"撤销") || EqualsIgnoreCaseW(clean, L"undo") || EqualsIgnoreCaseW(v, L"undo")) return true;
    if (clean == L"显示设置" || EqualsIgnoreCaseW(clean, L"display settings")) return true;
    if (clean == L"个性化" || EqualsIgnoreCaseW(clean, L"personalize")) return true;
    if (clean == L"在终端中打开" || clean == L"在 Windows 终端中打开" ||
        EqualsIgnoreCaseW(clean, L"open in terminal") ||
        EqualsIgnoreCaseW(clean, L"open in windows terminal") ||
        EqualsIgnoreCaseW(v, L"openinterminal") || EqualsIgnoreCaseW(v, L"wt")) {
        return true;
    }
    return false;
}
} // namespace

using render::DrawGlyph;
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kOverlayWndClass[] = L"PulseDesktopOverlayPopup";
constexpr wchar_t kSubOverlayWndClass[] = L"PulseDesktopSubOverlayPopup";
constexpr UINT_PTR kToastTimerId = 9001u;

D2D1_COLOR_F ArgbColor(uint32_t argb, float opacity_mul = 1.0f) noexcept {
    const float a = (static_cast<float>((argb >> 24) & 0xFFu) / 255.0f) * opacity_mul;
    const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(argb & 0xFFu) / 255.0f;
    return D2D1::ColorF(r, g, b, std::clamp(a, 0.0f, 1.0f));
}

std::wstring FormatSizeShort(uint64_t bytes) {
    wchar_t buf[64]{};
    if (bytes == 0) return L"0 B";
    if (bytes < 1024ull) {
        std::swprintf(buf, ARRAYSIZE(buf), L"%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ull * 1024ull) {
        std::swprintf(buf, ARRAYSIZE(buf), L"%.0f KB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024ull * 1024ull * 1024ull) {
        const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
        std::swprintf(buf, ARRAYSIZE(buf), mb >= 100.0 ? L"%.0f MB" : L"%.1f MB", mb);
    } else {
        const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
        std::swprintf(buf, ARRAYSIZE(buf), L"%.1f GB", gb);
    }
    return buf;
}

bool PtInRectXY(const RECT& r, int x, int y) noexcept {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

void OverlayLog(const char* fmt, ...) noexcept {
    char line[400]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    wchar_t dir[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    const std::wstring path = std::wstring(dir) + L"\\Pulse\\pulse_desktop_host.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") == 0 && f) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        std::fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u overlay %s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, line);
        std::fclose(f);
    }
}

void ConfigureOverlayDwmAttributes(HWND hwnd, bool transparent_dcomp) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return;
    constexpr DWORD kDwmwaWindowCornerPreference = 33;
    constexpr DWORD kDwmwaBorderColor = 34;
    constexpr DWORD kDwmwcpDoNotRound = 1;
    constexpr DWORD kDwmwcpRound = 2;
    constexpr COLORREF kDwmwaColorNone = 0xFFFFFFFEu;

    const DWORD corner = transparent_dcomp ? kDwmwcpDoNotRound : kDwmwcpRound;
    DwmSetWindowAttribute(hwnd, kDwmwaWindowCornerPreference, &corner, sizeof(corner));
    if (transparent_dcomp) {
        const COLORREF border_none = kDwmwaColorNone;
        DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &border_none, sizeof(border_none));
    }
}

D2D1_RECT_F SnapRectF(const D2D1_RECT_F& rc) noexcept {
    return D2D1::RectF(std::round(rc.left), std::round(rc.top),
                       std::round(rc.right), std::round(rc.bottom));
}

D2D1_ROUNDED_RECT SnapRoundedRect(const D2D1_RECT_F& rc, float radius) noexcept {
    return D2D1::RoundedRect(SnapRectF(rc), radius, radius);
}

void StrokeCrispRoundedRect(ID2D1RenderTarget* dc, const D2D1_RECT_F& rc, float radius,
                            ID2D1Brush* brush, float stroke_width = 1.0f) {
    if (!dc || !brush || stroke_width <= 0.0f) return;
    const D2D1_RECT_F s = SnapRectF(rc);
    const float half = stroke_width * 0.5f;
    const D2D1_RECT_F inset = D2D1::RectF(s.left + half, s.top + half,
                                          s.right - half, s.bottom - half);
    if (inset.right <= inset.left || inset.bottom <= inset.top) return;
    const float r = std::max(0.0f, radius - half);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(inset, r, r), brush, stroke_width);
}

size_t FindCaseInsensitivePos(std::wstring_view haystack, std::wstring_view needle) noexcept {
    if (needle.empty() || haystack.size() < needle.size()) return std::wstring_view::npos;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::towlower(haystack[i + j]) != std::towlower(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return i;
    }
    return std::wstring_view::npos;
}

int HealthCategoryIndex(model::HealthIssueKind kind) noexcept {
    switch (kind) {
    case model::HealthIssueKind::InstalledInstaller:
        return 0;
    case model::HealthIssueKind::StaleScreenshot:
    case model::HealthIssueKind::BrokenShortcut:
        return 1;
    case model::HealthIssueKind::EmptyFolder:
        return 2;
    case model::HealthIssueKind::OneTimeUse:
    case model::HealthIssueKind::DuplicateOrTemp:
    default:
        return 3;
    }
}

std::wstring ParentFolderDisplay(const std::wstring& full_path) {
    if (full_path.empty()) return L"桌面";
    std::filesystem::path p(full_path);
    const std::wstring parent = p.parent_path().filename().wstring();
    if (!parent.empty()) return parent;
    return p.parent_path().wstring();
}

std::wstring FormatAgeDaysShort(int32_t age_days) {
    if (age_days <= 0) return L"今天";
    wchar_t buf[32]{};
    std::swprintf(buf, ARRAYSIZE(buf), L"%d天前", static_cast<int>(age_days));
    return buf;
}

void DrawSparkleStar(ID2D1Factory* fac, ID2D1RenderTarget* rt,
                     float cx, float cy, float r, ID2D1Brush* brush) {
    if (!fac || !rt || !brush || r <= 0.5f) return;
    auto draw_one = [&](float x, float y, float rad) {
        ComPtr<ID2D1PathGeometry> geo;
        if (FAILED(fac->CreatePathGeometry(&geo)) || !geo) return;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geo->Open(&sink)) || !sink) return;
        const float k = rad * 0.24f;
        sink->BeginFigure(D2D1::Point2F(x, y - rad), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
            D2D1::Point2F(x + k, y - k), D2D1::Point2F(x + rad, y)));
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
            D2D1::Point2F(x + k, y + k), D2D1::Point2F(x, y + rad)));
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
            D2D1::Point2F(x - k, y + k), D2D1::Point2F(x - rad, y)));
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
            D2D1::Point2F(x - k, y - k), D2D1::Point2F(x, y - rad)));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (SUCCEEDED(sink->Close())) {
            rt->FillGeometry(geo.Get(), brush);
        }
    };
    draw_one(cx - r * 0.10f, cy + r * 0.08f, r * 0.88f);
    draw_one(cx + r * 0.62f, cy - r * 0.58f, r * 0.36f);
}

struct RuleTypeChipDef {
    const char* id;
    const wchar_t* label;
    const wchar_t* primary_ext;
    std::initializer_list<const wchar_t*> exts;
    bool is_folder_toggle;
};

static const RuleTypeChipDef kRuleTypeChips[11] = {
    {"rt:word",  L"Word",     L".docx", {L".docx", L".doc"}, false},
    {"rt:excel", L"Excel",    L".xlsx", {L".xlsx", L".xls", L".csv"}, false},
    {"rt:ppt",   L"演示",     L".pptx", {L".pptx", L".ppt"}, false},
    {"rt:pdf",   L"PDF",      L".pdf",  {L".pdf"}, false},
    {"rt:md",    L"Markdown", L".md",   {L".md"}, false},
    {"rt:txt",   L"文本",     L".txt",  {L".txt", L".log", L".json"}, false},
    {"rt:img",   L"图片",     L".png",  {L".png", L".jpg", L".jpeg", L".webp", L".bmp", L".gif"}, false},
    {"rt:exe",   L".exe",     L".exe",  {L".exe"}, false},
    {"rt:msi",   L".msi",     L".msi",  {L".msi"}, false},
    {"rt:zip",   L".zip",     L".zip",  {L".zip", L".7z", L".rar"}, false},
    {"rt:dir",   L"文件夹",   L"",      {}, true},
};

// Thread-local temporary brush for CompositeOnto external D2D device context
thread_local ComPtr<ID2D1SolidColorBrush> g_ext_brush;
thread_local ID2D1DeviceContext* g_ext_brush_dc = nullptr;

} // namespace

DesktopOverlayController::DesktopOverlayController() {
    static std::once_flag strings;
    std::call_once(strings, [] { l10n::Initialize(GetModuleHandleW(nullptr), L"system"); });
}

DesktopOverlayController::~DesktopOverlayController() {
    Shutdown();
}

bool DesktopOverlayController::Initialize(OverlayCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
    menu_policy_.Start();
    return EnsureWindowCreated();
}

void DesktopOverlayController::Shutdown() noexcept {
    menu_policy_.Stop();
    CancelInlineRename(false);
    DestroyHostedPopupEdit();
    Dismiss();
    shared_menu_.reset();
    shared_menu_compositor_.reset();
    ui_compositor_scale_ = 0.0f;
    if (ui_device_hwnd_ && IsWindow(ui_device_hwnd_)) DestroyWindow(ui_device_hwnd_);
    ui_device_hwnd_ = nullptr;
    if (sub_hwnd_ && IsWindow(sub_hwnd_)) {
        DestroyWindow(sub_hwnd_);
    }
    sub_hwnd_ = nullptr;
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    if (popup_edit_font_14_) {
        DeleteObject(popup_edit_font_14_);
        popup_edit_font_14_ = nullptr;
    }
    if (popup_edit_font_13_) {
        DeleteObject(popup_edit_font_13_);
        popup_edit_font_13_ = nullptr;
    }
    if (popup_edit_brush_) {
        DeleteObject(popup_edit_brush_);
        popup_edit_brush_ = nullptr;
    }

    ReleaseRenderDevices();
}

bool DesktopOverlayController::EnsureWindowCreated() {
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    if (!hwnd_ || !IsWindow(hwnd_)) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &DesktopOverlayController::StaticWndProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kOverlayWndClass;
        RegisterClassExW(&wc);

        hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
            kOverlayWndClass, L"", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, hinst, this);
        if (!hwnd_) {
            hwnd_ = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                kOverlayWndClass, L"", WS_POPUP,
                0, 0, 1, 1, nullptr, nullptr, hinst, this);
        }
    }

    if (!sub_hwnd_ || !IsWindow(sub_hwnd_)) {
        WNDCLASSEXW swc{};
        swc.cbSize = sizeof(swc);
        swc.lpfnWndProc = &DesktopOverlayController::StaticSubWndProc;
        swc.hInstance = hinst;
        swc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        swc.lpszClassName = kSubOverlayWndClass;
        RegisterClassExW(&swc);

        sub_hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            kSubOverlayWndClass, L"", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, hinst, this);
        if (!sub_hwnd_) {
            sub_hwnd_ = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                kSubOverlayWndClass, L"", WS_POPUP,
                0, 0, 1, 1, nullptr, nullptr, hinst, this);
        }
    }

    return hwnd_ != nullptr;
}

void DesktopOverlayController::ReleaseRenderDevices() noexcept {
    detail_text_.Reset();
    dock_tray_icon_cache_.clear();
    dock_tray_icon_dc_ = nullptr;
    beautify_img_cache_.clear();
    beautify_live_bmp_.Reset();
    beautify_bmp_dc_ = nullptr;
    beautify_wic_.Reset();
    DestroySwapChain();
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    sub_dcomp_visual_.Reset();
    sub_dcomp_target_.Reset();
    popup_shadow_bitmap_.Reset();

    fmt_gallery_big_.Reset();
    fmt_fluent_20_.Reset();
    fmt_fluent_15_.Reset();
    fmt_fluent_12_.Reset();
    fmt_mono_.Reset();
    fmt_small_right_.Reset();
    fmt_small_center_.Reset();
    fmt_small_.Reset();
    fmt_stat_num_.Reset();
    fmt_title_.Reset();
    fmt_body_semi_.Reset();
    fmt_body_.Reset();
    dwrite_.Reset();

    brush_.Reset();
    dcomp_device_.Reset();
    ID3D11Device* raw_d3d = d3d_device_.Get();
    d2d_dc_.Reset();
    d2d_device_.Reset();
    d2d_factory_.Reset();
    dxgi_device_.Reset();
    d3d_device_.Reset();
    if (raw_d3d) {
        ui::Compositor::ReleaseSharedThreadDevices(raw_d3d);
    }
}

bool DesktopOverlayController::EnsureRenderDevices() {
    if (d2d_dc_ && dwrite_) return true;

    IDCompositionDevice* shared_dcomp = nullptr;
    if (!d3d_device_ || !dxgi_device_ || !d2d_factory_ || !d2d_device_) {
        ui::Compositor::SharedThreadDevices shared{};
        if (!ui::Compositor::AcquireSharedThreadDevices(shared) || !shared.d3d || !shared.dxgi ||
            !shared.d2d_factory || !shared.d2d) {
            return false;
        }
        d3d_device_ = shared.d3d;
        dxgi_device_ = shared.dxgi;
        d2d_factory_ = shared.d2d_factory;
        d2d_device_ = shared.d2d;
        shared_dcomp = shared.dcomp;
    }
    d2d_device_->SetMaximumTextureMemory(6u * 1024u * 1024u);
    if (!d2d_dc_ && FAILED(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_dc_))) return false;
    d2d_dc_->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    if (!brush_ && FAILED(d2d_dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_))) return false;

    if (!dcomp_device_) {
        // Use a composition-only IDCompositionDevice so sharing the UI thread's
        // IDXGIDevice with DesktopRenderer never fails with E_ACCESSDENIED.
        if (FAILED(ui::Compositor::CreateCompositionOnlyDevice(&dcomp_device_))) {
            if (shared_dcomp) {
                dcomp_device_ = shared_dcomp;
            } else {
                DCompositionCreateDevice(dxgi_device_.Get(), IID_PPV_ARGS(&dcomp_device_));
            }
        }
    }

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) {
        return false;
    }

    auto family_exists = [this](const wchar_t* family_name) noexcept -> bool {
        if (!dwrite_ || !family_name) return false;
        ComPtr<IDWriteFontCollection> collection;
        if (FAILED(dwrite_->GetSystemFontCollection(&collection, FALSE)) || !collection) return false;
        UINT32 idx = 0;
        BOOL exists = FALSE;
        return SUCCEEDED(collection->FindFamilyName(family_name, &idx, &exists)) && exists;
    };
    const wchar_t* const body_family = family_exists(L"Segoe UI Variable Text")
        ? L"Segoe UI Variable Text"
        : (family_exists(L"Microsoft YaHei UI") ? L"Microsoft YaHei UI" : L"Segoe UI");
    const wchar_t* const icon_family = family_exists(L"Segoe Fluent Icons")
        ? L"Segoe Fluent Icons"
        : L"Segoe MDL2 Assets";
    const wchar_t* const display_family = family_exists(L"Segoe UI Variable Display")
        ? L"Segoe UI Variable Display"
        : L"Segoe UI";

    auto mk = [&](float px, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT h_align,
                  bool wrap, bool trim, ComPtr<IDWriteTextFormat>& out) -> bool {
        if (FAILED(dwrite_->CreateTextFormat(body_family, nullptr, weight,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             px, L"zh-CN", &out))) {
            if (FAILED(dwrite_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                 px, L"zh-CN", &out))) {
                return false;
            }
        }
        out->SetTextAlignment(h_align);
        out->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (trim) {
            ComPtr<IDWriteInlineObject> ell;
            if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(out.Get(), &ell)) && ell) {
                DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                out->SetTrimming(&tr, ell.Get());
            }
        }
        return true;
    };

    auto mk_fluent = [&](float px, ComPtr<IDWriteTextFormat>& out) -> bool {
        if (FAILED(dwrite_->CreateTextFormat(icon_family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             px, L"en-US", &out))) {
            if (FAILED(dwrite_->CreateTextFormat(L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                 px, L"en-US", &out))) {
                return false;
            }
        }
        out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    };

    if (!mk(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true, fmt_body_) ||
        !mk(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false, true, fmt_body_semi_) ||
        !mk(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false, true, fmt_title_) ||
        !mk(17.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false, false, fmt_stat_num_) ||
        !mk(11.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true, fmt_small_) ||
        !mk(11.0f, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER, false, false, fmt_small_center_) ||
        !mk(11.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, false, false, fmt_small_right_) ||
        !mk_fluent(12.0f, fmt_fluent_12_) ||
        !mk_fluent(15.0f, fmt_fluent_15_) ||
        !mk_fluent(20.0f, fmt_fluent_20_)) {
        return false;
    }

    if (FAILED(dwrite_->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         12.0f, L"zh-CN", &fmt_mono_))) {
        return false;
    }

    if (FAILED(dwrite_->CreateTextFormat(display_family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         38.0f, L"zh-CN", &fmt_gallery_big_))) {
        if (FAILED(dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             38.0f, L"zh-CN", &fmt_gallery_big_))) {
            return false;
        }
    }
    fmt_gallery_big_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt_gallery_big_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt_gallery_big_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt_mono_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fmt_mono_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    fmt_mono_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 19.0f, 15.0f);
    return true;
}

bool DesktopOverlayController::EnsureSwapChain(uint32_t win_w, uint32_t win_h) {
    if (!EnsureWindowCreated() || !EnsureRenderDevices()) return false;
    win_w = std::max<uint32_t>(32u, win_w);
    win_h = std::max<uint32_t>(32u, win_h);

    if (swap_chain_ && win_w == win_w_ && win_h == win_h_ && target_bitmap_) {
        return true;
    }

    if (d2d_dc_) d2d_dc_->SetTarget(nullptr);
    target_bitmap_.Reset();

    if (!swap_chain_) {
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgi_device_->GetAdapter(&adapter))) return false;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

        DXGI_SWAP_CHAIN_DESC1 sc{};
        sc.Width = win_w;
        sc.Height = win_h;
        sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sc.SampleDesc.Count = 1;
        sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount = 2;
        sc.Scaling = DXGI_SCALING_STRETCH;
        sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

        using_dcomp_ = false;
        if (dcomp_device_) {
            sc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            HRESULT step = factory->CreateSwapChainForComposition(d3d_device_.Get(), &sc, nullptr, &swap_chain_);
            const char* stage = "swapchain";
            // A window takes one DComp target, and a released one stays bound for a while: binding again on the
            // next show failed (WINDOW_ALREADY_COMPOSED) and left an opaque window that DWM rounded at 8 px. So the
            // target and visual live as long as the window; only the chain inside is replaced.
            if (SUCCEEDED(step) && !dcomp_target_) {
                stage = "target";
                step = dcomp_device_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_target_);
                if (step == static_cast<HRESULT>(0x88980800L)) {  // DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED
                    // Bound by someone we no longer hold: a fresh window has no binding.
                    OverlayLog("window already composed: recreating it");
                    DestroyHostedPopupEdit();
                    DestroyWindow(hwnd_);
                    hwnd_ = nullptr;
                    step = EnsureWindowCreated() ? dcomp_device_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_target_) : E_FAIL;
                }
                dcomp_visual_.Reset();
            }
            if (SUCCEEDED(step) && !dcomp_visual_) {
                stage = "visual";
                step = dcomp_device_->CreateVisual(&dcomp_visual_);
                if (SUCCEEDED(step)) step = dcomp_target_->SetRoot(dcomp_visual_.Get());
            }
            if (SUCCEEDED(step)) { stage = "content"; step = dcomp_visual_->SetContent(swap_chain_.Get()); }
            if (SUCCEEDED(step)) { stage = "commit"; step = dcomp_device_->Commit(); }
            if (SUCCEEDED(step)) {
                using_dcomp_ = true;
            } else {
                OverlayLog("dcomp %s failed hr=0x%08lX size=%ux%u", stage, static_cast<unsigned long>(step), win_w, win_h);
                swap_chain_.Reset();
            }
        }

        if (!swap_chain_) {
            sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            if (FAILED(factory->CreateSwapChainForHwnd(d3d_device_.Get(), hwnd_, &sc, nullptr, nullptr, &swap_chain_))) {
                return false;
            }
            OverlayLog("using an opaque window: DWM rounds it, the card follows DWM's corner");
        }
        ConfigureOverlayDwmAttributes(hwnd_, using_dcomp_);
    } else {
        // D2D can still hold the old back buffer until flushed, and ResizeBuffers then fails (menu -> drawer).
        if (d2d_dc_) d2d_dc_->Flush();
        if (const HRESULT hr = swap_chain_->ResizeBuffers(2, win_w, win_h, DXGI_FORMAT_B8G8R8A8_UNORM, 0); FAILED(hr)) {
            OverlayLog("resize %ux%u failed hr=0x%08lX: rebuilding", win_w, win_h, static_cast<unsigned long>(hr));
            // Rebuild the chain inside the same target and visual (see the creation path).
            if (dcomp_visual_) dcomp_visual_->SetContent(nullptr);
            swap_chain_.Reset();
            if (dcomp_device_) dcomp_device_->Commit();
            return EnsureSwapChain(win_w, win_h);
        }
        if (using_dcomp_ && dcomp_device_) {
            dcomp_device_->Commit();
        }
    }

    ComPtr<IDXGISurface> surface;
    if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface))) || !surface) return false;

    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f * scale_, 96.0f * scale_);
    if (FAILED(d2d_dc_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &target_bitmap_))) {
        return false;
    }
    win_w_ = win_w;
    win_h_ = win_h;
    return true;
}

bool DesktopOverlayController::EnsureSubSwapChain(uint32_t win_w, uint32_t win_h) {
    if (!EnsureWindowCreated() || !EnsureRenderDevices() || !sub_hwnd_) return false;
    win_w = std::max<uint32_t>(32u, win_w);
    win_h = std::max<uint32_t>(32u, win_h);

    if (sub_swap_chain_ && win_w == sub_win_w_ && win_h == sub_win_h_ && sub_target_bitmap_) {
        return true;
    }

    if (d2d_dc_) d2d_dc_->SetTarget(nullptr);
    sub_target_bitmap_.Reset();

    if (!sub_swap_chain_) {
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgi_device_->GetAdapter(&adapter))) return false;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

        DXGI_SWAP_CHAIN_DESC1 sc{};
        sc.Width = win_w;
        sc.Height = win_h;
        sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sc.SampleDesc.Count = 1;
        sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount = 2;
        sc.Scaling = DXGI_SCALING_STRETCH;
        sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

        sub_using_dcomp_ = false;
        if (dcomp_device_) {
            sc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            HRESULT step = factory->CreateSwapChainForComposition(d3d_device_.Get(), &sc, nullptr, &sub_swap_chain_);
            // One target for the submenu window's life, as for the main window.
            if (SUCCEEDED(step) && !sub_dcomp_target_) {
                step = dcomp_device_->CreateTargetForHwnd(sub_hwnd_, TRUE, &sub_dcomp_target_);
                sub_dcomp_visual_.Reset();
            }
            if (SUCCEEDED(step) && !sub_dcomp_visual_) {
                step = dcomp_device_->CreateVisual(&sub_dcomp_visual_);
                if (SUCCEEDED(step)) step = sub_dcomp_target_->SetRoot(sub_dcomp_visual_.Get());
            }
            if (SUCCEEDED(step)) step = sub_dcomp_visual_->SetContent(sub_swap_chain_.Get());
            if (SUCCEEDED(step)) step = dcomp_device_->Commit();
            if (SUCCEEDED(step)) {
                sub_using_dcomp_ = true;
            } else {
                OverlayLog("submenu dcomp failed hr=0x%08lX", static_cast<unsigned long>(step));
                sub_swap_chain_.Reset();
            }
        }

        if (!sub_swap_chain_) {
            sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            if (FAILED(factory->CreateSwapChainForHwnd(d3d_device_.Get(), sub_hwnd_, &sc, nullptr, nullptr, &sub_swap_chain_))) {
                return false;
            }
        }
        ConfigureOverlayDwmAttributes(sub_hwnd_, sub_using_dcomp_);
    } else {
        if (d2d_dc_) d2d_dc_->Flush();
        if (FAILED(sub_swap_chain_->ResizeBuffers(2, win_w, win_h, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) {
            // As EnsureSwapChain: a new chain inside the same target and visual.
            if (sub_dcomp_visual_) sub_dcomp_visual_->SetContent(nullptr);
            sub_swap_chain_.Reset();
            if (dcomp_device_) dcomp_device_->Commit();
            return EnsureSubSwapChain(win_w, win_h);
        }
        if (sub_using_dcomp_ && dcomp_device_) dcomp_device_->Commit();
    }

    ComPtr<IDXGISurface> surface;
    if (FAILED(sub_swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface))) || !surface) return false;

    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f * scale_, 96.0f * scale_);
    if (FAILED(d2d_dc_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &sub_target_bitmap_))) {
        return false;
    }
    sub_win_w_ = win_w;
    sub_win_h_ = win_h;
    return true;
}

void DesktopOverlayController::DestroySubmenu() noexcept {
    submenu_open_ = false;
    open_submenu_parent_row_ = -1;
    hovered_sub_row_ = -1;
    submenu_rows_.clear();
    if (sub_hwnd_ && IsWindow(sub_hwnd_)) {
        ShowWindow(sub_hwnd_, SW_HIDE);
    }
    if (d2d_dc_) d2d_dc_->SetTarget(nullptr);
    sub_target_bitmap_.Reset();
    if (sub_dcomp_visual_) sub_dcomp_visual_->SetContent(nullptr);  // the target stays (one per window)
    sub_swap_chain_.Reset();
    sub_win_w_ = 0;
    sub_win_h_ = 0;
    if (dcomp_device_) dcomp_device_->Commit();
}

void DesktopOverlayController::DestroySwapChain() noexcept {
    DestroySubmenu();
    if (d2d_dc_) {
        d2d_dc_->SetTarget(nullptr);
    }
    quicklook_bitmap_.Reset();
    target_bitmap_.Reset();
    // Keep the target and visual: the window can take a target only once (see EnsureSwapChain).
    if (dcomp_visual_) dcomp_visual_->SetContent(nullptr);
    swap_chain_.Reset();
    if (dcomp_device_) {
        dcomp_device_->Commit();
    }
    win_w_ = 0;
    win_h_ = 0;

    if (d2d_device_) {
        d2d_device_->ClearResources(0);
    }
    if (dxgi_device_) {
        ComPtr<IDXGIDevice3> dxgi3;
        if (SUCCEEDED(dxgi_device_.As(&dxgi3)) && dxgi3) {
            dxgi3->Trim();
        }
    }
}

void DesktopOverlayController::Dismiss() noexcept {
    ++menu_generation_;
    if (shared_menu_) shared_menu_->Dismiss();
    const bool was_open = (active_kind_ != OverlayKind::None);
    if (active_kind_ == OverlayKind::StickyNotePopover && popup_edit_hwnd_ && IsWindow(popup_edit_hwnd_)) {
        sticky_note_text_ = ReadHostedPopupEditText();
    }
    DestroyHostedPopupEdit();
    edit_on_commit_ = {};
    edit_allow_empty_ = false;
    edit_multiline_ = false;
    rename_stem_len_ = -1;
    if (hwnd_ && IsWindow(hwnd_)) {
        KillTimer(hwnd_, kToastTimerId);
        KillTimer(hwnd_, 8804);
        ShowWindow(hwnd_, SW_HIDE);
    }
    dock_panel_kind_ = DockPanelKind::None;
    dock_panel_tray_index_ = -1;
    dock_vol_dragging_ = false;
    dock_vol_track_ = {};
    if (shell_session_id_ != 0 && callbacks_.on_close_shell_session) {
        callbacks_.on_close_shell_session(shell_session_id_);
    }
    shell_session_id_ = 0;
    shell_verbs_.clear();
    shell_verbs_cached_ = false;
    menu_rows_.clear();
    click_regions_.clear();
    beautify_sliders_.clear();
    beautify_scroll_y_ = 0.0f;
    beautify_max_scroll_y_ = 0.0f;
    beautify_drop_hover_ = false;
    beautify_drop_filename_.clear();
    DetachBeautifyDropTarget();
    if (!dragging_beautify_slider_.empty()) {
        dragging_beautify_slider_.clear();
        if (hwnd_ && IsWindow(hwnd_) && GetCapture() == hwnd_) ReleaseCapture();
    }
    hovered_action_id_.clear();
    hovered_row_ = -1;
    hovered_icon_bar_idx_ = -1;
    hovered_tag_idx_ = -1;
    active_kind_ = OverlayKind::None;

    DestroySwapChain();
    if (was_open && callbacks_.on_overlay_dismissed) {
        callbacks_.on_overlay_dismissed();
    }
}

void DesktopOverlayController::RepositionAndShow(int card_screen_x, int card_screen_y,
                                                 int card_w, int card_h, bool take_focus) {
    card_screen_x_ = card_screen_x;
    card_screen_y_ = card_screen_y;
    card_w_ = std::max(48, card_w);
    card_h_ = std::max(26, card_h);

    const int req_w = card_w_ + kShadowMargin * 2;
    const int req_h = card_h_ + kShadowMargin * 2;
    if (!EnsureSwapChain(static_cast<uint32_t>(ToPx(req_w)), static_cast<uint32_t>(ToPx(req_h)))) return;

    const int pad = using_dcomp_ ? kShadowMargin : 0;
    const int final_w = using_dcomp_ ? req_w : card_w_;
    const int final_h = using_dcomp_ ? req_h : card_h_;
    if (!using_dcomp_) {
        EnsureSwapChain(static_cast<uint32_t>(ToPx(card_w_)), static_cast<uint32_t>(ToPx(card_h_)));
    }

    PlaceHostedPopupEdit();
    Render();
    if (using_dcomp_ && dcomp_device_) {
        dcomp_device_->Commit();
    }

    UINT flags = SWP_SHOWWINDOW;
    if (!take_focus) flags |= SWP_NOACTIVATE;
    SetWindowPos(hwnd_, HWND_TOPMOST, ToPx(card_screen_x_ - pad), ToPx(card_screen_y_ - pad), ToPx(final_w),
                 ToPx(final_h), flags);
    if (take_focus) {
        SetForegroundWindow(hwnd_);
        if (popup_edit_hwnd_ && IsWindow(popup_edit_hwnd_)) {
            SetFocus(popup_edit_hwnd_);
            Render();
            if (using_dcomp_ && dcomp_device_) {
                dcomp_device_->Commit();
            }
        } else {
            SetFocus(hwnd_);
        }
    }
}

ID2D1SolidColorBrush* DesktopOverlayController::Brush(ID2D1DeviceContext* dc, uint32_t argb, float opacity) {
    if (!dc) return nullptr;
    if (dc == d2d_dc_.Get() && brush_) {
        brush_->SetColor(ArgbColor(argb, opacity));
        return brush_.Get();
    }
    if (g_ext_brush_dc != dc || !g_ext_brush) {
        g_ext_brush.Reset();
        dc->CreateSolidColorBrush(ArgbColor(argb, opacity), &g_ext_brush);
        g_ext_brush_dc = dc;
    } else {
        g_ext_brush->SetColor(ArgbColor(argb, opacity));
    }
    return g_ext_brush.Get();
}

float DesktopOverlayController::MeasureTextW(const std::wstring& text, IDWriteTextFormat* fmt, float max_w) const {
    if (text.empty() || !dwrite_ || !fmt) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), fmt, max_w, 40.0f, &layout)) || !layout) {
        return static_cast<float>(text.size()) * 12.0f;
    }
    DWRITE_TEXT_METRICS tm{};
    if (FAILED(layout->GetMetrics(&tm))) return static_cast<float>(text.size()) * 12.0f;
    return tm.widthIncludingTrailingWhitespace;
}

// ---------------------------------------------------------------------------
// 1. 4-State Context Menu + Cascading #dsub Submenu
// ---------------------------------------------------------------------------
void DesktopOverlayController::BuildMenuRows(const model::BeautifySettings& settings) {
    l10n::SetLanguage(menu_policy_.Snapshot().language);
    menu_rows_.clear();

    auto mk_row = [](std::string id, wchar_t icon, std::wstring label, std::wstring badge = {},
                     bool sep = false, bool checked = false) -> MenuRow {
        MenuRow r;
        r.command_id = std::move(id);
        r.icon_glyph = icon;
        r.label = std::move(label);
        r.right_badge = std::move(badge);
        r.separator_after = sep;
        r.checked = checked;
        return r;
    };

    auto sanitize_separators = [](auto& self, std::vector<MenuRow>& rows) -> void {
        for (auto& r : rows) {
            if (!r.submenu_rows.empty()) self(self, r.submenu_rows);
        }
        if (!rows.empty()) {
            rows.back().separator_after = false;
        }
    };

    if (menu_spec_.target_kind == ContextMenuTargetKind::FileIcon && menu_spec_.item_in_apps_tile)
        menu_rows_.push_back(mk_row("app-out", L'\xE8B7', L"从「应用」放回桌面", {}, true));
    switch (menu_spec_.target_kind) {
    case ContextMenuTargetKind::AppTileShortcut: {
        {
            MenuRow op = mk_row("open-item", L'\xE8E5', L"打开", L"Enter");
            op.is_accent_primary = true;
            menu_rows_.push_back(std::move(op));
        }
        menu_rows_.push_back(mk_row("run-as-admin", L'\xE7EF', L"以管理员身份运行"));
        menu_rows_.push_back(mk_row("reveal-shortcut-target", L'\xEC50', L"打开文件所在的位置", {}, true));

        menu_rows_.push_back(mk_row("app-out", L'\xE8B7', L"从「应用」放回桌面"));
        menu_rows_.push_back(mk_row("pin-as-tile", L'\xE718', L"单独固定为磁贴", {}, true));

        menu_rows_.push_back(mk_row("copy-path", L'\xE71B', L"复制快捷方式路径"));
        menu_rows_.push_back(mk_row("properties", L'\xE946', L"属性", L"Alt+Enter", true));
        {
            MenuRow del = mk_row("delete-item", L'\xE74D', L"删除快捷方式");
            del.is_danger = true;
            menu_rows_.push_back(std::move(del));
        }
        break;
    }

    case ContextMenuTargetKind::FileIcon: {
        // Top Win11 5-icon strip: Cut, Copy, Rename, Delete, Share
        MenuRow bar;
        bar.command_id = "icon-bar";
        bar.is_icon_bar = true;
        bar.separator_after = true;
        menu_rows_.push_back(std::move(bar));

        menu_rows_.push_back(mk_row("open-item", L'\xE8E5', L"打开", L"Enter"));
        {
            MenuRow ow = mk_row("sub:open-with", L'\xE7AC', L"打开方式");
            ow.submenu_rows.push_back(mk_row("open-item", L'\xE8E5', L"系统默认应用"));
            ow.submenu_rows.push_back(mk_row("quick-look", L'\xE890', L"Pulse 快速查看"));
            ow.submenu_rows.push_back(mk_row("reveal-in-pulse", L'\xEC50', L"在 Pulse 中打开"));
            menu_rows_.push_back(std::move(ow));
        }
        menu_rows_.push_back(mk_row("quick-look", L'\xE890', L"快速查看", L"空格"));
        menu_rows_.push_back(mk_row("reveal-in-pulse", L'\xEC50', L"在 Pulse 中显示", {}, true));

        if (menu_spec_.target_is_folder && menu_spec_.selected_count == 1) {
            if (menu_spec_.current_group_can_map_folder) {
                MenuRow map_cur = mk_row("map-folder-to-current-group", L'\xE8B7', L"将此文件夹内容作为本组显示");
                map_cur.is_accent_primary = true;
                menu_rows_.push_back(std::move(map_cur));
            }
            if (!menu_spec_.target_folder_already_mapped) {
                MenuRow new_mapped = mk_row("create-mapped-group-from-folder", L'\xE8F4',
                                            L"展开为文件夹分组卡片", {}, true);
                if (!menu_spec_.current_group_can_map_folder) {
                    new_mapped.is_accent_primary = true;
                }
                menu_rows_.push_back(std::move(new_mapped));
            } else if (!menu_spec_.current_group_can_map_folder) {
                menu_rows_.back().separator_after = true;
            }
        }

        {
            MenuRow mv = mk_row("sub:move-group", L'\xE8DE', L"移到分组");
            for (const auto& grp : menu_spec_.groups_list) {
                mv.submenu_rows.push_back(mk_row(
                    "move-to-group:" + grp.id,
                    grp.icon_glyph != 0 ? grp.icon_glyph : L'\xE8B7',
                    grp.title,
                    {},
                    false,
                    menu_spec_.current_group_id == grp.id));
            }
            if (!mv.submenu_rows.empty()) {
                mv.submenu_rows.back().separator_after = true;
            }
            mv.submenu_rows.push_back(mk_row("move-to-loose", L'\xE74B', L"放在桌面上", {},
                                             false, menu_spec_.current_group_id.empty()));
            mv.submenu_rows.push_back(mk_row("new-group-with-sel", L'\xE8F4', L"新建分组并移入"));
            menu_rows_.push_back(std::move(mv));
        }
        menu_rows_.push_back(mk_row("pin-as-tile", L'\xE718', L"固定为磁贴"));
        menu_rows_.push_back(mk_row("add-to-tray", L'\xE7B8',
                                    menu_spec_.item_in_tray ? L"移出暂存区" : L"加入暂存区",
                                    L"Ctrl+Shift+S", true));

        menu_rows_.push_back(mk_row("copy-path", L'\xE71B', L"复制路径", L"Ctrl+Shift+C"));
        {
            MenuRow st = mk_row("sub:send-to", L'\xE724', L"发送到");
            st.submenu_rows.push_back(mk_row("send-desktop-link", L'\xE71B', L"桌面快捷方式"));
            st.submenu_rows.push_back(mk_row("send-docs", L'\xE8B7', L"文档"));
            menu_rows_.push_back(std::move(st));
        }
        menu_rows_.push_back(mk_row("properties", L'\xE946', L"属性", L"Alt+Enter", true));

        MenuRow tag_row;
        tag_row.command_id = "tag-bar";
        tag_row.label = L"标签";
        tag_row.is_tag_row = true;
        tag_row.separator_after = false;
        menu_rows_.push_back(std::move(tag_row));

        {
            MenuRow tm = mk_row("sub:tags", L'\xE8EC', L"颜色标签", {}, !shell_verbs_.empty());
            for (const auto& tag : menu_spec_.tag_palette) {
                const bool is_cur = (tag.id == menu_spec_.current_tag_id);
                tm.submenu_rows.push_back(mk_row(
                    is_cur ? "tag:clear" : ("tag:" + ipc::WideToUtf8(tag.id)),
                    0, tag.name, {}, false, is_cur));
            }
            if (!tm.submenu_rows.empty()) {
                tm.submenu_rows.back().separator_after = true;
            }
            if (!menu_spec_.current_tag_id.empty()) {
                tm.submenu_rows.push_back(mk_row("tag:clear", L'\xE711', L"清除标签"));
                tm.submenu_rows.push_back(mk_row("open-tag-pulse", L'\xEC50', L"在 Pulse 中查看此标签"));
            }
            tm.submenu_rows.push_back(mk_row("manage-tags-pulse", L'\xE713', L"在 Pulse 中管理标签…"));
            menu_rows_.push_back(std::move(tm));
        }
        break;
    }

    case ContextMenuTargetKind::BlankDesktop: {
        {
            MenuRow vw = mk_row("sub:view", L'\xE890', L"查看");
            vw.submenu_rows.push_back(mk_row("view-size:2", 0, L"大图标", {}, false,
                                             settings.icon_size == model::DesktopIconSize::Large));
            vw.submenu_rows.push_back(mk_row("view-size:1", 0, L"中等图标", {}, false,
                                             settings.icon_size == model::DesktopIconSize::Medium));
            vw.submenu_rows.push_back(mk_row("view-size:0", 0, L"小图标", {}, true,
                                             settings.icon_size == model::DesktopIconSize::Small));
            // The same choice as the top of the settings page: 整理 + 小组件, or only the 小组件.
            vw.submenu_rows.push_back(mk_row("view-mode-organize", 0, L"整理 + 小组件", {}, false,
                                             settings.organize_enabled));
            vw.submenu_rows.push_back(mk_row("view-mode-widgets", 0, L"只显示小组件", {}, true,
                                             !settings.organize_enabled));
            vw.submenu_rows.push_back(mk_row("toggle-tiles", 0, L"显示小组件", {}, false,
                                             settings.tiles_enabled));
            vw.submenu_rows.push_back(mk_row("toggle-dock", 0, L"显示液态玻璃程序坞", {}, false,
                                             settings.dock_enabled));
            vw.submenu_rows.push_back(mk_row("toggle-grid", 0, L"将图标对齐到网格", {}, true,
                                             settings.align_to_grid));
            vw.submenu_rows.push_back(mk_row("toggle-icons", 0, L"显示桌面图标", {}, false,
                                             !settings.icons_hidden));
            menu_rows_.push_back(std::move(vw));
        }
        {
            MenuRow sr = mk_row("sub:sort", L'\xE8CB', L"排序方式");
            sr.submenu_rows.push_back(mk_row("loose-sort:1", 0, L"修改日期", {}, false,
                                             settings.loose_sort_by == model::GroupSortBy::ModifiedTime));
            sr.submenu_rows.push_back(mk_row("loose-sort:0", 0, L"名称", {}, false,
                                             settings.loose_sort_by == model::GroupSortBy::Name));
            sr.submenu_rows.push_back(mk_row("loose-sort:2", 0, L"大小", {}, false,
                                             settings.loose_sort_by == model::GroupSortBy::Size));
            sr.submenu_rows.push_back(mk_row("loose-sort:3", 0, L"项目类型", {}, true,
                                             settings.loose_sort_by == model::GroupSortBy::Type));
            sr.submenu_rows.push_back(mk_row("loose-sort:4", 0, L"手动排列", {}, false,
                                             settings.loose_sort_by == model::GroupSortBy::Manual));
            menu_rows_.push_back(std::move(sr));
        }
        menu_rows_.push_back(mk_row("refresh", L'\xE72C', L"刷新", L"F5"));
        // Always here, top level: the fold setting leaves 新建分组 alone (DesktopMenuPolicy::Managed).
        menu_rows_.push_back(mk_row("new-group", L'\xE8F4', L"新建分组", L"Ctrl+Shift+G", true));

        if (menu_spec_.ai_organize_enabled) {
            MenuRow ai_row = mk_row("ai-organize", L'\xF5C2', L"AI 智能收纳…", L"Ctrl+Shift+A");
            ai_row.is_accent_primary = true;
            menu_rows_.push_back(std::move(ai_row));
        }
        menu_rows_.push_back(mk_row("auto-organize", L'\xE895', L"按规则重新整理"));
        {
            std::wstring h_badge;
            if (menu_spec_.health_issue_count > 0) {
                h_badge = std::to_wstring(menu_spec_.health_issue_count) + L" 项建议";
            }
            menu_rows_.push_back(mk_row("health-check", L'\xE7EF', L"桌面体检", std::move(h_badge)));
        }
        menu_rows_.push_back(mk_row("new-mapped-group", L'\xE8B7', L"从文件夹创建分组…"));
        {
            MenuRow at = mk_row("sub:add-tile", L'\xE710', L"添加小组件");
            MenuRow gal = mk_row("widget-gallery", L'\xE71D', L"打开小组件库…", {}, !menu_spec_.widget_catalog.empty());
            gal.is_accent_primary = true;
            at.submenu_rows.push_back(std::move(gal));
            for (const auto& [kind, placed] : menu_spec_.widget_catalog) {
                MenuRow r = mk_row("add-tile:" + std::to_string(static_cast<int>(kind)), WidgetGlyph(kind), WidgetName(kind),
                                   placed ? L"已添加" : L"");
                r.enabled = !placed;
                at.submenu_rows.push_back(std::move(r));
            }
            menu_rows_.push_back(std::move(at));
        }
        menu_rows_.push_back(mk_row("tiles-edit", L'\xE70F', L"编辑磁贴区", L"长按磁贴"));
        {
            MenuRow sc = mk_row("sub:scene", L'\xE7C4', L"场景", {}, true);
            sc.submenu_rows.push_back(mk_row("scene-work", 0, L"工作", {}, false,
                                             settings.active_scene == model::DesktopScene::Work));
            sc.submenu_rows.push_back(mk_row("scene-focus", 0, L"专注", {}, false,
                                             settings.active_scene == model::DesktopScene::Focus));
            sc.submenu_rows.push_back(mk_row("scene-present", 0, L"演示", {}, false,
                                             settings.active_scene == model::DesktopScene::Present));
            menu_rows_.push_back(std::move(sc));
        }

        {
            MenuRow nf = mk_row("sub:new-file", L'\xE710', L"新建");
            nf.submenu_rows.push_back(mk_row("new-folder", L'\xE8B7', L"文件夹", L"Ctrl+Shift+N"));
            nf.submenu_rows.push_back(mk_row("new-txt", L'\xE8A5', L"文本文档 (.txt)"));
            nf.submenu_rows.push_back(mk_row("new-md", L'\xE8A5', L"Markdown 文档 (.md)"));
            menu_rows_.push_back(std::move(nf));
        }
        {
            MenuRow u = mk_row("undo", L'\xE7A7',
                               menu_spec_.undo_label.empty() ? L"撤销" : (L"撤销 " + menu_spec_.undo_label),
                               L"Ctrl+Z", true);
            u.enabled = !menu_spec_.undo_label.empty();
            menu_rows_.push_back(std::move(u));
        }

        menu_rows_.push_back(mk_row("open-search", L'\xE721', L"搜索桌面和 Pulse", L"Ctrl+F"));
        menu_rows_.push_back(mk_row("open-desktop-pulse", L'\xEC50', L"在 Pulse 中打开桌面"));
        menu_rows_.push_back(mk_row("open-terminal", L'\xE756', L"在此处打开终端", {}, true));

        menu_rows_.push_back(mk_row("beautify", L'\xE790', L"美化桌面"));
        {
            MenuRow disp = mk_row("sys-display", L'\xE7F8', L"显示设置", L"系统");
            disp.is_shell_verb = true;
            menu_rows_.push_back(std::move(disp));
            MenuRow pers = mk_row("sys-personalize", L'\xE771', L"个性化", L"系统");
            pers.is_shell_verb = true;
            menu_rows_.push_back(std::move(pers));
        }
        menu_rows_.push_back(mk_row("open-pack-settings", L'\xE713', L"桌面整理设置", {}, !shell_verbs_.empty()));
        break;
    }

    case ContextMenuTargetKind::GroupHeader: {
        if (!menu_spec_.group_single_subfolder_path.empty()) {
            const std::wstring single_lbl = L"展开「" + menu_spec_.group_single_subfolder_name + L"」内容到此分组";
            MenuRow expand_row = mk_row("map-single-child-folder", L'\xE8B7', single_lbl, {}, true);
            expand_row.is_accent_primary = true;
            menu_rows_.push_back(std::move(expand_row));
        }
        {
            MenuRow view = mk_row("sub:group-view", L'\xE8A9', l10n::Get(l10n::StringId::DesktopGroupViewMode));
            const l10n::StringId labels[] = {l10n::StringId::DesktopGroupViewIcons,
                l10n::StringId::DesktopGroupViewLargeIcons, l10n::StringId::DesktopGroupViewTiles,
                l10n::StringId::DesktopGroupViewDetails};
            for (int mode = 0; mode < 4; ++mode) {
                view.submenu_rows.push_back(mk_row("group-view:" + std::to_string(mode), 0,
                    l10n::Get(labels[mode]), {}, false, static_cast<int>(menu_spec_.group_view_mode) == mode));
            }
            menu_rows_.push_back(std::move(view));
        }
        if (menu_spec_.group_tabbed) {
            menu_rows_.push_back(mk_row("group-detach-tab", L'\xE8A7', l10n::Get(l10n::StringId::DesktopGroupDetachTab)));
        }
        menu_rows_.push_back(mk_row("rename-group", L'\xE8AC', L"重命名分组", L"F2"));
        if (menu_spec_.group_mapped_folder_path.empty()) {
            menu_rows_.push_back(mk_row("edit-rule", L'\xE71C', L"整理规则…"));
        }
        if (menu_spec_.ai_organize_enabled) {
            MenuRow ai_grp = mk_row("ai-organize-group", 0, L"AI 归档本组文件…");
            ai_grp.is_accent_primary = menu_spec_.group_single_subfolder_path.empty();
            menu_rows_.push_back(std::move(ai_grp));
        }
        {
            MenuRow gs = mk_row("sub:group-sort", L'\xE8CB', L"排序");
            gs.submenu_rows.push_back(mk_row("sort-mtime", 0, L"修改日期", {}, false,
                                             menu_spec_.group_sort == model::GroupSortBy::ModifiedTime));
            gs.submenu_rows.push_back(mk_row("sort-name", 0, L"名称", {}, false,
                                             menu_spec_.group_sort == model::GroupSortBy::Name));
            gs.submenu_rows.push_back(mk_row("sort-size", 0, L"大小", {}, false,
                                             menu_spec_.group_sort == model::GroupSortBy::Size));
            gs.submenu_rows.push_back(mk_row("sort-type", 0, L"类型", {}, true,
                                             menu_spec_.group_sort == model::GroupSortBy::Type));
            gs.submenu_rows.push_back(mk_row("sort-manual", 0, L"手动", {}, false,
                                             menu_spec_.group_sort == model::GroupSortBy::Manual));
            menu_rows_.push_back(std::move(gs));
        }
        {
            MenuRow gc = mk_row("sub:group-cols", L'\xE740', L"宽度");
            for (int c = 3; c <= 6; ++c) {
                gc.submenu_rows.push_back(mk_row("group-cols:" + std::to_string(c), 0,
                                                 std::to_wstring(c) + L" 列", {}, false,
                                                 menu_spec_.group_cols == c));
            }
            menu_rows_.push_back(std::move(gc));
        }
        {
            MenuRow gm = mk_row("sub:group-mat", L'\xE790', L"效果", {}, true);
            gm.submenu_rows.push_back(mk_row("mat-follow", 0, L"跟随桌面设置", {}, true,
                                             menu_spec_.group_material == model::CardMaterial::FollowHost));
            gm.submenu_rows.push_back(mk_row("mat-acrylic", 0, L"亚克力", {}, false,
                                             menu_spec_.group_material == model::CardMaterial::Acrylic));
            gm.submenu_rows.push_back(mk_row("mat-liquid", 0, L"液态玻璃", {}, false,
                                             menu_spec_.group_material == model::CardMaterial::LiquidGlass));
            gm.submenu_rows.push_back(mk_row("mat-mica", 0, L"Mica", {}, false,
                                             menu_spec_.group_material == model::CardMaterial::Mica));
            gm.submenu_rows.push_back(mk_row("mat-mica-alt", 0, L"Mica Alt", {}, false,
                                             menu_spec_.group_material == model::CardMaterial::MicaAlt));
            gm.submenu_rows.push_back(mk_row("mat-solid", 0, L"纯色", {}, false,
                                             menu_spec_.group_material == model::CardMaterial::Solid));
            menu_rows_.push_back(std::move(gm));
        }
        menu_rows_.push_back(mk_row("toggle-collapse", L'\xE70E',
                                    menu_spec_.group_collapsed ? L"展开分组" : L"卷起分组", L"双击标题"));
        menu_rows_.push_back(mk_row("toggle-focus", L'\xE7C4', L"在专注场景中保持展开", {},
                                    false, menu_spec_.group_focus_visible));
        menu_rows_.push_back(mk_row("map-real-folder", L'\xE8B7',
                                    menu_spec_.group_mapped_folder_path.empty()
                                        ? L"对应到真实文件夹…"
                                        : L"更换关联文件夹…"));
        if (!menu_spec_.group_mapped_folder_path.empty()) {
            menu_rows_.push_back(mk_row("unmap-real-folder", L'\xE711', L"解除文件夹关联"));
        }
        menu_rows_.push_back(mk_row("open-group-pulse", L'\xEC50',
                                    menu_spec_.group_mapped_folder_path.empty()
                                        ? L"在 Pulse 中打开这些文件"
                                        : L"在 Pulse 中打开此文件夹",
                                    {}, true));
        {
            MenuRow del = mk_row("delete-group", L'\xE74D',
                                 menu_spec_.group_mapped_folder_path.empty()
                                     ? L"删除分组（文件留在桌面）"
                                     : L"移除分组卡片（原文件夹保留）");
            del.is_danger = true;
            menu_rows_.push_back(std::move(del));
        }
        break;
    }

    case ContextMenuTargetKind::DockBar: {
        const auto dk = static_cast<render::DockBarItemKind>(menu_spec_.dock_item_kind);
        if (menu_spec_.dock_item_index >= 0 && dk != render::DockBarItemKind::Divider) {
            if (dk == render::DockBarItemKind::App) {
                // WacOS-style live window list for running apps
                for (size_t wi = 0; wi < menu_spec_.dock_item_windows.size(); ++wi) {
                    const auto& [wh, wtitle] = menu_spec_.dock_item_windows[wi];
                    const bool is_last = (wi + 1 == menu_spec_.dock_item_windows.size());
                    menu_rows_.push_back(mk_row("dock-focus-win:" + std::to_string(wh), L'\xE737',
                                                wtitle.empty() ? L"窗口" : wtitle, {}, is_last,
                                                wh != 0 && wh == menu_spec_.dock_active_hwnd));
                }
                menu_rows_.push_back(mk_row("dock-open", L'\xE8A7',
                                            menu_spec_.dock_item_running
                                                ? (menu_spec_.dock_item_has_visible_window ? L"切换到窗口" : L"还原窗口")
                                                : L"打开应用"));
                const bool can_new_win = menu_spec_.dock_item_running &&
                                         !menu_spec_.target_path.empty() &&
                                         menu_spec_.dock_item_key != L"ms-settings:";
                if (can_new_win) {
                    menu_rows_.push_back(mk_row("dock-new-instance", L'\xE710', L"打开新窗口"));
                }
                if (menu_spec_.target_path.size() >= 3 && menu_spec_.target_path[1] == L':') {
                    menu_rows_.push_back(mk_row("dock-reveal", L'\xE8B7', L"在文件夹中显示"));
                }
                const bool can_pin_toggle = menu_spec_.dock_item_pinned || !menu_spec_.target_path.empty();
                if (can_pin_toggle) {
                    menu_rows_.push_back(mk_row("dock-pin-toggle", menu_spec_.dock_item_pinned ? L'\xE77A' : L'\xE718',
                                                menu_spec_.dock_item_pinned ? L"从程序坞移除" : L"固定到程序坞",
                                                {}, !menu_spec_.dock_item_running));
                }
                if (menu_spec_.dock_item_running) {
                    if (menu_spec_.dock_item_has_visible_window) {
                        menu_rows_.push_back(mk_row("dock-hide-app", L'\xE921', L"隐藏 / 最小化"));
                    } else {
                        menu_rows_.push_back(mk_row("dock-restore-app", L'\xE922', L"展开并还原窗口"));
                    }
                    MenuRow cls = mk_row("dock-close-app", L'\xE711', L"关闭窗口", {}, true);
                    cls.is_danger = true;
                    menu_rows_.push_back(std::move(cls));
                }
            } else if (dk == render::DockBarItemKind::Trash) {
                menu_rows_.push_back(mk_row("dock-open", L'\xE74D', L"打开回收站"));
                MenuRow emp = mk_row("dock-empty-trash", L'\xE74D', L"清空回收站", {}, true);
                emp.enabled = !menu_spec_.recycle_bin_empty;
                emp.is_danger = true;
                menu_rows_.push_back(std::move(emp));
            } else if (dk == render::DockBarItemKind::Folder) {
                const bool apps_stack = menu_spec_.dock_item_key == L"special:apps";
                if (!apps_stack) {
                    // Dock stacks (叠放): sort order and how the stack opens, as on the Mac.
                    MenuRow sort = mk_row("sub:dock-stack-sort", L'\xE8CB', std::wstring(l10n::Get(l10n::StringId::SortBy)));
                    const l10n::StringId sort_names[] = {l10n::StringId::ColumnCreated, l10n::StringId::ColumnName,
                                                         l10n::StringId::ColumnModified, l10n::StringId::ColumnType};
                    for (int si = 0; si < 4; ++si) {
                        sort.submenu_rows.push_back(mk_row("dock-stack-sort:" + std::to_string(si), 0,
                                                           std::wstring(l10n::Get(sort_names[si])), {}, false,
                                                           settings.dock_stack_sort == si));
                    }
                    menu_rows_.push_back(std::move(sort));
                    MenuRow view = mk_row("sub:dock-stack-view", L'\xE8A9', std::wstring(l10n::Get(l10n::StringId::DockStackViewAs)),
                                          {}, true);
                    const l10n::StringId view_names[] = {l10n::StringId::TextRenderAuto, l10n::StringId::DockStackViewFan,
                                                         l10n::StringId::DockStackViewGrid, l10n::StringId::ViewList};
                    for (int vi = 0; vi < 4; ++vi) {
                        view.submenu_rows.push_back(mk_row("dock-stack-view:" + std::to_string(vi), 0,
                                                           std::wstring(l10n::Get(view_names[vi])), {}, vi == 0,
                                                           settings.dock_stack_view == vi));
                    }
                    menu_rows_.push_back(std::move(view));
                } else {
                    menu_rows_.push_back(mk_row("dock-apps-stack:0", L'\xE77A',
                                                std::wstring(l10n::Get(l10n::StringId::DockStackRemove)), {}, true));
                }
                if (!menu_spec_.dock_recent_downloads.empty()) {
                    for (size_t di = 0; di < menu_spec_.dock_recent_downloads.size(); ++di) {
                        const bool is_last = (di + 1 == menu_spec_.dock_recent_downloads.size());
                        menu_rows_.push_back(mk_row("dock-open-download:" + std::to_string(di), L'\xE8A5',
                                                    menu_spec_.dock_recent_downloads[di].second, {}, is_last));
                    }
                }
                if (!menu_spec_.target_path.empty()) {
                    menu_rows_.push_back(mk_row("dock-open-folder", L'\xE8B7',
                                                std::wstring(l10n::Get(l10n::StringId::PickerPulseOpenPlaces)),
                                                {}, !menu_spec_.dock_item_pinned));
                }
                if (menu_spec_.dock_item_pinned && !menu_spec_.target_path.empty()) {
                    menu_rows_.push_back(mk_row("dock-pin-toggle", L'\xE77A', L"从程序坞移除", {}, true));
                }
            } else if (dk == render::DockBarItemKind::WidgetsToggle) {
                menu_rows_.push_back(mk_row("dock-toggle-widgets", L'\xE71D',
                                            settings.tiles_enabled ? L"隐藏桌面小组件" : L"显示桌面小组件", {}, true));
            }
        }
        {
            MenuRow sub = mk_row("sub:dock-settings", L'\xE713', L"液态玻璃程序坞设置");
            sub.submenu_rows.push_back(mk_row("dock-mag-toggle", 0, L"启用鼠标悬停放大", {}, false,
                                              settings.dock_magnification));
            sub.submenu_rows.push_back(mk_row(settings.dock_apps_stack ? "dock-apps-stack:0" : "dock-apps-stack:1", 0,
                                              std::wstring(l10n::Get(l10n::StringId::DockStackShowApps)), {}, false,
                                              settings.dock_apps_stack));
            sub.submenu_rows.push_back(mk_row("dock-sys-toggle", 0, L"显示系统状态与托盘区", {}, true,
                                              settings.dock_system_area));
            sub.submenu_rows.push_back(mk_row("dock-pos:0", 0, L"停靠在屏幕底部", {}, false,
                                              settings.dock_position == 0));
            sub.submenu_rows.push_back(mk_row("dock-pos:1", 0, L"停靠在屏幕左侧", {}, false,
                                              settings.dock_position == 1));
            sub.submenu_rows.push_back(mk_row("dock-pos:2", 0, L"停靠在屏幕右侧", {}, true,
                                              settings.dock_position == 2));
            sub.submenu_rows.push_back(mk_row("dock-size:42", 0, L"小图标（42）", {}, false,
                                              settings.dock_icon_size <= 44));
            sub.submenu_rows.push_back(mk_row("dock-size:48", 0, L"标准图标（48）", {}, false,
                                              settings.dock_icon_size > 44 && settings.dock_icon_size < 53));
            sub.submenu_rows.push_back(mk_row("dock-size:56", 0, L"大图标（56）", {}, true,
                                              settings.dock_icon_size >= 53));
            sub.submenu_rows.push_back(mk_row("dock-scale:132", 0, L"柔和放大（1.32×）", {}, false,
                                              settings.dock_max_scale_x100 < 142));
            sub.submenu_rows.push_back(mk_row("dock-scale:152", 0, L"经典放大（1.52×）", {}, false,
                                              settings.dock_max_scale_x100 >= 142 && settings.dock_max_scale_x100 < 165));
            sub.submenu_rows.push_back(mk_row("dock-scale:178", 0, L"动感放大（1.78×）", {}, true,
                                              settings.dock_max_scale_x100 >= 165));
            sub.submenu_rows.push_back(mk_row("dock-hide:0", 0, L"常驻显示程序坞", {}, false,
                                              settings.dock_hide_mode == 0));
            sub.submenu_rows.push_back(mk_row("dock-hide:1", 0, L"智能隐藏（有窗口遮挡时收回）", {}, false,
                                              settings.dock_hide_mode == 1));
            sub.submenu_rows.push_back(mk_row("dock-hide:2", 0, L"自动隐藏（贴边唤出展开）", {}, true,
                                              settings.dock_hide_mode == 2));
            sub.submenu_rows.push_back(mk_row("dock-minfx:1", 0, L"神奇效果（Genie 漏斗收回/展开）", {}, false,
                                              settings.dock_minimize_effect == 1));
            sub.submenu_rows.push_back(mk_row("dock-minfx:2", 0, L"缩放效果（平滑缩放收回/展开）", {}, false,
                                              settings.dock_minimize_effect == 2));
            sub.submenu_rows.push_back(mk_row("dock-minfx:0", 0, L"关闭窗口收回/展开动画", {}, true,
                                              settings.dock_minimize_effect == 0));
            sub.submenu_rows.push_back(mk_row("beautify", L'\xE790', L"打开桌面美化与程序坞面板…", {}, true));
            sub.submenu_rows.push_back(mk_row("dock-disable", 0, L"隐藏程序坞"));
            menu_rows_.push_back(std::move(sub));
        }
        break;
    }

    case ContextMenuTargetKind::LiveTile: {
        const auto k = menu_spec_.tile_kind;
        switch (k) {
        case model::TileKind::Photos:
            menu_rows_.push_back(mk_row("open-tile", L'\xEB9F', L"在 Pulse 中查看照片"));
            menu_rows_.push_back(mk_row("photo-next", L'\xE72C', L"换一张照片"));
            {
                // How often it changes, as the user likes it.
                MenuRow every = mk_row("sub:photo-interval", L'\xE916', L"轮播间隔");
                static constexpr std::pair<int, const wchar_t*> kIntervals[] = {
                    {5, L"5 秒"}, {10, L"10 秒"}, {30, L"30 秒"}, {60, L"1 分钟"}, {300, L"5 分钟"}, {900, L"15 分钟"},
                    {1800, L"30 分钟"}, {0, L"不自动切换"}};
                for (const auto& [seconds, label] : kIntervals) {
                    every.submenu_rows.push_back(mk_row("photo-interval:" + std::to_string(seconds), 0, label, {}, false,
                                                        menu_spec_.tile_photo_interval_s == seconds));
                    if (seconds == 1800) every.submenu_rows.back().separator_after = true;
                }
                menu_rows_.push_back(std::move(every));
            }
            menu_rows_.push_back(mk_row("photo-shuffle", L'\xE8B1', L"随机顺序", {}, false, menu_spec_.tile_photo_shuffle));
            menu_rows_.push_back(mk_row("pick-photo-folder", L'\xE8B7', L"选择照片文件夹…"));
            if (menu_spec_.tile_photo_custom_folder)
                menu_rows_.push_back(mk_row("photo-folder-reset", L'\xEB9F', L"改回图片库"));
            break;
        case model::TileKind::Downloads:
            menu_rows_.push_back(mk_row("open-tile", L'\xE896', L"在 Pulse 中打开下载"));
            menu_rows_.push_back(mk_row("open-latest-download", L'\xE8E5', L"打开最新下载项"));
            break;
        case model::TileKind::Recent:
            menu_rows_.push_back(mk_row("open-tile", L'\xE81C', L"在 Pulse 中查看最近使用"));
            menu_rows_.push_back(mk_row("clear-recent", L'\xE74D', L"清除最近记录"));
            break;
        case model::TileKind::PinnedFolder:
        case model::TileKind::PinnedItem:
            menu_rows_.push_back(mk_row("open-tile", L'\xEC50', L"在 Pulse 中打开"));
            menu_rows_.push_back(mk_row("change-tile-target", L'\xE8B7', L"更换目标路径…"));
            break;
        case model::TileKind::RecycleBin: {
            menu_rows_.push_back(mk_row("open-tile", L'\xE8E5', L"打开回收站"));
            MenuRow emp = mk_row("empty-recycle-bin", L'\xE74D', L"清空回收站");
            emp.enabled = !menu_spec_.recycle_bin_empty;
            emp.is_danger = true;
            menu_rows_.push_back(std::move(emp));
            break;
        }
        case model::TileKind::ThisPC:
            menu_rows_.push_back(mk_row("open-tile", L'\xE977', L"打开此电脑"));
            menu_rows_.push_back(mk_row("open-storage-settings", L'\xEDA2', L"磁盘与存储设置"));
            break;
        case model::TileKind::DiskRing:
            menu_rows_.push_back(mk_row("open-tile", L'\xEDA2', L"在 Pulse 中打开磁盘"));
            menu_rows_.push_back(mk_row("open-storage-settings", L'\xE713', L"磁盘与存储设置"));
            break;
        case model::TileKind::PulseLauncher:
            menu_rows_.push_back(mk_row("open-tile", L'\xEC50', L"打开 Pulse 主窗口"));
            menu_rows_.push_back(mk_row("open-search", L'\xE721', L"全盘秒搜…", L"Ctrl+F"));
            menu_rows_.push_back(mk_row("open-pack-settings", L'\xE713', L"桌面整理设置"));
            break;
        case model::TileKind::Clock:
            menu_rows_.push_back(mk_row("toggle-clock", L'\xE823', L"指针表盘", {}, false,
                                        menu_spec_.tile_analog_clock));
            menu_rows_.push_back(mk_row("open-clock-app", L'\xE916', L"打开系统时钟"));
            break;
        case model::TileKind::StickyNote:
            menu_rows_.push_back(mk_row("edit-note", L'\xE70F', L"编辑便笺"));
            {
                MenuRow colours = mk_row("sub:note-color", L'\xE790', L"便笺颜色");
                static constexpr const wchar_t* kNames[6] = {L"黄色", L"绿色", L"蓝色", L"粉色", L"紫色", L"灰色"};
                for (int c = 0; c < 6; ++c)
                    colours.submenu_rows.push_back(mk_row("note-color:" + std::to_string(c), 0, kNames[c], {}, false,
                                                          menu_spec_.tile_note_color == c));
                menu_rows_.push_back(std::move(colours));
            }
            menu_rows_.push_back(mk_row("copy-note", L'\xE8C8', L"复制便笺内容"));
            menu_rows_.push_back(mk_row("clear-note", L'\xE74D', L"清空便笺"));
            break;
        case model::TileKind::StagingTray:
            menu_rows_.push_back(mk_row("open-tile", L'\xE7B8', L"展开暂存区"));
            menu_rows_.push_back(mk_row("tray-open-pulse", L'\xEC50', L"在 Pulse 中查看暂存区"));
            menu_rows_.push_back(mk_row("tray-release-desktop", L'\xE8B7', L"全部释放到桌面"));
            menu_rows_.push_back(mk_row("tray-clear", L'\xE74D', L"清空暂存区"));
            break;
        case model::TileKind::Apps:
            menu_rows_.push_back(mk_row("open-tile", L'\xEC50', L"在 Pulse 中打开桌面"));
            menu_rows_.push_back(mk_row("apps-all-out", L'\xE8B7', L"全部快捷方式放回桌面"));
            break;
        case model::TileKind::Weather: {
            menu_rows_.push_back(mk_row("weather-refresh", L'\xE72C', L"刷新天气"));
            MenuRow wc = mk_row("sub:weather-city", L'\xE775', L"切换城市");
            static constexpr const wchar_t* kCities[] = {
                L"北京", L"上海", L"广州", L"深圳", L"杭州", L"成都", L"武汉", L"南京", L"西安"
            };
            for (const wchar_t* city : kCities) {
                wc.submenu_rows.push_back(mk_row(
                    "weather-city:" + ipc::WideToUtf8(city), 0, city, {}, false,
                    menu_spec_.current_weather_city == city));
            }
            wc.submenu_rows.back().separator_after = true;
            wc.submenu_rows.push_back(mk_row("weather-city-custom", L'\xE70F', L"输入其他城市…"));
            menu_rows_.push_back(std::move(wc));
            break;
        }
        case model::TileKind::Calendar:
            // 2×2 shows the big date or the month; the tile keeps the choice in its 表盘 flag.
            if (menu_spec_.tile_size.cols >= 2 && menu_spec_.tile_size.cols < 4 && menu_spec_.tile_size.rows == 2)
                menu_rows_.push_back(mk_row("toggle-clock", L'\xE787', L"显示整月", {}, false, !menu_spec_.tile_analog_clock));
            menu_rows_.push_back(mk_row("open-calendar-app", L'\xE787', L"打开系统日历"));
            break;
        case model::TileKind::Todo:
            menu_rows_.push_back(mk_row("todo-add", L'\xE710', L"添加待办…"));
            menu_rows_.push_back(mk_row("todo-clear-done", L'\xE74D', L"清除已完成项"));
            break;
        case model::TileKind::System:
            menu_rows_.push_back(mk_row("open-taskmgr", L'\xE9D9', L"打开任务管理器"));
            menu_rows_.push_back(mk_row("open-resmon", L'\xE9D2', L"打开资源监视器"));
            menu_rows_.push_back(mk_row("open-sys-about", L'\xE946', L"系统信息"));
            break;
        case model::TileKind::Pomodoro:
            menu_rows_.push_back(mk_row("pomo-toggle", L'\xE768', L"开始 / 暂停专注"));
            menu_rows_.push_back(mk_row("pomo-reset", L'\xE72C', L"重置当前计时"));
            menu_rows_.push_back(mk_row("pomo-25", L'\xE916', L"25 分钟标准专注"));
            menu_rows_.push_back(mk_row("pomo-45", L'\xE916', L"45 分钟深度专注"));
            menu_rows_.push_back(mk_row("pomo-break", L'\xE708', L"切换为 5 分钟短休"));
            break;
        case model::TileKind::MediaPlayer:
            menu_rows_.push_back(mk_row("media-play", L'\xE768', L"播放 / 暂停"));
            menu_rows_.push_back(mk_row("media-prev", L'\xE892', L"上一首"));
            menu_rows_.push_back(mk_row("media-next", L'\xE893', L"下一首"));
            break;
        case model::TileKind::Countdown:
            menu_rows_.push_back(mk_row("countdown-edit-title", L'\xE8AC', L"修改倒数日名称…"));
            menu_rows_.push_back(mk_row("countdown-edit-date", L'\xE787', L"设置目标日期 (YYYY-MM-DD)…"));
            menu_rows_.push_back(mk_row("countdown-plus30", L'\xE710', L"设为 30 天后"));
            break;
        case model::TileKind::Battery:
            menu_rows_.push_back(mk_row("open-power-settings", L'\xE83F', L"电源与电池设置"));
            menu_rows_.push_back(mk_row("open-taskmgr", L'\xE9D9', L"打开任务管理器"));
            break;
        }
        if (!menu_rows_.empty()) menu_rows_.back().separator_after = true;

        // A stack: pick a widget, smart rotation, take one out.
        if (menu_spec_.tile_stack_names.size() > 1) {
            MenuRow st = mk_row("sub:tile-stack", L'\xE8FD',
                                L"叠放 · " + std::to_wstring(menu_spec_.tile_stack_names.size()) + L" 个");
            for (size_t i = 0; i < menu_spec_.tile_stack_names.size(); ++i)
                st.submenu_rows.push_back(mk_row("tile-stack-sel:" + std::to_string(i), 0, menu_spec_.tile_stack_names[i], {},
                                                 i + 1 == menu_spec_.tile_stack_names.size(),
                                                 static_cast<int>(i) == menu_spec_.tile_stack_index));
            st.submenu_rows.push_back(mk_row("tile-stack-rotate", 0, L"智能轮换", L"没在看时自动换", false, menu_spec_.tile_smart_rotate));
            st.submenu_rows.push_back(mk_row("tile-unstack", 0,
                L"移出「" + menu_spec_.tile_stack_names[static_cast<size_t>(std::clamp(menu_spec_.tile_stack_index, 0,
                    static_cast<int>(menu_spec_.tile_stack_names.size()) - 1))] + L"」"));
            menu_rows_.push_back(std::move(st));
        }

        // 自定义磁贴: title, background (effect / colour / picture) and this tile's own Windows effect.
        {
            const auto& lk = menu_spec_.tile_look;
            const bool photo = k == model::TileKind::Photos;
            MenuRow cu = mk_row("sub:tile-custom", L'\xE771', L"自定义磁贴");
            cu.submenu_rows.push_back(mk_row("tile-rename", L'\xE8AC', L"改标题…"));
            cu.submenu_rows.push_back(mk_row("tile-show-title", 0, L"显示标题", {}, true, lk.show_title));
            cu.submenu_rows.push_back(mk_row("tile-bg:material", 0, L"背景：跟随效果", {}, false,
                                             lk.background == model::TileBackground::Material));
            if (!photo) {
                MenuRow col = mk_row("sub:tile-color", 0, L"背景：纯色");
                col.checked = lk.background == model::TileBackground::Color;
                struct Sw { uint32_t argb; const wchar_t* name; };
                static constexpr Sw kSwatches[] = {
                    {0xFF0078D4u, L"蓝"}, {0xFF8764B8u, L"紫"}, {0xFFE3008Cu, L"玫红"}, {0xFFE81123u, L"红"},
                    {0xFFCA5010u, L"橙"}, {0xFFFFB900u, L"金"}, {0xFF10893Eu, L"绿"}, {0xFF00B294u, L"青"},
                    {0xFF2D7D9Au, L"湖蓝"}, {0xFF515C6Bu, L"石墨"}, {0xFF1B1B1Bu, L"黑"}, {0xFFF3F3F3u, L"白"},
                };
                for (int line = 0; line < 2; ++line) {
                    MenuRow cr = mk_row("tile-color-row:" + std::to_string(line), 0, L"");
                    for (int j = 0; j < 6; ++j) {
                        const auto& sw = kSwatches[line * 6 + j];
                        char id[32]{};
                        std::snprintf(id, sizeof(id), "tile-color:%08X", sw.argb);
                        cr.color_swatches.push_back({id, sw.argb,
                            lk.background == model::TileBackground::Color && (lk.color_argb | 0xFF000000u) == sw.argb});
                    }
                    col.submenu_rows.push_back(std::move(cr));
                }
                cu.submenu_rows.push_back(std::move(col));
                MenuRow img = mk_row("sub:tile-image", 0, L"背景：图片");
                img.checked = lk.background == model::TileBackground::Image;
                img.submenu_rows.push_back(mk_row("tile-image-pick", L'\xEB9F', L"选择图片…"));
                MenuRow cur = mk_row("tile-image-photos", 0, L"用「照片」正在显示的那张");
                cur.enabled = menu_spec_.tile_has_photos;
                img.submenu_rows.push_back(std::move(cur));
                cu.submenu_rows.push_back(std::move(img));
            }
            MenuRow fx = mk_row("sub:tile-fx", 0, L"Windows 效果", {}, false);
            const auto m = menu_spec_.tile_material;
            fx.submenu_rows.push_back(mk_row("tile-mat:follow", 0, L"跟随桌面", {}, true, m == model::CardMaterial::FollowHost));
            fx.submenu_rows.push_back(mk_row("tile-mat:acrylic", 0, L"亚克力", {}, false, m == model::CardMaterial::Acrylic));
            fx.submenu_rows.push_back(mk_row("tile-mat:liquid-glass", 0, L"液态玻璃", {}, false, m == model::CardMaterial::LiquidGlass));
            fx.submenu_rows.push_back(mk_row("tile-mat:mica", 0, L"Mica", {}, false, m == model::CardMaterial::Mica));
            fx.submenu_rows.push_back(mk_row("tile-mat:mica-alt", 0, L"Mica Alt", {}, false, m == model::CardMaterial::MicaAlt));
            fx.submenu_rows.push_back(mk_row("tile-mat:solid", 0, L"无（纯色）", {}, false, m == model::CardMaterial::Solid));
            fx.enabled = !(lk.background == model::TileBackground::Image && !photo);
            cu.submenu_rows.back().separator_after = true;
            cu.submenu_rows.push_back(std::move(fx));
            cu.submenu_rows.push_back(mk_row("tile-reset-look", 0, L"恢复默认"));
            menu_rows_.push_back(std::move(cu));
        }

        {
            MenuRow ts = mk_row("sub:tile-size", L'\xE740', L"调整大小");
            for (const model::TileSize s : model::AllowedTileSizesFor(k)) {
                wchar_t lbl[24]{};
                std::swprintf(lbl, 24, L"%d×%d %ls", s.cols, s.rows,
                              s.cols == 1 && s.rows == 1 ? L"小" : s.rows == 1 ? L"横条" : s.cols == 2 ? L"中" : s.rows == 2 ? L"宽" : L"大");
                ts.submenu_rows.push_back(mk_row("tile-size:" + std::to_string(s.cols) + "x" + std::to_string(s.rows), 0, lbl, {}, false,
                                                 menu_spec_.tile_size == s));
            }
            ts.enabled = ts.submenu_rows.size() > 1;
            menu_rows_.push_back(std::move(ts));
        }
        menu_rows_.push_back(mk_row(menu_spec_.tile_edit_mode ? "tiles-edit-done" : "tiles-edit", L'\xE70F',
                                    menu_spec_.tile_edit_mode ? L"完成编辑" : L"编辑磁贴区",
                                    menu_spec_.tile_edit_mode ? L"Esc" : L"长按磁贴"));
        const bool supports_live_toggle =
            (k == model::TileKind::Photos || k == model::TileKind::Downloads ||
             k == model::TileKind::Recent || k == model::TileKind::PinnedFolder ||
             k == model::TileKind::Weather || k == model::TileKind::System ||
             k == model::TileKind::DiskRing || k == model::TileKind::RecycleBin ||
             k == model::TileKind::MediaPlayer || k == model::TileKind::Battery);
        if (supports_live_toggle) {
            menu_rows_.push_back(mk_row("toggle-tile-live", 0, L"动态内容", {}, false, menu_spec_.tile_live_content));
        }
        if (!menu_rows_.empty()) menu_rows_.back().separator_after = true;
        {
            MenuRow rm = mk_row("remove-tile", L'\xE77A', L"从桌面取消固定");
            rm.is_danger = true;
            menu_rows_.push_back(std::move(rm));
        }
        if (menu_spec_.initial_open_submenu_id == "sub:tile-custom") {
            std::vector<MenuRow> custom_menu;
            for (auto& r : menu_rows_) {
                if (r.command_id == "sub:weather-city" || r.command_id == "pick-photo-folder" ||
                    r.command_id == "sub:photo-interval" || r.command_id == "photo-shuffle" ||
                    r.command_id == "edit-note" || r.command_id == "todo-add" ||
                    r.command_id == "toggle-clock" || r.command_id == "change-tile-target" ||
                    r.command_id == "pomo-toggle" || r.command_id == "pomo-25" || r.command_id == "pomo-45" ||
                    r.command_id == "media-play" || r.command_id == "countdown-edit-title" ||
                    r.command_id == "countdown-edit-date" || r.command_id == "open-power-settings") {
                    r.separator_after = false;
                    custom_menu.push_back(std::move(r));
                }
            }
            if (!custom_menu.empty()) custom_menu.back().separator_after = true;
            for (auto& r : menu_rows_) {
                if (r.command_id == "sub:tile-custom") {
                    for (auto& cr : r.submenu_rows) custom_menu.push_back(std::move(cr));
                    if (!custom_menu.empty()) custom_menu.back().separator_after = true;
                }
            }
            for (auto& r : menu_rows_) {
                if (r.command_id == "sub:tile-size" || r.command_id == "sub:tile-stack" ||
                    r.command_id == "tiles-edit" || r.command_id == "tiles-edit-done") {
                    r.separator_after = false;
                    custom_menu.push_back(std::move(r));
                }
            }
            if (!custom_menu.empty()) menu_rows_ = std::move(custom_menu);
        }
        break;
    }
    }

    // System (Shell) items from the out-of-process host:
    // - On BlankDesktop, merge Explorer's 新建(&W) children into our single 新建 submenu without duplicates,
    //   and filter top-level Shell verbs that duplicate Pulse's own desktop commands.
    MenuRow* own_new_menu = nullptr;
    for (auto& row : menu_rows_) {
        if (row.command_id == "sub:new-file") {
            own_new_menu = &row;
            break;
        }
    }
    std::vector<std::vector<MenuRow>*> levels{&menu_rows_};
    bool skipping = false;
    uint32_t skip_depth = 0;
    bool merging_shell_new = false;
    bool added_shell_new_sep = false;
    for (const auto& sv : shell_verbs_) {
        if (sv.depth == 0) {
            merging_shell_new = false;
        }
        if (skipping && sv.depth > skip_depth) continue;
        skipping = false;
        if (sv.label.empty()) continue;

        if (menu_spec_.target_kind == ContextMenuTargetKind::BlankDesktop) {
            if (own_new_menu != nullptr && IsShellNewSubmenuHeader(sv)) {
                merging_shell_new = true;
                continue;
            }
            if (merging_shell_new && sv.depth >= 1) {
                if (sv.depth == 1 && !sv.has_children && !IsDuplicateShellNewChild(sv.label)) {
                    if (!added_shell_new_sep && !own_new_menu->submenu_rows.empty()) {
                        own_new_menu->submenu_rows.back().separator_after = true;
                        added_shell_new_sep = true;
                    }
                    MenuRow nr;
                    nr.command_id = "shell:" + ipc::WideToUtf8(sv.label) + ":" + std::to_string(sv.depth) + ":" +
                                    std::to_string(sv.item_id);
                    nr.label = StripMenuMnemonic(sv.label);
                    if (nr.label.empty()) nr.label = sv.label;
                    nr.enabled = sv.enabled;
                    nr.is_shell_verb = true;
                    nr.shell_item_id = sv.item_id;
                    nr.shell_verb = sv.verb;
                    nr.shell_depth = sv.depth;
                    own_new_menu->submenu_rows.push_back(std::move(nr));
                }
                continue;
            }
            if (IsDuplicateBlankDesktopShellVerb(sv)) {
                skipping = true;
                skip_depth = 0;
                continue;
            }
        }

        const size_t depth = std::min<size_t>(sv.depth, levels.size() - 1);
        levels.resize(depth + 1);
        MenuRow r;
        r.command_id = "shell:" + ipc::WideToUtf8(sv.label) + ":" + std::to_string(sv.depth) + ":" +
                       std::to_string(sv.item_id);
        r.label = sv.label;
        if (depth == 0) r.right_badge = L"系统";
        r.enabled = sv.enabled;
        r.separator_after = sv.separator_after;
        r.is_shell_verb = !sv.has_children;
        r.shell_item_id = sv.has_children ? 0 : sv.item_id;
        r.shell_verb = sv.verb;
        r.shell_depth = sv.depth;
        levels.back()->push_back(std::move(r));
        if (sv.has_children) levels.push_back(&levels.back()->back().submenu_rows);
    }

    if (menu_spec_.target_kind == ContextMenuTargetKind::BlankDesktop) {
        const auto policy = menu_policy_.Snapshot();
        l10n::SetLanguage(policy.language);
        policy.Apply(menu_rows_, l10n::Get(l10n::StringId::DesktopOrganizeMenuGroup));
        MenuRow system;
        system.command_id = "sub:system-extra";
        system.label = l10n::Get(l10n::StringId::ContextSystem);
        std::erase_if(menu_rows_, [&](MenuRow& row) {
            if (!row.command_id.starts_with("shell:")) return false;
            system.submenu_rows.push_back(std::move(row));
            return true;
        });
        if (!system.submenu_rows.empty()) menu_rows_.push_back(std::move(system));
    }
    sanitize_separators(sanitize_separators, menu_rows_);

    int y = 6;
    const int menu_w = 264;
    for (auto& r : menu_rows_) {
        const int row_h = r.is_icon_bar ? 38 : (r.is_tag_row ? 34 : 30);
        r.bounds = RECT{5, y, menu_w - 5, y + row_h};
        y += row_h;
        if (r.separator_after) y += 9;
    }
}

bool DesktopOverlayController::OpenContextMenu(const ContextMenuSpec& spec,
                                               const model::BeautifySettings& settings) {
    Dismiss();
    active_kind_ = OverlayKind::ContextMenu;
    menu_spec_ = spec;
    beautify_settings_ = settings;
    if (menu_spec_.tag_palette.empty()) {
        menu_spec_.tag_palette = {
            {L"red",    L"红", 0xFFE5484Du},
            {L"orange", L"橙", 0xFFF5A524u},
            {L"green",  L"绿", 0xFF30A46Cu},
            {L"yellow", L"黄", 0xFFF2C94Cu},
            {L"purple", L"紫", 0xFF8E4EC6u},
            {L"blue",   L"蓝", 0xFF3E8EF7u},
            {L"gray",   L"灰", 0xFF8B8D98u},
        };
    }
    // The system rows open with the menu: the last reply for this kind of target fills them at once and
    // the live reply replaces them in place, so the menu never grows or jumps after it appears.
    shell_cache_key_ = ShellCacheKey(menu_spec_);
    if (auto it = shell_verb_cache_.find(shell_cache_key_); it != shell_verb_cache_.end()) {
        shell_verbs_ = it->second;
        shell_verbs_cached_ = true;
    }
    BuildMenuRows(settings);

    // Post rather than enter a modal loop here: the caller must first send its
    // asynchronous Shell query. The shared menu pumps replies while tracking.
    return EnsureWindowCreated() && PostMessageW(hwnd_, WM_APP + 71, menu_generation_, 0) != FALSE;
}

void DesktopOverlayController::OpenSubmenuForRow(int row_idx) {
    if (row_idx < 0 || static_cast<size_t>(row_idx) >= menu_rows_.size() ||
        menu_rows_[static_cast<size_t>(row_idx)].submenu_rows.empty()) {
        DestroySubmenu();
        return;
    }
    if (submenu_open_ && open_submenu_parent_row_ == row_idx) return;

    submenu_rows_ = menu_rows_[static_cast<size_t>(row_idx)].submenu_rows;
    sub_card_w_ = 220;
    int sy = 6;
    for (auto& sr : submenu_rows_) {
        sr.bounds = RECT{5, sy, sub_card_w_ - 5, sy + 30};
        sy += 30;
        if (sr.separator_after) sy += 9;
    }
    sub_card_h_ = sy + 6;

    const RECT parent_r = menu_rows_[static_cast<size_t>(row_idx)].bounds;
    // Screen DIPs: the work area the menu was opened in.
    const int scr_right = static_cast<int>(work_area_.right > 0 ? work_area_.right : ToDip(GetSystemMetrics(SM_CXSCREEN)));
    const int scr_bottom = static_cast<int>(work_area_.bottom > 0 ? work_area_.bottom : ToDip(GetSystemMetrics(SM_CYSCREEN)));

    int sx = card_screen_x_ + card_w_ + 3;
    if (sx + sub_card_w_ + 8 > scr_right) {
        sx = card_screen_x_ - sub_card_w_ - 3;
    }
    int top_y = std::min<int>(card_screen_y_ + parent_r.top - 5, scr_bottom - sub_card_h_ - 8);
    top_y = std::max(8, top_y);

    sub_screen_x_ = sx;
    sub_screen_y_ = top_y;
    submenu_open_ = true;
    open_submenu_parent_row_ = row_idx;
    hovered_sub_row_ = -1;

    const int req_w = sub_card_w_ + kShadowMargin * 2;
    const int req_h = sub_card_h_ + kShadowMargin * 2;
    if (EnsureSubSwapChain(static_cast<uint32_t>(ToPx(req_w)), static_cast<uint32_t>(ToPx(req_h))) && sub_hwnd_) {
        const int pad = sub_using_dcomp_ ? kShadowMargin : 0;
        const int fw = sub_using_dcomp_ ? req_w : sub_card_w_;
        const int fh = sub_using_dcomp_ ? req_h : sub_card_h_;
        if (!sub_using_dcomp_) {
            EnsureSubSwapChain(static_cast<uint32_t>(ToPx(sub_card_w_)), static_cast<uint32_t>(ToPx(sub_card_h_)));
        }
        RenderSubmenu();
        if (sub_using_dcomp_ && dcomp_device_) {
            dcomp_device_->Commit();
        }
        SetWindowPos(sub_hwnd_, HWND_TOPMOST, ToPx(sub_screen_x_ - pad), ToPx(sub_screen_y_ - pad),
                     ToPx(fw), ToPx(fh), SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
}

std::string DesktopOverlayController::ShellCacheKey(const ContextMenuSpec& spec) {
    switch (spec.target_kind) {
    case ContextMenuTargetKind::BlankDesktop: return "bg";
    case ContextMenuTargetKind::FileIcon: {
        if (spec.target_is_folder) return "folder";
        const size_t dot = spec.target_path.find_last_of(L'.');
        std::wstring ext = (dot == std::wstring::npos) ? std::wstring{} : spec.target_path.substr(dot);
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        return "file:" + ipc::WideToUtf8(ext);
    }
    default: return {};
    }
}

void DesktopOverlayController::CacheShellVerbs(const std::string& kind_key, std::vector<ShellVerbEntry> verbs) {
    if (!kind_key.empty() && !verbs.empty()) shell_verb_cache_[kind_key] = std::move(verbs);
}

void DesktopOverlayController::SetAsyncShellVerbs(uint64_t session_id, std::vector<ShellVerbEntry> verbs) {
    if (!shell_cache_key_.empty() && !verbs.empty()) shell_verb_cache_[shell_cache_key_] = verbs;
    if (!pending_shell_label_.empty()) {
        // A cached row was clicked while this reply was on its way: run the same row from the live list.
        const std::wstring label = std::move(pending_shell_label_);
        const uint32_t depth = pending_shell_depth_;
        pending_shell_label_.clear();
        for (const auto& v : verbs) {
            if (v.label == label && v.depth == depth && !v.has_children && v.item_id != 0) {
                if (callbacks_.on_invoke_shell_verb) callbacks_.on_invoke_shell_verb(session_id, v.item_id, v.verb);
                return;
            }
        }
    }
    if (active_kind_ != OverlayKind::ContextMenu) {
        if (session_id != 0 && callbacks_.on_close_shell_session) {
            callbacks_.on_close_shell_session(session_id);
        }
        return;
    }
    shell_session_id_ = session_id;
    shell_verbs_ = std::move(verbs);
    shell_verbs_cached_ = false;
    BuildMenuRows(beautify_settings_);
    if (shared_menu_ && shared_menu_->IsOpen()) {
        shared_menu_->ReplaceItems(BuildSharedMenuItems());
    }
}

// ---------------------------------------------------------------------------
// 2. Type-to-Search Popup
// ---------------------------------------------------------------------------
bool DesktopOverlayController::OpenSearchPopup(const RECT& work_area, const std::wstring& initial_query,
                                               std::vector<SearchHitEntry> local_hits) {
    Dismiss();
    active_kind_ = OverlayKind::SearchPopup;
    work_area_ = work_area;
    search_query_ = initial_query;
    local_hits_ = std::move(local_hits);
    remote_hits_.clear();
    search_selected_idx_ = 0;

    const int w = 540;
    const int h = search_query_.empty() ? 188 : 424;
    const int x = work_area.left + ((work_area.right - work_area.left) - w) / 2;
    const int y = work_area.top + 108;
    card_w_ = w;
    card_h_ = h;
    const int qlen = static_cast<int>(search_query_.size());
    AttachHostedPopupEdit(D2D1::RectF(42.0f, 10.0f, static_cast<float>(w) - 58.0f, 44.0f),
                          search_query_, L"搜索桌面上的文件，顺便找找 Pulse 里的…",
                          false, false, qlen, qlen, 120);
    RepositionAndShow(x, y, w, h, true);
    return true;
}

void DesktopOverlayController::AppendSearchChar(wchar_t ch) {
    if (active_kind_ != OverlayKind::SearchPopup || ch < 0x20 || ch == 0x7F) return;
    if (popup_edit_hwnd_ && IsWindow(popup_edit_hwnd_)) {
        const int len = GetWindowTextLengthW(popup_edit_hwnd_);
        SendMessageW(popup_edit_hwnd_, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
        const wchar_t buf[2] = {ch, 0};
        SendMessageW(popup_edit_hwnd_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(buf));
        SetFocus(popup_edit_hwnd_);
        return;
    }
    HandleChar(ch);
}

void DesktopOverlayController::UpdateLocalSearchHits(std::vector<SearchHitEntry> local_hits) {
    if (active_kind_ != OverlayKind::SearchPopup) return;
    local_hits_ = std::move(local_hits);
    const int w = 540;
    const int h = search_query_.empty() ? 188 : 424;
    RepositionAndShow(card_screen_x_, card_screen_y_, w, h, false);
}

void DesktopOverlayController::UpdateRemoteSearchHits(std::vector<SearchHitEntry> remote_hits) {
    if (active_kind_ != OverlayKind::SearchPopup) return;
    remote_hits_ = std::move(remote_hits);
    Render();
}

// ---------------------------------------------------------------------------
// 3. Space Quick Look Overlay
// ---------------------------------------------------------------------------
bool DesktopOverlayController::OpenQuickLook(const RECT& work_area, const model::DesktopItem& item,
                                             const ipc::PreviewPayloadData* preview) {
    Dismiss();
    active_kind_ = OverlayKind::QuickLook;
    work_area_ = work_area;
    ql_item_ = item;
    ql_has_preview_ = false;
    quicklook_bitmap_.Reset();

    const int w = 680;
    const int h = 468;
    const int x = work_area.left + ((work_area.right - work_area.left) - w) / 2;
    const int y = work_area.top + ((work_area.bottom - work_area.top) - h) / 2;
    RepositionAndShow(x, y, w, h, true);
    if (preview) {
        UpdateQuickLookPreview(*preview);
    }
    return true;
}

void DesktopOverlayController::UpdateQuickLookPreview(const ipc::PreviewPayloadData& preview) {
    if (active_kind_ != OverlayKind::QuickLook || !d2d_dc_) return;
    ql_preview_ = preview;
    ql_has_preview_ = true;
    quicklook_bitmap_.Reset();

    if (preview.thumb_width > 0 && preview.thumb_height > 0 &&
        preview.thumb_stride >= preview.thumb_width * 4u &&
        preview.thumb_bgra.size() >= static_cast<size_t>(preview.thumb_stride) * preview.thumb_height) {
        const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        d2d_dc_->CreateBitmap(
            D2D1::SizeU(preview.thumb_width, preview.thumb_height),
            preview.thumb_bgra.data(),
            preview.thumb_stride,
            &bp,
            &quicklook_bitmap_);
    }
    Render();
}

// ---------------------------------------------------------------------------
// 4. Side Drawers (Health Check & Beautify Desktop)
// ---------------------------------------------------------------------------
bool DesktopOverlayController::OpenHealthDrawer(const RECT& work_area, const model::HealthReport& report,
                                                size_t total_desktop_items) {
    Dismiss();
    active_kind_ = OverlayKind::HealthDrawer;
    work_area_ = work_area;
    health_report_ = report;
    health_total_desktop_items_ = total_desktop_items > 0 ? total_desktop_items : std::max<size_t>(18u, report.issues.size() + 12u);
    health_completed_ = false;
    health_cleaned_count_ = 0;
    health_cleaned_bytes_ = 0;

    const int w = 384;
    const int h = std::max<int>(520, (work_area.bottom - work_area.top) - 28);
    const int x = work_area.right - w - 16;
    const int y = work_area.top + 14;
    RepositionAndShow(x, y, w, h, true);
    return true;
}

namespace {

class BeautifyDropTarget final : public IDropTarget {
public:
    explicit BeautifyDropTarget(DesktopOverlayController* owner,
                                std::function<bool()> can_accept_video,
                                std::function<void(bool, const std::wstring&)> on_hover,
                                std::function<void(const std::wstring&)> on_drop)
        : owner_(owner),
          can_accept_video_(std::move(can_accept_video)),
          on_hover_(std::move(on_hover)),
          on_drop_(std::move(on_drop)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ref_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data_obj, DWORD /*key_state*/, POINTL /*pt*/, DWORD* effect) override {
        valid_video_path_.clear();
        if (effect) *effect = DROPEFFECT_NONE;
        if (!owner_ || (can_accept_video_ && !can_accept_video_())) return S_OK;
        const auto paths = DesktopDropManager::ExtractHDropPaths(data_obj);
        if (paths.size() == 1 && video::IsSupportedVideoWallpaperPath(paths[0])) {
            valid_video_path_ = paths[0];
            if (effect) *effect = DROPEFFECT_COPY;
            if (on_hover_) on_hover_(true, video::VideoDisplayName(valid_video_path_));
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD /*key_state*/, POINTL /*pt*/, DWORD* effect) override {
        if (effect) *effect = valid_video_path_.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        valid_video_path_.clear();
        if (on_hover_) on_hover_(false, {});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data_obj, DWORD /*key_state*/, POINTL /*pt*/, DWORD* effect) override {
        std::wstring dropped = valid_video_path_;
        if (dropped.empty() && data_obj) {
            const auto paths = DesktopDropManager::ExtractHDropPaths(data_obj);
            if (paths.size() == 1 && video::IsSupportedVideoWallpaperPath(paths[0])) {
                dropped = paths[0];
            }
        }
        valid_video_path_.clear();
        if (on_hover_) on_hover_(false, {});
        if (!dropped.empty() && (!can_accept_video_ || can_accept_video_())) {
            if (effect) *effect = DROPEFFECT_COPY;
            if (on_drop_) on_drop_(dropped);
        } else if (effect) {
            *effect = DROPEFFECT_NONE;
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    DesktopOverlayController* owner_ = nullptr;
    std::function<bool()> can_accept_video_;
    std::function<void(bool, const std::wstring&)> on_hover_;
    std::function<void(const std::wstring&)> on_drop_;
    std::wstring valid_video_path_;
};

} // namespace

void DesktopOverlayController::AttachBeautifyDropTarget() noexcept {
    if (!hwnd_ || !IsWindow(hwnd_) || beautify_drop_registered_) return;
    if (!beautify_drop_target_) {
        beautify_drop_target_.Attach(new BeautifyDropTarget(
            this,
            [this]() { return active_kind_ == OverlayKind::BeautifyDrawer && beautify_video_.icon_takeover_active; },
            [this](bool hover, const std::wstring& name) { SetBeautifyDropHover(hover, name); },
            [this](const std::wstring& path) {
                if (callbacks_.on_set_video_wallpaper) callbacks_.on_set_video_wallpaper(path);
            }));
    }
    if (SUCCEEDED(RegisterDragDrop(hwnd_, beautify_drop_target_.Get()))) {
        beautify_drop_registered_ = true;
    }
}

void DesktopOverlayController::DetachBeautifyDropTarget() noexcept {
    if (beautify_drop_registered_ && hwnd_ && IsWindow(hwnd_)) {
        RevokeDragDrop(hwnd_);
    }
    beautify_drop_registered_ = false;
}

void DesktopOverlayController::SetBeautifyVideoState(const video::VideoWallpaperUiState& state, bool rerender) {
    beautify_video_ = state;
    if (beautify_video_.active_source_path.empty()) {
        beautify_live_bgra_.clear();
        beautify_live_w_ = 0;
        beautify_live_h_ = 0;
        beautify_live_bmp_.Reset();
    }
    if (rerender && active_kind_ == OverlayKind::BeautifyDrawer) {
        Render();
    }
}

void DesktopOverlayController::OnLiveVideoFrameArrived(bool force) {
    if (!IsOpen() || !d2d_dc_) return;
    if (IsDraggingBeautifySlider() || beautify_drop_hover_) return;

    const bool is_beautify = (active_kind_ == OverlayKind::BeautifyDrawer);
    const bool is_glass_popup =
        is_beautify ||
        active_kind_ == OverlayKind::DockSystemPanel ||
        active_kind_ == OverlayKind::DockVolumeOsd ||
        active_kind_ == OverlayKind::WidgetDetail ||
        active_kind_ == OverlayKind::WidgetGalleryDrawer ||
        active_kind_ == OverlayKind::ContextMenu;
    if (!is_glass_popup) return;

    const uint64_t now = GetTickCount64();
    // Smooth 20 fps on BeautifyDrawer Wall tab (live 16:9 preview + glass refraction) and
    // 15 fps on other glass popups — zero CPU readback, zero extra VRAM!
    const uint64_t min_interval_ms = (is_beautify && beautify_tab_ == BeautifyTab::Wall) ? 48ull : 64ull;
    if (!force && last_live_overlay_render_ms_ != 0 && (now - last_live_overlay_render_ms_) < min_interval_ms) {
        return;
    }
    last_live_overlay_render_ms_ = now;
    Render();
}

void DesktopOverlayController::UpdateBeautifyVideoLiveFrame(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride) {
    if (!bgra || w == 0 || h == 0) return;
    const uint32_t row_bytes = w * 4u;
    const uint32_t src_stride = stride ? stride : row_bytes;
    beautify_live_bgra_.resize(static_cast<size_t>(row_bytes) * h);
    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(beautify_live_bgra_.data() + static_cast<size_t>(y) * row_bytes,
                    bgra + static_cast<size_t>(y) * src_stride, row_bytes);
    }
    beautify_live_w_ = w;
    beautify_live_h_ = h;
    beautify_live_bmp_.Reset();
    if (active_kind_ == OverlayKind::BeautifyDrawer && beautify_tab_ == BeautifyTab::Wall && !beautify_drop_hover_) {
        Render();
    }
}

void DesktopOverlayController::SetBeautifyTab(BeautifyTab tab, bool rerender) {
    if (beautify_tab_ != tab) {
        beautify_tab_ = tab;
        beautify_scroll_y_ = 0.0f;
    }
    if (rerender && active_kind_ == OverlayKind::BeautifyDrawer) {
        Render();
    }
}

void DesktopOverlayController::SetBeautifyDropHover(bool hover, const std::wstring& filename) {
    beautify_drop_hover_ = hover;
    beautify_drop_filename_ = filename;
    if (hover) {
        beautify_tab_ = BeautifyTab::Wall;
    }
    if (active_kind_ == OverlayKind::BeautifyDrawer) {
        Render();
    }
}

ID2D1Bitmap* DesktopOverlayController::GetBeautifyCachedImage(ID2D1DeviceContext* dc, const std::wstring& path) noexcept {
    if (!dc || path.empty()) return nullptr;
    if (beautify_bmp_dc_ != dc) {
        beautify_img_cache_.clear();
        beautify_live_bmp_.Reset();
        beautify_bmp_dc_ = dc;
    }
    auto it = beautify_img_cache_.find(path);
    if (it != beautify_img_cache_.end()) {
        return it->second.Get();
    }
    if (beautify_img_cache_.size() >= 12u) {
        beautify_img_cache_.clear();
    }
    if (!beautify_wic_) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&beautify_wic_));
    }
    if (!beautify_wic_) return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(beautify_wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                        WICDecodeMetadataCacheOnLoad, &decoder)) || !decoder) {
        beautify_img_cache_[path] = nullptr;
        return nullptr;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame) {
        beautify_img_cache_[path] = nullptr;
        return nullptr;
    }
    UINT fw = 0, fh = 0;
    if (FAILED(frame->GetSize(&fw, &fh)) || fw == 0 || fh == 0) {
        beautify_img_cache_[path] = nullptr;
        return nullptr;
    }

    // Scale down to <= 672px wide (2x preview width) so drawer image cache stays tiny (< 2 MB total).
    ComPtr<IWICBitmapSource> src = frame;
    if (fw > 672u || fh > 384u) {
        const double sc = std::min(672.0 / static_cast<double>(fw), 384.0 / static_cast<double>(fh));
        const UINT sw = std::max(1u, static_cast<UINT>(std::lround(fw * sc)));
        const UINT sh = std::max(1u, static_cast<UINT>(std::lround(fh * sc)));
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(beautify_wic_->CreateBitmapScaler(&scaler)) && scaler &&
            SUCCEEDED(scaler->Initialize(frame.Get(), sw, sh, WICBitmapInterpolationModeFant))) {
            src = scaler;
            fw = sw;
            fh = sh;
        }
    }

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(beautify_wic_->CreateFormatConverter(&conv)) || !conv ||
        FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        beautify_img_cache_[path] = nullptr;
        return nullptr;
    }

    const uint32_t stride = fw * 4u;
    std::vector<uint8_t> pixels(static_cast<size_t>(stride) * fh);
    if (FAILED(conv->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()))) {
        beautify_img_cache_[path] = nullptr;
        return nullptr;
    }

    const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap> bmp;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(fw, fh), pixels.data(), stride, bp, &bmp)) || !bmp) {
        beautify_img_cache_[path] = nullptr;
        return nullptr;
    }
    beautify_img_cache_[path] = bmp;
    return bmp.Get();
}

ID2D1Bitmap* DesktopOverlayController::GetBeautifySystemWallpaper(ID2D1DeviceContext* dc) noexcept {
    if (!dc) return nullptr;
    if (backdrop_ && dc == backdrop_owner_dc_ &&
        backdrop_->ActivePreset() == model::WallpaperPreset::System &&
        backdrop_->QuarterWallpaperBitmap()) {
        return backdrop_->QuarterWallpaperBitmap();
    }
    const std::wstring wp = render::WallpaperBackdrop::QueryCurrentWallpaperPath();
    if (!wp.empty()) {
        if (ID2D1Bitmap* bmp = GetBeautifyCachedImage(dc, wp)) return bmp;
    }
    return nullptr;
}

bool DesktopOverlayController::OpenBeautifyDrawer(const RECT& work_area,
                                                  const model::BeautifySettings& settings,
                                                  const std::unordered_set<uint8_t>& active_widget_kinds) {
    const bool already_open = (active_kind_ == OverlayKind::BeautifyDrawer);
    if (!already_open) {
        Dismiss();
    }
    active_kind_ = OverlayKind::BeautifyDrawer;
    work_area_ = work_area;
    beautify_settings_ = settings;
    active_widget_kinds_ = active_widget_kinds;
    if (!already_open) {
        beautify_scroll_y_ = 0.0f;
        beautify_max_scroll_y_ = 0.0f;
    }
    beautify_sliders_.clear();
    dragging_beautify_slider_.clear();

    const int w = 372;
    const int avail_h = std::max<int>(520, (work_area.bottom - work_area.top) - 28);
    const int h = std::min<int>(avail_h, 700);
    const int x = work_area.right - w - 16;
    const int y = work_area.top + 14;
    RepositionAndShow(x, y, w, h, !already_open);
    AttachBeautifyDropTarget();
    return true;
}

bool DesktopOverlayController::OpenWidgetGalleryDrawer(const RECT& work_area,
                                                       int tile_board_screen_left,
                                                       int tile_board_screen_top,
                                                       std::span<const model::DesktopTile> current_tiles,
                                                       std::optional<model::TileKind> initial_kind,
                                                       std::optional<model::PointI> target_cell) {
    Dismiss();
    active_kind_ = OverlayKind::WidgetGalleryDrawer;
    work_area_ = work_area;
    gallery_board_left_ = tile_board_screen_left;
    gallery_board_top_ = tile_board_screen_top;
    gallery_target_cell_ = target_cell;
    UpdateWidgetGalleryTiles(current_tiles);

    if (initial_kind.has_value()) {
        gallery_selected_kind_ = *initial_kind;
    } else if (gallery_target_cell_.has_value()) {
        // Prefer an unplaced widget that supports 1x1 when clicking an empty + cell, else Calendar.
        bool picked = false;
        for (const auto& w : kGalleryWidgets) {
            if (!model::BoardHasKind(gallery_tiles_, w.kind)) {
                for (const auto& sz : w.sizes) {
                    if (sz.cols == 1 && sz.rows == 1) {
                        gallery_selected_kind_ = w.kind;
                        picked = true;
                        break;
                    }
                }
                if (picked) break;
            }
        }
        if (!picked) gallery_selected_kind_ = model::TileKind::Calendar;
    }

    const auto& def = FindGalleryWidgetDef(gallery_selected_kind_);
    bool valid_sz = false;
    for (const auto& sz : def.sizes) {
        if (sz == gallery_selected_size_) { valid_sz = true; break; }
    }
    if (gallery_target_cell_.has_value()) {
        for (const auto& sz : def.sizes) {
            if (sz.cols == 1 && sz.rows == 1) {
                gallery_selected_size_ = {1, 1};
                valid_sz = true;
                break;
            }
        }
    }
    if (!valid_sz) {
        gallery_selected_size_ = *def.sizes.begin();
        for (const auto& sz : def.sizes) {
            if (sz.cols == 2 && sz.rows == 2) {
                gallery_selected_size_ = {2, 2};
                break;
            }
        }
    }

    const int w = 392;
    const int avail_h = (work_area.bottom - work_area.top) - 28;
    const int h = std::clamp<int>(avail_h, 620, 676);
    const int x = (tile_board_screen_left > work_area.left + w + 40)
                      ? (tile_board_screen_left - w - 24)
                      : std::max<int>(work_area.left + 16, work_area.right - w - 16);
    const int y = std::clamp<int>(tile_board_screen_top - 30,
                                  work_area.top + 14,
                                  std::max<int>(work_area.top + 14, work_area.bottom - h - 14));
    RepositionAndShow(x, y, w, h, true);
    return true;
}

void DesktopOverlayController::UpdateWidgetGalleryTiles(std::span<const model::DesktopTile> current_tiles) {
    gallery_tiles_.assign(current_tiles.begin(), current_tiles.end());
    active_widget_kinds_.clear();
    for (const auto& w : kGalleryWidgets) {
        if (model::BoardHasKind(gallery_tiles_, w.kind)) {
            active_widget_kinds_.insert(static_cast<uint8_t>(w.kind));
        }
    }
    if (active_kind_ == OverlayKind::WidgetGalleryDrawer) {
        gallery_target_cell_.reset();
        Render();
    }
}

bool DesktopOverlayController::OpenAiOrganizeDrawer(const RECT& work_area,
                                                    const AiDrawerViewState& state) {
    Dismiss();
    active_kind_ = OverlayKind::AiOrganizeDrawer;
    work_area_ = work_area;
    ai_state_ = state;
    ai_scroll_y_ = 0.0f;
    ai_max_scroll_y_ = 0.0f;

    const int w = 400;
    const int h = std::max<int>(580, (work_area.bottom - work_area.top) - 28);
    const int x = work_area.right - w - 16;
    const int y = work_area.top + 14;
    RepositionAndShow(x, y, w, h, true);
    return true;
}

void DesktopOverlayController::UpdateAiOrganizeDrawer(const AiDrawerViewState& state) {
    ai_state_ = state;
    if (active_kind_ != OverlayKind::AiOrganizeDrawer) return;
    if (ai_state_.phase != AiDrawerPhase::PlanReady) {
        ai_scroll_y_ = 0.0f;
        ai_max_scroll_y_ = 0.0f;
    }
    Render();
}

// ---------------------------------------------------------------------------
// 5. Popovers, Inline F2 Rename, & Capsule Toast
// ---------------------------------------------------------------------------
bool DesktopOverlayController::OpenStagingTrayPopover(const RECT& anchor_screen_rc,
                                                      std::vector<render::TrayViewEntry> entries,
                                                      uint64_t total_bytes) {
    Dismiss();
    active_kind_ = OverlayKind::StagingTrayPopover;
    tray_entries_ = std::move(entries);
    tray_total_bytes_ = total_bytes;

    const int w = 356;
    const int h = 324;
    int x = anchor_screen_rc.left - w - 10;
    if (x < 12) x = anchor_screen_rc.left;
    const int y = std::clamp<int>(anchor_screen_rc.bottom - h, 12, std::max(12, static_cast<int>(anchor_screen_rc.top)));
    RepositionAndShow(x, y, w, h, true);
    return true;
}

bool DesktopOverlayController::OpenGroupRulePopover(const RECT& anchor_screen_rc,
                                                    const model::DesktopGroup& group) {
    Dismiss();
    active_kind_ = OverlayKind::GroupRulePopover;
    rule_group_ = group;

    const int w = 356;
    const int h = 348;
    const int x = std::max<int>(12, anchor_screen_rc.left);
    const int y = anchor_screen_rc.bottom + 6;
    RepositionAndShow(x, y, w, h, true);
    return true;
}

bool DesktopOverlayController::OpenStickyNotePopover(const RECT& anchor_screen_rc,
                                                     const std::string& tile_id,
                                                     const std::wstring& note_body) {
    Dismiss();
    active_kind_ = OverlayKind::StickyNotePopover;
    sticky_tile_id_ = tile_id;
    sticky_note_text_ = note_body;

    const bool single_line = (tile_id == "@weather_city" || tile_id.starts_with("@title:"));
    const int w = 328;
    const int h = single_line ? 138 : 248;
    int x = anchor_screen_rc.left - w - 10;
    if (x < 12) x = anchor_screen_rc.left;
    const int y = std::max<int>(12, anchor_screen_rc.top);
    card_w_ = w;
    card_h_ = h;

    const wchar_t* cue = L"写下待办与灵感…";
    if (tile_id == "@todos") cue = L"每行输入一项待办，回车换行…";
    else if (tile_id == "@weather_city") cue = L"输入城市名称，如 北京、上海…";
    else if (tile_id.starts_with("@title:")) cue = L"输入自定义磁贴名称…";

    const D2D1_RECT_F editor_dip = single_line
                                       ? D2D1::RectF(24.0f, 46.0f, static_cast<float>(w) - 24.0f, 84.0f)
                                       : D2D1::RectF(24.0f, 52.0f, static_cast<float>(w) - 24.0f, static_cast<float>(h) - 56.0f);
    const int len = static_cast<int>(sticky_note_text_.size());
    AttachHostedPopupEdit(editor_dip, sticky_note_text_, cue, !single_line, false,
                          single_line ? 0 : len, len, single_line ? 64u : 1024u);
    RepositionAndShow(x, y, w, h, true);
    return true;
}

bool DesktopOverlayController::BeginInlineRename(
    HWND parent_hwnd, const model::RectI& target_rect,
    const std::wstring& initial_text, bool multiline,
    std::function<void(const std::wstring& committed_text)> on_commit,
    int select_end_char,
    bool allow_empty) {
    CancelInlineRename(false);
    if (!parent_hwnd || !IsWindow(parent_hwnd) || target_rect.empty()) return false;

    const int len = static_cast<int>(initial_text.size());
    const int sel_end = (select_end_char >= 0) ? std::min(select_end_char, len) : len;

    if (parent_hwnd == hwnd_ && active_kind_ == OverlayKind::AiOrganizeDrawer) {
        edit_multiline_ = multiline;
        edit_allow_empty_ = allow_empty;
        edit_on_commit_ = std::move(on_commit);
        rename_stem_len_ = select_end_char;
        if (!AttachHostedPopupEdit(
                D2D1::RectF(43.0f, 86.0f, static_cast<float>(card_w_) - 60.0f, 118.0f),
                initial_text, L"输入补充要求（如：把合同和发票单独放一类）…",
                multiline, true, 0, sel_end, 240)) {
            edit_on_commit_ = {};
            return false;
        }
        SetFocus(popup_edit_hwnd_);
        Render();
        if (using_dcomp_ && dcomp_device_) {
            dcomp_device_->Commit();
        }
        return true;
    }

    POINT origin_px{0, 0};
    ClientToScreen(parent_hwnd, &origin_px);
    const int sw = std::max(84, target_rect.w);
    const int sh = std::max(28, target_rect.h);
    const int sx = ToDip(origin_px.x) + target_rect.x - (sw - target_rect.w) / 2;
    const int sy = ToDip(origin_px.y) + target_rect.y;

    Dismiss();
    active_kind_ = OverlayKind::InlineRename;
    edit_multiline_ = multiline;
    edit_allow_empty_ = allow_empty;
    edit_on_commit_ = std::move(on_commit);
    rename_stem_len_ = select_end_char;
    card_w_ = sw;
    card_h_ = sh;

    if (!AttachHostedPopupEdit(
            D2D1::RectF(8.0f, 2.0f, static_cast<float>(sw) - 8.0f, static_cast<float>(sh) - 2.0f),
            initial_text, L"", multiline, true, 0, sel_end, 255)) {
        Dismiss();
        return false;
    }
    RepositionAndShow(sx, sy, sw, sh, true);
    return true;
}

void DesktopOverlayController::CancelInlineRename(bool commit) noexcept {
    const bool is_drawer_edit = (active_kind_ == OverlayKind::AiOrganizeDrawer && popup_edit_hwnd_ != nullptr);
    const bool is_desktop_rename = (active_kind_ == OverlayKind::InlineRename);
    if (!is_drawer_edit && !is_desktop_rename) return;

    const std::wstring text = commit ? ReadHostedPopupEditText() : std::wstring{};
    auto cb = std::move(edit_on_commit_);
    edit_on_commit_ = {};
    const bool allow_empty = edit_allow_empty_;
    edit_allow_empty_ = false;
    edit_multiline_ = false;
    rename_stem_len_ = -1;

    DestroyHostedPopupEdit();
    if (is_desktop_rename) {
        Dismiss();
    } else if (hwnd_ && IsWindowVisible(hwnd_)) {
        SetFocus(hwnd_);
        Render();
        if (using_dcomp_ && dcomp_device_) {
            dcomp_device_->Commit();
        }
    }

    if (commit && cb && (allow_empty || !text.empty())) {
        cb(text);
    }
}

bool DesktopOverlayController::ShowToast(const RECT& work_area, const std::wstring& message,
                                         const std::wstring& action_label,
                                         std::function<void()> on_action,
                                         uint32_t duration_ms) {
    Dismiss();
    EnsureRenderDevices();
    active_kind_ = OverlayKind::ToastCapsule;
    toast_message_ = message;
    toast_action_label_ = action_label;
    toast_on_action_ = std::move(on_action);

    const float msg_w = MeasureTextW(message, fmt_body_.Get(), 460.0f);
    const float act_w = action_label.empty() ? 0.0f : (MeasureTextW(action_label, fmt_body_semi_.Get(), 140.0f) + 28.0f);
    const int w = std::clamp<int>(static_cast<int>(msg_w + act_w + 46.0f), 220, 560);
    const int h = 42;
    const int x = work_area.left + ((work_area.right - work_area.left) - w) / 2;
    const int y = work_area.bottom - h - 26;
    RepositionAndShow(x, y, w, h, false);
    SetTimer(hwnd_, kToastTimerId, std::clamp<UINT>(duration_ms, 1500u, 15000u), nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// Premultiplied-Alpha Rounded Card Frame & Compositor
// ---------------------------------------------------------------------------
void DesktopOverlayController::DrawPopupCardBackground(ID2D1DeviceContext* dc, const D2D1_RECT_F& card_rc,
                                                       float radius, bool with_shadow, bool sticky_tint) {
    const D2D1_RECT_F s_rc = SnapRectF(card_rc);
    if (with_shadow) {
        if (dc == d2d_dc_.Get()) {
            if (!popup_shadow_bitmap_) {
                popup_shadow_bitmap_ = render::CreateExteriorCardShadowBitmap(dc);
            }
            render::DrawExteriorCardShadow(dc, popup_shadow_bitmap_.Get(), s_rc, radius,
                                           dark_mode_ ? 0.96f : 0.76f);
        } else {
            ComPtr<ID2D1Bitmap1> ext_shadow = render::CreateExteriorCardShadowBitmap(dc);
            render::DrawExteriorCardShadow(dc, ext_shadow.Get(), s_rc, radius,
                                           dark_mode_ ? 0.96f : 0.76f);
        }
    }

    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(s_rc, radius, radius);
    const model::CardMaterial eff_mat = (beautify_settings_.default_material == model::CardMaterial::FollowHost)
                                            ? host_material_
                                            : beautify_settings_.default_material;
    // The dock's panels and the widgets' own popups wear the same glass as the widgets.
    const bool is_dock_popup = (active_kind_ == OverlayKind::DockSystemPanel ||
                                active_kind_ == OverlayKind::DockVolumeOsd);
    const bool is_widget_popup = (active_kind_ == OverlayKind::WidgetDetail ||
                                  active_kind_ == OverlayKind::WidgetGalleryDrawer ||
                                  active_kind_ == OverlayKind::BeautifyDrawer ||
                                  active_kind_ == OverlayKind::ContextMenu);
    if (is_dock_popup || is_widget_popup) {
        const bool light_ink = is_dock_popup ? dock_panel_light_ink_ : dark_mode_;
        bool drew_live_glass = false;
        if (backdrop_ != nullptr && eff_mat != model::CardMaterial::Solid) {
            const model::CardMaterial sample_mat =
                (eff_mat == model::CardMaterial::Mica || eff_mat == model::CardMaterial::MicaAlt)
                    ? eff_mat
                    : model::CardMaterial::LiquidGlass;
            float dpi_x = 96.0f, dpi_y = 96.0f;
            dc->GetDpi(&dpi_x, &dpi_y);
            const float eff_scale = (dpi_x > 1.0f) ? (dpi_x / 96.0f) : std::max(0.5f, scale_);
            const D2D1_RECT_F card_px = D2D1::RectF(
                s_rc.left * eff_scale, s_rc.top * eff_scale,
                s_rc.right * eff_scale, s_rc.bottom * eff_scale);
            const POINT popup_screen_px{
                static_cast<LONG>(std::lround((static_cast<float>(card_screen_x_) - s_rc.left) * eff_scale)),
                static_cast<LONG>(std::lround((static_cast<float>(card_screen_y_) - s_rc.top) * eff_scale)),
            };
            dc->SetDpi(96.0f, 96.0f);
            const uint8_t disp_tier = model::MapGlassGainToLegacyTier(beautify_settings_.glass_dispersion);
            render::WallpaperBackdrop::ExternalLiveGlassSource live_src{};
            const render::WallpaperBackdrop::ExternalLiveGlassSource* live_ptr = nullptr;
            if (live_wallpaper_ && live_wallpaper_->HasFrame() && live_wallpaper_->SharpBitmap() &&
                live_wallpaper_owner_dev_) {
                ComPtr<ID2D1Device> cur_dev;
                dc->GetDevice(&cur_dev);
                if (cur_dev && cur_dev.Get() == live_wallpaper_owner_dev_) {
                    live_src.sharp_bmp = live_wallpaper_->SharpBitmap();
                    live_src.sharp_w = live_wallpaper_->SharpWidth();
                    live_src.sharp_h = live_wallpaper_->SharpHeight();
                    live_src.frost_bmp = live_wallpaper_->FrostBitmap();
                    live_src.frost_w = live_wallpaper_->FrostWidth();
                    live_src.frost_h = live_wallpaper_->FrostHeight();
                    live_src.logical_w = live_wallpaper_->LogicalScreenWidth();
                    live_src.logical_h = live_wallpaper_->LogicalScreenHeight();
                    live_ptr = &live_src;
                }
            }
            drew_live_glass = backdrop_->DrawMenuMaterial(
                dc, card_px, popup_screen_px, radius * eff_scale, sample_mat, light_ink,
                beautify_settings_.card_opacity_percent, disp_tier, &beautify_settings_, live_ptr);
            dc->SetDpi(dpi_x, dpi_y);
        }

        dock_panel_drew_live_glass_ = drew_live_glass;
        if (drew_live_glass && is_widget_popup) {
            // DrawMenuMaterial already applies the adaptive theme veil; add only a subtle lift.
            dc->FillRoundedRectangle(rr, Brush(dc, light_ink ? 0x14101218u : 0x16FFFFFFu));
        }
        if (!drew_live_glass) {
            const uint32_t glass_bg = light_ink ? 0xD2181C28u : 0xCCF2F6FCu;
            dc->FillRoundedRectangle(rr, Brush(dc, glass_bg));
            if (backdrop_ != nullptr) {
                ID2D1BitmapBrush* nb = (dc == backdrop_owner_dc_ && backdrop_->NoiseBrush() != nullptr)
                                           ? backdrop_->NoiseBrush()
                                           : backdrop_->MenuNoiseBrush(dc);
                if (nb != nullptr) dc->FillRoundedRectangle(rr, nb);
            }
            const D2D1_GRADIENT_STOP stops[3] = {
                {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
                {0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, light_ink ? 0.34f : 0.56f)},
                {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
            };
            ComPtr<ID2D1GradientStopCollection> sc;
            ComPtr<ID2D1LinearGradientBrush> lg;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 3, &sc)) && sc &&
                SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(s_rc.left + radius, 0.0f),
                                                        D2D1::Point2F(s_rc.right - radius, 0.0f)),
                    sc.Get(), &lg)) && lg) {
                dc->DrawLine(D2D1::Point2F(s_rc.left + radius * 0.85f, s_rc.top + 1.0f),
                             D2D1::Point2F(s_rc.right - radius * 0.85f, s_rc.top + 1.0f), lg.Get(), 1.1f);
            }
        }
        return;
    }

    const bool use_grain = (eff_mat == model::CardMaterial::Acrylic ||
                            eff_mat == model::CardMaterial::FollowHost);
    if (backdrop_ != nullptr && dc == backdrop_owner_dc_ && backdrop_->AcrylicBrush() != nullptr) {
        dc->FillRoundedRectangle(rr, backdrop_->AcrylicBrush());
        const uint32_t tint = dark_mode_ ? 0xDC1C202Cu : 0xE5F7F9FCu;
        dc->FillRoundedRectangle(rr, Brush(dc, tint));
    } else {
        const uint32_t bg = dark_mode_ ? 0xF51F2430u : 0xFBF9FAFCu;
        dc->FillRoundedRectangle(rr, Brush(dc, bg));
    }

    if (sticky_tint) {
        dc->FillRoundedRectangle(rr, Brush(dc, dark_mode_ ? 0x4876621Eu : 0x66FCE178u));
    }
    if (use_grain && backdrop_ != nullptr) {
        ID2D1BitmapBrush* nb = (dc == backdrop_owner_dc_ && backdrop_->NoiseBrush() != nullptr)
                                   ? backdrop_->NoiseBrush()
                                   : backdrop_->MenuNoiseBrush(dc);
        if (nb != nullptr) {
            dc->FillRoundedRectangle(rr, nb);
        }
    }
}

void DesktopOverlayController::DrawPopupCardBorder(ID2D1DeviceContext* dc, const D2D1_RECT_F& card_rc,
                                                   float radius, bool sticky_tint) {
    if (active_kind_ == OverlayKind::DockSystemPanel || active_kind_ == OverlayKind::DockVolumeOsd ||
        active_kind_ == OverlayKind::WidgetDetail || active_kind_ == OverlayKind::WidgetGalleryDrawer ||
        active_kind_ == OverlayKind::BeautifyDrawer || active_kind_ == OverlayKind::ContextMenu) {
        // WacOS TileLiquidGlassRenderer already paints the exact circular SDF directional specular rim
        // and concentric hairline inside DrawMenuMaterial; avoid double-bordering live glass corners.
        if (dock_panel_drew_live_glass_) {
            return;
        }
        const bool ink = (active_kind_ == OverlayKind::DockSystemPanel || active_kind_ == OverlayKind::DockVolumeOsd)
                             ? dock_panel_light_ink_ : dark_mode_;
        const uint32_t fallback_border = ink ? 0x38FFFFFFu : 0x24000000u;
        StrokeCrispRoundedRect(dc, card_rc, radius, Brush(dc, fallback_border), 1.0f);
        return;
    }
    const uint32_t border_argb = sticky_tint
                                     ? (dark_mode_ ? 0x3AFFE58Fu : 0x348A6800u)
                                     : (dark_mode_ ? 0x2AFFFFFFu : 0x18000000u);
    StrokeCrispRoundedRect(dc, card_rc, radius, Brush(dc, border_argb), 1.0f);
}

void DesktopOverlayController::Render() {
    if (!d2d_dc_ || !swap_chain_ || !target_bitmap_ || active_kind_ == OverlayKind::None) return;

    d2d_dc_->SetTarget(target_bitmap_.Get());
    d2d_dc_->SetDpi(96.0f * scale_, 96.0f * scale_);
    click_regions_.clear();
    pulse_buttons_.clear();

    d2d_dc_->BeginDraw();
    if (using_dcomp_) {
        d2d_dc_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        DrawMainContent(d2d_dc_.Get(), static_cast<float>(kShadowMargin), static_cast<float>(kShadowMargin), true);
    } else {
        d2d_dc_->Clear(ArgbColor(dark_mode_ ? 0xFF222530u : 0xFFF9FAFCu));
        DrawMainContent(d2d_dc_.Get(), 0.0f, 0.0f, false);
    }

    if (SUCCEEDED(d2d_dc_->EndDraw())) {
        PaintPulseControls();
        DXGI_PRESENT_PARAMETERS pp{};
        swap_chain_->Present1(0, 0, &pp);
    }
}

void DesktopOverlayController::RenderSubmenu() {
    if (!submenu_open_ || !d2d_dc_ || !sub_swap_chain_ || !sub_target_bitmap_) return;

    d2d_dc_->SetTarget(sub_target_bitmap_.Get());
    d2d_dc_->SetDpi(96.0f * scale_, 96.0f * scale_);
    d2d_dc_->BeginDraw();
    if (sub_using_dcomp_) {
        d2d_dc_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        DrawSubmenuContent(d2d_dc_.Get(), static_cast<float>(kShadowMargin), static_cast<float>(kShadowMargin), true);
    } else {
        d2d_dc_->Clear(ArgbColor(dark_mode_ ? 0xFF222530u : 0xFFF9FAFCu));
        DrawSubmenuContent(d2d_dc_.Get(), 0.0f, 0.0f, false);
    }
    if (SUCCEEDED(d2d_dc_->EndDraw())) {
        DXGI_PRESENT_PARAMETERS pp{};
        sub_swap_chain_->Present1(0, 0, &pp);
    }
}

void DesktopOverlayController::CompositeOnto(ID2D1DeviceContext* dc, int origin_x, int origin_y) {
    if (!dc || active_kind_ == OverlayKind::None) return;
    EnsureRenderDevices();

    if (active_kind_ == OverlayKind::QuickLook) {
        D2D1_SIZE_F tsz = dc->GetSize();
        dc->FillRectangle(D2D1::RectF(0.0f, 0.0f, tsz.width, tsz.height), Brush(dc, 0x48000000u));
    }

    click_regions_.clear();
    DrawMainContent(dc, static_cast<float>(card_screen_x_ - origin_x),
                    static_cast<float>(card_screen_y_ - origin_y), true);

    if (submenu_open_ && !submenu_rows_.empty()) {
        DrawSubmenuContent(dc, static_cast<float>(sub_screen_x_ - origin_x),
                           static_cast<float>(sub_screen_y_ - origin_y), true);
    }

    g_ext_brush.Reset();
    g_ext_brush_dc = nullptr;
}

void DesktopOverlayController::DrawMainContent(ID2D1DeviceContext* dc, float ox, float oy, bool with_shadow) {
    // macOS radii: panels and popovers 22 (the widgets' radius), menus 14, small fields 8.
    float radius = 22.0f;
    if (active_kind_ == OverlayKind::ContextMenu) radius = 14.0f;
    else if (active_kind_ == OverlayKind::InlineRename) radius = 8.0f;
    else if (active_kind_ == OverlayKind::HealthDrawer ||
             active_kind_ == OverlayKind::BeautifyDrawer ||
             active_kind_ == OverlayKind::AiOrganizeDrawer ||
             active_kind_ == OverlayKind::WidgetGalleryDrawer || active_kind_ == OverlayKind::WidgetDetail) radius = 22.0f;
    else if (active_kind_ == OverlayKind::DockSystemPanel) radius = 22.0f;
    else if (active_kind_ == OverlayKind::DockVolumeOsd) radius = 20.0f;
    else if (active_kind_ == OverlayKind::ToastCapsule) radius = 21.0f;
    // An opaque window is clipped by DWM at its own 8 px corner with its own border: drawing a larger corner
    // and a second border inside it is what showed as a doubled corner.
    const bool dwm_shape = dc == d2d_dc_.Get() && !using_dcomp_;
    if (dwm_shape) radius = 8.0f;

    const D2D1_RECT_F card_rc = SnapRectF(
        D2D1::RectF(ox, oy, ox + static_cast<float>(card_w_), oy + static_cast<float>(card_h_)));
    const bool sticky_tint = (active_kind_ == OverlayKind::StickyNotePopover &&
                              sticky_tile_id_ != "@todos" &&
                              sticky_tile_id_ != "@weather_city" &&
                              !sticky_tile_id_.starts_with("@title:"));
    DrawPopupCardBackground(dc, card_rc, radius, with_shadow, sticky_tint);

    const D2D1_RECT_F clip_rc = D2D1::RectF(card_rc.left + 0.5f, card_rc.top + 0.5f,
                                            card_rc.right - 0.5f, card_rc.bottom - 0.5f);
    const float clip_r = std::max(0.0f, radius - 0.5f);

    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);
    ComPtr<ID2D1RoundedRectangleGeometry> clip_geo;
    if (fac && clip_rc.right > clip_rc.left && clip_rc.bottom > clip_rc.top) {
        fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(clip_rc, clip_r, clip_r), &clip_geo);
    }
    if (clip_geo) {
        dc->PushLayer(
            D2D1::LayerParameters1(clip_rc, clip_geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                   D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE),
            nullptr);
    }

    switch (active_kind_) {
    case OverlayKind::ContextMenu:
        DrawMenuRowList(dc, ox, oy, static_cast<float>(card_w_), menu_rows_,
                        hovered_row_, hovered_icon_bar_idx_, hovered_tag_idx_);
        break;
    case OverlayKind::SearchPopup:
        DrawSearchPopup(dc, ox, oy);
        break;
    case OverlayKind::QuickLook:
        DrawQuickLook(dc, ox, oy);
        break;
    case OverlayKind::HealthDrawer:
        DrawHealthDrawer(dc, ox, oy);
        break;
    case OverlayKind::BeautifyDrawer:
        DrawBeautifyDrawer(dc, ox, oy);
        break;
    case OverlayKind::WidgetGalleryDrawer:
        DrawWidgetGalleryDrawer(dc, ox, oy);
        break;
    case OverlayKind::AiOrganizeDrawer:
        DrawAiOrganizeDrawer(dc, ox, oy);
        break;
    case OverlayKind::StagingTrayPopover:
        DrawTrayPopover(dc, ox, oy);
        break;
    case OverlayKind::WidgetDetail:
        DrawWidgetDetail(dc, ox, oy);
        break;
    case OverlayKind::DockSystemPanel:
        DrawDockSystemPanel(dc, ox, oy);
        break;
    case OverlayKind::DockVolumeOsd:
        DrawDockVolumeOsd(dc, ox, oy);
        break;
    case OverlayKind::GroupRulePopover:
        DrawRulePopover(dc, ox, oy);
        break;
    case OverlayKind::StickyNotePopover:
        DrawStickyNotePopover(dc, ox, oy);
        break;
    case OverlayKind::ToastCapsule:
        DrawToastCapsule(dc, ox, oy);
        break;
    case OverlayKind::InlineRename:
    case OverlayKind::None:
        break;
    }

    if (clip_geo) {
        dc->PopLayer();
    }
    if (!dwm_shape) {
        if (active_kind_ == OverlayKind::InlineRename) {
            StrokeCrispRoundedRect(dc, card_rc, radius, Brush(dc, accent_argb_), 1.5f);
        } else {
            DrawPopupCardBorder(dc, card_rc, radius, sticky_tint);
        }
    }
}

void DesktopOverlayController::DrawSubmenuContent(ID2D1DeviceContext* dc, float ox, float oy, bool with_shadow) {
    const D2D1_RECT_F card_rc = SnapRectF(
        D2D1::RectF(ox, oy, ox + static_cast<float>(sub_card_w_), oy + static_cast<float>(sub_card_h_)));
    DrawPopupCardBackground(dc, card_rc, 10.0f, with_shadow, false);
    DrawMenuRowList(dc, ox, oy, static_cast<float>(sub_card_w_), submenu_rows_, hovered_sub_row_, -1, -1);
    DrawPopupCardBorder(dc, card_rc, 10.0f, false);
}

// ---------------------------------------------------------------------------
// Individual Overlay Painters (1:1 with desktop_pack_mockup.html)
// ---------------------------------------------------------------------------
void DesktopOverlayController::DrawMenuRowList(ID2D1DeviceContext* dc, float ox, float oy, float card_w,
                                               const std::vector<MenuRow>& rows,
                                               int hot_row, int hot_icon_idx, int hot_tag_idx) {
    const uint32_t pri = dark_mode_ ? 0xF8FFFFFFu : 0xF8181E28u;
    const uint32_t sec = dark_mode_ ? 0x92FFFFFFu : 0x92303846u;
    const uint32_t danger = dark_mode_ ? 0xFFFF6B6Bu : 0xFFD93025u;

    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const D2D1_RECT_F rc = D2D1::RectF(
            ox + static_cast<float>(row.bounds.left), oy + static_cast<float>(row.bounds.top),
            ox + static_cast<float>(row.bounds.right), oy + static_cast<float>(row.bounds.bottom));

        if (row.is_icon_bar) {
            static constexpr wchar_t kIcons[5] = {L'\xE8C6', L'\xE8C8', L'\xE8AC', L'\xE74D', L'\xE72D'};
            const float slot_w = (rc.right - rc.left) / 5.0f;
            for (int b = 0; b < 5; ++b) {
                const float bx = rc.left + slot_w * static_cast<float>(b);
                const D2D1_RECT_F brc = D2D1::RectF(bx + 4.0f, rc.top + 2.0f, bx + slot_w - 4.0f, rc.bottom - 2.0f);
                if (hot_icon_idx == b) {
                    dc->FillRoundedRectangle(D2D1::RoundedRect(brc, 6.0f, 6.0f),
                                             Brush(dc, dark_mode_ ? 0x22FFFFFFu : 0x12000000u));
                }
                const wchar_t gl[2] = {kIcons[b], 0};
                DrawGlyph(dc, gl, 1, fmt_fluent_15_.Get(), brc, Brush(dc, b == 3 ? danger : pri));
            }
        } else if (row.is_tag_row) {
            float tx = rc.left + 16.0f;
            const float ty = (rc.top + rc.bottom) * 0.5f;
            for (size_t t = 0; t < menu_spec_.tag_palette.size() && t < 7; ++t) {
                const bool sel = (menu_spec_.current_tag_id == menu_spec_.tag_palette[t].id);
                const float rad = (static_cast<int>(t) == hot_tag_idx || sel) ? 8.5f : 7.5f;
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx, ty), rad, rad),
                                Brush(dc, menu_spec_.tag_palette[t].argb));
                if (sel) {
                    const wchar_t chk[2] = {L'\xE73E', 0};
                    DrawGlyph(dc, chk, 1, fmt_fluent_12_.Get(),
                                 D2D1::RectF(tx - 8.0f, ty - 8.0f, tx + 8.0f, ty + 8.0f),
                                 Brush(dc, 0xFFFFFFFFu));
                }
                tx += 24.0f;
            }
            dc->DrawText(row.label.c_str(), static_cast<UINT32>(row.label.size()),
                         fmt_small_right_.Get(),
                         D2D1::RectF(rc.right - 56.0f, rc.top, rc.right - 10.0f, rc.bottom),
                         Brush(dc, sec));
        } else {
            const bool hot = (static_cast<int>(i) == hot_row) && row.enabled;
            if (hot) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 6.0f, 6.0f),
                                         Brush(dc, dark_mode_ ? 0x1FFFFFFFu : 0x10000000u));
            }

            const uint32_t fg = !row.enabled
                                    ? (dark_mode_ ? 0x50FFFFFFu : 0x50303846u)
                                    : (row.is_danger ? danger : (row.is_accent_primary ? accent_argb_ : pri));

            // Left icon or checkmark slot [rc.left + 6 .. rc.left + 26]
            if (row.checked) {
                const wchar_t chk[2] = {L'\xE73E', 0};
                DrawGlyph(dc, chk, 1, fmt_fluent_12_.Get(),
                             D2D1::RectF(rc.left + 6.0f, rc.top, rc.left + 26.0f, rc.bottom),
                             Brush(dc, accent_argb_));
            } else if (row.command_id.starts_with("ai-")) {
                DrawSparkleStar(fac.Get(), dc, rc.left + 16.0f, (rc.top + rc.bottom) * 0.5f, 6.4f,
                                Brush(dc, accent_argb_));
            } else if (row.icon_glyph != 0) {
                const wchar_t gl[2] = {row.icon_glyph, 0};
                DrawGlyph(dc, gl, 1, fmt_fluent_12_.Get(),
                             D2D1::RectF(rc.left + 6.0f, rc.top, rc.left + 26.0f, rc.bottom),
                             Brush(dc, row.is_accent_primary ? accent_argb_ : (row.is_danger ? danger : sec)));
            }

            dc->DrawText(row.label.c_str(), static_cast<UINT32>(row.label.size()),
                         row.is_accent_primary ? fmt_body_semi_.Get() : fmt_body_.Get(),
                         D2D1::RectF(rc.left + 30.0f, rc.top, rc.right - 68.0f, rc.bottom),
                         Brush(dc, fg));

            if (!row.submenu_rows.empty()) {
                const wchar_t chev[2] = {L'\xE76C', 0};
                DrawGlyph(dc, chev, 1, fmt_fluent_12_.Get(),
                             D2D1::RectF(rc.right - 22.0f, rc.top, rc.right - 6.0f, rc.bottom),
                             Brush(dc, sec));
            } else if (!row.right_badge.empty()) {
                if (row.is_shell_verb) {
                    const D2D1_RECT_F brc = D2D1::RectF(rc.right - 44.0f, rc.top + 6.0f, rc.right - 8.0f, rc.bottom - 6.0f);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(brc, 4.0f, 4.0f),
                                             Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x12000000u));
                    dc->DrawText(row.right_badge.c_str(), static_cast<UINT32>(row.right_badge.size()),
                                 fmt_small_center_.Get(), brc, Brush(dc, sec));
                } else {
                    dc->DrawText(row.right_badge.c_str(), static_cast<UINT32>(row.right_badge.size()),
                                 fmt_small_right_.Get(),
                                 D2D1::RectF(rc.right - 92.0f, rc.top, rc.right - 10.0f, rc.bottom),
                                 Brush(dc, sec));
                }
            }
        }

        if (row.separator_after) {
            const float sy = std::floor(rc.bottom + 4.0f) + 0.5f;
            dc->DrawLine(D2D1::Point2F(ox + 12.0f, sy), D2D1::Point2F(ox + card_w - 12.0f, sy),
                         Brush(dc, dark_mode_ ? 0x10FFFFFFu : 0x0E000000u), 1.0f);
        }
    }
}

void DesktopOverlayController::DrawSearchPopup(ID2D1DeviceContext* dc, float ox, float oy) {
    const uint32_t pri = dark_mode_ ? 0xF8FFFFFFu : 0xF8181E28u;
    const uint32_t sec = dark_mode_ ? 0x92FFFFFFu : 0x92303846u;
    const float w = static_cast<float>(card_w_);
    const float h = static_cast<float>(card_h_);

    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);

    // Top search bar (50px tall)
    const wchar_t mag[2] = {L'\xE721', 0};
    DrawGlyph(dc, mag, 1, fmt_fluent_15_.Get(),
                 D2D1::RectF(ox + 14.0f, oy + 10.0f, ox + 38.0f, oy + 44.0f),
                 Brush(dc, accent_argb_));
    click_regions_.push_back({"focus-popup-edit", RECT{38, 8, static_cast<LONG>(w - 56), 46}});

    const bool live_edit = (dc == d2d_dc_.Get() && popup_edit_hwnd_ != nullptr && IsWindow(popup_edit_hwnd_));
    if (!live_edit) {
        if (search_query_.empty()) {
            const wchar_t* ph = L"搜索桌面上的文件，顺便找找 Pulse 里的…";
            dc->DrawText(ph, static_cast<UINT32>(std::wcslen(ph)), fmt_body_.Get(),
                         D2D1::RectF(ox + 42.0f, oy + 10.0f, ox + w - 58.0f, oy + 44.0f), Brush(dc, sec));
        } else {
            dc->DrawText(search_query_.c_str(), static_cast<UINT32>(search_query_.size()), fmt_title_.Get(),
                         D2D1::RectF(ox + 42.0f, oy + 10.0f, ox + w - 58.0f, oy + 44.0f), Brush(dc, pri));
        }
    }

    const D2D1_RECT_F esc_rc = D2D1::RectF(ox + w - 50.0f, oy + 15.0f, ox + w - 14.0f, oy + 37.0f);
    dc->FillRoundedRectangle(D2D1::RoundedRect(esc_rc, 5.0f, 5.0f),
                             Brush(dc, dark_mode_ ? 0x1AFFFFFFu : 0x10000000u));
    dc->DrawText(L"Esc", 3, fmt_small_center_.Get(), esc_rc, Brush(dc, sec));
    click_regions_.push_back({"close-overlay", RECT{static_cast<LONG>(w - 50), 15, static_cast<LONG>(w - 14), 37}});

    dc->DrawLine(D2D1::Point2F(ox + 1.0f, std::round(oy + 50.0f) + 0.5f),
                 D2D1::Point2F(ox + w - 1.0f, std::round(oy + 50.0f) + 0.5f),
                 Brush(dc, dark_mode_ ? 0x1AFFFFFFu : 0x12000000u), 1.0f);

    if (search_query_.empty()) {
        const wchar_t* try_lbl = L"试试直接输入关键词，或点下面快速筛选：";
        dc->DrawText(try_lbl, static_cast<UINT32>(std::wcslen(try_lbl)), fmt_small_.Get(),
                     D2D1::RectF(ox + 18.0f, oy + 62.0f, ox + w - 18.0f, oy + 84.0f), Brush(dc, sec));

        static constexpr const wchar_t* kChips[6] = {L"汇报", L"截图", L"pptx", L"安装包", L"图片", L"文档"};
        float cx = 18.0f;
        const float cy = 94.0f;
        for (const wchar_t* ch : kChips) {
            const std::wstring s(ch);
            const float cw = std::round(MeasureTextW(s, fmt_body_.Get(), 120.0f) + 24.0f);
            const D2D1_RECT_F crc = D2D1::RectF(ox + cx, oy + cy, ox + cx + cw, oy + cy + 30.0f);
            dc->FillRoundedRectangle(SnapRoundedRect(crc, 15.0f),
                                     Brush(dc, dark_mode_ ? 0x1AFFFFFFu : 0x10000000u));
            StrokeCrispRoundedRect(dc, crc, 15.0f,
                                   Brush(dc, dark_mode_ ? 0x26FFFFFFu : 0x18000000u), 1.0f);
            dc->DrawText(s.c_str(), static_cast<UINT32>(s.size()), fmt_small_center_.Get(), crc, Brush(dc, pri));
            click_regions_.push_back({"search-chip:" + ipc::WideToUtf8(s),
                                      RECT{static_cast<LONG>(cx), static_cast<LONG>(cy),
                                           static_cast<LONG>(cx + cw), static_cast<LONG>(cy + 30.0f)}});
            cx += cw + 8.0f;
        }
    } else {
        float y = 58.0f;
        int flat_idx = 0;

        auto draw_hit_row = [&](const SearchHitEntry& hit) {
            const bool sel = (flat_idx == search_selected_idx_);
            const D2D1_RECT_F rrc = D2D1::RectF(ox + 10.0f, oy + y, ox + w - 10.0f, oy + y + 36.0f);
            click_regions_.push_back({"search-hit:" + std::to_string(flat_idx),
                                      RECT{10, static_cast<LONG>(y), static_cast<LONG>(w - 10), static_cast<LONG>(y + 36.0f)}});

            if (sel) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(rrc, 8.0f, 8.0f), Brush(dc, accent_argb_));
            }

            render::DrawVectorFileIcon(fac.Get(), dc, dwrite_.Get(),
                                       D2D1::RectF(rrc.left + 10.0f, rrc.top + 8.0f, rrc.left + 30.0f, rrc.top + 28.0f),
                                       hit.name, hit.is_folder);

            const uint32_t row_fg = sel ? (dark_mode_ ? 0xFF0A121Eu : 0xFFFFFFFFu) : pri;
            const uint32_t row_sub = sel ? (dark_mode_ ? 0xCC0A121Eu : 0xE0FFFFFFu) : sec;

            // Highlight matching query substring
            const size_t mpos = FindCaseInsensitivePos(hit.name, search_query_);
            if (mpos != std::wstring_view::npos && !sel) {
                const std::wstring pre = hit.name.substr(0, mpos);
                const std::wstring mid = hit.name.substr(mpos, search_query_.size());
                const float pre_w = MeasureTextW(pre, fmt_body_.Get(), 320.0f);
                const float mid_w = MeasureTextW(mid, fmt_body_.Get(), 200.0f);
                const D2D1_RECT_F hl = D2D1::RectF(rrc.left + 38.0f + pre_w - 1.0f, rrc.top + 8.0f,
                                                   rrc.left + 38.0f + pre_w + mid_w + 2.0f, rrc.bottom - 8.0f);
                dc->FillRoundedRectangle(D2D1::RoundedRect(hl, 3.0f, 3.0f), Brush(dc, accent_argb_, 0.24f));
            }

            dc->DrawText(hit.name.c_str(), static_cast<UINT32>(hit.name.size()),
                         sel ? fmt_body_semi_.Get() : fmt_body_.Get(),
                         D2D1::RectF(rrc.left + 38.0f, rrc.top, rrc.right - 140.0f, rrc.bottom),
                         Brush(dc, row_fg));

            std::wstring loc = !hit.location_badge.empty() ? hit.location_badge : ParentFolderDisplay(hit.path);
            if (hit.size_bytes > 0 && hit.from_local_desktop) {
                loc += L" · " + FormatSizeShort(hit.size_bytes);
            }
            dc->DrawText(loc.c_str(), static_cast<UINT32>(loc.size()), fmt_small_right_.Get(),
                         D2D1::RectF(rrc.right - 144.0f, rrc.top, rrc.right - 12.0f, rrc.bottom),
                         Brush(dc, row_sub));

            y += 38.0f;
            ++flat_idx;
        };

        // Section 1: Desktop
        dc->DrawText(L"桌面", 2, fmt_small_.Get(),
                     D2D1::RectF(ox + 18.0f, oy + y, ox + 120.0f, oy + y + 20.0f), Brush(dc, sec));
        wchar_t dcnt[64]{};
        std::swprintf(dcnt, ARRAYSIZE(dcnt), L"%zu 个，已在桌面上高亮", local_hits_.size());
        dc->DrawText(dcnt, static_cast<UINT32>(std::wcslen(dcnt)), fmt_small_right_.Get(),
                     D2D1::RectF(ox + 140.0f, oy + y, ox + w - 18.0f, oy + y + 20.0f), Brush(dc, accent_argb_));
        y += 22.0f;

        if (local_hits_.empty()) {
            dc->DrawText(L"桌面上没有匹配的文件", 10, fmt_small_.Get(),
                         D2D1::RectF(ox + 22.0f, oy + y, ox + w - 22.0f, oy + y + 26.0f), Brush(dc, sec));
            y += 30.0f;
        } else {
            for (size_t i = 0; i < std::min<size_t>(4, local_hits_.size()); ++i) {
                draw_hit_row(local_hits_[i]);
            }
        }

        // Section 2: In Pulse
        y += 6.0f;
        dc->DrawText(L"在 Pulse 中", 7, fmt_small_.Get(),
                     D2D1::RectF(ox + 18.0f, oy + y, ox + 140.0f, oy + y + 20.0f), Brush(dc, sec));
        dc->DrawText(L"索引 · 刚刚更新", 9, fmt_small_right_.Get(),
                     D2D1::RectF(ox + 160.0f, oy + y, ox + w - 18.0f, oy + y + 20.0f), Brush(dc, sec));
        y += 22.0f;

        for (size_t i = 0; i < std::min<size_t>(3, remote_hits_.size()) && y + 38.0f <= h - 72.0f; ++i) {
            draw_hit_row(remote_hits_[i]);
        }

        // Open all in Pulse row
        const float more_y = h - 68.0f;
        const D2D1_RECT_F mrc = D2D1::RectF(ox + 10.0f, oy + more_y, ox + w - 10.0f, oy + more_y + 32.0f);
        click_regions_.push_back({"search-open-pulse",
                                  RECT{10, static_cast<LONG>(more_y), static_cast<LONG>(w - 10), static_cast<LONG>(more_y + 32.0f)}});
        dc->FillRoundedRectangle(D2D1::RoundedRect(mrc, 7.0f, 7.0f), Brush(dc, accent_argb_, 0.14f));
        const std::wstring more_txt = L"在 Pulse 中查看「" + search_query_ + L"」的全部结果";
        const wchar_t open_g[2] = {L'\xEC50', 0};
        DrawGlyph(dc, open_g, 1, fmt_fluent_12_.Get(),
                     D2D1::RectF(mrc.left + 10.0f, mrc.top, mrc.left + 28.0f, mrc.bottom), Brush(dc, accent_argb_));
        dc->DrawText(more_txt.c_str(), static_cast<UINT32>(more_txt.size()), fmt_body_semi_.Get(),
                     D2D1::RectF(mrc.left + 32.0f, mrc.top, mrc.right - 90.0f, mrc.bottom), Brush(dc, accent_argb_));
        dc->DrawText(L"Ctrl+Enter", 10, fmt_small_right_.Get(),
                     D2D1::RectF(mrc.right - 92.0f, mrc.top, mrc.right - 12.0f, mrc.bottom), Brush(dc, sec));
    }

    // Bottom keyboard hints bar
    const float fy = oy + h - 30.0f;
    dc->DrawLine(D2D1::Point2F(ox + 1.0f, std::round(fy) + 0.5f),
                 D2D1::Point2F(ox + w - 1.0f, std::round(fy) + 0.5f),
                 Brush(dc, dark_mode_ ? 0x16FFFFFFu : 0x10000000u), 1.0f);
    const wchar_t* hints = L"↑↓ 选择     Enter 打开     Ctrl+Enter 在 Pulse 中显示";
    dc->DrawText(hints, static_cast<UINT32>(std::wcslen(hints)), fmt_small_.Get(),
                 D2D1::RectF(ox + 18.0f, fy, ox + w - 18.0f, oy + h), Brush(dc, sec));
}

void DesktopOverlayController::DrawQuickLook(ID2D1DeviceContext* dc, float ox, float oy) {
    const uint32_t pri = dark_mode_ ? 0xF8FFFFFFu : 0xF8181E28u;
    const uint32_t sec = dark_mode_ ? 0x92FFFFFFu : 0x92303846u;
    const float w = static_cast<float>(card_w_);
    const float h = static_cast<float>(card_h_);

    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);

    render::DrawVectorFileIcon(fac.Get(), dc, dwrite_.Get(),
                               D2D1::RectF(ox + 16.0f, oy + 13.0f, ox + 38.0f, oy + 35.0f),
                               ql_item_.extension.empty() ? std::wstring_view(ql_item_.name) : std::wstring_view(ql_item_.extension),
                               ql_item_.is_folder);
    dc->DrawText(ql_item_.name.c_str(), static_cast<UINT32>(ql_item_.name.size()), fmt_title_.Get(),
                 D2D1::RectF(ox + 44.0f, oy + 10.0f, ox + w - 180.0f, oy + 40.0f), Brush(dc, pri));

    const D2D1_RECT_F open_rc = D2D1::RectF(ox + w - 166.0f, oy + 11.0f, ox + w - 50.0f, oy + 37.0f);
    click_regions_.push_back({"ql-open", RECT{static_cast<LONG>(w - 166), 11, static_cast<LONG>(w - 50), 37}});
    dc->FillRoundedRectangle(D2D1::RoundedRect(open_rc, 7.0f, 7.0f), Brush(dc, accent_argb_, 0.22f));
    dc->DrawText(L"打开 (Enter)", 10, fmt_small_center_.Get(), open_rc, Brush(dc, accent_argb_));

    const D2D1_RECT_F cls_rc = D2D1::RectF(ox + w - 42.0f, oy + 11.0f, ox + w - 14.0f, oy + 37.0f);
    click_regions_.push_back({"close-overlay", RECT{static_cast<LONG>(w - 42), 11, static_cast<LONG>(w - 14), 37}});
    const wchar_t x_g[2] = {L'\xE711', 0};
    DrawGlyph(dc, x_g, 1, fmt_fluent_12_.Get(), cls_rc, Brush(dc, sec));

    const D2D1_RECT_F body = D2D1::RectF(ox + 16.0f, oy + 48.0f, ox + w - 16.0f, oy + h - 42.0f);
    dc->FillRoundedRectangle(D2D1::RoundedRect(body, 10.0f, 10.0f),
                             Brush(dc, dark_mode_ ? 0x26000000u : 0x0C000000u));

    if (quicklook_bitmap_ && dc == d2d_dc_.Get()) {
        const D2D1_SIZE_F sz = quicklook_bitmap_->GetSize();
        const float avail_w = body.right - body.left - 24.0f;
        const float avail_h = body.bottom - body.top - 24.0f;
        const float scale = (sz.width > 0 && sz.height > 0)
                                ? std::min({1.0f, avail_w / sz.width, avail_h / sz.height})
                                : 1.0f;
        const float dw = sz.width * scale;
        const float dh = sz.height * scale;
        const float cx = (body.left + body.right) * 0.5f;
        const float cy = (body.top + body.bottom) * 0.5f;
        dc->DrawBitmap(quicklook_bitmap_.Get(),
                       D2D1::RectF(cx - dw * 0.5f, cy - dh * 0.5f, cx + dw * 0.5f, cy + dh * 0.5f));
    } else if (ql_item_.extension == L".png" || ql_item_.extension == L".jpg" ||
               ql_item_.extension == L".jpeg" || ql_item_.extension == L".webp") {
        render::WallpaperBackdrop::DrawPhotoTileScene(
            fac.Get(), dc,
            D2D1::RectF(body.left + 20.0f, body.top + 16.0f, body.right - 20.0f, body.bottom - 16.0f),
            0, dark_mode_);
    } else if (ql_has_preview_ && !ql_preview_.text_excerpt.empty()) {
        dc->DrawText(ql_preview_.text_excerpt.c_str(),
                     static_cast<UINT32>(ql_preview_.text_excerpt.size()),
                     fmt_mono_.Get(),
                     D2D1::RectF(body.left + 16.0f, body.top + 14.0f, body.right - 16.0f, body.bottom - 14.0f),
                     Brush(dc, pri));
    } else {
        render::DrawVectorFileIcon(fac.Get(), dc, dwrite_.Get(),
                                   D2D1::RectF((body.left + body.right) * 0.5f - 36.0f, body.top + 88.0f,
                                               (body.left + body.right) * 0.5f + 36.0f, body.top + 160.0f),
                                   ql_item_.extension.empty() ? std::wstring_view(ql_item_.name) : std::wstring_view(ql_item_.extension),
                                   ql_item_.is_folder);
        const wchar_t* msg = L"按 Enter 用默认应用打开，或 ← / → 快速切换相邻桌面项目";
        dc->DrawText(msg, static_cast<UINT32>(std::wcslen(msg)), fmt_small_center_.Get(),
                     D2D1::RectF(body.left + 20.0f, body.top + 180.0f, body.right - 20.0f, body.top + 210.0f),
                     Brush(dc, sec));
    }

    const uint64_t sz_bytes = ql_has_preview_ && ql_preview_.size_bytes > 0 ? ql_preview_.size_bytes : ql_item_.size_bytes;
    std::wstring meta = FormatSizeShort(sz_bytes);
    if (ql_has_preview_ && !ql_preview_.display_type.empty()) {
        meta = ql_preview_.display_type + L" · " + meta;
    }
    meta += L"     ← / → 切换 · 空格关闭";
    dc->DrawText(meta.c_str(), static_cast<UINT32>(meta.size()), fmt_small_.Get(),
                 D2D1::RectF(ox + 18.0f, oy + h - 34.0f, ox + w - 18.0f, oy + h - 8.0f), Brush(dc, sec));
}

void DesktopOverlayController::DrawHealthDrawer(ID2D1DeviceContext* dc, float ox, float oy) {
    const uint32_t pri = dark_mode_ ? 0xF8FFFFFFu : 0xF8181E28u;
    const uint32_t sec = dark_mode_ ? 0x92FFFFFFu : 0x92303846u;
    const float w = static_cast<float>(card_w_);
    const float h = static_cast<float>(card_h_);

    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);

    // Header
    const wchar_t steth[2] = {L'\xE95E', 0};
    DrawGlyph(dc, steth, 1, fmt_fluent_15_.Get(),
                 D2D1::RectF(ox + 16.0f, oy + 12.0f, ox + 38.0f, oy + 42.0f), Brush(dc, accent_argb_));
    dc->DrawText(L"桌面体检", 4, fmt_title_.Get(),
                 D2D1::RectF(ox + 40.0f, oy + 12.0f, ox + w - 52.0f, oy + 42.0f), Brush(dc, pri));

    const D2D1_RECT_F cls_rc = D2D1::RectF(ox + w - 42.0f, oy + 13.0f, ox + w - 14.0f, oy + 41.0f);
    click_regions_.push_back({"close-overlay", RECT{static_cast<LONG>(w - 42), 13, static_cast<LONG>(w - 14), 41}});
    const wchar_t x_g[2] = {L'\xE711', 0};
    DrawGlyph(dc, x_g, 1, fmt_fluent_12_.Get(), cls_rc, Brush(dc, sec));

    if (health_completed_) {
        const float cx = ox + w * 0.5f;
        const float cy = oy + h * 0.42f;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 28.0f, 28.0f), Brush(dc, 0xFF30A46Cu, 0.18f));
        const wchar_t chk[2] = {L'\xE73E', 0};
        DrawGlyph(dc, chk, 1, fmt_fluent_20_.Get(),
                     D2D1::RectF(cx - 24.0f, cy - 24.0f, cx + 24.0f, cy + 24.0f), Brush(dc, 0xFF30A46Cu));

        const wchar_t* done_title = L"桌面清爽多了";
        dc->DrawText(done_title, static_cast<UINT32>(std::wcslen(done_title)), fmt_stat_num_.Get(),
                     D2D1::RectF(ox + 24.0f, cy + 40.0f, ox + w - 24.0f, cy + 68.0f), Brush(dc, pri));

        wchar_t done_sub[96]{};
        const std::wstring freed = FormatSizeShort(health_cleaned_bytes_);
        std::swprintf(done_sub, ARRAYSIZE(done_sub), L"已整理 %zu 项文件，腾出 %ls 空间",
                      health_cleaned_count_, freed.c_str());
        dc->DrawText(done_sub, static_cast<UINT32>(std::wcslen(done_sub)), fmt_small_center_.Get(),
                     D2D1::RectF(ox + 24.0f, cy + 70.0f, ox + w - 24.0f, cy + 94.0f), Brush(dc, sec));

        const float hb = cy - oy + 112.0f;
        PulseButton(dc, ox, oy, D2D1::RectF(w * 0.5f - 108.0f, hb, w * 0.5f - 8.0f, hb + 32.0f), L"撤销", "health-undo", false);
        PulseButton(dc, ox, oy, D2D1::RectF(w * 0.5f + 8.0f, hb, w * 0.5f + 108.0f, hb + 32.0f), L"完成", "close-overlay", true);
        return;
    }

    size_t selected_count = 0;
    uint64_t selected_bytes = 0;
    for (const auto& iss : health_report_.issues) {
        if (iss.selected_by_default) {
            ++selected_count;
            selected_bytes += iss.reclaimable_bytes;
        }
    }
    const size_t after_items = (health_total_desktop_items_ > selected_count)
                                   ? (health_total_desktop_items_ - selected_count)
                                   : 0;

    // 3-Column Summary Stat Cards
    const float stat_w = (w - 48.0f) / 3.0f;
    wchar_t s0[32]{}, s2[32]{};
    std::swprintf(s0, ARRAYSIZE(s0), L"%zu → %zu", health_total_desktop_items_, after_items);
    const std::wstring s1 = FormatSizeShort(selected_bytes > 0 ? selected_bytes : health_report_.total_reclaimable_bytes);
    std::swprintf(s2, ARRAYSIZE(s2), L"%zu", health_report_.issues.size());

    const wchar_t* stat_vals[3] = {s0, s1.c_str(), s2};
    const wchar_t* stat_lbls[3] = {L"桌面图标", L"可以腾出", L"条建议"};
    for (int c = 0; c < 3; ++c) {
        const float sx = ox + 16.0f + static_cast<float>(c) * (stat_w + 8.0f);
        const D2D1_RECT_F src = SnapRectF(D2D1::RectF(sx, oy + 50.0f, sx + stat_w, oy + 110.0f));
        dc->FillRoundedRectangle(SnapRoundedRect(src, 10.0f),
                                 Brush(dc, dark_mode_ ? 0x16FFFFFFu : 0x0C000000u));
        StrokeCrispRoundedRect(dc, src, 10.0f,
                               Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x12000000u), 1.0f);
        dc->DrawText(stat_vals[c], static_cast<UINT32>(std::wcslen(stat_vals[c])), fmt_stat_num_.Get(),
                     D2D1::RectF(src.left + 4.0f, src.top + 8.0f, src.right - 4.0f, src.top + 36.0f),
                     Brush(dc, c == 1 ? accent_argb_ : pri));
        dc->DrawText(stat_lbls[c], static_cast<UINT32>(std::wcslen(stat_lbls[c])), fmt_small_center_.Get(),
                     D2D1::RectF(src.left + 4.0f, src.top + 36.0f, src.right - 4.0f, src.bottom - 6.0f),
                     Brush(dc, sec));
    }

    // 4 Categorized Sections
    static constexpr const wchar_t* kCatNames[4] = {
        L"安装包：软件已经装好",
        L"超过 30 天没改过",
        L"空文件夹",
        L"用过就能删",
    };

    float y = 122.0f;
    const float max_list_y = h - 68.0f;
    for (int cat = 0; cat < 4 && y + 36.0f <= max_list_y; ++cat) {
        std::vector<size_t> indices;
        bool all_sel = true;
        for (size_t i = 0; i < health_report_.issues.size(); ++i) {
            if (HealthCategoryIndex(health_report_.issues[i].kind) == cat) {
                indices.push_back(i);
                if (!health_report_.issues[i].selected_by_default) all_sel = false;
            }
        }
        if (indices.empty()) continue;

        dc->DrawText(kCatNames[cat], static_cast<UINT32>(std::wcslen(kCatNames[cat])), fmt_body_semi_.Get(),
                     D2D1::RectF(ox + 18.0f, oy + y, ox + w - 84.0f, oy + y + 24.0f), Brush(dc, sec));

        const wchar_t* sel_btn = all_sel ? L"全不选" : L"全选";
        const D2D1_RECT_F brc = D2D1::RectF(ox + w - 70.0f, oy + y + 2.0f, ox + w - 16.0f, oy + y + 22.0f);
        click_regions_.push_back({"health-cat:" + std::to_string(cat),
                                  RECT{static_cast<LONG>(w - 70), static_cast<LONG>(y + 2),
                                       static_cast<LONG>(w - 16), static_cast<LONG>(y + 22)}});
        dc->DrawText(sel_btn, static_cast<UINT32>(std::wcslen(sel_btn)), fmt_small_right_.Get(),
                     D2D1::RectF(brc.left, brc.top, brc.right - 2.0f, brc.bottom), Brush(dc, accent_argb_));
        y += 26.0f;

        for (size_t idx : indices) {
            if (y + 42.0f > max_list_y) break;
            const auto& iss = health_report_.issues[idx];
            const D2D1_RECT_F rrc = D2D1::RectF(ox + 14.0f, oy + y, ox + w - 14.0f, oy + y + 40.0f);
            click_regions_.push_back({"health-toggle:" + std::to_string(idx),
                                      RECT{14, static_cast<LONG>(y), static_cast<LONG>(w - 14), static_cast<LONG>(y + 40)}});

            dc->FillRoundedRectangle(D2D1::RoundedRect(rrc, 8.0f, 8.0f),
                                     Brush(dc, dark_mode_ ? 0x12FFFFFFu : 0x08000000u));

            // Checkbox
            const D2D1_RECT_F cb = D2D1::RectF(rrc.left + 10.0f, rrc.top + 12.0f, rrc.left + 26.0f, rrc.top + 28.0f);
            if (iss.selected_by_default) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(cb, 4.0f, 4.0f), Brush(dc, accent_argb_));
                const wchar_t chk[2] = {L'\xE73E', 0};
                DrawGlyph(dc, chk, 1, fmt_fluent_12_.Get(), cb,
                             Brush(dc, dark_mode_ ? 0xFF0A121Eu : 0xFFFFFFFFu));
            } else {
                dc->DrawRoundedRectangle(D2D1::RoundedRect(cb, 4.0f, 4.0f),
                                         Brush(dc, dark_mode_ ? 0x55FFFFFFu : 0x45000000u), 1.2f);
            }

            const bool is_dir = (iss.kind == model::HealthIssueKind::EmptyFolder);
            render::DrawVectorFileIcon(fac.Get(), dc, dwrite_.Get(),
                                       D2D1::RectF(rrc.left + 32.0f, rrc.top + 9.0f, rrc.left + 54.0f, rrc.top + 31.0f),
                                       iss.name, is_dir);

            dc->DrawText(iss.name.c_str(), static_cast<UINT32>(iss.name.size()), fmt_body_.Get(),
                         D2D1::RectF(rrc.left + 60.0f, rrc.top + 3.0f, rrc.right - 66.0f, rrc.top + 22.0f),
                         Brush(dc, pri));
            dc->DrawText(iss.reason.c_str(), static_cast<UINT32>(iss.reason.size()), fmt_small_.Get(),
                         D2D1::RectF(rrc.left + 60.0f, rrc.top + 21.0f, rrc.right - 66.0f, rrc.bottom - 2.0f),
                         Brush(dc, sec));

            if (iss.reclaimable_bytes > 0) {
                const std::wstring sz_str = FormatSizeShort(iss.reclaimable_bytes);
                dc->DrawText(sz_str.c_str(), static_cast<UINT32>(sz_str.size()), fmt_small_right_.Get(),
                             D2D1::RectF(rrc.right - 68.0f, rrc.top, rrc.right - 10.0f, rrc.bottom),
                             Brush(dc, sec));
            }
            y += 43.0f;
        }
        y += 8.0f;
    }

    // Footer action bar
    const float fy = oy + h - 58.0f;
    dc->DrawLine(D2D1::Point2F(ox + 1.0f, std::round(fy) + 0.5f),
                 D2D1::Point2F(ox + w - 1.0f, std::round(fy) + 0.5f),
                 Brush(dc, dark_mode_ ? 0x1AFFFFFFu : 0x12000000u), 1.0f);

    wchar_t foot_sum[64]{};
    const std::wstring sel_sz = FormatSizeShort(selected_bytes);
    std::swprintf(foot_sum, ARRAYSIZE(foot_sum), L"已选 %zu 项 · %ls", selected_count, sel_sz.c_str());
    dc->DrawText(foot_sum, static_cast<UINT32>(std::wcslen(foot_sum)), fmt_small_.Get(),
                 D2D1::RectF(ox + 16.0f, fy, ox + 178.0f, oy + h), Brush(dc, sec));

    const D2D1_RECT_F arch_rc = D2D1::RectF(ox + w - 186.0f, fy + 12.0f, ox + w - 122.0f, oy + h - 12.0f);
    const D2D1_RECT_F clean_rc = D2D1::RectF(ox + w - 114.0f, fy + 12.0f, ox + w - 16.0f, oy + h - 12.0f);
    click_regions_.push_back({"health-archive", RECT{static_cast<LONG>(w - 186), static_cast<LONG>(h - 46),
                                                     static_cast<LONG>(w - 122), static_cast<LONG>(h - 12)}});
    click_regions_.push_back({"health-recycle", RECT{static_cast<LONG>(w - 114), static_cast<LONG>(h - 46),
                                                     static_cast<LONG>(w - 16), static_cast<LONG>(h - 12)}});

    dc->FillRoundedRectangle(D2D1::RoundedRect(arch_rc, 8.0f, 8.0f),
                             Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x12000000u));
    dc->DrawText(L"归档", 2, fmt_small_center_.Get(), arch_rc, Brush(dc, pri));

    dc->FillRoundedRectangle(D2D1::RoundedRect(clean_rc, 8.0f, 8.0f),
                             Brush(dc, accent_argb_, selected_count > 0 ? 1.0f : 0.40f));
    wchar_t clean_lbl[32]{};
    std::swprintf(clean_lbl, ARRAYSIZE(clean_lbl), L"整理 %zu 项", selected_count);
    dc->DrawText(clean_lbl, static_cast<UINT32>(std::wcslen(clean_lbl)), fmt_small_center_.Get(),
                 clean_rc, Brush(dc, dark_mode_ ? 0xFF0A121Eu : 0xFFFFFFFFu));
}

void DesktopOverlayController::NotifyBeautifyChanged(bool dragging) {
    if (callbacks_.on_beautify_live_changed) {
        callbacks_.on_beautify_live_changed(beautify_settings_, dragging);
    } else if (callbacks_.on_beautify_changed) {
        callbacks_.on_beautify_changed(beautify_settings_);
    }
}

bool DesktopOverlayController::ApplyBeautifySliderAtX(const std::string& id, int card_x) noexcept {
    const float span = std::max(1.0f, static_cast<float>(card_w_) - 36.0f);
    const float t = std::clamp((static_cast<float>(card_x) - 18.0f) / span, 0.0f, 1.0f);
    for (size_t i=0;i<kGlassOpticalFields.size();++i) {
        if (id != kGlassControlIds[i]) continue;
        const auto& field=kGlassOpticalFields[i];
        const double previous=beautify_settings_.glass_options.*(field.member);
        const double next=field.min+static_cast<double>(t)*(field.max-field.min);
        beautify_settings_.glass_options.*(field.member)=next;
        const bool changed=previous!=next||!beautify_settings_.glass_reference;
        beautify_settings_.glass_reference=true;
        beautify_settings_.glass_preset=static_cast<uint8_t>(model::GlassPreset::Custom);
        return changed;
    }
    return false;
}

void DesktopOverlayController::DrawBeautifyDrawer(ID2D1DeviceContext* dc, float ox, float oy) {
    // 1:1 visual implementation of docs/design/video_wallpaper_beautify.html (Light & Dark Liquid Glass).
    const uint32_t pri = dark_mode_ ? 0xF8FFFFFFu : 0xFF181E28u;
    const uint32_t sec = dark_mode_ ? 0x92FFFFFFu : 0x9E303846u;
    const uint32_t ter = dark_mode_ ? 0x57FFFFFFu : 0x61303846u;
    const uint32_t track_col = dark_mode_ ? 0x14FFFFFFu : 0x0E000000u;
    const uint32_t card_col = dark_mode_ ? 0x0FFFFFFFu : 0x8CFFFFFFu;
    const uint32_t line_col = dark_mode_ ? 0x1AFFFFFFu : 0x14000000u;
    const uint32_t tile_edge = dark_mode_ ? 0x29FFFFFFu : 0x1F000000u;
    // Ensure crisp text/pill contrast in Light mode when accent is a bright sky tint (e.g. #4CC2FF).
    uint32_t accent_ink = accent_argb_;
    if (!dark_mode_) {
        const uint32_t ar = (accent_argb_ >> 16) & 0xFFu;
        const uint32_t ag = (accent_argb_ >> 8) & 0xFFu;
        const uint32_t ab = accent_argb_ & 0xFFu;
        const float luma = (0.299f * ar + 0.587f * ag + 0.114f * ab) / 255.0f;
        if (luma > 0.52f) {
            const float f = 0.54f;
            accent_ink = 0xFF000000u |
                         (static_cast<uint32_t>(std::lround(ar * f)) << 16) |
                         (static_cast<uint32_t>(std::lround(ag * f)) << 8) |
                         static_cast<uint32_t>(std::lround(ab * f));
        }
    }
    const float w = static_cast<float>(card_w_);
    const float h = static_cast<float>(card_h_);

    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);

    auto draw_bitmap_rounded = [&](ID2D1Bitmap* bmp, const D2D1_RECT_F& dst, float rad,
                                   video::VideoFillMode fill_mode = video::VideoFillMode::Fill) {
        if (!bmp) return;
        const D2D1_SIZE_F bsz = bmp->GetSize();
        if (bsz.width < 1.0f || bsz.height < 1.0f) return;
        const float dw = dst.right - dst.left;
        const float dh = dst.bottom - dst.top;
        if (dw < 1.0f || dh < 1.0f) return;

        if (fill_mode == video::VideoFillMode::Fit) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(dst, rad, rad), Brush(dc, 0xFF0B0D12u));
        }

        const float sx = dw / bsz.width;
        const float sy = dh / bsz.height;
        const float sc = (fill_mode == video::VideoFillMode::Fill) ? std::max(sx, sy) : std::min(sx, sy);
        const float rw = bsz.width * sc;
        const float rh = bsz.height * sc;
        const float tx = dst.left + (dw - rw) * 0.5f;
        const float ty = dst.top + (dh - rh) * 0.5f;

        if (fill_mode == video::VideoFillMode::Fill) {
            ComPtr<ID2D1BitmapBrush> br;
            const D2D1_BITMAP_BRUSH_PROPERTIES bbp = D2D1::BitmapBrushProperties(
                D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            const D2D1_BRUSH_PROPERTIES bp = D2D1::BrushProperties(
                1.0f, D2D1::Matrix3x2F::Scale(sc, sc) * D2D1::Matrix3x2F::Translation(tx, ty));
            if (SUCCEEDED(dc->CreateBitmapBrush(bmp, bbp, bp, &br)) && br) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(dst, rad, rad), br.Get());
            }
        } else {
            ComPtr<ID2D1RoundedRectangleGeometry> geo;
            if (fac && SUCCEEDED(fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(dst, rad, rad), &geo)) && geo) {
                dc->PushLayer(D2D1::LayerParameters1(dst, geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
                dc->DrawBitmap(bmp, D2D1::RectF(tx, ty, tx + rw, ty + rh), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                dc->PopLayer();
            } else {
                dc->DrawBitmap(bmp, D2D1::RectF(tx, ty, tx + rw, ty + rh), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        }
    };

    auto draw_preset_rounded = [&](const D2D1_RECT_F& dst, float rad, model::WallpaperPreset preset) {
        const float sw_w = std::max(8.0f, dst.right - dst.left);
        const float sw_h = std::max(8.0f, dst.bottom - dst.top);
        ComPtr<ID2D1BitmapRenderTarget> off_rt;
        if (SUCCEEDED(dc->CreateCompatibleRenderTarget(D2D1::SizeF(sw_w, sw_h), &off_rt)) && off_rt) {
            off_rt->BeginDraw();
            if (preset == model::WallpaperPreset::System) {
                // Stylized deep blue fallback when system wallpaper file isn't readable.
                render::WallpaperBackdrop::DrawPresetScene(
                    fac.Get(), off_rt.Get(), D2D1::RectF(0.0f, 0.0f, sw_w, sw_h), model::WallpaperPreset::Bloom, true);
            } else {
                render::WallpaperBackdrop::DrawPresetScene(
                    fac.Get(), off_rt.Get(), D2D1::RectF(0.0f, 0.0f, sw_w, sw_h), preset, dark_mode_);
            }
            if (SUCCEEDED(off_rt->EndDraw())) {
                ComPtr<ID2D1Bitmap> sw_bmp;
                if (SUCCEEDED(off_rt->GetBitmap(&sw_bmp)) && sw_bmp) {
                    ComPtr<ID2D1BitmapBrush> br;
                    const D2D1_BITMAP_BRUSH_PROPERTIES bbp = D2D1::BitmapBrushProperties(
                        D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    const D2D1_BRUSH_PROPERTIES bp = D2D1::BrushProperties(
                        1.0f, D2D1::Matrix3x2F::Translation(dst.left, dst.top));
                    if (SUCCEEDED(dc->CreateBitmapBrush(sw_bmp.Get(), bbp, bp, &br)) && br) {
                        dc->FillRoundedRectangle(D2D1::RoundedRect(dst, rad, rad), br.Get());
                    }
                }
            }
        }
    };

    auto draw_wrapped_text = [&](const std::wstring& txt, float x, float top_y, float max_w,
                                 float font_px, DWRITE_FONT_WEIGHT weight, uint32_t col, float line_h) -> float {
        if (txt.empty() || !dwrite_) return 0.0f;
        ComPtr<IDWriteTextFormat> fmt;
        const wchar_t* fam = L"Segoe UI Variable Text";
        if (FAILED(dwrite_->CreateTextFormat(fam, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, font_px, L"zh-CN", &fmt))) {
            if (FAILED(dwrite_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, font_px, L"zh-CN", &fmt))) {
                return line_h;
            }
        }
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line_h, line_h * 0.80f);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwrite_->CreateTextLayout(txt.c_str(), static_cast<UINT32>(txt.size()), fmt.Get(),
                                             max_w, 260.0f, &layout)) || !layout) {
            return line_h;
        }
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        dc->DrawTextLayout(D2D1::Point2F(ox + x, oy + top_y), layout.Get(), Brush(dc, col));
        return std::max(line_h, std::ceil(tm.height));
    };

    auto measure_wrapped_h = [&](const std::wstring& txt, float max_w, float font_px, float line_h) -> float {
        if (txt.empty() || !dwrite_) return 0.0f;
        ComPtr<IDWriteTextFormat> fmt;
        if (FAILED(dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             font_px, L"zh-CN", &fmt))) {
            if (FAILED(dwrite_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                 font_px, L"zh-CN", &fmt))) {
                return line_h;
            }
        }
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line_h, line_h * 0.80f);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwrite_->CreateTextLayout(txt.c_str(), static_cast<UINT32>(txt.size()), fmt.Get(),
                                             max_w, 260.0f, &layout)) || !layout) {
            return line_h;
        }
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        return std::max(line_h, std::ceil(tm.height));
    };

    auto draw_centered_single_line = [&](const std::wstring& txt, const D2D1_RECT_F& rc,
                                         float font_px, DWRITE_FONT_WEIGHT weight,
                                         uint32_t color_argb, float alpha = 1.0f) {
        if (txt.empty() || !dwrite_) return;
        ComPtr<IDWriteTextFormat> fmt;
        if (FAILED(dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             font_px, L"zh-CN", &fmt))) {
            if (FAILED(dwrite_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight,
                                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                 font_px, L"zh-CN", &fmt))) {
                return;
            }
        }
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteInlineObject> ellipsis;
        dwrite_->CreateEllipsisTrimmingSign(fmt.Get(), &ellipsis);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        fmt->SetTrimming(&trim, ellipsis.Get());
        dc->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt.Get(), rc,
                     Brush(dc, color_argb, alpha), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    // ---- Fixed Header (height 52 DIP) ----
    const wchar_t pal_g[2] = {L'\xE790', 0};
    DrawGlyph(dc, pal_g, 1, fmt_fluent_15_.Get(),
              D2D1::RectF(ox + 16.0f, oy + 14.0f, ox + 38.0f, oy + 38.0f), Brush(dc, accent_argb_));
    dc->DrawText(L"美化桌面", 4, fmt_title_.Get(),
                 D2D1::RectF(ox + 42.0f, oy + 12.0f, ox + w - 52.0f, oy + 40.0f), Brush(dc, pri));

    const D2D1_RECT_F cls_rc = D2D1::RectF(ox + w - 42.0f, oy + 12.0f, ox + w - 14.0f, oy + 40.0f);
    click_regions_.push_back({"close-overlay", RECT{static_cast<LONG>(w - 42), 12, static_cast<LONG>(w - 14), 40}});
    if (hovered_action_id_ == "close-overlay") {
        dc->FillRoundedRectangle(D2D1::RoundedRect(cls_rc, 14.0f, 14.0f), Brush(dc, track_col));
    }
    const wchar_t x_g[2] = {L'\xE711', 0};
    DrawGlyph(dc, x_g, 1, fmt_fluent_12_.Get(), cls_rc, Brush(dc, sec));

    // ---- Fixed 4-Tab Capsule Segmented Bar (壁纸 · 效果 · 桌面 · 程序坞) ----
    const D2D1_RECT_F tab_track = SnapRectF(D2D1::RectF(ox + 18.0f, oy + 52.0f, ox + w - 18.0f, oy + 84.0f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(tab_track, 16.0f, 16.0f), Brush(dc, track_col));
    static constexpr const wchar_t* kTabLabels[4] = {L"壁纸", L"效果", L"桌面", L"程序坞"};
    const float tab_inner_w = (w - 36.0f - 6.0f) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        const float tx = 21.0f + tab_inner_w * static_cast<float>(i);
        const D2D1_RECT_F trc = D2D1::RectF(ox + tx, oy + 55.0f, ox + tx + tab_inner_w, oy + 81.0f);
        const std::string tid = "b-tab:" + std::to_string(i);
        click_regions_.push_back({tid, RECT{static_cast<LONG>(tx), 52, static_cast<LONG>(tx + tab_inner_w), 84}});
        const bool active = (static_cast<int>(beautify_tab_) == i);
        if (active) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(trc, 13.0f, 13.0f),
                                     Brush(dc, dark_mode_ ? 0x2EFFFFFFu : 0xFFFFFFFFu));
            if (!dark_mode_) {
                StrokeCrispRoundedRect(dc, trc, 13.0f, Brush(dc, 0x10000000u), 0.75f);
            }
        }
        dc->DrawText(kTabLabels[i], static_cast<UINT32>(std::wcslen(kTabLabels[i])),
                     fmt_small_center_.Get(), trc, Brush(dc, pri));
    }

    beautify_sliders_.clear();
    const float body_top = 94.0f;
    const float body_bot = std::max(body_top + 40.0f, h - 10.0f);
    const LONG clip_top = static_cast<LONG>(body_top);
    const LONG clip_bot = static_cast<LONG>(body_bot);
    auto push_region = [&](const std::string& id, RECT r) {
        r.top = std::max(r.top, clip_top);
        r.bottom = std::min(r.bottom, clip_bot);
        if (r.bottom > r.top) {
            click_regions_.push_back({id, r});
        }
    };

    dc->PushAxisAlignedClip(D2D1::RectF(ox + 1.0f, oy + body_top, ox + w - 1.0f, oy + body_bot),
                            D2D1_ANTIALIAS_MODE_ALIASED);

    float y = body_top + 2.0f - beautify_scroll_y_;
    const float content_w = w - 36.0f; // 336 DIP

    // Shared capsule helpers matching HTML CSS (.cseg, .sw, .cbtn)
    auto draw_toggle_row = [&](const wchar_t* label, bool on, const char* act_id, float top_pad = 0.0f) {
        y += top_pad;
        dc->DrawText(label, static_cast<UINT32>(std::wcslen(label)), fmt_body_.Get(),
                     D2D1::RectF(ox + 18.0f, oy + y, ox + w - 70.0f, oy + y + 34.0f), Brush(dc, pri));
        const D2D1_RECT_F sw = D2D1::RectF(ox + w - 56.0f, oy + y + 6.0f, ox + w - 18.0f, oy + y + 28.0f);
        push_region(act_id, RECT{18, static_cast<LONG>(y), static_cast<LONG>(w - 18), static_cast<LONG>(y + 34)});
        dc->FillRoundedRectangle(D2D1::RoundedRect(sw, 11.0f, 11.0f),
                                 Brush(dc, on ? accent_argb_ : track_col));
        const float kx = on ? (sw.right - 11.0f) : (sw.left + 11.0f);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, (sw.top + sw.bottom) * 0.5f), 8.0f, 8.0f),
                        Brush(dc, 0xFFFFFFFFu));
        y += 38.0f;
    };

    auto draw_capsule_seg_at = [&](float sx0, float sy0, float seg_total_w,
                                   std::initializer_list<std::pair<const char*, const wchar_t*>> opts,
                                   int active_idx) {
        const D2D1_RECT_F tr = SnapRectF(D2D1::RectF(ox + sx0, oy + sy0, ox + sx0 + seg_total_w, oy + sy0 + 32.0f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 16.0f, 16.0f), Brush(dc, track_col));
        const float inner_w = (seg_total_w - 6.0f) / static_cast<float>(opts.size());
        int idx = 0;
        for (const auto& [id, lbl] : opts) {
            const float bx = sx0 + 3.0f + inner_w * static_cast<float>(idx);
            const D2D1_RECT_F rc = D2D1::RectF(ox + bx, oy + sy0 + 3.0f, ox + bx + inner_w, oy + sy0 + 29.0f);
            push_region(id, RECT{static_cast<LONG>(bx), static_cast<LONG>(sy0),
                                 static_cast<LONG>(bx + inner_w), static_cast<LONG>(sy0 + 32.0f)});
            if (idx == active_idx) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 13.0f, 13.0f), Brush(dc, accent_ink, dark_mode_ ? 0.22f : 0.14f));
                StrokeCrispRoundedRect(dc, rc, 13.0f, Brush(dc, accent_ink, 0.55f), 1.0f);
            }
            dc->DrawText(lbl, static_cast<UINT32>(std::wcslen(lbl)), fmt_small_center_.Get(),
                         rc, Brush(dc, idx == active_idx ? accent_ink : pri));
            ++idx;
        }
    };

    auto draw_seg = [&](const wchar_t* caption,
                        std::initializer_list<std::pair<const char*, const wchar_t*>> opts,
                        int active_idx) {
        dc->DrawText(caption, static_cast<UINT32>(std::wcslen(caption)), fmt_small_.Get(),
                     D2D1::RectF(ox + 18.0f, oy + y, ox + w - 18.0f, oy + y + 18.0f), Brush(dc, sec));
        y += 22.0f;
        draw_capsule_seg_at(18.0f, y, content_w, opts, active_idx);
        y += 42.0f;
    };

    auto draw_capsule_btn = [&](float bx, float by, const wchar_t* label, const char* act_id, bool primary) -> float {
        const float tw = MeasureTextW(label, fmt_small_.Get(), 240.0f);
        const float bw = std::max(60.0f, std::ceil(tw) + 28.0f);
        const D2D1_RECT_F rc = SnapRectF(D2D1::RectF(ox + bx, oy + by, ox + bx + bw, oy + by + 28.0f));
        push_region(act_id, RECT{static_cast<LONG>(bx), static_cast<LONG>(by),
                                 static_cast<LONG>(bx + bw), static_cast<LONG>(by + 28.0f)});
        const bool hot = (hovered_action_id_ == act_id);
        if (primary) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 14.0f, 14.0f),
                                     Brush(dc, accent_argb_, hot ? 0.90f : 1.0f));
        } else {
            dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 14.0f, 14.0f),
                                     Brush(dc, hot ? (dark_mode_ ? 0x26FFFFFFu : 0x1A000000u) : track_col));
        }
        dc->DrawText(label, static_cast<UINT32>(std::wcslen(label)), fmt_small_center_.Get(),
                     rc, Brush(dc, primary ? 0xFFFFFFFFu : pri));
        return bw;
    };

    if (beautify_tab_ == BeautifyTab::Wall) {
        const auto& vs = beautify_video_;
        const bool vid_active = !vs.active_source_path.empty() &&
                                vs.phase != video::VideoWallpaperPhase::Inactive &&
                                vs.phase != video::VideoWallpaperPhase::RequiresIconTakeover &&
                                vs.icon_takeover_active;
        const bool is_optimizing = vid_active && (vs.phase == video::VideoWallpaperPhase::Optimizing);
        const bool is_poster_only = is_optimizing && !vs.playing_original_while_transcoding;
        const bool is_auto_paused = vid_active && (vs.phase == video::VideoWallpaperPhase::PausedAuto);
        const bool is_missing = vid_active && (vs.phase == video::VideoWallpaperPhase::FileMissing);
        const bool show_pause_btn = vid_active && !beautify_drop_hover_ && !is_poster_only &&
                                    !is_auto_paused && !is_missing &&
                                    vs.phase != video::VideoWallpaperPhase::ConfirmHeavyTranscode;

        // 1. 16:9 Live/Poster Preview (336 x 189, radius 14)
        const float pv_h = 189.0f;
        const D2D1_RECT_F pv_rc = SnapRectF(D2D1::RectF(ox + 18.0f, oy + y, ox + 18.0f + content_w, oy + y + pv_h));
        if (vid_active && !is_missing) {
            ID2D1Bitmap* pv_bmp = nullptr;
            if (!is_poster_only && vs.phase != video::VideoWallpaperPhase::ConfirmHeavyTranscode) {
                // Zero-copy live GPU video preview straight from LiveWallpaperSource (shared ID2D1Device).
                if (live_wallpaper_ && live_wallpaper_->IsInternalVideoMode() &&
                    live_wallpaper_->HasFrame() && live_wallpaper_->SharpBitmap() && live_wallpaper_owner_dev_) {
                    ComPtr<ID2D1Device> cur_dev;
                    dc->GetDevice(&cur_dev);
                    if (cur_dev && cur_dev.Get() == live_wallpaper_owner_dev_) {
                        pv_bmp = live_wallpaper_->SharpBitmap();
                    }
                }
                if (!pv_bmp && !beautify_live_bgra_.empty() && beautify_live_w_ > 0 && beautify_live_h_ > 0) {
                    if (beautify_bmp_dc_ != dc) {
                        beautify_img_cache_.clear();
                        beautify_live_bmp_.Reset();
                        beautify_bmp_dc_ = dc;
                    }
                    if (!beautify_live_bmp_) {
                        const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
                        dc->CreateBitmap(D2D1::SizeU(beautify_live_w_, beautify_live_h_),
                                         beautify_live_bgra_.data(), beautify_live_w_ * 4u, bp, &beautify_live_bmp_);
                    }
                    pv_bmp = beautify_live_bmp_.Get();
                }
            }
            if (!pv_bmp && !vs.active_poster_path.empty()) {
                pv_bmp = GetBeautifyCachedImage(dc, vs.active_poster_path);
            }
            if (pv_bmp) {
                draw_bitmap_rounded(pv_bmp, pv_rc, 14.0f, vs.fill_mode);
            } else {
                draw_preset_rounded(pv_rc, 14.0f, model::WallpaperPreset::Dusk);
            }
        } else if (beautify_settings_.wallpaper_preset == model::WallpaperPreset::System) {
            if (ID2D1Bitmap* sys_bmp = GetBeautifySystemWallpaper(dc)) {
                draw_bitmap_rounded(sys_bmp, pv_rc, 14.0f, video::VideoFillMode::Fill);
            } else {
                draw_preset_rounded(pv_rc, 14.0f, model::WallpaperPreset::System);
            }
        } else {
            draw_preset_rounded(pv_rc, 14.0f, beautify_settings_.wallpaper_preset);
        }

        // User-paused subtle veil / auto-paused / missing veil
        if (vid_active && !beautify_drop_hover_) {
            if (vs.user_paused && show_pause_btn) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(pv_rc, 14.0f, 14.0f), Brush(dc, 0x2E000000u));
            } else if (is_auto_paused) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(pv_rc, 14.0f, 14.0f), Brush(dc, 0x52000000u));
                const wchar_t pg[2] = {L'\xE769', 0};
                DrawGlyph(dc, pg, 1, fmt_fluent_20_.Get(),
                          D2D1::RectF(pv_rc.left, pv_rc.top + 66.0f, pv_rc.right, pv_rc.top + 96.0f), Brush(dc, 0xFFFFFFFFu));
                dc->DrawText(L"已暂停", 3, fmt_small_center_.Get(),
                             D2D1::RectF(pv_rc.left, pv_rc.top + 98.0f, pv_rc.right, pv_rc.top + 118.0f), Brush(dc, 0xFFFFFFFFu));
            } else if (is_missing) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(pv_rc, 14.0f, 14.0f), Brush(dc, 0x70000000u));
                dc->DrawText(L"找不到视频", 5, fmt_small_center_.Get(), pv_rc, Brush(dc, 0xFFFFFFFFu));
            }

            // Top-left chip ("视频" / "第一帧")
            const wchar_t* chip_txt = (is_poster_only || vs.phase == video::VideoWallpaperPhase::ConfirmHeavyTranscode) ? L"第一帧" : L"视频";
            const float chip_w = (chip_txt[0] == L'第') ? 62.0f : 50.0f;
            const D2D1_RECT_F chip_rc = D2D1::RectF(pv_rc.left + 8.0f, pv_rc.top + 8.0f,
                                                    pv_rc.left + 8.0f + chip_w, pv_rc.top + 30.0f);
            dc->FillRoundedRectangle(D2D1::RoundedRect(chip_rc, 11.0f, 11.0f), Brush(dc, 0x8C000000u));
            const wchar_t play_g[2] = {L'\xE768', 0};
            DrawGlyph(dc, play_g, 1, fmt_fluent_12_.Get(),
                      D2D1::RectF(chip_rc.left + 4.0f, chip_rc.top, chip_rc.left + 20.0f, chip_rc.bottom),
                      Brush(dc, 0xFFFFFFFFu));
            dc->DrawText(chip_txt, static_cast<UINT32>(std::wcslen(chip_txt)), fmt_small_.Get(),
                         D2D1::RectF(chip_rc.left + 21.0f, chip_rc.top, chip_rc.right - 4.0f, chip_rc.bottom),
                         Brush(dc, 0xFFFFFFFFu));

            // Bottom-right pause/play button
            if (show_pause_btn) {
                const float pb_w = 64.0f;
                const D2D1_RECT_F pb_rc = D2D1::RectF(pv_rc.right - 8.0f - pb_w, pv_rc.bottom - 36.0f,
                                                      pv_rc.right - 8.0f, pv_rc.bottom - 8.0f);
                push_region("b-vid-pause", RECT{
                    static_cast<LONG>(18.0f + content_w - 8.0f - pb_w), static_cast<LONG>(y + pv_h - 36.0f),
                    static_cast<LONG>(18.0f + content_w - 8.0f), static_cast<LONG>(y + pv_h - 8.0f)});
                const bool p_hot = (hovered_action_id_ == "b-vid-pause");
                dc->FillRoundedRectangle(D2D1::RoundedRect(pb_rc, 14.0f, 14.0f),
                                         Brush(dc, p_hot ? 0xA8000000u : 0x80000000u));
                const wchar_t pg[2] = {vs.user_paused ? L'\xE768' : L'\xE769', 0};
                DrawGlyph(dc, pg, 1, fmt_fluent_12_.Get(),
                          D2D1::RectF(pb_rc.left + 6.0f, pb_rc.top, pb_rc.left + 22.0f, pb_rc.bottom),
                          Brush(dc, 0xFFFFFFFFu));
                const wchar_t* plbl = vs.user_paused ? L"播放" : L"暂停";
                dc->DrawText(plbl, 2, fmt_small_.Get(),
                             D2D1::RectF(pb_rc.left + 24.0f, pb_rc.top, pb_rc.right - 6.0f, pb_rc.bottom),
                             Brush(dc, 0xFFFFFFFFu));
            }

            // Progress bar inside preview during optimizing
            if (is_optimizing) {
                const float pct = std::clamp(vs.optimize_progress, 0.02f, 1.0f);
                const D2D1_RECT_F ptrk = D2D1::RectF(pv_rc.left + 12.0f, pv_rc.bottom - 16.0f,
                                                     pv_rc.right - 12.0f, pv_rc.bottom - 12.0f);
                dc->FillRoundedRectangle(D2D1::RoundedRect(ptrk, 2.0f, 2.0f), Brush(dc, 0x59FFFFFFu));
                const D2D1_RECT_F pfill = D2D1::RectF(ptrk.left, ptrk.top,
                                                      ptrk.left + (ptrk.right - ptrk.left) * pct, ptrk.bottom);
                dc->FillRoundedRectangle(D2D1::RoundedRect(pfill, 2.0f, 2.0f), Brush(dc, 0xFFFFFFFFu));
            }
        }

        // Drag-over dashed drop zone overlay ("松开即设为壁纸")
        if (beautify_drop_hover_) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(pv_rc, 14.0f, 14.0f),
                                     Brush(dc, dark_mode_ ? 0xD420222Au : 0xDEFAF9F7u));
            dc->FillRoundedRectangle(D2D1::RoundedRect(pv_rc, 14.0f, 14.0f), Brush(dc, accent_argb_, 0.12f));
            ComPtr<ID2D1StrokeStyle> dash_st;
            if (fac) {
                const D2D1_STROKE_STYLE_PROPERTIES dp = D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                    D2D1_LINE_JOIN_ROUND, 4.0f, D2D1_DASH_STYLE_DASH, 0.0f);
                fac->CreateStrokeStyle(dp, nullptr, 0, &dash_st);
            }
            const D2D1_RECT_F dr = D2D1::RectF(pv_rc.left + 1.0f, pv_rc.top + 1.0f, pv_rc.right - 1.0f, pv_rc.bottom - 1.0f);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(dr, 13.0f, 13.0f), Brush(dc, accent_argb_), 2.0f, dash_st.Get());
            const wchar_t film_g[2] = {L'\xE714', 0};
            DrawGlyph(dc, film_g, 1, fmt_fluent_20_.Get(),
                      D2D1::RectF(pv_rc.left, pv_rc.top + 52.0f, pv_rc.right, pv_rc.top + 84.0f), Brush(dc, accent_ink));
            draw_centered_single_line(L"松开即设为壁纸",
                                      D2D1::RectF(pv_rc.left + 16.0f, pv_rc.top + 88.0f, pv_rc.right - 16.0f, pv_rc.top + 110.0f),
                                      13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, pri, 1.0f);
            if (!beautify_drop_filename_.empty()) {
                draw_centered_single_line(beautify_drop_filename_,
                                          D2D1::RectF(pv_rc.left + 16.0f, pv_rc.top + 112.0f, pv_rc.right - 16.0f, pv_rc.top + 132.0f),
                                          11.5f, DWRITE_FONT_WEIGHT_NORMAL, sec, 1.0f);
            }
        } else {
            StrokeCrispRoundedRect(dc, pv_rc, 14.0f, Brush(dc, tile_edge), 1.0f);
        }
        y += pv_h + 12.0f;

        // 2. 图片 Row (5 tiles: 系统 / 蓝 / 暮色 / 森林 / 照片, 62x40 radius 9)
        dc->DrawText(L"图片", 2, fmt_small_.Get(),
                     D2D1::RectF(ox + 18.0f, oy + y, ox + w - 18.0f, oy + y + 16.0f), Brush(dc, sec));
        y += 22.0f;

        struct WallOpt { model::WallpaperPreset preset; const char* id; const wchar_t* label; };
        static constexpr WallOpt kWalls[5] = {
            {model::WallpaperPreset::System, "b-wall:0", L"系统"},
            {model::WallpaperPreset::Bloom,  "b-wall:1", L"蓝"},
            {model::WallpaperPreset::Dusk,   "b-wall:2", L"暮色"},
            {model::WallpaperPreset::Forest, "b-wall:3", L"森林"},
            {model::WallpaperPreset::Photo,  "b-wall:4", L"照片"},
        };
        const float th_w = 62.0f;
        const float th_h = 40.0f;
        const float th_gap = (content_w - th_w * 5.0f) / 4.0f;
        for (int i = 0; i < 5; ++i) {
            const float wx = 18.0f + static_cast<float>(i) * (th_w + th_gap);
            const D2D1_RECT_F wrc = SnapRectF(D2D1::RectF(ox + wx, oy + y, ox + wx + th_w, oy + y + th_h));
            push_region(kWalls[i].id, RECT{static_cast<LONG>(wx), static_cast<LONG>(y),
                                           static_cast<LONG>(wx + th_w), static_cast<LONG>(y + th_h + 20.0f)});
            if (kWalls[i].preset == model::WallpaperPreset::System) {
                if (ID2D1Bitmap* sys_bmp = GetBeautifySystemWallpaper(dc)) {
                    draw_bitmap_rounded(sys_bmp, wrc, 9.0f, video::VideoFillMode::Fill);
                } else {
                    draw_preset_rounded(wrc, 9.0f, model::WallpaperPreset::System);
                }
            } else {
                draw_preset_rounded(wrc, 9.0f, kWalls[i].preset);
            }
            const bool active = !vid_active && (beautify_settings_.wallpaper_preset == kWalls[i].preset);
            StrokeCrispRoundedRect(dc, wrc, 9.0f,
                                   Brush(dc, active ? accent_ink : tile_edge),
                                   active ? 2.0f : 1.0f);
            dc->DrawText(kWalls[i].label, static_cast<UINT32>(std::wcslen(kWalls[i].label)),
                         fmt_small_center_.Get(),
                         D2D1::RectF(wrc.left - 2.0f, wrc.bottom + 4.0f, wrc.right + 2.0f, wrc.bottom + 20.0f),
                         Brush(dc, active ? accent_ink : sec));
        }
        y += th_h + 28.0f;

        // 3. 视频 Row (4-column grid: up to 3 recent poster thumbnails + "+ 选择视频…", 78x44 radius 9)
        dc->DrawText(L"视频", 2, fmt_small_.Get(),
                     D2D1::RectF(ox + 18.0f, oy + y, ox + 120.0f, oy + y + 16.0f), Brush(dc, sec));
        if (!vs.recent.empty()) {
            dc->DrawText(L"最近用过", 4, fmt_small_right_.Get(),
                         D2D1::RectF(ox + 120.0f, oy + y, ox + w - 18.0f, oy + y + 16.0f), Brush(dc, sec));
        }
        y += 22.0f;

        const float vid_alpha = vs.icon_takeover_active ? 1.0f : 0.40f;
        const size_t show_recent = std::min<size_t>(vs.recent.size(), 3u);
        const std::wstring active_norm = video::NormalizePathForCompare(vs.active_source_path);
        const std::wstring tried_norm = video::NormalizePathForCompare(vs.tried_source_path);
        const float v_gap = 8.0f;
        const float v_w = (content_w - v_gap * 3.0f) / 4.0f; // 78 DIP (4-col 16:9 tiles)
        const float v_h = 44.0f;

        for (size_t i = 0; i < show_recent; ++i) {
            const auto& r = vs.recent[i];
            const float vx = 18.0f + static_cast<float>(i) * (v_w + v_gap);
            const D2D1_RECT_F vrc = SnapRectF(D2D1::RectF(ox + vx, oy + y, ox + vx + v_w, oy + y + v_h));
            const std::string rid = "b-vid-recent:" + std::to_string(i);
            if (vs.icon_takeover_active) {
                push_region(rid, RECT{static_cast<LONG>(vx), static_cast<LONG>(y),
                                      static_cast<LONG>(vx + v_w), static_cast<LONG>(y + v_h + 20.0f)});
            }

            if (ID2D1Bitmap* pb = GetBeautifyCachedImage(dc, r.poster_path)) {
                draw_bitmap_rounded(pb, vrc, 9.0f, video::VideoFillMode::Fill);
            } else {
                draw_preset_rounded(vrc, 9.0f, model::WallpaperPreset::Dusk);
            }
            if (vid_alpha < 0.99f) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(vrc, 9.0f, 9.0f),
                                         Brush(dc, dark_mode_ ? 0x80181A20u : 0x80FFFFFFu));
            }

            // Duration badge (bottom-right)
            const std::wstring dur = video::FormatDurationMmSs(r.duration_ms);
            if (!dur.empty()) {
                const D2D1_RECT_F drc = D2D1::RectF(vrc.right - 31.0f, vrc.bottom - 15.0f, vrc.right - 3.0f, vrc.bottom - 3.0f);
                dc->FillRoundedRectangle(D2D1::RoundedRect(drc, 4.0f, 4.0f), Brush(dc, 0x8C000000u, vid_alpha));
                dc->DrawText(dur.c_str(), static_cast<UINT32>(dur.size()), fmt_small_center_.Get(),
                             D2D1::RectF(drc.left - 2.0f, drc.top - 1.0f, drc.right + 2.0f, drc.bottom + 1.0f),
                             Brush(dc, 0xFFFFFFFFu, vid_alpha));
            }

            const std::wstring rnorm = video::NormalizePathForCompare(r.path);
            const bool is_cur = vid_active && !active_norm.empty() && rnorm == active_norm;
            const bool is_warn = !tried_norm.empty() && rnorm == tried_norm;
            if (is_warn) {
                const D2D1_POINT_2F wc{vrc.right - 10.0f, vrc.top + 10.0f};
                dc->FillEllipse(D2D1::Ellipse(wc, 7.0f, 7.0f), Brush(dc, 0xFFFFB020u, vid_alpha));
                dc->DrawText(L"!", 1, fmt_small_center_.Get(),
                             D2D1::RectF(wc.x - 7.0f, wc.y - 7.0f, wc.x + 7.0f, wc.y + 7.0f),
                             Brush(dc, 0xFF000000u, vid_alpha));
            }

            StrokeCrispRoundedRect(dc, vrc, 9.0f,
                                   Brush(dc, is_cur ? accent_ink : tile_edge, vid_alpha),
                                   is_cur ? 2.0f : 1.0f);
            const std::wstring stem = video::VideoStemName(r.path);
            draw_centered_single_line(stem,
                                      D2D1::RectF(vrc.left, vrc.bottom + 4.0f, vrc.right, vrc.bottom + 20.0f),
                                      10.8f, is_cur ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                      is_cur ? accent_ink : sec, vid_alpha);
        }

        // "+" Add Video Tile ("选择视频…")
        {
            const float ax = 18.0f + static_cast<float>(show_recent) * (v_w + v_gap);
            const D2D1_RECT_F arc = SnapRectF(D2D1::RectF(ox + ax, oy + y, ox + ax + v_w, oy + y + v_h));
            if (vs.icon_takeover_active) {
                push_region("b-vid-pick", RECT{static_cast<LONG>(ax), static_cast<LONG>(y),
                                               static_cast<LONG>(ax + v_w), static_cast<LONG>(y + v_h + 20.0f)});
            }
            if (hovered_action_id_ == "b-vid-pick" && vs.icon_takeover_active) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(arc, 9.0f, 9.0f), Brush(dc, track_col));
            }
            ComPtr<ID2D1StrokeStyle> dash_st;
            if (fac) {
                const D2D1_STROKE_STYLE_PROPERTIES dp = D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                    D2D1_LINE_JOIN_ROUND, 4.0f, D2D1_DASH_STYLE_DASH, 0.0f);
                fac->CreateStrokeStyle(dp, nullptr, 0, &dash_st);
            }
            dc->DrawRoundedRectangle(D2D1::RoundedRect(arc, 9.0f, 9.0f),
                                     Brush(dc, tile_edge, vid_alpha), 1.4f, dash_st.Get());
            const wchar_t plus_g[2] = {L'\xE710', 0};
            DrawGlyph(dc, plus_g, 1, fmt_fluent_15_.Get(), arc, Brush(dc, sec, vid_alpha));
            draw_centered_single_line(L"选择视频…",
                                      D2D1::RectF(arc.left, arc.bottom + 4.0f, arc.right, arc.bottom + 20.0f),
                                      10.8f, DWRITE_FONT_WEIGHT_NORMAL, sec, vid_alpha);
        }
        y += v_h + 28.0f;

        // 4. Status / Info Card (.info) or First-Time Hint
        const bool has_tried_error = !vs.tried_display_name.empty() &&
            (vs.phase == video::VideoWallpaperPhase::MissingCodecHevc ||
             vs.phase == video::VideoWallpaperPhase::MissingCodecAv1 ||
             vs.phase == video::VideoWallpaperPhase::UnsupportedCodec ||
             vs.phase == video::VideoWallpaperPhase::TooLargeToTranscode ||
             vs.phase == video::VideoWallpaperPhase::UnsupportedHdr ||
             vs.phase == video::VideoWallpaperPhase::Error);
        const bool show_info_card = !vs.icon_takeover_active || vid_active || has_tried_error ||
                                    vs.phase == video::VideoWallpaperPhase::ConfirmHeavyTranscode;

        if (show_info_card) {
            float card_inner_h = 20.0f;
            std::wstring title_name;
            std::wstring title_dur;
            uint32_t dot_col = 0xFF34C759u;
            std::wstring st_line;
            std::wstring sub_line;
            bool show_fill_row = false;
            std::vector<std::tuple<std::wstring, std::string, bool>> actions;

            if (!vs.icon_takeover_active) {
                dot_col = 0xFFFFB020u;
                st_line = L"视频壁纸需要由桌面整理显示图标";
                sub_line = L"现在桌面图标由 Windows 显示，视频会盖住它们。";
                actions.push_back({L"开启", "b-vid-takeover", true});
            } else if (has_tried_error) {
                title_name = vs.tried_display_name;
                title_dur = vs.tried_duration_ms > 0 ? video::FormatDurationMmSs(vs.tried_duration_ms) : L"";
                dot_col = 0xFFFF3B30u;
                st_line = vs.status_text;
                sub_line = vs.sub_text;
                if (!vs.store_uri.empty()) {
                    actions.push_back({L"去 Microsoft Store", "b-vid-store", true});
                }
            } else if (vs.phase == video::VideoWallpaperPhase::ConfirmHeavyTranscode) {
                title_name = !vs.tried_display_name.empty() ? vs.tried_display_name : vs.active_display_name;
                const uint32_t dm = vs.tried_duration_ms > 0 ? vs.tried_duration_ms : vs.duration_ms;
                title_dur = dm > 0 ? video::FormatDurationMmSs(dm) : L"";
                dot_col = 0xFFFFB020u;
                st_line = vs.status_text;
                sub_line = vs.sub_text;
                actions.push_back({L"开始优化", "b-vid-confirm-heavy", true});
                actions.push_back({L"取消", "b-vid-cancel", false});
            } else {
                title_name = vs.active_display_name;
                title_dur = vs.duration_ms > 0 ? video::FormatDurationMmSs(vs.duration_ms) : L"";
                st_line = vs.status_text;
                sub_line = vs.sub_text;
                if (vs.phase == video::VideoWallpaperPhase::Optimizing) {
                    dot_col = accent_argb_;
                    actions.push_back({L"取消", "b-vid-cancel", false});
                } else if (vs.phase == video::VideoWallpaperPhase::PausedAuto ||
                           vs.phase == video::VideoWallpaperPhase::PausedManual) {
                    dot_col = 0xFFFFB020u;
                } else if (vs.phase == video::VideoWallpaperPhase::FileMissing ||
                           vs.phase == video::VideoWallpaperPhase::Error) {
                    dot_col = 0xFFFF3B30u;
                    if (vs.phase == video::VideoWallpaperPhase::FileMissing) {
                        actions.push_back({L"重新选择…", "b-vid-pick", true});
                    }
                } else {
                    dot_col = 0xFF34C759u;
                }
                show_fill_row = (vs.phase != video::VideoWallpaperPhase::FileMissing &&
                                 vs.phase != video::VideoWallpaperPhase::Error);
            }

            const float inner_w = content_w - 24.0f;
            if (!title_name.empty()) card_inner_h += 22.0f;
            const float st_h = st_line.empty() ? 0.0f : measure_wrapped_h(st_line, inner_w - 14.0f, 11.5f, 17.0f);
            if (st_h > 0.0f) card_inner_h += st_h + (title_name.empty() ? 0.0f : 4.0f);
            const float sub_h = sub_line.empty() ? 0.0f : measure_wrapped_h(sub_line, inner_w, 11.0f, 16.0f);
            if (sub_h > 0.0f) card_inner_h += sub_h + 4.0f;
            if (!actions.empty()) card_inner_h += 36.0f;
            if (show_fill_row) card_inner_h += 42.0f;

            const D2D1_RECT_F card_rc = SnapRectF(D2D1::RectF(ox + 18.0f, oy + y, ox + 18.0f + content_w, oy + y + card_inner_h));
            dc->FillRoundedRectangle(D2D1::RoundedRect(card_rc, 12.0f, 12.0f), Brush(dc, card_col));
            StrokeCrispRoundedRect(dc, card_rc, 12.0f, Brush(dc, line_col), 1.0f);

            float cy = y + 10.0f;
            if (!title_name.empty()) {
                dc->DrawText(title_name.c_str(), static_cast<UINT32>(title_name.size()), fmt_body_semi_.Get(),
                             D2D1::RectF(ox + 30.0f, oy + cy, ox + w - 74.0f, oy + cy + 18.0f), Brush(dc, pri));
                if (!title_dur.empty()) {
                    dc->DrawText(title_dur.c_str(), static_cast<UINT32>(title_dur.size()), fmt_small_right_.Get(),
                                 D2D1::RectF(ox + w - 74.0f, oy + cy, ox + w - 30.0f, oy + cy + 18.0f), Brush(dc, sec));
                }
                cy += 22.0f;
            }
            if (!st_line.empty()) {
                if (!title_name.empty()) cy += 2.0f;
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ox + 33.5f, oy + cy + 8.0f), 3.5f, 3.5f), Brush(dc, dot_col));
                const float used = draw_wrapped_text(st_line, 43.0f, cy, inner_w - 14.0f, 11.5f,
                                                     DWRITE_FONT_WEIGHT_NORMAL, sec, 17.0f);
                cy += used;
            }
            if (!sub_line.empty()) {
                cy += 4.0f;
                const float used = draw_wrapped_text(sub_line, 30.0f, cy, inner_w, 11.0f,
                                                     DWRITE_FONT_WEIGHT_NORMAL, ter, 16.0f);
                cy += used;
            }
            if (!actions.empty()) {
                cy += 8.0f;
                float bx = 30.0f;
                for (const auto& [lbl, act_id, is_pri] : actions) {
                    const float bw = draw_capsule_btn(bx, cy, lbl.c_str(), act_id.c_str(), is_pri);
                    bx += bw + 8.0f;
                }
                cy += 28.0f;
            }
            if (show_fill_row) {
                cy += 10.0f;
                dc->DrawText(L"填充方式", 4, fmt_body_.Get(),
                             D2D1::RectF(ox + 30.0f, oy + cy, ox + 170.0f, oy + cy + 32.0f), Brush(dc, pri));
                draw_capsule_seg_at(w - 30.0f - 150.0f, cy, 150.0f,
                                    {{"b-vfill:0", L"铺满"}, {"b-vfill:1", L"适应"}},
                                    vs.fill_mode == video::VideoFillMode::Fit ? 1 : 0);
            }
            y += card_inner_h + 10.0f;
        } else if (!vid_active && vs.recent.empty()) {
            const std::wstring hint = L"选一个视频当壁纸：MP4、MOV、MKV 都可以，也可以把视频文件拖到上面的预览里。比屏幕大的视频会先在后台优化，不占用这么多内存。";
            const float used = draw_wrapped_text(hint, 18.0f, y, content_w, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, ter, 17.0f);
            y += used + 10.0f;
        }

        // 5. 强调色跟随壁纸 + 6 swatches
        draw_toggle_row(L"强调色跟随壁纸", beautify_settings_.auto_wallpaper_accent, "b-auto-acc", 2.0f);
        if (!beautify_settings_.auto_wallpaper_accent) {
            static constexpr uint32_t kSwatches[6] = {
                0xFF4CC2FFu, 0xFFE68A6Eu, 0xFF5FD08Au, 0xFFA193FFu, 0xFFF5A524u, 0xFFE5484Du
            };
            for (int i = 0; i < 6; ++i) {
                const float cx = 30.0f + static_cast<float>(i) * 34.0f;
                const float cy = y + 12.0f;
                push_region("b-acc:" + std::to_string(i),
                            RECT{static_cast<LONG>(cx - 14), static_cast<LONG>(cy - 14),
                                 static_cast<LONG>(cx + 14), static_cast<LONG>(cy + 14)});
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ox + cx, oy + cy), 10.5f, 10.5f), Brush(dc, kSwatches[i]));
                if (beautify_settings_.accent_argb == kSwatches[i]) {
                    dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ox + cx, oy + cy), 13.5f, 13.5f), Brush(dc, kSwatches[i]), 1.8f);
                }
            }
            y += 30.0f;
        }
    } else if (beautify_tab_ == BeautifyTab::Fx) {
        y += 6.0f;
        const int mat_idx = (beautify_settings_.default_material == model::CardMaterial::Mica) ? 1
                          : (beautify_settings_.default_material == model::CardMaterial::MicaAlt) ? 2
                          : (beautify_settings_.default_material == model::CardMaterial::Solid) ? 3
                          : (beautify_settings_.default_material == model::CardMaterial::LiquidGlass) ? 4 : 0;
        draw_seg(L"窗口效果",
                 {{"b-mat:1", L"亚克力"}, {"b-mat:2", L"Mica"}, {"b-mat:3", L"Mica Alt"},
                  {"b-mat:4", L"纯色"}, {"b-mat:5", L"液态玻璃"}},
                 mat_idx);

        if (beautify_settings_.default_material == model::CardMaterial::LiquidGlass) {
            draw_seg(L"液态玻璃风格",
                     {{"b-gp:5", L"清透"}, {"b-gp:1", L"标准"}, {"b-gp:3", L"柔雾"}, {"b-gp:2", L"棱镜"}},
                     static_cast<int>(model::GlassPresetToModeIndex(beautify_settings_.glass_preset)));
            draw_seg(L"动态折射",
                     {{"b-rate:0", L"静态"}, {"b-rate:1", L"30Hz"}, {"b-rate:2", L"60Hz"}},
                     std::clamp<int>(beautify_settings_.glass_live_rate, 0, 2));
            const float used = draw_wrapped_text(
                L"视频壁纸或动态壁纸下，玻璃按这个频率刷新折射；静态最省电。",
                18.0f, y, content_w, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, ter, 17.0f);
            y += used + 8.0f;
        }
    } else if (beautify_tab_ == BeautifyTab::Desk) {
        y += 6.0f;
        draw_seg(L"图标文字",
                 {{"b-lbl:0", L"阴影"}, {"b-lbl:1", L"底板"}, {"b-lbl:2", L"隐藏"},
                  {"b-lbl:3", l10n::Get(l10n::StringId::DesktopIconLabelNormal).c_str()}},
                 static_cast<int>(beautify_settings_.label_style));

        draw_seg(L"图标大小",
                 {{"b-sz:0", L"小"}, {"b-sz:1", L"中"}, {"b-sz:2", L"大"}},
                 static_cast<int>(beautify_settings_.icon_size));

        draw_toggle_row(L"双击空白处隐藏图标", beautify_settings_.double_click_hide_icons, "b-dbl-hide", 4.0f);

        y += 6.0f;
        dc->DrawText(L"小组件", 3, fmt_small_.Get(),
                     D2D1::RectF(ox + 18.0f, oy + y, ox + 120.0f, oy + y + 18.0f), Brush(dc, sec));
        dc->DrawText(L"放进磁贴区的空位", 8, fmt_small_right_.Get(),
                     D2D1::RectF(ox + 120.0f, oy + y, ox + w - 18.0f, oy + y + 18.0f), Brush(dc, sec));
        y += 24.0f;

        struct WidgetOpt { model::TileKind kind; const wchar_t* name; const char* id; };
        static constexpr WidgetOpt kWidgets[3] = {
            {model::TileKind::Weather,  L"天气", "wg-open:12"},
            {model::TileKind::Calendar, L"日历", "wg-open:13"},
            {model::TileKind::Todo,     L"待办", "wg-open:14"},
        };
        const float ww = (content_w - 12.0f) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            const float wx = 18.0f + static_cast<float>(i) * (ww + 6.0f);
            const D2D1_RECT_F wrc = SnapRectF(D2D1::RectF(ox + wx, oy + y, ox + wx + ww, oy + y + 58.0f));
            push_region(kWidgets[i].id, RECT{static_cast<LONG>(wx), static_cast<LONG>(y),
                                             static_cast<LONG>(wx + ww), static_cast<LONG>(y + 58.0f)});
            const bool added = active_widget_kinds_.contains(static_cast<uint8_t>(kWidgets[i].kind));
            const bool whot = (hovered_action_id_ == kWidgets[i].id);
            dc->FillRoundedRectangle(D2D1::RoundedRect(wrc, 12.0f, 12.0f),
                                     Brush(dc, whot ? (dark_mode_ ? 0x1EFFFFFFu : 0xB8FFFFFFu) : card_col));
            StrokeCrispRoundedRect(dc, wrc, 12.0f,
                                   Brush(dc, added ? accent_argb_ : line_col),
                                   added ? 1.5f : 1.0f);
            dc->DrawText(kWidgets[i].name, static_cast<UINT32>(std::wcslen(kWidgets[i].name)),
                         fmt_small_center_.Get(),
                         D2D1::RectF(wrc.left, wrc.top + 10.0f, wrc.right, wrc.top + 30.0f),
                         Brush(dc, added ? accent_argb_ : pri));
            const wchar_t* sub_lbl = added ? L"已添加" : L"添加";
            dc->DrawText(sub_lbl, static_cast<UINT32>(std::wcslen(sub_lbl)),
                         fmt_small_center_.Get(),
                         D2D1::RectF(wrc.left, wrc.top + 31.0f, wrc.right, wrc.bottom - 8.0f),
                         Brush(dc, sec));
        }
        y += 68.0f;

        push_region("b-more-widgets", RECT{18, static_cast<LONG>(y), 140, static_cast<LONG>(y + 24.0f)});
        dc->DrawText(L"全部小组件…", 6, fmt_small_.Get(),
                     D2D1::RectF(ox + 18.0f, oy + y, ox + 140.0f, oy + y + 22.0f),
                     Brush(dc, accent_argb_));
        y += 28.0f;
    } else { // BeautifyTab::Dock
        y += 4.0f;
        draw_toggle_row(L"显示液态玻璃程序坞", beautify_settings_.dock_enabled, "b-dock-en");
        if (beautify_settings_.dock_enabled) {
            draw_seg(L"程序坞位置",
                     {{"b-dpos:0", L"底部"}, {"b-dpos:1", L"左侧"}, {"b-dpos:2", L"右侧"}},
                     std::clamp<int>(beautify_settings_.dock_position, 0, 2));
            draw_seg(L"程序坞收回",
                     {{"b-dhide:0", L"常驻"}, {"b-dhide:1", L"智能隐藏"}, {"b-dhide:2", L"自动隐藏"}},
                     std::clamp<int>(beautify_settings_.dock_hide_mode, 0, 2));
            draw_seg(L"窗口收回效果",
                     {{"b-dmin:1", L"神奇"}, {"b-dmin:2", L"缩放"}, {"b-dmin:0", L"关闭"}},
                     beautify_settings_.dock_minimize_effect == 1 ? 0 : (beautify_settings_.dock_minimize_effect == 2 ? 1 : 2));
            draw_toggle_row(L"系统状态与托盘区", beautify_settings_.dock_system_area, "b-dsys", 4.0f);
        }
    }

    const float content_end = y + 14.0f + beautify_scroll_y_;
    beautify_max_scroll_y_ = std::max(0.0f, content_end - body_bot);
    beautify_scroll_y_ = std::clamp(beautify_scroll_y_, 0.0f, beautify_max_scroll_y_);
    dc->PopAxisAlignedClip();

    if (beautify_max_scroll_y_ > 1.0f) {
        const float track_h = std::max(40.0f, body_bot - body_top);
        const float thumb_h = std::max(28.0f, track_h * (track_h / std::max(track_h + beautify_max_scroll_y_, 1.0f)));
        const float thumb_t = beautify_scroll_y_ / beautify_max_scroll_y_;
        const float thumb_y = body_top + thumb_t * (track_h - thumb_h);
        dc->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(ox + w - 7.0f, oy + thumb_y, ox + w - 4.0f, oy + thumb_y + thumb_h),
                              1.5f, 1.5f),
            Brush(dc, dark_mode_ ? 0x38FFFFFFu : 0x30000000u));
    }
}

void DesktopOverlayController::DrawWidgetGalleryDrawer(ID2D1DeviceContext* dc, float ox, float oy) {
    const uint32_t pri = dark_mode_ ? 0xF8FFFFFFu : 0xF8181E28u;
    const uint32_t sec = dark_mode_ ? 0x96FFFFFFu : 0x96303846u;
    const float w = static_cast<float>(card_w_);
    const float h = static_cast<float>(card_h_);

    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);

    ID2D1BitmapBrush* grain_brush = nullptr;
    if (backdrop_ != nullptr) {
        grain_brush = (dc == backdrop_owner_dc_ && backdrop_->NoiseBrush() != nullptr)
                          ? backdrop_->NoiseBrush()
                          : backdrop_->MenuNoiseBrush(dc);
    }

    // 1. Header bar (.dp-h): + 添加小组件  ... [×] + snow-grain band
    {
        dc->PushAxisAlignedClip(D2D1::RectF(ox, oy, ox + w, oy + 44.0f), D2D1_ANTIALIAS_MODE_ALIASED);
        const D2D1_ROUNDED_RECT full_rr = D2D1::RoundedRect(D2D1::RectF(ox, oy, ox + w, oy + h), 14.0f, 14.0f);
        dc->FillRoundedRectangle(full_rr, Brush(dc, dark_mode_ ? 0x14FFFFFFu : 0x60FFFFFFu));
        if (grain_brush != nullptr) {
            grain_brush->SetOpacity(dark_mode_ ? 0.09f : 0.44f);
            dc->FillRoundedRectangle(full_rr, grain_brush);
            grain_brush->SetOpacity(1.0f);
        }
        dc->PopAxisAlignedClip();
        dc->FillRectangle(D2D1::RectF(ox + 1.0f, oy + 44.0f, ox + w - 1.0f, oy + 45.0f),
                          Brush(dc, dark_mode_ ? 0x1AFFFFFFu : 0x12000000u));

        const wchar_t plus_g[2] = {L'\xE710', 0};
        DrawGlyph(dc, plus_g, 1, fmt_fluent_15_.Get(),
                  D2D1::RectF(ox + 16.0f, oy + 8.0f, ox + 36.0f, oy + 38.0f), Brush(dc, accent_argb_));
        dc->DrawText(L"添加小组件", 5, fmt_title_.Get(),
                     D2D1::RectF(ox + 40.0f, oy + 8.0f, ox + w - 48.0f, oy + 38.0f), Brush(dc, pri));

        const D2D1_RECT_F cls_rc = D2D1::RectF(ox + w - 40.0f, oy + 9.0f, ox + w - 12.0f, oy + 37.0f);
        click_regions_.push_back({"close-overlay", RECT{static_cast<LONG>(w - 40), 9, static_cast<LONG>(w - 12), 37}});
        if (hovered_action_id_ == "close-overlay") {
            dc->FillRoundedRectangle(D2D1::RoundedRect(cls_rc, 6.0f, 6.0f),
                                     Brush(dc, dark_mode_ ? 0x1CFFFFFFu : 0x10000000u));
        }
        const wchar_t x_g[2] = {L'\xE711', 0};
        DrawGlyph(dc, x_g, 1, fmt_fluent_12_.Get(), cls_rc, Brush(dc, sec));
    }

    // 2. 4x4 colorful squircle widget grid (.gal-grid)
    const float grid_x = 16.0f;
    const float grid_y = 51.0f;
    const float col_gap = 6.0f;
    const float row_gap = 4.0f;
    const float cell_w = (w - 32.0f - col_gap * 3.0f) / 4.0f;
    const float cell_h = 56.0f;

    for (size_t i = 0; i < std::size(kGalleryWidgets); ++i) {
        const auto& wd = kGalleryWidgets[i];
        const int c = static_cast<int>(i % 4);
        const int r = static_cast<int>(i / 4);
        const float cx0 = grid_x + static_cast<float>(c) * (cell_w + col_gap);
        const float cy0 = grid_y + static_cast<float>(r) * (cell_h + row_gap);
        const D2D1_RECT_F crc = SnapRectF(D2D1::RectF(ox + cx0, oy + cy0, ox + cx0 + cell_w, oy + cy0 + cell_h));

        const std::string act_id = "wg-sel:" + std::to_string(static_cast<int>(wd.kind));
        click_regions_.push_back({act_id, RECT{static_cast<LONG>(cx0), static_cast<LONG>(cy0),
                                               static_cast<LONG>(cx0 + cell_w), static_cast<LONG>(cy0 + cell_h)}});

        const bool sel = (wd.kind == gallery_selected_kind_);
        const bool hot = (hovered_action_id_ == act_id);
        if (sel) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(crc, 10.0f, 10.0f),
                                     Brush(dc, accent_argb_, dark_mode_ ? 0.18f : 0.13f));
            StrokeCrispRoundedRect(dc, crc, 10.0f, Brush(dc, accent_argb_, dark_mode_ ? 0.72f : 0.58f), 1.0f);
        } else if (hot) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(crc, 10.0f, 10.0f),
                                     Brush(dc, dark_mode_ ? 0x16FFFFFFu : 0x0E000000u));
        }

        // 32x32 drawn widget icon: a small scene on a lit squircle (widget_icons.cpp).
        const float bx = (crc.left + crc.right) * 0.5f - 16.0f;
        const float by = crc.top + 4.0f;
        const D2D1_RECT_F brc = D2D1::RectF(bx, by, bx + 32.0f, by + 32.0f);
        DrawWidgetIcon(dc, wd.kind, brc, 8.5f, fmt_small_center_.Get());

        dc->DrawText(wd.name, static_cast<UINT32>(std::wcslen(wd.name)), fmt_small_center_.Get(),
                     D2D1::RectF(crc.left + 2.0f, crc.top + 37.0f, crc.right - 2.0f, crc.bottom - 1.0f),
                     Brush(dc, pri));

        const bool placed = !gallery_tiles_.empty()
                                ? model::BoardHasKind(gallery_tiles_, wd.kind)
                                : active_widget_kinds_.contains(static_cast<uint8_t>(wd.kind));
        if (placed) {
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(crc.right - 9.0f, crc.top + 7.0f), 3.0f, 3.0f),
                            Brush(dc, 0xFF30D158u));
        }
    }

    // Divider (.dp-sep)
    const float sep_y = grid_y + cell_h * 4.0f + row_gap * 3.0f + 6.0f;
    dc->FillRectangle(D2D1::RectF(ox + 16.0f, oy + sep_y, ox + w - 16.0f, oy + sep_y + 1.0f),
                      Brush(dc, dark_mode_ ? 0x16FFFFFFu : 0x10000000u));

    // Layout bottom anchored metrics so preview stage dynamically fills remaining space cleanly
    const float footer_top = h - 44.0f;
    const float size_row_top = footer_top - 34.0f;
    const float desc_top = size_row_top - 42.0f;
    const float dots_y = desc_top - 13.0f;
    const float stage_top = sep_y + 8.0f;
    const float stage_bot = std::max(stage_top + 120.0f, dots_y - 7.0f);
    const D2D1_RECT_F stage_rc = SnapRectF(D2D1::RectF(ox + 16.0f, oy + stage_top, ox + w - 16.0f, oy + stage_bot));

    // 3. 45-degree diagonal-striped live preview stage (.gal-prev)
    dc->FillRoundedRectangle(D2D1::RoundedRect(stage_rc, 12.0f, 12.0f),
                             Brush(dc, dark_mode_ ? 0x22000000u : 0x0B000000u));
    dc->PushAxisAlignedClip(stage_rc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    {
        const float sh = stage_rc.bottom - stage_rc.top;
        for (float sx = stage_rc.left - sh - 28.0f; sx < stage_rc.right + 28.0f; sx += 28.284f) {
            dc->DrawLine(D2D1::Point2F(sx, stage_rc.top - 16.0f),
                         D2D1::Point2F(sx + sh + 32.0f, stage_rc.bottom + 16.0f),
                         Brush(dc, dark_mode_ ? 0x07FFFFFFu : 0x07000000u), 10.0f);
        }

        const auto& sel_def = FindGalleryWidgetDef(gallery_selected_kind_);
        const model::TileSize sz = gallery_selected_size_;
        const float tw = static_cast<float>(sz.cols * model::kTileUnitPx + (sz.cols - 1) * model::kTileGapPx);
        const float th = static_cast<float>(sz.rows * model::kTileUnitPx + (sz.rows - 1) * model::kTileGapPx);
        const float scale_k = std::min({1.0f, (stage_rc.right - stage_rc.left - 28.0f) / tw, (sh - 22.0f) / th});
        const float tx = (stage_rc.left + stage_rc.right) * 0.5f - tw * scale_k * 0.5f;
        const float ty = (stage_rc.top + stage_rc.bottom) * 0.5f - th * scale_k * 0.5f;

        D2D1_MATRIX_3X2_F saved_tf{};
        dc->GetTransform(&saved_tf);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(scale_k, scale_k) *
                         D2D1::Matrix3x2F::Translation(tx, ty) * saved_tf);

        const D2D1_RECT_F tr = D2D1::RectF(0.0f, 0.0f, tw, th);
        const float card_r = std::min({22.0f, tw * 0.5f, th * 0.5f});
        const D2D1_ROUNDED_RECT trr = D2D1::RoundedRect(tr, 16.0f, 16.0f);
        dc->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(1.0f, 4.0f, tw + 1.0f, th + 6.0f), card_r, card_r),
            Brush(dc, 0x42000000u));

        // The widget as the desktop draws it (same faces, material and data); the sketch below is only the
        // fallback when no renderer is attached.
        bool drew_real = false;
        if (callbacks_.on_render_widget_preview) {
            const uint64_t key = (static_cast<uint64_t>(gallery_selected_kind_) << 32) |
                                 (static_cast<uint64_t>(sz.cols) << 16) | (static_cast<uint64_t>(sz.rows) << 8) |
                                 (dark_mode_ ? 1u : 0u);
            const uint64_t now_ms = GetTickCount64();
            if (key != gallery_preview_key_ || now_ms - gallery_preview_ms_ >= 1000) {
                gallery_preview_px_.clear();
                gallery_preview_w_ = gallery_preview_h_ = 0;
                if (!callbacks_.on_render_widget_preview(gallery_selected_kind_, sz, gallery_preview_px_,
                                                         gallery_preview_w_, gallery_preview_h_))
                    gallery_preview_px_.clear();
                gallery_preview_key_ = key;
                gallery_preview_ms_ = now_ms;
            }
            if (!gallery_preview_px_.empty() && gallery_preview_w_ && gallery_preview_h_) {
                ComPtr<ID2D1Bitmap> bmp;
                const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                ComPtr<ID2D1RoundedRectangleGeometry> clip;
                if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(gallery_preview_w_, gallery_preview_h_), gallery_preview_px_.data(),
                                               gallery_preview_w_ * 4u, props, &bmp)) &&
                    SUCCEEDED(fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(tr, card_r, card_r), &clip))) {
                    dc->PushLayer(D2D1::LayerParameters1(tr, clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
                    dc->DrawBitmap(bmp.Get(), tr, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    dc->PopLayer();
                    drew_real = true;
                }
            }
        }
        if (!drew_real) {
        const bool is_photo = (gallery_selected_kind_ == model::TileKind::Photos);
        const bool is_note = (gallery_selected_kind_ == model::TileKind::StickyNote);
        if (is_photo) {
            dc->PushAxisAlignedClip(tr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            render::WallpaperBackdrop::DrawPhotoTileScene(fac.Get(), dc, tr, 0, dark_mode_);
            dc->FillRectangle(D2D1::RectF(0.0f, th - 48.0f, tw, th), Brush(dc, 0x68000000u));
            dc->DrawText(L"照片", 2, fmt_body_semi_.Get(), D2D1::RectF(14.0f, th - 32.0f, 120.0f, th - 10.0f),
                         Brush(dc, 0xFFFFFFFFu));
            dc->DrawText(L"128 项 · 每 3 秒换一张", 13, fmt_small_right_.Get(),
                         D2D1::RectF(tw - 160.0f, th - 30.0f, tw - 14.0f, th - 10.0f),
                         Brush(dc, 0xCCFFFFFFu));
            dc->PopAxisAlignedClip();
        } else {
            dc->FillRoundedRectangle(trr, Brush(dc, dark_mode_ ? 0xE6222834u : 0xF0F7F9FCu));
            if (is_note) {
                dc->FillRoundedRectangle(trr, Brush(dc, dark_mode_ ? 0x428A7322u : 0x52FCE178u));
            }
            if (grain_brush != nullptr) {
                dc->FillRoundedRectangle(trr, grain_brush);
            }
        }

        const uint32_t hue = sel_def.badge_rgb;
        auto draw_band_hdr = [&](const wchar_t* htitle, const wchar_t* hright) {
            dc->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, tw, 36.0f), D2D1_ANTIALIAS_MODE_ALIASED);
            dc->FillRoundedRectangle(trr, Brush(dc, is_note ? (dark_mode_ ? 0x1EFFE58Fu : 0x26D9A400u)
                                                            : (dark_mode_ ? 0x18FFFFFFu : 0x70FFFFFFu)));
            if (grain_brush != nullptr) {
                grain_brush->SetOpacity(dark_mode_ ? 0.12f : 0.44f);
                dc->FillRoundedRectangle(trr, grain_brush);
                grain_brush->SetOpacity(1.0f);
            }
            dc->PopAxisAlignedClip();
            dc->FillRectangle(D2D1::RectF(1.0f, 35.0f, tw - 1.0f, 36.0f),
                              Brush(dc, dark_mode_ ? 0x1AFFFFFFu : 0x12000000u));
            const wchar_t hg[2] = {sel_def.glyph, 0};
            DrawGlyph(dc, hg, 1, fmt_fluent_12_.Get(), D2D1::RectF(12.0f, 0.0f, 28.0f, 36.0f), Brush(dc, hue));
            dc->DrawText(htitle, static_cast<UINT32>(std::wcslen(htitle)), fmt_body_semi_.Get(),
                         D2D1::RectF(31.0f, 0.0f, tw - 96.0f, 36.0f), Brush(dc, is_note ? hue : (dark_mode_ ? pri : hue)));
            if (hright && hright[0]) {
                dc->DrawText(hright, static_cast<UINT32>(std::wcslen(hright)), fmt_small_right_.Get(),
                             D2D1::RectF(tw - 110.0f, 0.0f, tw - 12.0f, 36.0f), Brush(dc, sec));
            }
        };

        auto draw_ring = [&](float rcx, float rcy, float rad, float ratio, uint32_t col) {
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(rcx, rcy), rad, rad),
                            Brush(dc, dark_mode_ ? 0x24FFFFFFu : 0x16000000u), 4.0f);
            if (ratio <= 0.01f || !fac) return;
            ComPtr<ID2D1PathGeometry> geo;
            if (FAILED(fac->CreatePathGeometry(&geo)) || !geo) return;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(geo->Open(&sink)) || !sink) return;
            const float a0 = -1.5707963f;
            const float a1 = a0 + std::min(0.999f, ratio) * 6.2831853f;
            sink->BeginFigure(D2D1::Point2F(rcx + std::cos(a0) * rad, rcy + std::sin(a0) * rad),
                              D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(
                D2D1::Point2F(rcx + std::cos(a1) * rad, rcy + std::sin(a1) * rad),
                D2D1::SizeF(rad, rad), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                ratio > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (SUCCEEDED(sink->Close())) dc->DrawGeometry(geo.Get(), Brush(dc, col), 4.0f);
        };

        auto draw_wx_mini = [&](float wx, float wy, float wsz, int code) {
            const float s = wsz / 24.0f;
            if (code <= 2) {
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(wx + (code == 2 ? 9.0f : 12.0f) * s,
                                                            wy + (code == 2 ? 9.0f : 12.0f) * s),
                                              (code == 2 ? 4.2f : 5.2f) * s, (code == 2 ? 4.2f : 5.2f) * s),
                                Brush(dc, 0xFFFFC53Du));
            }
            if (code >= 2) {
                const uint32_t cc = dark_mode_ ? 0xFFE8EEF6u : 0xFF9AA8BAu;
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(wx + 11.0f * s, wy + 15.0f * s), 4.4f * s, 4.4f * s), Brush(dc, cc));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(wx + 15.5f * s, wy + 13.5f * s), 5.0f * s, 5.0f * s), Brush(dc, cc));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(wx + 6.0f * s, wy + 14.5f * s, wx + 21.0f * s, wy + 19.8f * s),
                                                           2.8f * s, 2.8f * s), Brush(dc, cc));
            }
        };

        // One-row sizes preview as the desktop draws them: the glance strip (DrawLiveTile).
        const bool strip_preview = sz.rows == 1 && sz.cols >= 2 &&
            (gallery_selected_kind_ == model::TileKind::Weather || gallery_selected_kind_ == model::TileKind::Calendar ||
             gallery_selected_kind_ == model::TileKind::Todo || gallery_selected_kind_ == model::TileKind::System);
        if (strip_preview) {
            const bool wide = sz.cols >= 4;
            const float cy = th * 0.5f;
            const float left = 14.0f;
            const float main_right = wide ? std::min(tw - 14.0f, 214.0f) : tw - 12.0f;
            dc->PushAxisAlignedClip(tr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            auto text = [&](const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, uint32_t c) {
                if (r.right > r.left) dc->DrawText(t.c_str(), static_cast<UINT32>(t.size()), f, r, Brush(dc, c));
            };
            auto two_lines = [&](float x, float right, const std::wstring& top, const std::wstring& bottom) {
                dc->PushAxisAlignedClip(D2D1::RectF(x, 0.0f, right, th), D2D1_ANTIALIAS_MODE_ALIASED);
                text(top, fmt_body_semi_.Get(), D2D1::RectF(x, cy - 21.0f, right + 200.0f, cy - 1.0f), pri);
                text(bottom, fmt_small_.Get(), D2D1::RectF(x, cy + 1.0f, right + 200.0f, cy + 19.0f), sec);
                dc->PopAxisAlignedClip();
            };
            auto divider = [&](float x) {
                dc->FillRectangle(D2D1::RectF(x, 14.0f, x + 1.0f, th - 14.0f), Brush(dc, dark_mode_ ? 0x1CFFFFFFu : 0x12000000u));
            };
            SYSTEMTIME now{};
            GetLocalTime(&now);
            static constexpr const wchar_t* kWd[7] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
            switch (gallery_selected_kind_) {
            case model::TileKind::Weather: {
                draw_wx_mini(left - 2.0f, cy - 16.0f, 32.0f, 2);
                text(L"12°", fmt_gallery_big_.Get(), D2D1::RectF(left + 36.0f, cy - 22.0f, left + 100.0f, cy + 22.0f), pri);
                const float x = left + 96.0f;
                const float right = wide ? x + 106.0f : main_right;
                two_lines(x, right, L"北京", wide ? L"多云转晴 · 7°/15°" : L"多云转晴");
                if (wide) {
                    divider(right + 6.0f);
                    static constexpr std::pair<const wchar_t*, int> kHours[] = {{L"15时", 15}, {L"16时", 15}, {L"17时", 14}, {L"18时", 12}};
                    const float hx0 = right + 14.0f, hx1 = tw - 10.0f;
                    const size_t n = std::min<size_t>(4, static_cast<size_t>(std::max(0.0f, (hx1 - hx0) / 38.0f)));
                    const float step = n ? (hx1 - hx0) / static_cast<float>(n) : 0.0f;
                    for (size_t i = 0; i < n; ++i) {
                        const float hcx = hx0 + step * (static_cast<float>(i) + 0.5f);
                        text(kHours[i].first, fmt_small_center_.Get(), D2D1::RectF(hcx - 22.0f, 8.0f, hcx + 22.0f, 24.0f), sec);
                        draw_wx_mini(hcx - 10.0f, cy - 10.0f, 20.0f, i == 3 ? 3 : 0);
                        text(std::to_wstring(kHours[i].second) + L"°", fmt_small_center_.Get(),
                             D2D1::RectF(hcx - 22.0f, th - 26.0f, hcx + 22.0f, th - 8.0f), pri);
                    }
                }
                break;
            }
            case model::TileKind::Calendar: {
                const D2D1_RECT_F page = D2D1::RectF(left, cy - 22.0f, left + 44.0f, cy + 22.0f);
                dc->FillRoundedRectangle(D2D1::RoundedRect(page, 8.0f, 8.0f), Brush(dc, dark_mode_ ? 0x1FFFFFFFu : 0xD9FFFFFFu));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(page.left, page.top, page.right, page.top + 14.0f), 8.0f, 8.0f), Brush(dc, 0xFFE5484Du));
                dc->FillRectangle(D2D1::RectF(page.left, page.top + 8.0f, page.right, page.top + 14.0f), Brush(dc, 0xFFE5484Du));
                text(kWd[now.wDayOfWeek % 7], fmt_small_center_.Get(), D2D1::RectF(page.left, page.top - 1.0f, page.right, page.top + 14.0f), 0xFFFFFFFFu);
                text(std::to_wstring(now.wDay), fmt_small_center_.Get(), D2D1::RectF(page.left, page.top + 18.0f, page.right, page.bottom), pri);
                wchar_t date[32]{};
                std::swprintf(date, ARRAYSIZE(date), L"%u月%u日 %ls", now.wMonth, now.wDay, kWd[now.wDayOfWeek % 7]);
                two_lines(page.right + 12.0f, main_right, date, L"季度汇报评审 · 16:00");
                if (wide) {
                    divider(main_right + 6.0f);
                    text(L"设计走查 · 17:30", fmt_small_.Get(), D2D1::RectF(main_right + 16.0f, cy - 20.0f, tw - 12.0f, cy - 1.0f), sec);
                    text(L"发版前核对 · 19:00", fmt_small_.Get(), D2D1::RectF(main_right + 16.0f, cy + 1.0f, tw - 12.0f, cy + 20.0f), sec);
                }
                break;
            }
            case model::TileKind::Todo: {
                draw_ring(left + 18.0f, cy, 15.0f, 0.5f, hue);
                text(L"2", fmt_small_center_.Get(), D2D1::RectF(left + 3.0f, cy - 9.0f, left + 33.0f, cy + 9.0f), pri);
                two_lines(left + 46.0f, main_right, L"待办 · 2/4", L"核对预算表");
                if (wide) {
                    divider(main_right + 6.0f);
                    float item_y = cy - 20.0f;
                    for (const wchar_t* item : {L"合同扫描件发法务", L"给设计评审写纪要"}) {
                        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(main_right + 22.0f, item_y + 9.5f), 5.0f, 5.0f), Brush(dc, sec), 1.2f);
                        text(item, fmt_small_.Get(), D2D1::RectF(main_right + 34.0f, item_y, tw - 12.0f, item_y + 19.0f), pri);
                        item_y += 21.0f;
                    }
                }
                break;
            }
            default: {  // System
                struct Meter { const wchar_t* label; float ratio; uint32_t color; const wchar_t* value; };
                std::vector<Meter> meters = {{L"CPU", 0.23f, hue, L"23%"}, {L"内存", 0.62f, 0xFF30A46Cu, L"62%"}};
                if (wide) {
                    meters.push_back({L"C:", 0.71f, 0xFFF5A524u, L"剩 113 GB"});
                    meters.push_back({L"网络", 0.3f, 0xFF8E6BE8u, L"↓ 2.4 MB/s"});
                }
                const float slot = (tw - 12.0f) / static_cast<float>(meters.size());
                for (size_t i = 0; i < meters.size(); ++i) {
                    const float x0 = 8.0f + slot * static_cast<float>(i);
                    draw_ring(x0 + 22.0f, cy, 13.0f, meters[i].ratio, meters[i].color);
                    two_lines(x0 + 42.0f, x0 + slot - 4.0f, meters[i].label, meters[i].value);
                }
                break;
            }
            }
            dc->PopAxisAlignedClip();
        } else
        switch (gallery_selected_kind_) {
        case model::TileKind::Photos:
            break;

        case model::TileKind::Calendar: {
            if (sz.cols == 1) {
                dc->DrawText(L"周五", 2, fmt_body_semi_.Get(), D2D1::RectF(12.0f, 10.0f, 92.0f, 26.0f), Brush(dc, 0xFFFF6B6Fu));
                dc->DrawText(L"6", 1, fmt_gallery_big_.Get(), D2D1::RectF(12.0f, 24.0f, 92.0f, 74.0f), Brush(dc, pri));
                dc->DrawText(L"16:00", 5, fmt_body_semi_.Get(), D2D1::RectF(12.0f, 74.0f, 52.0f, 92.0f), Brush(dc, pri));
                dc->DrawText(L"汇报", 2, fmt_small_.Get(), D2D1::RectF(50.0f, 74.0f, 94.0f, 92.0f), Brush(dc, sec));
            } else if (sz.cols == 2) {
                dc->DrawText(L"11月6日 周五", 8, fmt_body_semi_.Get(), D2D1::RectF(14.0f, 12.0f, 194.0f, 30.0f), Brush(dc, 0xFFFF6B6Fu));
                struct Ev { const wchar_t* title; const wchar_t* sub; uint32_t col; };
                static constexpr Ev kEvs[2] = {
                    {L"季度汇报评审", L"16:00 · Teams", 0xFF4CA6FFu},
                    {L"设计走查",     L"17:30 · 302 室", 0xFF30A46Cu},
                };
                for (int ei = 0; ei < 2; ++ei) {
                    const float ey = 42.0f + static_cast<float>(ei) * 46.0f;
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(14.0f, ey + 2.0f, 17.0f, ey + 34.0f), 1.5f, 1.5f),
                                             Brush(dc, kEvs[ei].col));
                    dc->DrawText(kEvs[ei].title, static_cast<UINT32>(std::wcslen(kEvs[ei].title)), fmt_body_semi_.Get(),
                                 D2D1::RectF(24.0f, ey, 196.0f, ey + 18.0f), Brush(dc, pri));
                    dc->DrawText(kEvs[ei].sub, static_cast<UINT32>(std::wcslen(kEvs[ei].sub)), fmt_small_.Get(),
                                 D2D1::RectF(24.0f, ey + 18.0f, 196.0f, ey + 36.0f), Brush(dc, sec));
                }
                dc->DrawText(L"还有 1 个日程", 7, fmt_small_.Get(), D2D1::RectF(14.0f, th - 28.0f, 194.0f, th - 10.0f), Brush(dc, sec));
            } else {
                dc->DrawText(L"11月", 3, fmt_body_semi_.Get(), D2D1::RectF(16.0f, 12.0f, 120.0f, 30.0f), Brush(dc, 0xFFFF6B6Fu));
                static constexpr const wchar_t* kHd[7] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
                for (int c = 0; c < 7; ++c) {
                    dc->DrawText(kHd[c], 1, fmt_small_center_.Get(),
                                 D2D1::RectF(14.0f + c * 26.0f, 32.0f, 40.0f + c * 26.0f, 48.0f), Brush(dc, sec));
                }
                for (int d = 1; d <= 30; ++d) {
                    const int cell = 5 + d - 1, c = cell % 7, r = cell / 7;
                    const D2D1_RECT_F drc = D2D1::RectF(14.0f + c * 26.0f, 50.0f + r * 22.0f, 40.0f + c * 26.0f, 72.0f + r * 22.0f);
                    if (d == 6) {
                        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((drc.left + drc.right) * 0.5f, (drc.top + drc.bottom) * 0.5f), 9.5f, 9.5f),
                                        Brush(dc, 0xFFE5484Du));
                    }
                    wchar_t db[4]{};
                    std::swprintf(db, ARRAYSIZE(db), L"%d", d);
                    dc->DrawText(db, static_cast<UINT32>(std::wcslen(db)), fmt_small_center_.Get(), drc,
                                 Brush(dc, d == 6 ? 0xFFFFFFFFu : pri));
                }
                dc->DrawText(L"今天 · 周五", 6, fmt_body_semi_.Get(), D2D1::RectF(214.0f, 12.0f, tw - 14.0f, 30.0f), Brush(dc, 0xFFFF6B6Fu));
                struct Ev { const wchar_t* title; const wchar_t* sub; uint32_t col; };
                static constexpr Ev kEvs3[3] = {
                    {L"季度汇报评审", L"16:00 · Teams", 0xFF4CA6FFu},
                    {L"设计走查",     L"17:30 · 302 室", 0xFF30A46Cu},
                    {L"发版前核对",   L"19:00 · 线上",   0xFFF5A524u},
                };
                for (int ei = 0; ei < 3; ++ei) {
                    const float ey = 42.0f + static_cast<float>(ei) * 48.0f;
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(214.0f, ey + 2.0f, 217.0f, ey + 34.0f), 1.5f, 1.5f),
                                             Brush(dc, kEvs3[ei].col));
                    dc->DrawText(kEvs3[ei].title, static_cast<UINT32>(std::wcslen(kEvs3[ei].title)), fmt_body_semi_.Get(),
                                 D2D1::RectF(224.0f, ey, tw - 12.0f, ey + 18.0f), Brush(dc, pri));
                    dc->DrawText(kEvs3[ei].sub, static_cast<UINT32>(std::wcslen(kEvs3[ei].sub)), fmt_small_.Get(),
                                 D2D1::RectF(224.0f, ey + 18.0f, tw - 12.0f, ey + 36.0f), Brush(dc, sec));
                }
            }
            break;
        }

        case model::TileKind::Weather: {
            if (sz.cols == 1) {
                dc->DrawText(L"北京", 2, fmt_body_semi_.Get(), D2D1::RectF(12.0f, 10.0f, 90.0f, 26.0f), Brush(dc, pri));
                draw_wx_mini(12.0f, 30.0f, 26.0f, 2);
                dc->DrawText(L"12°", 3, fmt_stat_num_.Get(), D2D1::RectF(8.0f, 62.0f, 60.0f, 90.0f), Brush(dc, pri));
            } else {
                const wchar_t pin[2] = {L'\xE707', 0};
                DrawGlyph(dc, pin, 1, fmt_fluent_12_.Get(), D2D1::RectF(14.0f, 12.0f, 28.0f, 28.0f), Brush(dc, sec));
                dc->DrawText(L"北京", 2, fmt_body_semi_.Get(), D2D1::RectF(30.0f, 11.0f, 140.0f, 29.0f), Brush(dc, pri));
                dc->DrawText(L"12°", 3, fmt_gallery_big_.Get(), D2D1::RectF(14.0f, 28.0f, 96.0f, 78.0f), Brush(dc, pri));
                draw_wx_mini(76.0f, 36.0f, 30.0f, 2);
                dc->DrawText(L"多云转晴 · 7°/15°", 12, fmt_small_.Get(), D2D1::RectF(14.0f, 82.0f, 180.0f, 100.0f), Brush(dc, pri));
                const float hx0 = sz.cols >= 4 ? 176.0f : 12.0f;
                const float hy = sz.cols >= 4 ? 18.0f : th - 68.0f;
                if (sz.cols < 4) {
                    dc->FillRectangle(D2D1::RectF(14.0f, hy - 8.0f, tw - 14.0f, hy - 7.0f), Brush(dc, dark_mode_ ? 0x1CFFFFFFu : 0x12000000u));
                }
                static constexpr const wchar_t* kHrs[4] = {L"15时", L"16时", L"17时", L"18时"};
                static constexpr int kCodes[4] = {2, 0, 0, 3};
                static constexpr const wchar_t* kTemps[4] = {L"15°", L"15°", L"14°", L"12°"};
                const float step = (tw - 14.0f - hx0) / 4.0f;
                for (int hi = 0; hi < 4; ++hi) {
                    const float cx = hx0 + step * (static_cast<float>(hi) + 0.5f);
                    dc->DrawText(kHrs[hi], 3, fmt_small_center_.Get(), D2D1::RectF(cx - 22.0f, hy, cx + 22.0f, hy + 16.0f), Brush(dc, sec));
                    draw_wx_mini(cx - 10.0f, hy + 18.0f, 20.0f, kCodes[hi]);
                    dc->DrawText(kTemps[hi], 3, fmt_small_center_.Get(), D2D1::RectF(cx - 22.0f, hy + 40.0f, cx + 22.0f, hy + 56.0f), Brush(dc, pri));
                }
            }
            break;
        }

        case model::TileKind::Todo: {
            if (sz.cols == 1) {
                draw_ring(50.0f, 38.0f, 21.0f, 0.25f, hue);
                dc->DrawText(L"1/4", 3, fmt_small_center_.Get(), D2D1::RectF(28.0f, 28.0f, 72.0f, 48.0f), Brush(dc, pri));
                dc->DrawText(L"待办", 2, fmt_small_center_.Get(), D2D1::RectF(8.0f, 72.0f, 92.0f, 92.0f), Brush(dc, pri));
            } else {
                draw_band_hdr(L"待办", L"1/4 完成");
                static constexpr const wchar_t* kTodos[4] = {L"发季度汇报给张经理", L"核对 2027 预算表", L"合同扫描件发法务", L"清理旧安装包"};
                for (int ti = 0; ti < 4; ++ti) {
                    const float ry = 44.0f + static_cast<float>(ti) * 36.0f;
                    const bool done = (ti == 3);
                    const D2D1_ELLIPSE el = D2D1::Ellipse(D2D1::Point2F(22.0f, ry + 13.0f), 7.0f, 7.0f);
                    if (done) dc->FillEllipse(el, Brush(dc, hue));
                    else dc->DrawEllipse(el, Brush(dc, sec), 1.4f);
                    dc->DrawText(kTodos[ti], static_cast<UINT32>(std::wcslen(kTodos[ti])), fmt_body_.Get(),
                                 D2D1::RectF(36.0f, ry, tw - 12.0f, ry + 26.0f), Brush(dc, done ? sec : pri));
                }
            }
            break;
        }

        case model::TileKind::Clock: {
            if (sz.cols == 1) {
                const float cx = 50.0f, cy = 40.0f, rad = 24.0f;
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), Brush(dc, dark_mode_ ? 0x26000000u : 0x10000000u));
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), Brush(dc, dark_mode_ ? 0x36FFFFFFu : 0x22000000u), 1.3f);
                dc->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx - 9.0f, cy - 8.0f), Brush(dc, pri), 2.2f);
                dc->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx + 12.0f, cy - 11.0f), Brush(dc, pri), 1.6f);
                dc->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx + 4.0f, cy + 16.0f), Brush(dc, hue), 1.2f);
                dc->DrawText(L"北京 · 周五", 7, fmt_small_center_.Get(), D2D1::RectF(4.0f, 72.0f, 96.0f, 92.0f), Brush(dc, pri));
            } else {
                dc->DrawText(L"14:36", 5, fmt_gallery_big_.Get(), D2D1::RectF(16.0f, 14.0f, 160.0f, 66.0f), Brush(dc, pri));
                dc->DrawText(L"11月6日 周五 · 北京", 13, fmt_small_.Get(), D2D1::RectF(16.0f, 66.0f, tw - 12.0f, 88.0f), Brush(dc, sec));
            }
            break;
        }

        case model::TileKind::StickyNote: {
            if (sz.cols == 1) {
                const wchar_t ng[2] = {L'\xE70B', 0};
                DrawGlyph(dc, ng, 1, fmt_fluent_20_.Get(), D2D1::RectF(24.0f, 18.0f, 76.0f, 58.0f), Brush(dc, hue));
                dc->DrawText(L"便笺", 2, fmt_small_center_.Get(), D2D1::RectF(8.0f, 68.0f, 92.0f, 90.0f), Brush(dc, pri));
            } else {
                draw_band_hdr(L"便笺", L"自动保存");
                dc->DrawText(L"周五 16:00 前交季度汇报\n· 预算表再核一遍\n· 合同扫描件发法务", 36,
                             fmt_body_.Get(), D2D1::RectF(14.0f, 46.0f, tw - 14.0f, th - 12.0f), Brush(dc, pri));
            }
            break;
        }

        case model::TileKind::Apps: {
            // Render authentic bottom-center macOS 26 Tahoe Liquid Glass Dock preview
            const float ph = std::min(th - 16.0f, 66.0f);
            const float py = (th - ph) * 0.5f + 4.0f;
            const float pw = std::max(84.0f, tw - 16.0f);
            const float px = (tw - pw) * 0.5f;
            const float pr = std::min(ph * 0.31f, 20.0f);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(px, py, px + pw, py + ph), pr, pr),
                                     Brush(dc, dark_mode_ ? 0x26FFFFFFu : 0x42FFFFFFu));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(px + 0.5f, py + 0.5f, px + pw - 0.5f, py + ph - 0.5f), pr, pr),
                                     Brush(dc, dark_mode_ ? 0x42FFFFFFu : 0x62FFFFFFu), 1.0f);
            const float base = std::clamp(ph - 22.0f, 28.0f, 40.0f);
            const float cy = py + ph - 12.0f - base * 0.5f;
            const int count = (pw >= 280.0f) ? 6 : 3;
            const float step = (pw - 24.0f) / static_cast<float>(count);
            for (int i = 0; i < count; ++i) {
                const float cx = px + 12.0f + (static_cast<float>(i) + 0.5f) * step;
                const float sc = (i == count / 2) ? 1.24f : 1.0f;
                const float s = base * sc;
                const float lift = (sc - 1.0f) * base * 0.25f;
                const D2D1_RECT_F ir = D2D1::RectF(cx - s * 0.5f, cy - s * 0.5f - lift, cx + s * 0.5f, cy + s * 0.5f - lift);
                if (i == count - 1) render::DrawDockWidgetsVectorIcon(dc, ir);
                else if (i == count - 2 && count >= 4) render::DrawDockSettingsVectorIcon(dc, ir);
                else if (i == 1) render::DrawDockTerminalVectorIcon(dc, ir);
                else if (i == 2 && count >= 4) render::DrawDockCalculatorVectorIcon(dc, ir);
                else {
                    static constexpr uint32_t kC[3] = {0xFF2F8FE8u, 0xFF34C759u, 0xFFFF9500u};
                    dc->FillRoundedRectangle(D2D1::RoundedRect(ir, s * 0.23f, s * 0.23f), Brush(dc, kC[i % 3]));
                }
                if (i == count / 2) {
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, py + ph - 5.0f), 4.5f, 3.0f), Brush(dc, 0x736BD1FFu));
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, py + ph - 5.0f), 2.0f, 2.0f), Brush(dc, 0xF5FFFFFFu));
                } else if (i == 0) {
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, py + ph - 5.0f), 1.9f, 1.9f), Brush(dc, pri, 0.85f));
                }
            }
            break;
        }

        case model::TileKind::Recent:
        case model::TileKind::Downloads: {
            const bool is_dl = (gallery_selected_kind_ == model::TileKind::Downloads);
            if (sz.cols == 1) {
                const wchar_t fg[2] = {sel_def.glyph, 0};
                DrawGlyph(dc, fg, 1, fmt_fluent_20_.Get(), D2D1::RectF(24.0f, 18.0f, 76.0f, 58.0f), Brush(dc, hue));
                dc->DrawText(sel_def.name, static_cast<UINT32>(std::wcslen(sel_def.name)), fmt_small_center_.Get(),
                             D2D1::RectF(4.0f, 68.0f, 96.0f, 90.0f), Brush(dc, pri));
            } else {
                draw_band_hdr(sel_def.name, is_dl ? L"4 项" : L"Pulse 和应用");
                struct FRow { const wchar_t* name; const wchar_t* ext; const wchar_t* meta; };
                static constexpr FRow kDlRows[4] = {
                    {L"PulseSetup-1.0.58.exe", L".exe", L"刚刚"},
                    {L"设计走查截图.png",      L".png", L"14 分钟前"},
                    {L"归档素材包.zip",        L".zip", L"昨天"},
                    {L"年度报告模板.pptx",     L".pptx", L"2 天前"},
                };
                static constexpr FRow kRecRows[4] = {
                    {L"季度汇报.pptx",    L".pptx", L"8 分钟前"},
                    {L"预算-2027.xlsx",   L".xlsx", L"1 小时前"},
                    {L"合同扫描.pdf",     L".pdf",  L"今天"},
                    {L"需求说明.docx",    L".docx", L"昨天"},
                };
                const FRow* rows = is_dl ? kDlRows : kRecRows;
                for (int ri = 0; ri < 4; ++ri) {
                    const float ry = 46.0f + static_cast<float>(ri) * 34.0f;
                    render::DrawVectorFileIcon(fac.Get(), dc, dwrite_.Get(),
                                               D2D1::RectF(14.0f, ry + 4.0f, 30.0f, ry + 20.0f),
                                               rows[ri].ext, false);
                    dc->DrawText(rows[ri].name, static_cast<UINT32>(std::wcslen(rows[ri].name)), fmt_body_.Get(),
                                 D2D1::RectF(36.0f, ry, tw - 68.0f, ry + 24.0f), Brush(dc, pri));
                    dc->DrawText(rows[ri].meta, static_cast<UINT32>(std::wcslen(rows[ri].meta)), fmt_small_right_.Get(),
                                 D2D1::RectF(tw - 68.0f, ry, tw - 12.0f, ry + 24.0f), Brush(dc, sec));
                }
            }
            break;
        }

        case model::TileKind::System: {
            if (sz.cols == 1) {
                draw_ring(30.0f, 38.0f, 16.0f, 0.23f, hue);
                dc->DrawText(L"23", 2, fmt_small_center_.Get(), D2D1::RectF(14.0f, 29.0f, 46.0f, 47.0f), Brush(dc, pri));
                draw_ring(70.0f, 38.0f, 16.0f, 0.61f, 0xFF30A46Cu);
                dc->DrawText(L"61", 2, fmt_small_center_.Get(), D2D1::RectF(54.0f, 29.0f, 86.0f, 47.0f), Brush(dc, pri));
                dc->DrawText(L"CPU · 内存", 8, fmt_small_center_.Get(), D2D1::RectF(4.0f, 70.0f, 96.0f, 90.0f), Brush(dc, pri));
            } else {
                draw_band_hdr(L"系统", L"良好");
                struct Bar { const wchar_t* lbl; float pct; const wchar_t* val; uint32_t col; };
                static constexpr Bar kBars[4] = {
                    {L"CPU",  0.23f, L"23%",      0xFF5A72E8u},
                    {L"内存", 0.61f, L"9.8 GB",   0xFF30A46Cu},
                    {L"C:",   0.76f, L"113 GB",   0xFFF5A524u},
                    {L"网络", 0.30f, L"2.4 MB/s", 0xFF8E6BE8u},
                };
                const int nbars = sz.cols >= 4 ? 4 : 3;
                for (int bi = 0; bi < nbars; ++bi) {
                    const float by = 48.0f + static_cast<float>(bi) * 36.0f;
                    dc->DrawText(kBars[bi].lbl, static_cast<UINT32>(std::wcslen(kBars[bi].lbl)), fmt_small_.Get(),
                                 D2D1::RectF(14.0f, by, 52.0f, by + 22.0f), Brush(dc, sec));
                    const float bx0 = 54.0f, bx1 = tw - 66.0f;
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, by + 8.0f, bx1, by + 14.0f), 3.0f, 3.0f),
                                             Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x14000000u));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, by + 8.0f, bx0 + (bx1 - bx0) * kBars[bi].pct, by + 14.0f), 3.0f, 3.0f),
                                             Brush(dc, kBars[bi].col));
                    dc->DrawText(kBars[bi].val, static_cast<UINT32>(std::wcslen(kBars[bi].val)), fmt_small_right_.Get(),
                                 D2D1::RectF(tw - 64.0f, by, tw - 12.0f, by + 22.0f), Brush(dc, pri));
                }
            }
            break;
        }

        case model::TileKind::StagingTray: {
            if (sz.cols == 1) {
                const wchar_t tg[2] = {L'\xE7B8', 0};
                DrawGlyph(dc, tg, 1, fmt_fluent_20_.Get(), D2D1::RectF(24.0f, 18.0f, 76.0f, 58.0f), Brush(dc, hue));
                dc->DrawText(L"暂存区", 3, fmt_small_center_.Get(), D2D1::RectF(4.0f, 68.0f, 96.0f, 90.0f), Brush(dc, pri));
            } else {
                draw_band_hdr(L"暂存区", sz.cols >= 4 ? L"和 Pulse 同步" : L"");
                if (sz.cols >= 4) {
                    const float ky = 56.0f;
                    dc->DrawText(L"把文件拖到这里，或选中后按 ", 14, fmt_small_.Get(),
                                 D2D1::RectF(14.0f, ky, 186.0f, ky + 22.0f), Brush(dc, sec));
                    const D2D1_RECT_F kbd = D2D1::RectF(176.0f, ky + 2.0f, 256.0f, ky + 20.0f);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(kbd, 4.0f, 4.0f), Brush(dc, dark_mode_ ? 0x22FFFFFFu : 0x10000000u));
                    StrokeCrispRoundedRect(dc, kbd, 4.0f, Brush(dc, dark_mode_ ? 0x2EFFFFFFu : 0x1C000000u), 1.0f);
                    dc->DrawText(L"Ctrl+Shift+S", 12, fmt_small_center_.Get(), kbd, Brush(dc, pri));
                } else {
                    dc->DrawText(L"把文件拖到这里暂存", 9, fmt_small_.Get(),
                                 D2D1::RectF(14.0f, 46.0f, tw - 14.0f, th - 10.0f), Brush(dc, sec));
                }
            }
            break;
        }

        case model::TileKind::Pomodoro: {
            const uint32_t pc = 0xFFFF5A52u;
            if (sz.cols == 1) {
                draw_ring(50.0f, 38.0f, 22.0f, 0.72f, pc);
                dc->DrawText(L"18:24", 5, fmt_small_center_.Get(), D2D1::RectF(24.0f, 28.0f, 76.0f, 48.0f), Brush(dc, pri));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(14.0f, 72.0f, 86.0f, 91.0f), 9.0f, 9.0f), Brush(dc, pc, 0.88f));
                dc->DrawText(L"专注", 2, fmt_small_center_.Get(), D2D1::RectF(14.0f, 72.0f, 86.0f, 91.0f), Brush(dc, 0xFFFFFFFFu));
            } else if (sz.rows == 1) {
                draw_ring(36.0f, th * 0.5f, 20.0f, 0.72f, pc);
                dc->DrawText(L"72%", 3, fmt_small_center_.Get(), D2D1::RectF(14.0f, th * 0.5f - 9.0f, 58.0f, th * 0.5f + 9.0f), Brush(dc, pri));
                dc->DrawText(L"18:24", 5, fmt_body_semi_.Get(), D2D1::RectF(66.0f, th * 0.5f - 20.0f, 146.0f, th * 0.5f), Brush(dc, pri));
                dc->DrawText(L"专注中 · 今日 3 个", 10, fmt_small_.Get(), D2D1::RectF(66.0f, th * 0.5f + 1.0f, 162.0f, th * 0.5f + 19.0f), Brush(dc, sec));
                const float bx = tw - 28.0f, by = th * 0.5f;
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bx, by), 14.0f, 14.0f), Brush(dc, pc));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx - 4.0f, by - 4.5f, bx - 1.2f, by + 4.5f), 1.0f, 1.0f), Brush(dc, 0xFFFFFFFFu));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx + 1.2f, by - 4.5f, bx + 4.0f, by + 4.5f), 1.0f, 1.0f), Brush(dc, 0xFFFFFFFFu));
            } else if (sz.cols == 2) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(12.0f, 11.0f, 86.0f, 30.0f), 9.0f, 9.0f), Brush(dc, pc, 0.22f));
                dc->DrawText(L"番茄专注", 4, fmt_small_center_.Get(), D2D1::RectF(12.0f, 11.0f, 86.0f, 30.0f), Brush(dc, pc));
                dc->DrawText(L"今日 3 个", 6, fmt_small_right_.Get(), D2D1::RectF(96.0f, 12.0f, tw - 12.0f, 29.0f), Brush(dc, sec));
                draw_ring(tw * 0.5f, 98.0f, 44.0f, 0.72f, pc);
                dc->DrawText(L"18:24", 5, fmt_gallery_big_.Get(), D2D1::RectF(tw * 0.5f - 54.0f, 70.0f, tw * 0.5f + 54.0f, 112.0f), Brush(dc, pri));
                dc->DrawText(L"专注中", 3, fmt_small_center_.Get(), D2D1::RectF(tw * 0.5f - 40.0f, 108.0f, tw * 0.5f + 40.0f, 126.0f), Brush(dc, sec));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(14.0f, th - 36.0f, tw - 72.0f, th - 12.0f), 12.0f, 12.0f), Brush(dc, pc));
                dc->DrawText(L"暂停专注", 4, fmt_small_center_.Get(), D2D1::RectF(14.0f, th - 36.0f, tw - 72.0f, th - 12.0f), Brush(dc, 0xFFFFFFFFu));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tw - 64.0f, th - 36.0f, tw - 14.0f, th - 12.0f), 12.0f, 12.0f), Brush(dc, dark_mode_ ? 0x22FFFFFFu : 0x12000000u));
                dc->DrawText(L"重置", 2, fmt_small_center_.Get(), D2D1::RectF(tw - 64.0f, th - 36.0f, tw - 14.0f, th - 12.0f), Brush(dc, pri));
            } else {
                draw_ring(98.0f, th * 0.5f, 54.0f, 0.72f, pc);
                dc->DrawText(L"18:24", 5, fmt_gallery_big_.Get(), D2D1::RectF(42.0f, th * 0.5f - 28.0f, 154.0f, th * 0.5f + 14.0f), Brush(dc, pri));
                dc->DrawText(L"专注中", 3, fmt_small_center_.Get(), D2D1::RectF(56.0f, th * 0.5f + 12.0f, 140.0f, th * 0.5f + 30.0f), Brush(dc, sec));
                const float rx0 = 198.0f;
                dc->DrawText(L"番茄专注 · 今日已完成 3 个", 14, fmt_body_semi_.Get(), D2D1::RectF(rx0, 22.0f, tw - 14.0f, 44.0f), Brush(dc, pri));
                const wchar_t* chips[3] = {L"25 分钟", L"45 分钟", L"5 分休"};
                const float cw = (tw - 14.0f - rx0 - 12.0f) / 3.0f;
                for (int ci = 0; ci < 3; ++ci) {
                    const D2D1_RECT_F cr = D2D1::RectF(rx0 + ci * (cw + 6.0f), 78.0f, rx0 + ci * (cw + 6.0f) + cw, 106.0f);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(cr, 9.0f, 9.0f), Brush(dc, ci == 0 ? pc : (dark_mode_ ? 0x1EFFFFFFu : 0x10000000u), ci == 0 ? 0.24f : 1.0f));
                    dc->DrawText(chips[ci], static_cast<UINT32>(std::wcslen(chips[ci])), fmt_small_center_.Get(), cr, Brush(dc, ci == 0 ? pc : pri));
                }
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rx0, th - 42.0f, tw - 88.0f, th - 14.0f), 13.0f, 13.0f), Brush(dc, pc));
                dc->DrawText(L"暂停专注", 4, fmt_small_center_.Get(), D2D1::RectF(rx0, th - 42.0f, tw - 88.0f, th - 14.0f), Brush(dc, 0xFFFFFFFFu));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tw - 80.0f, th - 42.0f, tw - 14.0f, th - 14.0f), 13.0f, 13.0f), Brush(dc, dark_mode_ ? 0x20FFFFFFu : 0x10000000u));
                dc->DrawText(L"重置", 2, fmt_small_center_.Get(), D2D1::RectF(tw - 80.0f, th - 42.0f, tw - 14.0f, th - 14.0f), Brush(dc, pri));
            }
            break;
        }

        case model::TileKind::MediaPlayer: {
            const uint32_t mc = 0xFFFF4D8Du;
            auto draw_vinyl = [&](float x, float y, float s) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + s, y + s), s * 0.2f, s * 0.2f), Brush(dc, 0xFF1F1830u));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + s * 0.5f, y + s * 0.5f), s * 0.36f, s * 0.36f), Brush(dc, 0xFF100E18u));
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + s * 0.5f, y + s * 0.5f), s * 0.28f, s * 0.28f), Brush(dc, 0x30FFFFFFu), 1.0f);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + s * 0.5f, y + s * 0.5f), s * 0.14f, s * 0.14f), Brush(dc, mc));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + s * 0.5f, y + s * 0.5f), s * 0.04f, s * 0.04f), Brush(dc, 0xFFFFFFFFu));
            };
            auto draw_ctrl_cluster = [&](float bcx, float by, float pr, float spread) {
                const float sr = pr * 0.78f;
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bcx - spread, by), sr, sr), Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x12000000u));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bcx + spread, by), sr, sr), Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x12000000u));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bcx, by), pr, pr), Brush(dc, mc));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bcx - 4.0f, by - 5.0f, bcx - 1.2f, by + 5.0f), 1.0f, 1.0f), Brush(dc, 0xFFFFFFFFu));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bcx + 1.2f, by - 5.0f, bcx + 4.0f, by + 5.0f), 1.0f, 1.0f), Brush(dc, 0xFFFFFFFFu));
                const wchar_t prev_g[2] = {L'\xE892', 0}, next_g[2] = {L'\xE893', 0};
                DrawGlyph(dc, prev_g, 1, fmt_fluent_12_.Get(), D2D1::RectF(bcx - spread - sr, by - sr, bcx - spread + sr, by + sr), Brush(dc, pri));
                DrawGlyph(dc, next_g, 1, fmt_fluent_12_.Get(), D2D1::RectF(bcx + spread - sr, by - sr, bcx + spread + sr, by + sr), Brush(dc, pri));
            };
            if (sz.rows == 1) {
                const float art = sz.cols >= 4 ? 62.0f : 52.0f;
                draw_vinyl(12.0f, (th - art) * 0.5f, art);
                dc->DrawText(L"夜曲 · Nocturne", 13, fmt_body_semi_.Get(), D2D1::RectF(art + 22.0f, 16.0f, tw - (sz.cols >= 4 ? 130.0f : 12.0f), 36.0f), Brush(dc, pri));
                dc->DrawText(L"周杰伦 · 十一月的萧邦", 11, fmt_small_.Get(), D2D1::RectF(art + 22.0f, 36.0f, tw - (sz.cols >= 4 ? 130.0f : 12.0f), 54.0f), Brush(dc, sec));
                const float bcx = sz.cols >= 4 ? (tw - 68.0f) : ((art + 22.0f + tw - 10.0f) * 0.5f);
                const float by = sz.cols >= 4 ? th * 0.5f : th - 20.0f;
                draw_ctrl_cluster(bcx, by, sz.cols >= 4 ? 16.0f : 13.0f, sz.cols >= 4 ? 36.0f : 32.0f);
            } else if (sz.cols == 2) {
                draw_vinyl(14.0f, 14.0f, 70.0f);
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(94.0f, 18.0f, tw - 14.0f, 38.0f), 9.0f, 9.0f), Brush(dc, mc, 0.22f));
                dc->DrawText(L"正在播放", 4, fmt_small_center_.Get(), D2D1::RectF(94.0f, 18.0f, tw - 14.0f, 38.0f), Brush(dc, mc));
                dc->DrawText(L"夜曲 · Nocturne", 13, fmt_body_semi_.Get(), D2D1::RectF(14.0f, 98.0f, tw - 14.0f, 120.0f), Brush(dc, pri));
                dc->DrawText(L"周杰伦 · 网易云音乐", 10, fmt_small_.Get(), D2D1::RectF(14.0f, 120.0f, tw - 14.0f, 138.0f), Brush(dc, sec));
                draw_ctrl_cluster(tw * 0.5f, th - 28.0f, 17.0f, 44.0f);
            } else {
                draw_vinyl(18.0f, 26.0f, 152.0f);
                const float rx = 188.0f;
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rx, 24.0f, rx + 82.0f, 44.0f), 10.0f, 10.0f), Brush(dc, mc, 0.22f));
                dc->DrawText(L"正在播放", 4, fmt_small_center_.Get(), D2D1::RectF(rx, 24.0f, rx + 82.0f, 44.0f), Brush(dc, mc));
                dc->DrawText(L"夜曲 · Nocturne", 13, fmt_title_.Get(), D2D1::RectF(rx, 56.0f, tw - 16.0f, 82.0f), Brush(dc, pri));
                dc->DrawText(L"周杰伦 · 十一月的萧邦", 11, fmt_body_.Get(), D2D1::RectF(rx, 84.0f, tw - 16.0f, 106.0f), Brush(dc, sec));
                draw_ctrl_cluster((rx + tw - 16.0f) * 0.5f, th - 36.0f, 19.0f, 52.0f);
            }
            break;
        }

        case model::TileKind::Countdown: {
            const uint32_t cc = 0xFF6B7BFFu;
            if (sz.cols == 1) {
                dc->DrawText(L"2027 元旦", 7, fmt_small_center_.Get(), D2D1::RectF(6.0f, 10.0f, 94.0f, 26.0f), Brush(dc, cc));
                dc->DrawText(L"36", 2, fmt_gallery_big_.Get(), D2D1::RectF(6.0f, 24.0f, 94.0f, 68.0f), Brush(dc, pri));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(14.0f, 72.0f, 86.0f, 90.0f), 9.0f, 9.0f), Brush(dc, cc, 0.22f));
                dc->DrawText(L"还有 36 天", 7, fmt_small_center_.Get(), D2D1::RectF(14.0f, 72.0f, 86.0f, 90.0f), Brush(dc, pri));
            } else if (sz.rows == 1) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(12.0f, 16.0f, 74.0f, th - 16.0f), 12.0f, 12.0f), Brush(dc, cc, 0.22f));
                dc->DrawText(L"36天", 3, fmt_body_semi_.Get(), D2D1::RectF(12.0f, 26.0f, 74.0f, th - 26.0f), Brush(dc, cc));
                dc->DrawText(L"2027 元旦", 7, fmt_body_semi_.Get(), D2D1::RectF(84.0f, 20.0f, tw - 12.0f, 40.0f), Brush(dc, pri));
                dc->DrawText(L"2027-01-01 · 今年 90%", 18, fmt_small_.Get(), D2D1::RectF(84.0f, 42.0f, tw - 12.0f, 60.0f), Brush(dc, sec));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(84.0f, 68.0f, tw - 16.0f, 74.0f), 3.0f, 3.0f), Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x14000000u));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(84.0f, 68.0f, 84.0f + (tw - 100.0f) * 0.90f, 74.0f), 3.0f, 3.0f), Brush(dc, cc));
            } else if (sz.cols == 2) {
                dc->DrawText(L"2027 元旦", 7, fmt_body_semi_.Get(), D2D1::RectF(14.0f, 12.0f, 120.0f, 30.0f), Brush(dc, cc));
                dc->DrawText(L"2027-01-01", 10, fmt_small_right_.Get(), D2D1::RectF(108.0f, 13.0f, tw - 14.0f, 29.0f), Brush(dc, sec));
                dc->DrawText(L"36 天", 4, fmt_gallery_big_.Get(), D2D1::RectF(14.0f, 44.0f, tw - 14.0f, 102.0f), Brush(dc, pri));
                dc->DrawText(L"今日 62%  ·  今年 90%", 15, fmt_small_.Get(), D2D1::RectF(14.0f, th - 48.0f, tw - 14.0f, th - 30.0f), Brush(dc, sec));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(14.0f, th - 22.0f, tw - 14.0f, th - 15.0f), 3.5f, 3.5f), Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x14000000u));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(14.0f, th - 22.0f, 14.0f + (tw - 28.0f) * 0.90f, th - 15.0f), 3.5f, 3.5f), Brush(dc, cc));
            } else {
                dc->DrawText(L"2027 元旦", 7, fmt_body_semi_.Get(), D2D1::RectF(16.0f, 16.0f, 180.0f, 36.0f), Brush(dc, cc));
                dc->DrawText(L"36", 2, fmt_gallery_big_.Get(), D2D1::RectF(16.0f, 52.0f, 160.0f, 114.0f), Brush(dc, pri));
                dc->DrawText(L"还有 36 天 · 2027-01-01", 19, fmt_small_.Get(), D2D1::RectF(16.0f, 128.0f, 184.0f, 148.0f), Brush(dc, sec));
                struct TBar { const wchar_t* lbl; float pct; uint32_t col; const wchar_t* val; };
                static constexpr TBar kT[4] = {
                    {L"今日", 0.62f, 0xFF33B8FFu, L"62%"}, {L"本周", 0.43f, 0xFF38DB70u, L"43%"},
                    {L"本月", 0.84f, 0xFFFFA629u, L"84%"}, {L"今年", 0.90f, 0xFF9F7BFFu, L"90%"},
                };
                for (int bi = 0; bi < 4; ++bi) {
                    const float by = 34.0f + bi * 40.0f;
                    dc->DrawText(kT[bi].lbl, 2, fmt_small_.Get(), D2D1::RectF(204.0f, by, 238.0f, by + 20.0f), Brush(dc, sec));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(242.0f, by + 7.0f, tw - 54.0f, by + 13.0f), 3.0f, 3.0f), Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x14000000u));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(242.0f, by + 7.0f, 242.0f + (tw - 296.0f) * kT[bi].pct, by + 13.0f), 3.0f, 3.0f), Brush(dc, kT[bi].col));
                    dc->DrawText(kT[bi].val, 3, fmt_small_right_.Get(), D2D1::RectF(tw - 50.0f, by, tw - 14.0f, by + 20.0f), Brush(dc, pri));
                }
            }
            break;
        }

        case model::TileKind::Battery: {
            const uint32_t bc = 0xFF30D158u;
            if (sz.cols == 1) {
                draw_ring(50.0f, 38.0f, 22.0f, 0.86f, bc);
                dc->DrawText(L"86%", 3, fmt_small_center_.Get(), D2D1::RectF(26.0f, 28.0f, 74.0f, 48.0f), Brush(dc, pri));
                dc->DrawText(L"充电中", 3, fmt_small_center_.Get(), D2D1::RectF(4.0f, 70.0f, 96.0f, 90.0f), Brush(dc, bc));
            } else if (sz.rows == 1) {
                draw_ring(36.0f, th * 0.5f, 20.0f, 0.86f, bc);
                dc->DrawText(L"86%", 3, fmt_small_center_.Get(), D2D1::RectF(14.0f, th * 0.5f - 9.0f, 58.0f, th * 0.5f + 9.0f), Brush(dc, pri));
                dc->DrawText(L"本机电源 · 充电中", 9, fmt_body_semi_.Get(), D2D1::RectF(66.0f, th * 0.5f - 20.0f, tw - 12.0f, th * 0.5f), Brush(dc, pri));
                dc->DrawText(L"平衡模式 · 已运行 4h 22m", 14, fmt_small_.Get(), D2D1::RectF(66.0f, th * 0.5f + 1.0f, tw - 12.0f, th * 0.5f + 19.0f), Brush(dc, sec));
            } else if (sz.cols == 2) {
                struct Dev { const wchar_t* n; float r; uint32_t c; const wchar_t* p; };
                static constexpr Dev kD[4] = {
                    {L"本机", 0.86f, 0xFF30D158u, L"86"}, {L"耳机", 0.72f, 0xFF33B8FFu, L"72"},
                    {L"鼠标", 0.54f, 0xFFFFA629u, L"54"}, {L"键盘", 0.91f, 0xFF30D158u, L"91"},
                };
                for (int di = 0; di < 4; ++di) {
                    const float cx = (di % 2 == 0) ? 56.0f : (tw - 56.0f);
                    const float cy = (di / 2 == 0) ? 54.0f : (th - 58.0f);
                    draw_ring(cx, cy, 24.0f, kD[di].r, kD[di].c);
                    dc->DrawText(kD[di].p, 2, fmt_small_center_.Get(), D2D1::RectF(cx - 20.0f, cy - 9.0f, cx + 20.0f, cy + 9.0f), Brush(dc, pri));
                    dc->DrawText(kD[di].n, 2, fmt_small_center_.Get(), D2D1::RectF(cx - 34.0f, cy + 28.0f, cx + 34.0f, cy + 44.0f), Brush(dc, sec));
                }
            } else {
                draw_ring(88.0f, th * 0.5f - 10.0f, 38.0f, 0.86f, bc);
                dc->DrawText(L"86%", 3, fmt_body_semi_.Get(), D2D1::RectF(48.0f, th * 0.5f - 22.0f, 128.0f, th * 0.5f + 2.0f), Brush(dc, pri));
                dc->DrawText(L"本机 · 平衡模式", 8, fmt_small_center_.Get(), D2D1::RectF(16.0f, th - 48.0f, 160.0f, th - 26.0f), Brush(dc, sec));
                struct Row { const wchar_t* n; float r; uint32_t c; const wchar_t* p; };
                static constexpr Row kR[3] = {
                    {L"AirPods Pro", 0.72f, 0xFF33B8FFu, L"72%"},
                    {L"MX Master",   0.54f, 0xFFFFA629u, L"54%"},
                    {L"机械键盘",     0.91f, 0xFF30D158u, L"91%"},
                };
                for (int ri = 0; ri < 3; ++ri) {
                    const float ry = 42.0f + ri * 46.0f;
                    dc->DrawText(kR[ri].n, static_cast<UINT32>(std::wcslen(kR[ri].n)), fmt_small_.Get(), D2D1::RectF(192.0f, ry, 280.0f, ry + 18.0f), Brush(dc, pri));
                    dc->DrawText(kR[ri].p, 3, fmt_small_right_.Get(), D2D1::RectF(tw - 52.0f, ry, tw - 16.0f, ry + 18.0f), Brush(dc, sec));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(192.0f, ry + 22.0f, tw - 16.0f, ry + 28.0f), 3.0f, 3.0f), Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x14000000u));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(192.0f, ry + 22.0f, 192.0f + (tw - 208.0f) * kR[ri].r, ry + 28.0f), 3.0f, 3.0f), Brush(dc, kR[ri].c));
                }
            }
            break;
        }

        case model::TileKind::DiskRing:
        default: {
            draw_ring(50.0f, 38.0f, 22.0f, 0.82f, hue);
            dc->DrawText(L"82%", 3, fmt_small_center_.Get(), D2D1::RectF(28.0f, 28.0f, 72.0f, 48.0f), Brush(dc, pri));
            dc->DrawText(L"C: 剩 84 GB", 10, fmt_small_center_.Get(), D2D1::RectF(4.0f, 70.0f, 96.0f, 90.0f), Brush(dc, pri));
            break;
        }
        }

        StrokeCrispRoundedRect(dc, tr, 16.0f, Brush(dc, dark_mode_ ? 0x2AFFFFFFu : 0x1A000000u), 1.0f);
        }  // !drew_real
        dc->SetTransform(saved_tf);
    }
    dc->PopAxisAlignedClip();

    // 4. Clickable pagination dots (.gal-dots)
    const auto& sel_def = FindGalleryWidgetDef(gallery_selected_kind_);
    const size_t n_sizes = sel_def.sizes.size();
    {
        const float total_dots_w = static_cast<float>(n_sizes) * 6.0f + static_cast<float>(n_sizes > 0 ? n_sizes - 1 : 0) * 6.0f;
        float dx = ox + (w - total_dots_w) * 0.5f;
        for (const auto& sz : sel_def.sizes) {
            const std::string sz_act = "wg-sz:" + std::to_string(sz.cols) + "x" + std::to_string(sz.rows);
            click_regions_.push_back({sz_act, RECT{static_cast<LONG>(dx - ox - 3.0f), static_cast<LONG>(dots_y - 3.0f),
                                                   static_cast<LONG>(dx - ox + 9.0f), static_cast<LONG>(dots_y + 9.0f)}});
            const bool active_dot = (sz == gallery_selected_size_);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dx + 3.0f, oy + dots_y + 3.0f), 3.0f, 3.0f),
                            Brush(dc, active_dot ? accent_argb_ : sec, active_dot ? 1.0f : 0.36f));
            dx += 12.0f;
        }
    }

    // 5. Selected widget title + description (.gal-desc)
    dc->DrawText(sel_def.name, static_cast<UINT32>(std::wcslen(sel_def.name)), fmt_body_semi_.Get(),
                 D2D1::RectF(ox + 16.0f, oy + desc_top, ox + w - 16.0f, oy + desc_top + 19.0f), Brush(dc, pri));
    const wchar_t* wdesc = WidgetDesc(gallery_selected_kind_);
    dc->DrawText(wdesc, static_cast<UINT32>(std::wcslen(wdesc)), fmt_small_.Get(),
                 D2D1::RectF(ox + 16.0f, oy + desc_top + 19.0f, ox + w - 16.0f, oy + desc_top + 38.0f), Brush(dc, sec));

    // 6. Size row (.dp-line: 大小 + .dseg)
    dc->DrawText(L"大小", 2, fmt_body_.Get(),
                 D2D1::RectF(ox + 16.0f, oy + size_row_top, ox + 72.0f, oy + size_row_top + 28.0f), Brush(dc, pri));
    gallery_segments_.clear();
    {
        const float seg_btn_w = n_sizes >= 5 ? 62.0f : n_sizes >= 4 ? 68.0f : 72.0f;
        const float seg_total_w = seg_btn_w * static_cast<float>(n_sizes) + 4.0f;
        const float seg_left = w - 16.0f - seg_total_w;
        const D2D1_RECT_F track = SnapRectF(D2D1::RectF(ox + seg_left, oy + size_row_top - 2.0f, ox + w - 16.0f, oy + size_row_top + 30.0f));
        // Pulse's own segmented control (PaintPulseControls) when its UI device is up; this drawing only as a fallback.
        const bool pulse_control = dc == d2d_dc_.Get() && EnsureUiCompositor();
        gallery_segment_track_ = track;
        if (!pulse_control) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(track, 7.0f, 7.0f),
                                     Brush(dc, dark_mode_ ? 0x18FFFFFFu : 0x0E000000u));
            StrokeCrispRoundedRect(dc, track, 7.0f, Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x12000000u), 1.0f);
        }
        float bx = seg_left + 2.0f;
        for (const auto& sz : sel_def.sizes) {
            const std::string sz_act = "wg-sz:" + std::to_string(sz.cols) + "x" + std::to_string(sz.rows);
            const D2D1_RECT_F brc = SnapRectF(D2D1::RectF(ox + bx, oy + size_row_top + 2.0f, ox + bx + seg_btn_w, oy + size_row_top + 26.0f));
            click_regions_.push_back({sz_act, RECT{static_cast<LONG>(bx), static_cast<LONG>(size_row_top),
                                                   static_cast<LONG>(bx + seg_btn_w), static_cast<LONG>(size_row_top + 28.0f)}});
            const bool on = (sz == gallery_selected_size_);
            if (pulse_control) {
                const float seg_cell_w = (track.right - track.left) / static_cast<float>(n_sizes);
                const float cl = track.left + seg_cell_w * static_cast<float>(gallery_segments_.size());
                gallery_segments_.push_back({D2D1::RectF(cl, track.top, cl + seg_cell_w, track.bottom),
                                             n_sizes >= 5 ? SizeDims(sz) : std::wstring(SizeShortLabel(sz)), on,
                                             hovered_action_id_ == sz_act});
                // Hit the cell Pulse's control draws (card coordinates, like the other regions).
                click_regions_.back().bounds = RECT{static_cast<LONG>(cl - ox), static_cast<LONG>(track.top - oy),
                                                static_cast<LONG>(cl + seg_cell_w - ox), static_cast<LONG>(track.bottom - oy)};
                bx += seg_btn_w;
                continue;
            }
            if (on) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(brc, 5.5f, 5.5f),
                                         Brush(dc, dark_mode_ ? 0x2EFFFFFFu : 0xFFFFFFFFu));
                StrokeCrispRoundedRect(dc, brc, 5.5f, Brush(dc, accent_argb_, 0.65f), 1.0f);
            }
            const wchar_t* slbl = SizeShortLabel(sz);
            dc->DrawText(slbl, static_cast<UINT32>(std::wcslen(slbl)), fmt_small_center_.Get(), brc,
                         Brush(dc, on ? pri : sec));
            bx += seg_btn_w;
        }
    }

    // 7. Footer (.dp-f): placement hint + [添加小组件] primary button
    dc->FillRectangle(D2D1::RectF(ox + 1.0f, oy + footer_top, ox + w - 1.0f, oy + footer_top + 1.0f),
                      Brush(dc, dark_mode_ ? 0x1AFFFFFFu : 0x12000000u));

    const bool already_placed = !gallery_tiles_.empty()
                                    ? model::BoardHasKind(gallery_tiles_, gallery_selected_kind_)
                                    : active_widget_kinds_.contains(static_cast<uint8_t>(gallery_selected_kind_));
    std::optional<model::PointI> spot;
    if (!already_placed) {
        if (gallery_target_cell_.has_value()) {
            const int tc = gallery_target_cell_->x;
            const int tr_row = gallery_target_cell_->y;
            if (tc >= 0 && tr_row >= 0 &&
                tc + gallery_selected_size_.cols <= model::kTileCols &&
                tr_row + gallery_selected_size_.rows <= model::kTileRows) {
                const model::RectI cand{tc, tr_row, gallery_selected_size_.cols, gallery_selected_size_.rows};
                bool free_here = true;
                for (const auto& t : gallery_tiles_) {
                    if (cand.intersects(model::RectI{t.col, t.row, t.size.cols, t.size.rows}, 0)) {
                        free_here = false;
                        break;
                    }
                }
                if (free_here) spot = *gallery_target_cell_;
            }
        }
        if (!spot.has_value() && !gallery_tiles_.empty()) {
            spot = model::FindFreeTileCell(gallery_tiles_, gallery_selected_size_);
        } else if (!spot.has_value() && gallery_tiles_.empty()) {
            spot = model::PointI{0, 0};
        }
    }
    const model::DesktopTile* stack_target = nullptr;
    if (!already_placed && !spot.has_value()) {
        for (const auto& t : gallery_tiles_) {
            if (t.size == gallery_selected_size_ && t.stack.size() < 6) {
                stack_target = &t;
                break;
            }
        }
    }

    std::wstring where_txt;
    if (already_placed) {
        where_txt = L"已经在桌面里了";
    } else if (spot.has_value()) {
        wchar_t wb[64]{};
        std::swprintf(wb, ARRAYSIZE(wb), L"放到第 %d 行第 %d 格（可全桌面自由拖动缩放）", spot->y + 1, spot->x + 1);
        where_txt = wb;
    } else if (stack_target != nullptr) {
        const auto tv = model::ActiveTileView(*stack_target);
        const std::wstring tname = tv.title.empty() ? std::wstring(WidgetName(tv.kind)) : tv.title;
        where_txt = L"放到桌面空闲位置，或拖到「" + tname + L"」上叠放";
    } else {
        where_txt = L"放到桌面空闲位置（可自由拖动与缩放）";
    }

    dc->DrawText(where_txt.c_str(), static_cast<UINT32>(where_txt.size()), fmt_small_.Get(),
                 D2D1::RectF(ox + 16.0f, oy + footer_top, ox + w - 116.0f, oy + h), Brush(dc, sec));

    const bool can_add = !already_placed;
    PulseButton(dc, ox, oy, D2D1::RectF(w - 120.0f, footer_top + 6.0f, w - 16.0f, footer_top + 38.0f), L"添加小组件", "wg-add",
                true, can_add);
}

void DesktopOverlayController::DrawAiOrganizeDrawer(ID2D1DeviceContext* dc, float ox, float oy) {
    const uint32_t pri = dark_mode_ ? 0xF8FFFFFFu : 0xF8181E28u;
    const uint32_t sec = dark_mode_ ? 0x92FFFFFFu : 0x92303846u;
    const float w = static_cast<float>(card_w_);
    const float h = static_cast<float>(card_h_);

    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);

    // 1. Top Header Bar: ✦ AI 智能收纳 + Connection Status Capsule + × Close
    DrawSparkleStar(fac.Get(), dc, ox + 24.0f, oy + 27.0f, 8.2f, Brush(dc, accent_argb_));
    dc->DrawText(L"AI 智能收纳", 7, fmt_title_.Get(),
                 D2D1::RectF(ox + 38.0f, oy + 12.0f, ox + 152.0f, oy + 42.0f), Brush(dc, pri));

    // Connection status capsule [x..w-46] -> clickable to jump to pulse:settings:assistant
    const bool conn_ok = ai_state_.connection.configured;
    const std::wstring conn_txt = conn_ok
                                      ? ai_state_.connection.badge_text()
                                      : std::wstring(L"未连接 · 去设置");
    const float conn_txt_w = std::min(168.0f, MeasureTextW(conn_txt, fmt_small_.Get(), 175.0f));
    const float pill_w = std::clamp(conn_txt_w + 26.0f, 96.0f, 196.0f);
    const float pill_right = w - 46.0f;
    const float pill_left = std::max(148.0f, pill_right - pill_w);
    const D2D1_RECT_F conn_rc = D2D1::RectF(ox + pill_left, oy + 15.0f, ox + pill_right, oy + 39.0f);
    click_regions_.push_back({"ai-conn-settings",
                              RECT{static_cast<LONG>(pill_left), 15, static_cast<LONG>(pill_right), 39}});
    const uint32_t pill_col = conn_ok ? 0xFF30A46Cu : 0xFFF5A524u;
    dc->FillRoundedRectangle(D2D1::RoundedRect(conn_rc, 12.0f, 12.0f),
                             Brush(dc, pill_col, hovered_action_id_ == "ai-conn-settings" ? 0.28f : 0.16f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(conn_rc, 12.0f, 12.0f),
                             Brush(dc, pill_col, 0.50f), 1.0f);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(conn_rc.left + 11.0f, (conn_rc.top + conn_rc.bottom) * 0.5f), 3.2f, 3.2f),
                    Brush(dc, pill_col));
    dc->DrawText(conn_txt.c_str(), static_cast<UINT32>(conn_txt.size()), fmt_small_.Get(),
                 D2D1::RectF(conn_rc.left + 19.0f, conn_rc.top, conn_rc.right - 7.0f, conn_rc.bottom),
                 Brush(dc, conn_ok ? (dark_mode_ ? 0xFFD9FCE8u : 0xFF166534u)
                                   : (dark_mode_ ? 0xFFFFDFA0u : 0xFF9A5B00u)));

    const D2D1_RECT_F cls_rc = D2D1::RectF(ox + w - 42.0f, oy + 13.0f, ox + w - 14.0f, oy + 41.0f);
    click_regions_.push_back({"close-overlay", RECT{static_cast<LONG>(w - 42), 13, static_cast<LONG>(w - 14), 41}});
    const wchar_t x_g[2] = {L'\xE711', 0};
    DrawGlyph(dc, x_g, 1, fmt_fluent_12_.Get(), cls_rc, Brush(dc, sec));

    // Phase 4: Completed View
    if (ai_state_.phase == AiDrawerPhase::Completed) {
        const float cx = ox + w * 0.5f;
        const float cy = oy + h * 0.38f;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 30.0f, 30.0f), Brush(dc, 0xFF30A46Cu, 0.18f));
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 30.0f, 30.0f), Brush(dc, 0xFF30A46Cu, 0.45f), 1.2f);
        const wchar_t chk[2] = {L'\xE73E', 0};
        DrawGlyph(dc, chk, 1, fmt_fluent_20_.Get(),
                     D2D1::RectF(cx - 24.0f, cy - 24.0f, cx + 24.0f, cy + 24.0f), Brush(dc, 0xFF30A46Cu));

        const wchar_t* done_title = L"桌面收纳已完成";
        dc->DrawText(done_title, static_cast<UINT32>(std::wcslen(done_title)), fmt_stat_num_.Get(),
                     D2D1::RectF(ox + 24.0f, cy + 42.0f, ox + w - 24.0f, cy + 70.0f), Brush(dc, pri));

        wchar_t done_sub[112]{};
        std::swprintf(done_sub, ARRAYSIZE(done_sub), L"已将 %zu 个文件归档至 %zu 个桌面子目录",
                      ai_state_.completed_moved_count, ai_state_.completed_folder_count);
        dc->DrawText(done_sub, static_cast<UINT32>(std::wcslen(done_sub)), fmt_small_center_.Get(),
                     D2D1::RectF(ox + 20.0f, cy + 72.0f, ox + w - 20.0f, cy + 96.0f), Brush(dc, sec));

        const float by = cy + 116.0f;
        const float b_left = (w - 340.0f) * 0.5f;
        const float byr = by - oy;
        PulseButton(dc, ox, oy, D2D1::RectF(b_left, byr, b_left + 96.0f, byr + 32.0f), L"撤销归档", "ai-undo-archive", false);
        PulseButton(dc, ox, oy, D2D1::RectF(b_left + 106.0f, byr, b_left + 238.0f, byr + 32.0f), L"在 Pulse 中查看",
                    "ai-open-desktop-pulse", false);
        PulseButton(dc, ox, oy, D2D1::RectF(b_left + 248.0f, byr, b_left + 340.0f, byr + 32.0f), L"完成", "close-overlay", true);
        return;
    }

    // 2. Strategy 3-Segment Switcher + Custom Instruction Input Bar
    const D2D1_RECT_F seg_track = D2D1::RectF(ox + 16.0f, oy + 48.0f, ox + w - 16.0f, oy + 80.0f);
    dc->FillRoundedRectangle(D2D1::RoundedRect(seg_track, 8.0f, 8.0f),
                             Brush(dc, dark_mode_ ? 0x16FFFFFFu : 0x0E000000u));
    struct StratOpt { const char* id; const wchar_t* label; ai::AiOrganizeStrategy val; };
    static constexpr StratOpt kStrats[3] = {
        {"ai-strat:0", L"归档散落旧件", ai::AiOrganizeStrategy::ArchiveClutter},
        {"ai-strat:1", L"按项目主题",   ai::AiOrganizeStrategy::ByProjectTopic},
        {"ai-strat:2", L"全盘收纳",     ai::AiOrganizeStrategy::FullDesktop},
    };
    const float seg_w = (w - 32.0f) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const float sx = 16.0f + seg_w * static_cast<float>(i);
        const D2D1_RECT_F src = D2D1::RectF(ox + sx + 2.0f, oy + 50.0f, ox + sx + seg_w - 2.0f, oy + 78.0f);
        click_regions_.push_back({kStrats[i].id,
                                  RECT{static_cast<LONG>(sx), 48, static_cast<LONG>(sx + seg_w), 80}});
        const bool active = (ai_state_.strategy == kStrats[i].val);
        if (active) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(src, 6.0f, 6.0f), Brush(dc, accent_argb_, 0.26f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(src, 6.0f, 6.0f), Brush(dc, accent_argb_, 0.65f), 1.0f);
        }
        dc->DrawText(kStrats[i].label, static_cast<UINT32>(std::wcslen(kStrats[i].label)),
                     fmt_small_center_.Get(), src, Brush(dc, active ? accent_argb_ : pri));
    }

    // Custom instruction rounded input box + ↻ Regenerate button
    const D2D1_RECT_F prompt_rc = D2D1::RectF(ox + 16.0f, oy + 86.0f, ox + w - 52.0f, oy + 118.0f);
    const bool editing_prompt = (popup_edit_hwnd_ != nullptr && IsWindow(popup_edit_hwnd_));
    click_regions_.push_back({"ai-edit-prompt", RECT{16, 86, static_cast<LONG>(w - 52), 118}});
    dc->FillRoundedRectangle(D2D1::RoundedRect(prompt_rc, 8.0f, 8.0f),
                             Brush(dc, dark_mode_ ? 0x22000000u : 0x0A000000u));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(prompt_rc, 8.0f, 8.0f),
                             Brush(dc, (editing_prompt || hovered_action_id_ == "ai-edit-prompt")
                                           ? accent_argb_
                                           : (dark_mode_ ? 0x24FFFFFFu : 0x18000000u)),
                             editing_prompt ? 1.4f : 1.0f);
    const wchar_t edit_g[2] = {L'\xE70F', 0};
    DrawGlyph(dc, edit_g, 1, fmt_fluent_12_.Get(),
                 D2D1::RectF(prompt_rc.left + 6.0f, prompt_rc.top, prompt_rc.left + 26.0f, prompt_rc.bottom),
                 Brush(dc, (editing_prompt || !ai_state_.custom_instruction.empty()) ? accent_argb_ : sec));
    if (!(dc == d2d_dc_.Get() && editing_prompt)) {
        const std::wstring prompt_txt = ai_state_.custom_instruction.empty()
                                            ? std::wstring(L"输入补充要求（如：把合同和发票单独放一类）…")
                                            : ai_state_.custom_instruction;
        dc->DrawText(prompt_txt.c_str(), static_cast<UINT32>(prompt_txt.size()), fmt_small_.Get(),
                     D2D1::RectF(prompt_rc.left + 27.0f, prompt_rc.top, prompt_rc.right - 8.0f, prompt_rc.bottom),
                     Brush(dc, ai_state_.custom_instruction.empty() ? sec : pri));
    }

    const D2D1_RECT_F regen_rc = D2D1::RectF(ox + w - 46.0f, oy + 86.0f, ox + w - 16.0f, oy + 118.0f);
    click_regions_.push_back({"ai-regenerate", RECT{static_cast<LONG>(w - 46), 86, static_cast<LONG>(w - 16), 118}});
    dc->FillRoundedRectangle(D2D1::RoundedRect(regen_rc, 8.0f, 8.0f),
                             Brush(dc, hovered_action_id_ == "ai-regenerate"
                                           ? (dark_mode_ ? 0x28FFFFFFu : 0x18000000u)
                                           : (dark_mode_ ? 0x16FFFFFFu : 0x0E000000u)));
    const wchar_t ref_g[2] = {L'\xE72C', 0};
    DrawGlyph(dc, ref_g, 1, fmt_fluent_12_.Get(), regen_rc, Brush(dc, accent_argb_));

    // Phase 1: Unconfigured Guide View
    if (ai_state_.phase == AiDrawerPhase::UnconfiguredGuide) {
        const D2D1_RECT_F gcard = D2D1::RectF(ox + 16.0f, oy + 136.0f, ox + w - 16.0f, oy + 412.0f);
        dc->FillRoundedRectangle(D2D1::RoundedRect(gcard, 12.0f, 12.0f),
                                 Brush(dc, dark_mode_ ? 0x16FFFFFFu : 0x0A000000u));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(gcard, 12.0f, 12.0f),
                                 Brush(dc, accent_argb_, 0.35f), 1.0f);

        const float gcx = (gcard.left + gcard.right) * 0.5f;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(gcx, gcard.top + 44.0f), 24.0f, 24.0f),
                        Brush(dc, accent_argb_, 0.18f));
        DrawSparkleStar(fac.Get(), dc, gcx, gcard.top + 44.0f, 12.0f, Brush(dc, accent_argb_));

        const wchar_t* gtitle = L"复用 Pulse AI 助手，智能规划归档";
        dc->DrawText(gtitle, static_cast<UINT32>(std::wcslen(gtitle)), fmt_small_center_.Get(),
                     D2D1::RectF(gcard.left + 16.0f, gcard.top + 78.0f, gcard.right - 16.0f, gcard.top + 102.0f),
                     Brush(dc, pri));

        ComPtr<IDWriteTextFormat> wrap_fmt;
        if (dwrite_) {
            dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      11.5f, L"zh-CN", &wrap_fmt);
            if (wrap_fmt) {
                wrap_fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                wrap_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                wrap_fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 18.0f, 14.0f);
            }
        }
        const wchar_t* gdesc =
            L"直接复用 Pulse「AI 助手」中已连接的模型服务（DeepSeek / 通义千问 / Kimi / Ollama 等）"
            L"读取桌面文件信息，智能规划桌面归档子目录，全程可预览勾选、一键无损撤销。";
        dc->DrawText(gdesc, static_cast<UINT32>(std::wcslen(gdesc)),
                     wrap_fmt ? wrap_fmt.Get() : fmt_small_center_.Get(),
                     D2D1::RectF(gcard.left + 20.0f, gcard.top + 108.0f, gcard.right - 20.0f, gcard.top + 180.0f),
                     Brush(dc, sec));

        const D2D1_RECT_F btn_pri = D2D1::RectF(gcard.left + 24.0f, gcard.top + 186.0f, gcard.right - 24.0f, gcard.top + 222.0f);
        const D2D1_RECT_F btn_sec = D2D1::RectF(gcard.left + 24.0f, gcard.top + 230.0f, gcard.right - 24.0f, gcard.top + 264.0f);
        click_regions_.push_back({"ai-open-settings", RECT{40, 322, static_cast<LONG>(w - 40), 358}});
        click_regions_.push_back({"ai-local-preview", RECT{40, 366, static_cast<LONG>(w - 40), 400}});

        dc->FillRoundedRectangle(D2D1::RoundedRect(btn_pri, 8.0f, 8.0f), Brush(dc, accent_argb_));
        dc->DrawText(L"去 Pulse 连接 AI 助手", 13, fmt_small_center_.Get(), btn_pri,
                     Brush(dc, dark_mode_ ? 0xFF0A121Eu : 0xFFFFFFFFu));

        dc->FillRoundedRectangle(D2D1::RoundedRect(btn_sec, 8.0f, 8.0f),
                                 Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x12000000u));
        dc->DrawText(L"先用本地智能规则预览", 10, fmt_small_center_.Get(), btn_sec, Brush(dc, pri));
        return;
    }

    // Phase 2: Loading View
    if (ai_state_.phase == AiDrawerPhase::Loading) {
        const D2D1_RECT_F lcard = D2D1::RectF(ox + 16.0f, oy + 148.0f, ox + w - 16.0f, oy + 368.0f);
        dc->FillRoundedRectangle(D2D1::RoundedRect(lcard, 12.0f, 12.0f),
                                 Brush(dc, dark_mode_ ? 0x14FFFFFFu : 0x0A000000u));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(lcard, 12.0f, 12.0f),
                                 Brush(dc, accent_argb_, 0.30f), 1.0f);

        const float lcx = (lcard.left + lcard.right) * 0.5f;
        const float lcy = lcard.top + 54.0f;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(lcx, lcy), 30.0f, 30.0f), Brush(dc, accent_argb_, 0.10f));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(lcx, lcy), 20.0f, 20.0f), Brush(dc, accent_argb_, 0.22f));
        DrawSparkleStar(fac.Get(), dc, lcx, lcy, 11.0f, Brush(dc, accent_argb_));

        const wchar_t* lmsg = L"正在读取桌面文件特征并生成归档建议…";
        dc->DrawText(lmsg, static_cast<UINT32>(std::wcslen(lmsg)), fmt_small_center_.Get(),
                     D2D1::RectF(lcard.left + 16.0f, lcard.top + 96.0f, lcard.right - 16.0f, lcard.top + 120.0f),
                     Brush(dc, pri));

        const std::wstring mbadge = conn_ok ? ai_state_.connection.badge_text() : std::wstring(L"本地智能规则引擎");
        dc->DrawText(mbadge.c_str(), static_cast<UINT32>(mbadge.size()), fmt_small_center_.Get(),
                     D2D1::RectF(lcard.left + 20.0f, lcard.top + 122.0f, lcard.right - 20.0f, lcard.top + 144.0f),
                     Brush(dc, accent_argb_));

        PulseButton(dc, ox, oy, D2D1::RectF(lcx - ox - 56.0f, lcard.top - oy + 160.0f, lcx - ox + 56.0f, lcard.top - oy + 192.0f),
                    L"取消", "ai-cancel", false);
        return;
    }

    // Phase 3: PlanReady View (3-Column Stats + Summary + Scrollable Folder Cards + Pin + Footer)
    const size_t before_cnt = ai_state_.plan.desktop_before_count;
    const size_t after_cnt = ai_state_.plan.DesktopAfterCount();
    const size_t folder_cnt = ai_state_.plan.ActiveFolderCount();
    const size_t new_folder_cnt = ai_state_.plan.NewFolderCount();
    const size_t sel_cnt = ai_state_.plan.SelectedItemCount();
    const uint64_t sel_bytes = ai_state_.plan.SelectedTotalBytes();

    const float stat_w = (w - 48.0f) / 3.0f;
    wchar_t st0[32]{}, st1[32]{}, st2[32]{}, sub1[32]{};
    std::swprintf(st0, ARRAYSIZE(st0), L"%zu → %zu", before_cnt, after_cnt);
    std::swprintf(st1, ARRAYSIZE(st1), L"%zu", folder_cnt);
    std::swprintf(st2, ARRAYSIZE(st2), L"%zu", sel_cnt);
    if (new_folder_cnt > 0) {
        std::swprintf(sub1, ARRAYSIZE(sub1), L"新建 %zu · 已有 %zu",
                      new_folder_cnt, folder_cnt >= new_folder_cnt ? (folder_cnt - new_folder_cnt) : 0u);
    } else {
        std::swprintf(sub1, ARRAYSIZE(sub1), L"复用已有目录");
    }
    const std::wstring sub2 = FormatSizeShort(sel_bytes);

    const wchar_t* s_vals[3] = {st0, st1, st2};
    const wchar_t* s_lbls[3] = {L"桌面顶层项", L"目标归档目录", L"已勾选文件"};
    const wchar_t* s_subs[3] = {
        ai_state_.scope_group_title.empty() ? L"收纳后更清爽" : ai_state_.scope_group_title.c_str(),
        sub1,
        sub2.c_str(),
    };

    for (int c = 0; c < 3; ++c) {
        const float sx = ox + 16.0f + static_cast<float>(c) * (stat_w + 8.0f);
        const D2D1_RECT_F src = D2D1::RectF(sx, oy + 126.0f, sx + stat_w, oy + 192.0f);
        dc->FillRoundedRectangle(D2D1::RoundedRect(src, 10.0f, 10.0f),
                                 Brush(dc, dark_mode_ ? 0x16FFFFFFu : 0x0C000000u));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(src, 10.0f, 10.0f),
                                 Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x12000000u), 1.0f);
        dc->DrawText(s_vals[c], static_cast<UINT32>(std::wcslen(s_vals[c])), fmt_stat_num_.Get(),
                     D2D1::RectF(src.left + 4.0f, src.top + 5.0f, src.right - 4.0f, src.top + 31.0f),
                     Brush(dc, c == 1 ? accent_argb_ : pri));
        dc->DrawText(s_lbls[c], static_cast<UINT32>(std::wcslen(s_lbls[c])), fmt_small_center_.Get(),
                     D2D1::RectF(src.left + 2.0f, src.top + 30.0f, src.right - 2.0f, src.top + 47.0f),
                     Brush(dc, pri));
        dc->DrawText(s_subs[c], static_cast<UINT32>(std::wcslen(s_subs[c])), fmt_small_center_.Get(),
                     D2D1::RectF(src.left + 2.0f, src.top + 46.0f, src.right - 2.0f, src.bottom - 3.0f),
                     Brush(dc, sec));
    }

    // AI Summary Banner
    const D2D1_RECT_F sum_rc = D2D1::RectF(ox + 16.0f, oy + 198.0f, ox + w - 16.0f, oy + 228.0f);
    dc->FillRoundedRectangle(D2D1::RoundedRect(sum_rc, 8.0f, 8.0f), Brush(dc, accent_argb_, 0.12f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(sum_rc, 8.0f, 8.0f), Brush(dc, accent_argb_, 0.28f), 1.0f);
    DrawSparkleStar(fac.Get(), dc, sum_rc.left + 13.0f, (sum_rc.top + sum_rc.bottom) * 0.5f, 5.5f,
                    Brush(dc, accent_argb_));

    const bool local_preview = !ai_state_.plan.from_ai_model;
    const float sum_right_pad = local_preview ? 66.0f : 10.0f;
    const std::wstring sum_txt = !ai_state_.error_banner.empty()
                                     ? ai_state_.error_banner
                                     : (ai_state_.plan.summary.empty()
                                            ? std::wstring(L"已根据文件类型、时间与语义整理归档目录")
                                            : ai_state_.plan.summary);
    dc->DrawText(sum_txt.c_str(), static_cast<UINT32>(sum_txt.size()), fmt_small_.Get(),
                 D2D1::RectF(sum_rc.left + 24.0f, sum_rc.top, sum_rc.right - sum_right_pad, sum_rc.bottom),
                 Brush(dc, pri));
    if (local_preview) {
        const D2D1_RECT_F lpill = D2D1::RectF(sum_rc.right - 60.0f, sum_rc.top + 5.0f,
                                              sum_rc.right - 6.0f, sum_rc.bottom - 5.0f);
        dc->FillRoundedRectangle(D2D1::RoundedRect(lpill, 5.0f, 5.0f),
                                 Brush(dc, dark_mode_ ? 0x28FFFFFFu : 0x16000000u));
        dc->DrawText(L"本地预览", 4, fmt_small_center_.Get(), lpill, Brush(dc, sec));
    }

    // Scrollable Archive Folder Cards List
    const float list_top = 234.0f;
    const float list_bottom = h - 90.0f;
    const float view_h = std::max(60.0f, list_bottom - list_top);

    // Compute total content height for scroll clamping
    float total_content_h = 0.0f;
    for (const auto& grp : ai_state_.plan.groups) {
        const float hdr_h = grp.reason.empty() ? 30.0f : 46.0f;
        total_content_h += hdr_h + static_cast<float>(grp.items.size()) * 30.0f + 8.0f + 8.0f;
    }
    if (total_content_h > 0.0f) total_content_h -= 8.0f;
    ai_max_scroll_y_ = std::max(0.0f, total_content_h - view_h);
    ai_scroll_y_ = std::clamp(ai_scroll_y_, 0.0f, ai_max_scroll_y_);

    dc->PushAxisAlignedClip(D2D1::RectF(ox, oy + list_top, ox + w, oy + list_bottom),
                            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (ai_state_.plan.groups.empty()) {
        const wchar_t* empty_msg = L"当前桌面非常整洁，暂无需要归档的项目";
        dc->DrawText(empty_msg, static_cast<UINT32>(std::wcslen(empty_msg)), fmt_small_center_.Get(),
                     D2D1::RectF(ox + 24.0f, oy + list_top + 80.0f, ox + w - 24.0f, oy + list_top + 114.0f),
                     Brush(dc, sec));
    } else {
        float y = list_top - ai_scroll_y_;
        for (size_t gi = 0; gi < ai_state_.plan.groups.size(); ++gi) {
            const auto& grp = ai_state_.plan.groups[gi];
            const float hdr_h = grp.reason.empty() ? 30.0f : 46.0f;
            const float card_h_px = hdr_h + static_cast<float>(grp.items.size()) * 30.0f + 8.0f;

            if (y + card_h_px >= list_top - 4.0f && y <= list_bottom + 4.0f) {
                const D2D1_RECT_F grc = D2D1::RectF(ox + 14.0f, oy + y, ox + w - 14.0f, oy + y + card_h_px);
                dc->FillRoundedRectangle(D2D1::RoundedRect(grc, 10.0f, 10.0f),
                                         Brush(dc, dark_mode_ ? 0x12FFFFFFu : 0x08000000u));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(grc, 10.0f, 10.0f),
                                         Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x12000000u), 1.0f);

                // Folder icon + folder_name + [新建目录]/[已有目录] pill + 全选/全不选
                render::DrawVectorFileIcon(fac.Get(), dc, dwrite_.Get(),
                                           D2D1::RectF(grc.left + 10.0f, grc.top + 6.0f, grc.left + 28.0f, grc.top + 24.0f),
                                           L"", true);
                const float max_name_w = (grc.right - grc.left) - 166.0f;
                const float name_w = std::min(max_name_w, MeasureTextW(grp.folder_name, fmt_body_semi_.Get(), max_name_w));
                dc->DrawText(grp.folder_name.c_str(), static_cast<UINT32>(grp.folder_name.size()),
                             fmt_body_semi_.Get(),
                             D2D1::RectF(grc.left + 34.0f, grc.top + 4.0f, grc.left + 34.0f + max_name_w, grc.top + 26.0f),
                             Brush(dc, pri));

                const float tag_x = grc.left + 34.0f + name_w + 6.0f;
                const D2D1_RECT_F tag_rc = D2D1::RectF(tag_x, grc.top + 6.0f, tag_x + 56.0f, grc.top + 24.0f);
                if (grp.folder_already_exists) {
                    dc->FillRoundedRectangle(D2D1::RoundedRect(tag_rc, 6.0f, 6.0f),
                                             Brush(dc, dark_mode_ ? 0x22FFFFFFu : 0x14000000u));
                    dc->DrawText(L"已有目录", 4, fmt_small_center_.Get(), tag_rc, Brush(dc, sec));
                } else {
                    dc->FillRoundedRectangle(D2D1::RoundedRect(tag_rc, 6.0f, 6.0f),
                                             Brush(dc, accent_argb_, 0.20f));
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(tag_rc, 6.0f, 6.0f),
                                             Brush(dc, accent_argb_, 0.45f), 0.8f);
                    dc->DrawText(L"新建目录", 4, fmt_small_center_.Get(), tag_rc, Brush(dc, accent_argb_));
                }

                const bool all_sel = grp.AllSelected();
                const wchar_t* sel_lbl = all_sel ? L"全不选" : L"全选";
                dc->DrawText(sel_lbl, static_cast<UINT32>(std::wcslen(sel_lbl)), fmt_small_right_.Get(),
                             D2D1::RectF(grc.right - 64.0f, grc.top + 5.0f, grc.right - 10.0f, grc.top + 25.0f),
                             Brush(dc, accent_argb_));

                const LONG grp_btn_top = std::max(static_cast<LONG>(list_top), static_cast<LONG>(y + 4.0f));
                const LONG grp_btn_bot = std::min(static_cast<LONG>(list_bottom), static_cast<LONG>(y + 26.0f));
                if (grp_btn_bot > grp_btn_top) {
                    click_regions_.push_back({"ai-grp-all:" + std::to_string(gi),
                                              RECT{static_cast<LONG>(w - 80.0f), grp_btn_top,
                                                   static_cast<LONG>(w - 16.0f), grp_btn_bot}});
                }

                if (!grp.reason.empty()) {
                    dc->DrawText(grp.reason.c_str(), static_cast<UINT32>(grp.reason.size()), fmt_small_.Get(),
                                 D2D1::RectF(grc.left + 34.0f, grc.top + 25.0f, grc.right - 10.0f, grc.top + 44.0f),
                                 Brush(dc, sec));
                }

                float ry = y + hdr_h;
                for (size_t ii = 0; ii < grp.items.size(); ++ii) {
                    const auto& it = grp.items[ii];
                    const std::string item_act = "ai-item:" + std::to_string(gi) + ":" + std::to_string(ii);
                    const D2D1_RECT_F rrc = D2D1::RectF(grc.left + 6.0f, oy + ry, grc.right - 6.0f, oy + ry + 28.0f);
                    if (hovered_action_id_ == item_act) {
                        dc->FillRoundedRectangle(D2D1::RoundedRect(rrc, 6.0f, 6.0f),
                                                 Brush(dc, dark_mode_ ? 0x16FFFFFFu : 0x0C000000u));
                    }

                    const LONG r_top = std::max(static_cast<LONG>(list_top), static_cast<LONG>(ry));
                    const LONG r_bot = std::min(static_cast<LONG>(list_bottom), static_cast<LONG>(ry + 28.0f));
                    if (r_bot - r_top >= 8) {
                        click_regions_.push_back({item_act, RECT{20, r_top, static_cast<LONG>(w - 20.0f), r_bot}});
                    }

                    // Checkbox
                    const D2D1_RECT_F cb = D2D1::RectF(rrc.left + 6.0f, rrc.top + 6.5f, rrc.left + 21.0f, rrc.top + 21.5f);
                    if (it.selected) {
                        dc->FillRoundedRectangle(D2D1::RoundedRect(cb, 4.0f, 4.0f), Brush(dc, accent_argb_));
                        const wchar_t chk[2] = {L'\xE73E', 0};
                        DrawGlyph(dc, chk, 1, fmt_fluent_12_.Get(), cb,
                                     Brush(dc, dark_mode_ ? 0xFF0A121Eu : 0xFFFFFFFFu));
                    } else {
                        dc->DrawRoundedRectangle(D2D1::RoundedRect(cb, 4.0f, 4.0f),
                                                 Brush(dc, dark_mode_ ? 0x55FFFFFFu : 0x45000000u), 1.2f);
                    }

                    render::DrawVectorFileIcon(fac.Get(), dc, dwrite_.Get(),
                                               D2D1::RectF(rrc.left + 27.0f, rrc.top + 5.0f, rrc.left + 45.0f, rrc.top + 23.0f),
                                               it.name, it.is_folder, it.selected ? 1.0f : 0.58f);

                    dc->DrawText(it.name.c_str(), static_cast<UINT32>(it.name.size()), fmt_body_.Get(),
                                 D2D1::RectF(rrc.left + 50.0f, rrc.top, rrc.right - 108.0f, rrc.bottom),
                                 Brush(dc, it.selected ? pri : sec));

                    const std::wstring sz_part = (it.is_folder && it.size_bytes == 0)
                                                     ? std::wstring(L"文件夹")
                                                     : FormatSizeShort(it.size_bytes);
                    const std::wstring meta = sz_part + L" · " + FormatAgeDaysShort(it.age_days);
                    dc->DrawText(meta.c_str(), static_cast<UINT32>(meta.size()), fmt_small_right_.Get(),
                                 D2D1::RectF(rrc.right - 106.0f, rrc.top, rrc.right - 6.0f, rrc.bottom),
                                 Brush(dc, sec));

                    ry += 30.0f;
                }
            }
            y += card_h_px + 8.0f;
        }
    }

    if (ai_max_scroll_y_ > 1.0f) {
        const float thumb_h = std::max(28.0f, view_h * (view_h / (view_h + ai_max_scroll_y_)));
        const float thumb_y = list_top + (view_h - thumb_h) * (ai_scroll_y_ / ai_max_scroll_y_);
        dc->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(ox + w - 7.0f, oy + thumb_y, ox + w - 4.0f, oy + thumb_y + thumb_h), 1.5f, 1.5f),
            Brush(dc, dark_mode_ ? 0x45FFFFFFu : 0x35000000u));
    }

    dc->PopAxisAlignedClip();

    // Pin-to-tiles checkbox strip [h - 88 .. h - 58]
    const float pin_y = h - 88.0f;
    dc->DrawLine(D2D1::Point2F(ox, oy + pin_y), D2D1::Point2F(ox + w, oy + pin_y),
                 Brush(dc, dark_mode_ ? 0x16FFFFFFu : 0x10000000u), 1.0f);
    click_regions_.push_back({"ai-toggle-pin",
                              RECT{14, static_cast<LONG>(pin_y + 2.0f), static_cast<LONG>(w - 14.0f), static_cast<LONG>(h - 58.0f)}});
    const D2D1_RECT_F pcb = D2D1::RectF(ox + 16.0f, oy + pin_y + 7.5f, ox + 31.0f, oy + pin_y + 22.5f);
    if (ai_state_.pin_created_folders) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(pcb, 4.0f, 4.0f), Brush(dc, accent_argb_));
        const wchar_t chk[2] = {L'\xE73E', 0};
        DrawGlyph(dc, chk, 1, fmt_fluent_12_.Get(), pcb,
                     Brush(dc, dark_mode_ ? 0xFF0A121Eu : 0xFFFFFFFFu));
    } else {
        dc->DrawRoundedRectangle(D2D1::RoundedRect(pcb, 4.0f, 4.0f),
                                 Brush(dc, dark_mode_ ? 0x55FFFFFFu : 0x45000000u), 1.2f);
    }
    const wchar_t* pin_lbl = L"将新建的归档目录固定为桌面磁贴，方便随时展开";
    dc->DrawText(pin_lbl, static_cast<UINT32>(std::wcslen(pin_lbl)), fmt_small_.Get(),
                 D2D1::RectF(ox + 37.0f, oy + pin_y, ox + w - 14.0f, oy + h - 58.0f), Brush(dc, pri));

    // Footer Action Bar [h - 58 .. h]
    const float fy = oy + h - 58.0f;
    dc->DrawLine(D2D1::Point2F(ox, fy), D2D1::Point2F(ox + w, fy),
                 Brush(dc, dark_mode_ ? 0x1AFFFFFFu : 0x12000000u), 1.0f);

    wchar_t foot_sum[48]{};
    std::swprintf(foot_sum, ARRAYSIZE(foot_sum), L"已选 %zu 项", sel_cnt);
    dc->DrawText(foot_sum, static_cast<UINT32>(std::wcslen(foot_sum)), fmt_small_.Get(),
                 D2D1::RectF(ox + 16.0f, fy, ox + 136.0f, oy + h), Brush(dc, sec));

    const D2D1_RECT_F virt_rc = D2D1::RectF(ox + w - 258.0f, fy + 12.0f, ox + w - 154.0f, oy + h - 12.0f);
    const D2D1_RECT_F arch_rc = D2D1::RectF(ox + w - 146.0f, fy + 12.0f, ox + w - 16.0f, oy + h - 12.0f);
    click_regions_.push_back({"ai-apply-virtual",
                              RECT{static_cast<LONG>(w - 258.0f), static_cast<LONG>(h - 46.0f),
                                   static_cast<LONG>(w - 154.0f), static_cast<LONG>(h - 12.0f)}});
    click_regions_.push_back({"ai-apply-archive",
                              RECT{static_cast<LONG>(w - 146.0f), static_cast<LONG>(h - 46.0f),
                                   static_cast<LONG>(w - 16.0f), static_cast<LONG>(h - 12.0f)}});

    dc->FillRoundedRectangle(D2D1::RoundedRect(virt_rc, 8.0f, 8.0f),
                             Brush(dc, dark_mode_ ? 0x1EFFFFFFu : 0x12000000u, sel_cnt > 0 ? 1.0f : 0.45f));
    dc->DrawText(L"仅建虚拟分组", 6, fmt_small_center_.Get(), virt_rc,
                 Brush(dc, pri, sel_cnt > 0 ? 1.0f : 0.45f));

    dc->FillRoundedRectangle(D2D1::RoundedRect(arch_rc, 8.0f, 8.0f),
                             Brush(dc, accent_argb_, sel_cnt > 0 ? 1.0f : 0.40f));
    wchar_t arch_lbl[40]{};
    std::swprintf(arch_lbl, ARRAYSIZE(arch_lbl), L"归档到目录 (%zu)", sel_cnt);
    dc->DrawText(arch_lbl, static_cast<UINT32>(std::wcslen(arch_lbl)), fmt_small_center_.Get(),
                 arch_rc, Brush(dc, dark_mode_ ? 0xFF0A121Eu : 0xFFFFFFFFu));
}

void DesktopOverlayController::DrawTrayPopover(ID2D1DeviceContext* dc, float ox, float oy) {
    const uint32_t pri = dark_mode_ ? 0xF8FFFFFFu : 0xF8181E28u;
    const uint32_t sec = dark_mode_ ? 0x92FFFFFFu : 0x92303846u;
    const float w = static_cast<float>(card_w_);
    const float h = static_cast<float>(card_h_);

    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);

    const wchar_t tray_g[2] = {L'\xE7B8', 0};
    DrawGlyph(dc, tray_g, 1, fmt_fluent_15_.Get(),
                 D2D1::RectF(ox + 14.0f, oy + 12.0f, ox + 36.0f, oy + 38.0f), Brush(dc, accent_argb_));
    wchar_t hdr[64]{};
    std::swprintf(hdr, ARRAYSIZE(hdr), L"暂存区 (%zu)", tray_entries_.size());
    dc->DrawText(hdr, static_cast<UINT32>(std::wcslen(hdr)), fmt_title_.Get(),
                 D2D1::RectF(ox + 38.0f, oy + 12.0f, ox + w - 110.0f, oy + 38.0f), Brush(dc, pri));
    dc->DrawText(L"和 Pulse 同步", 9, fmt_small_right_.Get(),
                 D2D1::RectF(ox + w - 116.0f, oy + 12.0f, ox + w - 16.0f, oy + 38.0f), Brush(dc, sec));

    float y = 46.0f;
    if (tray_entries_.empty()) {
        const wchar_t* empty = L"把文件拖到这里，或选中后按 Ctrl+Shift+S";
        dc->DrawText(empty, static_cast<UINT32>(std::wcslen(empty)), fmt_small_center_.Get(),
                     D2D1::RectF(ox + 18.0f, oy + 118.0f, ox + w - 18.0f, oy + 148.0f), Brush(dc, sec));
    } else {
        for (size_t i = 0; i < tray_entries_.size() && y + 40.0f <= h - 54.0f; ++i) {
            const auto& te = tray_entries_[i];
            const D2D1_RECT_F rrc = D2D1::RectF(ox + 12.0f, oy + y, ox + w - 12.0f, oy + y + 38.0f);
            dc->FillRoundedRectangle(D2D1::RoundedRect(rrc, 7.0f, 7.0f),
                                     Brush(dc, dark_mode_ ? 0x12FFFFFFu : 0x08000000u));
            render::DrawVectorFileIcon(fac.Get(), dc, dwrite_.Get(),
                                       D2D1::RectF(rrc.left + 8.0f, rrc.top + 8.0f, rrc.left + 30.0f, rrc.top + 30.0f),
                                       te.name, te.is_folder);
            dc->DrawText(te.name.c_str(), static_cast<UINT32>(te.name.size()), fmt_body_.Get(),
                         D2D1::RectF(rrc.left + 36.0f, rrc.top + 2.0f, rrc.right - 36.0f, rrc.top + 21.0f),
                         Brush(dc, pri));
            const std::wstring from_dir = L"来自 " + ParentFolderDisplay(te.path);
            dc->DrawText(from_dir.c_str(), static_cast<UINT32>(from_dir.size()), fmt_small_.Get(),
                         D2D1::RectF(rrc.left + 36.0f, rrc.top + 20.0f, rrc.right - 36.0f, rrc.bottom - 2.0f),
                         Brush(dc, sec));

            click_regions_.push_back({"tray-rm:" + std::to_string(te.item_id),
                                      RECT{static_cast<LONG>(w - 42), static_cast<LONG>(y),
                                           static_cast<LONG>(w - 12), static_cast<LONG>(y + 38)}});
            const wchar_t x_g[2] = {L'\xE711', 0};
            DrawGlyph(dc, x_g, 1, fmt_fluent_12_.Get(),
                         D2D1::RectF(rrc.right - 28.0f, rrc.top, rrc.right - 4.0f, rrc.bottom), Brush(dc, sec));
            y += 42.0f;
        }
    }

    const float fy = oy + h - 46.0f;
    dc->DrawLine(D2D1::Point2F(ox, fy - 4.0f), D2D1::Point2F(ox + w, fy - 4.0f),
                 Brush(dc, dark_mode_ ? 0x1AFFFFFFu : 0x12000000u), 1.0f);

    PulseButton(dc, ox, oy, D2D1::RectF(12.0f, h - 43.0f, 124.0f, h - 11.0f), L"全部放到桌面", "tray-release", true);
    PulseButton(dc, ox, oy, D2D1::RectF(130.0f, h - 43.0f, 266.0f, h - 11.0f), L"在 Pulse 中打开", "tray-open-pulse", false);
    PulseButton(dc, ox, oy, D2D1::RectF(272.0f, h - 43.0f, w - 12.0f, h - 11.0f), L"清空", "tray-clear", false);
}

void DesktopOverlayController::DrawRulePopover(ID2D1DeviceContext* dc, float ox, float oy) {
    const uint32_t pri = dark_mode_ ? 0xF8FFFFFFu : 0xF8181E28u;
    const uint32_t sec = dark_mode_ ? 0x92FFFFFFu : 0x92303846u;
    const float w = static_cast<float>(card_w_);
    const float h = static_cast<float>(card_h_);

    const std::wstring hdr = rule_group_.title + L" · 整理规则";
    dc->DrawText(hdr.c_str(), static_cast<UINT32>(hdr.size()), fmt_title_.Get(),
                 D2D1::RectF(ox + 16.0f, oy + 12.0f, ox + w - 16.0f, oy + 38.0f), Brush(dc, pri));

    dc->DrawText(L"包含这些类型", 6, fmt_small_.Get(),
                 D2D1::RectF(ox + 16.0f, oy + 44.0f, ox + w - 16.0f, oy + 62.0f), Brush(dc, sec));

    auto has_ext = [&](std::wstring_view ext) {
        return std::find(rule_group_.rule.extensions.begin(), rule_group_.rule.extensions.end(), ext) !=
               rule_group_.rule.extensions.end();
    };

    float px = 16.0f;
    float py = 68.0f;
    for (const auto& c : kRuleTypeChips) {
        const std::wstring lbl(c.label);
        const float pw = MeasureTextW(lbl, fmt_small_center_.Get(), 120.0f) + 22.0f;
        if (px + pw > w - 16.0f) {
            px = 16.0f;
            py += 34.0f;
        }
        const bool active = c.is_folder_toggle ? rule_group_.rule.match_folders : has_ext(c.primary_ext);
        const D2D1_RECT_F prc = D2D1::RectF(ox + px, oy + py, ox + px + pw, oy + py + 28.0f);
        click_regions_.push_back({c.id, RECT{static_cast<LONG>(px), static_cast<LONG>(py),
                                             static_cast<LONG>(px + pw), static_cast<LONG>(py + 28.0f)}});
        dc->FillRoundedRectangle(D2D1::RoundedRect(prc, 14.0f, 14.0f),
                                 Brush(dc, active ? accent_argb_ : (dark_mode_ ? 0x1AFFFFFFu : 0x10000000u),
                                       active ? 0.28f : 1.0f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(prc, 14.0f, 14.0f),
                                 Brush(dc, active ? accent_argb_ : (dark_mode_ ? 0x26FFFFFFu : 0x18000000u)), 1.0f);
        dc->DrawText(lbl.c_str(), static_cast<UINT32>(lbl.size()), fmt_small_center_.Get(),
                     prc, Brush(dc, active ? accent_argb_ : pri));
        px += pw + 6.0f;
    }

    py += 36.0f;
    dc->DrawText(L"Pulse 颜色标签", 10, fmt_small_.Get(),
                 D2D1::RectF(ox + 16.0f, oy + py, ox + w - 16.0f, oy + py + 18.0f), Brush(dc, sec));
    py += 21.0f;

    std::vector<ColorTagOption> tags_for_rule = rule_tag_palette_;
    if (tags_for_rule.empty()) {
        tags_for_rule = {
            {L"red",    L"红", 0xFFE5484Du},
            {L"orange", L"橙", 0xFFF5A524u},
            {L"green",  L"绿", 0xFF30A46Cu},
            {L"yellow", L"黄", 0xFFF2C94Cu},
            {L"purple", L"紫", 0xFF8E4EC6u},
            {L"blue",   L"蓝", 0xFF3E8EF7u},
            {L"gray",   L"灰", 0xFF8B8D98u},
        };
    }
    {
        float rtx = 16.0f;
        const bool any_active = rule_group_.rule.tag_id.empty();
        const D2D1_RECT_F any_rc = D2D1::RectF(ox + rtx, oy + py, ox + rtx + 50.0f, oy + py + 26.0f);
        click_regions_.push_back({"rtag:clear", RECT{static_cast<LONG>(rtx), static_cast<LONG>(py),
                                                     static_cast<LONG>(rtx + 50.0f), static_cast<LONG>(py + 26.0f)}});
        dc->FillRoundedRectangle(D2D1::RoundedRect(any_rc, 13.0f, 13.0f),
                                 Brush(dc, any_active ? accent_argb_ : (dark_mode_ ? 0x1AFFFFFFu : 0x10000000u),
                                       any_active ? 0.28f : 1.0f));
        dc->DrawText(L"不限", 2, fmt_small_center_.Get(), any_rc, Brush(dc, any_active ? accent_argb_ : pri));
        rtx += 56.0f;

        const size_t max_tags = std::min<size_t>(7, tags_for_rule.size());
        for (size_t ti = 0; ti < max_tags; ++ti) {
            const auto& tg = tags_for_rule[ti];
            const bool sel = (rule_group_.rule.tag_id == tg.id);
            const float cx = ox + rtx + 13.0f;
            const float cy = oy + py + 13.0f;
            click_regions_.push_back({"rtag:" + ipc::WideToUtf8(tg.id),
                                      RECT{static_cast<LONG>(rtx), static_cast<LONG>(py),
                                           static_cast<LONG>(rtx + 26.0f), static_cast<LONG>(py + 26.0f)}});
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), sel ? 10.0f : 8.5f, sel ? 10.0f : 8.5f),
                            Brush(dc, tg.argb));
            if (sel) {
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 12.0f, 12.0f), Brush(dc, pri), 1.5f);
            }
            rtx += 32.0f;
        }
    }

    py += 34.0f;
    dc->DrawText(L"修改时间", 4, fmt_small_.Get(),
                 D2D1::RectF(ox + 16.0f, oy + py, ox + w - 16.0f, oy + py + 18.0f), Brush(dc, sec));
    py += 22.0f;

    struct TimeOpt { const char* id; const wchar_t* lbl; int days; };
    static constexpr TimeOpt kTimes[3] = {
        {"rd:0",  L"不限",    0},
        {"rd:7",  L"7 天内",  7},
        {"rd:30", L"30 天内", 30},
    };
    float tx = 16.0f;
    for (const auto& t : kTimes) {
        const float tw = 78.0f;
        const bool active = (rule_group_.rule.modified_within_days == t.days);
        const D2D1_RECT_F trc = D2D1::RectF(ox + tx, oy + py, ox + tx + tw, oy + py + 28.0f);
        click_regions_.push_back({t.id, RECT{static_cast<LONG>(tx), static_cast<LONG>(py),
                                             static_cast<LONG>(tx + tw), static_cast<LONG>(py + 28.0f)}});
        dc->FillRoundedRectangle(D2D1::RoundedRect(trc, 14.0f, 14.0f),
                                 Brush(dc, active ? accent_argb_ : (dark_mode_ ? 0x1AFFFFFFu : 0x10000000u),
                                       active ? 0.28f : 1.0f));
        dc->DrawText(t.lbl, static_cast<UINT32>(std::wcslen(t.lbl)), fmt_small_center_.Get(),
                     trc, Brush(dc, active ? accent_argb_ : pri));
        tx += tw + 8.0f;
    }

    PulseButton(dc, ox, oy, D2D1::RectF(w - 180.0f, h - 44.0f, w - 112.0f, h - 12.0f), L"取消", "close-overlay", false);
    PulseButton(dc, ox, oy, D2D1::RectF(w - 104.0f, h - 44.0f, w - 14.0f, h - 12.0f), L"应用并整理", "rule-apply", true);
}

void DesktopOverlayController::DrawStickyNotePopover(ID2D1DeviceContext* dc, float ox, float oy) {
    const bool is_todo = (sticky_tile_id_ == "@todos");
    const bool is_city = (sticky_tile_id_ == "@weather_city");
    const bool is_title = sticky_tile_id_.starts_with("@title:");
    const bool warm = (!is_todo && !is_city && !is_title);

    const uint32_t pri = warm ? (dark_mode_ ? 0xF8FFF6D6u : 0xF83A2A0Du)
                              : (dark_mode_ ? 0xF8FFFFFFu : 0xF8181E28u);
    const uint32_t sec = warm ? (dark_mode_ ? 0x99FFF6D6u : 0x995C461Au)
                              : (dark_mode_ ? 0x92FFFFFFu : 0x92303846u);
    const uint32_t accent = warm ? 0xFFF5C542u : accent_argb_;
    const float w = static_cast<float>(card_w_);
    const float h = static_cast<float>(card_h_);

    wchar_t icon_ch = L'\xE70B';
    const wchar_t* hdr_title = L"桌面便笺";
    const wchar_t* hdr_sub = L"直接输入 · 自动保存";
    if (is_todo) {
        icon_ch = L'\xE73A';
        hdr_title = L"待办清单";
        hdr_sub = L"每行一项 · 自动保存";
    } else if (is_city) {
        icon_ch = L'\xE707';
        hdr_title = L"天气城市";
        hdr_sub = L"回车确认";
    } else if (is_title) {
        icon_ch = L'\xE8AC';
        hdr_title = L"磁贴标题";
        hdr_sub = L"留空恢复默认";
    }

    const wchar_t note_g[2] = {icon_ch, 0};
    DrawGlyph(dc, note_g, 1, fmt_fluent_15_.Get(),
              D2D1::RectF(ox + 14.0f, oy + 12.0f, ox + 36.0f, oy + 38.0f), Brush(dc, accent));
    dc->DrawText(hdr_title, static_cast<UINT32>(std::wcslen(hdr_title)), fmt_title_.Get(),
                 D2D1::RectF(ox + 38.0f, oy + 12.0f, ox + 140.0f, oy + 38.0f), Brush(dc, pri));
    dc->DrawText(hdr_sub, static_cast<UINT32>(std::wcslen(hdr_sub)), fmt_small_right_.Get(),
                 D2D1::RectF(ox + 132.0f, oy + 12.0f, ox + w - 16.0f, oy + 38.0f), Brush(dc, sec));

    const D2D1_RECT_F editor = D2D1::RectF(ox + 14.0f, oy + 44.0f, ox + w - 14.0f, oy + h - 48.0f);
    click_regions_.push_back({"focus-popup-edit", RECT{14, 44, static_cast<LONG>(w - 14), static_cast<LONG>(h - 48)}});
    dc->FillRoundedRectangle(D2D1::RoundedRect(editor, 8.0f, 8.0f),
                             Brush(dc, dark_mode_ ? 0x26000000u : 0x44FFFFFFu));
    StrokeCrispRoundedRect(dc, editor, 8.0f,
                           Brush(dc, warm ? (dark_mode_ ? 0x48FFE58Fu : 0x3CB88900u) : accent, warm ? 1.0f : 0.7f),
                           1.0f);

    const bool live_edit = (dc == d2d_dc_.Get() && popup_edit_hwnd_ != nullptr && IsWindow(popup_edit_hwnd_));
    if (!live_edit) {
        const std::wstring shown = sticky_note_text_.empty()
                                       ? (popup_edit_cue_.empty() ? std::wstring(L"写下待办与灵感…") : popup_edit_cue_)
                                       : sticky_note_text_;
        ComPtr<IDWriteTextFormat> snap_fmt;
        if (dwrite_) {
            dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      14.0f, L"zh-CN", &snap_fmt);
            if (snap_fmt) {
                snap_fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                snap_fmt->SetParagraphAlignment((is_city || is_title) ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
                                                                      : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
        }
        dc->DrawText(shown.c_str(), static_cast<UINT32>(shown.size()),
                     snap_fmt ? snap_fmt.Get() : fmt_body_.Get(),
                     D2D1::RectF(editor.left + 10.0f, editor.top + ((is_city || is_title) ? 0.0f : 8.0f),
                                 editor.right - 10.0f, editor.bottom - ((is_city || is_title) ? 0.0f : 8.0f)),
                     Brush(dc, sticky_note_text_.empty() ? sec : pri));
    }

    PulseButton(dc, ox, oy, D2D1::RectF(w - 152.0f, h - 42.0f, w - 86.0f, h - 10.0f), L"清空", "note-clear", false);
    PulseButton(dc, ox, oy, D2D1::RectF(w - 78.0f, h - 42.0f, w - 14.0f, h - 10.0f), L"完成", "note-done", true);
}

void DesktopOverlayController::DrawToastCapsule(ID2D1DeviceContext* dc, float ox, float oy) {
    const uint32_t pri = dark_mode_ ? 0xF8FFFFFFu : 0xF8181E28u;
    const float w = static_cast<float>(card_w_);
    const float h = static_cast<float>(card_h_);
    const float act_w = toast_action_label_.empty() ? 0.0f : 74.0f;

    dc->DrawText(toast_message_.c_str(), static_cast<UINT32>(toast_message_.size()), fmt_body_.Get(),
                 D2D1::RectF(ox + 18.0f, oy, ox + w - act_w - 14.0f, oy + h), Brush(dc, pri));

    if (!toast_action_label_.empty()) {
        const D2D1_RECT_F brc = D2D1::RectF(ox + w - 80.0f, oy + 7.0f, ox + w - 8.0f, oy + h - 7.0f);
        click_regions_.push_back({"toast-action", RECT{static_cast<LONG>(w - 80), 7,
                                                       static_cast<LONG>(w - 8), static_cast<LONG>(h - 7)}});
        dc->FillRoundedRectangle(D2D1::RoundedRect(brc, 14.0f, 14.0f), Brush(dc, accent_argb_, 0.24f));
        dc->DrawText(toast_action_label_.c_str(), static_cast<UINT32>(toast_action_label_.size()),
                     fmt_small_center_.Get(), brc, Brush(dc, accent_argb_));
    }
}

// ---------------------------------------------------------------------------
// Input & Window Dispatch
// ---------------------------------------------------------------------------
void DesktopOverlayController::HandleMouseMove(int card_x, int card_y) {
    if (active_kind_ == OverlayKind::ContextMenu) {
        int new_row = -1;
        int new_icon = -1;
        int new_tag = -1;
        for (size_t i = 0; i < menu_rows_.size(); ++i) {
            const auto& r = menu_rows_[i];
            if (PtInRectXY(r.bounds, card_x, card_y)) {
                new_row = static_cast<int>(i);
                if (r.is_icon_bar) {
                    const int slot_w = std::max<int>(1, (r.bounds.right - r.bounds.left) / 5);
                    new_icon = std::clamp<int>((card_x - r.bounds.left) / slot_w, 0, 4);
                } else if (r.is_tag_row) {
                    const int rel_x = card_x - (r.bounds.left + 16);
                    if (rel_x >= -10 && rel_x < 7 * 24) {
                        new_tag = std::clamp<int>((rel_x + 10) / 24, 0,
                                                  static_cast<int>(menu_spec_.tag_palette.size()) - 1);
                    }
                }
                break;
            }
        }
        if (new_row != hovered_row_ || new_icon != hovered_icon_bar_idx_ || new_tag != hovered_tag_idx_) {
            hovered_row_ = new_row;
            hovered_icon_bar_idx_ = new_icon;
            hovered_tag_idx_ = new_tag;
            Render();
            if (new_row >= 0 && !menu_rows_[static_cast<size_t>(new_row)].submenu_rows.empty()) {
                OpenSubmenuForRow(new_row);
            } else if (new_row >= 0) {
                DestroySubmenu();
            }
        }
        return;
    }

    if (active_kind_ == OverlayKind::DockSystemPanel && dock_vol_dragging_) {
        ApplyDockVolumeTrackAtX(card_x, true);
        return;
    }

    if (active_kind_ == OverlayKind::BeautifyDrawer && !dragging_beautify_slider_.empty()) {
        if (ApplyBeautifySliderAtX(dragging_beautify_slider_, card_x)) {
            NotifyBeautifyChanged(true);
            Render();
        }
        return;
    }

    if (active_kind_ == OverlayKind::AiOrganizeDrawer || active_kind_ == OverlayKind::BeautifyDrawer ||
        active_kind_ == OverlayKind::DockSystemPanel) {
        std::string hit_act;
        if (active_kind_ == OverlayKind::BeautifyDrawer) {
            for (const auto& sl : beautify_sliders_) {
                if (PtInRectXY(sl.bounds, card_x, card_y)) {
                    hit_act = sl.id;
                    break;
                }
            }
        }
        if (hit_act.empty()) {
            for (const auto& reg : click_regions_) {
                if (PtInRectXY(reg.bounds, card_x, card_y)) {
                    hit_act = reg.action_id;
                    break;
                }
            }
        }
        if (hit_act != hovered_action_id_) {
            hovered_action_id_ = std::move(hit_act);
            Render();
        }
    }
}

void DesktopOverlayController::HandleSubmenuMouseMove(int sub_card_x, int sub_card_y) {
    if (!submenu_open_) return;
    int new_sub = -1;
    for (size_t i = 0; i < submenu_rows_.size(); ++i) {
        if (PtInRectXY(submenu_rows_[i].bounds, sub_card_x, sub_card_y)) {
            new_sub = static_cast<int>(i);
            break;
        }
    }
    if (new_sub != hovered_sub_row_) {
        hovered_sub_row_ = new_sub;
        RenderSubmenu();
    }
}

void DesktopOverlayController::HandleSubmenuClick(int sub_card_x, int sub_card_y) {
    if (!submenu_open_) return;
    for (const auto& sr : submenu_rows_) {
        if (!PtInRectXY(sr.bounds, sub_card_x, sub_card_y) || !sr.enabled) continue;
        const std::string cmd = sr.command_id;
        const ContextMenuSpec spec_copy = menu_spec_;
        Dismiss();
        if (callbacks_.on_menu_command) callbacks_.on_menu_command(cmd, spec_copy);
        return;
    }
}

void DesktopOverlayController::HandleLeftClick(int card_x, int card_y) {
    if (active_kind_ == OverlayKind::ContextMenu) {
        for (size_t i = 0; i < menu_rows_.size(); ++i) {
            const auto& row = menu_rows_[i];
            if (!PtInRectXY(row.bounds, card_x, card_y) || !row.enabled) continue;

            if (row.is_icon_bar) {
                static constexpr const char* kBarCmds[5] = {
                    "cut-item", "copy-item", "rename-item", "delete-item", "share-item"
                };
                const int slot_w = std::max<int>(1, (row.bounds.right - row.bounds.left) / 5);
                const int idx = std::clamp<int>((card_x - row.bounds.left) / slot_w, 0, 4);
                const std::string cmd = kBarCmds[idx];
                const ContextMenuSpec spec_copy = menu_spec_;
                Dismiss();
                if (callbacks_.on_menu_command) callbacks_.on_menu_command(cmd, spec_copy);
                return;
            }
            if (row.is_tag_row) {
                const int rel_x = card_x - (row.bounds.left + 16);
                if (rel_x >= -10 && rel_x < 7 * 24) {
                    const int idx = std::clamp<int>((rel_x + 10) / 24, 0,
                                                    static_cast<int>(menu_spec_.tag_palette.size()) - 1);
                    const std::wstring& chosen = menu_spec_.tag_palette[static_cast<size_t>(idx)].id;
                    const std::string cmd = (chosen == menu_spec_.current_tag_id)
                                                ? "tag:clear"
                                                : ("tag:" + ipc::WideToUtf8(chosen));
                    const ContextMenuSpec spec_copy = menu_spec_;
                    Dismiss();
                    if (callbacks_.on_menu_command) callbacks_.on_menu_command(cmd, spec_copy);
                }
                return;
            }
            if (!row.submenu_rows.empty()) {
                OpenSubmenuForRow(static_cast<int>(i));
                return;
            }
            if (row.is_shell_verb && row.shell_item_id != 0) {
                const uint64_t sid = shell_session_id_;
                const uint32_t iid = row.shell_item_id;
                const std::wstring verb = row.shell_verb;
                shell_session_id_ = 0;
                Dismiss();
                if (callbacks_.on_invoke_shell_verb) callbacks_.on_invoke_shell_verb(sid, iid, verb);
                return;
            }
            const std::string cmd = row.command_id;
            const ContextMenuSpec spec_copy = menu_spec_;
            Dismiss();
            if (callbacks_.on_menu_command) callbacks_.on_menu_command(cmd, spec_copy);
            return;
        }
        return;
    }

    if (active_kind_ == OverlayKind::AiOrganizeDrawer && popup_edit_hwnd_ != nullptr) {
        bool clicked_prompt = false;
        for (const auto& reg : click_regions_) {
            if (PtInRectXY(reg.bounds, card_x, card_y) && reg.action_id == "ai-edit-prompt") {
                clicked_prompt = true;
                break;
            }
        }
        if (!clicked_prompt) {
            ai_state_.custom_instruction = ReadHostedPopupEditText();
            DestroyHostedPopupEdit();
            edit_on_commit_ = {};
            edit_allow_empty_ = false;
            Render();
        }
    }

    if (active_kind_ == OverlayKind::DockSystemPanel &&
        dock_panel_kind_ == DockPanelKind::Volume &&
        PtInRectXY(dock_vol_track_, card_x, card_y)) {
        dock_vol_dragging_ = true;
        if (hwnd_) SetCapture(hwnd_);
        ApplyDockVolumeTrackAtX(card_x, true);
        return;
    }

    if (active_kind_ == OverlayKind::BeautifyDrawer) {
        for (const auto& sl : beautify_sliders_) {
            if (PtInRectXY(sl.bounds, card_x, card_y)) {
                dragging_beautify_slider_ = sl.id;
                if (hwnd_) SetCapture(hwnd_);
                ApplyBeautifySliderAtX(sl.id, card_x);
                NotifyBeautifyChanged(true);
                Render();
                return;
            }
        }
    }

    for (const auto& reg : click_regions_) {
        if (!PtInRectXY(reg.bounds, card_x, card_y)) continue;
        const std::string act = reg.action_id;

        if (HandleDockPanelAction(act)) return;

        if (act == "focus-popup-edit") {
            if (popup_edit_hwnd_ && IsWindow(popup_edit_hwnd_)) {
                SetFocus(popup_edit_hwnd_);
            }
            return;
        }
        if (act == "close-overlay") {
            if (active_kind_ == OverlayKind::AiOrganizeDrawer &&
                ai_state_.phase == AiDrawerPhase::Loading &&
                callbacks_.on_ai_cancel) {
                callbacks_.on_ai_cancel();
            }
            Dismiss();
            return;
        }
        if (act == "ai-conn-settings" || act == "ai-open-settings") {
            if (ai_state_.phase == AiDrawerPhase::Loading && callbacks_.on_ai_cancel) {
                callbacks_.on_ai_cancel();
            }
            Dismiss();
            if (callbacks_.on_ai_open_assistant_settings) {
                callbacks_.on_ai_open_assistant_settings();
            }
            return;
        }
        if (act == "ai-local-preview") {
            ai_scroll_y_ = 0.0f;
            ai_state_.phase = AiDrawerPhase::Loading;
            Render();
            if (callbacks_.on_ai_request_plan) {
                callbacks_.on_ai_request_plan(ai_state_.strategy, ai_state_.custom_instruction, true);
            }
            return;
        }
        if (act.starts_with("ai-strat:")) {
            const auto next_strat = static_cast<ai::AiOrganizeStrategy>(std::clamp(std::stoi(act.substr(9)), 0, 2));
            ai_state_.strategy = next_strat;
            ai_scroll_y_ = 0.0f;
            if (ai_state_.phase != AiDrawerPhase::UnconfiguredGuide) {
                const bool local_only = !ai_state_.connection.configured;
                ai_state_.phase = AiDrawerPhase::Loading;
                Render();
                if (callbacks_.on_ai_request_plan) {
                    callbacks_.on_ai_request_plan(ai_state_.strategy, ai_state_.custom_instruction, local_only);
                }
            } else {
                Render();
            }
            return;
        }
        if (act == "ai-edit-prompt") {
            if (popup_edit_hwnd_ && IsWindow(popup_edit_hwnd_)) {
                SetFocus(popup_edit_hwnd_);
                return;
            }
            const int pad = using_dcomp_ ? kShadowMargin : 0;
            BeginInlineRename(
                hwnd_,
                model::RectI{pad + 16, pad + 86, card_w_ - 68, 32},
                ai_state_.custom_instruction,
                false,
                [this](const std::wstring& committed) {
                    const bool changed = (committed != ai_state_.custom_instruction);
                    ai_state_.custom_instruction = committed;
                    if (changed || ai_state_.phase != AiDrawerPhase::UnconfiguredGuide) {
                        ai_scroll_y_ = 0.0f;
                        const bool local_only = !ai_state_.connection.configured;
                        ai_state_.phase = AiDrawerPhase::Loading;
                        Render();
                        if (callbacks_.on_ai_request_plan) {
                            callbacks_.on_ai_request_plan(ai_state_.strategy, ai_state_.custom_instruction, local_only);
                        }
                    } else {
                        Render();
                    }
                },
                -1,
                true);
            return;
        }
        if (act == "ai-regenerate") {
            if (popup_edit_hwnd_ && IsWindow(popup_edit_hwnd_)) {
                ai_state_.custom_instruction = ReadHostedPopupEditText();
            }
            CancelInlineRename(false);
            ai_scroll_y_ = 0.0f;
            const bool local_only = !ai_state_.connection.configured;
            ai_state_.phase = AiDrawerPhase::Loading;
            Render();
            if (callbacks_.on_ai_request_plan) {
                callbacks_.on_ai_request_plan(ai_state_.strategy, ai_state_.custom_instruction, local_only);
            }
            return;
        }
        if (act == "ai-cancel") {
            if (callbacks_.on_ai_cancel) callbacks_.on_ai_cancel();
            return;
        }
        if (act.starts_with("ai-grp-all:")) {
            const size_t gi = static_cast<size_t>(std::stoul(act.substr(11)));
            if (gi < ai_state_.plan.groups.size()) {
                auto& grp = ai_state_.plan.groups[gi];
                const bool target_sel = !grp.AllSelected();
                for (auto& it : grp.items) it.selected = target_sel;
                Render();
            }
            return;
        }
        if (act.starts_with("ai-item:")) {
            const size_t sep = act.find(':', 8);
            if (sep != std::string::npos) {
                const size_t gi = static_cast<size_t>(std::stoul(act.substr(8, sep - 8)));
                const size_t ii = static_cast<size_t>(std::stoul(act.substr(sep + 1)));
                if (gi < ai_state_.plan.groups.size() && ii < ai_state_.plan.groups[gi].items.size()) {
                    auto& it = ai_state_.plan.groups[gi].items[ii];
                    it.selected = !it.selected;
                    Render();
                }
            }
            return;
        }
        if (act == "ai-toggle-pin") {
            ai_state_.pin_created_folders = !ai_state_.pin_created_folders;
            Render();
            return;
        }
        if (act == "ai-apply-virtual") {
            if (ai_state_.plan.SelectedItemCount() > 0 && callbacks_.on_ai_apply_virtual_groups) {
                const ai::AiOrganizePlan plan_copy = ai_state_.plan;
                callbacks_.on_ai_apply_virtual_groups(plan_copy);
            }
            return;
        }
        if (act == "ai-apply-archive") {
            if (ai_state_.plan.SelectedItemCount() > 0 && callbacks_.on_ai_execute_archive) {
                const ai::AiOrganizePlan plan_copy = ai_state_.plan;
                const bool pin = ai_state_.pin_created_folders;
                callbacks_.on_ai_execute_archive(plan_copy, pin);
            }
            return;
        }
        if (act == "ai-undo-archive") {
            Dismiss();
            if (callbacks_.on_undo_requested) callbacks_.on_undo_requested();
            return;
        }
        if (act == "ai-open-desktop-pulse") {
            Dismiss();
            if (callbacks_.on_ai_open_desktop_in_pulse) callbacks_.on_ai_open_desktop_in_pulse();
            return;
        }
        if (act == "toast-action") {
            auto fn = std::move(toast_on_action_);
            Dismiss();
            if (fn) fn();
            return;
        }
        if (act == "ql-open") {
            const std::wstring p = ql_item_.path;
            Dismiss();
            if (callbacks_.on_open_path) callbacks_.on_open_path(p);
            return;
        }
        if (act.starts_with("search-chip:")) {
            search_query_ = ipc::Utf8ToWide(act.substr(12));
            search_selected_idx_ = 0;
            if (popup_edit_hwnd_ && IsWindow(popup_edit_hwnd_)) {
                const bool prev_ignore = popup_edit_ignore_killfocus_;
                popup_edit_ignore_killfocus_ = true;
                SetWindowTextW(popup_edit_hwnd_, search_query_.c_str());
                const int qlen = static_cast<int>(search_query_.size());
                SendMessageW(popup_edit_hwnd_, EM_SETSEL, qlen, qlen);
                popup_edit_ignore_killfocus_ = prev_ignore;
            }
            if (callbacks_.on_search_query_changed) callbacks_.on_search_query_changed(search_query_);
            RepositionAndShow(card_screen_x_, card_screen_y_, 540, 424, true);
            return;
        }
        if (act == "search-open-pulse") {
            const std::wstring q = search_query_;
            Dismiss();
            if (callbacks_.on_open_search_in_pulse) callbacks_.on_open_search_in_pulse(q);
            return;
        }
        if (act.starts_with("search-hit:")) {
            const size_t idx = static_cast<size_t>(std::stoul(act.substr(11)));
            std::vector<SearchHitEntry> combined;
            for (size_t i = 0; i < std::min<size_t>(4, local_hits_.size()); ++i) combined.push_back(local_hits_[i]);
            for (size_t i = 0; i < std::min<size_t>(3, remote_hits_.size()); ++i) combined.push_back(remote_hits_[i]);
            if (idx < combined.size()) {
                const std::wstring p = combined[idx].path;
                Dismiss();
                if (callbacks_.on_open_path) callbacks_.on_open_path(p);
            }
            return;
        }
        if (act.starts_with("health-cat:")) {
            const int cat = std::stoi(act.substr(11));
            bool all_sel = true;
            for (const auto& iss : health_report_.issues) {
                if (HealthCategoryIndex(iss.kind) == cat && !iss.selected_by_default) {
                    all_sel = false;
                    break;
                }
            }
            for (auto& iss : health_report_.issues) {
                if (HealthCategoryIndex(iss.kind) == cat) iss.selected_by_default = !all_sel;
            }
            Render();
            return;
        }
        if (act.starts_with("health-toggle:")) {
            const size_t idx = static_cast<size_t>(std::stoul(act.substr(14)));
            if (idx < health_report_.issues.size()) {
                health_report_.issues[idx].selected_by_default = !health_report_.issues[idx].selected_by_default;
                Render();
            }
            return;
        }
        if (act == "health-archive" || act == "health-recycle") {
            std::vector<std::wstring> selected;
            uint64_t bytes = 0;
            for (const auto& iss : health_report_.issues) {
                if (iss.selected_by_default) {
                    selected.push_back(iss.path);
                    bytes += iss.reclaimable_bytes;
                }
            }
            if (selected.empty()) return;
            health_cleaned_count_ = selected.size();
            health_cleaned_bytes_ = bytes;
            health_completed_ = true;
            if (callbacks_.on_execute_health_action) {
                callbacks_.on_execute_health_action(act == "health-archive", selected);
            }
            Render();
            return;
        }
        if (act == "health-undo") {
            Dismiss();
            if (callbacks_.on_undo_requested) callbacks_.on_undo_requested();
            return;
        }
        if (HandleWidgetDetailAction(act)) return;
        if (act.starts_with("b-tab:")) {
            const int idx = std::clamp(std::stoi(act.substr(6)), 0, 3);
            SetBeautifyTab(static_cast<BeautifyTab>(idx), true);
            return;
        }
        if (act == "b-vid-pause") {
            if (callbacks_.on_toggle_video_pause) callbacks_.on_toggle_video_pause();
            return;
        }
        if (act == "b-vid-pick") {
            if (callbacks_.on_pick_video_wallpaper) callbacks_.on_pick_video_wallpaper();
            return;
        }
        if (act.starts_with("b-vid-recent:")) {
            const size_t idx = static_cast<size_t>(std::max(0, std::stoi(act.substr(13))));
            if (idx < beautify_video_.recent.size() && callbacks_.on_set_video_wallpaper) {
                callbacks_.on_set_video_wallpaper(beautify_video_.recent[idx].path);
            }
            return;
        }
        if (act.starts_with("b-vfill:")) {
            const auto fm = (std::stoi(act.substr(8)) == 1) ? video::VideoFillMode::Fit : video::VideoFillMode::Fill;
            beautify_video_.fill_mode = fm;
            if (callbacks_.on_set_video_fill_mode) callbacks_.on_set_video_fill_mode(fm);
            Render();
            return;
        }
        if (act == "b-vid-cancel") {
            if (callbacks_.on_cancel_video_optimize) callbacks_.on_cancel_video_optimize();
            return;
        }
        if (act == "b-vid-confirm-heavy") {
            if (callbacks_.on_confirm_heavy_video_optimize) callbacks_.on_confirm_heavy_video_optimize();
            return;
        }
        if (act == "b-vid-store") {
            if (!beautify_video_.store_uri.empty() && callbacks_.on_open_codec_store) {
                callbacks_.on_open_codec_store(beautify_video_.store_uri);
            }
            return;
        }
        if (act == "b-vid-takeover") {
            if (callbacks_.on_enable_icon_takeover) callbacks_.on_enable_icon_takeover();
            return;
        }
        if (act.starts_with("b-wall:")) {
            const auto chosen = static_cast<model::WallpaperPreset>(std::stoi(act.substr(7)));
            const bool was_video = !beautify_video_.active_source_path.empty() &&
                                   beautify_video_.phase != video::VideoWallpaperPhase::Inactive;
            beautify_settings_.wallpaper_preset =
                (!was_video && chosen != model::WallpaperPreset::System && beautify_settings_.wallpaper_preset == chosen)
                    ? model::WallpaperPreset::System
                    : chosen;
            beautify_video_.active_source_path.clear();
            beautify_video_.tried_display_name.clear();
            beautify_video_.tried_source_path.clear();
            beautify_video_.phase = video::VideoWallpaperPhase::Inactive;
            if (beautify_settings_.auto_wallpaper_accent) {
                beautify_settings_.accent_argb =
                    render::WallpaperBackdrop::PresetAccentArgb(beautify_settings_.wallpaper_preset, dark_mode_);
                accent_argb_ = beautify_settings_.accent_argb;
            }
            if (was_video && callbacks_.on_set_video_wallpaper) {
                callbacks_.on_set_video_wallpaper(L""); // empty path = switch back to picture wallpaper
            }
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act == "b-more-widgets") {
            if (callbacks_.on_request_open_widget_gallery) {
                callbacks_.on_request_open_widget_gallery(std::nullopt);
            } else {
                const RECT wa = work_area_;
                OpenWidgetGalleryDrawer(wa, wa.right - 560, wa.top + 44, gallery_tiles_, std::nullopt, std::nullopt);
            }
            return;
        }
        if (act.starts_with("wg-open:")) {
            const auto wk = static_cast<model::TileKind>(std::stoi(act.substr(8)));
            if (callbacks_.on_request_open_widget_gallery) {
                callbacks_.on_request_open_widget_gallery(wk);
            } else {
                const RECT wa = work_area_;
                OpenWidgetGalleryDrawer(wa, wa.right - 560, wa.top + 44, gallery_tiles_, wk, std::nullopt);
            }
            return;
        }
        if (act.starts_with("wg-sel:")) {
            gallery_selected_kind_ = static_cast<model::TileKind>(std::stoi(act.substr(7)));
            const auto& def = FindGalleryWidgetDef(gallery_selected_kind_);
            bool valid_sz = false;
            for (const auto& sz : def.sizes) {
                if (sz == gallery_selected_size_) { valid_sz = true; break; }
            }
            if (!valid_sz) {
                gallery_selected_size_ = *def.sizes.begin();
                for (const auto& sz : def.sizes) {
                    if (sz.cols == 2 && sz.rows == 2) { gallery_selected_size_ = {2, 2}; break; }
                }
            }
            Render();
            return;
        }
        if (act.starts_with("wg-sz:")) {
            const size_t xp = act.find('x', 6);
            if (xp != std::string::npos) {
                const int c = std::stoi(act.substr(6, xp - 6));
                const int r = std::stoi(act.substr(xp + 1));
                gallery_selected_size_ = {c, r};
                Render();
            }
            return;
        }
        if (act == "wg-add") {
            const auto wk = gallery_selected_kind_;
            const auto sz = gallery_selected_size_;
            const auto cell = gallery_target_cell_;
            gallery_target_cell_.reset();
            if (callbacks_.on_add_widget_tile) {
                callbacks_.on_add_widget_tile(wk, sz, cell);
            } else if (callbacks_.on_toggle_widget_tile) {
                callbacks_.on_toggle_widget_tile(wk);
                active_widget_kinds_.insert(static_cast<uint8_t>(wk));
                Render();
            }
            return;
        }
        if (act == "b-auto-acc") {
            beautify_settings_.auto_wallpaper_accent = !beautify_settings_.auto_wallpaper_accent;
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act.starts_with("b-acc:")) {
            static constexpr uint32_t kSwatches[6] = {
                0xFF4CC2FFu, 0xFFE68A6Eu, 0xFF5FD08Au, 0xFFA193FFu, 0xFFF5A524u, 0xFFE5484Du
            };
            const int idx = std::clamp(std::stoi(act.substr(6)), 0, 5);
            beautify_settings_.auto_wallpaper_accent = false;
            beautify_settings_.accent_argb = kSwatches[idx];
            accent_argb_ = kSwatches[idx];
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act.starts_with("b-mat:")) {
            beautify_settings_.default_material = static_cast<model::CardMaterial>(std::stoi(act.substr(6)));
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act.starts_with("b-gp:")) {
            const uint8_t p = static_cast<uint8_t>(std::clamp(std::stoi(act.substr(5)), 0, 5));
            model::ApplyGlassPreset(p, beautify_settings_);
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act.starts_with("b-disp:")) {
            const uint8_t tier = static_cast<uint8_t>(std::clamp(std::stoi(act.substr(7)), 0, 2));
            beautify_settings_.glass_dispersion = model::MapLegacyGlassDispersionToGain(tier);
            beautify_settings_.glass_preset = static_cast<uint8_t>(model::GlassPreset::Custom);
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act=="b-blur-edge") {
            beautify_settings_.glass_options.blur_edge=!beautify_settings_.glass_options.blur_edge;
            beautify_settings_.glass_reference=true;
            beautify_settings_.glass_preset=static_cast<uint8_t>(model::GlassPreset::Custom);
            NotifyBeautifyChanged(false);Render();return;
        }
        if (act.starts_with("b-rate:")) {
            beautify_settings_.glass_live_rate = static_cast<uint8_t>(std::clamp(std::stoi(act.substr(7)), 0, 2));
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act.starts_with("b-lbl:")) {
            beautify_settings_.label_style = static_cast<model::IconLabelStyle>(std::stoi(act.substr(6)));
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act.starts_with("b-sz:")) {
            beautify_settings_.icon_size = static_cast<model::DesktopIconSize>(std::stoi(act.substr(5)));
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act == "b-dock-en") {
            beautify_settings_.dock_enabled = !beautify_settings_.dock_enabled;
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act.starts_with("b-dpos:")) {
            beautify_settings_.dock_position = static_cast<uint8_t>(std::clamp(std::stoi(act.substr(7)), 0, 2));
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act.starts_with("b-dhide:")) {
            beautify_settings_.dock_hide_mode = static_cast<uint8_t>(std::clamp(std::stoi(act.substr(8)), 0, 2));
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act.starts_with("b-dmin:")) {
            beautify_settings_.dock_minimize_effect = static_cast<uint8_t>(std::clamp(std::stoi(act.substr(7)), 0, 2));
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act == "b-dsys") {
            beautify_settings_.dock_system_area = !beautify_settings_.dock_system_area;
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act == "b-dbl-hide") {
            beautify_settings_.double_click_hide_icons = !beautify_settings_.double_click_hide_icons;
            NotifyBeautifyChanged(false);
            Render();
            return;
        }
        if (act.starts_with("b-wid:")) {
            const uint8_t kind_u = static_cast<uint8_t>(std::stoi(act.substr(6)));
            if (active_widget_kinds_.contains(kind_u)) active_widget_kinds_.erase(kind_u);
            else active_widget_kinds_.insert(kind_u);
            if (callbacks_.on_toggle_widget_tile) {
                callbacks_.on_toggle_widget_tile(static_cast<model::TileKind>(kind_u));
            }
            Render();
            return;
        }
        if (act.starts_with("tray-rm:")) {
            const uint64_t id = std::stoull(act.substr(8));
            if (callbacks_.on_tray_remove_item) callbacks_.on_tray_remove_item(id);
            std::erase_if(tray_entries_, [id](const auto& e) { return e.item_id == id; });
            Render();
            return;
        }
        if (act == "tray-release") {
            Dismiss();
            if (callbacks_.on_tray_release_to_desktop) callbacks_.on_tray_release_to_desktop();
            return;
        }
        if (act == "tray-open-pulse") {
            Dismiss();
            if (callbacks_.on_tray_open_in_pulse) callbacks_.on_tray_open_in_pulse();
            return;
        }
        if (act == "tray-clear") {
            tray_entries_.clear();
            if (callbacks_.on_tray_clear) callbacks_.on_tray_clear();
            Render();
            return;
        }
        if (act.starts_with("rt:")) {
            for (const auto& c : kRuleTypeChips) {
                if (act != c.id) continue;
                if (c.is_folder_toggle) {
                    rule_group_.rule.match_folders = !rule_group_.rule.match_folders;
                } else {
                    const bool present = std::find(rule_group_.rule.extensions.begin(),
                                                   rule_group_.rule.extensions.end(), c.primary_ext) !=
                                         rule_group_.rule.extensions.end();
                    for (const wchar_t* e : c.exts) {
                        std::erase(rule_group_.rule.extensions, std::wstring(e));
                        if (!present) rule_group_.rule.extensions.emplace_back(e);
                    }
                }
                break;
            }
            Render();
            return;
        }
        if (act.starts_with("rtag:")) {
            const std::string sub = act.substr(5);
            if (sub == "clear") {
                rule_group_.rule.tag_id.clear();
            } else {
                const std::wstring wid = ipc::Utf8ToWide(sub);
                rule_group_.rule.tag_id = (rule_group_.rule.tag_id == wid) ? std::wstring{} : wid;
            }
            Render();
            return;
        }
        if (act.starts_with("rd:")) {
            rule_group_.rule.modified_within_days = std::stoi(act.substr(3));
            Render();
            return;
        }
        if (act == "rule-apply") {
            const std::string gid = rule_group_.id;
            const model::GroupRule updated = rule_group_.rule;
            Dismiss();
            if (callbacks_.on_group_rule_changed) callbacks_.on_group_rule_changed(gid, updated);
            return;
        }
        if (act == "note-clear") {
            sticky_note_text_.clear();
            if (popup_edit_hwnd_ && IsWindow(popup_edit_hwnd_)) {
                SetWindowTextW(popup_edit_hwnd_, L"");
                SetFocus(popup_edit_hwnd_);
            }
            if (callbacks_.on_sticky_note_changed &&
                sticky_tile_id_ != "@weather_city" && !sticky_tile_id_.starts_with("@title:")) {
                callbacks_.on_sticky_note_changed(sticky_tile_id_, sticky_note_text_);
            }
            Render();
            return;
        }
        if (act == "note-done") {
            if (popup_edit_hwnd_ && IsWindow(popup_edit_hwnd_)) {
                sticky_note_text_ = ReadHostedPopupEditText();
            }
            const std::string tid = sticky_tile_id_;
            const std::wstring txt = sticky_note_text_;
            Dismiss();
            if (callbacks_.on_sticky_note_changed) callbacks_.on_sticky_note_changed(tid, txt);
            return;
        }
    }

    if ((active_kind_ == OverlayKind::StickyNotePopover ||
         active_kind_ == OverlayKind::SearchPopup ||
         active_kind_ == OverlayKind::InlineRename) &&
        popup_edit_hwnd_ && IsWindow(popup_edit_hwnd_)) {
        SetFocus(popup_edit_hwnd_);
    }
}

void DesktopOverlayController::HandleKeyDown(WPARAM vk) {
    if (vk == VK_ESCAPE) {
        if (active_kind_ == OverlayKind::AiOrganizeDrawer &&
            ai_state_.phase == AiDrawerPhase::Loading &&
            callbacks_.on_ai_cancel) {
            callbacks_.on_ai_cancel();
        }
        if (active_kind_ == OverlayKind::StickyNotePopover && callbacks_.on_sticky_note_changed) {
            callbacks_.on_sticky_note_changed(sticky_tile_id_, sticky_note_text_);
        }
        Dismiss();
        return;
    }
    if (active_kind_ == OverlayKind::WidgetDetail && detail_.kind == model::TileKind::Calendar &&
        (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME)) {
        // ← → (or PageUp / PageDown) turn months; Home is today.
        if (vk == VK_HOME) { detail_month_offset_ = 0; detail_sel_year_ = detail_sel_month_ = detail_sel_day_ = 0; }
        else detail_month_offset_ += (vk == VK_RIGHT || vk == VK_NEXT) ? 1 : -1;
        Render();
        return;
    }

    if (vk == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
        (active_kind_ == OverlayKind::AiOrganizeDrawer || active_kind_ == OverlayKind::HealthDrawer)) {
        Dismiss();
        if (callbacks_.on_undo_requested) callbacks_.on_undo_requested();
        return;
    }

    if (active_kind_ == OverlayKind::StickyNotePopover) {
        if (vk == VK_BACK && !sticky_note_text_.empty()) {
            sticky_note_text_.pop_back();
            if (callbacks_.on_sticky_note_changed) {
                callbacks_.on_sticky_note_changed(sticky_tile_id_, sticky_note_text_);
            }
            Render();
        } else if (vk == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            const std::string tid = sticky_tile_id_;
            const std::wstring txt = sticky_note_text_;
            Dismiss();
            if (callbacks_.on_sticky_note_changed) callbacks_.on_sticky_note_changed(tid, txt);
        }
        return;
    }

    if (active_kind_ == OverlayKind::QuickLook) {
        if (vk == VK_SPACE) {
            Dismiss();
        } else if (vk == VK_LEFT && callbacks_.on_cycle_quick_look) {
            callbacks_.on_cycle_quick_look(-1);
        } else if (vk == VK_RIGHT && callbacks_.on_cycle_quick_look) {
            callbacks_.on_cycle_quick_look(+1);
        } else if (vk == VK_RETURN) {
            const std::wstring p = ql_item_.path;
            Dismiss();
            if (callbacks_.on_open_path) callbacks_.on_open_path(p);
        }
        return;
    }

    if (active_kind_ == OverlayKind::SearchPopup) {
        const int n_loc = static_cast<int>(std::min<size_t>(4, local_hits_.size()));
        const int n_rem = static_cast<int>(std::min<size_t>(3, remote_hits_.size()));
        const int total = n_loc + n_rem;
        if (vk == VK_DOWN && total > 0) {
            search_selected_idx_ = (search_selected_idx_ + 1) % total;
            Render();
        } else if (vk == VK_UP && total > 0) {
            search_selected_idx_ = (search_selected_idx_ + total - 1) % total;
            Render();
        } else if (vk == VK_BACK && !search_query_.empty()) {
            search_query_.pop_back();
            search_selected_idx_ = 0;
            if (callbacks_.on_search_query_changed) callbacks_.on_search_query_changed(search_query_);
            RepositionAndShow(card_screen_x_, card_screen_y_, 540, search_query_.empty() ? 188 : 424, false);
        } else if (vk == VK_RETURN) {
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 || total == 0) {
                const std::wstring q = search_query_;
                Dismiss();
                if (callbacks_.on_open_search_in_pulse) callbacks_.on_open_search_in_pulse(q);
            } else {
                std::vector<SearchHitEntry> combined;
                for (int i = 0; i < n_loc; ++i) combined.push_back(local_hits_[static_cast<size_t>(i)]);
                for (int i = 0; i < n_rem; ++i) combined.push_back(remote_hits_[static_cast<size_t>(i)]);
                if (search_selected_idx_ >= 0 && static_cast<size_t>(search_selected_idx_) < combined.size()) {
                    const std::wstring p = combined[static_cast<size_t>(search_selected_idx_)].path;
                    Dismiss();
                    if (callbacks_.on_open_path) callbacks_.on_open_path(p);
                }
            }
        }
    }
}

void DesktopOverlayController::HandleChar(wchar_t ch) {
    if (active_kind_ == OverlayKind::StickyNotePopover) {
        if (ch == L'\r') ch = L'\n';
        if ((ch >= 0x20 || ch == L'\n') && ch != 0x7F && sticky_note_text_.size() < 400) {
            sticky_note_text_.push_back(ch);
            if (callbacks_.on_sticky_note_changed) {
                callbacks_.on_sticky_note_changed(sticky_tile_id_, sticky_note_text_);
            }
            Render();
        }
        return;
    }

    if (active_kind_ != OverlayKind::SearchPopup) return;
    if (ch < 0x20 || ch == 0x7F) return;
    if (search_query_.size() >= 80) return;
    const bool was_empty = search_query_.empty();
    search_query_.push_back(ch);
    search_selected_idx_ = 0;
    if (callbacks_.on_search_query_changed) {
        callbacks_.on_search_query_changed(search_query_);
    }
    if (was_empty) {
        RepositionAndShow(card_screen_x_, card_screen_y_, 540, 424, false);
    } else {
        Render();
    }
}

LRESULT CALLBACK DesktopOverlayController::StaticWndProc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) noexcept {
    DesktopOverlayController* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<DesktopOverlayController*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<DesktopOverlayController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleWndProc(msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK DesktopOverlayController::StaticSubWndProc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) noexcept {
    DesktopOverlayController* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<DesktopOverlayController*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->sub_hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<DesktopOverlayController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleSubWndProc(msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT DesktopOverlayController::HandleWndProc(UINT msg, WPARAM wparam, LPARAM lparam) noexcept {
    const int pad = using_dcomp_ ? kShadowMargin : 0;
    switch (msg) {
    case WM_APP + 71:
        if (wparam == menu_generation_ && active_kind_ == OverlayKind::ContextMenu) TrackSharedMenu();
        return 0;
    case WM_APP + 72:
        CancelInlineRename(wparam != 0);
        return 0;
    case WM_APP + 73:
        Dismiss();
        return 0;
    case WM_SETFOCUS:
        if ((active_kind_ == OverlayKind::StickyNotePopover ||
             active_kind_ == OverlayKind::SearchPopup ||
             active_kind_ == OverlayKind::InlineRename) &&
            popup_edit_hwnd_ && IsWindow(popup_edit_hwnd_)) {
            SetFocus(popup_edit_hwnd_);
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        if (active_kind_ == OverlayKind::DockVolumeOsd) {
            return HTTRANSPARENT;
        }
        if (using_dcomp_ && hwnd_) {
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd_, &pt);
            const int dx = ToDip(pt.x) - pad;
            const int dy = ToDip(pt.y) - pad;
            if (dx < 0 || dy < 0 || dx >= card_w_ || dy >= card_h_) {
                return HTTRANSPARENT;
            }
        }
        return HTCLIENT;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Render();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        HandleMouseMove(ToDip(GET_X_LPARAM(lparam)) - pad, ToDip(GET_Y_LPARAM(lparam)) - pad);
        return 0;
    case WM_LBUTTONDOWN:
        HandleLeftClick(ToDip(GET_X_LPARAM(lparam)) - pad, ToDip(GET_Y_LPARAM(lparam)) - pad);
        return 0;
    case WM_LBUTTONUP:
        if (dock_vol_dragging_) {
            dock_vol_dragging_ = false;
            if (hwnd_ && GetCapture() == hwnd_) ReleaseCapture();
            if (callbacks_.on_dock_volume_changed) {
                callbacks_.on_dock_volume_changed(dock_panel_snap_.volume_pct, false);
            }
            Render();
            return 0;
        }
        if (!dragging_beautify_slider_.empty()) {
            dragging_beautify_slider_.clear();
            if (hwnd_ && GetCapture() == hwnd_) ReleaseCapture();
            NotifyBeautifyChanged(false);
            Render();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (dock_vol_dragging_ && reinterpret_cast<HWND>(lparam) != hwnd_) {
            dock_vol_dragging_ = false;
            if (callbacks_.on_dock_volume_changed) {
                callbacks_.on_dock_volume_changed(dock_panel_snap_.volume_pct, false);
            }
            Render();
        }
        if (!dragging_beautify_slider_.empty() && reinterpret_cast<HWND>(lparam) != hwnd_) {
            dragging_beautify_slider_.clear();
            NotifyBeautifyChanged(false);
            Render();
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (active_kind_ == OverlayKind::DockSystemPanel && dock_panel_kind_ == DockPanelKind::Clock) {
            dock_panel_month_offset_ += GET_WHEEL_DELTA_WPARAM(wparam) < 0 ? 1 : -1;
            Render();
            return 0;
        }
        if (active_kind_ == OverlayKind::DockSystemPanel && dock_panel_kind_ == DockPanelKind::Volume) {
            const int step = (GET_WHEEL_DELTA_WPARAM(wparam) > 0) ? 2 : -2;
            const int next = std::clamp(dock_panel_snap_.volume_pct + step, 0, 100);
            dock_panel_snap_.volume_pct = next;
            dock_panel_snap_.muted = (next == 0);
            if (callbacks_.on_dock_volume_changed) callbacks_.on_dock_volume_changed(next, false);
            Render();
            return 0;
        }
        if (active_kind_ == OverlayKind::WidgetDetail && detail_.kind == model::TileKind::Calendar) {
            // The wheel turns months, as a swipe does on a phone's calendar.
            detail_month_offset_ += GET_WHEEL_DELTA_WPARAM(wparam) < 0 ? 1 : -1;
            Render();
            return 0;
        }
        if (active_kind_ == OverlayKind::BeautifyDrawer && beautify_max_scroll_y_ > 0.0f) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            beautify_scroll_y_ = std::clamp(beautify_scroll_y_ - static_cast<float>(delta) * 0.45f,
                                            0.0f, beautify_max_scroll_y_);
            Render();
            return 0;
        }
        if (active_kind_ == OverlayKind::AiOrganizeDrawer &&
            ai_state_.phase == AiDrawerPhase::PlanReady &&
            ai_max_scroll_y_ > 0.0f) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            ai_scroll_y_ = std::clamp(ai_scroll_y_ - static_cast<float>(delta) * 0.45f,
                                      0.0f, ai_max_scroll_y_);
            Render();
            return 0;
        }
        if (active_kind_ == OverlayKind::WidgetGalleryDrawer) {
            const auto& def = FindGalleryWidgetDef(gallery_selected_kind_);
            std::vector<model::TileSize> list(def.sizes.begin(), def.sizes.end());
            if (list.size() > 1) {
                size_t idx = 0;
                for (size_t i = 0; i < list.size(); ++i) {
                    if (list[i] == gallery_selected_size_) { idx = i; break; }
                }
                const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
                idx = (delta < 0) ? ((idx + 1) % list.size()) : ((idx + list.size() - 1) % list.size());
                gallery_selected_size_ = list[idx];
                Render();
            }
            return 0;
        }
        {
            // The wheel goes to this thread's focus window, which is often this popup even with the cursor
            // over the desktop layer: hand it on to the layer window under the cursor (same thread) instead.
            const POINT at{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const HWND under = GetAncestor(WindowFromPoint(at), GA_ROOT);
            if (under && under != hwnd_ && GetWindowThreadProcessId(under, nullptr) == GetCurrentThreadId())
                return SendMessageW(under, msg, wparam, lparam);
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if (reinterpret_cast<HWND>(lparam) == popup_edit_hwnd_ && popup_edit_brush_) {
            const D2D1_COLOR_F fg = HostedPopupEditForeground();
            const HDC edit_dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(edit_dc, RGB(static_cast<int>(fg.r * 255.0f), static_cast<int>(fg.g * 255.0f), static_cast<int>(fg.b * 255.0f)));
            SetBkColor(edit_dc, dark_mode_ ? RGB(28, 32, 42) : RGB(250, 250, 252));
            return reinterpret_cast<LRESULT>(popup_edit_brush_);
        }
        break;
    case WM_KEYDOWN:
        HandleKeyDown(wparam);
        return 0;
    case WM_CHAR:
        HandleChar(static_cast<wchar_t>(wparam));
        return 0;
    case WM_ACTIVATE:
        if (!shared_menu_tracking_ && !popup_edit_ignore_killfocus_ &&
            !beautify_suppress_deactivate_ && !beautify_drop_hover_ &&
            LOWORD(wparam) == WA_INACTIVE &&
            reinterpret_cast<HWND>(lparam) != sub_hwnd_ &&
            (popup_edit_hwnd_ == nullptr || reinterpret_cast<HWND>(lparam) != popup_edit_hwnd_) &&
            active_kind_ != OverlayKind::ToastCapsule &&
            active_kind_ != OverlayKind::DockVolumeOsd &&
            active_kind_ != OverlayKind::None) {
            if (active_kind_ == OverlayKind::InlineRename) {
                CancelInlineRename(true);
            } else if (active_kind_ == OverlayKind::StickyNotePopover) {
                if (popup_edit_hwnd_ && IsWindow(popup_edit_hwnd_)) {
                    sticky_note_text_ = ReadHostedPopupEditText();
                }
                const std::string tid = sticky_tile_id_;
                const std::wstring txt = sticky_note_text_;
                Dismiss();
                if (callbacks_.on_sticky_note_changed) {
                    callbacks_.on_sticky_note_changed(tid, txt);
                }
            } else {
                Dismiss();
            }
        }
        return 0;
    case WM_TIMER:
        if (wparam == kToastTimerId || (wparam == 8804 && active_kind_ == OverlayKind::DockVolumeOsd)) {
            Dismiss();
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

LRESULT DesktopOverlayController::HandleSubWndProc(UINT msg, WPARAM wparam, LPARAM lparam) noexcept {
    const int pad = sub_using_dcomp_ ? kShadowMargin : 0;
    switch (msg) {
    case WM_NCHITTEST: {
        if (sub_using_dcomp_ && sub_hwnd_) {
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(sub_hwnd_, &pt);
            const int dx = ToDip(pt.x) - pad;
            const int dy = ToDip(pt.y) - pad;
            if (dx < 0 || dy < 0 || dx >= sub_card_w_ || dy >= sub_card_h_) {
                return HTTRANSPARENT;
            }
        }
        return HTCLIENT;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(sub_hwnd_, &ps);
        RenderSubmenu();
        EndPaint(sub_hwnd_, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        HandleSubmenuMouseMove(ToDip(GET_X_LPARAM(lparam)) - pad, ToDip(GET_Y_LPARAM(lparam)) - pad);
        return 0;
    case WM_LBUTTONDOWN:
        HandleSubmenuClick(ToDip(GET_X_LPARAM(lparam)) - pad, ToDip(GET_Y_LPARAM(lparam)) - pad);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(sub_hwnd_, msg, wparam, lparam);
}

} // namespace pulse::desktop::overlays

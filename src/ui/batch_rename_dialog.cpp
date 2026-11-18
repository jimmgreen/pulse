// batch_rename_dialog.cpp — 批量重命名: a stack of switchable steps on the
// left, the live old → new preview on the right. The rename engine lives in
// app/batch_rename_plan; this file only draws, edits the steps and hands the
// chain-safe list of renames back to the caller.
#include "edit_host.h"
#include "../common/windows_compat.h"
#include "batch_rename_dialog.h"
#include "../app/batch_rename_plan.h"
#include "../app/batch_rename_presets.h"
#include "../app/batch_rename_ai.h"
#include "../app/batch_rename_content.h"
#include "FluentTokens.h"
#include "fluent_components.h"
#include "svg_path_icon.h"
#include "typography.h"
#include "ui_compositor.h"
#include "window_helpers.h"
#include "surface_capture.h"
#include "../common/localization.h"

#include <commctrl.h>
#include <windowsx.h>
#include <propkey.h>
#include <propsys.h>
#include <shobjidl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace pulse::ui {
#if defined(PULSE_TEST_BATCH_RENAME_PRESENT)
void BatchRenameShownForTest(HWND hwnd);
float BatchRenameScaleForTest();
#endif
namespace {

namespace rn = app::rename;
using l10n::StringId;

constexpr wchar_t kClass[] = L"PulseBatchRenameWindow";
constexpr float kDlgW = 1064.0f;
constexpr float kDlgH = 672.0f;
constexpr float kTitleH = 52.0f;
constexpr float kBodyTop = 146.0f;    // title 52 + AI bar 48 + 12 + examples 26 + 8
constexpr float kFootH = 60.0f;
constexpr float kStepsW = 372.0f;
constexpr float kPaneHeadH = 44.0f;
constexpr float kPresetH = 44.0f;
constexpr float kRowH = 36.0f;
constexpr float kTallH = 52.0f;
constexpr UINT_PTR kAnimTimer = 1;
constexpr LONG_PTR kAiEditId = 100;   // GetDlgItem handle for the AI sentence field

std::wstring S(StringId id) { return l10n::Get(id); }

std::wstring FmtS(StringId id, ...) {
    wchar_t buf[512]{};
    va_list args;
    va_start(args, id);
    _vsnwprintf_s(buf, _TRUNCATE, l10n::Get(id).c_str(), args);
    va_end(args);
    return buf;
}

D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}
D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
D2D1_RECT_F R(float l, float t, float w, float h) { return D2D1::RectF(l, t, l + w, t + h); }
float EaseOut(float t) { t = std::clamp(t, 0.0f, 1.0f); const float u = 1.0f - t; return 1.0f - u * u * u; }
COLORREF Ref(D2D1_COLOR_F c) {
    return RGB(static_cast<int>(c.r * 255.0f + 0.5f), static_cast<int>(c.g * 255.0f + 0.5f),
               static_cast<int>(c.b * 255.0f + 0.5f));
}

// ------------------------------------------------------------------ palette

struct Pal {
    D2D1_COLOR_F accent, accent_h, accent_soft, accent_tint, on_accent;
    D2D1_COLOR_F bg, pane, card, glass, glass_pane, glass_sheet, line, line2, text, text2, text3, hover, field, field_line;
    D2D1_COLOR_F amber, amber_bg, amber_line, ok, del, del_bg, ins, ins_bg, ai, ai_bg, bad;
    D2D1_COLOR_F c_img, s_img, c_doc, s_doc, c_vid, s_vid;
};

Pal MakePal(bool dark, D2D1_COLOR_F accent) {
    Pal p{};
    accent.a = 1.0f;
    if (!dark) {
        p.bg = Hex(0xfdfcfb); p.pane = Hex(0xf7f7f3); p.card = Hex(0xffffff); p.line = Hex(0xe3e4dd);
        p.line2 = Hex(0xecede7); p.text = Hex(0x1f2328); p.text2 = Hex(0x5b626c); p.text3 = Hex(0x8d939b);
        p.hover = Hex(0xe9ebe4); p.field = Hex(0xffffff); p.field_line = Hex(0xdcded6);
        p.amber = Hex(0xa86c12); p.amber_bg = Hex(0xfbf0d9); p.amber_line = Hex(0xefd59f); p.ok = Hex(0x2f8a5b);
        p.del = Hex(0xb4442f); p.del_bg = Hex(0xfbe6e1); p.ins = Hex(0x1f6b52); p.ins_bg = Hex(0xd9efe4);
        p.ai = Hex(0x6b5bd2); p.ai_bg = Hex(0xeeebfb); p.bad = Hex(0xc0392b);
        p.c_img = Hex(0x8257c2); p.s_img = Hex(0xf1eafb); p.c_doc = Hex(0x2f6bc0); p.s_doc = Hex(0xe6effb);
        p.c_vid = Hex(0xc03f66); p.s_vid = Hex(0xfbe7ee);
        p.on_accent = Hex(0xffffff);
    } else {
        p.bg = Hex(0x1c1e1d); p.pane = Hex(0x222523); p.card = Hex(0x2a2d2b); p.line = Hex(0x353936);
        p.line2 = Hex(0x2d302e); p.text = Hex(0xe6e8e5); p.text2 = Hex(0xb1b6b0); p.text3 = Hex(0x858b85);
        p.hover = Hex(0x333734); p.field = Hex(0x232624); p.field_line = Hex(0x3c403d);
        p.amber = Hex(0xe3ab52); p.amber_bg = Hex(0x3a3020); p.amber_line = Hex(0x5e4a26); p.ok = Hex(0x5fbf8a);
        p.del = Hex(0xf09a88); p.del_bg = Hex(0x46291f); p.ins = Hex(0x8fd6b4); p.ins_bg = Hex(0x203a2f);
        p.ai = Hex(0xb3a8f5); p.ai_bg = Hex(0x2f2a48); p.bad = Hex(0xf08a7a);
        p.c_img = Hex(0xb393e6); p.s_img = Hex(0x352c45); p.c_doc = Hex(0x7eaaf0); p.s_doc = Hex(0x253248);
        p.c_vid = Hex(0xee86a6); p.s_vid = Hex(0x43262f);
        p.on_accent = Hex(0x10201a);
    }
    p.glass = p.card;
    p.glass_pane = p.pane;
    p.glass_sheet = Hex(0x000000, 0.0f);
    p.accent = accent;
    p.accent_h = dark ? Mix(accent, Hex(0xffffff), 0.12f) : Mix(accent, Hex(0x000000), 0.14f);
    p.accent_soft = Mix(p.card, accent, dark ? 0.20f : 0.13f);
    p.accent_tint = Mix(p.card, accent, dark ? 0.10f : 0.07f);
    return p;
}

// ------------------------------------------------------------------ icons

constexpr SvgPart kIcoRename[] = {
    {"M3 6.5h7M3 10h4.5M3 13.5h6", false, 1.5f, 1.0f},
    {"M12.8 14.6l4-4a1.3 1.3 0 0 0-1.9-1.9l-4 4-.6 2.5z", false, 1.5f, 1.0f}};
constexpr SvgPart kIcoList[] = {
    {"M5.5 4h8M5.5 8h8M5.5 12h8", false, 1.4f, 1.0f},
    {"M2 4a.6.6 0 1 0 1.2 0a.6.6 0 1 0-1.2 0zM2 8a.6.6 0 1 0 1.2 0a.6.6 0 1 0-1.2 0zM2 12a.6.6 0 1 0 1.2 0a.6.6 0 1 0-1.2 0z", true, 0.0f, 1.0f}};
constexpr SvgPart kIcoSpark[] = {
    {"M10 1.8c.4 3.9 2.3 5.8 6.2 6.2-3.9.4-5.8 2.3-6.2 6.2-.4-3.9-2.3-5.8-6.2-6.2 3.9-.4 5.8-2.3 6.2-6.2z", true, 0.0f, 1.0f},
    {"M15.6 12.6c.2 1.9 1.1 2.8 3 3-1.9.2-2.8 1.1-3 3-.2-1.9-1.1-2.8-3-3 1.9-.2 2.8-1.1 3-3z", true, 0.0f, 0.7f}};
constexpr SvgPart kIcoChev[] = {{"M3 4.5l3 3 3-3", false, 1.4f, 1.0f}};
constexpr SvgPart kIcoSend[] = {{"M8 13V3.5M4 7.5l4-4 4 4", false, 1.7f, 1.0f}};
constexpr SvgPart kIcoStop[] = {{"M6 4.5h4a1.5 1.5 0 0 1 1.5 1.5v4a1.5 1.5 0 0 1-1.5 1.5H6a1.5 1.5 0 0 1-1.5-1.5V6A1.5 1.5 0 0 1 6 4.5z", true, 0.0f, 1.0f}};
constexpr SvgPart kIcoGrip[] = {
    {"M4 3.5a1 1 0 1 0 2 0a1 1 0 1 0-2 0zM8 3.5a1 1 0 1 0 2 0a1 1 0 1 0-2 0zM4 7a1 1 0 1 0 2 0a1 1 0 1 0-2 0zM8 7a1 1 0 1 0 2 0a1 1 0 1 0-2 0zM4 10.5a1 1 0 1 0 2 0a1 1 0 1 0-2 0zM8 10.5a1 1 0 1 0 2 0a1 1 0 1 0-2 0z", true, 0.0f, 1.0f}};
constexpr SvgPart kIcoReplace[] = {{"M2.5 5.5h8.5M8.5 3l2.5 2.5L8.5 8M13.5 10.5H5M7.5 8L5 10.5 7.5 13", false, 1.4f, 1.0f}};
constexpr SvgPart kIcoErase[] = {
    {"M6.2 13.5h7M3 9.8l5.6-5.6a1.5 1.5 0 0 1 2.1 0l1.9 1.9a1.5 1.5 0 0 1 0 2.1L8 13.5H5.6z", false, 1.4f, 1.0f},
    {"M6 6.8l3.2 3.2", false, 1.4f, 1.0f}};
constexpr SvgPart kIcoCase[] = {
    {"M1.8 12.5L5 3.5l3.2 9M2.9 9.5h4.2", false, 1.4f, 1.0f},
    {"M9.5 10.3a2.3 2.3 0 1 0 4.6 0a2.3 2.3 0 1 0-4.6 0z", false, 1.4f, 1.0f},
    {"M14.1 7.8v4.7", false, 1.4f, 1.0f}};
constexpr SvgPart kIcoTpl[] = {{"M5.5 2.5c-1.6 0-2 .7-2 2v1.6c0 .9-.5 1.4-1.5 1.9 1 .5 1.5 1 1.5 1.9v1.6c0 1.3.4 2 2 2M10.5 2.5c1.6 0 2 .7 2 2v1.6c0 .9.5 1.4 1.5 1.9-1 .5-1.5 1-1.5 1.9v1.6c0 1.3-.4 2-2 2", false, 1.4f, 1.0f}};
constexpr SvgPart kIcoExt[] = {
    {"M4 1.8h5L12.5 5.3V14a.7.7 0 0 1-.7.7H4a.7.7 0 0 1-.7-.7V2.5a.7.7 0 0 1 .7-.7z", false, 1.4f, 1.0f},
    {"M5.5 11a.9.9 0 1 0 1.8 0a.9.9 0 1 0-1.8 0z", true, 0.0f, 1.0f},
    {"M8.5 11h2", false, 1.4f, 1.0f}};
constexpr SvgPart kIcoInsert[] = {{"M2.5 3.5h6M5.5 3.5v9M10 9h4.5M12.25 6.75v4.5", false, 1.4f, 1.0f}};
constexpr SvgPart kIcoNumber[] = {{"M6.3 2.5l-1.6 11M11.3 2.5l-1.6 11M2.8 6h11M2.2 10h11", false, 1.4f, 1.0f}};
constexpr SvgPart kIcoArrow[] = {{"M2 6h7.5M6.5 3l3 3-3 3", false, 1.4f, 1.0f}};
constexpr SvgPart kIcoCheck[] = {{"M3.5 8.4l3 3 6-6.6", false, 1.8f, 1.0f}};
constexpr SvgPart kIcoWarn[] = {{"M8 2.3l6.2 11H1.8z", false, 1.4f, 1.0f}, {"M8 6.8v3M8 11.6v.1", false, 1.4f, 1.0f}};
constexpr SvgPart kIcoDash[] = {{"M4.5 8h7", false, 1.6f, 1.0f}};
constexpr SvgPart kIcoShield[] = {
    {"M8 1.8l5.2 2v4.1c0 3.2-2.2 5.3-5.2 6.3-3-1-5.2-3.1-5.2-6.3V3.8z", false, 1.4f, 1.0f},
    {"M5.8 8.2l1.6 1.6 3-3.2", false, 1.4f, 1.0f}};
constexpr SvgPart kIcoInfo[] = {
    {"M1.8 8a6.2 6.2 0 1 0 12.4 0a6.2 6.2 0 1 0-12.4 0z", false, 1.4f, 1.0f},
    {"M8 7.2v4M8 4.9v.1", false, 1.4f, 1.0f}};
constexpr SvgPart kIcoX[] = {{"M3 3l8 8M11 3l-8 8", false, 1.3f, 1.0f}};
constexpr SvgPart kIcoPlus[] = {{"M10 4v12M4 10h12", false, 1.6f, 1.0f}};
constexpr SvgPart kIcoFtImg[] = {{"M2 3.2A1.2 1.2 0 0 1 3.2 2h7.6A1.2 1.2 0 0 1 12 3.2v7.6a1.2 1.2 0 0 1-1.2 1.2H3.2A1.2 1.2 0 0 1 2 10.8zm1.3 7.4h7.4L8.2 7.3 6.4 9.6 5.2 8.2z", true, 0.0f, 1.0f}};
constexpr SvgPart kIcoFtDoc[] = {{"M3.5 1.5h4.6l3 3v7.2a.8.8 0 0 1-.8.8H3.5a.8.8 0 0 1-.8-.8V2.3a.8.8 0 0 1 .8-.8zM4.6 7.2v1h4.8v-1zm0 2.2v1h3.6v-1z", true, 0.0f, 1.0f}};
constexpr SvgPart kIcoFtVid[] = {{"M2 4a1.2 1.2 0 0 1 1.2-1.2h5.6A1.2 1.2 0 0 1 10 4v6a1.2 1.2 0 0 1-1.2 1.2H3.2A1.2 1.2 0 0 1 2 10zm8.6 1.6L12.5 4v6l-1.9-1.6z", true, 0.0f, 1.0f}};

struct Icon { const SvgPart* parts; size_t count; float view; };
#define BR_ICON(arr, view) Icon{arr, std::size(arr), view}
const Icon kRename = BR_ICON(kIcoRename, 20), kList = BR_ICON(kIcoList, 16), kSpark = BR_ICON(kIcoSpark, 20),
           kChev = BR_ICON(kIcoChev, 12), kSend = BR_ICON(kIcoSend, 16), kStop = BR_ICON(kIcoStop, 16),
           kGrip = BR_ICON(kIcoGrip, 14), kReplace = BR_ICON(kIcoReplace, 16), kErase = BR_ICON(kIcoErase, 16),
           kCase = BR_ICON(kIcoCase, 16), kTpl = BR_ICON(kIcoTpl, 16), kExt = BR_ICON(kIcoExt, 16), kInsert = BR_ICON(kIcoInsert, 16), kNumber = BR_ICON(kIcoNumber, 16),
           kArrow = BR_ICON(kIcoArrow, 12), kCheck = BR_ICON(kIcoCheck, 16), kWarn = BR_ICON(kIcoWarn, 16),
           kDash = BR_ICON(kIcoDash, 16), kShield = BR_ICON(kIcoShield, 16), kInfo = BR_ICON(kIcoInfo, 16),
           kX = BR_ICON(kIcoX, 14), kPlus = BR_ICON(kIcoPlus, 20), kFtImg = BR_ICON(kIcoFtImg, 14),
           kFtDoc = BR_ICON(kIcoFtDoc, 14), kFtVid = BR_ICON(kIcoFtVid, 14);
#undef BR_ICON

// ------------------------------------------------------------------ model

enum FieldId { kFind = 0, kWith = 1, kCustom = 0, kPattern = 0, kStart = 1, kDigits = 2, kStride = 3, kExtTo = 0,
               kText = 0, kAt = 1, kSep = 0 };

// Step types in menu / chip order, plus the per-type icon and title.
constexpr rn::StepType kStepOrder[] = {rn::StepType::Replace, rn::StepType::Remove, rn::StepType::Insert,
                                       rn::StepType::Case, rn::StepType::Template, rn::StepType::Number,
                                       rn::StepType::Ext};
const Icon& StepIcon(rn::StepType t) {
    switch (t) {
    case rn::StepType::Replace: return kReplace;
    case rn::StepType::Remove: return kErase;
    case rn::StepType::Case: return kCase;
    case rn::StepType::Template: return kTpl;
    case rn::StepType::Ext: return kExt;
    case rn::StepType::Insert: return kInsert;
    case rn::StepType::Number: return kNumber;
    }
    return kReplace;
}
StringId StepTitle(rn::StepType t) {
    switch (t) {
    case rn::StepType::Replace: return StringId::BrStepReplace;
    case rn::StepType::Remove: return StringId::BrStepRemove;
    case rn::StepType::Case: return StringId::BrStepCase;
    case rn::StepType::Template: return StringId::BrStepTemplate;
    case rn::StepType::Ext: return StringId::BrStepExt;
    case rn::StepType::Insert: return StringId::BrStepInsert;
    case rn::StepType::Number: return StringId::BrStepNumber;
    }
    return StringId::BrStepReplace;
}
constexpr int kSegWhere = 100;   // Hit.b offset: Seg items that set Step::where

struct StepUi {
    int id = 0;
    bool custom_on = false;   // 删除 → 自定义… chip
    std::wstring custom_text; // kept while the chip is off
};

enum class H : uint8_t {
    None, Close, ModeRules, ModeContent, AiInput, AiSend, AiModel, Example, AddStep, AddItem,
    Grip, Toggle, Delete, Card, Field, Chip, Seg, Token, PresetDD, PresetSave, PresetItem, PresetDel, PresetOk,
    PresetCancel, Tab, OrderDD,
    OrderItem, OrderDaily, Row, ExtToggle, PolicyDD, PolicyItem, Cancel, Apply, Menu, StepsArea, Preview,
    CFormat, CLang, CRange, CStart, CRegen, CPick, CName,   // 按内容命名
};

struct Hit {
    H k = H::None;
    int a = -1;
    int b = -1;
    bool operator==(const Hit& o) const { return k == o.k && a == o.a && b == o.b; }
    bool operator!=(const Hit& o) const { return !(*this == o); }
};

struct HitRect { D2D1_RECT_F r; Hit h; };

enum class Menu { None, AddStep, Order, Policy, Preset };
constexpr int kAiField = -1, kPresetField = -2, kRowField = -3;   // kRowField: field = row index

struct Field {
    HWND hwnd = nullptr;
    int step_id = -1;         // -1 = the AI bar, -2 = the preset name
    int field = 0;
    bool numeric = false;
    D2D1_RECT_F want{};       // field box in client px
    bool want_visible = false;
};

struct Anim { float from, to; ULONGLONG start; float dur; };


enum class Fmt {
    T15b, T14, T135, T135m, T135b, T13, T13m, T13b, T125, T125m, T125b, T12, T12m, T12b,
    T115, T115m, T115b, T11, T11b, Count
};

bool IsImageExt(const std::wstring& ext) {
    static const wchar_t* k[] = {L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".webp", L".heic", L".heif",
                                 L".tif", L".tiff", L".raw", L".cr2", L".cr3", L".nef", L".arw", L".dng",
                                 L".svg", L".ico", L".avif", L".jxl"};
    for (auto* e : k) if (_wcsicmp(ext.c_str(), e) == 0) return true;
    return false;
}
bool IsVideoExt(const std::wstring& ext) {
    static const wchar_t* k[] = {L".mp4", L".mov", L".mkv", L".avi", L".wmv", L".webm", L".m4v", L".3gp",
                                 L".ts", L".mts", L".m2ts"};
    for (auto* e : k) if (_wcsicmp(ext.c_str(), e) == 0) return true;
    return false;
}
std::wstring ExtOf(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? std::wstring() : name.substr(dot);
}
std::wstring FileNameOf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 1 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}
std::wstring ParentOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}
uint64_t Ticks(const FILETIME& ft) { return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }
uint64_t LocalTicks(const FILETIME& utc) {
    FILETIME local{};
    if (!FileTimeToLocalFileTime(&utc, &local)) return Ticks(utc);
    return Ticks(local);
}

// Photo metadata ({拍摄日期}, {尺寸}) read off the UI thread. The job is shared
// with its worker, which stops early once the dialog cancels it.
constexpr UINT kMetaMsg = WM_APP + 0x31;   // wparam 1 = done, 0 = progress
constexpr UINT kReviewReadyMsg = WM_APP + 0x34;
constexpr UINT kAiMsg = WM_APP + 0x32;     // the AI request finished
struct MetaJob {
    bool taken = false, dims = false;
    std::vector<std::wstring> paths;   // media files only
    std::vector<size_t> source;        // their index in sources_
    std::vector<uint64_t> taken_out;
    std::vector<uint32_t> width_out, height_out;
    std::atomic<int> done{0};
    std::atomic<bool> cancel{false};
};

void ReadMeta(const std::shared_ptr<MetaJob>& job, HWND hwnd) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ULONGLONG last_post = GetTickCount64();
    for (size_t i = 0; i < job->paths.size() && !job->cancel.load(); ++i) {
        IPropertyStore* store = nullptr;
        const std::wstring& path = job->paths[i];
        // The shell parses no \\?\ paths (tabs keep the prefix).
        std::wstring shell_path = path;
        if (shell_path.rfind(L"\\\\?\\UNC\\", 0) == 0) shell_path = L"\\\\" + shell_path.substr(8);
        else if (shell_path.rfind(L"\\\\?\\", 0) == 0) shell_path.erase(0, 4);
        if (SUCCEEDED(SHGetPropertyStoreFromParsingName(shell_path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store)))) {
            const bool video = IsVideoExt(ExtOf(path));
            PROPVARIANT v;
            if (job->taken) {
                PropVariantInit(&v);
                if (SUCCEEDED(store->GetValue(PKEY_Photo_DateTaken, &v)) && v.vt == VT_FILETIME)
                    job->taken_out[i] = LocalTicks(v.filetime);
                PropVariantClear(&v);
                if (!job->taken_out[i] && video) {
                    PropVariantInit(&v);
                    if (SUCCEEDED(store->GetValue(PKEY_Media_DateEncoded, &v)) && v.vt == VT_FILETIME)
                        job->taken_out[i] = LocalTicks(v.filetime);
                    PropVariantClear(&v);
                }
            }
            if (job->dims) {
                const PROPERTYKEY* keys[2] = {&PKEY_Image_HorizontalSize, &PKEY_Image_VerticalSize};
                if (video) { keys[0] = &PKEY_Video_FrameWidth; keys[1] = &PKEY_Video_FrameHeight; }
                uint32_t* out[2] = {&job->width_out[i], &job->height_out[i]};
                for (int k = 0; k < 2; ++k) {
                    PropVariantInit(&v);
                    if (SUCCEEDED(store->GetValue(*keys[k], &v)) && v.vt == VT_UI4) *out[k] = v.ulVal;
                    PropVariantClear(&v);
                }
            }
            store->Release();
        }
        job->done.store(static_cast<int>(i + 1));
        const ULONGLONG now = GetTickCount64();
        if (hwnd && now - last_post >= 120) { last_post = now; PostMessageW(hwnd, kMetaMsg, 0, 0); }
    }
    if (SUCCEEDED(init)) CoUninitialize();
    if (hwnd && !job->cancel.load()) PostMessageW(hwnd, kMetaMsg, 1, 0);
}

// 按内容命名: text read off the UI thread through Options::read_texts. Each
// file read posts kContentMsg (wparam 0); the end of reading posts wparam 1;
// a finished model reply wparam 2. lparam carries the run so stale posts
// from a stopped run are ignored.
constexpr UINT kContentMsg = WM_APP + 0x33;
struct ContentJob {
    std::vector<std::wstring> paths;
    std::vector<size_t> source;                           // index in sources_
    std::mutex m;
    std::vector<std::pair<size_t, std::wstring>> ready;   // (k, text), guarded by m
    std::atomic<size_t> done{0};
    std::atomic<bool> cancel{false};
};

// "**bold**" markers in catalog strings -> runs.
struct Run { std::wstring text; bool bold; };
std::vector<Run> Rich(const std::wstring& s) {
    std::vector<Run> out;
    bool bold = false;
    size_t i = 0;
    while (i <= s.size()) {
        const size_t m = s.find(L"**", i);
        const size_t end = m == std::wstring::npos ? s.size() : m;
        if (end > i) out.push_back({s.substr(i, end - i), bold});
        if (m == std::wstring::npos) break;
        bold = !bold;
        i = m + 2;
    }
    return out;
}

// ------------------------------------------------------------------ window

class BatchRenameWindow {
public:
    ~BatchRenameWindow() { ReleaseFormats(); }

    BatchRenameDialogResult Show(HWND owner, const std::vector<std::wstring>& paths, bool dark,
                                 D2D1_COLOR_F accent, const BatchRenameDialogOptions& options) {
        owner_ = owner;
        paths_ = paths;
        dark_ = dark;
        accent_ = accent;
        options_ = options;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner ? owner : GetDesktopWindow())) / 96.0f;
#if defined(PULSE_TEST_BATCH_RENAME_PRESENT)
        scale_ = BatchRenameScaleForTest();
#endif
        result_ = {};
        if (options_.proposed_names) {
            if (!options_.proposed_plan || options_.proposed_sources.size() != paths_.size() ||
                options_.proposed_names->size() != paths_.size()) return result_;
            sources_ = options_.proposed_sources;
            plan_ = *options_.proposed_plan;
            if (plan_.rows.size() != sources_.size()) return result_;
            folder_label_ = sources_.empty() ? std::wstring() : sources_.front().folder;
            opts_.policy = rn::Policy::Block;
        } else LoadSources();
#ifndef PULSE_TEST_BATCH_RENAME_PRESENT
        if (!options_.proposed_names) store_ = rn::LoadPresets();
#endif
        // A fresh dialog opens with the cleanup most people start from (the
        // design's first scene), or with the steps it was handed.
        if (options_.proposed_names) {
            steps_.clear();
        } else if (options_.preset) {
            rn::Preset p = *options_.preset;
            for (auto& st : p.steps) {
                st.pattern = UiTokens(st.pattern);
                st.text = UiTokens(st.text);
            }
            ApplyPreset(p, false);
        } else {
            ApplyPreset(Builtins().front(), false);
        }
        Recompute(false);

        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!GetClassInfoExW(wc.hInstance, kClass, &wc)) RegisterClassExW(&wc);

        // Fit the work area on small screens; the preview column absorbs it.
        float w = kDlgW, h = kDlgH;
        if (HMONITOR mon = MonitorFromWindow(owner ? owner : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST)) {
            MONITORINFO mi{sizeof(mi)};
            if (GetMonitorInfoW(mon, &mi)) {
                const float aw = static_cast<float>(mi.rcWork.right - mi.rcWork.left) / scale_ - 24.0f;
                const float ah = static_cast<float>(mi.rcWork.bottom - mi.rcWork.top) / scale_ - 24.0f;
                w = std::clamp(aw, 820.0f, kDlgW);
                h = std::clamp(ah, 520.0f, kDlgH);
            }
        }
        const int width = static_cast<int>(w * scale_);
        const int height = static_cast<int>(h * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClass, S(StringId::BatchRename).c_str(),
                                WS_POPUP | WS_THICKFRAME | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                                owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return result_;
        pulse::ui::CenterOwnedWindow(hwnd_, owner_, width, height);
        if (owner_) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);   // first frame places the step fields
#if defined(PULSE_TEST_BATCH_RENAME_PRESENT)
        BatchRenameShownForTest(hwnd_);
#endif
        SetForegroundWindow(hwnd_);
        if (!steps_ui_.empty()) {
            if (Field* f = FindField(steps_ui_.front().id, kFind)) SetFocus(f->hwnd);
        }

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        done_ = true;
        if (IsWindow(hwnd_)) HideComposedDialog(hwnd_, owner_);
        DestroyFields();
        if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        if (owner_) EnableWindow(owner_, TRUE);
        return result_;
    }

private:
    // ---------------------------------------------------------------- data

    void LoadSources() {
        sources_.clear();
        std::unordered_map<std::wstring, std::unordered_set<std::wstring>> selected;  // parent -> names
        for (const auto& p : paths_) {
            rn::Source s;
            s.path = p;
            s.name = FileNameOf(p);
            s.folder = FileNameOf(ParentOf(p));
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) s.modified = LocalTicks(fad.ftLastWriteTime);
            sources_.push_back(std::move(s));
            selected[rn::Lower(ParentOf(p))].insert(rn::Lower(FileNameOf(p)));
        }
        folder_label_ = sources_.empty() ? std::wstring() : sources_.front().folder;
        c_lang_ = pulse::l10n::effective_language() == pulse::l10n::Language::EnUS ? 1 : 0;
        // Names already in each folder that are not part of the selection.
        existing_.clear();
        folder_samples_.clear();
        for (const auto& kv : selected) {
            auto& names = existing_[kv.first];
            const std::wstring dir = ParentOf(paths_[IndexWithParent(kv.first)]);
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                        nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2])))
                    continue;
                std::wstring lower = rn::Lower(fd.cFileName);
                if (kv.second.count(lower)) continue;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && folder_samples_.size() < 40)
                    folder_samples_.push_back(fd.cFileName);
                names.insert(std::move(lower));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    size_t IndexWithParent(const std::wstring& lower_parent) const {
        for (size_t i = 0; i < paths_.size(); ++i)
            if (rn::Lower(ParentOf(paths_[i])) == lower_parent) return i;
        return 0;
    }

    // Date taken and pixel size come from the shell property store, read
    // once and only when a step or the numbering order needs them.
    // Starts the background read when a step first needs taken dates or
    // sizes. Until it lands, rows fall back to the modified date, show 读取中
    // and 重命名 waits.
    void EnsureMetadata(bool taken, bool dims) {
        const bool want_taken = taken && !taken_loaded_, want_dims = dims && !dims_loaded_;
        if ((!want_taken && !want_dims) || meta_job_) return;
        auto job = std::make_shared<MetaJob>();
        job->taken = want_taken;
        job->dims = want_dims;
        meta_rank_.assign(sources_.size(), -1);
        for (size_t i = 0; i < sources_.size(); ++i) {
            const std::wstring ext = ExtOf(sources_[i].name);
            if (!IsImageExt(ext) && !IsVideoExt(ext)) continue;
            meta_rank_[i] = static_cast<int>(job->paths.size());
            job->paths.push_back(sources_[i].path);
            job->source.push_back(i);
        }
        if (job->paths.empty()) {
            if (want_taken) taken_loaded_ = true;
            if (want_dims) dims_loaded_ = true;
            return;
        }
        job->taken_out.assign(job->paths.size(), 0);
        job->width_out.assign(job->paths.size(), 0);
        job->height_out.assign(job->paths.size(), 0);
        if (!hwnd_) {
            // First Recompute, before the window exists: read inline so the
            // dialog opens with real names.
            ReadMeta(job, nullptr);
            StoreMeta(*job);
            if (want_taken) taken_loaded_ = true;
            if (want_dims) dims_loaded_ = true;
            return;
        }
        meta_job_ = job;
        std::thread([job, hwnd = hwnd_] { ReadMeta(job, hwnd); }).detach();
    }

    void StoreMeta(const MetaJob& job) {
        for (size_t k = 0; k < job.source.size(); ++k) {
            rn::Source& src = sources_[job.source[k]];
            if (job.taken) src.taken = job.taken_out[k];
            if (job.dims) { src.width = job.width_out[k]; src.height = job.height_out[k]; }
        }
    }

    void OnMeta(bool finished) {
        if (!meta_job_) return;
        if (!finished) { InvalidateRect(hwnd_, nullptr, FALSE); return; }
        StoreMeta(*meta_job_);
        if (meta_job_->taken) taken_loaded_ = true;
        if (meta_job_->dims) dims_loaded_ = true;
        meta_job_.reset();
        Recompute(true);   // may start the other half (sizes after dates)
    }

    bool MetaPending(size_t source) const {
        return meta_job_ && source < meta_rank_.size() && meta_rank_[source] >= meta_job_->done.load();
    }

    void Recompute(bool flash) {
        if (!options_.proposed_names && !content_mode_) EnsureMetadata(rn::NeedsTakenDate(steps_, opts_), rn::NeedsDimensions(steps_));
        std::vector<std::wstring> before;
        if (flash) {
            before.reserve(plan_.rows.size());
            for (const auto& r : plan_.rows) before.push_back(r.new_name);
        }
        std::unordered_map<std::wstring, std::wstring> parent_keys;
        parent_keys.reserve(sources_.size());
        for (const auto& source : sources_)
            parent_keys.emplace(source.path, rn::Lower(ParentOf(source.path)));
        const auto exists = [this, &parent_keys](const std::wstring& source, const std::wstring& lower) {
            const auto parent = parent_keys.find(source);
            if (parent == parent_keys.end()) return false;
            auto it = existing_.find(parent->second);
            return it != existing_.end() && it->second.count(lower) > 0;
        };
        if (options_.proposed_names) {
            plan_ = *options_.proposed_plan;
        } else if (content_mode_) {
            // Checked suggestions take the same checks (clashes, validity, order) as steps.
            std::vector<std::wstring> names(sources_.size());
            for (size_t i = 0; i < sources_.size() && i < crows_.size(); ++i)
                if (crows_[i].st == CSt::Ok && crows_[i].pick) names[i] = crows_[i].name + ExtOf(sources_[i].name);
            plan_ = rn::RunNames(sources_, names, opts_, exists);
        } else {
            plan_ = rn::Run(sources_, steps_, opts_, exists);
        }
        diffs_.assign(plan_.rows.size(), {});
        changed_ = same_ = issues_ = 0;
        for (const auto& r : plan_.rows) {
            changed_ += RowIn(r, 1) ? 1 : 0;
            issues_ += RowIn(r, 2) ? 1 : 0;
            same_ += RowIn(r, 3) ? 1 : 0;
        }
        const ULONGLONG now = GetTickCount64();
        if (flash && before.size() == plan_.rows.size())
            for (size_t i = 0; i < plan_.rows.size(); ++i)
                if (before[i] != plan_.rows[i].new_name) flash_[i] = now;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    static bool IsIssue(const rn::Row& r) {
        switch (r.status) {
        case rn::RowStatus::Conflict: case rn::RowStatus::Skipped: case rn::RowStatus::Invalid:
        case rn::RowStatus::Empty: case rn::RowStatus::Fixed: case rn::RowStatus::Info: return true;
        default: return false;
        }
    }

    bool RowVisible(const rn::Row& r) const { return RowIn(r, filter_); }
    bool RowIn(const rn::Row& r, int tab) const {
        if (content_mode_ && r.src < crows_.size()) {
            const CRow& c = crows_[r.src];
            switch (tab) {
            case 1: return r.status != rn::RowStatus::Same;
            case 2: return IsIssue(r) || c.st == CSt::Fail;
            case 3: return c.st == CSt::Keep || c.st == CSt::Skip || (c.st == CSt::Ok && !c.pick);
            default: return true;
            }
        }
        switch (tab) {
        case 1: return r.status != rn::RowStatus::Same && r.status != rn::RowStatus::Skipped;
        case 2: return IsIssue(r);
        case 3: return r.status == rn::RowStatus::Same || r.status == rn::RowStatus::Skipped;
        default: return true;
        }
    }

    bool CanApply() const {
        return plan_.renames > 0 && !plan_.blocked && !meta_job_ && !(content_mode_ && ContentRunning());
    }

    void Complete(bool accepted) {
        if (executing_) return;
        if (accepted) {
            if (options_.review_valid && !options_.review_valid()) { Complete(false); return; }
            CommitRowEdit(true);
            if (!CanApply()) return;
            result_.accepted = true;
            if (!options_.proposed_names && !content_mode_) {
                store_.last = {S(StringId::BrPresetLast), steps_, opts_};
                store_.has_last = true;
                SaveStore();
            }
            for (size_t i : plan_.exec_order) {
                app::BatchRenameItem item;
                item.source_path = sources_[i].path;
                item.original_name = sources_[i].name;
                item.new_name = plan_.rows[i].new_name;
                item.status = app::BatchRenameStatus::Ok;
                result_.items.push_back(std::move(item));
            }
            if (options_.execute && options_.poll && hwnd_) {
                exec_task_ = options_.execute(result_.items);
                if (exec_task_) {
                    result_.executed = true;
                    result_.task = exec_task_;
                    executing_ = true;
                    exec_start_ = GetTickCount64();
                    exec_rank_.assign(plan_.rows.size(), -1);
                    for (size_t k = 0; k < plan_.exec_order.size(); ++k)
                        exec_rank_[plan_.exec_order[k]] = static_cast<int>(k);
                    menu_ = Menu::None;
                    for (auto& f : fields_) EnableWindow(f.hwnd, FALSE);
                    SetFocus(hwnd_);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
                result_.accepted = false;   // nothing started: leave the names alone
                result_.items.clear();
            }
        }
        Close();
    }

    void Close() {
        done_ = true;
        if (ai_task_) { ai_task_->Cancel(); ai_task_.reset(); }
        if (meta_job_) { meta_job_->cancel.store(true); meta_job_.reset(); }
        if (c_job_) { c_job_->cancel.store(true); c_job_.reset(); }
        if (c_task_) { c_task_->Cancel(); c_task_.reset(); }
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    // ---------------------------------------------------------------- AI

    // English tokens from the model -> the UI language's, as the chips show.
    static std::wstring UiTokens(std::wstring s) {
        static const wchar_t* const en[] = {L"{name}", L"{n}", L"{taken}", L"{modified}", L"{folder}", L"{size}"};
        for (int i = 0; i < 6; ++i) {
            const std::wstring from = en[i], to = TokenText(i);
            for (size_t at = s.find(from); at != std::wstring::npos; at = s.find(from, at + to.size()))
                s.replace(at, from.size(), to);
        }
        return s;
    }

    void ShowAiError(const std::wstring& why) {
        ai_note_ = AiNote::Error;
        ai_error_ = FmtS(StringId::BrAiFailed, why.c_str());
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Send, or stop while a request runs.
    void SendAi() {
        if (ai_task_) {
            ai_task_->Cancel();
            ai_task_.reset();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        std::wstring q = ai_text_;
        while (!q.empty() && iswspace(q.back())) q.pop_back();
        while (!q.empty() && iswspace(q.front())) q.erase(0, 1);
        if (q.empty()) return;
        LeaveContentMode();   // a sentence describes steps
        if (options_.ai_model.empty() || !options_.ai_start) { ShowAiError(S(StringId::BrAiNoConn)); return; }
        rn::AiAsk ask;
        ask.sentence = q;
        ask.model = options_.ai_model;
        ask.files = sources_.size();
        std::map<std::wstring, size_t> counts;
        for (const auto& src : sources_) ++counts[rn::Lower(ExtOf(src.name))];
        ask.exts.assign(counts.begin(), counts.end());
        std::stable_sort(ask.exts.begin(), ask.exts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::wstring error;
        auto task = options_.ai_start(rn::AiRequestBody(ask), error);
        if (!task) { ShowAiError(error.empty() ? S(StringId::BrAiNoConn) : error); return; }
        ai_task_ = std::move(task);
        ai_note_ = AiNote::None;
        ai_started_ = GetTickCount64();
        steps_scroll_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Per frame: poll the request; the finished reply is applied from a
    // posted message (it rebuilds the step fields, not a thing to do mid-paint).
    void TickAi() {
        if (!ai_task_ || ai_done_) return;
        anim_active_ = true;
        if (!ai_task_->Poll(ai_reply_, ai_failure_)) return;
        ai_done_ = true;
        PostMessageW(hwnd_, kAiMsg, 0, 0);
    }

    void OnAiDone() {
        if (!ai_task_ || !ai_done_) return;
        ai_task_.reset();
        ai_done_ = false;
        const std::string reply = std::move(ai_reply_);
        const std::wstring failure = std::move(ai_failure_);
        ai_reply_.clear();
        ai_failure_.clear();
        if (!failure.empty()) { ShowAiError(failure); return; }
        rn::AiReply r;
        if (!rn::ParseAiReply(reply, r)) { ShowAiError(S(StringId::BrAiErrReply)); return; }
        if (r.steps.empty()) { ShowAiError(r.note.empty() ? S(StringId::BrAiErrReply) : r.note); return; }
        for (auto& st : r.steps) {
            st.pattern = UiTokens(st.pattern);
            st.text = UiTokens(st.text);
        }
        rn::Preset p;
        p.steps = std::move(r.steps);
        p.opts = opts_;
        if (r.has_order) p.opts.order = r.order;
        p.opts.daily = r.daily;
        ApplyPreset(p, true, 260);   // the cards come in one by one, as in the design
        preset_name_.clear();
        preset_key_.clear();
        ai_note_ = AiNote::Info;
        Recompute(true);
    }


    // ---------------------------------------------------------------- 按内容命名

    bool ContentRunning() const { return c_job_ != nullptr || c_task_ != nullptr; }
    size_t RangeBudget() const {
        static const size_t k[] = {1200, 2500, 12000};   // 标题和首页 / 前 2 页 / 前 10 页, in characters
        return k[std::clamp(c_range_, 0, 2)];
    }
    bool Readable(size_t i) const {
        const std::wstring ext = rn::Lower(ExtOf(sources_[i].name));
        return options_.can_read && options_.read_texts && !ext.empty() && !IsImageExt(ext) && !IsVideoExt(ext) &&
               options_.can_read(ext);
    }
    int ReadableCount() const {
        int n = 0;
        for (size_t i = 0; i < sources_.size(); ++i) n += Readable(i) ? 1 : 0;
        return n;
    }

    void EnterContentMode() {
        if (content_mode_) return;
        CommitRowEdit(true);
        if (naming_) EndNaming();
        content_mode_ = true;
        if (crows_.size() != sources_.size()) ResetContentRows();
        menu_ = Menu::None;
        filter_ = 0;
        rows_scroll_ = 0;
        Recompute(false);
    }
    void LeaveContentMode() {
        if (!content_mode_) return;
        CommitRowEdit(false);
        StopContent();
        content_mode_ = false;
        filter_ = 0;
        rows_scroll_ = 0;
        Recompute(false);
    }

    void ResetContentRows() {
        crows_.assign(sources_.size(), {});
        for (size_t i = 0; i < sources_.size(); ++i) {
            if (Readable(i)) continue;
            crows_[i].st = CSt::Skip;
            crows_[i].basis = S(IsImageExt(rn::Lower(ExtOf(sources_[i].name))) ? StringId::BrCmSkipImage
                                                                                 : StringId::BrCmSkipType);
        }
    }

    // 开始生成建议 / 重新生成: nothing is read or sent before this.
    void StartContent() {
        CommitRowEdit(false);
        StopContent();
        c_error_.clear();
        if (options_.ai_model.empty() || !options_.ai_start) {
            c_error_ = S(StringId::BrAiNoConn);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        ResetContentRows();
        auto job = std::make_shared<ContentJob>();
        for (size_t i = 0; i < crows_.size(); ++i)
            if (crows_[i].st == CSt::Wait) { job->paths.push_back(sources_[i].path); job->source.push_back(i); }
        c_ran_ = true;
        filter_ = 0;
        rows_scroll_ = 0;
        if (job->paths.empty()) { Recompute(false); return; }
        crows_[job->source.front()].st = CSt::Read;
        c_budget_ = RangeBudget();
        c_job_ = job;
        const LPARAM gen = ++c_gen_;
        auto read = options_.read_texts;   // a copy: the worker may outlive the dialog
        std::thread([job, read, budget = c_budget_, hwnd = hwnd_, gen] {
            read(job->paths, budget, [job] { return job->cancel.load(); }, [job, hwnd, gen](size_t k, std::wstring text) {
                {
                    std::lock_guard<std::mutex> lock(job->m);
                    job->ready.emplace_back(k, std::move(text));
                }
                job->done.fetch_add(1);
                if (!job->cancel.load()) PostMessageW(hwnd, kContentMsg, 0, gen);
            });
            if (!job->cancel.load()) PostMessageW(hwnd, kContentMsg, 1, gen);
        }).detach();
        Recompute(false);
    }

    // 停止: rows without an answer go back to 等待读取.
    void StopContent() {
        if (c_job_) { c_job_->cancel.store(true); c_job_.reset(); }
        if (c_task_) { c_task_->Cancel(); c_task_.reset(); }
        c_task_done_ = false;
        c_inflight_.clear();
        c_reply_.clear();
        c_failure_.clear();
        for (auto& r : crows_)
            if (r.st == CSt::Read || r.st == CSt::Think) { r.st = CSt::Wait; r.text.clear(); r.sent = false; }
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void OnContentMsg(WPARAM what, LPARAM gen) {
        if (what == 2) { OnContentReply(); return; }
        if (!c_job_ || gen != c_gen_) return;
        std::vector<std::pair<size_t, std::wstring>> got;
        {
            std::lock_guard<std::mutex> lock(c_job_->m);
            got.swap(c_job_->ready);
        }
        for (auto& [k, text] : got) {
            if (k >= c_job_->source.size()) continue;
            CRow& r = crows_[c_job_->source[k]];
            r.text = rn::CompactText(text, c_budget_);
            if (r.text.empty()) { r.st = CSt::Skip; r.basis = S(StringId::BrCmNoText); }
            else r.st = CSt::Think;
        }
        const size_t next = c_job_->done.load();
        if (what == 1) c_job_.reset();
        else if (next < c_job_->source.size() && crows_[c_job_->source[next]].st == CSt::Wait)
            crows_[c_job_->source[next]].st = CSt::Read;
        PumpContent();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // One request at a time: up to 5 files / ~9000 characters. While files
    // are still being read, a group waits until it is full.
    void PumpContent() {
        if (c_task_) return;
        std::vector<size_t> group;
        size_t chars = 0;
        bool full = false;
        for (size_t i = 0; i < crows_.size(); ++i) {
            const CRow& r = crows_[i];
            if (r.st != CSt::Think || r.sent) continue;
            if (!group.empty() && (group.size() >= 5 || chars + r.text.size() > 9000)) { full = true; break; }
            group.push_back(i);
            chars += r.text.size();
        }
        if (group.empty() || (c_job_ && !full && group.size() < 5)) return;
        rn::ContentAsk ask;
        ask.model = options_.ai_model;
        ask.format = static_cast<rn::NameFormat>(c_format_);
        if (ask.format == rn::NameFormat::LikeFolder && folder_samples_.empty()) ask.format = rn::NameFormat::DateTypeSubject;
        ask.language = static_cast<rn::NameLanguage>(c_lang_);
        switch (pulse::l10n::effective_language()) {
        case pulse::l10n::Language::EnUS: ask.reason_language = "English"; break;
        case pulse::l10n::Language::ZhTW: ask.reason_language = "Traditional Chinese (Taiwan)"; break;
        default: ask.reason_language = "Simplified Chinese"; break;
        }
        ask.folder_names = folder_samples_;
        for (size_t i : group) {
            ask.files.push_back({static_cast<int>(i) + 1, sources_[i].name, crows_[i].text});
            crows_[i].sent = true;
        }
        std::wstring error;
        auto task = options_.ai_start(rn::ContentRequestBody(ask), error);
        if (!task) { FailContent(error.empty() ? S(StringId::BrAiNoConn) : error, group); return; }
        c_task_ = std::move(task);
        c_inflight_ = std::move(group);
    }

    // A request that could not run: the connection is the problem, so the
    // whole pass stops and says why.
    void FailContent(const std::wstring& why, const std::vector<size_t>& group) {
        for (size_t i : group) { crows_[i].st = CSt::Fail; crows_[i].basis = why; crows_[i].text.clear(); }
        for (auto& r : crows_)
            if (r.st == CSt::Think && !r.sent) { r.st = CSt::Fail; r.basis = why; r.text.clear(); }
        c_error_ = FmtS(StringId::BrAiFailed, why.c_str());
        StopContent();
        Recompute(true);
    }

    void TickContent() {
        if (!c_task_ || c_task_done_) return;
        anim_active_ = true;
        if (!c_task_->Poll(c_reply_, c_failure_)) return;
        c_task_done_ = true;
        PostMessageW(hwnd_, kContentMsg, 2, 0);
    }

    void OnContentReply() {
        if (!c_task_ || !c_task_done_) return;
        c_task_.reset();
        c_task_done_ = false;
        const std::string reply = std::move(c_reply_);
        const std::wstring failure = std::move(c_failure_);
        c_reply_.clear();
        c_failure_.clear();
        std::vector<size_t> group = std::move(c_inflight_);
        c_inflight_.clear();
        if (!failure.empty()) { FailContent(failure, group); return; }
        std::vector<rn::ContentSuggestion> out;
        const bool parsed = rn::ParseContentReply(reply, out);
        std::vector<bool> answered(crows_.size(), false);
        for (const auto& sug : out) {
            const size_t i = static_cast<size_t>(sug.id - 1);
            if (sug.id < 1 || i >= crows_.size() || std::find(group.begin(), group.end(), i) == group.end()) continue;
            CRow& r = crows_[i];
            answered[i] = true;
            r.text.clear();
            r.basis = sug.basis;
            // A "new" name equal to the current one is a keep too.
            if (sug.keep || rn::Lower(sug.name + ExtOf(sources_[i].name)) == rn::Lower(sources_[i].name)) {
                r.st = CSt::Keep;
                if (r.basis.empty()) r.basis = S(StringId::BrCmNameKeep);
            } else {
                r.st = CSt::Ok;
                r.name = sug.name;
                r.pick = true;
            }
        }
        for (size_t i : group)
            if (!answered[i]) {
                crows_[i].st = CSt::Fail;
                crows_[i].basis = S(parsed ? StringId::BrCmMissing : StringId::BrAiErrReply);
                crows_[i].text.clear();
            }
        PumpContent();
        Recompute(true);
    }

    // Inline edit of a suggestion (click its name).
    void BeginRowEdit(size_t i) {
        if (i >= crows_.size() || crows_[i].st != CSt::Ok || executing_) return;
        CommitRowEdit(true);
        c_edit_ = static_cast<int>(i);
        crows_[i].pick = true;
        CreateField(kRowField, c_edit_, crows_[i].name, false);
        pending_row_focus_ = true;   // focused once the first layout placed it
        Recompute(false);
    }
    void CommitRowEdit(bool keep) {
        if (c_edit_ < 0) return;
        const size_t i = static_cast<size_t>(c_edit_);
        if (Field* f = FindField(kRowField, c_edit_)) {
            if (keep && i < crows_.size()) {
                const std::wstring name = rn::CleanName(EditText(f->hwnd), 120);
                if (!name.empty()) crows_[i].name = name;
            }
            const bool had_focus = GetFocus() == f->hwnd;
            DestroyWindow(f->hwnd);
            fields_.erase(fields_.begin() + (f - fields_.data()));
            if (had_focus && hwnd_) SetFocus(hwnd_);
        }
        c_edit_ = -1;
        Recompute(true);
    }

    // ---------------------------------------------------------------- presets

    // Built-in presets: the same jobs as the AI examples, without AI. Tokens
    // follow the UI language.
    static std::vector<rn::Preset> Builtins() {
        std::vector<rn::Preset> out;
        auto step = [](rn::StepType t) { rn::Step st; st.type = t; return st; };
        {
            rn::Preset p;
            p.name = S(StringId::BrPresetClean);
            rn::Step a = step(rn::StepType::Replace); a.find = L"_"; a.with = L" ";
            rn::Step b = step(rn::StepType::Remove); b.brackets = true; b.spaces = true;
            rn::Step c = step(rn::StepType::Case); c.mode = rn::CaseMode::Title;
            p.steps = {a, b, c};
            out.push_back(std::move(p));
        }
        {
            rn::Preset p;
            p.name = S(StringId::BrPresetPhoto);
            rn::Step t = step(rn::StepType::Template); t.pattern = TokenText(2) + L" " + TokenText(1);
            p.steps = {t};
            p.opts.order = rn::Order::Taken;
            out.push_back(std::move(p));
        }
        {
            rn::Preset p;
            p.name = S(StringId::BrPresetKebab);
            rn::Step a = step(rn::StepType::Remove); a.spaces = true;
            rn::Step b = step(rn::StepType::Replace); b.find = L" "; b.with = L"-";
            rn::Step c = step(rn::StepType::Case); c.mode = rn::CaseMode::Lower;
            p.steps = {a, b, c};
            out.push_back(std::move(p));
        }
        {
            rn::Preset p;
            p.name = S(StringId::BrPresetFolder);
            rn::Step t = step(rn::StepType::Template); t.pattern = TokenText(4) + L" " + TokenText(1);
            p.steps = {t};
            out.push_back(std::move(p));
        }
        {
            rn::Preset p;
            p.name = S(StringId::BrPresetDate);
            rn::Step t = step(rn::StepType::Insert); t.text = TokenText(3) + L" "; t.where = rn::Where::Start;
            p.steps = {t};
            out.push_back(std::move(p));
        }
        return out;
    }

    void ApplyPreset(const rn::Preset& p, bool animate, ULONGLONG stagger = 40) {
        ai_note_ = AiNote::None;
        for (auto it = fields_.begin(); it != fields_.end();) {
            if (it->step_id >= 0) { DestroyWindow(it->hwnd); it = fields_.erase(it); }
            else ++it;
        }
        steps_.clear();
        steps_ui_.clear();
        const ULONGLONG now = GetTickCount64();
        for (const rn::Step& st : p.steps) {
            steps_.push_back(st);
            StepUi ui;
            ui.id = next_step_id_++;
            if (st.type == rn::StepType::Remove && !st.custom.empty()) { ui.custom_on = true; ui.custom_text = st.custom; }
            if (animate) enter_[ui.id] = now + stagger * steps_ui_.size();
            steps_ui_.push_back(ui);
        }
        opts_ = p.opts;
        preset_name_ = p.name;
        preset_key_ = rn::StepsKey(steps_, opts_);
        focus_step_ = -1;
        steps_scroll_ = 0;
        if (hwnd_) { EnsureFields(); SetFocus(hwnd_); }
    }

    bool PresetEdited() const { return !preset_name_.empty() && rn::StepsKey(steps_, opts_) != preset_key_; }

    bool IsMine(const std::wstring& name) const {
        for (const auto& p : store_.presets) if (p.name == name) return true;
        return false;
    }

    void StartNaming() {
        if (naming_) return;
        naming_ = true;
        naming_focus_ = true;
        CreateField(kPresetField, 0, IsMine(preset_name_) ? preset_name_ : std::wstring(), false);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void EndNaming() {
        naming_ = false;
        for (auto it = fields_.begin(); it != fields_.end();) {
            if (it->step_id == kPresetField) { DestroyWindow(it->hwnd); it = fields_.erase(it); }
            else ++it;
        }
        SetFocus(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SaveStore() {
#ifndef PULSE_TEST_BATCH_RENAME_PRESENT
        rn::SavePresets(store_);
#endif
    }

    void CommitPresetName() {
        Field* f = FindField(kPresetField, 0);
        if (!f) return;
        std::wstring name = EditText(f->hwnd);
        while (!name.empty() && iswspace(name.back())) name.pop_back();
        while (!name.empty() && iswspace(name.front())) name.erase(0, 1);
        if (name.empty() || steps_.empty()) { MessageBeep(MB_OK); return; }
        rn::Preset p{name, steps_, opts_};
        auto it = std::find_if(store_.presets.begin(), store_.presets.end(),
                               [&](const rn::Preset& x) { return x.name == name; });
        if (it != store_.presets.end()) *it = std::move(p);
        else store_.presets.push_back(std::move(p));
        SaveStore();
        preset_name_ = name;
        preset_key_ = rn::StepsKey(steps_, opts_);
        EndNaming();
    }

    // Per frame while running: how many rows show their check. The checks
    // follow the task but are spread over at most ~0.6 s, so a fast batch
    // still reads as going through in order; a slow one is never held back.
    void TickExecution() {
        if (!executing_) return;
        anim_active_ = true;
        const size_t total = plan_.exec_order.size();
        const BatchRenameProgress p = options_.poll(exec_task_);
        const size_t real = p.finished ? total : (std::min)(p.done, total);
        const ULONGLONG per_row = std::clamp<ULONGLONG>(600 / (std::max<size_t>)(total, 1), 25, 110);
        const size_t by_time = static_cast<size_t>((now_ - exec_start_) / per_row) + 1;
        exec_shown_ = (std::min)(real, by_time);
        if (p.finished && exec_shown_ >= total && !exec_end_) exec_end_ = now_;
        // Done: hold the last check for a moment (HTML: 380 ms), then close.
        // Queued behind another job: leave it to the operations UI.
        const bool stuck = !p.finished && real == 0 && now_ - exec_start_ > 1500;
        if ((exec_end_ && now_ - exec_end_ >= 380) || stuck) {
            executing_ = false;
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
    }

    // ---------------------------------------------------------------- steps

    int StepIndex(int id) const {
        for (size_t i = 0; i < steps_ui_.size(); ++i) if (steps_ui_[i].id == id) return static_cast<int>(i);
        return -1;
    }

    void AddStep(rn::StepType type, bool animate) {
        rn::Step st;
        st.type = type;
        if (type == rn::StepType::Case) st.mode = rn::CaseMode::Title;
        if (type == rn::StepType::Ext) st.mode = rn::CaseMode::Lower;
        if (type == rn::StepType::Template) st.pattern = TokenText(0);
        steps_.push_back(st);
        StepUi ui;
        ui.id = next_step_id_++;
        steps_ui_.push_back(ui);
        if (animate) enter_[ui.id] = GetTickCount64();
        if (hwnd_) EnsureFields();
    }

    void DeleteStep(int index) {
        if (index < 0 || index >= static_cast<int>(steps_.size())) return;
        const int id = steps_ui_[static_cast<size_t>(index)].id;
        steps_.erase(steps_.begin() + index);
        steps_ui_.erase(steps_ui_.begin() + index);
        for (auto it = fields_.begin(); it != fields_.end();) {
            if (it->step_id == id) { DestroyWindow(it->hwnd); it = fields_.erase(it); }
            else ++it;
        }
        if (focus_step_ == id) focus_step_ = -1;
        Recompute(true);
    }

    // Template token text: Chinese keys in a Chinese UI, English keys otherwise.
    static std::wstring TokenText(int t) {
        static const StringId ids[] = {StringId::BrTokName, StringId::BrTokNumber, StringId::BrTokTaken,
                                       StringId::BrTokModified, StringId::BrTokFolder, StringId::BrTokSize};
        static const wchar_t* en[] = {L"name", L"number", L"taken", L"modified", L"folder", L"size"};
        const std::wstring label = S(ids[t]);
        const bool cjk = !label.empty() && label[0] >= 0x2E80;
        return L"{" + (cjk ? label : std::wstring(en[t])) + L"}";
    }

    // ---------------------------------------------------------------- edits

    Field* FindField(int step_id, int field) {
        for (auto& f : fields_) if (f.step_id == step_id && f.field == field) return &f;
        return nullptr;
    }
    Field* FieldOf(HWND hwnd) {
        for (auto& f : fields_) if (f.hwnd == hwnd) return &f;
        return nullptr;
    }

    std::wstring FieldValue(int step_index, int field) const {
        const rn::Step& st = steps_[static_cast<size_t>(step_index)];
        const StepUi& ui = steps_ui_[static_cast<size_t>(step_index)];
        switch (st.type) {
        case rn::StepType::Replace: return field == kFind ? st.find : st.with;
        case rn::StepType::Remove: return ui.custom_text;
        case rn::StepType::Template:
            if (field == kPattern) return st.pattern;
            return std::to_wstring(field == kStart ? st.start : field == kDigits ? st.digits_n : st.stride);
        case rn::StepType::Ext: return st.ext_to;
        case rn::StepType::Insert: return field == kText ? st.text : std::to_wstring(st.at);
        case rn::StepType::Number:
            if (field == kSep) return st.sep;
            return std::to_wstring(field == kStart ? st.start : field == kDigits ? st.digits_n : st.stride);
        default: return {};
        }
    }

    void EnsureFields() {
        if (options_.proposed_names) return;
        if (!FindField(-1, 0)) CreateField(-1, 0, std::wstring(), false);
        for (size_t i = 0; i < steps_.size(); ++i) {
            const int id = steps_ui_[i].id;
            int count = 0;
            switch (steps_[i].type) {
            case rn::StepType::Replace: count = 2; break;
            case rn::StepType::Remove: count = 1; break;
            case rn::StepType::Template: count = 4; break;
            case rn::StepType::Ext: count = 1; break;
            case rn::StepType::Insert: count = 2; break;
            case rn::StepType::Number: count = 4; break;
            default: break;
            }
            const rn::StepType t = steps_[i].type;
            for (int f = 0; f < count; ++f)
                if (!FindField(id, f))
                    CreateField(id, f, FieldValue(static_cast<int>(i), f),
                                (t == rn::StepType::Template || t == rn::StepType::Number) ? f != 0
                                : t == rn::StepType::Insert ? f == kAt : false);
        }
    }

    void CreateField(int step_id, int field, const std::wstring& text, bool numeric) {
        HWND edit = CreateChildEdit(hwnd_, text.c_str(), numeric ? (ES_NUMBER | ES_CENTER) : 0);
        if (!edit) return;
        if (step_id == kAiField) SetWindowLongPtrW(edit, GWLP_ID, kAiEditId);
        HFONT font = step_id == kAiField ? font_ai_ : font_;
        if (font) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (numeric) SendMessageW(edit, EM_SETLIMITTEXT, 4, 0);
        SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
        Field f;
        f.hwnd = edit;
        f.step_id = step_id;
        f.field = field;
        f.numeric = numeric;
        fields_.push_back(f);
    }

    void DestroyFields() {
        for (auto& f : fields_) if (f.hwnd && IsWindow(f.hwnd)) DestroyWindow(f.hwnd);
        fields_.clear();
    }

    // Over acrylic the panes, cards and chips let the frost through (效果图 ④);
    // fields, the AI bar, dropdowns and menus stay solid.
    void GlassPal() {
        if (!glass_) return;
        pal_.glass = pulse::ui::GlassCard(dark_);
        pal_.glass_pane = pulse::ui::GlassSheet(dark_);
        pal_.glass_sheet = pal_.glass_pane;
    }
    D2D1_COLOR_F FieldBack(const Field& f) const { return f.step_id == kAiField ? pal_.card : pal_.field; }
    IDWriteTextFormat* FieldFormat(const Field& f) const { return f.step_id == kAiField ? F(Fmt::T14) : F(Fmt::T13); }

    // Positions every EDIT where the last frame put its box; hidden boxes and
    // boxes outside the steps viewport hide the control.
    void PlaceFields() {
        if (done_ || !hwnd_) return;
        for (auto& f : fields_) {
            if (!f.want_visible) {
                if (IsWindowVisible(f.hwnd)) {
                    if (GetFocus() == f.hwnd) SetFocus(hwnd_);
                    ShowWindow(f.hwnd, SW_HIDE);
                }
                continue;
            }
            const float pad_l = (f.step_id == kAiField ? 0.0f : (f.numeric ? 3.0f : 9.0f)) * scale_;
            const float pad_r = (f.step_id == kAiField ? 0.0f : (f.numeric ? 3.0f : (HintFor(f).empty() ? 9.0f : 62.0f))) * scale_;
            const int x = static_cast<int>(std::lround(f.want.left + pad_l));
            const int w = (std::max)(16, static_cast<int>(std::lround(f.want.right - f.want.left - pad_l - pad_r)));
            const int box_h = (std::max)(12, static_cast<int>(std::lround(f.want.bottom - f.want.top)));
            const int line_h = EditLineHeight(f.hwnd, f.step_id == kAiField ? font_ai_ : font_, box_h - 2);
            const int y = static_cast<int>(std::lround(f.want.top)) + (std::max)(0, (box_h - line_h) / 2);
            RECT prev{};
            GetWindowRect(f.hwnd, &prev);
            MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&prev), 2);
            const bool moved = !IsWindowVisible(f.hwnd) || prev.left != x || prev.top != y ||
                               prev.right - prev.left != w || prev.bottom - prev.top != line_h;
            if (!moved) continue;
            SetWindowPos(f.hwnd, HWND_TOP, x, y, w, line_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            PresentChildEdit(compositor_, FieldFormat(f), pal_.text, FieldBack(f), f.hwnd);
        }
    }

    std::wstring HintFor(const Field& f) const {
        if (f.step_id < 0) return {};
        const int i = StepIndex(f.step_id);
        if (i < 0) return {};
        const rn::StepType t = steps_[static_cast<size_t>(i)].type;
        if (t == rn::StepType::Replace || (t == rn::StepType::Insert && f.field == kText) ||
            (t == rn::StepType::Number && f.field == kSep))
            return WsHint(FieldValue(i, f.field));
        return {};
    }

    static std::wstring WsHint(const std::wstring& v) {
        if (v == L" ") return S(StringId::BrWsSpace);
        if (v == L"_") return S(StringId::BrWsUnderscore);
        if (v == L"-") return S(StringId::BrWsHyphen);
        if (v == L".") return S(StringId::BrWsDot);
        if (!v.empty() && v.find_first_not_of(L" \t\x3000") == std::wstring::npos)
            return FmtS(StringId::BrWsSpaces, static_cast<int>(v.size()));
        return {};
    }

    std::wstring EditText(HWND hwnd) const {
        const int n = GetWindowTextLengthW(hwnd);
        std::wstring text(static_cast<size_t>(n), L'\0');
        if (n > 0) GetWindowTextW(hwnd, text.data(), n + 1);
        return text;
    }

    void OnFieldChanged(HWND hwnd) {
        Field* f = FieldOf(hwnd);
        if (!f) return;
        const std::wstring text = EditText(hwnd);
        if (f->step_id == kAiField) { ai_text_ = text; InvalidateRect(hwnd_, nullptr, FALSE); return; }
        if (f->step_id == kPresetField || f->step_id == kRowField) { InvalidateRect(hwnd_, nullptr, FALSE); return; }
        const int i = StepIndex(f->step_id);
        if (i < 0) return;
        rn::Step& st = steps_[static_cast<size_t>(i)];
        StepUi& ui = steps_ui_[static_cast<size_t>(i)];
        auto num = [&](int lo, int hi, int fallback) {
            if (text.empty()) return fallback;
            return std::clamp(static_cast<int>(wcstol(text.c_str(), nullptr, 10)), lo, hi);
        };
        switch (st.type) {
        case rn::StepType::Replace: (f->field == kFind ? st.find : st.with) = text; break;
        case rn::StepType::Remove: ui.custom_text = text; st.custom = ui.custom_on ? text : std::wstring(); break;
        case rn::StepType::Template:
            if (f->field == kPattern) st.pattern = text;
            else if (f->field == kStart) st.start = num(0, 999999, 1);
            else if (f->field == kDigits) st.digits_n = num(1, 9, 3);
            else st.stride = num(1, 9999, 1);
            break;
        case rn::StepType::Ext: st.ext_to = text; break;
        case rn::StepType::Insert:
            if (f->field == kText) st.text = text;
            else st.at = num(0, 9999, 1);
            break;
        case rn::StepType::Number:
            if (f->field == kSep) st.sep = text;
            else if (f->field == kStart) st.start = num(0, 999999, 1);
            else if (f->field == kDigits) st.digits_n = num(1, 9, 3);
            else st.stride = num(1, 9999, 1);
            break;
        default: break;
        }
        Recompute(true);
    }

    // Tab order: the AI bar, then each visible step field top to bottom.
    void FocusNext(HWND from, int dir) {
        if (Field* row = FieldOf(from); row && row->step_id == kRowField) { CommitRowEdit(true); return; }
        std::vector<HWND> order;
        if (Field* ai = FindField(kAiField, 0)) order.push_back(ai->hwnd);
        for (const auto& ui : steps_ui_)
            for (int f = 0; f < 4; ++f)
                if (Field* x = FindField(ui.id, f); x && x->want_visible) order.push_back(x->hwnd);
        if (Field* name = FindField(kPresetField, 0); name && name->want_visible) order.push_back(name->hwnd);
        if (order.empty()) return;
        auto it = std::find(order.begin(), order.end(), from);
        int idx = it == order.end() ? 0 : static_cast<int>(it - order.begin()) + dir;
        const int n = static_cast<int>(order.size());
        idx = ((idx % n) + n) % n;
        SetFocus(order[static_cast<size_t>(idx)]);
    }

    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<BatchRenameWindow*>(ref);
        if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);
        switch (msg) {
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { self->OnEscape(); return 0; }
            if (wparam == VK_RETURN) { self->OnEnter(hwnd); return 0; }
            if (wparam == VK_TAB) { self->FocusNext(hwnd, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); return 0; }
            break;
        case WM_CHAR:
            if (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB) return 0;
            break;
        case WM_SETFOCUS: {
            if (Field* f = self->FieldOf(hwnd); f && f->step_id >= 0) self->focus_step_ = f->step_id;
            break;
        }
        default: break;
        }
        const bool ring = msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_LBUTTONDOWN;
        Field* f = self->FieldOf(hwnd);
        const D2D1_COLOR_F back = f ? self->FieldBack(*f) : self->pal_.field;
        IDWriteTextFormat* fmt = f ? self->FieldFormat(*f) : self->F(Fmt::T13);
        HBRUSH brush = f && f->step_id == kAiField ? self->brush_ai_ : self->brush_field_;
        LRESULT result = 0;
        if (!HandleChildEditMessage(self->compositor_, fmt, self->pal_.text, back, brush, hwnd, msg, wparam, lparam,
                                    result))
            result = DefPresentedChildEditProc(self->compositor_, fmt, self->pal_.text, back, hwnd, msg, wparam, lparam);
        if (ring) InvalidateRect(self->hwnd_, nullptr, FALSE);
        return result;
    }

    void OnEscape() {
        if (menu_ != Menu::None) { menu_ = Menu::None; InvalidateRect(hwnd_, nullptr, FALSE); return; }
        if (naming_) { EndNaming(); return; }
        if (c_edit_ >= 0) { CommitRowEdit(false); return; }
        if (ai_task_) { SendAi(); return; }   // Esc stops the request first
        if (content_mode_ && ContentRunning()) { StopContent(); Recompute(false); return; }
        Complete(false);
    }
    void OnEnter(HWND from) {
        // Enter with a menu open only closes it; it never starts the rename.
        if (menu_ != Menu::None) { menu_ = Menu::None; InvalidateRect(hwnd_, nullptr, FALSE); return; }
        Field* f = from ? FieldOf(from) : nullptr;
        if (f && f->step_id == kPresetField) { CommitPresetName(); return; }
        if (f && f->step_id == kAiField) { SendAi(); return; }
        if (f && f->step_id == kRowField) { CommitRowEdit(true); return; }
        Complete(true);
    }

    // ---------------------------------------------------------------- drawing kit

    IDWriteTextFormat* F(Fmt f) const { return formats_[static_cast<size_t>(f)]; }

    void CreateFormats() {
        ReleaseFormats();
        IDWriteFactory2* factory = compositor_.DwriteFactory();
        if (!factory) return;
        struct Spec { float size; DWRITE_FONT_WEIGHT weight; };
        const Spec specs[] = {
            {15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD}, {14.0f, DWRITE_FONT_WEIGHT_NORMAL},
            {13.5f, DWRITE_FONT_WEIGHT_NORMAL}, {13.5f, DWRITE_FONT_WEIGHT_MEDIUM}, {13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
            {13.0f, DWRITE_FONT_WEIGHT_NORMAL}, {13.0f, DWRITE_FONT_WEIGHT_MEDIUM}, {13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
            {12.5f, DWRITE_FONT_WEIGHT_NORMAL}, {12.5f, DWRITE_FONT_WEIGHT_MEDIUM}, {12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
            {12.0f, DWRITE_FONT_WEIGHT_NORMAL}, {12.0f, DWRITE_FONT_WEIGHT_MEDIUM}, {12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
            {11.5f, DWRITE_FONT_WEIGHT_NORMAL}, {11.5f, DWRITE_FONT_WEIGHT_MEDIUM}, {11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
            {11.0f, DWRITE_FONT_WEIGHT_NORMAL}, {11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
        };
        static_assert(std::size(specs) == static_cast<size_t>(Fmt::Count), "one spec per format");
        for (size_t i = 0; i < std::size(specs); ++i) {
            typography::TextFormatSpec spec;
            spec.size = specs[i].size * scale_;
            spec.weight = specs[i].weight;
            typography::CreateTextFormat(factory, spec, &formats_[i]);
        }
    }
    void ReleaseFormats() {
        for (auto*& f : formats_) if (f) { f->Release(); f = nullptr; }
    }

    float X(float dip) const { return dip * scale_; }
    D2D1_COLOR_F Fade(D2D1_COLOR_F c) const { return Alpha(c, fade_); }
    ID2D1SolidColorBrush* B(D2D1_COLOR_F c) const { brush_->SetColor(Fade(c)); return brush_; }

    void Fill(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) const {
        if (c.a <= 0.0f) return;
        if (radius <= 0.0f) dc_->FillRectangle(r, B(c));
        else dc_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), B(c));
    }
    void Stroke(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float width = 1.0f) const {
        if (c.a <= 0.0f) return;
        const float h = width * 0.5f;
        const D2D1_RECT_F in = D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h);
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(in, (std::max)(0.0f, radius - h), (std::max)(0.0f, radius - h)),
                                  B(c), width);
    }
    void Shadow(const D2D1_RECT_F& r, float radius, float blur, float dy, D2D1_COLOR_F c) const {
        const int steps = 7;
        for (int i = steps; i >= 1; --i) {
            const float t = static_cast<float>(i) / steps;
            const float grow = blur * t * 0.6f;
            D2D1_RECT_F g = D2D1::RectF(r.left - grow, r.top - grow + dy * t, r.right + grow, r.bottom + grow + dy * t);
            Fill(g, radius + grow, Alpha(c, (1.0f - t) * 2.0f / steps));
        }
    }
    void Ico(const Icon& icon, float x, float y, float size, D2D1_COLOR_F c) const {
        DrawSvgParts(dc_, icon.parts, icon.count, icon.view, D2D1::RectF(x, y, x + size, y + size), Fade(c));
    }

    IDWriteTextLayout* Layout(const std::wstring& text, IDWriteTextFormat* fmt, float w, float h, bool wrap,
                              bool ellipsis, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                              float line_h = 0.0f) const {
        if (!fmt || text.empty()) return nullptr;
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(compositor_.DwriteFactory()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                                                                 (std::max)(1.0f, w), (std::max)(1.0f, h), &layout)))
            return nullptr;
        layout->SetTextAlignment(align);
        layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (line_h > 0.0f) layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line_h, line_h * 0.78f);
        if (!wrap && ellipsis) {
            IDWriteInlineObject* sign = nullptr;
            if (SUCCEEDED(compositor_.DwriteFactory()->CreateEllipsisTrimmingSign(fmt, &sign))) {
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                layout->SetTrimming(&trim, sign);
                sign->Release();
            }
        }
        return layout;
    }
    // Single line, vertically centred in r; returns the drawn width.
    float Text(const std::wstring& text, Fmt f, const D2D1_RECT_F& r, D2D1_COLOR_F c,
               DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) const {
        IDWriteTextLayout* layout = Layout(text, F(f), r.right - r.left, r.bottom - r.top, false, true, align);
        if (!layout) return 0.0f;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        const float y = std::round(r.top + ((r.bottom - r.top) - m.height) * 0.5f);
        compositor_.DrawTextLayout(D2D1::Point2F(r.left, y), layout, B(c));
        layout->Release();
        return m.widthIncludingTrailingWhitespace;
    }
    float Measure(const std::wstring& text, Fmt f) const {
        IDWriteTextLayout* layout = Layout(text, F(f), 10000.0f, 1000.0f, false, false);
        if (!layout) return 0.0f;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        layout->Release();
        return std::ceil(m.widthIncludingTrailingWhitespace);
    }
    // Wrapped paragraph with **bold** runs (weight only); returns its height.
    float Para(const std::wstring& marked, Fmt f, Fmt bold, const D2D1_RECT_F& r, D2D1_COLOR_F c, float line_h,
               bool draw = true) const {
        const auto runs = Rich(marked);
        std::wstring text;
        for (const auto& run : runs) text += run.text;
        IDWriteTextLayout* layout = Layout(text, F(f), r.right - r.left, 10000.0f, true, false,
                                           DWRITE_TEXT_ALIGNMENT_LEADING, line_h);
        if (!layout) return 0.0f;
        UINT32 pos = 0;
        DWRITE_FONT_WEIGHT bw = DWRITE_FONT_WEIGHT_SEMI_BOLD;
        if (IDWriteTextFormat* bf = F(bold)) bw = bf->GetFontWeight();
        for (const auto& run : runs) {
            if (run.bold) layout->SetFontWeight(bw, DWRITE_TEXT_RANGE{pos, static_cast<UINT32>(run.text.size())});
            pos += static_cast<UINT32>(run.text.size());
        }
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        if (draw) compositor_.DrawTextLayout(D2D1::Point2F(r.left, r.top), layout, B(c));
        layout->Release();
        return std::ceil(m.height);
    }
    // One line of **bold** runs: bold uses `bold` format and `bold_color`.
    float RichLine(const std::wstring& marked, Fmt f, Fmt bold, D2D1_COLOR_F c, D2D1_COLOR_F bold_color,
                   const D2D1_RECT_F& r, bool measure_only = false) const {
        float x = r.left;
        for (const auto& run : Rich(marked)) {
            const Fmt rf = run.bold ? bold : f;
            const float w = Measure(run.text, rf);
            if (!measure_only) {
                const float room = r.right - x;
                if (room <= X(4)) break;
                Text(run.text, rf, D2D1::RectF(x, r.top, r.right, r.bottom), run.bold ? bold_color : c);
            }
            x += w;
        }
        return x - r.left;
    }

    void Reg(const D2D1_RECT_F& r, Hit h) { hits_.push_back({r, h}); }
    bool Hov(Hit h) const { return hover_ == h; }

    // ---------------------------------------------------------------- render

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFormats();
        }
        if (!compositor_.Dc()) return;
        if (!F(Fmt::T13)) CreateFormats();
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        pal_ = MakePal(dark_, theme.accent);
        GlassPal();
        pulse::ui::BeginSurface(compositor_, painter_, theme, dark_, high_contrast, glass_, scale_);
        dc_ = compositor_.Dc();
        if (FAILED(dc_->CreateSolidColorBrush(pal_.text, &brush_))) { pulse::ui::EndSurface(compositor_); return; }
        hits_.clear();
        anim_active_ = false;
        now_ = GetTickCount64();
        TickExecution();
        TickAi();
        TickContent();
        for (auto& f : fields_) f.want_visible = false;

        W_ = static_cast<float>(compositor_.Width());
        H_ = static_cast<float>(compositor_.Height());
        if (!glass_) Fill(D2D1::RectF(0, 0, W_, H_), 0.0f, pal_.card);
        Stroke(D2D1::RectF(0, 0, W_, H_), X(12), Alpha(pal_.text, 0.09f));

        DrawTitle();
        if (!options_.proposed_names) {
            DrawAiBar();
            DrawExamples();
            if (content_mode_) DrawContentPane();
            else DrawSteps();
        }
        DrawPreview();
        DrawFooter();
        DrawMenus();

        brush_->Release();
        brush_ = nullptr;
        pulse::ui::EndSurface(compositor_);
        PlaceFields();
        if (pending_row_focus_) {
            Field* f = FindField(kRowField, c_edit_);
            if (!f) pending_row_focus_ = false;
            else if (IsWindowVisible(f->hwnd)) {
                pending_row_focus_ = false;
                SetFocus(f->hwnd);
                SendMessageW(f->hwnd, EM_SETSEL, 0, -1);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        if (options_.proposed_names && options_.on_review_ready && ++review_frames_ == 2)
            PostMessageW(hwnd_, kReviewReadyMsg, 0, 0);
        if (anim_active_ || options_.review_valid) SetTimer(hwnd_, kAnimTimer, anim_active_ ? 15 : 50, nullptr);
        else KillTimer(hwnd_, kAnimTimer);
    }

    float Animate(uint64_t key, float target, float dur_ms) {
        auto it = anims_.find(key);
        if (it == anims_.end()) { anims_[key] = {target, target, now_, dur_ms}; return target; }
        Anim& a = it->second;
        const float t = a.dur <= 0.0f ? 1.0f : static_cast<float>(now_ - a.start) / a.dur;
        const float cur = a.from + (a.to - a.from) * EaseOut(t);
        if (a.to != target) { a = {cur, target, now_, dur_ms}; anim_active_ = true; return cur; }
        if (t < 1.0f) { anim_active_ = true; return cur; }
        return a.to;
    }

    void DrawTitle() {
        const float y = 0;
        // Icon tile, title and folder line.
        const D2D1_RECT_F tile = R(X(18), y + X(12), X(28), X(28));
        Fill(tile, X(8), pal_.accent_soft);
        Ico(kRename, tile.left + X(6), tile.top + X(6), X(16), pal_.accent);
        float x = X(56);
        x += Text(S(StringId::BatchRename), Fmt::T15b, R(x, y, X(300), X(kTitleH)), pal_.text) + X(10);
        Text(FmtS(StringId::BrSubtitle, folder_label_.c_str(), static_cast<int>(sources_.size())), Fmt::T13,
             R(x, y, X(360), X(kTitleH)), pal_.text3);

        // Close.
        const D2D1_RECT_F close = R(W_ - X(8 + 36), y + X(8), X(36), X(36));
        if (Hov({H::Close})) Fill(close, X(8), pal_.hover);
        Ico(kX, close.left + X(11), close.top + X(11), X(14), pal_.text2);
        Reg(close, {H::Close});

        if (options_.proposed_names) return;
        // Mode switch: 规则 | 按内容命名.
        const std::wstring l0 = S(StringId::BrModeRules), l1 = S(StringId::BrModeContent);
        const float w0 = X(12 + 14 + 6 + 12) + Measure(l0, Fmt::T13);
        const float w1 = X(12 + 14 + 6 + 12) + Measure(l1, Fmt::T13);
        const float mw = w0 + w1 + X(6);
        const D2D1_RECT_F seg = R(close.left - X(6) - mw, y + X(10), mw, X(34));
        Fill(seg, X(9), pal_.hover);
        const D2D1_RECT_F b0 = R(seg.left + X(3), seg.top + X(3), w0, X(28));
        const D2D1_RECT_F b1 = R(b0.right, seg.top + X(3), w1, X(28));
        const D2D1_RECT_F on = content_mode_ ? b1 : b0;
        Shadow(on, X(7), X(2), X(1), Hex(0x000000, 0.10f));
        Fill(on, X(7), pal_.card);
        Stroke(on, X(7), Alpha(pal_.text, 0.06f));
        Ico(kList, b0.left + X(12), b0.top + X(7), X(14), content_mode_ ? pal_.text2 : pal_.text);
        Text(l0, Fmt::T13, D2D1::RectF(b0.left + X(32), b0.top, b0.right, b0.bottom), content_mode_ ? pal_.text2 : pal_.text);
        Ico(kSpark, b1.left + X(12), b1.top + X(7), X(14), content_mode_ ? pal_.ai : pal_.text2);
        Text(l1, Fmt::T13, D2D1::RectF(b1.left + X(32), b1.top, b1.right, b1.bottom), content_mode_ ? pal_.text : pal_.text2);
        Reg(b0, {H::ModeRules});
        Reg(b1, {H::ModeContent});
    }

    void DrawAiBar() {
        const D2D1_RECT_F outer = R(X(18), X(kTitleH), W_ - X(36), X(48));
        const bool busy = ai_task_ != nullptr;
        // Busy: a soft violet halo around the bar (.d-ai.busy).
        const float halo = Animate(10ull << 40, busy ? 1.0f : 0.0f, 300.0f);
        if (halo > 0.01f)
            Fill(D2D1::RectF(outer.left - X(4), outer.top - X(4), outer.right + X(4), outer.bottom + X(4)), X(16),
                 Alpha(pal_.ai, 0.14f * halo));
        // 1 px gradient ring (AI violet -> accent), card inside.
        ID2D1GradientStopCollection* stops = nullptr;
        const D2D1_GRADIENT_STOP gs[2] = {{0.0f, Alpha(pal_.ai, 0.55f)}, {1.0f, Alpha(pal_.accent, 0.55f)}};
        if (SUCCEEDED(dc_->CreateGradientStopCollection(gs, 2, &stops))) {
            ID2D1LinearGradientBrush* grad = nullptr;
            const float dy = (outer.right - outer.left) * 0.176f;  // 100deg
            if (SUCCEEDED(dc_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(outer.left, outer.top - dy * 0.5f),
                                                        D2D1::Point2F(outer.right, outer.bottom + dy * 0.5f)),
                    stops, &grad))) {
                dc_->FillRoundedRectangle(D2D1::RoundedRect(outer, X(12), X(12)), grad);
                grad->Release();
            }
            stops->Release();
        }
        const D2D1_RECT_F in = D2D1::RectF(outer.left + X(1), outer.top + X(1), outer.right - X(1), outer.bottom - X(1));
        Fill(in, X(11), pal_.card);
        Ico(kSpark, in.left + X(14), in.top + X(14), X(18), pal_.ai);

        // Send button + model name, right aligned.
        const D2D1_RECT_F go = R(in.right - X(6 + 34), in.top + X(6), X(34), X(34));
        const bool can_send = !ai_text_.empty() && !options_.ai_model.empty() && options_.ai_start;
        if (busy) {
            // Stop (.go.stop).
            Fill(go, X(9), Hov({H::AiSend}) ? Alpha(pal_.text2, 0.85f) : pal_.text2);
            Ico(kStop, go.left + X(9), go.top + X(9), X(16), pal_.card);
        } else {
            Fill(go, X(9), can_send ? (Hov({H::AiSend}) ? pal_.accent_h : pal_.accent) : Alpha(pal_.accent, 0.45f));
            Ico(kSend, go.left + X(9), go.top + X(9), X(16), pal_.on_accent);
        }
        Reg(go, {H::AiSend});
        float right = go.left - X(10);
        if (!options_.ai_model.empty()) {
            const float mw = Measure(options_.ai_model, Fmt::T125) + X(8 + 4 + 12 + 8);
            const D2D1_RECT_F model = R(right - mw, in.top + X(8), mw, X(30));
            if (Hov({H::AiModel})) Fill(model, X(7), pal_.hover);
            Text(options_.ai_model, Fmt::T125, D2D1::RectF(model.left + X(8), model.top, model.right, model.bottom), pal_.text2);
            Ico(kChev, model.right - X(8 + 12), model.top + X(9), X(12), pal_.text2);
            Reg(model, {H::AiModel});
            right = model.left - X(6);
        }
        // Input: the EDIT sits on the card; placeholder while empty.
        const D2D1_RECT_F field = D2D1::RectF(in.left + X(42), in.top + X(4), right, in.bottom - X(4));
        if (Field* f = FindField(-1, 0)) {
            f->want = field;
            f->want_visible = true;
            if (ai_text_.empty() && GetFocus() != f->hwnd) {
                f->want_visible = false;
                Text(S(StringId::BrAiPlaceholder), Fmt::T14, field, pal_.text3);
            }
        }
        Reg(field, {H::AiInput});
    }

    void DrawExamples() {
        const float y = X(kTitleH + 48 + 12 - 4);
        float x = X(18);
        x += Text(S(StringId::BrTry), Fmt::T125, R(x, y, X(60), X(26)), pal_.text3) + X(6);
        static const StringId ex[] = {StringId::BrEx1, StringId::BrEx2, StringId::BrEx3, StringId::BrEx4, StringId::BrEx5};
        for (int i = 0; i < 5; ++i) {
            const std::wstring label = S(ex[i]);
            const float w = Measure(label, Fmt::T125) + X(22);
            if (x + w > W_ - X(18)) break;
            const D2D1_RECT_F chip = R(x, y, w, X(26));
            Fill(chip, X(13), Hov({H::Example, i}) ? pal_.hover : pal_.glass);
            Stroke(chip, X(13), pal_.line);
            Text(label, Fmt::T125, chip, pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
            Reg(chip, {H::Example, i});
            x += w + X(6);
        }
    }

    // ---------------------------------------------------------------- steps pane

    float StepBodyH(size_t i) const {
        const rn::Step& st = steps_[i];
        switch (st.type) {
        case rn::StepType::Replace: return 30 + 8 + 30 + 8 + 24;
        case rn::StepType::Remove: return steps_ui_[i].custom_on ? 62.0f : 24.0f;
        case rn::StepType::Case: return 28;
        case rn::StepType::Template: {
            const float chips = TokenRows() * 24.0f + (TokenRows() - 1) * 5.0f;
            return 30 + 8 + chips + (rn::UsesNumber(st.pattern) ? 8 + 30 : 0);
        }
        case rn::StepType::Ext: return st.mode == rn::CaseMode::Change ? 66.0f : 28.0f;
        case rn::StepType::Insert: return st.where == rn::Where::Index ? 104.0f : 66.0f;
        case rn::StepType::Number: return 28.0f + 8.0f + 30.0f + 8.0f + 30.0f;
        }
        return 0;
    }
    float StepH(size_t i) const { return 40.0f + StepBodyH(i) + 12.0f; }

    int TokenRows() const {
        float x = 0;
        int rows = 1;
        for (int t = 0; t < 6; ++t) {
            const float w = Measure(TokenLabel(t), Fmt::T12) / scale_ + 18.0f;
            if (x > 0 && x + w > 302.0f) { ++rows; x = 0; }
            x += w + 5.0f;
        }
        return rows;
    }
    static std::wstring TokenLabel(int t) {
        static const StringId ids[] = {StringId::BrTokName, StringId::BrTokNumber, StringId::BrTokTaken,
                                       StringId::BrTokModified, StringId::BrTokFolder, StringId::BrTokSize};
        return S(ids[t]);
    }

    void DrawSteps() {
        const float top = X(kBodyTop), bottom = H_ - X(kFootH);
        const D2D1_RECT_F pane = D2D1::RectF(0, top, X(kStepsW), bottom);
        Fill(pane, 0, pal_.glass_pane);
        Fill(D2D1::RectF(0, top, W_, top + X(1)), 0, pal_.line2);
        Fill(D2D1::RectF(pane.right - X(1), top, pane.right, bottom), 0, pal_.line2);
        Reg(pane, {H::StepsArea});

        // Header: 步骤 N + 添加步骤.
        float x = X(16);
        const D2D1_RECT_F head = D2D1::RectF(0, top, pane.right, top + X(kPaneHeadH));
        x += Text(S(StringId::BrSteps), Fmt::T13b, D2D1::RectF(x, head.top, pane.right, head.bottom), pal_.text) + X(8);
        if (!steps_.empty())
            Text(std::to_wstring(steps_.size()), Fmt::T13, D2D1::RectF(x, head.top, pane.right, head.bottom), pal_.text3);
        const std::wstring add = S(StringId::BrAddStep);
        const float aw = Measure(add, Fmt::T13m) + X(8 + 14 + 5 + 8);
        const D2D1_RECT_F link = R(pane.right - X(14) - aw, head.top + X(8), aw, X(28));
        if (Hov({H::AddStep}) || (menu_ == Menu::AddStep && !menu_from_box_)) Fill(link, X(7), pal_.hover);
        Ico(kPlus, link.left + X(8), link.top + X(7), X(14), pal_.accent);
        Text(add, Fmt::T13m, D2D1::RectF(link.left + X(27), link.top, link.right, link.bottom), pal_.accent);
        Reg(link, {H::AddStep});
        add_link_ = link;

        // Cards.
        const float list_top = head.bottom + X(2);
        const float list_bottom = bottom - X(kPresetH);
        steps_view_ = D2D1::RectF(0, list_top, pane.right, list_bottom);
        // Quick-add panel under the cards: every step type is one click away.
        const float qa_w = pane.right - X(24);
        struct QaChip { rn::StepType type; std::wstring label; float w; float x; float y; };
        std::vector<QaChip> chips;
        float qa_h = 0;
        {
            const float pad = X(10);
            float cx = pad, cy = pad + X(18 + 6);
            for (rn::StepType t : kStepOrder) {
                std::wstring l = S(StepTitle(t));
                const float w = X(10 + 14 + 5 + 10) + Measure(l, Fmt::T12);
                if (cx > pad && cx + w > qa_w - pad) { cx = pad; cy += X(28 + 6); }
                chips.push_back({t, std::move(l), w, cx, cy});
                cx += w + X(6);
            }
            qa_h = cy + X(28) + pad;
        }
        float content = 0;
        for (size_t i = 0; i < steps_.size(); ++i) content += StepH(i) + 8.0f;
        content = X(content + 4.0f);
        // AI: the "thinking" card above the steps, the note card under them.
        const bool generating = ai_task_ != nullptr;
        const float gen_h = generating ? X(84 + 8) : 0.0f;
        std::wstring note;
        if (ai_note_ == AiNote::Info) note = S(StringId::BrAiNote);
        else if (ai_note_ == AiNote::Error) note = ai_error_;
        const float note_text_w = qa_w - X(11 + 14 + 8 + 11);
        const float note_h = note.empty() ? 0.0f
            : Para(note, Fmt::T12, Fmt::T12b, R(0, 0, note_text_w, X(400)), pal_.text2, X(17), false) + X(18 + 8);
        content += gen_h + note_h;
        // Full chip panel only when it fits without scrolling; otherwise a one-line bar.
        const bool qa_full = content + qa_h <= list_bottom - list_top;
        if (!qa_full) qa_h = X(38);
        content += qa_h;
        const float max_scroll = (std::max)(0.0f, content - (list_bottom - list_top));
        if (reveal_step_ >= 0) {
            // Keep the edited card (target layout, not the animated one) fully visible.
            float cy = gen_h;
            for (size_t i = 0; i < steps_.size(); ++i) {
                const float h = X(StepH(i));
                if (steps_ui_[i].id == reveal_step_) {
                    const float view = list_bottom - list_top;
                    float bottom_pad = X(8);
                    if (i + 1 == steps_.size()) bottom_pad += qa_h;   // the last card brings the add bar along
                    if (cy + h + bottom_pad - steps_scroll_ > view) steps_scroll_ = cy + h + bottom_pad - view;
                    if (cy - steps_scroll_ < 0) steps_scroll_ = cy;
                    break;
                }
                cy += h + X(8);
            }
            reveal_step_ = -1;
        }
        steps_scroll_ = std::clamp(steps_scroll_, 0.0f, max_scroll);

        dc_->PushAxisAlignedClip(steps_view_, D2D1_ANTIALIAS_MODE_ALIASED);
        float y = list_top - steps_scroll_;
        if (generating) {
            // .gen: a violet shimmer sweeping across, 1.4 s per pass.
            const D2D1_RECT_F g = R(X(12), y + X(4), qa_w, X(84));
            Fill(g, X(10), pal_.card);
            const float w = g.right - g.left;
            const float phase = static_cast<float>((now_ - ai_started_) % 1400) / 1400.0f;
            const float cx = g.left - w * 0.5f + phase * w * 2.0f;
            ID2D1GradientStopCollection* stops = nullptr;
            D2D1_COLOR_F mid = pal_.card;
            mid.r += (pal_.ai.r - mid.r) * 0.10f;
            mid.g += (pal_.ai.g - mid.g) * 0.10f;
            mid.b += (pal_.ai.b - mid.b) * 0.10f;
            const D2D1_GRADIENT_STOP gs[3] = {{0.0f, pal_.card}, {0.5f, mid}, {1.0f, pal_.card}};
            if (SUCCEEDED(dc_->CreateGradientStopCollection(gs, 3, &stops))) {
                ID2D1LinearGradientBrush* sweep = nullptr;
                if (SUCCEEDED(dc_->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx - w * 0.6f, g.top), D2D1::Point2F(cx + w * 0.6f, g.top)),
                        stops, &sweep))) {
                    dc_->FillRoundedRectangle(D2D1::RoundedRect(g, X(10), X(10)), sweep);
                    sweep->Release();
                }
                stops->Release();
            }
            Stroke(g, X(10), pal_.line);
            Ico(kSpark, g.left + X(16), (g.top + g.bottom) * 0.5f - X(8), X(16), pal_.ai);
            Text(S(StringId::BrAiGenerating), Fmt::T13, D2D1::RectF(g.left + X(42), g.top, g.right - X(16), g.bottom), pal_.text2);
            anim_active_ = true;
            y += gen_h;
        }
        const size_t dragging = drag_index_ >= 0 ? static_cast<size_t>(drag_index_) : SIZE_MAX;
        for (size_t i = 0; i < steps_.size(); ++i) {
            const int id = steps_ui_[i].id;
            const float h = X(StepH(i));
            const float slot = Animate((1ull << 40) | static_cast<uint64_t>(id), y + steps_scroll_, 300.0f) - steps_scroll_;
            if (i != dragging) DrawStepCard(i, slot, h, std::abs(slot - y) < 0.5f);
            y += h + X(8);
        }
        if (!note.empty()) {
            // .ainote (what AI did and did not see) or the reason it failed.
            const bool info = ai_note_ == AiNote::Info;
            const float ny = Animate(11ull << 40, y + steps_scroll_, 300.0f) - steps_scroll_;
            const D2D1_RECT_F nb = R(X(12), ny, qa_w, note_h - X(8));
            Fill(nb, X(9), info ? pal_.ai_bg : pal_.amber_bg);
            Ico(info ? kInfo : kWarn, nb.left + X(11), nb.top + X(10), X(14), info ? pal_.ai : pal_.amber);
            Para(note, Fmt::T12, Fmt::T12b, D2D1::RectF(nb.left + X(33), nb.top + X(9), nb.right - X(11), nb.bottom), pal_.text2, X(17));
            y += note_h;
        }
        {
            const float qy = Animate(1ull << 41, y + steps_scroll_, 300.0f) - steps_scroll_;
            const D2D1_RECT_F box = R(X(12), qy, qa_w, qa_h);
            qa_box_ = box;
            if (!qa_full) {
                const bool hov = Hov({H::AddStep, -1, 1}) || (menu_ == Menu::AddStep && menu_from_box_);
                if (hov) Fill(box, X(10), pal_.hover);
                DashedBox(box, X(10));
                const std::wstring hint = S(StringId::BrAddStepHint);
                const float tw = Measure(hint, Fmt::T13) + X(20);
                const float hx = (box.left + box.right - tw) * 0.5f;
                Ico(kPlus, hx, box.top + X(12), X(14), hov ? pal_.text : pal_.text2);
                Text(hint, Fmt::T13, D2D1::RectF(hx + X(20), box.top, box.right, box.bottom), hov ? pal_.text : pal_.text2);
                Reg(box, {H::AddStep, -1, 1});
                chips.clear();
            } else {
            DashedBox(box, X(10));
            Ico(kPlus, box.left + X(10), box.top + X(12), X(12), pal_.text3);
            Text(S(StringId::BrAddStepHint), Fmt::T12, R(box.left + X(27), box.top + X(8), qa_w - X(37), X(20)), pal_.text3);
            for (const QaChip& c : chips) {
                const D2D1_RECT_F r = R(box.left + c.x, box.top + c.y, c.w, X(28));
                const bool hov = Hov({H::AddItem, static_cast<int>(c.type)});
                Fill(r, X(7), hov ? pal_.hover : pal_.glass);
                Stroke(r, X(7), hov ? pal_.line : pal_.line2);
                Ico(StepIcon(c.type), r.left + X(10), r.top + X(7), X(14), hov ? pal_.text : pal_.text2);
                Text(c.label, Fmt::T12, D2D1::RectF(r.left + X(29), r.top, r.right, r.bottom), hov ? pal_.text : pal_.text2);
                Reg(r, {H::AddItem, static_cast<int>(c.type)});
            }
            }
        }
        if (dragging != SIZE_MAX) {
            const float h = X(StepH(dragging));
            const float gy = std::clamp(drag_y_ - drag_grab_, list_top - h * 0.5f, list_bottom - h * 0.5f);
            DrawStepCard(dragging, gy, h, false, true);
        }
        dc_->PopAxisAlignedClip();
        if (max_scroll > 0) {
            const float view = list_bottom - list_top;
            const float th = (std::max)(X(24), view * view / content);
            const float ty = list_top + (view - th) * (steps_scroll_ / max_scroll);
            Fill(R(pane.right - X(6), ty, X(3), th), X(1.5f), Alpha(pal_.text3, 0.35f));
        }

        // Preset row.
        const D2D1_RECT_F pr = D2D1::RectF(0, list_bottom, pane.right - X(1), bottom);
        Fill(D2D1::RectF(0, pr.top, pr.right, pr.top + X(1)), 0, pal_.line2);
        x = X(16);
        x += Text(S(StringId::BrPreset), Fmt::T125, D2D1::RectF(x, pr.top, pr.right, pr.bottom), pal_.text2) + X(8);
        if (naming_) {
            // 保存为预设: the name field with 保存 / 取消 in place of the dropdown.
            const std::wstring ok = S(StringId::BrSave), no = S(StringId::Cancel);
            const float ow = Measure(ok, Fmt::T125m) + X(16), nw = Measure(no, Fmt::T125) + X(16);
            const D2D1_RECT_F cancel = R(pr.right - X(8) - nw, pr.top + X(8), nw, X(28));
            const D2D1_RECT_F save = R(cancel.left - X(2) - ow, pr.top + X(8), ow, X(28));
            const D2D1_RECT_F box = D2D1::RectF(x, pr.top + X(8), save.left - X(6), pr.top + X(36));
            Field* f = FindField(kPresetField, 0);
            const bool focus = f && GetFocus() == f->hwnd;
            Fill(box, X(7), pal_.field);
            Stroke(box, X(7), focus ? pal_.accent : pal_.field_line);
            if (focus) Fill(D2D1::RectF(box.left + X(1), box.bottom - X(2.5f), box.right - X(1), box.bottom - X(1)), 0, pal_.accent);
            if (f) {
                f->want = box;
                f->want_visible = true;
                if (GetWindowTextLengthW(f->hwnd) == 0 && !focus)
                    Text(S(StringId::BrPresetNameHint), Fmt::T125, D2D1::RectF(box.left + X(9), box.top, box.right, box.bottom), pal_.text3);
            }
            if (Hov({H::PresetOk})) Fill(save, X(7), pal_.hover);
            Text(ok, Fmt::T125m, save, pal_.accent, DWRITE_TEXT_ALIGNMENT_CENTER);
            Reg(save, {H::PresetOk});
            if (Hov({H::PresetCancel})) Fill(cancel, X(7), pal_.hover);
            Text(no, Fmt::T125, cancel, pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
            Reg(cancel, {H::PresetCancel});
            return;
        }
        const std::wstring save = S(StringId::BrSavePreset);
        const float sw = Measure(save, Fmt::T125m);
        const D2D1_RECT_F sv = R(pr.right - X(14) - sw, pr.top + X(8), sw, X(28));
        const std::wstring preset = preset_name_.empty() ? S(StringId::BrPresetNone) : preset_name_;
        const bool edited = PresetEdited();
        const std::wstring mark = edited ? L" \x00B7 " + S(StringId::BrPresetEdited) : std::wstring();
        const float max_w = sv.left - X(12) - x;
        const float mw = edited ? Measure(mark, Fmt::T12) : 0.0f;
        const float dw = (std::min)(Measure(preset, Fmt::T125) + mw + X(8 + 4 + 12 + 8), max_w);
        const D2D1_RECT_F dd = R(x, pr.top + X(8), dw, X(28));
        preset_dd_ = dd;
        Fill(dd, X(7), Hov({H::PresetDD}) || menu_ == Menu::Preset ? pal_.hover : pal_.card);
        Stroke(dd, X(7), pal_.line);
        const float text_right = dd.right - X(8 + 12 + 4);
        const float nw = (std::min)(Measure(preset, Fmt::T125), text_right - dd.left - X(8) - mw);
        Text(preset, Fmt::T125, D2D1::RectF(dd.left + X(8), dd.top, dd.left + X(8) + nw, dd.bottom), pal_.text);
        if (edited) Text(mark, Fmt::T12, D2D1::RectF(dd.left + X(8) + nw, dd.top, text_right, dd.bottom), pal_.text3);
        Ico(kChev, dd.right - X(8 + 12), dd.top + X(8), X(12), pal_.text3);
        Reg(dd, {H::PresetDD});
        Text(save, Fmt::T125m, sv, Hov({H::PresetSave}) ? pal_.accent_h : pal_.accent);
        Reg(sv, {H::PresetSave});
    }

    // ---------------------------------------------------------------- 按内容命名 (left pane)

    void Ring(float cx, float cy, float r, D2D1_COLOR_F c, float w) {
        dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), B(c), w);
    }

    // .spin: a ring at 25% with a quarter arc turning once per 0.8 s.
    void Spinner(float cx, float cy, float size, D2D1_COLOR_F c) {
        const float w = X(1.8f), r = size * 0.5f - w * 0.5f;
        Ring(cx, cy, r, Alpha(c, 0.25f), w);
        anim_active_ = true;
        ID2D1Factory* factory = nullptr;
        dc_->GetFactory(&factory);
        if (!factory) return;
        ID2D1PathGeometry* path = nullptr;
        if (SUCCEEDED(factory->CreatePathGeometry(&path))) {
            ID2D1GeometrySink* sink = nullptr;
            if (SUCCEEDED(path->Open(&sink))) {
                const float a0 = static_cast<float>(now_ % 800) / 800.0f * 6.2831853f - 1.5707963f;
                const float a1 = a0 + 1.5707963f;
                sink->BeginFigure(D2D1::Point2F(cx + r * std::cos(a0), cy + r * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r * std::cos(a1), cy + r * std::sin(a1)), D2D1::SizeF(r, r),
                                              0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                sink->Release();
                dc_->DrawGeometry(path, B(c), w);
            }
            path->Release();
        }
        factory->Release();
    }

    // .seg2 at (x, y); disabled: dimmed and not clickable.
    void SegRow(const StringId* labels, int n, int on, float x, float y, H kind, bool enabled) {
        float w = X(4);
        for (int m = 0; m < n; ++m) w += Measure(S(labels[m]), Fmt::T12) + X(18) + (m ? X(2) : 0);
        Fill(R(x, y, w, X(28)), X(7), pal_.hover);
        float sx = x + X(2);
        for (int m = 0; m < n; ++m) {
            const std::wstring label = S(labels[m]);
            const float iw = Measure(label, Fmt::T12) + X(18);
            const D2D1_RECT_F item = R(sx, y + X(2), iw, X(24));
            if (m == on) {
                Shadow(item, X(5), X(2), X(1), Hex(0x000000, 0.08f));
                Fill(item, X(5), pal_.card);
            }
            const D2D1_COLOR_F tc = m == on ? pal_.text : pal_.text2;
            Text(label, Fmt::T12, item, enabled ? tc : Alpha(tc, 0.55f), DWRITE_TEXT_ALIGNMENT_CENTER);
            if (enabled) Reg(item, {kind, m});
            sx += iw + X(2);
        }
    }

    void DrawContentPane() {
        const float top = X(kBodyTop), bottom = H_ - X(kFootH);
        const D2D1_RECT_F pane = D2D1::RectF(0, top, X(kStepsW), bottom);
        Fill(pane, 0, pal_.glass_pane);
        Fill(D2D1::RectF(0, top, W_, top + X(1)), 0, pal_.line2);
        Fill(D2D1::RectF(pane.right - X(1), top, pane.right, bottom), 0, pal_.line2);
        Reg(pane, {H::StepsArea});

        // Header: 按内容命名 · AI 读取文字后起名.
        const D2D1_RECT_F head = D2D1::RectF(0, top, pane.right, top + X(kPaneHeadH));
        float hx = X(16);
        hx += Text(S(StringId::BrModeContent), Fmt::T13b, D2D1::RectF(hx, head.top, pane.right, head.bottom), pal_.text) + X(8);
        Text(S(StringId::BrCmSub), Fmt::T13, D2D1::RectF(hx, head.top, pane.right - X(14), head.bottom), pal_.text3);

        const bool busy = ContentRunning();
        const float list_top = head.bottom + X(2), list_bottom = bottom - X(kPresetH);
        steps_view_ = D2D1::RectF(0, list_top, pane.right, list_bottom);
        const float view_h = list_bottom - list_top;
        c_scroll_ = std::clamp(c_scroll_, 0.0f, (std::max)(0.0f, c_content_h_ - view_h));
        const float l = X(14), w = pane.right - X(1) - X(28);
        dc_->PushAxisAlignedClip(steps_view_, D2D1_ANTIALIAS_MODE_ALIASED);
        float y = list_top - c_scroll_;
        auto group_label = [&](StringId label, const std::wstring& sub) {
            const float lw = Text(S(label), Fmt::T125b, R(l, y, w, X(17)), pal_.text);
            if (!sub.empty()) Text(sub, Fmt::T125, R(l + lw + X(6), y, w - lw - X(6), X(17)), pal_.text3);
            y += X(17 + 5);
        };

        // 名字格式: three radio cards.
        group_label(StringId::BrCmFormat, {});
        static const StringId formats[] = {StringId::BrCmF1, StringId::BrCmF2, StringId::BrCmF3};
        static const StringId examples[] = {StringId::BrCmF1E, StringId::BrCmF2E, StringId::BrCmF3E};
        for (int k = 0; k < 3; ++k) {
            const D2D1_RECT_F card = R(l, y, w, X(32));
            const bool on = c_format_ == k;
            const bool hov = !busy && !on && Hov({H::CFormat, k});
            Fill(card, X(9), on ? Mix(pal_.glass, pal_.accent_tint, 0.6f) : hov ? Mix(pal_.glass, pal_.hover, 0.5f) : pal_.glass);
            Stroke(card, X(9), on ? pal_.accent : pal_.line, on ? X(1.5f) : X(1.0f));
            const float cx = card.left + X(10 + 8), cy = (card.top + card.bottom) * 0.5f;
            if (on) Ring(cx, cy, X(5.5f), pal_.accent, X(5.0f));
            else Ring(cx, cy, X(7.25f), pal_.text3, X(1.5f));
            const float lx = card.left + X(10 + 16 + 10), rx = card.right - X(10);
            const float lw = Text(S(formats[k]), Fmt::T13, D2D1::RectF(lx, card.top, rx, card.bottom), pal_.text);
            const float ex = lx + lw + X(10);
            if (ex < rx - X(24))
                Text(Ellipsize(S(examples[k]), Fmt::T12, rx - ex), Fmt::T12, D2D1::RectF(ex, card.top, rx, card.bottom),
                     pal_.text3, DWRITE_TEXT_ALIGNMENT_TRAILING);
            if (!busy) Reg(card, {H::CFormat, k});
            y += X(32 + 5);
        }
        y += X(8 - 5);

        // 名字语言.
        group_label(StringId::BrCmLang, {});
        static const StringId langs[] = {StringId::BrCmLangZh, StringId::BrCmLangEn, StringId::BrCmLangAuto};
        SegRow(langs, 3, c_lang_, l, y, H::CLang, !busy);
        y += X(28 + 10);

        // 读取范围 + what can be read.
        group_label(StringId::BrCmRange, S(StringId::BrCmRangeSub));
        static const StringId ranges[] = {StringId::BrCmR1, StringId::BrCmR2, StringId::BrCmR3};
        SegRow(ranges, 3, c_range_, l, y, H::CRange, !busy);
        y += X(28 + 6);
        y += Para(S(StringId::BrCmSupport), Fmt::T12, Fmt::T12b, R(l, y, w, 0), pal_.text3, X(17)) + X(8);

        // What is sent, to whom, and roughly how much.
        const int n = ReadableCount();
        size_t name_chars = 0;
        for (size_t i = 0; i < sources_.size(); ++i)
            if (Readable(i)) name_chars += sources_[i].name.size();
        const size_t chars = static_cast<size_t>(n) * (busy ? c_budget_ : RangeBudget()) + name_chars;
        const size_t batches = (static_cast<size_t>(n) + 4) / 5;
        const size_t tokens = chars * 7 / 10 + batches * 600 + static_cast<size_t>(n) * 40;
        auto qty = [](size_t v) { return FmtS(StringId::BrCmQtyK, (std::max)(1, static_cast<int>((v + 500) / 1000))); };
        std::wstring privacy;
        if (options_.ai_model.empty() || !options_.ai_start) privacy = S(StringId::BrAiNoConn);
        else {
            const std::wstring provider = options_.ai_provider.empty() ? std::wstring(L"AI") : options_.ai_provider;
            privacy = FmtS(StringId::BrCmPrivacy, n, qty(chars).c_str(), options_.ai_model.c_str(), provider.c_str(),
                           qty(tokens).c_str());
            for (size_t at = privacy.find(L"\\n"); at != std::wstring::npos; at = privacy.find(L"\\n", at)) privacy.replace(at, 2, L"\n");
        }
        const float pw = w - X(11 + 15 + 9 + 11);
        const float ph = Para(privacy, Fmt::T12, Fmt::T12b, D2D1::RectF(0, 0, pw, 0), pal_.text2, X(16.5f), false);
        const D2D1_RECT_F card = R(l, y, w, ph + X(18));
        Fill(card, X(9), pal_.amber_bg);
        Stroke(card, X(9), pal_.amber_line);
        Ico(kShield, card.left + X(11), card.top + X(10), X(15), pal_.amber);
        Para(privacy, Fmt::T12, Fmt::T12b, R(card.left + X(35), card.top + X(9), pw, ph), pal_.text2, X(16.5f));
        y = card.bottom;
        if (!c_error_.empty()) {
            y += X(8);
            y += Para(c_error_, Fmt::T12, Fmt::T12b, R(l, y, w, 0), pal_.amber, X(17));
        }
        y += X(2);
        c_content_h_ = y + c_scroll_ - list_top;
        dc_->PopAxisAlignedClip();
        const float max_scroll = (std::max)(0.0f, c_content_h_ - view_h);
        if (max_scroll > 0) {
            const float th = (std::max)(X(24), view_h * view_h / c_content_h_);
            const float ty = list_top + (view_h - th) * (c_scroll_ / max_scroll);
            Fill(R(pane.right - X(6), ty, X(3), th), X(1.5f), Alpha(pal_.text3, 0.35f));
        }

        // 开始生成建议 / 停止 / 重新生成, pinned where the presets sit in 规则 so
        // it is always in view right under the privacy note.
        const D2D1_RECT_F pr = D2D1::RectF(0, list_bottom, pane.right - X(1), bottom);
        if (max_scroll > 0) Fill(D2D1::RectF(0, pr.top, pr.right, pr.top + X(1)), 0, pal_.line2);
        const D2D1_RECT_F btn = D2D1::RectF(l, pr.top + X(4), l + w, pr.bottom - X(8));
        const bool can = n > 0;
        const bool hov = Hov({H::CStart});
        std::wstring label;
        const Icon* icon = nullptr;
        D2D1_COLOR_F fg{};
        if (busy) {
            Fill(btn, X(8), hov ? Alpha(pal_.text2, 0.85f) : pal_.text2);
            label = S(StringId::BrStop);
            icon = &kStop;
            fg = pal_.card;
        } else {
            Fill(btn, X(8), can ? (hov ? pal_.accent_h : pal_.accent) : Alpha(pal_.accent, 0.45f));
            label = S(c_ran_ ? StringId::BrCmRegenShort : StringId::BrCmStart);
            icon = &kSpark;
            fg = pal_.on_accent;
        }
        const float tw = Measure(label, Fmt::T135b);
        const float bx = (btn.left + btn.right - (X(16 + 6) + tw)) * 0.5f;
        Ico(*icon, bx, (btn.top + btn.bottom) * 0.5f - X(8), X(16), fg);
        Text(label, Fmt::T135b, D2D1::RectF(bx + X(22), btn.top, btn.right, btn.bottom), fg);
        if (busy || can) Reg(btn, {H::CStart});
    }

    void DashedBox(const D2D1_RECT_F& r, float radius) {
        ID2D1Factory* factory = nullptr;
        dc_->GetFactory(&factory);
        if (!factory) return;
        ID2D1StrokeStyle* dash = nullptr;
        const float dashes[] = {3.0f, 2.5f};
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                               D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND, 10.0f,
                                                               D2D1_DASH_STYLE_CUSTOM),
                                   dashes, 2, &dash);
        const float w = X(1.5f);
        const D2D1_RECT_F in = D2D1::RectF(r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2);
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(in, radius, radius), B(pal_.line), w, dash);
        if (dash) dash->Release();
        factory->Release();
    }

    void DrawStepCard(size_t i, float y, float h, bool settled, bool lifted = false) {
        const rn::Step& st = steps_[i];
        const StepUi& ui = steps_ui_[i];
        const int si = static_cast<int>(i);
        // Enter: fade + rise 10 px over 300 ms.
        float enter = 1.0f;
        if (auto it = enter_.find(ui.id); it != enter_.end()) {
            // A preset's cards come in one after another (start may be ahead).
            enter = now_ < it->second ? 0.0f : EaseOut(static_cast<float>(now_ - it->second) / 300.0f);
            if (enter < 1.0f) anim_active_ = true;
            else enter_.erase(it);
        }
        y += X(10) * (1.0f - enter);
        const D2D1_RECT_F card = R(X(12), y, X(kStepsW - 24), h);
        const float saved_fade = fade_;
        fade_ = enter;
        if (lifted) Shadow(card, X(10), X(16), X(6), Hex(0x000000, dark_ ? 0.5f : 0.16f));
        if (lifted || !glass_) Shadow(card, X(10), X(2), X(1), Hex(0x000000, 0.04f));
        Fill(card, X(10), lifted ? pal_.card : pal_.glass);
        const bool focused = focus_step_ == ui.id || lifted;
        if (focused) Stroke(D2D1::RectF(card.left - X(0.75f), card.top - X(0.75f), card.right + X(0.75f), card.bottom + X(0.75f)),
                            X(10.75f), pal_.accent, X(1.5f));
        else Stroke(card, X(10), pal_.line);
        Reg(card, {H::Card, si});

        const float body_fade = st.on ? 1.0f : 0.45f;
        fade_ = enter * body_fade;
        // Header.
        const float hy = card.top;
        const D2D1_RECT_F grip = R(card.left + X(4), hy + X(8), X(18), X(24));
        Ico(kGrip, card.left + X(6), hy + X(13), X(14), Alpha(pal_.text3, Hov({H::Grip, si}) ? 1.0f : 0.7f));
        Reg(grip, {H::Grip, si});
        const D2D1_RECT_F sn = R(card.left + X(28), hy + X(10), X(20), X(20));
        Fill(sn, X(6), pal_.hover);
        Text(std::to_wstring(i + 1), Fmt::T115b, sn, pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
        Ico(StepIcon(st.type), card.left + X(56), hy + X(12), X(16), pal_.text2);
        float tx = card.left + X(80);
        tx += Text(S(StepTitle(st.type)), Fmt::T135b, R(tx, hy, X(160), X(40)), pal_.text) + X(8);
        if (st.ai) {
            const float aw = Measure(L"AI", Fmt::T11b) + X(6 + 10 + 3 + 6);
            const D2D1_RECT_F tag = R(tx, hy + X(11), aw, X(18));
            Fill(tag, X(5), pal_.ai_bg);
            Ico(kSpark, tag.left + X(6), tag.top + X(4), X(10), pal_.ai);
            Text(L"AI", Fmt::T11b, D2D1::RectF(tag.left + X(19), tag.top, tag.right, tag.bottom), pal_.ai);
        }
        fade_ = enter;
        // Switch + delete.
        const D2D1_RECT_F del = R(card.right - X(8 + 26), hy + X(7), X(26), X(26));
        if (Hov({H::Delete, si})) Fill(del, X(6), pal_.hover);
        Ico(kX, del.left + X(7), del.top + X(7), X(12), pal_.text3);
        Reg(del, {H::Delete, si});
        const D2D1_RECT_F sw = R(del.left - X(8 + 32), hy + X(11), X(32), X(18));
        const float k = Animate((2ull << 40) | static_cast<uint64_t>(ui.id), st.on ? 1.0f : 0.0f, 200.0f);
        if (k > 0.0f) Fill(sw, X(9), Alpha(pal_.accent, k));
        if (k < 1.0f) Stroke(sw, X(9), Alpha(pal_.text3, 1.0f - k), X(1.5f));
        const float kx = sw.left + X(3) + X(14) * k;
        Fill(R(kx, sw.top + X(3), X(12), X(12)), X(6), Mix(pal_.text3, Hex(0xffffff), k));
        Reg(D2D1::RectF(sw.left - X(4), sw.top - X(4), sw.right + X(4), sw.bottom + X(4)), {H::Toggle, si});

        // Body.
        fade_ = enter * body_fade;
        const float bx = card.left + X(34), bw = X(kStepsW - 24 - 34 - 12);
        float by = hy + X(40);
        const bool show_fields = settled && !lifted && enter >= 1.0f;
        auto field_box = [&](int field, const D2D1_RECT_F& r, D2D1_COLOR_F back) {
            Field* f = FindField(ui.id, field);
            const bool focus = f && GetFocus() == f->hwnd;
            Fill(r, X(7), back);
            Stroke(r, X(7), focus ? pal_.accent : pal_.field_line);
            if (focus) Fill(D2D1::RectF(r.left + X(1), r.bottom - X(2.5f), r.right - X(1), r.bottom - X(1)), 0, pal_.accent);
            Reg(r, {H::Field, si, field});
            if (!f) return;
            const bool inside = r.top >= steps_view_.top && r.bottom <= steps_view_.bottom;
            f->want = r;
            // Edit windows sit above the canvas: under an open menu they hide
            // and the card draws their text instead.
            const bool covered = menu_ != Menu::None && r.right > menu_box_.left && r.left < menu_box_.right &&
                                 r.bottom > menu_box_.top && r.top < menu_box_.bottom;
            f->want_visible = show_fields && inside && !covered;
            if (!f->want_visible) {
                const std::wstring v = FieldValue(si, field);
                if (!v.empty())
                    Text(v, Fmt::T13, D2D1::RectF(r.left + X(f->numeric ? 3.0f : 9.0f), r.top, r.right - X(9), r.bottom), pal_.text,
                         f->numeric ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        };
        auto label = [&](const std::wstring& s, float yy) {
            Text(s, Fmt::T125, R(bx, yy, X(46), X(30)), pal_.text2);
        };
        auto chip = [&](const std::wstring& s, float cx, float cy, bool on_, Hit hit, bool token = false) {
            const float w = Measure(s, on_ ? Fmt::T12m : Fmt::T12) + X(18);
            const D2D1_RECT_F r = R(cx, cy, w, X(24));
            if (on_) Fill(r, X(6), pal_.accent_soft);
            else {
                Fill(r, X(6), Hov(hit) ? pal_.hover : pal_.glass);
                if (token) DashedStroke(r, X(6), Alpha(pal_.ai, 0.7f));
                else Stroke(r, X(6), pal_.line);
            }
            Text(s, on_ ? Fmt::T12m : Fmt::T12, r, on_ ? pal_.accent : (token ? pal_.ai : pal_.text2),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
            Reg(r, hit);
            return w;
        };
        auto seg2 = [&](const StringId* labels, const rn::CaseMode* modes, int n, float sy) {
            float w = X(4);
            for (int m = 0; m < n; ++m) w += Measure(S(labels[m]), Fmt::T12) + X(18) + (m ? X(2) : 0);
            const D2D1_RECT_F r = R(bx, sy, w, X(28));
            Fill(r, X(7), pal_.hover);
            float sx = r.left + X(2);
            for (int m = 0; m < n; ++m) {
                const std::wstring s = S(labels[m]);
                const float iw = Measure(s, Fmt::T12) + X(18);
                const D2D1_RECT_F item = R(sx, sy + X(2), iw, X(24));
                const bool on_ = st.mode == modes[m];
                if (on_) {
                    Shadow(item, X(5), X(2), X(1), Hex(0x000000, 0.08f));
                    Fill(item, X(5), pal_.card);
                }
                Text(s, Fmt::T12, item, on_ ? pal_.text : pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
                Reg(item, {H::Seg, si, static_cast<int>(modes[m])});
                sx += iw + X(2);
            }
        };

        // Position segments (Insert / Number) set Step::where.
        auto seg_where = [&](int n, float sy) {
            static const StringId labels[] = {StringId::BrPosStart, StringId::BrPosEnd, StringId::BrPosIndex};
            float w = X(4);
            for (int m = 0; m < n; ++m) w += Measure(S(labels[m]), Fmt::T12) + X(18) + (m ? X(2) : 0);
            Fill(R(bx, sy, w, X(28)), X(7), pal_.hover);
            float sx = bx + X(2);
            for (int m = 0; m < n; ++m) {
                const std::wstring s = S(labels[m]);
                const float iw = Measure(s, Fmt::T12) + X(18);
                const D2D1_RECT_F item = R(sx, sy + X(2), iw, X(24));
                const bool on_ = static_cast<int>(st.where) == m;
                if (on_) {
                    Shadow(item, X(5), X(2), X(1), Hex(0x000000, 0.08f));
                    Fill(item, X(5), pal_.card);
                }
                Text(s, Fmt::T12, item, on_ ? pal_.text : pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
                Reg(item, {H::Seg, si, kSegWhere + m});
                sx += iw + X(2);
            }
        };
        auto number_row = [&](float ny) {
            label(S(StringId::BrTokNumber), ny);
            float nx = bx + X(54);
            const StringId hints[] = {StringId::BrNumFrom, StringId::BrNumDigits, StringId::BrNumStep};
            for (int f = 0; f < 3; ++f) {
                nx += Text(S(hints[f]), Fmt::T12, R(nx, ny, X(60), X(30)), pal_.text3) + X(6);
                field_box(kStart + f, R(nx, ny, X(46), X(30)), pal_.field);
                nx += X(46 + 10);
            }
        };

        switch (st.type) {
        case rn::StepType::Insert: {
            label(S(StringId::BrInsText), by);
            const D2D1_RECT_F r = R(bx + X(54), by, bw - X(54), X(30));
            field_box(kText, r, pal_.field);
            if (const std::wstring hint = WsHint(st.text); !hint.empty()) {
                const float hw = Measure(hint, Fmt::T11) + X(12);
                const D2D1_RECT_F hr = R(r.right - X(6) - hw, r.top + X(6), hw, X(18));
                Fill(hr, X(5), pal_.hover);
                Text(hint, Fmt::T11, hr, pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            by += X(38);
            seg_where(3, by);
            if (st.where == rn::Where::Index) {
                by += X(36);
                float nx = bx;
                nx += Text(S(StringId::BrPosAfterPre), Fmt::T12, R(nx, by, X(80), X(30)), pal_.text3) + X(6);
                field_box(kAt, R(nx, by, X(46), X(30)), pal_.field);
                Text(S(StringId::BrPosAfterPost), Fmt::T12, R(nx + X(52), by, X(120), X(30)), pal_.text3);
            }
            break;
        }
        case rn::StepType::Number: {
            seg_where(2, by);
            by += X(36);
            label(S(StringId::BrNumSep), by);
            const D2D1_RECT_F r = R(bx + X(54), by, X(120), X(30));
            field_box(kSep, r, pal_.field);
            if (const std::wstring hint = WsHint(st.sep); !hint.empty()) {
                const float hw = Measure(hint, Fmt::T11) + X(12);
                Fill(R(r.right - X(6) - hw, r.top + X(6), hw, X(18)), X(5), pal_.hover);
                Text(hint, Fmt::T11, R(r.right - X(6) - hw, r.top + X(6), hw, X(18)), pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            by += X(38);
            number_row(by);
            break;
        }
        case rn::StepType::Replace: {
            const StringId labels[2] = {StringId::BrFind, StringId::BrWith};
            for (int f = 0; f < 2; ++f) {
                label(S(labels[f]), by);
                const D2D1_RECT_F r = R(bx + X(54), by, bw - X(54), X(30));
                field_box(f, r, pal_.field);
                const std::wstring v = FieldValue(si, f);
                if (const std::wstring hint = WsHint(v); !hint.empty()) {
                    const float hw = Measure(hint, Fmt::T11) + X(12);
                    const D2D1_RECT_F hr = R(r.right - X(6) - hw, r.top + X(6), hw, X(18));
                    Fill(hr, X(5), pal_.hover);
                    Text(hint, Fmt::T11, hr, pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
                } else if (f == kWith && v.empty()) {
                    Field* fld = FindField(ui.id, kWith);
                    if (!fld || GetFocus() != fld->hwnd)
                        Text(S(StringId::BrWithPlaceholder), Fmt::T13, D2D1::RectF(r.left + X(9), r.top, r.right, r.bottom),
                             pal_.text3);
                    if (fld && GetFocus() != fld->hwnd) fld->want_visible = false;
                }
                by += X(38);
            }
            float cx = bx + X(54);
            cx += chip(S(StringId::BrRegex), cx, by, st.regex, {H::Chip, si, 0}) + X(5);
            cx += chip(S(StringId::BrMatchCase), cx, by, st.match_case, {H::Chip, si, 1}) + X(5);
            chip(S(StringId::BrAll), cx, by, st.all, {H::Chip, si, 2});
            if (plan_.bad_regex && st.regex && st.on)
                Text(S(StringId::BrBadRegex), Fmt::T115, R(bx + X(54), card.bottom - X(14), bw, X(14)), pal_.bad);
            break;
        }
        case rn::StepType::Remove: {
            float cx = bx;
            cx += chip(S(StringId::BrRmBrackets), cx, by, st.brackets, {H::Chip, si, 0}) + X(5);
            cx += chip(S(StringId::BrRmSpaces), cx, by, st.spaces, {H::Chip, si, 1}) + X(5);
            cx += chip(S(StringId::BrRmDigits), cx, by, st.digits, {H::Chip, si, 2}) + X(5);
            chip(S(StringId::BrRmCustom), cx, by, ui.custom_on, {H::Chip, si, 3});
            if (ui.custom_on) field_box(kCustom, R(bx, by + X(32), bw, X(30)), pal_.field);
            break;
        }
        case rn::StepType::Case: {
            static const StringId l[] = {StringId::BrCaseKeep, StringId::BrCaseLower, StringId::BrCaseUpper, StringId::BrCaseTitle};
            static const rn::CaseMode m[] = {rn::CaseMode::Keep, rn::CaseMode::Lower, rn::CaseMode::Upper, rn::CaseMode::Title};
            seg2(l, m, 4, by);
            break;
        }
        case rn::StepType::Template: {
            field_box(kPattern, R(bx, by, bw, X(30)), pal_.field);
            by += X(38);
            float cx = bx;
            for (int tk = 0; tk < 6; ++tk) {
                const float w = Measure(TokenLabel(tk), Fmt::T12) + X(18);
                if (cx > bx && cx + w > bx + bw) { cx = bx; by += X(29); }
                cx += chip(TokenLabel(tk), cx, by, false, {H::Token, si, tk}, true) + X(5);
            }
            by += X(24 + 8);
            if (rn::UsesNumber(st.pattern)) number_row(by);
            break;
        }
        case rn::StepType::Ext: {
            static const StringId l[] = {StringId::BrCaseKeep, StringId::BrExtLower, StringId::BrExtUpper, StringId::BrExtChange};
            static const rn::CaseMode m[] = {rn::CaseMode::Keep, rn::CaseMode::Lower, rn::CaseMode::Upper, rn::CaseMode::Change};
            seg2(l, m, 4, by);
            if (st.mode == rn::CaseMode::Change) field_box(kExtTo, R(bx, by + X(36), X(140), X(30)), pal_.field);
            break;
        }
        }
        fade_ = saved_fade;
    }

    void DashedStroke(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
        ID2D1Factory* factory = nullptr;
        dc_->GetFactory(&factory);
        if (!factory) return;
        ID2D1StrokeStyle* dash = nullptr;
        const float dashes[] = {3.0f, 2.0f};
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                               D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND, 10.0f,
                                                               D2D1_DASH_STYLE_CUSTOM),
                                   dashes, 2, &dash);
        const float w = X(1.0f);
        const D2D1_RECT_F in = D2D1::RectF(r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2);
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(in, radius, radius), B(c), w, dash);
        if (dash) dash->Release();
        factory->Release();
    }

    // ---------------------------------------------------------------- preview

    void DrawPreview() {
        const float top = X(options_.proposed_names ? kTitleH : kBodyTop) + X(1), bottom = H_ - X(kFootH);
        const float left = options_.proposed_names ? 0.0f : X(kStepsW);
        const D2D1_RECT_F pane = D2D1::RectF(left, top, W_, bottom);
        Reg(pane, {H::Preview});

        // Filter tabs.
        const int counts[4] = {static_cast<int>(plan_.rows.size()), changed_, issues_, same_};
        static const StringId labels[4] = {StringId::BrAll, StringId::BrTabChange, StringId::BrTabIssues, StringId::BrTabSame};
        float x = left + X(12);
        for (int i = 0; i < 4; ++i) {
            const bool on = filter_ == i;
            const std::wstring label = S(labels[i]);
            const std::wstring count = std::to_wstring(counts[i]);
            const Fmt lf = on ? Fmt::T13m : Fmt::T13;
            const float w = X(10) + Measure(label, lf) + X(6) + Measure(count, Fmt::T12b) + X(10);
            const D2D1_RECT_F tab = R(x, top + X(8), w, X(28));
            if (on || Hov({H::Tab, i})) Fill(tab, X(7), pal_.hover);
            const float lw = Text(label, lf, D2D1::RectF(tab.left + X(10), tab.top, tab.right, tab.bottom), on ? pal_.text : pal_.text2);
            const D2D1_COLOR_F cc = i == 2 && counts[i] ? pal_.amber : (on ? pal_.text2 : pal_.text3);
            Text(count, Fmt::T12b, D2D1::RectF(tab.left + X(10) + lw + X(6), tab.top, tab.right, tab.bottom), cc);
            Reg(tab, {H::Tab, i});
            x += w + X(4);
        }
        if (content_mode_) {
            // ✦ 全部重新生成 in place of the numbering order.
            if (c_ran_ && !ContentRunning()) {
                const std::wstring rg = S(StringId::BrCmRegen);
                const float rw = X(8 + 14 + 5) + Measure(rg, Fmt::T125m) + X(8);
                const D2D1_RECT_F link = R(W_ - X(14) - rw, top + X(8), rw, X(28));
                if (Hov({H::CRegen})) Fill(link, X(7), pal_.hover);
                Ico(kSpark, link.left + X(8), link.top + X(7), X(14), pal_.ai);
                Text(rg, Fmt::T125m, D2D1::RectF(link.left + X(27), link.top, link.right, link.bottom), pal_.ai);
                Reg(link, {H::CRegen});
            }
        } else if (!options_.proposed_names) {
        // Numbering order.
        static const StringId orders[] = {StringId::BrOrderList, StringId::BrOrderTaken, StringId::BrOrderModified,
                                          StringId::BrOrderName};
        const std::wstring ol = S(StringId::BrOrder),
                           ov = S(orders[static_cast<int>(opts_.order)]) +
                                (opts_.daily ? L" \u00B7 " + S(StringId::BrOrderDailyShort) : std::wstring());
        const float ow = X(8) + Measure(ol, Fmt::T125) + X(4) + Measure(ov, Fmt::T125m) + X(4 + 12 + 8);
        const D2D1_RECT_F order = R(W_ - X(14) - ow, top + X(8), ow, X(28));
        if (Hov({H::OrderDD}) || menu_ == Menu::Order) Fill(order, X(7), pal_.hover);
        float ox = order.left + X(8);
        ox += Text(ol, Fmt::T125, D2D1::RectF(ox, order.top, order.right, order.bottom), pal_.text2) + X(4);
        Text(ov, Fmt::T125m, D2D1::RectF(ox, order.top, order.right, order.bottom), pal_.text);
        Ico(kChev, order.right - X(8 + 12), order.top + X(8), X(12), pal_.text3);
        Reg(order, {H::OrderDD});
        order_dd_ = order;
        }

        // Column header: 28 | 1fr | 22 | 1fr | 128 with 14 / 16 padding.
        const float hy = top + X(kPaneHeadH);
        const D2D1_RECT_F head = D2D1::RectF(left, hy, W_, hy + X(30));
        Fill(D2D1::RectF(left, head.bottom - X(1), W_, head.bottom), 0, pal_.line2);
        const float cl = left + X(14), cr = W_ - X(16);
        const float st_w = X(content_mode_ ? 104.0f : 128.0f);
        const float fr = (cr - cl - X(28 + 22) - st_w) / (content_mode_ ? 2.25f : 2.0f);
        Text(S(StringId::BrTokName), Fmt::T12, D2D1::RectF(cl + X(28), head.top, cl + X(28) + fr, head.bottom), pal_.text3);
        Text(S(StringId::BrColNew), Fmt::T12, D2D1::RectF(cl + X(50) + fr, head.top, cr - st_w, head.bottom), pal_.text3);
        Text(S(StringId::BrColStatus), Fmt::T12, D2D1::RectF(cr - st_w, head.top, cr, head.bottom), pal_.text3,
             DWRITE_TEXT_ALIGNMENT_TRAILING);

        // Rows.
        const D2D1_RECT_F view = D2D1::RectF(left, head.bottom, W_, bottom);
        rows_view_ = view;
        const bool two = filter_ == 2 || content_mode_;
        const bool show_note = filter_ == 2 && !content_mode_;
        std::vector<size_t> vis;
        for (size_t i = 0; i < plan_.rows.size(); ++i) if (RowVisible(plan_.rows[i])) vis.push_back(i);
        const float rh = X(two ? kTallH + 4 : kRowH);
        float note_h = 0;
        const float note_w = view.right - view.left - X(16) - X(28 + 14);
        if (show_note && !vis.empty()) note_h = X(10) + X(24) + Para(S(StringId::BrIssNote), Fmt::T125, Fmt::T125b,
                                                                D2D1::RectF(0, 0, note_w, 0), pal_.text2, X(19), false);
        // 按内容命名: only rows with a 依据 line are tall.
        auto row_h = [&](size_t i) {
            if (!content_mode_) return rh;
            const CSt cst = crows_[i].st;
            const bool tall = !crows_[i].basis.empty() && cst != CSt::Wait && cst != CSt::Read && cst != CSt::Think;
            return tall ? X(kTallH + 4) : X(kRowH);
        };
        float rows_h = 0;
        for (size_t i : vis) rows_h += row_h(i);
        const float content = X(8) + rows_h + note_h;
        const float max_scroll = (std::max)(0.0f, content - (view.bottom - view.top));
        rows_scroll_ = std::clamp(rows_scroll_, 0.0f, max_scroll);
        dc_->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_ALIASED);
        float y = view.top + X(4) - rows_scroll_;
        for (size_t i : vis) {
            const float hi = row_h(i);
            const bool tall_row = content_mode_ ? hi > X(kRowH) : two;
            if (y + hi >= view.top && y <= view.bottom) {
                const D2D1_RECT_F rr = D2D1::RectF(left + X(8), y, W_ - X(8), y + (tall_row ? X(kTallH) : hi));
                if (content_mode_) DrawContentRow(i, rr);
                else DrawRow(i, rr, two);
            } else if (content_mode_ && c_edit_ == static_cast<int>(i)) {
                if (Field* f = FindField(kRowField, c_edit_)) f->want_visible = false;
            }
            y += hi;
        }
        if (show_note && !vis.empty()) {
            const D2D1_RECT_F note = D2D1::RectF(left + X(16), y + X(10) - X(4), W_ - X(16), y + note_h - X(4));
            Fill(note, X(10), pal_.glass_pane);
            Stroke(note, X(10), pal_.line2);
            Ico(kShield, note.left + X(14), note.top + X(14), X(15), pal_.accent);
            Para(S(StringId::BrIssNote), Fmt::T125, Fmt::T125b, D2D1::RectF(note.left + X(39), note.top + X(12), note.right - X(14), note.bottom),
                 pal_.text2, X(19));
        }
        if (vis.empty()) {
            const std::wstring empty = filter_ == 2 ? S(StringId::BrIssNone) : std::wstring();
            if (!empty.empty()) Text(empty, Fmt::T13, D2D1::RectF(view.left, view.top + X(24), view.right, view.top + X(60)),
                                     pal_.text3, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        dc_->PopAxisAlignedClip();
        if (max_scroll > 0) {
            const float vh = view.bottom - view.top;
            const float th = (std::max)(X(24), vh * vh / content);
            const float ty = view.top + (vh - th) * (rows_scroll_ / max_scroll);
            Fill(R(W_ - X(6), ty, X(3), th), X(1.5f), Alpha(pal_.text3, 0.35f));
        }
    }

    const std::pair<std::vector<rn::Seg>, std::vector<rn::Seg>>& DiffFor(size_t i) {
        auto& d = diffs_[i];
        if (!d.computed) {
            rn::Diff(sources_[i].name, plan_.rows[i].new_name, d.segs.first, d.segs.second);
            d.computed = true;
        }
        return d.segs;
    }

    // Old/new name with marked runs; whitespace-only runs show as "·".
    // CJK fonts with a real Medium face (PingFang) read much heavier than Medium Latin; the design's
    // stack falls back to YaHei, which has no 500 face. Keep CJK runs at the regular weight.
    static Fmt RegularFor(Fmt f) {
        switch (f) {
        case Fmt::T135m: return Fmt::T135;
        case Fmt::T13m: return Fmt::T13;
        case Fmt::T125m: return Fmt::T125;
        case Fmt::T12m: return Fmt::T12;
        case Fmt::T115m: return Fmt::T115;
        default: return f;
        }
    }
    static bool HasCjk(const std::wstring& s) {
        for (wchar_t c : s)
            if ((c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
                (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0xD800 && c <= 0xDBFF))
                return true;
        return false;
    }

    // The longest prefix of text that fits in width (whole code points).
    size_t FitPrefix(const std::wstring& text, Fmt f, float width) {
        size_t lo = 0, hi = text.size();   // prefix lo fits; the answer is in [lo, hi]
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            if (Measure(text.substr(0, mid), f) <= width) lo = mid;
            else hi = mid - 1;
        }
        if (lo > 0 && lo < text.size() && IS_LOW_SURROGATE(text[lo])) --lo;   // keep pairs whole
        return lo;
    }

    // A name cut to width with "…", as the design's text-overflow: ellipsis.
    std::wstring Ellipsize(const std::wstring& text, Fmt f, float width) {
        if (Measure(text, f) <= width) return text;
        const std::wstring dots = L"\x2026";
        return text.substr(0, FitPrefix(text, f, (std::max)(0.0f, width - Measure(dots, f)))) + dots;
    }

    void DrawSegs(const std::vector<rn::Seg>& segs, bool old_side, Fmt seg_fmt, const D2D1_RECT_F& r, D2D1_COLOR_F base) {
        float x = r.left;
        // Too long for the column: stop at limit and end with "…".
        float total = 0;
        for (const auto& seg : segs) {
            const bool marked = seg.kind == L'-' || seg.kind == L'+';
            total += Measure(seg.text, HasCjk(seg.text) ? RegularFor(seg_fmt) : seg_fmt) + (marked ? X(2) : 0);
        }
        const std::wstring dots = L"\x2026";
        const bool cut = total > r.right - r.left;
        const float limit = cut ? r.right - Measure(dots, seg_fmt) : r.right;
        for (const auto& seg : segs) {
            const Fmt f = HasCjk(seg.text) ? RegularFor(seg_fmt) : seg_fmt;
            if (x >= limit - X(2)) break;
            const bool marked = seg.kind == L'-' || seg.kind == L'+';
            std::wstring text = seg.text;
            if (marked && text.find_first_not_of(L" \t\x3000") == std::wstring::npos)
                text.assign(text.size(), L'\x00B7');
            bool last = false;
            if (cut && x + Measure(text, f) + (marked ? X(2) : 0) > limit) {
                text.resize(FitPrefix(text, f, limit - x - (marked ? X(2) : 0)));
                last = true;
                if (text.empty()) break;
            }
            D2D1_COLOR_F color = base;
            if (seg.kind == L'-') color = pal_.del;
            else if (seg.kind == L'+') color = pal_.ins;
            else if (seg.kind == L'~' && old_side) color = pal_.text2;
            const float w = Measure(text, f) + (marked ? X(2) : 0);
            const float room = r.right - x;
            const D2D1_RECT_F box = D2D1::RectF(x, r.top + X(8), x + (std::min)(w, room), r.bottom - X(8));
            if (marked) Fill(box, X(3), seg.kind == L'-' ? pal_.del_bg : pal_.ins_bg);
            Text(text, f, D2D1::RectF(x + (marked ? X(1) : 0), r.top, r.right, r.bottom), color);
            if (seg.kind == L'-') {
                const float my = std::round((r.top + r.bottom) * 0.5f + X(0.5f));
                Fill(D2D1::RectF(box.left + X(1), my - X(0.75f), box.right - X(1), my + X(0.75f)), 0, pal_.del);
            }
            x += w;
            if (last) break;
        }
        if (cut) Text(dots, seg_fmt, D2D1::RectF((std::min)(x, limit), r.top, r.right + X(4), r.bottom), base);
    }

    void DrawRow(size_t i, const D2D1_RECT_F& r, bool two) {
        const rn::Row& row = plan_.rows[i];
        const rn::Source& src = sources_[i];
        const bool same = row.status == rn::RowStatus::Same;
        const int ri = static_cast<int>(i);
        if (two) Fill(r, X(8), Alpha(pal_.amber_bg, 0.6f));
        else if (Hov({H::Row, ri})) Fill(r, X(7), pal_.hover);
        if (auto it = flash_.find(i); it != flash_.end()) {
            const float t = static_cast<float>(now_ - it->second) / 500.0f;
            if (t < 1.0f) { Fill(r, X(7), Alpha(pal_.accent, 0.16f * (1.0f - t))); anim_active_ = true; }
            else flash_.erase(it);
        }
        Reg(r, {H::Row, ri});
        const float line_top = r.top, line_bottom = two ? r.top + X(30) + X(3) : r.bottom;
        // Glyph.
        const std::wstring ext = ExtOf(src.name);
        const bool img = IsImageExt(ext), vid = IsVideoExt(ext);
        const D2D1_RECT_F g = R(r.left + X(6), (line_top + line_bottom) * 0.5f - X(10), X(20), X(20));
        Fill(g, X(5), img ? pal_.s_img : vid ? pal_.s_vid : pal_.s_doc);
        Ico(img ? kFtImg : vid ? kFtVid : kFtDoc, g.left + X(3.5f), g.top + X(3.5f), X(13), img ? pal_.c_img : vid ? pal_.c_vid : pal_.c_doc);

        const float cl = r.left + X(6), cr = r.right - X(8);
        const float fr = (cr - cl - X(28 + 22 + 128)) / 2.0f;
        const D2D1_RECT_F oc = D2D1::RectF(cl + X(28), line_top, cl + X(28) + fr - X(4), line_bottom);
        const D2D1_RECT_F ac = D2D1::RectF(oc.right + X(4), line_top, oc.right + X(26), line_bottom);
        const D2D1_RECT_F nc = D2D1::RectF(ac.right, line_top, cr - X(128) - X(6), line_bottom);
        const D2D1_RECT_F sc = D2D1::RectF(cr - X(128), line_top, cr, line_bottom);
        if (same || row.status == rn::RowStatus::Skipped) {
            Text(Ellipsize(src.name, Fmt::T135, oc.right - oc.left), Fmt::T135, oc, pal_.text2);
            Text(Ellipsize(row.new_name, Fmt::T135, nc.right - nc.left), Fmt::T135, nc, pal_.text3);
        } else {
            const auto& d = DiffFor(i);
            DrawSegs(d.first, true, Fmt::T135, oc, pal_.text2);
            DrawSegs(d.second, false, Fmt::T135m, nc, pal_.text);
        }
        Ico(kArrow, (ac.left + ac.right) * 0.5f - X(6), (line_top + line_bottom) * 0.5f - X(6), X(12), pal_.text3);

        // Status: icon + text, right aligned.
        std::wstring st;
        const Icon* icon = nullptr;
        D2D1_COLOR_F sc_color = pal_.amber;
        const bool applied = (executing_ || result_.executed) && i < exec_rank_.size() && exec_rank_[i] >= 0 &&
                             static_cast<size_t>(exec_rank_[i]) < exec_shown_;
        if (applied) { st = S(StringId::BrStApplied); icon = &kCheck; sc_color = pal_.ok; }
        else if (MetaPending(row.src)) { st = S(StringId::BrStReading); sc_color = pal_.text3; }
        else switch (row.status) {
        case rn::RowStatus::Same: st = S(StringId::BrTabSame); icon = &kDash; sc_color = pal_.text3; break;
        case rn::RowStatus::Conflict:
            icon = &kWarn;
            st = row.why == rn::Why::ClashCycle ? S(StringId::BrStSwap)
                 : row.added_n > 0 ? FmtS(StringId::BrStClashAdded, row.added_n) : S(StringId::BrStClash);
            break;
        case rn::RowStatus::Skipped: st = S(StringId::BrStSkipped); icon = &kDash; sc_color = pal_.text3; break;
        case rn::RowStatus::Fixed: st = S(StringId::BrStTrailing); icon = &kWarn; break;
        case rn::RowStatus::Info: st = S(StringId::BrStModDate); icon = &kInfo; break;
        case rn::RowStatus::Invalid: st = S(StringId::BrStInvalid); icon = &kWarn; sc_color = pal_.bad; break;
        case rn::RowStatus::Empty: st = S(StringId::BrStEmpty); icon = &kWarn; sc_color = pal_.bad; break;
        default: break;
        }
        if (!st.empty()) {
            const float tw = (std::min)(Measure(st, Fmt::T12), X(128 - 18));
            const float sx = sc.right - tw;
            Text(st, Fmt::T12, D2D1::RectF(sx, sc.top, sc.right + X(2), sc.bottom), sc_color);
            if (icon) Ico(*icon, sx - X(5 + 13), (sc.top + sc.bottom) * 0.5f - X(6.5f), X(13), sc_color);
        }
        if (two) {
            const std::wstring why = WhyText(i);
            if (!why.empty())
                RichLine(why, Fmt::T115, Fmt::T115m, pal_.text2, pal_.amber,
                         D2D1::RectF(oc.left, r.top + X(30), sc.right, r.top + X(30) + X(18)));
        }
    }

    // 按内容命名 row (.pr.tall): 28 | 1fr | 22 | 1.25fr | 104, 依据 underneath.
    void DrawContentRow(size_t i, const D2D1_RECT_F& r) {
        const CRow& c = crows_[i];
        const rn::Row& row = plan_.rows[i];
        const rn::Source& src = sources_[i];
        const int ri = static_cast<int>(i);
        if (Hov({H::Row, ri}) || Hov({H::CPick, ri}) || Hov({H::CName, ri})) Fill(r, X(8), pal_.hover);
        if (auto it = flash_.find(i); it != flash_.end()) {
            const float t = static_cast<float>(now_ - it->second) / 500.0f;
            if (t < 1.0f) { Fill(r, X(8), Alpha(pal_.accent, 0.16f * (1.0f - t))); anim_active_ = true; }
            else flash_.erase(it);
        }
        Reg(r, {H::Row, ri});
        const bool tall = r.bottom - r.top > X(kRowH + 1);
        const float lt = tall ? r.top + X(2) : r.top, lb = tall ? r.top + X(32) : r.bottom, cy = (lt + lb) * 0.5f;
        const std::wstring ext = ExtOf(src.name);
        const bool img = IsImageExt(ext), vid = IsVideoExt(ext);
        const D2D1_RECT_F g = R(r.left + X(6), cy - X(10), X(20), X(20));
        Fill(g, X(5), img ? pal_.s_img : vid ? pal_.s_vid : pal_.s_doc);
        Ico(img ? kFtImg : vid ? kFtVid : kFtDoc, g.left + X(3.5f), g.top + X(3.5f), X(13), img ? pal_.c_img : vid ? pal_.c_vid : pal_.c_doc);

        const float cl = r.left + X(6), cr = r.right - X(8);
        const float fr = (cr - cl - X(28 + 22 + 104)) / 2.25f;
        const D2D1_RECT_F oc = D2D1::RectF(cl + X(28), lt, cl + X(28) + fr - X(4), lb);
        const D2D1_RECT_F ac = D2D1::RectF(oc.right + X(4), lt, oc.right + X(26), lb);
        const D2D1_RECT_F nc = D2D1::RectF(ac.right, lt, cr - X(104) - X(6), lb);
        const D2D1_RECT_F sc = D2D1::RectF(cr - X(104), lt, cr, lb);
        Text(Ellipsize(src.name, Fmt::T135, oc.right - oc.left), Fmt::T135, oc, pal_.text2);
        Ico(kArrow, (ac.left + ac.right) * 0.5f - X(6), cy - X(6), X(12), pal_.text3);

        std::wstring st;
        const Icon* icon = nullptr;
        D2D1_COLOR_F scol = pal_.text3;
        bool spin = false;
        auto muted = [&](const std::wstring& t) { Text(Ellipsize(t, Fmt::T135, nc.right - nc.left), Fmt::T135, nc, pal_.text3); };
        switch (c.st) {
        case CSt::Wait: muted(S(StringId::BrCmWait)); break;
        case CSt::Read: muted(S(StringId::BrCmReading)); st = S(StringId::BrStReading); spin = true; scol = pal_.ai; break;
        case CSt::Think: muted(S(StringId::BrCmThinking)); spin = true; scol = pal_.ai; break;
        case CSt::Ok: {
            const D2D1_RECT_F cb = R(nc.left, cy - X(8), X(16), X(16));
            if (c.pick) {
                Fill(cb, X(4), pal_.accent);
                Ico(kCheck, cb.left + X(2.5f), cb.top + X(2.5f), X(11), pal_.on_accent);
            } else {
                Stroke(cb, X(4), pal_.text3, X(1.5f));
            }
            Reg(D2D1::RectF(cb.left - X(4), lt, cb.right + X(4), lb), {H::CPick, ri});
            const D2D1_RECT_F name = D2D1::RectF(cb.right + X(8), lt, nc.right, lb);
            Field* f = c_edit_ == ri ? FindField(kRowField, ri) : nullptr;
            if (f) {
                const D2D1_RECT_F box = D2D1::RectF(name.left - X(4), lt + X(1), name.right, lb - X(1));
                const bool focus = GetFocus() == f->hwnd;
                Fill(box, X(7), pal_.field);
                Stroke(box, X(7), focus ? pal_.accent : pal_.field_line);
                if (focus) Fill(D2D1::RectF(box.left + X(1), box.bottom - X(2.5f), box.right - X(1), box.bottom - X(1)), 0, pal_.accent);
                f->want = box;
                f->want_visible = box.top >= rows_view_.top && box.bottom <= rows_view_.bottom;
            } else {
                const std::wstring nn = c.pick ? row.new_name : c.name + ext;
                const Fmt nf = HasCjk(nn) ? Fmt::T135 : Fmt::T135m;
                Text(Ellipsize(nn, nf, name.right - name.left), nf, name, c.pick ? pal_.text : pal_.text3);
                Reg(name, {H::CName, ri});
            }
            if (c.pick && row.status == rn::RowStatus::Conflict) {
                icon = &kWarn;
                scol = pal_.amber;
                st = row.why == rn::Why::ClashCycle ? S(StringId::BrStSwap)
                     : row.added_n > 0 ? FmtS(StringId::BrStClashAdded, row.added_n) : S(StringId::BrStClash);
            } else if (c.pick && (row.status == rn::RowStatus::Invalid || row.status == rn::RowStatus::Empty)) {
                icon = &kWarn;
                scol = pal_.bad;
                st = S(StringId::BrStInvalid);
            } else {
                icon = &kSpark;
                scol = pal_.ai;
                st = S(StringId::BrCmAiSuggest);
            }
            break;
        }
        case CSt::Keep: muted(src.name); icon = &kDash; st = S(StringId::BrCmKeep); break;
        case CSt::Skip: muted(src.name); icon = &kDash; st = S(StringId::BrCmSkipped); break;
        case CSt::Fail: muted(src.name); icon = &kWarn; scol = pal_.amber; st = S(StringId::BrCmFailed); break;
        }
        const bool applied = (executing_ || result_.executed) && i < exec_rank_.size() && exec_rank_[i] >= 0 &&
                             static_cast<size_t>(exec_rank_[i]) < exec_shown_;
        if (applied) { st = S(StringId::BrStApplied); icon = &kCheck; scol = pal_.ok; spin = false; }
        float sx = sc.right;
        if (!st.empty()) {
            const float tw = (std::min)(Measure(st, Fmt::T12), X(104 - 18));
            sx = sc.right - tw;
            Text(st, Fmt::T12, D2D1::RectF(sx, sc.top, sc.right + X(2), sc.bottom), scol);
        }
        if (spin) Spinner(sx - X(5 + 6.5f), cy, X(13), pal_.ai);
        else if (icon) Ico(*icon, sx - X(5 + 13), cy - X(6.5f), X(13), scol);

        // 依据：the words the name rests on (quoted), or why the name stays.
        if (tall && !c.basis.empty()) {
            const D2D1_RECT_F br = D2D1::RectF(oc.left, r.top + X(32), sc.right, r.top + X(50));
            const float lw = Text(S(StringId::BrCmBasis), Fmt::T115, br, pal_.text3);
            const bool en = pulse::l10n::effective_language() == pulse::l10n::Language::EnUS;
            const std::wstring q = c.st == CSt::Ok ? (en ? L"\u201C" + c.basis + L"\u201D" : L"\u300C" + c.basis + L"\u300D") : c.basis;
            Text(Ellipsize(q, Fmt::T115, br.right - br.left - lw - X(4)), Fmt::T115,
                 D2D1::RectF(br.left + lw + X(2), br.top, br.right, br.bottom), c.st == CSt::Fail ? pal_.amber : pal_.text2);
        }
    }

    std::wstring WhyText(size_t i) const {
        const rn::Row& row = plan_.rows[i];
        switch (row.why) {
        case rn::Why::ClashExisting: return FmtS(StringId::BrWhyExisting, row.why_arg.c_str());
        case rn::Why::ClashBatch: return FmtS(StringId::BrWhyBatch, row.why_arg.c_str());
        case rn::Why::ClashCycle: return S(StringId::BrWhySwap);
        case rn::Why::TrailingFixed: return S(StringId::BrWhyTrailing);
        case rn::Why::NoTakenDate: {
            SYSTEMTIME st{};
            FILETIME ft{static_cast<DWORD>(sources_[i].modified & 0xFFFFFFFF), static_cast<DWORD>(sources_[i].modified >> 32)};
            FileTimeToSystemTime(&ft, &st);
            wchar_t date[32]{};
            swprintf_s(date, L"%04d-%02d-%02d", static_cast<int>(st.wYear), static_cast<int>(st.wMonth),
                       static_cast<int>(st.wDay));
            return FmtS(StringId::BrWhyNoTaken, date);
        }
        case rn::Why::BadChars: return S(StringId::BrWhyBad);
        case rn::Why::EmptyName: return S(StringId::BrWhyEmpty);
        default: return {};
        }
    }

    // ---------------------------------------------------------------- footer

    void DrawFooter() {
        const float top = H_ - X(kFootH);
        if (pal_.glass_sheet.a > 0) Fill(D2D1::RectF(0, top, W_, H_), 0, pal_.glass_sheet);
        Fill(D2D1::RectF(0, top, W_, top + X(1)), 0, pal_.line2);
        if (executing_ || result_.executed) {
            // .prog: 2 px accent line on the footer's top edge, width .2 s linear.
            const float total = static_cast<float>((std::max<size_t>)(plan_.exec_order.size(), 1));
            const float k = Animate(9ull << 40, static_cast<float>(exec_shown_) / total, 200.0f);
            if (k > 0) Fill(D2D1::RectF(0, top - X(1), W_ * k, top + X(1)), 0, pal_.accent);
        }
        const float cy = top + X(kFootH) * 0.5f;
        if (!options_.proposed_names) {
        // 同时修改扩展名 (rules only: suggestions keep the extension).
        float x = X(18);
        if (!content_mode_) {
        const D2D1_RECT_F tg = R(x, cy - X(9), X(34), X(18));
        const float k = Animate(3ull << 40, opts_.ext_too ? 1.0f : 0.0f, 200.0f);
        if (k > 0) Fill(tg, X(9), Alpha(pal_.accent, k));
        if (k < 1) Stroke(tg, X(9), Alpha(pal_.text3, 1.0f - k), X(1.5f));
        Fill(R(tg.left + X(3) + X(16) * k, tg.top + X(3), X(12), X(12)), X(6), Mix(pal_.text3, Hex(0xffffff), k));
        const std::wstring ext = S(StringId::BrExtToo);
        const float ew = Text(ext, Fmt::T13, R(tg.right + X(8), top, X(200), X(kFootH)), pal_.text2);
        Reg(D2D1::RectF(tg.left - X(4), top + X(14), tg.right + X(8) + ew, top + X(46)), {H::ExtToggle});
        x = tg.right + X(8) + ew + X(14);
        Fill(R(x, cy - X(11), X(1), X(22)), 0, pal_.line);
        x += X(15);
        }
        // 重名时 [policy].
        x += Text(S(StringId::BrOnClash), Fmt::T13, R(x, top, X(120), X(kFootH)), pal_.text2) + X(8);
        static const StringId pols[] = {StringId::BrPolAuto, StringId::BrPolSkip, StringId::BrPolBlock};
        const std::wstring pv = S(pols[static_cast<int>(opts_.policy)]);
        const float pw = Measure(pv, Fmt::T13) + X(8 + 4 + 12 + 8);
        const D2D1_RECT_F pd = R(x, cy - X(14), pw, X(28));
        Fill(pd, X(7), Hov({H::PolicyDD}) || menu_ == Menu::Policy ? pal_.hover : pal_.card);
        Stroke(pd, X(7), pal_.line);
        Text(pv, Fmt::T13, D2D1::RectF(pd.left + X(8), pd.top, pd.right, pd.bottom), pal_.text);
        Ico(kChev, pd.right - X(8 + 12), pd.top + X(8), X(12), pal_.text3);
        Reg(pd, {H::PolicyDD});
        policy_dd_ = pd;

        }
        // Buttons, right to left.
        const bool can = CanApply();
        const std::wstring go = FmtS(StringId::BrRenameN, plan_.renames);
        const float gw = X(16) + Measure(go, Fmt::T135b) + X(6) + Measure(L"Enter", Fmt::T11) + X(16);
        const D2D1_RECT_F apply = R(W_ - X(16) - gw, cy - X(17), gw, X(34));
        Fill(apply, X(8), can ? (Hov({H::Apply}) ? pal_.accent_h : pal_.accent) : pal_.hover);
        const D2D1_COLOR_F gc = can ? pal_.on_accent : pal_.text3;
        const float gx = apply.left + X(16);
        const float gtw = Text(go, Fmt::T135b, D2D1::RectF(gx, apply.top, apply.right, apply.bottom), gc);
        Text(L"Enter", Fmt::T11, D2D1::RectF(gx + gtw + X(6), apply.top + X(1), apply.right, apply.bottom), Alpha(gc, 0.75f));
        Reg(apply, {H::Apply});
        const std::wstring cancel = S(StringId::Cancel);
        const float cw = Measure(cancel, Fmt::T135) + X(32);
        const D2D1_RECT_F cb = R(apply.left - X(14) - cw, cy - X(17), cw, X(34));
        Fill(cb, X(8), Hov({H::Cancel}) ? pal_.hover : pal_.card);
        Stroke(cb, X(8), pal_.line);
        Text(cancel, Fmt::T135, cb, pal_.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        Reg(cb, {H::Cancel});

        // Summary, right aligned before the buttons.
        std::vector<std::pair<std::wstring, D2D1_COLOR_F>> parts;
        if (content_mode_) {
            int readable = 0, read = 0, skipped = 0, kept = 0;
            for (size_t i = 0; i < crows_.size(); ++i) {
                const CSt cst = crows_[i].st;
                if (Readable(i)) {
                    ++readable;
                    if (cst != CSt::Wait && cst != CSt::Read) ++read;
                }
                if (cst == CSt::Skip || cst == CSt::Fail) ++skipped;
                if (cst == CSt::Keep) ++kept;
            }
            if (ContentRunning()) {
                if (c_job_) parts.push_back({FmtS(StringId::BrCmReadN, read, readable), pal_.text2});
                else parts.push_back({S(StringId::BrCmThinking), pal_.text2});
            } else if (!c_ran_) {
                parts.push_back({S(StringId::BrCmFootHint), pal_.text3});
            } else {
                parts.push_back({FmtS(StringId::BrCmSumChecked, plan_.renames), pal_.text2});
                if (plan_.auto_fixed && opts_.policy == rn::Policy::AutoNumber)
                    parts.push_back({FmtS(StringId::BrSumAuto, plan_.auto_fixed), pal_.amber});
                parts.push_back({FmtS(StringId::BrSumSkip, skipped), pal_.text2});
                parts.push_back({FmtS(StringId::BrCmSumKeep, kept), pal_.text2});
            }
        } else if (meta_job_) {
            parts.push_back({FmtS(StringId::BrSumReading, meta_job_->done.load(),
                                  static_cast<int>(meta_job_->paths.size())), pal_.text2});
        } else {
        parts.push_back({FmtS(StringId::BrSumRename, plan_.renames), pal_.text2});
        int clashes = 0, skipped = 0;
        for (const auto& r : plan_.rows) {
            if (r.status == rn::RowStatus::Conflict) ++clashes;
            if (r.status == rn::RowStatus::Skipped) ++skipped;
        }
        if (opts_.policy == rn::Policy::AutoNumber && plan_.auto_fixed)
            parts.push_back({FmtS(StringId::BrSumAuto, plan_.auto_fixed), pal_.amber});
        else if (opts_.policy == rn::Policy::Skip && skipped)
            parts.push_back({FmtS(StringId::BrSumSkip, skipped), pal_.amber});
        else if (opts_.policy == rn::Policy::Block && clashes)
            parts.push_back({FmtS(StringId::BrSumClash, clashes), pal_.amber});
        const int unchanged = plan_.same;
        if (unchanged) parts.push_back({FmtS(StringId::BrSumSame, unchanged), pal_.text2});
        }
        const std::wstring sep = L" \x00B7 ";
        float total = 0;
        for (size_t p = 0; p < parts.size(); ++p)
            total += RichLine(parts[p].first, Fmt::T125, Fmt::T125b, pal_.text2, pal_.text, D2D1::RectF(0, 0, 1e6f, 1), true) +
                     (p ? Measure(sep, Fmt::T125) : 0);
        float sx = (std::max)(policy_dd_.right + X(14), cb.left - X(14) - total);
        for (size_t p = 0; p < parts.size(); ++p) {
            if (p) sx += Text(sep, Fmt::T125, D2D1::RectF(sx, top, cb.left, top + X(kFootH)), pal_.text3);
            sx += RichLine(parts[p].first, Fmt::T125, Fmt::T125b, parts[p].second, pal_.text,
                           D2D1::RectF(sx, top, cb.left - X(10), top + X(kFootH)));
        }
    }

    // ---------------------------------------------------------------- menus

    void MenuFrame(const D2D1_RECT_F& r) {
        Shadow(r, X(10), X(28), X(10), Hex(0x000000, dark_ ? 0.5f : 0.18f));
        Fill(r, X(10), pal_.card);
        Stroke(r, X(10), pal_.line);
        Reg(r, {H::Menu});
        menu_box_ = r;
    }

    void DrawMenus() {
        if (menu_ == Menu::None) { menu_box_ = {}; return; }
        const float t = Animate(4ull << 40 | static_cast<uint64_t>(menu_), 1.0f, 200.0f);
        const float saved = fade_;
        fade_ = t;
        D2D1_MATRIX_3X2_F m;
        dc_->GetTransform(&m);
        if (menu_ == Menu::AddStep) {
            const int n = static_cast<int>(std::size(kStepOrder));
            const float mh = X(10) + X(34) * static_cast<float>(n);
            // From the compact quick-add bar the menu opens upward, left-aligned to the bar.
            D2D1_RECT_F r = R(add_link_.right - X(190), add_link_.bottom + X(4), X(190), mh);
            bool up = false;
            if (menu_from_box_) {
                up = qa_box_.top - X(4) - mh >= X(kBodyTop);
                r = up ? R(qa_box_.left, qa_box_.top - X(4) - mh, X(190), mh) : R(qa_box_.left, qa_box_.bottom + X(4), X(190), mh);
            }
            const float dir = up ? 1.0f : -1.0f;
            dc_->SetTransform(D2D1::Matrix3x2F::Translation(0, dir * X(6) * (1 - t)) * m);
            MenuFrame(r);
            for (int i = 0; i < n; ++i) {
                const int type = static_cast<int>(kStepOrder[i]);
                const D2D1_RECT_F it = R(r.left + X(5), r.top + X(5) + X(34) * static_cast<float>(i), r.right - r.left - X(10), X(34));
                if (Hov({H::AddItem, type})) Fill(it, X(7), pal_.hover);
                Ico(StepIcon(kStepOrder[i]), it.left + X(10), it.top + X(9), X(16), pal_.text2);
                Text(S(StepTitle(kStepOrder[i])), Fmt::T13, D2D1::RectF(it.left + X(36), it.top, it.right, it.bottom), pal_.text);
                Reg(it, {H::AddItem, type});
            }
        } else if (menu_ == Menu::Order) {
            static const StringId l[] = {StringId::BrOrderList, StringId::BrOrderTaken, StringId::BrOrderModified,
                                         StringId::BrOrderName};
            // The four orders, a rule, then 每天从头编号 (a check of its own).
            const D2D1_RECT_F r = R(order_dd_.right - X(200), order_dd_.bottom + X(4), X(200), X(10 + 5 * 34 + 9));
            dc_->SetTransform(D2D1::Matrix3x2F::Translation(0, -X(6) * (1 - t)) * m);
            MenuFrame(r);
            for (int i = 0; i < 4; ++i) {
                const D2D1_RECT_F it = R(r.left + X(5), r.top + X(5) + X(34) * static_cast<float>(i), r.right - r.left - X(10), X(34));
                const bool on = static_cast<int>(opts_.order) == i;
                if (Hov({H::OrderItem, i}) || on) Fill(it, X(7), pal_.hover);
                if (on) Ico(kCheck, it.left + X(10), it.top + X(10), X(14), pal_.accent);
                Text(S(l[i]), Fmt::T13, D2D1::RectF(it.left + X(34), it.top, it.right, it.bottom), pal_.text);
                Reg(it, {H::OrderItem, i});
            }
            const float rule_y = r.top + X(5 + 4 * 34 + 4);
            Fill(D2D1::RectF(r.left + X(10), rule_y, r.right - X(10), rule_y + X(1)), 0, pal_.line2);
            const D2D1_RECT_F daily = R(r.left + X(5), rule_y + X(5), r.right - r.left - X(10), X(34));
            if (Hov({H::OrderDaily})) Fill(daily, X(7), pal_.hover);
            if (opts_.daily) Ico(kCheck, daily.left + X(10), daily.top + X(10), X(14), pal_.accent);
            Text(S(StringId::BrOrderDaily), Fmt::T13, D2D1::RectF(daily.left + X(34), daily.top, daily.right, daily.bottom),
                 pal_.text);
            Reg(daily, {H::OrderDaily});
        } else if (menu_ == Menu::Preset) {
            // Opens upward from the dropdown: 上次使用, 常用 (built in), 我的预设.
            struct Entry { int kind; int index; std::wstring name; bool header; };
            std::vector<Entry> list;
            if (store_.has_last) list.push_back({0, 0, S(StringId::BrPresetLast), false});
            list.push_back({-1, 0, S(StringId::BrPresetCommon), true});
            const auto builtins = Builtins();
            for (size_t i = 0; i < builtins.size(); ++i) list.push_back({1, static_cast<int>(i), builtins[i].name, false});
            if (!store_.presets.empty()) {
                list.push_back({-1, 0, S(StringId::BrPresetMine), true});
                for (size_t i = 0; i < store_.presets.size(); ++i)
                    list.push_back({2, static_cast<int>(i), store_.presets[i].name, false});
            }
            float mh = X(10);
            for (const Entry& e : list) mh += e.header ? X(26) : X(34);
            const float room = preset_dd_.top - X(4) - X(kBodyTop) - X(8);
            mh = (std::min)(mh, room);
            const D2D1_RECT_F r = R(preset_dd_.left, preset_dd_.top - X(4) - mh, X(280), mh);
            dc_->SetTransform(D2D1::Matrix3x2F::Translation(0, X(6) * (1 - t)) * m);
            MenuFrame(r);
            dc_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
            float y = r.top + X(5);
            for (const Entry& e : list) {
                if (e.header) {
                    Text(e.name, Fmt::T115, R(r.left + X(15), y + X(6), r.right - r.left - X(30), X(20)), pal_.text3);
                    y += X(26);
                    continue;
                }
                const D2D1_RECT_F it = R(r.left + X(5), y, r.right - r.left - X(10), X(34));
                const Hit hit{H::PresetItem, e.index, e.kind};
                const bool on = e.name == preset_name_;
                const bool hov = Hov(hit) || (e.kind == 2 && Hov({H::PresetDel, e.index}));
                if (hov || on) Fill(it, X(7), pal_.hover);
                if (on) Ico(kCheck, it.left + X(10), it.top + X(10), X(14), pal_.accent);
                Text(e.name, Fmt::T13, D2D1::RectF(it.left + X(34), it.top, it.right - X(30), it.bottom), pal_.text);
                Reg(it, hit);
                if (e.kind == 2) {   // × shows on hover; its spot always deletes
                    const D2D1_RECT_F del = R(it.right - X(30), it.top + X(5), X(24), X(24));
                    if (Hov({H::PresetDel, e.index})) Fill(del, X(6), Alpha(pal_.text, 0.08f));
                    if (hov) Ico(kX, del.left + X(6), del.top + X(6), X(12), pal_.text2);
                    Reg(del, {H::PresetDel, e.index});
                }
                y += X(34);
            }
            dc_->PopAxisAlignedClip();
        } else if (menu_ == Menu::Policy) {
            static const StringId l[] = {StringId::BrPolAuto, StringId::BrPolSkip, StringId::BrPolBlock};
            static const StringId d[] = {StringId::BrPolAutoD, StringId::BrPolSkipD, StringId::BrPolBlockD};
            const float ih = X(52);
            const D2D1_RECT_F r = R(policy_dd_.left - X(50), policy_dd_.top - X(4) - (X(10) + ih * 3), X(300), X(10) + ih * 3);
            dc_->SetTransform(D2D1::Matrix3x2F::Translation(0, X(6) * (1 - t)) * m);
            MenuFrame(r);
            for (int i = 0; i < 3; ++i) {
                const D2D1_RECT_F it = R(r.left + X(5), r.top + X(5) + ih * static_cast<float>(i), r.right - r.left - X(10), ih);
                const bool on = static_cast<int>(opts_.policy) == i;
                if (Hov({H::PolicyItem, i}) || on) Fill(it, X(7), pal_.hover);
                if (on) Ico(kCheck, it.left + X(10), it.top + X(11), X(14), pal_.accent);
                Text(S(l[i]), Fmt::T13m, R(it.left + X(34), it.top + X(8), it.right - it.left - X(44), X(19)), pal_.text);
                Text(S(d[i]), Fmt::T12, R(it.left + X(34), it.top + X(27), it.right - it.left - X(44), X(17)), pal_.text3);
                Reg(it, {H::PolicyItem, i});
            }
        }
        dc_->SetTransform(m);
        fade_ = saved;
    }

    // ---------------------------------------------------------------- input

    Hit HitAt(float x, float y) const {
        // Menus sit on top: inside one, only its items count.
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            if (it->h.k == H::Menu && In(it->r, x, y)) {
                for (auto jt = hits_.rbegin(); jt != it; ++jt)
                    if (In(jt->r, x, y)) return jt->h;
                return it->h;
            }
        }
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            if (it->h.k == H::Menu) continue;
            if (In(it->r, x, y)) return it->h;
        }
        return {};
    }

    void Click(const Hit& h) {
        if (executing_) return;
        if (options_.proposed_names && h.k != H::Close && h.k != H::Cancel && h.k != H::Apply &&
            h.k != H::Tab && h.k != H::Preview && h.k != H::Row) return;
        if (c_edit_ >= 0 && !(h.k == H::CName && h.a == c_edit_)) CommitRowEdit(true);
        const bool had_menu = menu_ != Menu::None;
        const Menu was = menu_;
        if (h.k != H::Menu) menu_ = Menu::None;
        switch (h.k) {
        case H::Close: case H::Cancel: Complete(false); return;
        case H::PresetDD: menu_ = was == Menu::Preset ? Menu::None : Menu::Preset; break;
        case H::PresetItem: {
            if (h.b == 0 && store_.has_last) ApplyPreset(store_.last, true);
            else if (h.b == 1) {
                const auto builtins = Builtins();
                if (h.a >= 0 && h.a < static_cast<int>(builtins.size())) ApplyPreset(builtins[static_cast<size_t>(h.a)], true);
            } else if (h.b == 2 && h.a >= 0 && h.a < static_cast<int>(store_.presets.size()))
                ApplyPreset(store_.presets[static_cast<size_t>(h.a)], true);
            Recompute(true);
            break;
        }
        case H::PresetDel:
            if (h.a >= 0 && h.a < static_cast<int>(store_.presets.size())) {
                if (store_.presets[static_cast<size_t>(h.a)].name == preset_name_) preset_name_.clear();
                store_.presets.erase(store_.presets.begin() + h.a);
                SaveStore();
            }
            menu_ = Menu::Preset;   // keep the list open
            break;
        case H::PresetSave: StartNaming(); break;
        case H::PresetOk: CommitPresetName(); break;
        case H::PresetCancel: EndNaming(); break;
        case H::Apply: Complete(true); return;
        case H::ModeRules: LeaveContentMode(); break;
        case H::ModeContent: EnterContentMode(); break;
        case H::CFormat: c_format_ = h.a; break;
        case H::CLang: c_lang_ = h.a; break;
        case H::CRange: c_range_ = h.a; break;
        case H::CStart:
            if (ContentRunning()) { StopContent(); Recompute(false); }
            else StartContent();
            break;
        case H::CRegen: StartContent(); break;
        case H::CPick:
            if (h.a >= 0 && static_cast<size_t>(h.a) < crows_.size()) {
                crows_[static_cast<size_t>(h.a)].pick = !crows_[static_cast<size_t>(h.a)].pick;
                Recompute(false);
            }
            break;
        case H::CName: if (h.a >= 0) BeginRowEdit(static_cast<size_t>(h.a)); break;
        case H::Example: {
            static const StringId ex[] = {StringId::BrEx1, StringId::BrEx2, StringId::BrEx3, StringId::BrEx4, StringId::BrEx5};
            if (Field* f = FindField(-1, 0)) {
                if (ai_task_) break;
                ai_text_ = S(ex[h.a]);
                // Full width first, or the edit scrolls to the caret while narrow.
                f->want_visible = true;
                PlaceFields();
                SetWindowTextW(f->hwnd, ai_text_.c_str());
                SetFocus(f->hwnd);
                SendMessageW(f->hwnd, EM_SETSEL, ai_text_.size(), ai_text_.size());
                SendAi();   // an example is a whole request: send it
            }
            break;
        }
        case H::AiSend: SendAi(); break;
        case H::AiInput:
            if (Field* f = FindField(-1, 0)) { f->want_visible = true; PlaceFields(); SetFocus(f->hwnd); }
            break;
        case H::AddStep:
            menu_from_box_ = h.b == 1;
            menu_ = was == Menu::AddStep ? Menu::None : Menu::AddStep;
            break;
        case H::AddItem: {
            AddStep(static_cast<rn::StepType>(h.a), true);
            focus_step_ = steps_ui_.back().id;
            reveal_step_ = focus_step_;
            if (const rn::StepType t = steps_.back().type;
                t == rn::StepType::Replace || t == rn::StepType::Insert || t == rn::StepType::Template)
                pending_focus_ = {focus_step_, 0};
            else
                SetFocus(hwnd_);   // no text field: don't leave the caret in another card
            Recompute(true);

            break;
        }
        case H::Toggle: steps_[static_cast<size_t>(h.a)].on = !steps_[static_cast<size_t>(h.a)].on; Recompute(true); break;
        case H::Delete: DeleteStep(h.a); break;
        case H::Card: case H::Grip: focus_step_ = steps_ui_[static_cast<size_t>(h.a)].id; break;
        case H::Field:
            if (Field* f = FindField(steps_ui_[static_cast<size_t>(h.a)].id, h.b)) {
                f->want_visible = true;
                PlaceFields();
                SetFocus(f->hwnd);
            }
            break;
        case H::Chip: ToggleChip(h.a, h.b); break;
        case H::Seg: {
            rn::Step& st = steps_[static_cast<size_t>(h.a)];
            if (h.b >= kSegWhere) st.where = static_cast<rn::Where>(h.b - kSegWhere);
            else st.mode = static_cast<rn::CaseMode>(h.b);
            focus_step_ = steps_ui_[static_cast<size_t>(h.a)].id;
            Recompute(true);
            reveal_step_ = focus_step_;
            if (st.type == rn::StepType::Insert && st.where == rn::Where::Index)
                pending_focus_ = {steps_ui_[static_cast<size_t>(h.a)].id, kAt};
            if (st.type == rn::StepType::Ext && st.mode == rn::CaseMode::Change)
                pending_focus_ = {steps_ui_[static_cast<size_t>(h.a)].id, kExtTo};
            break;
        }
        case H::Token: InsertToken(h.a, h.b); break;
        case H::Tab: filter_ = h.a; rows_scroll_ = 0; break;
        case H::OrderDD: menu_ = was == Menu::Order ? Menu::None : Menu::Order; break;
        case H::OrderItem: opts_.order = static_cast<rn::Order>(h.a); Recompute(true); break;
        case H::OrderDaily: opts_.daily = !opts_.daily; Recompute(true); break;
        case H::PolicyDD: menu_ = was == Menu::Policy ? Menu::None : Menu::Policy; break;
        case H::PolicyItem: opts_.policy = static_cast<rn::Policy>(h.a); Recompute(true); break;
        case H::ExtToggle: opts_.ext_too = !opts_.ext_too; Recompute(true); break;
        default: break;
        }
        if (had_menu && menu_ == Menu::None) anims_.erase(4ull << 40 | static_cast<uint64_t>(was));
        if (menu_ != Menu::None && menu_ != was) anims_[4ull << 40 | static_cast<uint64_t>(menu_)] = {0.0f, 1.0f, GetTickCount64(), 200.0f};
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void ToggleChip(int si, int chip) {
        rn::Step& st = steps_[static_cast<size_t>(si)];
        StepUi& ui = steps_ui_[static_cast<size_t>(si)];
        focus_step_ = ui.id;
        reveal_step_ = ui.id;
        if (st.type == rn::StepType::Replace) {
            if (chip == 0) st.regex = !st.regex;
            else if (chip == 1) st.match_case = !st.match_case;
            else st.all = !st.all;
        } else if (st.type == rn::StepType::Remove) {
            if (chip == 0) st.brackets = !st.brackets;
            else if (chip == 1) st.spaces = !st.spaces;
            else if (chip == 2) st.digits = !st.digits;
            else {
                ui.custom_on = !ui.custom_on;
                st.custom = ui.custom_on ? ui.custom_text : std::wstring();
                if (ui.custom_on) pending_focus_ = {ui.id, kCustom};
            }
        }
        Recompute(true);
    }

    void InsertToken(int si, int token) {
        Field* f = FindField(steps_ui_[static_cast<size_t>(si)].id, kPattern);
        if (!f) return;
        focus_step_ = f->step_id;
        const std::wstring text = TokenText(token);
        if (GetFocus() != f->hwnd) {
            const int n = GetWindowTextLengthW(f->hwnd);
            SendMessageW(f->hwnd, EM_SETSEL, n, n);
        }
        SendMessageW(f->hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()));
        SetFocus(f->hwnd);
    }

    void ApplyPendingFocus() {
        if (naming_focus_) {
            if (Field* f = FindField(kPresetField, 0); f && f->want_visible) {
                SetFocus(f->hwnd);
                SendMessageW(f->hwnd, EM_SETSEL, 0, -1);
                naming_focus_ = false;
            }
        }
        if (pending_focus_.first < 0) return;
        if (Field* f = FindField(pending_focus_.first, pending_focus_.second); f && f->want_visible) {
            SetFocus(f->hwnd);
            pending_focus_ = {-1, 0};
        }
    }

    void DragTo(float y) {
        drag_y_ = y;
        const size_t n = steps_.size();
        const size_t cur = static_cast<size_t>(drag_index_);
        // Card centre of the dragged card vs the slots of the others.
        const float centre = y - drag_grab_ + X(StepH(cur)) * 0.5f;
        // Target slot = number of other cards whose middle is above the centre.
        size_t target = 0;
        float slot_y = steps_view_.top + X(2) - steps_scroll_;
        for (size_t i = 0; i < n; ++i) {
            if (i == cur) continue;
            const float h = X(StepH(i));
            if (centre > slot_y + h * 0.5f) ++target;
            slot_y += h + X(8);
        }
        if (target != cur) {
            rn::Step st = steps_[cur];
            StepUi ui = steps_ui_[cur];
            steps_.erase(steps_.begin() + static_cast<std::ptrdiff_t>(cur));
            steps_ui_.erase(steps_ui_.begin() + static_cast<std::ptrdiff_t>(cur));
            steps_.insert(steps_.begin() + static_cast<std::ptrdiff_t>(target), st);
            steps_ui_.insert(steps_ui_.begin() + static_cast<std::ptrdiff_t>(target), ui);
            drag_index_ = static_cast<int>(target);
            Recompute(true);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE: {
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
#if defined(PULSE_TEST_BATCH_RENAME_PRESENT)
            scale_ = BatchRenameScaleForTest();
#endif
            glass_ = pulse::ui::ApplyBackdrop(hwnd_, dark_) && pulse::ui::IsAcrylicBackdrop(hwnd_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            CreateFormats();
            pal_ = MakePal(dark_, accent_);
            GlassPal();
            font_ = typography::CreateEditFont(scale_, 13.0f);
            font_ai_ = typography::CreateEditFont(scale_, 14.0f);
            brush_field_ = CreateSolidBrush(Ref(pal_.field));
            brush_ai_ = CreateSolidBrush(Ref(pal_.card));
            EnsureFields();
            return 0;
        }
        case WM_NCCALCSIZE:
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lparam);
            mm->ptMinTrackSize.x = static_cast<LONG>(X(820));
            mm->ptMinTrackSize.y = static_cast<LONG>(X(520));
            return 0;
        }
        case WM_NCHITTEST: {
            const LRESULT hit = pulse::ui::BorderlessHitTest(hwnd_, lparam, X(kTitleH), D2D1::RectF(0, 0, 0, 0));
            if (hit == HTCAPTION) {
                POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(hwnd_, &pt);
                const Hit h = HitAt(static_cast<float>(pt.x), static_cast<float>(pt.y));
                if (h.k != H::None) return HTCLIENT;
            }
            return hit;
        }
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case kMetaMsg:
            OnMeta(wparam != 0);
            return 0;
        case kAiMsg:
            OnAiDone();
            return 0;
        case kContentMsg:
            OnContentMsg(wparam, lparam);
            return 0;
        case WM_DPICHANGED: {
            // Moved to a monitor with another scale: rebuild text and edit
            // fonts, then take the size Windows suggests (same DIP size).
#if !defined(PULSE_TEST_BATCH_RENAME_PRESENT)
            scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            CreateFormats();
            HFONT old_font = font_, old_ai = font_ai_;
            font_ = typography::CreateEditFont(scale_, 13.0f);
            font_ai_ = typography::CreateEditFont(scale_, 14.0f);
            for (const auto& f : fields_)
                SendMessageW(f.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(f.step_id == kAiField ? font_ai_ : font_), FALSE);
            if (old_font) DeleteObject(old_font);
            if (old_ai) DeleteObject(old_ai);
            const auto* rc = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd_, nullptr, FALSE);
#endif
            return 0;
        }
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_CTLCOLOREDIT: {
            const HDC hdc = reinterpret_cast<HDC>(wparam);
            Field* f = FieldOf(reinterpret_cast<HWND>(lparam));
            const bool ai = f && f->step_id == kAiField;
            SetTextColor(hdc, Ref(pal_.text));
            SetBkColor(hdc, Ref(ai ? pal_.card : pal_.field));
            return reinterpret_cast<LRESULT>(ai ? brush_ai_ : brush_field_);
        }
        case kReviewReadyMsg: {
            auto callback = std::move(options_.on_review_ready);
            const HWND window = hwnd_;
            const std::weak_ptr<uint8_t> lifetime = review_lifetime_;
            if (callback) callback(window, [this, window, lifetime](const std::wstring& path) {
                if (lifetime.expired() || !IsWindow(window)) return false;
                // DWM acrylic is external to the backbuffer. Use the same real
                // opaque-theme capture pass as other native dialogs, then restore.
                const bool saved = CaptureDialogSurface(path, [this] { Render(); });
                if (!lifetime.expired() && IsWindow(window)) Render();
                return saved;
            });
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wparam) == EN_CHANGE) OnFieldChanged(reinterpret_cast<HWND>(lparam));
            return 0;
        case WM_TIMER:
            if (options_.review_valid && !options_.review_valid()) { Complete(false); return 0; }
            if (wparam == kAnimTimer) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd_, &pt);
            const float delta = -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * X(kRowH * 3);
            if (In(steps_view_, static_cast<float>(pt.x), static_cast<float>(pt.y))) (content_mode_ ? c_scroll_ : steps_scroll_) += delta;
            else { CommitRowEdit(true); rows_scroll_ += delta; }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const float x = static_cast<float>(GET_X_LPARAM(lparam)), y = static_cast<float>(GET_Y_LPARAM(lparam));
            if (drag_index_ >= 0) { DragTo(y); return 0; }
            if (press_.k == H::Grip && std::abs(y - press_y_) > X(4)) {
                drag_index_ = press_.a;
                const float card_top = drag_card_top_;
                drag_grab_ = press_y_ - card_top;
                DragTo(y);
                return 0;
            }
            const Hit next = HitAt(x, y);
            if (next != hover_) { hover_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
            const bool hand = next.k != H::None && next.k != H::StepsArea && next.k != H::Preview && next.k != H::Menu &&
                              next.k != H::Card && next.k != H::Row && next.k != H::AiInput && next.k != H::Field;
            SetCursor(LoadCursorW(nullptr, next.k == H::Grip ? IDC_SIZEALL : hand ? IDC_HAND : IDC_ARROW));
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_SETCURSOR:
            // WM_MOUSEMOVE sets our cursor; child EDITs keep their I-beam.
            if (reinterpret_cast<HWND>(wparam) == hwnd_ && LOWORD(lparam) == HTCLIENT) return TRUE;
            break;
        case WM_MOUSELEAVE:
            if (hover_.k != H::None) { hover_ = {}; InvalidateRect(hwnd_, nullptr, FALSE); }
            return 0;
        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(GET_X_LPARAM(lparam)), y = static_cast<float>(GET_Y_LPARAM(lparam));
            press_ = executing_ ? Hit{} : HitAt(x, y);
            press_y_ = y;
            if (press_.k == H::Grip) {
                // Remember where the card started for the grab offset.
                drag_card_top_ = y;
                for (const auto& hr : hits_)
                    if (hr.h.k == H::Card && hr.h.a == press_.a) drag_card_top_ = hr.r.top;
            }
            if (press_.k != H::Field && press_.k != H::AiInput && GetFocus() != hwnd_) {
                Field* f = FieldOf(GetFocus());
                if (!f) SetFocus(hwnd_);
            }
            SetCapture(hwnd_);
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const float x = static_cast<float>(GET_X_LPARAM(lparam)), y = static_cast<float>(GET_Y_LPARAM(lparam));
            if (drag_index_ >= 0) {
                drag_index_ = -1;
                press_ = {};
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            const Hit h = HitAt(x, y);
            const Hit pressed = press_;
            press_ = {};
            if (h == pressed) Click(h);
            else if (menu_ != Menu::None && h.k != H::Menu) { menu_ = Menu::None; InvalidateRect(hwnd_, nullptr, FALSE); }
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { OnEscape(); return 0; }
            if (wparam == VK_RETURN) { OnEnter(nullptr); return 0; }
            if (wparam == VK_TAB) { FocusNext(nullptr, 1); return 0; }
            break;
        case WM_CLOSE:
            Complete(false);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            ApplyPendingFocus();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            // The owner may be closed while a read-only review pumps its modal loop.
            if (options_.proposed_names) done_ = true;
            KillTimer(hwnd_, kAnimTimer);
            DestroyFields();
            if (font_) { DeleteObject(font_); font_ = nullptr; }
            if (font_ai_) { DeleteObject(font_ai_); font_ai_ = nullptr; }
            if (brush_field_) { DeleteObject(brush_field_); brush_field_ = nullptr; }
            if (brush_ai_) { DeleteObject(brush_ai_); brush_ai_ = nullptr; }
            ReleaseFormats();
            compositor_.Shutdown();
            hwnd_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<BatchRenameWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<BatchRenameWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    struct DiffCache { bool computed = false; std::pair<std::vector<rn::Seg>, std::vector<rn::Seg>> segs; };

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HFONT font_ = nullptr;
    HFONT font_ai_ = nullptr;
    HBRUSH brush_field_ = nullptr;
    HBRUSH brush_ai_ = nullptr;
    mutable Compositor compositor_;   // const draw helpers lay out and draw text
    fluent::Painter painter_;
    std::array<IDWriteTextFormat*, static_cast<size_t>(Fmt::Count)> formats_{};
    ID2D1DeviceContext* dc_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    Pal pal_{};
    float fade_ = 1.0f;
    float W_ = 0, H_ = 0;
    ULONGLONG now_ = 0;
    bool anim_active_ = false;
    std::unordered_map<uint64_t, Anim> anims_;
    std::unordered_map<int, ULONGLONG> enter_;
    std::unordered_map<size_t, ULONGLONG> flash_;

    std::vector<std::wstring> paths_;
    std::vector<rn::Source> sources_;
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> existing_;
    std::wstring folder_label_;
    bool taken_loaded_ = false, dims_loaded_ = false;
    std::vector<rn::Step> steps_;
    std::vector<StepUi> steps_ui_;
    int next_step_id_ = 1;
    rn::Options opts_;
    rn::Plan plan_;
    std::vector<DiffCache> diffs_;
    int changed_ = 0, same_ = 0, issues_ = 0;
    std::vector<Field> fields_;
    std::wstring ai_text_;
    enum class AiNote { None, Info, Error };
    AiNote ai_note_ = AiNote::None;
    std::wstring ai_error_;
    std::shared_ptr<BatchRenameAiTask> ai_task_;
    bool ai_done_ = false;          // finished, waiting for kAiMsg
    std::string ai_reply_;
    std::wstring ai_failure_;
    ULONGLONG ai_started_ = 0;
    std::wstring preset_name_;
    std::string preset_key_;       // StepsKey when the preset was applied or saved
    rn::PresetStore store_;
    bool naming_ = false;          // the preset row edits a name (保存为预设)
    bool naming_focus_ = false;
    D2D1_RECT_F preset_dd_{};
    D2D1_RECT_F menu_box_{};
    std::shared_ptr<MetaJob> meta_job_;
    std::vector<int> meta_rank_;   // source -> order in meta_job_ (-1 = not media)       // the open menu, from the last frame
    BatchRenameDialogOptions options_;
    unsigned review_frames_ = 0;
    std::shared_ptr<uint8_t> review_lifetime_ = std::make_shared<uint8_t>(0);
    BatchRenameDialogResult result_;

    std::vector<HitRect> hits_;
    Hit hover_, press_;
    float press_y_ = 0;
    Menu menu_ = Menu::None;
    D2D1_RECT_F add_link_{}, qa_box_{}, order_dd_{}, policy_dd_{}, steps_view_{}, rows_view_{};
    float steps_scroll_ = 0, rows_scroll_ = 0;
    bool menu_from_box_ = false;
    int reveal_step_ = -1;          // card to scroll fully into view on the next layout   // AddStep menu opened from the compact quick-add bar
    int filter_ = 0;
    int focus_step_ = -1;
    std::pair<int, int> pending_focus_{-1, 0};
    int drag_index_ = -1;
    float drag_y_ = 0, drag_grab_ = 0, drag_card_top_ = 0;
    bool content_mode_ = false;
    // 按内容命名: one row per source.
    enum class CSt : uint8_t { Wait, Read, Think, Ok, Keep, Skip, Fail };
    struct CRow {
        CSt st = CSt::Wait;
        std::wstring name;    // suggested name without extension (Ok)
        std::wstring basis;   // 依据, or why it keeps / skips / failed
        std::wstring text;    // compacted text, until sent
        bool pick = true;
        bool sent = false;
    };
    std::vector<CRow> crows_;
    int c_format_ = 0, c_lang_ = 0, c_range_ = 1;
    size_t c_budget_ = 2500;              // chars per file for the running pass
    std::shared_ptr<ContentJob> c_job_;   // reading
    LPARAM c_gen_ = 0;                    // run number, echoed in kContentMsg
    std::shared_ptr<BatchRenameAiTask> c_task_;
    std::vector<size_t> c_inflight_;
    bool c_task_done_ = false;
    std::string c_reply_;
    std::wstring c_failure_;
    bool c_ran_ = false;
    std::wstring c_error_;
    int c_edit_ = -1;                     // row whose name is being edited
    std::vector<std::wstring> folder_samples_;   // other names in the folder (保持我的写法)
    float c_scroll_ = 0, c_content_h_ = 0;
    bool pending_row_focus_ = false;
    bool dark_ = false;
    bool glass_ = false;
    bool done_ = false;
    // Running (scene 5): the renames go through while the dialog shows them.
    bool executing_ = false;
    uint64_t exec_task_ = 0;
    ULONGLONG exec_start_ = 0, exec_end_ = 0;
    size_t exec_shown_ = 0;
    std::vector<int> exec_rank_;   // row -> position in exec_order (-1: not renamed)
    float scale_ = 1.0f;
    D2D1_COLOR_F accent_ = HexColor(0x527D70);
};

} // namespace

BatchRenameDialogResult ShowBatchRenameDialog(HWND owner, const std::vector<std::wstring>& paths, bool dark,
                                              D2D1_COLOR_F accent, const BatchRenameDialogOptions& options) {
    // The dialog runs a nested modal loop; posted commands (menu, shortcut) can still reach the
    // disabled owner, so a second request just raises the open dialog.
    static bool active = false;
    if (active) {
        if (HWND open = FindWindowW(kClass, nullptr)) SetForegroundWindow(open);
        return {};
    }
    active = true;
    BatchRenameWindow window;
    BatchRenameDialogResult result = window.Show(owner, paths, dark, accent, options);
    active = false;
    return result;
}

} // namespace pulse::ui

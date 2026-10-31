#include "dialog_jump.h"
#include "app_catalog.h"
#include "jump_history.h"
#include "jump_match.h"
#include "../common/localization.h"
#include "../index/index_client.h"
#include "../ui/FluentTokens.h"
#include "../ui/edit_host.h"
#include "../ui/shell_icons.h"
#include "../ui/typography.h"
#include "../ui/ui_compositor.h"
#include <commctrl.h>
#include <d2d1_1.h>
#include <dwmapi.h>
#include <dwrite_2.h>
#include <objbase.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>
#include <cwctype>
#include <map>
#include <numeric>

namespace pulse::app {
namespace {
using Microsoft::WRL::ComPtr;
using l10n::StringId;

constexpr UINT kMsgContext = WM_APP + 1;      // SetContext stored a new context
constexpr UINT kMsgFocus = WM_APP + 2;        // FocusSearch
constexpr UINT kMsgIndexStatus = WM_APP + 3;
constexpr UINT kMsgIndexResult = WM_APP + 4;
constexpr UINT kMsgStop = WM_APP + 5;
constexpr UINT_PTR kTimerRestore = 1, kTimerAddress = 2, kTimerSearch = 3, kTimerProbe = 4, kTimerAnim = 5,
                   kTimerGiveUp = 6, kTimerAuto = 7, kTimerForeground = 8;
constexpr int kHotkeyId = 0x5054;
constexpr uint64_t kSession = 0x50554c53444a4d50ull;
// Geometry in DIPs, after the approved quick jump mockup (scene A).
constexpr float kStripHeight = 44, kStripInset = 16, kAccentLine = 2, kStripRadius = 10;
constexpr float kQueryWidth = 210, kQueryFocusWidth = 300, kQueryHeight = 30;
constexpr float kChipHeight = 28, kChipMaxText = 168;
constexpr float kListWidth = 520, kListPad = 6, kRowHeight = 38, kGroupHeight = 25, kListGap = 0;
constexpr float kShadowBlur = 24, kShadowOffset = 10, kShadowAlpha = 0.22f;
constexpr size_t kMaxRows = 6, kMaxChips = 4, kMaxPlaces = 8;
constexpr wchar_t kBarClass[] = L"Pulse.DialogJump";
constexpr wchar_t kListClass[] = L"Pulse.DialogJumpList";
constexpr wchar_t kShadowClass[] = L"Pulse.DialogJumpShadow";

std::wstring ClassOf(HWND hwnd) {
    wchar_t buffer[64]{};
    GetClassNameW(hwnd, buffer, 64);
    return buffer;
}
std::wstring TextOf(HWND hwnd) {
    DWORD_PTR length = 0;
    if (!SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 300, &length)) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    DWORD_PTR copied = 0;
    if (!SendMessageTimeoutW(hwnd, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()), SMTO_ABORTIFHUNG,
                             300, &copied))
        return {};
    text.resize(std::min(static_cast<size_t>(copied), text.size()));
    while (!text.empty() && text.back() == L'\0') text.pop_back();
    return text;
}
bool SetText(HWND hwnd, const std::wstring& text) {
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()), SMTO_ABORTIFHUNG, 500,
                               &result) != 0;
}
HWND AddressControl(HWND dialog, bool edit) {
    if (!dialog || !IsWindow(dialog)) return nullptr;
    // The address band can be nested differently between Windows versions.
    struct Search {
        HWND result = nullptr;
        bool edit;
    } search{nullptr, edit};
    EnumChildWindows(dialog, [](HWND hwnd, LPARAM param) -> BOOL {
        auto& s = *reinterpret_cast<Search*>(param);
        if (ClassOf(hwnd) != L"Address Band Root") return TRUE;
        EnumChildWindows(hwnd, [](HWND child, LPARAM value) -> BOOL {
            auto& state = *reinterpret_cast<Search*>(value);
            if (IsWindowVisible(child) && ClassOf(child) == (state.edit ? L"Edit" : L"ToolbarWindow32") &&
                (state.edit || GetDlgCtrlID(child) == 1001)) {
                state.result = child;
                return FALSE;
            }
            return TRUE;
        }, param);
        return s.result ? FALSE : TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.result;
}
std::wstring Leaf(const std::wstring& path) {
    std::wstring trimmed = path;
    while (trimmed.size() > 3 && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
    const auto slash = trimmed.find_last_of(L"\\/");
    return slash == std::wstring::npos || slash + 1 >= trimmed.size() ? trimmed : trimmed.substr(slash + 1);
}
std::wstring Parent(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return path.substr(0, slash == 2 ? 3 : slash);
}
bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
// Dialogs reject the long-path form Pulse keeps: \\?\C:\x is C:\x and
// \\?\UNC\s\x is \\s\x.
std::wstring PlainPath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + path.substr(8);
    if (path.rfind(L"\\\\?\\", 0) == 0) return path.substr(4);
    return path;
}
std::wstring FolderInput(std::wstring path) {
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    return path;
}

DWORD IntegrityOf(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return 0xFFFFFFFF;
    DWORD level = 0xFFFFFFFF;
    alignas(8) BYTE buffer[96]{};
    DWORD size = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) {
        const auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer);
        level = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1u);
    }
    CloseHandle(token);
    return level;
}
// Messages to a more privileged process are dropped (UIPI): leave its dialogs alone.
bool ReachableProcess(DWORD pid) {
    static const DWORD ours = IntegrityOf(GetCurrentProcess());
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    const DWORD theirs = IntegrityOf(process);
    CloseHandle(process);
    return theirs != 0xFFFFFFFF && theirs <= ours;
}

struct DialogInfo {
    HWND dialog = nullptr, name_edit = nullptr, ok = nullptr;
    bool save = false, folder_picker = false, modern = false;
    DWORD pid = 0, tid = 0;
};

HWND FirstEdit(HWND parent) {
    HWND found = nullptr;
    EnumChildWindows(parent, [](HWND child, LPARAM param) -> BOOL {
        if (ClassOf(child) != L"Edit") return TRUE;
        *reinterpret_cast<HWND*>(param) = child;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

// A common file dialog: #32770 with a shell view inside. The file name box is
// control 0x47C (cmb13, an Edit inside) or 0x480 (edt1); the type filter is
// 0x470 (cmb1), which folder pickers do not have. Windows 11 hosts both in a
// FloatNotifySink with id 0: the name box is the combo with an Edit inside.
bool ProbeDialog(HWND hwnd, DialogInfo& out) {
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || ClassOf(hwnd) != L"#32770") return false;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (!tid || pid == GetCurrentProcessId()) return false;
    struct Scan {
        bool view = false, modern = false, types = false;
        HWND name_box = nullptr, edt1 = nullptr;
    } scan;
    EnumChildWindows(hwnd, [](HWND child, LPARAM param) -> BOOL {
        auto& s = *reinterpret_cast<Scan*>(param);
        const auto cls = ClassOf(child);
        const int id = GetDlgCtrlID(child);
        if (cls == L"DUIViewWndClassName") s.view = s.modern = true;
        else if (cls == L"SHELLDLL_DefView") s.view = true;
        if (id == 0x470) s.types = true;
        if (id == 0x47C && !s.name_box && (cls == L"ComboBoxEx32" || cls == L"ComboBox")) s.name_box = child;
        if (id == 0 && cls == L"ComboBox" && ClassOf(GetParent(child)) == L"FloatNotifySink") {
            if (FindWindowExW(child, nullptr, L"Edit", nullptr)) {
                if (!s.name_box) s.name_box = child;
            } else {
                s.types = true;
            }
        }
        if (id == 0x480 && cls == L"Edit") s.edt1 = child;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&scan));
    if (!scan.view || !ReachableProcess(pid)) return false;
    DialogInfo info;
    info.dialog = hwnd;
    info.pid = pid;
    info.tid = tid;
    info.modern = scan.modern;
    info.name_edit = scan.name_box ? FirstEdit(scan.name_box) : scan.edt1;
    info.ok = GetDlgItem(hwnd, IDOK);
    const auto ok_text = info.ok ? TextOf(info.ok) : std::wstring();
    for (const wchar_t* word : {L"\u4FDD\u5B58", L"\u5132\u5B58", L"\u5B58\u50A8", L"Save", L"save"})
        if (ok_text.find(word) != std::wstring::npos) info.save = true;
    info.folder_picker = scan.modern && !scan.types && !info.save;
    if (!info.name_edit && !info.modern) return false;
    out = info;
    return true;
}


D2D1_COLOR_F Hex(uint32_t rgb, float alpha = 1.0f) { return D2D1::ColorF(rgb, alpha); }
D2D1_COLOR_F Alpha(D2D1_COLOR_F color, float alpha) {
    color.a = alpha;
    return color;
}
D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
COLORREF ToColorRef(D2D1_COLOR_F color) {
    return RGB(static_cast<int>(color.r * 255.0f + 0.5f), static_cast<int>(color.g * 255.0f + 0.5f),
               static_cast<int>(color.b * 255.0f + 0.5f));
}
bool Contains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
D2D1_RECT_F Inflate(D2D1_RECT_F r, float d) { return {r.left - d, r.top - d, r.right + d, r.bottom + d}; }
D2D1_RECT_F Inset(D2D1_RECT_F r, float d) { return Inflate(r, -d); }

// A rectangle whose top and bottom corners have their own radius (the strip
// hangs flat from the dialog's edge; its free corners are round).
ComPtr<ID2D1PathGeometry> RoundedShape(ID2D1DeviceContext* dc, D2D1_RECT_F r, float top, float bottom) {
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(factory.GetAddressOf());
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(path.GetAddressOf())) || FAILED(path->Open(sink.GetAddressOf())))
        return {};
    auto arc = [&](float x, float y, float radius) {
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(radius, radius), 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    };
    sink->BeginFigure(D2D1::Point2F(r.left, r.top + top), D2D1_FIGURE_BEGIN_FILLED);
    if (top > 0) arc(r.left + top, r.top, top);
    sink->AddLine(D2D1::Point2F(r.right - top, r.top));
    if (top > 0) arc(r.right, r.top + top, top);
    sink->AddLine(D2D1::Point2F(r.right, r.bottom - bottom));
    if (bottom > 0) arc(r.right - bottom, r.bottom, bottom);
    sink->AddLine(D2D1::Point2F(r.left + bottom, r.bottom));
    if (bottom > 0) arc(r.left, r.bottom - bottom, bottom);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return path;
}

// Soft drop shadow in a click-through layered window under a surface, like
// CSS box-shadow (offset, blur). `flat_top` keeps it off the dialog the strip
// hangs from.
class Shadow {
public:
    HWND hwnd = nullptr;
    void Create(HINSTANCE instance) {
        hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                               kShadowClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    }
    void Destroy() {
        if (hwnd) DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    void Hide() {
        if (hwnd && IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
    }
    void Show(HWND above, const RECT& shape, float scale, float top_radius, float bottom_radius, float direction,
              bool flat_top, bool flat_bottom, float strength) {
        if (!hwnd) return;
        const int blur = std::max(2, static_cast<int>(std::lround(kShadowBlur * scale)));
        const int offset = static_cast<int>(std::lround(kShadowOffset * scale * direction));
        const int margin = blur + std::abs(offset);
        const int sw = shape.right - shape.left, sh = shape.bottom - shape.top;
        const int top_margin = flat_top ? 0 : margin, bottom_margin = flat_bottom ? 0 : margin;
        const int w = sw + 2 * margin, h = sh + top_margin + bottom_margin;
        if (w <= 0 || h <= 0) return;
        const auto key = std::make_tuple(w, h, offset, blur, top_radius, bottom_radius, flat_top, flat_bottom, strength);
        if (key != key_ || pixels_.empty()) {
            key_ = key;
            Render(w, h, margin, top_margin, sw, sh, offset, blur, top_radius * scale, bottom_radius * scale, strength);
        }
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap && bits) {
            std::memcpy(bits, pixels_.data(), pixels_.size() * sizeof(uint32_t));
            HGDIOBJ old = SelectObject(memory, bitmap);
            POINT position{shape.left - margin, shape.top - top_margin};
            SIZE size{w, h};
            POINT origin{0, 0};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(hwnd, screen, &position, &size, memory, &origin, 0, &blend, ULW_ALPHA);
            SelectObject(memory, old);
        }
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        SetWindowPos(hwnd, above, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

private:
    void Render(int w, int h, int margin, int top_margin, int sw, int sh, int offset, int blur, float rt, float rb,
                float strength) {
        std::vector<float> mask(static_cast<size_t>(w) * static_cast<size_t>(h), 0.0f);
        // Coverage of the shape (moved by the offset), then three box blurs.
        const float left = static_cast<float>(margin), top = static_cast<float>(top_margin + offset);
        const float right = left + static_cast<float>(sw), bottom = top + static_cast<float>(sh);
        for (int y = 0; y < h; ++y) {
            const float py = static_cast<float>(y) + 0.5f;
            for (int x = 0; x < w; ++x) {
                const float px = static_cast<float>(x) + 0.5f;
                const float radius = py < (top + bottom) / 2 ? rt : rb;
                const float cx = std::clamp(px, left + radius, right - radius);
                const float cy = std::clamp(py, top + radius, bottom - radius);
                float d;
                if (px >= left + radius && px <= right - radius) d = std::max(top - py, py - bottom);
                else if (py >= top + radius && py <= bottom - radius) d = std::max(left - px, px - right);
                else d = std::hypot(px - cx, py - cy) - radius;
                mask[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = std::clamp(0.5f - d, 0.0f, 1.0f);
            }
        }
        const int radius = std::max(1, blur / 2);
        std::vector<float> line(static_cast<size_t>(std::max(w, h)));
        auto pass = [&](bool horizontal) {
            const int outer = horizontal ? h : w, inner = horizontal ? w : h;
            for (int o = 0; o < outer; ++o) {
                auto at = [&](int i) -> float& {
                    return horizontal ? mask[static_cast<size_t>(o) * static_cast<size_t>(w) + static_cast<size_t>(i)]
                                      : mask[static_cast<size_t>(i) * static_cast<size_t>(w) + static_cast<size_t>(o)];
                };
                float sum = 0;
                for (int i = -radius; i <= radius; ++i) sum += (i >= 0 && i < inner) ? at(i) : 0.0f;
                for (int i = 0; i < inner; ++i) {
                    line[static_cast<size_t>(i)] = sum / static_cast<float>(2 * radius + 1);
                    const int add = i + radius + 1, drop = i - radius;
                    if (add < inner) sum += at(add);
                    if (drop >= 0) sum -= at(drop);
                }
                for (int i = 0; i < inner; ++i) at(i) = line[static_cast<size_t>(i)];
            }
        };
        for (int i = 0; i < 3; ++i) {
            pass(true);
            pass(false);
        }
        pixels_.assign(mask.size(), 0);
        for (size_t i = 0; i < mask.size(); ++i) {
            const float a = std::clamp(mask[i] * strength, 0.0f, 1.0f);
            const auto channel = [&](float c) { return static_cast<uint32_t>(c * a + 0.5f); };
            pixels_[i] = (static_cast<uint32_t>(a * 255.0f + 0.5f) << 24) | (channel(20) << 16) | (channel(30) << 8) | channel(50);
        }
    }
    std::vector<uint32_t> pixels_;
    std::tuple<int, int, int, int, float, float, bool, bool, float> key_{};
};

// One composition-backed popup (premultiplied alpha, so rounded corners and
// the hosted edit draw like the rest of Pulse).
struct Surface {
    HWND hwnd = nullptr;
    std::unique_ptr<ui::Compositor> comp;
    ComPtr<ID2D1SolidColorBrush> brush;
    std::map<std::pair<int, int>, ComPtr<IDWriteTextFormat>> formats;
    ID2D1DeviceContext* Begin(float scale) {
        if (!comp) comp = std::make_unique<ui::Compositor>();
        if (comp->NeedsRecovery()) {
            if (!comp->Recover()) return nullptr;
            brush.Reset();
        } else if (!comp->Dc() && !comp->Init(hwnd)) {
            return nullptr;
        }
        RECT rc{};
        GetClientRect(hwnd, &rc);
        if (rc.right != comp->Width() || rc.bottom != comp->Height()) comp->Resize(rc.right, rc.bottom);
        ID2D1DeviceContext* dc = comp->Dc();
        dc->SetDpi(96.0f * scale, 96.0f * scale);
        if (!brush && FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0), brush.GetAddressOf()))) return nullptr;
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0.0f));
        return dc;
    }
    bool End() {
        const HRESULT hr = comp->Dc()->EndDraw();
        if (FAILED(hr)) {
            comp->NotifyDeviceLost(hr);
            brush.Reset();
            return false;
        }
        comp->Present();
        return true;
    }
    IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT weight) {
        const auto key = std::make_pair(static_cast<int>(size * 100.0f), static_cast<int>(weight));
        auto& slot = formats[key];
        if (!slot && comp && comp->DwriteFactory()) {
            ui::typography::CreateTextFormat(comp->DwriteFactory(),
                                             {ui::typography::FontRole::Text, size * ui::typography::UiFontScale(), weight},
                                             slot.GetAddressOf());
            if (slot) {
                slot->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                slot->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                ComPtr<IDWriteInlineObject> ellipsis;
                comp->DwriteFactory()->CreateEllipsisTrimmingSign(slot.Get(), ellipsis.GetAddressOf());
                slot->SetTrimming(&trimming, ellipsis.Get());
            }
        }
        return slot.Get();
    }
    float Measure(const std::wstring& text, float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL) {
        IDWriteTextFormat* format = Format(size, weight);
        if (!format || text.empty()) return 0;
        return std::ceil(ui::typography::MeasureLine(comp.get(), format, text));
    }
    // Keeps the end of a path that does not fit: drive, an ellipsis, then the last folders.
    std::wstring Elide(const std::wstring& path, float size, float max_width) {
        if (Measure(path, size) <= max_width) return path;
        std::vector<std::wstring> parts;
        for (size_t at = 0; at <= path.size();) {
            const size_t next = std::min(path.find(L'\\', at), path.size());
            if (next > at) parts.push_back(path.substr(at, next - at));
            at = next + 1;
        }
        if (parts.size() < 2) return path;
        const std::wstring head = parts.front() + L"\\\u2026";
        std::wstring best = head;
        std::wstring tail;
        for (size_t i = parts.size(); i-- > 1;) {
            tail = L"\\" + parts[i] + tail;
            if (Measure(head + tail, size) > max_width) break;
            best = head + tail;
        }
        return best;
    }
    // Text in `rect`, vertically centred; `marks` are ranges drawn semibold in `mark`.
    void Text(const std::wstring& text, D2D1_RECT_F rect, float size, D2D1_COLOR_F color,
              DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
              const std::vector<DWRITE_TEXT_RANGE>* marks = nullptr, D2D1_COLOR_F mark = {}) {
        IDWriteTextFormat* format = Format(size, weight);
        if (!format || text.empty() || rect.right <= rect.left || !brush) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(comp->DwriteFactory()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                                           rect.right - rect.left, rect.bottom - rect.top,
                                                           layout.GetAddressOf())))
            return;
        layout->SetTextAlignment(align);
        ComPtr<ID2D1SolidColorBrush> mark_brush;
        if (marks && !marks->empty() && SUCCEEDED(comp->Dc()->CreateSolidColorBrush(mark, mark_brush.GetAddressOf()))) {
            for (const auto& range : *marks) {
                layout->SetDrawingEffect(mark_brush.Get(), range);
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range);
            }
        }
        brush->SetColor(color);
        comp->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void Fill(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius) {
        brush->SetColor(color);
        if (radius > 0) comp->Dc()->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
        else comp->Dc()->FillRectangle(rect, brush.Get());
    }
    void Stroke(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius, float width = 1.0f) {
        brush->SetColor(color);
        const D2D1_RECT_F inner = Inset(rect, width / 2);
        comp->Dc()->DrawRoundedRectangle(D2D1::RoundedRect(inner, radius, radius), brush.Get(), width);
    }
};

// Case-insensitive occurrences of each typed word in `text` (pinyin matches
// are not marked).
std::vector<DWRITE_TEXT_RANGE> MatchRanges(const std::wstring& text, const std::wstring& query) {
    std::vector<DWRITE_TEXT_RANGE> ranges;
    size_t begin = 0;
    while ((begin = query.find_first_not_of(L" \t", begin)) != std::wstring::npos) {
        const size_t end = std::min(query.find_first_of(L" \t", begin), query.size());
        std::wstring word = query.substr(begin, end - begin);
        begin = end;
        if (const auto slash = word.find_last_of(L"\\/"); slash != std::wstring::npos) word = word.substr(slash + 1);
        if (word.empty()) continue;
        for (size_t at = 0; at + word.size() <= text.size(); ++at) {
            if (CompareStringOrdinal(text.data() + at, static_cast<int>(word.size()), word.data(),
                                     static_cast<int>(word.size()), TRUE) == CSTR_EQUAL) {
                ranges.push_back({static_cast<UINT32>(at), static_cast<UINT32>(word.size())});
                break;
            }
        }
    }
    return ranges;
}

thread_local DialogJump::Impl* t_impl = nullptr;
} // namespace

struct DialogJump::Impl {
    // Shared with the UI thread.
    std::mutex mu;
    DialogJumpContext shared_context;
    std::shared_ptr<JumpHistory> shared_history;
    std::atomic<HWND> service{nullptr};
    std::atomic<UINT> context_message{0};
    std::atomic<HWND> bar_window{nullptr};
    std::atomic<bool> showing{false};
    std::atomic<bool> ready{false};

    // The bar thread only.
    struct Palette {
        D2D1_COLOR_F card, line, text, sub, faint, row, accent, accent_soft, accent_line, hit, logo, logo_text;
        float shadow = kShadowAlpha;
    } pal{};
    enum class Hot { None, Query, Chip, Keys, More };
    enum class Placement { Below, Above, InsideTop };
    enum class ListMode { Hidden, Search, Places };
    struct Chip {
        D2D1_RECT_F rect{};
        size_t folder = 0;
        bool current = false;
        std::wstring name, note;
    };
    struct Row {
        enum class Kind { Group, Item, Note } kind = Kind::Item;
        std::wstring text, path;
        bool dir = true;
    };
    struct Found {
        std::wstring path, name;
        bool dir = true;
    };

    HINSTANCE instance = nullptr;
    Surface bar, list;
    Shadow bar_shadow, list_shadow;
    std::unique_ptr<ui::ShellIconCache> icons;
    HWND edit = nullptr, tooltip = nullptr;
    HFONT edit_font = nullptr;
    HBRUSH field_brush = nullptr;
    ComPtr<IDWriteTextFormat> edit_format;
    HWINEVENTHOOK foreground_hook = nullptr, dialog_hook = nullptr;
    bool hotkey = false;
    DialogInfo dlg;
    HWND probe_target = nullptr;
    HWND observed_foreground = nullptr;
    int probe_tries = 0;
    DialogJumpContext ctx;
    // Frequent places: ctx.folders from `history_from` on came from the jump
    // history only; `scores` (per folder) puts the most visited first and
    // `exists` caches whether a history-only folder is still there.
    std::shared_ptr<JumpHistory> history;
    std::wstring dlg_app;  // exe name of the dialog's program
    std::vector<double> scores;
    std::vector<int8_t> exists;
    size_t history_from = 0;
    bool auto_pending = false;  // a new dialog waits for its auto-jump
    float scale = 1.0f;
    float width = 0;  // strip width in DIPs
    Placement placement = Placement::Below;
    RECT strip_px{};

    // Layout (DIPs, strip client coordinates).
    D2D1_RECT_F logo_rect{}, query_rect{}, keys_rect{}, ctrl_rect{}, plus_rect{}, g_rect{}, more_rect{};
    std::vector<Chip> chips;
    float query_width = kQueryWidth, query_from = kQueryWidth, query_to = kQueryWidth;
    ULONGLONG anim_start = 0;
    bool edit_focused = false;
    Hot hover = Hot::None;
    size_t hover_chip = 0;
    bool tracking = false, list_tracking = false;

    ListMode list_mode = ListMode::Hidden;
    bool list_above = false;
    std::vector<Row> rows;
    std::vector<Found> known, indexed;
    int selected = -1, list_hover = -1;
    std::wstring query;
    bool searching = false;
    index::IndexClient index_client;
    bool index_started = false;
    uint32_t search_id = 0;

    std::wstring restore_text, set_text;
    HWND restore_edit = nullptr;
    int restore_ticks = 0;
    std::wstring address_path;
    int address_ticks = 0;
    bool address_keys_sent = false;

    void Run();
    void Evaluate(HWND foreground);
    void Attach(const DialogInfo& info);
    void Detach();
    void TakeContext();
    void UpdatePalette();
    void Place();
    void Layout();
    void LayoutEdit();
    void PaintBar();
    void PaintList();
    void UpdateTooltips();
    D2D1_COLOR_F FieldColor() const { return edit_focused ? pal.card : pal.row; }
    void UpdateFieldBrush();
    void OnEditFocus(bool focused);
    bool EditKey(WPARAM key);
    void OnSearchChanged();
    void StartIndexSearch();
    void OnIndexResult(uint32_t id);
    void BuildSearchRows();
    void BuildPlaceRows();
    bool ShowFrequent();
    bool Exists(size_t folder);
    std::vector<size_t> ByScore(std::vector<size_t> folders) const;
    int FirstItem() const;
    void MoveSelection(int delta);
    float ListHeight() const;
    void ShowList();
    void HideList();
    void Pick(size_t row);
    void Navigate(const std::wstring& path, bool dir, bool record = true);
    void NavigateAddressBar(const std::wstring& path);
    void SendAddressKeys();
    void ReturnToDialog();
    void FocusEdit();
    std::vector<size_t> OpenFolders() const;

    static LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data);
    static void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG id_object, LONG id_child, DWORD, DWORD);
    static void CALLBACK OnDialogEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD,
                                       DWORD);
    LRESULT HandleBar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleList(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
};

// --- thread -----------------------------------------------------------------

void DialogJump::Impl::Run() {
    t_impl = this;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = &Impl::BarProc;
    wc.lpszClassName = kBarClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = &Impl::ListProc;
    wc.lpszClassName = kListClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = kShadowClass;
    RegisterClassExW(&wc);
    const DWORD popup_ex = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
    bar.hwnd = CreateWindowExW(popup_ex, kBarClass, L"Pulse", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 10, 10, nullptr,
                               nullptr, instance, nullptr);
    list.hwnd = CreateWindowExW(popup_ex | WS_EX_NOACTIVATE, kListClass, L"", WS_POPUP, 0, 0, 10, 10, bar.hwnd,
                                nullptr, instance, nullptr);
    bar_shadow.Create(instance);
    list_shadow.Create(instance);
    if (!bar.hwnd || !list.hwnd) {
        ready = true;
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }
    for (HWND hwnd : {bar.hwnd, list.hwnd}) {
        const DWORD no_round = 1;            // DWMWCP_DONOTROUND: the surfaces draw their own shape
        const DWORD no_border = 0xFFFFFFFE;  // DWMWA_COLOR_NONE
        DwmSetWindowAttribute(hwnd, 33, &no_round, sizeof(no_round));
        DwmSetWindowAttribute(hwnd, 34, &no_border, sizeof(no_border));
    }
    icons = std::make_unique<ui::ShellIconCache>();
    icons->SetNotifyWindow(bar.hwnd);
    icons->AddNotifyWindow(list.hwnd);
    edit = ui::CreateChildEdit(bar.hwnd);
    if (edit) {
        SetWindowTheme(edit, L"", L"");
        SetWindowSubclass(edit, &Impl::EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(edit, EM_SETLIMITTEXT, 260, 0);
    }
    tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0,
                              0, bar.hwnd, nullptr, instance, nullptr);
    if (tooltip) SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 640);

    foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &Impl::OnForeground, 0,
                                      0, WINEVENT_OUTOFCONTEXT);
    bar_window = bar.hwnd;
    ready = true;
    SetTimer(bar.hwnd, kTimerForeground, 300, nullptr);
    Evaluate(GetForegroundWindow());
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kMsgStop) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Detach();
    if (foreground_hook) UnhookWinEvent(foreground_hook);
    index_client.Stop();
    bar_window = nullptr;
    if (edit) RemoveWindowSubclass(edit, &Impl::EditProc, 1);
    if (tooltip) DestroyWindow(tooltip);
    // Graphics belong to this thread: release them here, not in ~Impl.
    icons.reset();
    edit_format.Reset();
    for (Surface* surface : {&list, &bar}) {
        surface->formats.clear();
        surface->brush.Reset();
        surface->comp.reset();
        DestroyWindow(surface->hwnd);
        surface->hwnd = nullptr;
    }
    bar_shadow.Destroy();
    list_shadow.Destroy();
    if (edit_font) DeleteObject(edit_font);
    if (field_brush) DeleteObject(field_brush);
    UnregisterClassW(kBarClass, instance);
    UnregisterClassW(kListClass, instance);
    UnregisterClassW(kShadowClass, instance);
    if (SUCCEEDED(com)) CoUninitialize();
    t_impl = nullptr;
}

void CALLBACK DialogJump::Impl::OnForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG id_object, LONG id_child, DWORD,
                                             DWORD) {
    if (t_impl && id_object == OBJID_WINDOW && id_child == CHILDID_SELF) {
        // Out-of-context events may be queued behind a later activation.
        const HWND current = GetForegroundWindow();
        t_impl->Evaluate(current ? current : hwnd);
    }
}

void CALLBACK DialogJump::Impl::OnDialogEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_object, LONG id_child,
                                              DWORD, DWORD) {
    Impl* self = t_impl;
    if (!self || hwnd != self->dlg.dialog || id_object != OBJID_WINDOW || id_child != CHILDID_SELF) return;
    if (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE) self->Detach();
    else if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        if (IsIconic(hwnd)) self->Detach();
        else self->Place();
    }
}

void DialogJump::Impl::Evaluate(HWND foreground) {
    if (!foreground) return;
    observed_foreground = foreground;
    if (foreground == bar.hwnd || foreground == list.hwnd || foreground == tooltip) return;
    if (dlg.dialog && foreground == dlg.dialog) {
        if (!IsWindowVisible(bar.hwnd)) Attach(dlg);
        return;
    }
    DialogInfo info;
    if (ProbeDialog(foreground, info)) {
        Attach(info);
        return;
    }
    Detach();
    KillTimer(bar.hwnd, kTimerProbe);
    // A dialog can take the foreground before it is visible and before its
    // controls exist, and no new foreground event follows: keep looking for
    // up to 3 s while no other window takes over.
    if (ClassOf(foreground) == L"#32770") {
        probe_target = foreground;
        probe_tries = 0;
        SetTimer(bar.hwnd, kTimerProbe, 100, nullptr);
    }
}

void DialogJump::Impl::Attach(const DialogInfo& info) {
    if (dlg.dialog != info.dialog) {
        Detach();
        dlg = info;
        dialog_hook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr, &Impl::OnDialogEvent,
                                      info.pid, info.tid, WINEVENT_OUTOFCONTEXT);
        if (edit) SetWindowTextW(edit, L"");
        rows.clear();
        list_mode = ListMode::Hidden;
        dlg_app = ProcessExeName(info.pid);
        // Auto-jump once the fresh context is in (or after 700 ms without).
        auto_pending = true;
        SetTimer(bar.hwnd, kTimerAuto, 700, nullptr);
    }
    KillTimer(bar.hwnd, kTimerProbe);
    if (const HWND service_window = service.load()) PostMessageW(service_window, context_message.load(), 0, 0);
    TakeContext();
    if (!hotkey) hotkey = RegisterHotKey(bar.hwnd, kHotkeyId, MOD_CONTROL | MOD_NOREPEAT, 'G') != FALSE;
    if (edit) {
        const auto& cue = l10n::Get(dlg.save || dlg.folder_picker ? StringId::DialogJumpSearchCue
                                                                  : StringId::DialogJumpSearchCueOpen);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
    }
    UpdatePalette();
    Place();
    showing = true;
}

void DialogJump::Impl::Detach() {
    if (edit && GetFocus() == edit) SetFocus(nullptr);
    edit_focused = false;
    query_width = query_from = query_to = kQueryWidth;
    KillTimer(bar.hwnd, kTimerAddress);
    KillTimer(bar.hwnd, kTimerAuto);
    auto_pending = false;
    KillTimer(bar.hwnd, kTimerAnim);
    if (dialog_hook) {
        UnhookWinEvent(dialog_hook);
        dialog_hook = nullptr;
    }
    if (hotkey) {
        UnregisterHotKey(bar.hwnd, kHotkeyId);
        hotkey = false;
    }
    HideList();
    if (bar.hwnd && IsWindowVisible(bar.hwnd)) ShowWindow(bar.hwnd, SW_HIDE);
    bar_shadow.Hide();
    showing = false;
    if (dlg.dialog) {
        dlg = {};
        if (index_started) index_client.CancelSession(kSession);
    }
}

void DialogJump::Impl::TakeContext() {
    {
        std::lock_guard lock(mu);
        ctx = shared_context;
        history = shared_history;
    }
    ctx.current = PlainPath(ctx.current);
    for (auto& folder : ctx.folders) folder.path = PlainPath(folder.path);
    history_from = ctx.folders.size();
    scores.assign(ctx.folders.size(), 0.0);
    if (ctx.remember && history) {
        for (const auto& path : history->Top(dlg_app, 24)) {
            if (IsAppPath(path)) continue;  // programs started from quick search
            const std::wstring plain = PlainPath(path);
            if (std::none_of(ctx.folders.begin(), ctx.folders.end(), [&](const auto& f) { return SamePath(f.path, plain); }))
                ctx.folders.push_back({plain, {}, false, false});
        }
        scores.resize(ctx.folders.size());
        for (size_t i = 0; i < ctx.folders.size(); ++i) scores[i] = history->Score(ctx.folders[i].path, dlg_app);
    }
    exists.assign(ctx.folders.size(), 1);
    for (size_t i = history_from; i < exists.size(); ++i) exists[i] = -1;
}

// History-only folders may be gone; network ones are not probed (slow).
bool DialogJump::Impl::Exists(size_t folder) {
    if (folder >= exists.size()) return false;
    if (exists[folder] < 0) {
        const auto& path = ctx.folders[folder].path;
        bool ok = path.rfind(L"\\\\", 0) == 0;
        if (!ok && path.size() >= 2 && path[1] == L':') {
            const wchar_t root[] = {path[0], L':', L'\\', 0};
            const DWORD attributes = GetDriveTypeW(root) == DRIVE_REMOTE ? FILE_ATTRIBUTE_DIRECTORY : GetFileAttributesW(path.c_str());
            ok = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
        }
        exists[folder] = static_cast<int8_t>(ok ? 1 : 0);
    }
    return exists[folder] > 0;
}

// Most visited first; equal scores keep their order.
std::vector<size_t> DialogJump::Impl::ByScore(std::vector<size_t> folders) const {
    std::stable_sort(folders.begin(), folders.end(), [&](size_t a, size_t b) {
        return (a < scores.size() ? scores[a] : 0) > (b < scores.size() ? scores[b] : 0);
    });
    return folders;
}

// --- look ---------------------------------------------------------------------

void DialogJump::Impl::UpdatePalette() {
    const bool hc = ui::IsHighContrast();
    const ui::Theme theme = hc ? ui::MakeHighContrastTheme() : ui::MakeTheme(ctx.dark, ctx.accent);
    pal.accent = theme.accent;
    pal.text = theme.text;
    if (ctx.dark) {
        pal.card = Hex(0x2B2B2B);
        pal.line = Hex(0x3A3A3A);
        pal.sub = Hex(0xA9ADB3);
        pal.faint = Hex(0x80858C);
        pal.row = Hex(0x333333);
        pal.accent_soft = Mix(pal.card, pal.accent, 0.24f);
        pal.hit = Mix(pal.accent, Hex(0xFFFFFF), 0.42f);
        pal.logo = Hex(0xF2F2F2);
        pal.logo_text = Hex(0x161616);
        pal.shadow = 0.5f;
    } else {
        pal.card = Hex(0xFFFFFF);
        pal.line = theme.stroke_divider;
        pal.sub = theme.text_secondary;
        pal.faint = Mix(theme.text_secondary, pal.card, 0.25f);
        pal.row = Hex(0xF6F8F6);
        pal.accent_soft = Mix(pal.card, pal.accent, 0.13f);
        pal.hit = Mix(pal.accent, Hex(0x000000), 0.18f);
        pal.logo = theme.text;
        pal.logo_text = Hex(0xFFFFFF);
        pal.shadow = kShadowAlpha;
    }
    if (hc) {
        pal.card = theme.bg;
        pal.row = theme.bg;
        pal.line = theme.text;
        pal.sub = pal.faint = theme.text;
    }
    pal.accent_line = Alpha(pal.accent, 0.35f);
    UpdateFieldBrush();
}

void DialogJump::Impl::UpdateFieldBrush() {
    if (field_brush) DeleteObject(field_brush);
    field_brush = CreateSolidBrush(ToColorRef(FieldColor()));
}

// --- placement and layout -----------------------------------------------------

void DialogJump::Impl::Place() {
    if (!dlg.dialog || !IsWindow(dlg.dialog)) return;
    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(dlg.dialog, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
        GetWindowRect(dlg.dialog, &frame);
    const HMONITOR monitor = MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST);
    UINT dpi_x = 96, dpi_y = 96;
    // shcore is loaded at run time: a static import breaks the Windows 8.1 startup guard.
    using MonitorDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (const HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        if (const auto dpi = reinterpret_cast<MonitorDpi>(GetProcAddress(shcore, "GetDpiForMonitor")))
            if (FAILED(dpi(monitor, 0, &dpi_x, &dpi_y))) dpi_x = dpi_y = 96;
        FreeLibrary(shcore);
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const float new_scale = static_cast<float>(dpi_x) / 96.0f;
    const bool rescaled = std::fabs(new_scale - scale) > 0.001f || !edit_font;
    scale = new_scale;
    const int height = static_cast<int>(std::lround(kStripHeight * scale));
    const int inset = static_cast<int>(std::lround(kStripInset * scale));
    int left = frame.left + inset, right = frame.right - inset;
    const int min_width = static_cast<int>(std::lround(420 * scale));
    if (right - left < min_width) {
        const int mid = (frame.left + frame.right) / 2;
        left = mid - min_width / 2;
        right = left + min_width;
    }
    int top = frame.bottom;
    placement = Placement::Below;
    if (top + height > mi.rcWork.bottom) {
        if (frame.top - height >= mi.rcWork.top) {
            top = frame.top - height;
            placement = Placement::Above;
        } else {
            top = std::max(frame.top, mi.rcWork.top);
            placement = Placement::InsideTop;
        }
    }
    strip_px = {left, top, right, top + height};
    if (rescaled && edit) {
        if (HFONT font = ui::typography::CreateEditFont(scale, 13.0f)) {
            if (edit_font) DeleteObject(edit_font);
            edit_font = font;
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font), FALSE);
        }
        edit_format.Reset();
    }
    SetWindowPos(bar.hwnd, HWND_TOPMOST, left, top, right - left, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    width = static_cast<float>(right - left) / scale;
    if (!bar.comp || !bar.comp->Dc()) {
        if (!bar.comp) bar.comp = std::make_unique<ui::Compositor>();
        if (!bar.comp->Init(bar.hwnd)) bar.comp.reset();
    }
    if (bar.comp && !edit_format && bar.comp->DwriteFactory()) {
        bar.comp->DwriteFactory()->CreateTextFormat(ui::typography::PreferredTextFamily(), nullptr,
                                                    DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                    DWRITE_FONT_STRETCH_NORMAL,
                                                    static_cast<float>(ui::typography::EditFontPixels(scale, 13.0f)),
                                                    l10n::LocaleName(), edit_format.GetAddressOf());
    }
    if (icons && bar.comp) {
        icons->SetDeviceContext(bar.comp->Dc());
        icons->SetScale(scale);
    }
    const bool flipped = placement == Placement::Above;
    bar_shadow.Show(bar.hwnd, strip_px, scale, flipped ? kStripRadius : 0.0f, flipped ? 0.0f : kStripRadius,
                    flipped ? -0.5f : 1.0f, !flipped, flipped, pal.shadow);
    Layout();
    InvalidateRect(bar.hwnd, nullptr, FALSE);
    if (list_mode != ListMode::Hidden) ShowList();
}

std::vector<size_t> DialogJump::Impl::OpenFolders() const {
    std::vector<size_t> open;
    for (size_t i = 0; i < ctx.folders.size(); ++i)
        if (ctx.folders[i].open) open.push_back(i);
    std::stable_partition(open.begin(), open.end(),
                          [&](size_t i) { return SamePath(ctx.folders[i].path, ctx.current); });
    return open;
}

void DialogJump::Impl::Layout() {
    if (!bar.comp) return;
    // Content sits below (or, flipped, above) the 2 DIP accent edge.
    const float mid = placement == Placement::Above ? (kStripHeight - kAccentLine) / 2
                                                    : kAccentLine + (kStripHeight - kAccentLine) / 2;
    logo_rect = D2D1::RectF(10, mid - 10, 30, mid + 10);
    query_rect = D2D1::RectF(36, mid - kQueryHeight / 2, 36 + query_width, mid + kQueryHeight / 2);
    more_rect = D2D1::RectF(width - 8 - 28, mid - 14, width - 8, mid + 14);
    keys_rect = ctrl_rect = plus_rect = g_rect = {};
    float right = more_rect.left - 4;
    if (!ctx.current.empty()) {
        const float ctrl_w = bar.Measure(L"Ctrl", 11.5f) + 12, g_w = bar.Measure(L"G", 11.5f) + 12;
        const float plus_w = bar.Measure(L"+", 12.0f) + 6;
        const float group = 6 + ctrl_w + plus_w + g_w + 6;
        keys_rect = D2D1::RectF(right - group, mid - 14, right, mid + 14);
        ctrl_rect = D2D1::RectF(keys_rect.left + 6, mid - 10, keys_rect.left + 6 + ctrl_w, mid + 10);
        plus_rect = D2D1::RectF(ctrl_rect.right, mid - 10, ctrl_rect.right + plus_w, mid + 10);
        g_rect = D2D1::RectF(plus_rect.right, mid - 10, plus_rect.right + g_w, mid + 10);
        right = keys_rect.left - 6;
    }
    chips.clear();
    float x = query_rect.right + 6;
    for (size_t index : OpenFolders()) {
        if (chips.size() >= kMaxChips) break;
        const auto& folder = ctx.folders[index];
        Chip chip;
        chip.folder = index;
        chip.current = SamePath(folder.path, ctx.current);
        chip.name = folder.label.empty() ? Leaf(folder.path) : folder.label;
        if (chip.current) chip.note = l10n::Get(StringId::DialogJumpChipCurrent);
        else if (folder.path.size() > 2 && folder.path[1] == L':' && ctx.current.size() > 2 &&
                 std::towupper(folder.path[0]) != std::towupper(ctx.current[0]))
            chip.note = folder.path.substr(0, 2);
        const float name_w = std::min(bar.Measure(chip.name, 13.0f), kChipMaxText);
        const float note_w = chip.note.empty() ? 0.0f : bar.Measure(chip.note, 11.5f) + 4;
        const float lead = chip.current ? 6.0f : 16.0f;
        float w = 9 + lead + 6 + name_w + note_w + 9;
        if (x + w > right) {
            const float room = right - x;
            if (room < 9 + lead + 6 + 48 + 9) break;
            w = room;
        }
        chip.rect = D2D1::RectF(x, mid - kChipHeight / 2, x + w, mid + kChipHeight / 2);
        chips.push_back(std::move(chip));
        x += w + 2;
    }
    LayoutEdit();
    UpdateTooltips();
}

void DialogJump::Impl::LayoutEdit() {
    if (!edit) return;
    const int line = ui::EditLineHeight(edit, edit_font, static_cast<int>(std::lround(kQueryHeight * scale)));
    const float left = query_rect.left + 28, right = query_rect.right - 8;
    const float mid = (query_rect.top + query_rect.bottom) / 2;
    MoveWindow(edit, static_cast<int>(std::lround(left * scale)), static_cast<int>(std::lround(mid * scale)) - line / 2,
               std::max(1, static_cast<int>(std::lround((right - left) * scale))), line, TRUE);
    if (!IsWindowVisible(edit)) ShowWindow(edit, SW_SHOW);
    if (bar.comp && edit_format) ui::PresentChildEdit(*bar.comp, edit_format.Get(), pal.text, FieldColor(), edit);
}

void DialogJump::Impl::UpdateTooltips() {
    if (!tooltip) return;
    while (SendMessageW(tooltip, TTM_GETTOOLCOUNT, 0, 0) > 0) {
        TTTOOLINFOW info{sizeof(info)};
        if (!SendMessageW(tooltip, TTM_ENUMTOOLSW, 0, reinterpret_cast<LPARAM>(&info))) break;
        SendMessageW(tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }
    auto add = [&](UINT_PTR id, const D2D1_RECT_F& r, const std::wstring& text) {
        if (r.right <= r.left) return;
        TTTOOLINFOW info{sizeof(info)};
        info.uFlags = TTF_SUBCLASS;
        info.hwnd = bar.hwnd;
        info.uId = id;
        info.rect = {static_cast<LONG>(r.left * scale), static_cast<LONG>(r.top * scale),
                     static_cast<LONG>(r.right * scale), static_cast<LONG>(r.bottom * scale)};
        info.lpszText = const_cast<wchar_t*>(text.c_str());
        SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    };
    for (size_t i = 0; i < chips.size(); ++i) add(1 + i, chips[i].rect, ctx.folders[chips[i].folder].path);
    add(20, keys_rect, l10n::Get(StringId::DialogJumpCurrent));
    add(21, more_rect, l10n::Get(StringId::DialogJumpMore));
}

// --- paint --------------------------------------------------------------------

void DialogJump::Impl::PaintBar() {
    ID2D1DeviceContext* dc = bar.Begin(scale);
    if (!dc) return;
    const bool flipped = placement == Placement::Above;
    const D2D1_RECT_F all{0, 0, width, kStripHeight};
    if (auto shape = RoundedShape(dc, all, flipped ? kStripRadius : 0.0f, flipped ? 0.0f : kStripRadius)) {
        bar.brush->SetColor(pal.card);
        dc->FillGeometry(shape.Get(), bar.brush.Get());
    }
    bar.Fill(flipped ? D2D1::RectF(0, kStripHeight - kAccentLine, width, kStripHeight) : D2D1::RectF(0, 0, width, kAccentLine),
             pal.accent, 0);
    // P mark.
    bar.Fill(logo_rect, pal.logo, 6);
    bar.Text(L"P", logo_rect, 12.0f, pal.logo_text, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    // Search box: widens with a soft accent ring while it has the focus.
    if (edit_focused) {
        bar.Fill(Inflate(query_rect, 3), pal.accent_soft, 10);
        bar.Fill(query_rect, pal.card, 7);
        bar.Stroke(query_rect, pal.accent, 7);
    } else {
        bar.Fill(query_rect, pal.row, 7);
        bar.Stroke(query_rect, pal.line, 7);
    }
    {
        const float s = 14.0f / 24.0f, x0 = query_rect.left + 8, y0 = (query_rect.top + query_rect.bottom) / 2 - 7;
        bar.brush->SetColor(pal.faint);
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x0 + 11 * s, y0 + 11 * s), 7 * s, 7 * s), bar.brush.Get(), 2.2f * s);
        dc->DrawLine(D2D1::Point2F(x0 + 20 * s, y0 + 20 * s), D2D1::Point2F(x0 + 16.5f * s, y0 + 16.5f * s),
                     bar.brush.Get(), 2.2f * s);
    }
    // Folders open in Pulse.
    for (size_t i = 0; i < chips.size(); ++i) {
        const auto& chip = chips[i];
        const bool hot = hover == Hot::Chip && hover_chip == i;
        if (chip.current) {
            bar.Fill(chip.rect, hot ? Mix(pal.accent_soft, pal.accent, 0.08f) : pal.accent_soft, 7);
            bar.Stroke(chip.rect, pal.accent_line, 7);
        } else if (hot) {
            bar.Fill(chip.rect, pal.row, 7);
        }
        const float mid = (chip.rect.top + chip.rect.bottom) / 2;
        float x = chip.rect.left + 9;
        if (chip.current) {
            bar.brush->SetColor(pal.accent);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 3, mid), 3, 3), bar.brush.Get());
            x += 6;
        } else {
            const auto& folder = ctx.folders[chip.folder];
            if (!icons || !icons->Draw(dc, D2D1::RectF(x, mid - 8, x + 16, mid + 8), folder.path, Leaf(folder.path), true,
                                       FILE_ATTRIBUTE_DIRECTORY))
                bar.Fill(D2D1::RectF(x + 1, mid - 5, x + 15, mid + 6), Hex(0xE8B44A), 2);
            x += 16;
        }
        x += 6;
        const float note_w = chip.note.empty() ? 0.0f : bar.Measure(chip.note, 11.5f) + 4;
        const float name_right = chip.rect.right - 9 - note_w;
        bar.Text(chip.name, D2D1::RectF(x, chip.rect.top, name_right, chip.rect.bottom), 13.0f, pal.text);
        if (!chip.note.empty()) {
            const float name_w = std::min(bar.Measure(chip.name, 13.0f), name_right - x);
            bar.Text(chip.note, D2D1::RectF(x + name_w + 4, chip.rect.top, chip.rect.right - 9, chip.rect.bottom), 11.5f,
                     pal.faint);
        }
    }
    if (chips.empty()) {
        const float left = query_rect.right + 12;
        const float right = (keys_rect.right > keys_rect.left ? keys_rect.left : more_rect.left) - 8;
        bar.Text(l10n::Get(StringId::DialogJumpNone), D2D1::RectF(left, query_rect.top, right, query_rect.bottom), 12.5f,
                 pal.faint);
    }
    // Ctrl + G: the current Pulse folder.
    if (keys_rect.right > keys_rect.left) {
        if (hover == Hot::Keys) bar.Fill(keys_rect, pal.row, 7);
        for (const auto* key : {&ctrl_rect, &g_rect}) {
            bar.Fill(*key, hover == Hot::Keys ? pal.card : pal.row, 5);
            bar.Stroke(*key, pal.line, 5);
        }
        bar.Text(L"Ctrl", ctrl_rect, 11.5f, pal.sub, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        bar.Text(L"+", plus_rect, 12.0f, pal.faint, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        bar.Text(L"G", g_rect, 11.5f, pal.sub, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    // More places.
    if (hover == Hot::More || list_mode == ListMode::Places) bar.Fill(more_rect, pal.row, 7);
    {
        const float cx = (more_rect.left + more_rect.right) / 2, cy = (more_rect.top + more_rect.bottom) / 2;
        bar.brush->SetColor(pal.sub);
        for (float dx : {-5.0f, 0.0f, 5.0f}) dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + dx, cy), 1.4f, 1.4f), bar.brush.Get());
    }
    if (!bar.End() && icons) icons->Reset();
}

float DialogJump::Impl::ListHeight() const {
    float h = 2 * kListPad;
    for (const auto& row : rows) h += row.kind == Row::Kind::Item ? kRowHeight : kGroupHeight;
    return h;
}

void DialogJump::Impl::PaintList() {
    ID2D1DeviceContext* dc = list.Begin(scale);
    if (!dc) return;
    RECT rc{};
    GetClientRect(list.hwnd, &rc);
    const float w = static_cast<float>(rc.right) / scale, h = static_cast<float>(rc.bottom) / scale;
    // Rises from the strip: round on the free side, square where it meets it.
    const bool down = !list_above;
    const D2D1_RECT_F card{0, 0, w, h};
    if (auto shape = RoundedShape(dc, card, down ? 0.0f : 10.0f, down ? 10.0f : 0.0f)) {
        list.brush->SetColor(pal.card);
        dc->FillGeometry(shape.Get(), list.brush.Get());
        list.brush->SetColor(Alpha(pal.line, ctx.dark ? 1.0f : 0.8f));
        dc->DrawGeometry(shape.Get(), list.brush.Get(), 1.0f / scale);
    }
    float y = kListPad;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (row.kind != Row::Kind::Item) {
            const D2D1_RECT_F r{kListPad + 10, y + 6, w - kListPad - 10, y + kGroupHeight - 2};
            list.Text(row.text, r, row.kind == Row::Kind::Note ? 12.5f : 11.5f, pal.faint);
            y += kGroupHeight;
            continue;
        }
        const D2D1_RECT_F r{kListPad, y, w - kListPad, y + kRowHeight};
        const bool on = static_cast<int>(i) == selected;
        if (on) list.Fill(r, pal.accent_soft, 7);
        else if (static_cast<int>(i) == list_hover) list.Fill(r, pal.row, 7);
        const float mid = y + kRowHeight / 2;
        const float icon_x = r.left + 10;
        if (!icons || !icons->Draw(dc, D2D1::RectF(icon_x, mid - 9, icon_x + 18, mid + 9), row.path, row.text, row.dir,
                                   row.dir ? FILE_ATTRIBUTE_DIRECTORY : 0))
            list.Fill(D2D1::RectF(icon_x + 2, mid - 6, icon_x + 16, mid + 6), row.dir ? Hex(0xE8B44A) : pal.line, 2);
        float right = r.right - 10;
        if (on) {
            const auto& act = l10n::Get(row.dir ? StringId::DialogJumpGoHere : StringId::DialogJumpFillOpen);
            const float act_w = list.Measure(act, 11.0f);
            list.Text(act, D2D1::RectF(right - act_w, y, right, y + kRowHeight), 11.0f, pal.accent);
            right -= act_w + 12;
        }
        const float name_left = icon_x + 18 + 10;
        const float name_w = std::min(list.Measure(row.text, 13.0f), std::max(60.0f, (right - name_left) * 0.62f));
        const auto marks = list_mode == ListMode::Search ? MatchRanges(row.text, query) : std::vector<DWRITE_TEXT_RANGE>();
        list.Text(row.text, D2D1::RectF(name_left, y, name_left + name_w + 1, y + kRowHeight), 13.0f, pal.text,
                  DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, &marks, pal.hit);
        // Where it is (the parent folder), keeping the end when it is long.
        const float path_left = std::max(name_left + name_w + 16, right - 250);
        if (right - path_left > 40) {
            const std::wstring where = list.Elide(Parent(row.path), 11.5f, right - path_left);
            list.Text(where, D2D1::RectF(path_left, y, right, y + kRowHeight), 11.5f, pal.faint, DWRITE_FONT_WEIGHT_NORMAL,
                      DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        y += kRowHeight;
    }
    if (!list.End() && icons) icons->Reset();
}

// --- list -----------------------------------------------------------------------

int DialogJump::Impl::FirstItem() const {
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind == Row::Kind::Item) return static_cast<int>(i);
    return -1;
}

void DialogJump::Impl::MoveSelection(int delta) {
    if (rows.empty()) return;
    const int n = static_cast<int>(rows.size());
    int at = selected < 0 ? (delta > 0 ? -1 : n) : selected;
    for (int step = 0; step < n; ++step) {
        at = (at + delta + n) % n;
        if (rows[static_cast<size_t>(at)].kind == Row::Kind::Item) {
            selected = at;
            break;
        }
    }
    InvalidateRect(list.hwnd, nullptr, FALSE);
}

void DialogJump::Impl::BuildSearchRows() {
    rows.clear();
    std::vector<const Found*> folders, files;
    for (const auto& f : known) folders.push_back(&f);
    for (const auto& f : indexed) {
        bool dup = false;
        for (const auto* k : folders) dup = dup || SamePath(k->path, f.path);
        if (dup) continue;
        if (f.dir) folders.push_back(&f);
        else if (!dlg.save && !dlg.folder_picker) files.push_back(&f);
    }
    const size_t folder_cap = files.empty() ? kMaxRows : std::max<size_t>(kMaxRows - std::min<size_t>(files.size(), 3), 3);
    if (folders.size() > folder_cap) folders.resize(folder_cap);
    if (files.size() > kMaxRows - folders.size()) files.resize(kMaxRows - folders.size());
    if (!folders.empty()) {
        rows.push_back({Row::Kind::Group, l10n::Get(StringId::DialogJumpGroupFolders), {}, true});
        for (const auto* f : folders) rows.push_back({Row::Kind::Item, f->name, f->path, true});
    }
    if (!files.empty()) {
        rows.push_back({Row::Kind::Group, l10n::Get(StringId::DialogJumpGroupFiles), {}, false});
        for (const auto* f : files) rows.push_back({Row::Kind::Item, f->name, f->path, false});
    }
    if (rows.empty())
        rows.push_back({Row::Kind::Note, l10n::Get(searching ? StringId::DialogJumpSearching : StringId::DialogJumpNoMatch), {}, true});
}

void DialogJump::Impl::BuildPlaceRows() {
    rows.clear();
    // Quick access, then the most visited of the rest, then recent ones.
    std::vector<size_t> pinned, frequent, recent;
    for (size_t i = 0; i < ctx.folders.size(); ++i) {
        const auto& folder = ctx.folders[i];
        if (folder.open) continue;
        if (folder.pinned) pinned.push_back(i);
        else if (scores[i] > 0) frequent.push_back(i);
        else if (i < history_from) recent.push_back(i);
    }
    frequent = ByScore(std::move(frequent));
    frequent.erase(std::remove_if(frequent.begin(), frequent.end(), [&](size_t i) { return !Exists(i); }), frequent.end());
    if (frequent.size() > 3) {
        // The rest of Pulse's recent folders stay under 最近使用.
        for (size_t k = 3; k < frequent.size(); ++k) if (frequent[k] < history_from) recent.push_back(frequent[k]);
        std::sort(recent.begin(), recent.end());
        frequent.resize(3);
    }
    const bool others = !frequent.empty() || !recent.empty();
    if (pinned.size() > kMaxPlaces / 2 + 1 && others) pinned.resize(kMaxPlaces / 2 + 1);
    if (pinned.size() + frequent.size() + recent.size() > kMaxPlaces)
        recent.resize(kMaxPlaces - std::min(pinned.size() + frequent.size(), kMaxPlaces));
    auto add = [&](StringId title, const std::vector<size_t>& items) {
        if (items.empty()) return;
        rows.push_back({Row::Kind::Group, l10n::Get(title), {}, true});
        for (size_t i : items) {
            const auto& folder = ctx.folders[i];
            rows.push_back({Row::Kind::Item, folder.label.empty() ? Leaf(folder.path) : folder.label, folder.path, true});
        }
    };
    add(StringId::DialogJumpPinned, pinned);
    add(StringId::DialogJumpFrequent, frequent);
    add(StringId::DialogJumpRecent, recent);
    if (rows.empty()) rows.push_back({Row::Kind::Note, l10n::Get(StringId::DialogJumpNoMatch), {}, true});
}

// The empty search box with the focus: where this program's dialogs usually
// go. False (and nothing shown) when there is no history.
bool DialogJump::Impl::ShowFrequent() {
    if (!ctx.remember || !dlg.dialog || !edit_focused || GetFocus() != edit) return false;
    std::vector<size_t> visited;
    for (size_t i = 0; i < ctx.folders.size(); ++i)
        if (scores[i] > 0) visited.push_back(i);
    std::vector<Row> items;
    for (size_t i : ByScore(std::move(visited))) {
        if (items.size() >= kMaxRows) break;
        if (!Exists(i)) continue;
        const auto& folder = ctx.folders[i];
        items.push_back({Row::Kind::Item, folder.label.empty() ? Leaf(folder.path) : folder.label, folder.path, true});
    }
    if (items.empty()) return false;
    rows.clear();
    rows.push_back({Row::Kind::Group, l10n::Get(StringId::DialogJumpFrequent), {}, true});
    rows.insert(rows.end(), items.begin(), items.end());
    list_mode = ListMode::Search;
    selected = -1;  // Enter in an empty box does nothing until a row is chosen
    ShowList();
    return true;
}

void DialogJump::Impl::ShowList() {
    if (list_mode == ListMode::Hidden || !dlg.dialog) {
        HideList();
        return;
    }
    const float w_dip = std::min(kListWidth, width);
    const int w = static_cast<int>(std::lround(w_dip * scale));
    const int h = static_cast<int>(std::lround(ListHeight() * scale));
    const int gap = static_cast<int>(std::lround(kListGap * scale));
    int x = strip_px.left;
    if (list_mode == ListMode::Places) x = std::max(static_cast<int>(strip_px.left), static_cast<int>(strip_px.left + std::lround(more_rect.right * scale)) - w);
    bool up = placement == Placement::Above;
    int y = up ? strip_px.top - gap - h : strip_px.bottom + gap;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&strip_px, MONITOR_DEFAULTTONEAREST), &mi);
    RECT frame{};
    GetWindowRect(dlg.dialog, &frame);
    // Prefer the outside of the dialog. If that edge is full, try either
    // side before falling back to an on-screen, explicitly opened popup.
    if (y < mi.rcWork.top || y + h > mi.rcWork.bottom) {
        if (frame.right + w <= mi.rcWork.right) x = frame.right;
        else if (frame.left - w >= mi.rcWork.left) x = frame.left - w;
        else {
            up = strip_px.top - mi.rcWork.top > mi.rcWork.bottom - strip_px.bottom;
            y = up ? strip_px.top - gap - h : strip_px.bottom + gap;
        }
    }
    x = std::clamp(x, static_cast<int>(mi.rcWork.left),
                   std::max(static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - w));
    y = std::clamp(y, static_cast<int>(mi.rcWork.top),
                   std::max(static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - h));
    list_above = up;
    SetWindowPos(list.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    const RECT shape{x, y, x + w, y + h};
    list_shadow.Show(list.hwnd, shape, scale, up ? 10.0f : 0.0f, up ? 0.0f : 10.0f, up ? -0.8f : 0.8f, !up, up,
                     pal.shadow);
    InvalidateRect(list.hwnd, nullptr, FALSE);
    UpdateWindow(list.hwnd);
    InvalidateRect(bar.hwnd, nullptr, FALSE);
}

void DialogJump::Impl::HideList() {
    if (list.hwnd && IsWindowVisible(list.hwnd)) ShowWindow(list.hwnd, SW_HIDE);
    list_shadow.Hide();
    if (list_mode == ListMode::Places) InvalidateRect(bar.hwnd, nullptr, FALSE);
    list_mode = ListMode::Hidden;
    list_hover = -1;
    KillTimer(list.hwnd, 1);
}

// --- search ---------------------------------------------------------------------

void DialogJump::Impl::OnSearchChanged() {
    wchar_t buffer[512]{};
    if (edit) GetWindowTextW(edit, buffer, 512);
    query = buffer;
    while (!query.empty() && query.back() == L' ') query.pop_back();
    known.clear();
    indexed.clear();
    KillTimer(bar.hwnd, kTimerSearch);
    KillTimer(bar.hwnd, kTimerGiveUp);
    if (index_started) index_client.CancelSession(kSession);
    searching = false;
    if (!edit_focused || GetFocus() != edit) {
        HideList();
        return;
    }
    if (query.empty()) {
        if (!ShowFrequent() && list_mode == ListMode::Search) HideList();
        return;
    }
    // Equal matches rank in list order, so the most visited go in first.
    std::vector<size_t> order(ctx.folders.size());
    std::iota(order.begin(), order.end(), size_t{0});
    order = ByScore(std::move(order));
    std::vector<JumpCandidate> candidates;
    candidates.reserve(order.size());
    for (size_t i : order) candidates.push_back({ctx.folders[i].path, ctx.folders[i].label});
    for (size_t ranked : RankJumpFolders(candidates, query, ctx.pinyin, 8)) {
        if (known.size() >= 5) break;
        const size_t index = order[ranked];
        if (!Exists(index)) continue;
        const auto& folder = ctx.folders[index];
        known.push_back({folder.path, folder.label.empty() ? Leaf(folder.path) : folder.label, true});
    }
    if (ctx.index) {
        searching = true;
        SetTimer(bar.hwnd, kTimerSearch, 90, nullptr);
    }
    list_mode = ListMode::Search;
    BuildSearchRows();
    selected = FirstItem();
    ShowList();
}

void DialogJump::Impl::StartIndexSearch() {
    KillTimer(bar.hwnd, kTimerSearch);
    if (query.empty()) return;
    if (!index_started) {
        index_client.Start(bar.hwnd, kMsgIndexStatus, kMsgIndexResult);
        index_started = true;
    }
    index::Query q;
    q.session_id = kSession;
    q.needle = ctx.pinyin ? query : L"nopinyin: " + query;
    q.folders_only = dlg.save || dlg.folder_picker;
    q.limit = 24;
    index_client.SearchAsync(q, ++search_id);
    SetTimer(bar.hwnd, kTimerGiveUp, 4000, nullptr);  // no index answer: stop saying "searching"
}

void DialogJump::Impl::OnIndexResult(uint32_t id) {
    index::SearchResult result;
    if (!index_client.TakeResult(id, result) || id != search_id || list_mode != ListMode::Search || query.empty()) return;
    KillTimer(bar.hwnd, kTimerGiveUp);
    searching = false;
    indexed.clear();
    for (const auto& hit : result.hits)
        indexed.push_back({PlainPath(hit.path), hit.name.empty() ? Leaf(hit.path) : hit.name, hit.is_dir});
    const std::wstring kept = selected >= 0 && static_cast<size_t>(selected) < rows.size() ? rows[static_cast<size_t>(selected)].path : std::wstring();
    BuildSearchRows();
    selected = FirstItem();
    for (size_t i = 0; i < rows.size() && !kept.empty(); ++i)
        if (rows[i].kind == Row::Kind::Item && SamePath(rows[i].path, kept)) selected = static_cast<int>(i);
    ShowList();
}

void DialogJump::Impl::Pick(size_t row) {
    if (row >= rows.size() || rows[row].kind != Row::Kind::Item) return;
    const Row picked = rows[row];
    if (edit) SetWindowTextW(edit, L"");
    Navigate(picked.path, picked.dir);
}

// --- driving the dialog -----------------------------------------------------------

void DialogJump::Impl::ReturnToDialog() {
    if (dlg.dialog && GetForegroundWindow() == bar.hwnd) SetForegroundWindow(dlg.dialog);
}

void DialogJump::Impl::Navigate(const std::wstring& path, bool dir, bool record) {
    HideList();
    if (path.empty() || !dlg.dialog || !IsWindow(dlg.dialog)) return;
    auto_pending = false;  // the user went somewhere first
    if (record && ctx.remember && history) history->Record(dir ? path : Parent(path), dlg_app);
    KillTimer(bar.hwnd, kTimerRestore);
    if (dir && (dlg.modern || dlg.folder_picker || !dlg.name_edit)) {
        NavigateAddressBar(path);
        return;
    }
    // The file name box: a folder path there plus OK moves the dialog; the
    // name that was typed is put back once it has.
    const std::wstring typed = TextOf(dlg.name_edit);
    set_text = dir ? FolderInput(path) : path;
    if (!SetText(dlg.name_edit, set_text)) return;
    PostMessageW(dlg.dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(dlg.ok));
    restore_text = dir && typed.find_first_of(L"\\/:") == std::wstring::npos ? typed : std::wstring();
    restore_edit = dlg.name_edit;
    restore_ticks = 0;
    if (dir) SetTimer(bar.hwnd, kTimerRestore, 90, nullptr);
    ReturnToDialog();
}

static bool ModifiersDown() {
    for (int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN})
        if (GetAsyncKeyState(key) & 0x8000) return true;
    return false;
}

void DialogJump::Impl::NavigateAddressBar(const std::wstring& path) {
    // Wait for Ctrl+G's modifiers to be released, then activate the native
    // address edit without changing the file name or pressing Save/Open.
    if (GetForegroundWindow() != dlg.dialog) SetForegroundWindow(dlg.dialog);
    address_path = path;
    address_ticks = 0;
    address_keys_sent = false;
    SetTimer(bar.hwnd, kTimerAddress, 30, nullptr);
}

void DialogJump::Impl::SendAddressKeys() {
    address_keys_sent = true;
    if (AddressControl(dlg.dialog, true)) return;
    if (const HWND breadcrumb = AddressControl(dlg.dialog, false)) {
        RECT rc{};
        GetClientRect(breadcrumb, &rc);
        const LPARAM point = MAKELPARAM(std::max(0L, rc.right - 2), (rc.bottom - rc.top) / 2);
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(breadcrumb, WM_LBUTTONDOWN, MK_LBUTTON, point, SMTO_ABORTIFHUNG, 300, &ignored);
        SendMessageTimeoutW(breadcrumb, WM_LBUTTONUP, 0, point, SMTO_ABORTIFHUNG, 300, &ignored);
        if (AddressControl(dlg.dialog, true)) return;
    }
    // Older common dialogs may not expose the breadcrumb control.
    INPUT keys[4]{};
    for (auto& key : keys) key.type = INPUT_KEYBOARD;
    keys[0].ki.wVk = VK_MENU;
    keys[1].ki.wVk = 'D';
    keys[2].ki.wVk = 'D';
    keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
    keys[3].ki.wVk = VK_MENU;
    keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
    if (GetForegroundWindow() == dlg.dialog) SendInput(4, keys, sizeof(INPUT));
}

void DialogJump::Impl::FocusEdit() {
    if (!showing || !dlg.dialog || !edit) return;
    const HWND foreground = GetForegroundWindow();
    const DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD self = GetCurrentThreadId();
    const bool attach = foreground_thread && foreground_thread != self &&
                        AttachThreadInput(self, foreground_thread, TRUE) != FALSE;
    SetForegroundWindow(bar.hwnd);
    SetFocus(edit);
    if (attach) AttachThreadInput(self, foreground_thread, FALSE);
    SendMessageW(edit, EM_SETSEL, 0, -1);
}

void DialogJump::Impl::OnEditFocus(bool focused) {
    if (focused == edit_focused) return;
    edit_focused = focused;
    UpdateFieldBrush();
    // The box widens while it has the focus (150 ms, eased).
    query_from = query_width;
    query_to = focused ? kQueryFocusWidth : kQueryWidth;
    anim_start = GetTickCount64();
    SetTimer(bar.hwnd, kTimerAnim, 10, nullptr);
    if (!focused && list_mode == ListMode::Search) HideList();
    if (focused && list_mode == ListMode::Hidden) {
        if (!query.empty()) OnSearchChanged();
        else ShowFrequent();
    }
    if (bar.comp && edit_format) ui::PresentChildEdit(*bar.comp, edit_format.Get(), pal.text, FieldColor(), edit);
    InvalidateRect(bar.hwnd, nullptr, FALSE);
}

bool DialogJump::Impl::EditKey(WPARAM key) {
    switch (key) {
    case VK_DOWN:
    case VK_UP:
        if (list_mode == ListMode::Hidden) {
            if (!query.empty()) OnSearchChanged();
            else ShowFrequent();
        } else {
            MoveSelection(key == VK_DOWN ? 1 : -1);
        }
        return true;
    case VK_RETURN:
        if (list_mode == ListMode::Search && selected >= 0) Pick(static_cast<size_t>(selected));
        return true;
    case VK_ESCAPE:
        if (list_mode != ListMode::Hidden) HideList();
        else if (GetWindowTextLengthW(edit) > 0) SetWindowTextW(edit, L"");
        else ReturnToDialog();
        return true;
    default: return false;
    }
}

// --- window procedures --------------------------------------------------------

LRESULT CALLBACK DialogJump::Impl::BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return t_impl ? t_impl->HandleBar(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}
LRESULT CALLBACK DialogJump::Impl::ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return t_impl ? t_impl->HandleList(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DialogJump::Impl::EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<Impl*>(data);
    if (msg == WM_KEYDOWN && self->EditKey(wp)) return 0;
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == L'\n')) return 0;  // no beep
    if (!self->bar.comp || !self->edit_format) return DefSubclassProc(hwnd, msg, wp, lp);
    LRESULT result = 0;
    if (!ui::HandleChildEditMessage(*self->bar.comp, self->edit_format.Get(), self->pal.text, self->FieldColor(),
                                    ui::EditBackBrush(self->field_brush), hwnd, msg, wp, lp, result))
        result = ui::DefPresentedChildEditProc(*self->bar.comp, self->edit_format.Get(), self->pal.text,
                                               self->FieldColor(), hwnd, msg, wp, lp);
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) self->OnEditFocus(msg == WM_SETFOCUS);
    return result;
}

LRESULT DialogJump::Impl::HandleBar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto point = [&](float& x, float& y) {
        x = static_cast<float>(GET_X_LPARAM(lp)) / scale;
        y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
    };
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        PaintBar();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE: {
        // Only the search box takes the focus; the rest works without it.
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        const float x = static_cast<float>(pt.x) / scale, y = static_cast<float>(pt.y) / scale;
        return Contains(Inflate(query_rect, 3), x, y) ? MA_ACTIVATE : MA_NOACTIVATE;
    }
    case WM_MOUSEMOVE: {
        float x, y;
        point(x, y);
        Hot hit = Hot::None;
        size_t chip_index = 0;
        for (size_t i = 0; i < chips.size(); ++i)
            if (Contains(chips[i].rect, x, y)) {
                hit = Hot::Chip;
                chip_index = i;
            }
        if (Contains(keys_rect, x, y)) hit = Hot::Keys;
        if (Contains(more_rect, x, y)) hit = Hot::More;
        if (Contains(query_rect, x, y)) hit = Hot::Query;
        if (hit != hover || chip_index != hover_chip) {
            hover = hit;
            hover_chip = chip_index;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (!tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            tracking = TrackMouseEvent(&tme) != FALSE;
        }
        SetCursor(LoadCursorW(nullptr, hit == Hot::Query ? IDC_IBEAM : hit == Hot::None ? IDC_ARROW : IDC_HAND));
        return 0;
    }
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wp) == hwnd) return TRUE;  // WM_MOUSEMOVE sets it
        break;
    case WM_MOUSELEAVE:
        tracking = false;
        if (hover != Hot::None) {
            hover = Hot::None;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        float x, y;
        point(x, y);
        if (Contains(Inflate(query_rect, 3), x, y)) FocusEdit();
        return 0;
    }
    case WM_LBUTTONUP: {
        float x, y;
        point(x, y);
        for (const auto& chip : chips)
            if (Contains(chip.rect, x, y)) {
                Navigate(ctx.folders[chip.folder].path, true);
                return 0;
            }
        if (Contains(keys_rect, x, y)) {
            Navigate(ctx.current, true);
        } else if (Contains(more_rect, x, y)) {
            if (list_mode == ListMode::Places) {
                HideList();
            } else {
                list_mode = ListMode::Places;
                BuildPlaceRows();
                selected = -1;
                ShowList();
                SetTimer(list.hwnd, 1, 50, nullptr);  // closes on a click elsewhere
            }
        }
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        const HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, ui::HcEditText(ToColorRef(pal.text)));
        SetBkColor(dc, ToColorRef(FieldColor()));
        return reinterpret_cast<LRESULT>(ui::EditBackBrush(field_brush));
    }
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lp) == edit && HIWORD(wp) == EN_CHANGE) OnSearchChanged();
        return 0;
    case WM_HOTKEY:
        if (wp == kHotkeyId && !ctx.current.empty()) Navigate(ctx.current, true);
        return 0;
    case WM_TIMER:
        if (wp == kTimerForeground) {
            const HWND foreground = GetForegroundWindow();
            if (foreground && (foreground != observed_foreground ||
                (foreground == dlg.dialog && !IsWindowVisible(bar.hwnd)) ||
                (!dlg.dialog && ClassOf(foreground) == L"#32770"))) Evaluate(foreground);
        } else if (wp == kTimerAnim) {
            const float t = std::min(1.0f, static_cast<float>(GetTickCount64() - anim_start) / 150.0f);
            const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
            query_width = query_from + (query_to - query_from) * eased;
            if (t >= 1.0f) KillTimer(hwnd, kTimerAnim);
            Layout();
            PaintBar();
            ValidateRect(hwnd, nullptr);
        } else if (wp == kTimerAuto) {
            // Only once per dialog, and only while nothing was picked yet.
            KillTimer(hwnd, kTimerAuto);
            const bool go = auto_pending && ctx.auto_jump && showing && !ctx.current.empty();
            auto_pending = false;
            if (go) Navigate(ctx.current, true, false);
        } else if (wp == kTimerSearch) {
            StartIndexSearch();
        } else if (wp == kTimerGiveUp) {
            KillTimer(hwnd, kTimerGiveUp);
            if (searching && list_mode == ListMode::Search) {
                searching = false;
                const int kept = selected;
                BuildSearchRows();
                selected = kept >= 0 && static_cast<size_t>(kept) < rows.size() &&
                                   rows[static_cast<size_t>(kept)].kind == Row::Kind::Item
                               ? kept
                               : FirstItem();
                ShowList();
            }
        } else if (wp == kTimerProbe) {
            DialogInfo info;
            if (GetForegroundWindow() == probe_target && ProbeDialog(probe_target, info)) Attach(info);
            else if (++probe_tries >= 30 || !IsWindow(probe_target)) KillTimer(hwnd, kTimerProbe);
        } else if (wp == kTimerRestore) {
            // Put the typed name back once the dialog has taken the path.
            const std::wstring now = IsWindow(restore_edit) ? TextOf(restore_edit) : std::wstring();
            if (!IsWindow(restore_edit) || (!now.empty() && now != set_text)) {
                KillTimer(hwnd, kTimerRestore);
            } else if (++restore_ticks >= 2) {
                KillTimer(hwnd, kTimerRestore);
                SetText(restore_edit, restore_text);
                if (!restore_text.empty()) {
                    DWORD_PTR ignored = 0;
                    SendMessageTimeoutW(restore_edit, EM_SETSEL, 0, -1, SMTO_ABORTIFHUNG, 300, &ignored);
                }
            }
        } else if (wp == kTimerAddress) {
            if (!address_keys_sent) {
                if (ModifiersDown() && ++address_ticks < 40) return 0;
                address_ticks = 0;
                SendAddressKeys();
                return 0;
            }
            GUITHREADINFO gti{sizeof(gti)};
            HWND focus = AddressControl(dlg.dialog, true);
            if (!focus && !dlg.modern && dlg.dialog && GetGUIThreadInfo(dlg.tid, &gti)) focus = gti.hwndFocus;
            if (focus && focus != dlg.name_edit && IsChild(dlg.dialog, focus) && ClassOf(focus) == L"Edit") {
                KillTimer(hwnd, kTimerAddress);
                if (SetText(focus, address_path)) {
                    // Deliver Enter to this address edit, never to a Save button
                    // or another application's newly focused window.
                    PostMessageW(focus, WM_KEYDOWN, VK_RETURN, 1);
                    PostMessageW(focus, WM_KEYUP, VK_RETURN, static_cast<LPARAM>(0xC0000001));
                }
            } else if (++address_ticks > 12) {
                KillTimer(hwnd, kTimerAddress);
            }
        }
        return 0;
    case kMsgContext: {
        // A new dialog's context is in: auto-jump shortly, once the dialog
        // has settled on its own first folder.
        if (auto_pending) SetTimer(hwnd, kTimerAuto, 150, nullptr);
        const bool dark = ctx.dark;
        const D2D1_COLOR_F accent = ctx.accent;
        TakeContext();
        UpdatePalette();
        if (showing) {
            if (dark != ctx.dark || std::memcmp(&accent, &ctx.accent, sizeof(accent)) != 0) Place();
            Layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (list_mode == ListMode::Search) OnSearchChanged();
            else if (list_mode == ListMode::Places) {
                BuildPlaceRows();
                ShowList();
            }
        }
        return 0;
    }
    case kMsgFocus:
        FocusEdit();
        return 0;
    case kMsgIndexResult:
        OnIndexResult(static_cast<uint32_t>(wp));
        return 0;
    case kMsgIndexStatus:
        return 0;
    case WM_DPICHANGED:
        return 0;  // Place() sizes from the dialog's monitor
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT DialogJump::Impl::HandleList(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto row_at = [&](LPARAM at) -> int {
        float y = static_cast<float>(GET_Y_LPARAM(at)) / scale - kListPad;
        for (size_t i = 0; i < rows.size(); ++i) {
            const float h = rows[i].kind == Row::Kind::Item ? kRowHeight : kGroupHeight;
            if (y >= 0 && y < h) return rows[i].kind == Row::Kind::Item ? static_cast<int>(i) : -1;
            y -= h;
        }
        return -1;
    };
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        PaintList();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        const int hovered = row_at(lp);
        if (hovered != list_hover) {
            list_hover = hovered;
            if (list_mode == ListMode::Places && hovered >= 0) selected = hovered;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (!list_tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            list_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        SetCursor(LoadCursorW(nullptr, hovered >= 0 ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wp) == hwnd) return TRUE;
        break;
    case WM_MOUSELEAVE:
        list_tracking = false;
        if (list_hover >= 0) {
            list_hover = -1;
            if (list_mode == ListMode::Places) selected = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        const int row = row_at(lp);
        if (row >= 0) Pick(static_cast<size_t>(row));
        return 0;
    }
    case WM_TIMER:
        if (wp == 1 && list_mode == ListMode::Places) {
            // The places list closes on a click anywhere else.
            const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
            POINT pt{};
            GetCursorPos(&pt);
            const HWND under = WindowFromPoint(pt);
            if (down && under != hwnd && under != bar.hwnd) HideList();
        }
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --- public -------------------------------------------------------------------

DialogJump::DialogJump() : impl_(std::make_shared<Impl>()) {}
DialogJump::~DialogJump() { Stop(); }

void DialogJump::Update(HWND service, UINT context_message, bool enabled) {
    impl_->service = service;
    impl_->context_message = context_message;
    if (!enabled) {
        Stop();
        return;
    }
    if (thread_.joinable()) return;
    impl_->ready = false;
    thread_ = std::thread([impl = impl_] { impl->Run(); });
}

void DialogJump::Stop() {
    if (!thread_.joinable()) return;
    // Run() may still be creating its windows.
    for (int i = 0; i < 200 && !impl_->ready.load(); ++i) Sleep(5);
    PostThreadMessageW(GetThreadId(thread_.native_handle()), kMsgStop, 0, 0);
    thread_.join();
    impl_->showing = false;
}

void DialogJump::SetHistory(std::shared_ptr<JumpHistory> history) {
    std::lock_guard lock(impl_->mu);
    impl_->shared_history = std::move(history);
}

void DialogJump::SetContext(DialogJumpContext context) {
    {
        std::lock_guard lock(impl_->mu);
        impl_->shared_context = std::move(context);
    }
    if (const HWND bar = impl_->bar_window.load()) PostMessageW(bar, kMsgContext, 0, 0);
}

bool DialogJump::FocusSearch() {
    const HWND bar = impl_->bar_window.load();
    if (!bar || !impl_->showing.load()) return false;
    PostMessageW(bar, kMsgFocus, 0, 0);
    return true;
}
} // namespace pulse::app

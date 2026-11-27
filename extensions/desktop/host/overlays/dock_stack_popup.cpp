// dock_stack_popup.cpp - Dock stacks (叠放): Fan / Grid / List popups for Dock folders and the
// 应用程序 stack. A layered top-level window painted with Direct2D into a DIB (UpdateLayeredWindow),
// so the Fan can float its icons and name pills straight over the desktop with no panel behind.
#include "host/overlays/dock_stack_popup.h"

#include "../../../../src/common/localization.h"

#include <d2d1.h>
#include <dwrite.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>

namespace pulse::desktop::overlays {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kClassName[] = L"PulseDockStackPopup";
constexpr UINT_PTR kTimerFrame = 1;
constexpr UINT_PTR kTimerWatch = 2;
constexpr float kFanRadius = 1250.0f;   // DIP; the arc the fan's items rise along
constexpr int kFanMaxFiles = 14;
constexpr float kFanOpenMs = 300.0f;
constexpr float kFanCascadeMs = 16.0f;
constexpr float kFanCloseMs = 170.0f;
constexpr float kPanelOpenMs = 200.0f;
constexpr float kPanelCloseMs = 140.0f;
constexpr float kGridCellW = 96.0f, kGridCellH = 100.0f, kGridIcon = 56.0f;
constexpr float kListW = 300.0f, kListRow = 32.0f, kListIcon = 22.0f;
constexpr float kHeader = 44.0f, kFooter = 36.0f, kPad = 12.0f, kShadow = 22.0f, kArrow = 10.0f;
constexpr int kHitNone = -1, kHitOpenFolder = -2, kHitBack = -3;

std::wstring OpenInPulseText() { return std::wstring(l10n::Get(l10n::StringId::PickerPulseOpenPlaces)); }

float Clamp01(float v) noexcept { return std::clamp(v, 0.0f, 1.0f); }
float EaseOutCubic(float t) noexcept { t = Clamp01(t); const float u = 1.0f - t; return 1.0f - u * u * u; }
float EaseInCubic(float t) noexcept { t = Clamp01(t); return t * t * t; }
// A small overshoot, so each fan item settles onto the arc like the Mac's spring.
float EaseOutBack(float t, float k = 1.15f) noexcept {
    t = Clamp01(t);
    const float u = t - 1.0f;
    return 1.0f + (k + 1.0f) * u * u * u + k * u * u;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool EndsWithI(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

std::wstring Extension(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return {};
    return Lower(name.substr(dot + 1));
}

uint64_t Ticks(const FILETIME& ft) noexcept {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

double NowMs() noexcept {
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

std::vector<DockStackEntry> EnumerateFolder(const std::wstring& folder, DockStackSort sort) {
    std::vector<DockStackEntry> out;
    if (folder.empty()) return out;
    std::wstring pattern = folder;
    if (pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
    const std::wstring base = pattern;
    pattern += L'*';
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        const std::wstring lower = Lower(name);
        if (lower == L"desktop.ini" || lower == L"thumbs.db") continue;
        DockStackEntry e;
        e.path = base + name;
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.name = DockStackDisplayName(name, e.is_dir);
        e.added = Ticks(fd.ftCreationTime);
        e.modified = Ticks(fd.ftLastWriteTime);
        out.push_back(std::move(e));
    } while (out.size() < 4000 && FindNextFileW(h, &fd));
    FindClose(h);
    SortDockStackEntries(out, sort);
    return out;
}

// Every app of the Start menu's 所有应用 list (desktop and Store apps), from shell:AppsFolder.
std::vector<DockStackEntry> EnumerateApps() {
    static std::vector<DockStackEntry> cache;
    static ULONGLONG cache_tick = 0;
    const ULONGLONG now = GetTickCount64();
    if (!cache.empty() && now - cache_tick < 60000) return cache;
    std::vector<DockStackEntry> out;
    ComPtr<IShellItem> apps;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&apps))) || !apps) {
        return out;
    }
    ComPtr<IEnumShellItems> items;
    if (FAILED(apps->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items))) || !items) return out;
    std::unordered_set<std::wstring> seen;
    for (;;) {
        ComPtr<IShellItem> item;
        ULONG fetched = 0;
        if (items->Next(1, &item, &fetched) != S_OK || !item) break;
        PWSTR display = nullptr, parsing = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display)) && display &&
            SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &parsing)) && parsing) {
            const std::wstring name = display;
            if (!name.empty() && !IsDockStackAppNoise(name) && seen.insert(Lower(name)).second) {
                DockStackEntry e;
                e.name = name;
                e.path = std::wstring(L"shell:AppsFolder\\") + parsing;
                e.is_app = true;
                out.push_back(std::move(e));
            }
        }
        if (display) CoTaskMemFree(display);
        if (parsing) CoTaskMemFree(parsing);
        if (out.size() >= 1500) break;
    }
    SortDockStackEntries(out, DockStackSort::Name);
    cache = out;
    cache_tick = now;
    return out;
}

// Converts a shell image (straight or premultiplied, or alpha-less thumbnails) to a premultiplied
// D2D bitmap on `rt`.
ComPtr<ID2D1Bitmap> BitmapFromHbitmap(ID2D1RenderTarget* rt, HBITMAP hbm) {
    if (!rt || !hbm) return {};
    BITMAP bm{};
    if (!GetObjectW(hbm, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0) return {};
    const int w = bm.bmWidth, h = std::abs(bm.bmHeight);
    std::vector<uint32_t> px(static_cast<size_t>(w) * h);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    const int lines = GetDIBits(dc, hbm, 0, static_cast<UINT>(h), px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines != h) return {};
    bool any_alpha = false, straight = false;
    for (uint32_t p : px) {
        const uint32_t a = p >> 24;
        if (a) any_alpha = true;
        if (((p >> 16) & 0xFF) > a || ((p >> 8) & 0xFF) > a || (p & 0xFF) > a) straight = true;
    }
    for (auto& p : px) {
        if (!any_alpha) { p |= 0xFF000000u; continue; }
        if (!straight) continue;
        const uint32_t a = p >> 24;
        const uint32_t r = ((p >> 16) & 0xFF) * a / 255, g = ((p >> 8) & 0xFF) * a / 255, b = (p & 0xFF) * a / 255;
        p = (a << 24) | (r << 16) | (g << 8) | b;
    }
    ComPtr<ID2D1Bitmap> out;
    rt->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), px.data(),
                     static_cast<UINT32>(w * 4),
                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                     &out);
    return out;
}

bool IsImageExtension(const std::wstring& ext) {
    for (const wchar_t* e : {L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"heic", L"tif", L"tiff", L"ico"}) {
        if (ext == e) return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------------------------

void SortDockStackEntries(std::vector<DockStackEntry>& entries, DockStackSort sort) {
    auto by_name = [](const DockStackEntry& a, const DockStackEntry& b) {
        return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
    };
    switch (sort) {
    case DockStackSort::Name:
        std::stable_sort(entries.begin(), entries.end(), by_name);
        break;
    case DockStackSort::DateModified:
        std::stable_sort(entries.begin(), entries.end(), [&](const DockStackEntry& a, const DockStackEntry& b) {
            return a.modified != b.modified ? a.modified > b.modified : by_name(a, b);
        });
        break;
    case DockStackSort::Kind:
        std::stable_sort(entries.begin(), entries.end(), [&](const DockStackEntry& a, const DockStackEntry& b) {
            if (a.is_dir != b.is_dir) return a.is_dir;
            const std::wstring ea = Extension(a.path), eb = Extension(b.path);
            return ea != eb ? ea < eb : by_name(a, b);
        });
        break;
    case DockStackSort::DateAdded:
    default:
        std::stable_sort(entries.begin(), entries.end(), [&](const DockStackEntry& a, const DockStackEntry& b) {
            const uint64_t ta = std::max(a.added, a.modified), tb = std::max(b.added, b.modified);
            return ta != tb ? ta > tb : by_name(a, b);
        });
        break;
    }
}

std::wstring DockStackDisplayName(const std::wstring& file_name, bool is_dir) {
    if (is_dir) return file_name;
    for (const wchar_t* hidden : {L".lnk", L".url", L".appref-ms"}) {
        if (EndsWithI(file_name, hidden) && file_name.size() > wcslen(hidden)) {
            return file_name.substr(0, file_name.size() - wcslen(hidden));
        }
    }
    return file_name;
}

std::vector<DockStackEntry> ListDockStackFolder(const std::wstring& folder, DockStackSort sort) {
    return EnumerateFolder(folder, sort);
}

bool IsDockStackAppNoise(const std::wstring& name) {
    const std::wstring l = Lower(name);
    for (const wchar_t* k : {L"uninstall", L"卸载", L"readme", L"read me", L"release notes", L"help", L"帮助",
                             L"documentation", L"website", L"官方网站", L"manual", L"license"}) {
        if (l.find(k) != std::wstring::npos) return true;
    }
    return false;
}

DockFanPoint DockFanItemCenter(int i) noexcept {
    const float d = 10.0f + kDockFanIcon * 0.5f + kDockFanStep * static_cast<float>(std::max(0, i));
    const float theta = d / kFanRadius;
    return {kFanRadius * (1.0f - std::cos(theta)), -kFanRadius * std::sin(theta)};
}

int DockFanCapacity(float room_dip) noexcept {
    // Rows whose centre plus half an icon and a 12 DIP margin stay inside the room, less the
    // 在 Pulse 中打开 row on top.
    int rows = 0;
    while (rows < kFanMaxFiles + 1 && -DockFanItemCenter(rows).y + kDockFanIcon * 0.5f + 12.0f <= room_dip) ++rows;
    return std::max(0, rows - 1);
}

DockStackView ResolveDockStackView(DockStackView requested, bool is_apps, uint8_t dock_edge,
                                   size_t item_count, int fan_capacity) noexcept {
    if (is_apps) return DockStackView::Grid;
    const bool fan_possible = dock_edge == 0 && fan_capacity >= 3;
    switch (requested) {
    case DockStackView::Fan: return fan_possible ? DockStackView::Fan : DockStackView::Grid;
    case DockStackView::Grid: return DockStackView::Grid;
    case DockStackView::List: return DockStackView::List;
    case DockStackView::Automatic:
    default:
        return (fan_possible && item_count > 0) ? DockStackView::Fan : DockStackView::Grid;
    }
}

// ---------------------------------------------------------------------------------------------
// Popup
// ---------------------------------------------------------------------------------------------

struct DockStackPopup::Impl {
    HWND hwnd = nullptr;
    OpenFn open_fn;
    DockStackRequest req;
    DockStackView view = DockStackView::Grid;
    std::vector<std::wstring> nav;           // grid/list subfolders opened inside the stack
    std::vector<DockStackEntry> entries;     // the current folder's items (sorted)
    int shown = 0;                           // items laid out (fan: newest that fit)
    bool open = false, closing = false;
    double anim_start = 0.0;
    float forced_progress = -1.0f;
    int hover = kHitNone, pressed = kHitNone;
    float scroll = 0.0f, scroll_max = 0.0f;
    bool had_foreground = false;
    bool mouse_was_down = true;
    std::wstring closed_key;
    ULONGLONG closed_tick = 0;

    // Layout, window-relative DIP.
    float win_w = 0, win_h = 0;           // DIP
    int px_x = 0, px_y = 0, px_w = 0, px_h = 0;
    D2D1_POINT_2F fan_origin{};           // top centre of the Dock icon
    std::vector<float> label_w;           // fan label widths (index shown = open item)
    D2D1_RECT_F panel{};                  // grid/list panel
    D2D1_POINT_2F arrow_tip{};
    bool has_arrow = false;
    int cols = 1, rows_visible = 1;

    // Rendering.
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDWriteTextFormat> fmt_label, fmt_caption, fmt_title, fmt_link, fmt_empty;
    ComPtr<IDWriteInlineObject> ellipsis_label, ellipsis_caption;
    ComPtr<ID2D1DCRenderTarget> rt;
    HDC mem_dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ old_bmp = nullptr;
    void* dib_bits = nullptr;
    int dib_w = 0, dib_h = 0;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> icons;
    std::unordered_set<std::wstring> icon_failed;

    ~Impl() {
        ReleaseSurface();
        if (hwnd && IsWindow(hwnd)) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            DestroyWindow(hwnd);
        }
    }

    bool EnsureFactories() {
        if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
        if (!dwrite && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                  reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) {
            return false;
        }
        auto make = [&](float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align,
                        ComPtr<IDWriteTextFormat>& f, ComPtr<IDWriteInlineObject>* ellipsis) {
            if (f) return;
            if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &f)) || !f) return;
            f->SetTextAlignment(align);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (ellipsis && SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(f.Get(), ellipsis->GetAddressOf()))) {
                const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                f->SetTrimming(&trim, ellipsis->Get());
            }
        };
        make(13.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, fmt_label, &ellipsis_label);
        make(12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, fmt_caption, &ellipsis_caption);
        make(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, fmt_title, nullptr);
        make(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, fmt_link, nullptr);
        make(13.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, fmt_empty, nullptr);
        return fmt_label && fmt_caption && fmt_title && fmt_link && fmt_empty;
    }

    float TextWidth(const std::wstring& s, IDWriteTextFormat* f) const {
        ComPtr<IDWriteTextLayout> layout;
        if (!dwrite || !f || FAILED(dwrite->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 2000.0f,
                                                             40.0f, &layout)) || !layout) {
            return static_cast<float>(s.size()) * 7.0f;
        }
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }

    std::wstring CurrentFolder() const { return nav.empty() ? req.folder : nav.back(); }

    std::wstring CurrentTitle() const {
        if (nav.empty()) return req.title;
        const std::wstring& p = nav.back();
        const size_t slash = p.find_last_of(L"\\/");
        return slash == std::wstring::npos ? p : p.substr(slash + 1);
    }

    bool EnsureWindow() {
        if (hwnd) return true;
        static ATOM atom = 0;
        if (!atom) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = &Impl::WndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kClassName;
            atom = RegisterClassExW(&wc);
            if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        }
        hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP,
                               0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        return hwnd != nullptr;
    }

    void ReleaseSurface() {
        rt.Reset();
        icons.clear();
        icon_failed.clear();
        if (mem_dc) {
            if (old_bmp) SelectObject(mem_dc, old_bmp);
            DeleteDC(mem_dc);
            mem_dc = nullptr;
        }
        if (dib) {
            DeleteObject(dib);
            dib = nullptr;
        }
        old_bmp = nullptr;
        dib_bits = nullptr;
        dib_w = dib_h = 0;
    }

    bool EnsureSurface() {
        if (px_w <= 0 || px_h <= 0) return false;
        if (dib && dib_w == px_w && dib_h == px_h && rt) return true;
        if (dib) {
            SelectObject(mem_dc, old_bmp);
            DeleteObject(dib);
            dib = nullptr;
        }
        if (!mem_dc) mem_dc = CreateCompatibleDC(nullptr);
        if (!mem_dc) return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = px_w;
        bi.bmiHeader.biHeight = -px_h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dib = CreateDIBSection(mem_dc, &bi, DIB_RGB_COLORS, &dib_bits, nullptr, 0);
        if (!dib) return false;
        HGDIOBJ prev = SelectObject(mem_dc, dib);
        if (!old_bmp) old_bmp = prev;
        dib_w = px_w;
        dib_h = px_h;
        if (!rt) {
            const auto props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            if (FAILED(d2d->CreateDCRenderTarget(&props, &rt)) || !rt) return false;
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        }
        return true;
    }

    ID2D1Bitmap* Icon(const DockStackEntry& e, float dip, int& budget) {
        if (auto it = icons.find(e.path); it != icons.end()) return it->second.Get();
        if (icon_failed.count(e.path) || budget <= 0 || !rt) return nullptr;
        --budget;
        ComPtr<IShellItemImageFactory> factory;
        if (FAILED(SHCreateItemFromParsingName(e.path.c_str(), nullptr, IID_PPV_ARGS(&factory))) || !factory) {
            icon_failed.insert(e.path);
            return nullptr;
        }
        const int want = std::clamp(static_cast<int>(std::ceil(dip * req.scale)), 32, 256);
        const SIZE sz{want, want};
        HBITMAP hbm = nullptr;
        const bool image = !e.is_dir && !e.is_app && IsImageExtension(Extension(e.path));
        HRESULT hr = E_FAIL;
        if (image) hr = factory->GetImage(sz, SIIGBF_RESIZETOFIT, &hbm);
        if (FAILED(hr) && !e.is_app && !e.is_dir) {
            hr = factory->GetImage(sz, static_cast<SIIGBF>(SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY | SIIGBF_INCACHEONLY), &hbm);
        }
        if (FAILED(hr)) hr = factory->GetImage(sz, static_cast<SIIGBF>(SIIGBF_RESIZETOFIT | SIIGBF_ICONONLY), &hbm);
        ComPtr<ID2D1Bitmap> bmp;
        if (SUCCEEDED(hr) && hbm) bmp = BitmapFromHbitmap(rt.Get(), hbm);
        if (hbm) DeleteObject(hbm);
        if (!bmp) {
            icon_failed.insert(e.path);
            return nullptr;
        }
        auto* raw = bmp.Get();
        icons.emplace(e.path, std::move(bmp));
        return raw;
    }

    // -----------------------------------------------------------------------------------------
    // Layout
    // -----------------------------------------------------------------------------------------

    void LoadEntries() {
        entries = req.is_apps && nav.empty() ? EnumerateApps() : EnumerateFolder(CurrentFolder(), req.sort);
        scroll = 0.0f;
        icons.clear();
        icon_failed.clear();
    }

    float RoomAboveDip() const {
        return static_cast<float>(req.icon_screen.top - req.work_screen.top) / req.scale - 8.0f;
    }

    void Layout() {
        const float s = req.scale;
        const float icon_cx = (req.icon_screen.left + req.icon_screen.right) * 0.5f;
        const float icon_cy = (req.icon_screen.top + req.icon_screen.bottom) * 0.5f;
        has_arrow = false;
        if (view == DockStackView::Fan) {
            const int capacity = DockFanCapacity(RoomAboveDip());
            shown = std::min<int>(static_cast<int>(entries.size()), std::min(capacity, kFanMaxFiles));
            label_w.assign(static_cast<size_t>(shown) + 1, 0.0f);
            float max_label = 0.0f;
            for (int i = 0; i <= shown; ++i) {
                const std::wstring& text = (i == shown) ? OpenInPulseText() : entries[static_cast<size_t>(i)].name;
                label_w[static_cast<size_t>(i)] = std::min(250.0f, TextWidth(text, fmt_label.Get())) + 22.0f;
                max_label = std::max(max_label, label_w[static_cast<size_t>(i)]);
            }
            const DockFanPoint top = DockFanItemCenter(shown);
            fan_origin = D2D1::Point2F(16.0f + max_label + 8.0f + kDockFanIcon * 0.5f, 16.0f - top.y + kDockFanIcon * 0.5f);
            win_w = fan_origin.x + top.x + kDockFanIcon * 0.5f + 16.0f;
            win_h = fan_origin.y + 4.0f;
            px_x = static_cast<int>(std::lround(icon_cx - fan_origin.x * s));
            px_y = static_cast<int>(std::lround(req.icon_screen.top - 2.0f * s - fan_origin.y * s));
        } else {
            const bool list = view == DockStackView::List;
            const bool footer = !req.is_apps || !nav.empty();
            const int n = static_cast<int>(entries.size());
            shown = n;
            float room = 0.0f;
            if (req.dock_edge == 0) room = RoomAboveDip() - kArrow - 2.0f * kShadow;
            else room = static_cast<float>(req.work_screen.bottom - req.work_screen.top) / s - 2.0f * kShadow - 16.0f;
            const float chrome = kHeader + (footer ? kFooter : kPad);
            float content_w = 0.0f;
            int total_rows = 0;
            float row_h = 0.0f;
            if (list) {
                cols = 1;
                row_h = kListRow;
                total_rows = n;
                content_w = kListW;
                rows_visible = std::clamp(static_cast<int>((room - chrome) / row_h), 1, 14);
            } else {
                cols = req.is_apps ? std::clamp(static_cast<int>(std::ceil(std::sqrt(std::max(1, n)))), 4, 7)
                                   : std::clamp(static_cast<int>(std::ceil(std::sqrt(std::max(1, n)))), 3, 6);
                if (n > 0 && n < cols) cols = n;
                cols = std::max(cols, 3);
                row_h = kGridCellH;
                total_rows = (n + cols - 1) / cols;
                content_w = cols * kGridCellW;
                rows_visible = std::clamp(static_cast<int>((room - chrome) / row_h), 1, 5);
            }
            rows_visible = std::max(1, std::min(rows_visible, std::max(1, total_rows)));
            scroll_max = std::max(0.0f, (total_rows - rows_visible) * row_h);
            const float pw = content_w + 2.0f * kPad;
            const float ph = chrome + rows_visible * row_h;
            const float arrow = (req.dock_edge == 0) ? kArrow : 0.0f;
            win_w = pw + 2.0f * kShadow;
            win_h = ph + 2.0f * kShadow + arrow;
            panel = D2D1::RectF(kShadow, kShadow, kShadow + pw, kShadow + ph);
            const float work_l = static_cast<float>(req.work_screen.left), work_r = static_cast<float>(req.work_screen.right);
            const float work_t = static_cast<float>(req.work_screen.top), work_b = static_cast<float>(req.work_screen.bottom);
            if (req.dock_edge == 0) {
                float x = icon_cx - win_w * 0.5f * s;
                x = std::clamp(x, work_l - kShadow * s + 4.0f, std::max(work_l, work_r - (win_w - kShadow) * s - 4.0f));
                px_x = static_cast<int>(std::lround(x));
                px_y = static_cast<int>(std::lround(req.icon_screen.top - 4.0f * s - (win_h - kShadow) * s));
                has_arrow = true;
                arrow_tip = D2D1::Point2F(std::clamp((icon_cx - px_x) / s, panel.left + 22.0f, panel.right - 22.0f),
                                          panel.bottom + kArrow);
            } else {
                const float x = (req.dock_edge == 1) ? req.icon_screen.right + 8.0f * s - kShadow * s
                                                     : req.icon_screen.left - 8.0f * s - (win_w - kShadow) * s;
                float y = icon_cy - win_h * 0.5f * s;
                y = std::clamp(y, work_t - kShadow * s, std::max(work_t, work_b - (win_h - kShadow) * s));
                px_x = static_cast<int>(std::lround(x));
                px_y = static_cast<int>(std::lround(y));
            }
        }
        px_w = std::max(1, static_cast<int>(std::ceil(win_w * s)));
        px_h = std::max(1, static_cast<int>(std::ceil(win_h * s)));
    }

    // -----------------------------------------------------------------------------------------
    // Hit testing (window DIP)
    // -----------------------------------------------------------------------------------------

    D2D1_RECT_F FanIconRect(int i) const {
        const DockFanPoint c = DockFanItemCenter(i);
        const float x = fan_origin.x + c.x, y = fan_origin.y + c.y, h = kDockFanIcon * 0.5f;
        return D2D1::RectF(x - h, y - h, x + h, y + h);
    }

    D2D1_RECT_F FanLabelRect(int i) const {
        const D2D1_RECT_F ic = FanIconRect(i);
        const float cy = (ic.top + ic.bottom) * 0.5f;
        const float w = label_w.empty() ? 80.0f : label_w[static_cast<size_t>(std::min<int>(i, static_cast<int>(label_w.size()) - 1))];
        return D2D1::RectF(ic.left - 8.0f - w, cy - 12.0f, ic.left - 8.0f, cy + 12.0f);
    }

    float ContentTop() const { return panel.top + kHeader; }

    D2D1_RECT_F CellRect(int index) const {
        if (view == DockStackView::List) {
            const float top = ContentTop() + index * kListRow - scroll;
            return D2D1::RectF(panel.left + 6.0f, top, panel.right - 6.0f, top + kListRow);
        }
        const int r = index / cols, c = index % cols;
        const float left = panel.left + kPad + c * kGridCellW;
        const float top = ContentTop() + r * kGridCellH - scroll;
        return D2D1::RectF(left, top, left + kGridCellW, top + kGridCellH);
    }

    D2D1_RECT_F BackRect() const { return D2D1::RectF(panel.left + 8.0f, panel.top + 8.0f, panel.left + 36.0f, panel.top + 36.0f); }
    D2D1_RECT_F LinkRect() const {
        return D2D1::RectF(panel.right - 170.0f, panel.bottom - kFooter + 4.0f, panel.right - 12.0f, panel.bottom - 4.0f);
    }
    bool HasFooter() const { return !req.is_apps || !nav.empty(); }

    static bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

    int HitTest(float x, float y) const {
        if (closing) return kHitNone;
        if (view == DockStackView::Fan) {
            for (int i = 0; i <= shown; ++i) {
                if (In(FanIconRect(i), x, y) || In(FanLabelRect(i), x, y)) return i == shown ? kHitOpenFolder : i;
            }
            return kHitNone;
        }
        if (!nav.empty() && In(BackRect(), x, y)) return kHitBack;
        if (HasFooter() && In(LinkRect(), x, y)) return kHitOpenFolder;
        const float top = ContentTop();
        const float bottom = top + rows_visible * (view == DockStackView::List ? kListRow : kGridCellH);
        if (y < top || y >= bottom) return kHitNone;
        for (int i = 0; i < shown; ++i) {
            if (In(CellRect(i), x, y)) return i;
        }
        return kHitNone;
    }

    // -----------------------------------------------------------------------------------------
    // Painting
    // -----------------------------------------------------------------------------------------

    float Progress(double now) const {
        if (forced_progress >= 0.0f) return forced_progress;
        const float dur = (view == DockStackView::Fan) ? (closing ? kFanCloseMs : kFanOpenMs + kFanCascadeMs * shown)
                                                       : (closing ? kPanelCloseMs : kPanelOpenMs);
        const float t = Clamp01(static_cast<float>((now - anim_start) / dur));
        return closing ? 1.0f - t : t;
    }

    bool Animating(double now) const {
        if (forced_progress >= 0.0f) return false;
        const float dur = (view == DockStackView::Fan) ? (closing ? kFanCloseMs : kFanOpenMs + kFanCascadeMs * shown)
                                                       : (closing ? kPanelCloseMs : kPanelOpenMs);
        return (now - anim_start) < dur;
    }

    D2D1_COLOR_F Ink(float a = 1.0f) const {
        return req.dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.94f * a) : D2D1::ColorF(0.06f, 0.07f, 0.09f, 0.92f * a);
    }

    void DrawOpenFolderGlyph(ID2D1SolidColorBrush* br, const D2D1_RECT_F& r, float alpha) {
        const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f, rad = (r.right - r.left) * 0.42f;
        br->SetColor(D2D1::ColorF(0.10f, 0.48f, 1.0f, 0.96f * alpha));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), br);
        br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.32f * alpha));
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad - 0.5f, rad - 0.5f), br, 1.0f);
        br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha));
        const float a = rad * 0.46f, st = std::max(2.0f, rad * 0.14f);
        rt->DrawLine(D2D1::Point2F(cx - a, cy), D2D1::Point2F(cx + a, cy), br, st);
        rt->DrawLine(D2D1::Point2F(cx + a * 0.25f, cy - a * 0.72f), D2D1::Point2F(cx + a, cy), br, st);
        rt->DrawLine(D2D1::Point2F(cx + a * 0.25f, cy + a * 0.72f), D2D1::Point2F(cx + a, cy), br, st);
    }

    void DrawGenericIcon(ID2D1SolidColorBrush* br, const D2D1_RECT_F& r, bool dir, float alpha) {
        const float w = r.right - r.left;
        if (dir) {
            br->SetColor(D2D1::ColorF(0.30f, 0.62f, 0.98f, alpha));
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + w * 0.06f, r.top + w * 0.22f, r.right - w * 0.06f, r.bottom - w * 0.12f), w * 0.08f, w * 0.08f), br);
        } else {
            br->SetColor(D2D1::ColorF(0.95f, 0.95f, 0.97f, alpha));
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + w * 0.18f, r.top + w * 0.06f, r.right - w * 0.18f, r.bottom - w * 0.06f), w * 0.06f, w * 0.06f), br);
        }
    }

    void DrawEntryIcon(ID2D1SolidColorBrush* br, const DockStackEntry& e, const D2D1_RECT_F& r, float alpha, int& budget) {
        if (ID2D1Bitmap* bmp = Icon(e, r.right - r.left, budget)) {
            const D2D1_SIZE_F sz = bmp->GetSize();
            const float k = std::min((r.right - r.left) / std::max(1.0f, sz.width), (r.bottom - r.top) / std::max(1.0f, sz.height));
            const float w = sz.width * k, h = sz.height * k;
            const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;
            rt->DrawBitmap(bmp, D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f), alpha,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            DrawGenericIcon(br, r, e.is_dir, alpha);
        }
    }

    bool PaintFan(ID2D1SolidColorBrush* br, float progress, double now, int& budget) {
        (void)now;
        const float total = kFanOpenMs + kFanCascadeMs * shown;
        for (int i = shown; i >= 0; --i) {
            float e = 0.0f;
            if (forced_progress >= 0.0f) {
                e = EaseOutBack(Clamp01((progress * total - kFanCascadeMs * i) / kFanOpenMs));
            } else if (closing) {
                const float t = Clamp01(1.0f - progress);           // 0 -> 1 while closing
                const float lag = 0.06f * (shown - i) / std::max(1, shown); // the top goes first
                e = 1.0f - EaseInCubic(Clamp01((t - lag) / (1.0f - 0.06f)));
            } else {
                const float ms = progress * total;
                e = EaseOutBack(Clamp01((ms - kFanCascadeMs * i) / kFanOpenMs));
            }
            if (e <= 0.001f) continue;
            const DockFanPoint c = DockFanItemCenter(i);
            // Each item travels from the Dock icon to its place on the arc.
            const float x = fan_origin.x + c.x * e;
            const float y = fan_origin.y + kDockFanIcon * 0.5f + (c.y - kDockFanIcon * 0.5f) * e;
            const float size = kDockFanIcon * (0.42f + 0.58f * std::min(1.0f, e));
            const float alpha = Clamp01(e * 1.7f);
            const D2D1_RECT_F ir = D2D1::RectF(x - size * 0.5f, y - size * 0.5f, x + size * 0.5f, y + size * 0.5f);
            const bool hot = (hover == (i == shown ? kHitOpenFolder : i));
            // A soft contact shadow lifts each icon off busy wallpapers.
            br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.16f * alpha));
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y + size * 0.42f), size * 0.38f, size * 0.09f), br);
            if (i == shown) DrawOpenFolderGlyph(br, ir, alpha);
            else DrawEntryIcon(br, entries[static_cast<size_t>(i)], ir, alpha, budget);
            if (hot) {
                br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f * alpha));
                rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ir.left - 3, ir.top - 3, ir.right + 3, ir.bottom + 3), 10.0f, 10.0f), br);
            }
            // The name pill to the left, fading in as the item lands.
            const float la = Clamp01((e - 0.35f) / 0.65f) * alpha;
            if (la <= 0.01f) continue;
            const float w = label_w[static_cast<size_t>(i)];
            const D2D1_RECT_F lr = D2D1::RectF(ir.left - 8.0f - w, y - 12.0f, ir.left - 8.0f, y + 12.0f);
            if (hot) br->SetColor(D2D1::ColorF(0.04f, 0.42f, 0.96f, 0.96f * la));
            else if (req.dark) br->SetColor(D2D1::ColorF(0.13f, 0.13f, 0.15f, 0.84f * la));
            else br->SetColor(D2D1::ColorF(0.98f, 0.98f, 0.99f, 0.90f * la));
            rt->FillRoundedRectangle(D2D1::RoundedRect(lr, 12.0f, 12.0f), br);
            if (!req.dark && !hot) {
                br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f * la));
                rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(lr.left + 0.5f, lr.top + 0.5f, lr.right - 0.5f, lr.bottom - 0.5f), 11.5f, 11.5f), br, 1.0f);
            }
            br->SetColor(hot ? D2D1::ColorF(1, 1, 1, la) : Ink(la));
            const std::wstring& text = (i == shown) ? OpenInPulseText() : entries[static_cast<size_t>(i)].name;
            rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), fmt_label.Get(),
                         D2D1::RectF(lr.left + 11.0f, lr.top, lr.right - 9.0f, lr.bottom), br,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        return false;
    }

    bool PaintPanel(ID2D1SolidColorBrush* br, float progress, int& budget) {
        const float e = closing ? EaseInCubic(progress) : EaseOutCubic(progress);
        const float alpha = Clamp01(e * 1.2f);
        const float k = 0.92f + 0.08f * e;
        const D2D1_POINT_2F pivot = has_arrow ? arrow_tip
            : D2D1::Point2F(req.dock_edge == 1 ? panel.left : panel.right, (panel.top + panel.bottom) * 0.5f);
        const D2D1::Matrix3x2F base = D2D1::Matrix3x2F::Scale(req.scale, req.scale);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k, pivot) * base);
        const float r = 14.0f;
        // Soft shadow.
        for (int i = 6; i >= 1; --i) {
            const float g = i * 3.0f;
            br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.028f * alpha));
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(panel.left - g * 0.6f, panel.top - g * 0.3f + 4.0f,
                                                                   panel.right + g * 0.6f, panel.bottom + g * 0.8f + 4.0f),
                                                       r + g, r + g), br);
        }
        const D2D1_COLOR_F fill = req.dark ? D2D1::ColorF(0.14f, 0.14f, 0.16f, 0.95f * alpha)
                                           : D2D1::ColorF(0.97f, 0.97f, 0.98f, 0.96f * alpha);
        br->SetColor(fill);
        rt->FillRoundedRectangle(D2D1::RoundedRect(panel, r, r), br);
        br->SetColor(req.dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f * alpha) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f * alpha));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(panel.left + 0.5f, panel.top + 0.5f, panel.right - 0.5f, panel.bottom - 0.5f), r - 0.5f, r - 0.5f), br, 1.0f);
        if (has_arrow) {
            ComPtr<ID2D1PathGeometry> tri;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(d2d->CreatePathGeometry(&tri)) && SUCCEEDED(tri->Open(&sink))) {
                sink->BeginFigure(D2D1::Point2F(arrow_tip.x - kArrow * 1.3f, panel.bottom - 1.0f), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(arrow_tip);
                sink->AddLine(D2D1::Point2F(arrow_tip.x + kArrow * 1.3f, panel.bottom - 1.0f));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                br->SetColor(fill);
                rt->FillGeometry(tri.Get(), br);
            }
        }
        // Header: back chevron and title.
        if (!nav.empty()) {
            const D2D1_RECT_F b = BackRect();
            if (hover == kHitBack) {
                br->SetColor(req.dark ? D2D1::ColorF(1, 1, 1, 0.12f * alpha) : D2D1::ColorF(0, 0, 0, 0.07f * alpha));
                rt->FillRoundedRectangle(D2D1::RoundedRect(b, 7.0f, 7.0f), br);
            }
            br->SetColor(Ink(alpha));
            const float cx = (b.left + b.right) * 0.5f, cy = (b.top + b.bottom) * 0.5f;
            rt->DrawLine(D2D1::Point2F(cx + 3.0f, cy - 6.0f), D2D1::Point2F(cx - 3.0f, cy), br, 1.8f);
            rt->DrawLine(D2D1::Point2F(cx - 3.0f, cy), D2D1::Point2F(cx + 3.0f, cy + 6.0f), br, 1.8f);
        }
        const std::wstring title = CurrentTitle();
        br->SetColor(Ink(alpha));
        rt->DrawText(title.c_str(), static_cast<UINT32>(title.size()), fmt_title.Get(),
                     D2D1::RectF(panel.left + 40.0f, panel.top, panel.right - 40.0f, panel.top + kHeader), br,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
        // Content.
        const float top = ContentTop();
        const bool list = view == DockStackView::List;
        const float bottom = top + rows_visible * (list ? kListRow : kGridCellH);
        bool more = false;
        if (shown == 0) {
            const std::wstring empty(l10n::Get(l10n::StringId::FolderEmpty));
            br->SetColor(Ink(0.55f * alpha));
            rt->DrawText(empty.c_str(), static_cast<UINT32>(empty.size()), fmt_empty.Get(),
                         D2D1::RectF(panel.left, top, panel.right, bottom), br);
        }
        rt->PushAxisAlignedClip(D2D1::RectF(panel.left, top, panel.right, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < shown; ++i) {
            const D2D1_RECT_F c = CellRect(i);
            if (c.bottom < top || c.top > bottom) continue;
            const auto& entry = entries[static_cast<size_t>(i)];
            const bool hot = hover == i;
            if (hot) {
                br->SetColor(req.dark ? D2D1::ColorF(1, 1, 1, (pressed == i ? 0.18f : 0.11f) * alpha)
                                      : D2D1::ColorF(0, 0, 0, (pressed == i ? 0.10f : 0.06f) * alpha));
                rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.left + 3, c.top + 2, c.right - 3, c.bottom - 2), 9.0f, 9.0f), br);
            }
            if (list) {
                const float cy = (c.top + c.bottom) * 0.5f;
                DrawEntryIcon(br, entry, D2D1::RectF(c.left + 8, cy - kListIcon * 0.5f, c.left + 8 + kListIcon, cy + kListIcon * 0.5f), alpha, budget);
                br->SetColor(Ink(alpha));
                rt->DrawText(entry.name.c_str(), static_cast<UINT32>(entry.name.size()), fmt_label.Get(),
                             D2D1::RectF(c.left + 16 + kListIcon, c.top, c.right - 22, c.bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                if (entry.is_dir) {
                    br->SetColor(Ink(0.45f * alpha));
                    rt->DrawLine(D2D1::Point2F(c.right - 14, cy - 4), D2D1::Point2F(c.right - 10, cy), br, 1.4f);
                    rt->DrawLine(D2D1::Point2F(c.right - 10, cy), D2D1::Point2F(c.right - 14, cy + 4), br, 1.4f);
                }
            } else {
                const float cx = (c.left + c.right) * 0.5f;
                DrawEntryIcon(br, entry, D2D1::RectF(cx - kGridIcon * 0.5f, c.top + 8, cx + kGridIcon * 0.5f, c.top + 8 + kGridIcon), alpha, budget);
                br->SetColor(Ink(alpha));
                rt->DrawText(entry.name.c_str(), static_cast<UINT32>(entry.name.size()), fmt_caption.Get(),
                             D2D1::RectF(c.left + 4, c.top + 8 + kGridIcon + 4, c.right - 4, c.bottom - 6), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            if (!icons.count(entry.path) && !icon_failed.count(entry.path)) more = true; // still to load
        }
        rt->PopAxisAlignedClip();
        // Scroll indicator.
        if (scroll_max > 0.5f) {
            const float track = bottom - top - 8.0f;
            const float vis = rows_visible * (list ? kListRow : kGridCellH);
            const float thumb = std::max(24.0f, track * vis / (vis + scroll_max));
            const float y0 = top + 4.0f + (track - thumb) * (scroll / scroll_max);
            br->SetColor(Ink(0.28f * alpha));
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(panel.right - 7.0f, y0, panel.right - 4.0f, y0 + thumb), 1.5f, 1.5f), br);
        }
        if (HasFooter()) {
            br->SetColor(req.dark ? D2D1::ColorF(1, 1, 1, 0.08f * alpha) : D2D1::ColorF(0, 0, 0, 0.07f * alpha));
            rt->DrawLine(D2D1::Point2F(panel.left + 12, panel.bottom - kFooter), D2D1::Point2F(panel.right - 12, panel.bottom - kFooter), br, 1.0f);
            const std::wstring link = OpenInPulseText() + L" \u203A";
            br->SetColor(hover == kHitOpenFolder ? D2D1::ColorF(0.10f, 0.50f, 1.0f, alpha)
                                                 : D2D1::ColorF(0.16f, 0.54f, 1.0f, 0.86f * alpha));
            rt->DrawText(link.c_str(), static_cast<UINT32>(link.size()), fmt_link.Get(), LinkRect(), br);
        }
        rt->SetTransform(base);
        return more;
    }

    // Paints the current frame into the DIB; returns true when another frame is needed.
    bool Paint() {
        if (!EnsureFactories() || !EnsureSurface()) return false;
        RECT rc{0, 0, px_w, px_h};
        if (FAILED(rt->BindDC(mem_dc, &rc))) return false;
        const double now = NowMs();
        const float progress = Progress(now);
        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Scale(req.scale, req.scale));
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        ComPtr<ID2D1SolidColorBrush> br;
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br);
        int budget = (forced_progress >= 0.0f) ? 400 : 8; // lazy icon loading for long app lists
        bool more = false;
        if (br) more = (view == DockStackView::Fan) ? PaintFan(br.Get(), progress, now, budget)
                                                    : PaintPanel(br.Get(), progress, budget);
        const HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            rt.Reset();
            icons.clear();
            return true;
        }
        return more || Animating(now);
    }

    void Present() {
        if (!hwnd || req.headless) return;
        HDC screen = GetDC(nullptr);
        POINT dst{px_x, px_y}, src{0, 0};
        SIZE size{px_w, px_h};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd, screen, &dst, &size, mem_dc, &src, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
    }

    void Frame() {
        const bool more = Paint();
        Present();
        if (closing && !Animating(NowMs())) {
            FinishClose();
            return;
        }
        if (hwnd) {
            if (more) SetTimer(hwnd, kTimerFrame, 8, nullptr);
            else KillTimer(hwnd, kTimerFrame);
        }
    }

    // -----------------------------------------------------------------------------------------
    // Open / close / navigation
    // -----------------------------------------------------------------------------------------

    bool Show(const DockStackRequest& r) {
        if (open) FinishClose();
        req = r;
        if (req.scale < 0.5f) req.scale = 1.0f;
        nav.clear();
        if (!EnsureFactories()) return false;
        if (!req.headless && !EnsureWindow()) return false;
        LoadEntries();
        const int capacity = DockFanCapacity(RoomAboveDip());
        view = ResolveDockStackView(req.view, req.is_apps, req.dock_edge, entries.size(), capacity);
        if (view == DockStackView::Fan && entries.empty()) view = DockStackView::Grid;
        Layout();
        open = true;
        closing = false;
        hover = pressed = kHitNone;
        anim_start = NowMs();
        had_foreground = false;
        mouse_was_down = true;
        if (req.headless) return true;
        Frame();
        SetWindowPos(hwnd, HWND_TOPMOST, px_x, px_y, px_w, px_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        SetTimer(hwnd, kTimerWatch, 40, nullptr);
        return true;
    }

    void Relayout() {
        Layout();
        anim_start = NowMs() - 1000.0; // already open: no entrance animation
        hover = pressed = kHitNone;
        if (hwnd && !req.headless) {
            Frame();
            SetWindowPos(hwnd, HWND_TOPMOST, px_x, px_y, px_w, px_h, SWP_NOACTIVATE);
        }
    }

    void Close(bool animate) {
        if (!open || closing) return;
        if (!animate || !hwnd || req.headless) {
            FinishClose();
            return;
        }
        closed_key = req.key;
        closed_tick = GetTickCount64();
        // Reverse from wherever the entrance animation is.
        const double now = NowMs();
        const float p = Progress(now);
        closing = true;
        const float dur = (view == DockStackView::Fan) ? kFanCloseMs : kPanelCloseMs;
        anim_start = now - (1.0f - p) * dur;
        hover = kHitNone;
        Frame();
    }

    void FinishClose() {
        if (!open) return;
        open = false;
        closing = false;
        closed_key = req.key;
        closed_tick = GetTickCount64();
        if (hwnd) {
            KillTimer(hwnd, kTimerFrame);
            KillTimer(hwnd, kTimerWatch);
            if (GetCapture() == hwnd) ReleaseCapture();
            ShowWindow(hwnd, SW_HIDE);
        }
        entries.clear();
        nav.clear();
        ReleaseSurface();
    }

    void Activate(int hit) {
        if (hit == kHitBack) {
            if (!nav.empty()) nav.pop_back();
            LoadEntries();
            Relayout();
            return;
        }
        if (hit == kHitOpenFolder) {
            DockStackEntry folder;
            folder.path = CurrentFolder();
            folder.is_dir = true;
            Close(true);
            if (open_fn && !folder.path.empty()) open_fn(folder);
            return;
        }
        if (hit < 0 || hit >= static_cast<int>(entries.size())) return;
        const DockStackEntry entry = entries[static_cast<size_t>(hit)];
        if (entry.is_dir) {
            // A subfolder opens inside the stack, in the Grid (or stays a List), as on the Mac.
            nav.push_back(entry.path);
            if (view == DockStackView::Fan) {
                // Re-anchor: the panel takes the fan's place above the icon.
                view = DockStackView::Grid;
            }
            LoadEntries();
            Relayout();
            return;
        }
        Close(true);
        if (open_fn) open_fn(entry);
    }

    void OnMouse(UINT msg, int x, int y) {
        const float dx = x / req.scale, dy = y / req.scale;
        const int hit = HitTest(dx, dy);
        switch (msg) {
        case WM_MOUSEMOVE:
            if (hit != hover) {
                hover = hit;
                Frame();
            }
            {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            break;
        case WM_LBUTTONDOWN:
            pressed = hit;
            SetCapture(hwnd);
            Frame();
            break;
        case WM_LBUTTONUP:
            if (GetCapture() == hwnd) ReleaseCapture();
            if (hit != kHitNone && hit == pressed) {
                pressed = kHitNone;
                Activate(hit);
            } else {
                pressed = kHitNone;
                if (hit == kHitNone && view == DockStackView::Fan) Close(true); // a click between fan items
                else Frame();
            }
            break;
        default:
            break;
        }
    }

    // Closes on a click anywhere else, or when another window takes the foreground (also covers a
    // host that could not hand the popup the foreground).
    void Watch() {
        if (!open || closing) return;
        const HWND fg = GetForegroundWindow();
        if (fg == hwnd) had_foreground = true;
        else if (had_foreground) {
            Close(true);
            return;
        }
        const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
                          (GetAsyncKeyState(VK_MBUTTON) & 0x8000);
        if (down && !mouse_was_down) {
            POINT pt{};
            GetCursorPos(&pt);
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const bool inside = PtInRect(&wr, pt) && HitTest((pt.x - wr.left) / req.scale, (pt.y - wr.top) / req.scale) != kHitNone;
            const bool inside_panel = PtInRect(&wr, pt) && view != DockStackView::Fan &&
                In(panel, (pt.x - wr.left) / req.scale, (pt.y - wr.top) / req.scale);
            if (!inside && !inside_panel) Close(true);
        }
        mouse_was_down = down;
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        switch (msg) {
        case WM_TIMER:
            if (wp == kTimerFrame) self->Frame();
            else if (wp == kTimerWatch) self->Watch();
            return 0;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            self->OnMouse(msg, static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
            return 0;
        case WM_MOUSELEAVE:
            if (self->hover != kHitNone) {
                self->hover = kHitNone;
                self->Frame();
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (self->view != DockStackView::Fan && self->scroll_max > 0.0f) {
                const float delta = -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * 64.0f;
                self->scroll = std::clamp(self->scroll + delta, 0.0f, self->scroll_max);
                POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
                ScreenToClient(h, &pt);
                self->hover = self->HitTest(pt.x / self->req.scale, pt.y / self->req.scale);
                self->Frame();
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) self->Close(true);
            else if (wp == VK_BACK && !self->nav.empty()) self->Activate(kHitBack);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && self->open) self->Close(true);
            else if (LOWORD(wp) != WA_INACTIVE) self->had_foreground = true;
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        default:
            break;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};

DockStackPopup::DockStackPopup() : impl_(std::make_unique<Impl>()) {}
DockStackPopup::~DockStackPopup() = default;
void DockStackPopup::SetOpenHandler(OpenFn fn) { impl_->open_fn = std::move(fn); }
bool DockStackPopup::Show(const DockStackRequest& request) { return impl_->Show(request); }
void DockStackPopup::Close(bool animate) { impl_->Close(animate); }
bool DockStackPopup::IsOpen() const noexcept { return impl_->open; }
bool DockStackPopup::IsOpenFor(const std::wstring& key) const noexcept { return impl_->open && !impl_->closing && impl_->req.key == key; }
bool DockStackPopup::RecentlyClosedFor(const std::wstring& key, ULONGLONG window_ms) const noexcept {
    return (!impl_->open || impl_->closing) && impl_->closed_key == key &&
           GetTickCount64() - impl_->closed_tick < window_ms;
}
HWND DockStackPopup::Hwnd() const noexcept { return impl_->hwnd; }

bool DockStackPopup::RenderSnapshot(std::vector<uint32_t>& out_bgra, int& out_w, int& out_h, float progress) {
    if (!impl_->open) return false;
    impl_->forced_progress = Clamp01(progress);
    impl_->Paint();
    impl_->forced_progress = -1.0f;
    if (!impl_->dib_bits) return false;
    out_w = impl_->px_w;
    out_h = impl_->px_h;
    out_bgra.assign(static_cast<const uint32_t*>(impl_->dib_bits),
                    static_cast<const uint32_t*>(impl_->dib_bits) + static_cast<size_t>(out_w) * out_h);
    return true;
}

} // namespace pulse::desktop::overlays

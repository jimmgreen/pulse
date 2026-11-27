// liquid_dock_bar.cpp - Reference-faithful macOS 26 Tahoe Liquid Glass Dock bar implementation
// ported from reference demo/liquid_dock.cpp.
#include "host/render/liquid_dock_bar.h"

#include "../../../../src/common/localization.h"

#include <commctrl.h>
#include <commoncontrols.h>
#include <dwmapi.h>
#include <propkey.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <set>

namespace pulse::desktop::render {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kBounceHopDurSec = 0.36f;
constexpr int kBounceHops = 3;
constexpr double kPoofDurationMs = 420.0;
constexpr double kSlotTransitionMs = 280.0;

inline float EaseOutCubic01(float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

inline float EaseInOutCubic01(float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    return (t < 0.5f) ? (4.0f * t * t * t) : (1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f);
}

int ParseTitleBadgeCount(const wchar_t* title) noexcept {
    if (!title || title[0] != L'(') return 0;
    int val = 0;
    int digits = 0;
    for (int i = 1; i < 6 && title[i] != L'\0'; ++i) {
        if (title[i] == L')') return digits > 0 ? val : 0;
        if (title[i] < L'0' || title[i] > L'9') return 0;
        val = val * 10 + (title[i] - L'0');
        ++digits;
    }
    return 0;
}

std::wstring ToLower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

std::wstring BaseNameNoExt(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    return name;
}

bool FileExists(const std::wstring& path) noexcept {
    if (path.empty()) return false;
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ExpandEnv(const wchar_t* raw) {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = ExpandEnvironmentStringsW(raw, buf, ARRAYSIZE(buf));
    return (n > 0 && n < ARRAYSIZE(buf)) ? std::wstring(buf) : std::wstring(raw);
}

std::wstring FindExecutableOnPath(const wchar_t* exe_name) {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = SearchPathW(nullptr, exe_name, nullptr, ARRAYSIZE(buf), buf, nullptr);
    return (n > 0 && n < ARRAYSIZE(buf)) ? std::wstring(buf) : std::wstring{};
}

void ReadShortcutTargetAndAumid(const std::wstring& path,
                                std::wstring& out_target,
                                std::wstring& out_aumid) {
    out_target = path;
    out_aumid.clear();
    const std::wstring lower = ToLower(path);
    if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, L".lnk") != 0) return;

    ComPtr<IShellLinkW> psl;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl))) && psl) {
        ComPtr<IPersistFile> ppf;
        if (SUCCEEDED(psl.As(&ppf)) && ppf && SUCCEEDED(ppf->Load(path.c_str(), STGM_READ))) {
            wchar_t target[MAX_PATH * 2]{};
            if (SUCCEEDED(psl->GetPath(target, ARRAYSIZE(target), nullptr, SLGP_RAWPATH)) && target[0] != L'\0') {
                std::wstring resolved(target);
                if (FileExists(resolved)) out_target = std::move(resolved);
            }
            ComPtr<IPropertyStore> ps;
            if (SUCCEEDED(psl.As(&ps)) && ps) {
                PROPVARIANT pv{};
                PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                    out_aumid = pv.pwszVal;
                }
                PropVariantClear(&pv);
            }
        }
    }
}

std::wstring ResolveShortcutTargetIfValid(const std::wstring& path) {
    std::wstring target;
    std::wstring aumid;
    ReadShortcutTargetAndAumid(path, target, aumid);
    return target;
}

std::wstring GetProcessExePath(DWORD pid) {
    if (pid == 0) return {};
    HANDLE hproc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hproc) return {};
    wchar_t buf[MAX_PATH * 2]{};
    DWORD len = ARRAYSIZE(buf);
    std::wstring res;
    if (QueryFullProcessImageNameW(hproc, 0, buf, &len) && len > 0) {
        res.assign(buf, len);
    }
    CloseHandle(hproc);
    return res;
}

std::wstring GetProcessAumid(DWORD pid) {
    if (pid == 0) return {};
    using GetAppUserModelIdFn = LONG(WINAPI*)(HANDLE, UINT32*, PWSTR);
    static const auto fn = []() noexcept -> GetAppUserModelIdFn {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        return k32 ? reinterpret_cast<GetAppUserModelIdFn>(GetProcAddress(k32, "GetApplicationUserModelId")) : nullptr;
    }();
    if (!fn) return {};
    HANDLE hproc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hproc) return {};
    wchar_t buf[260]{};
    UINT32 len = ARRAYSIZE(buf);
    std::wstring out;
    if (fn(hproc, &len, buf) == ERROR_SUCCESS && buf[0] != L'\0') {
        out.assign(buf);
    }
    CloseHandle(hproc);
    return out;
}

std::wstring GetWindowAumid(HWND hwnd, DWORD fallback_pid = 0) {
    std::wstring out;
    if (hwnd && IsWindow(hwnd)) {
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&ps))) && ps) {
            PROPVARIANT pv{};
            PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                out = pv.pwszVal;
            }
            PropVariantClear(&pv);
        }
    }
    if (out.empty() && fallback_pid != 0) {
        out = GetProcessAumid(fallback_pid);
    }
    return out;
}

bool IsSharedHostExe(const std::wstring& base_lower) noexcept {
    return base_lower == L"applicationframehost" || base_lower == L"rundll32" ||
           base_lower == L"dllhost" || base_lower == L"svchost" ||
           base_lower == L"conhost" || base_lower == L"wwahost";
}

// Symmetrically maps Windows launcher stubs, Store/MSIX replacements, and minimized
// ApplicationFrameHost UWP shells onto the same canonical family key so pinned Dock icons
// reliably show running dots and never duplicate when opened or minimized.
std::wstring CanonicalAppFamily(const std::wstring& path_or_exe,
                                const std::wstring& aumid,
                                const std::wstring& win_title = {},
                                bool is_afh = false) {
    const std::wstring lo_path = ToLower(path_or_exe);
    const std::wstring lo_aumid = ToLower(aumid);
    const std::wstring base = ToLower(BaseNameNoExt(path_or_exe));
    const std::wstring lo_title = ToLower(win_title);

    if (lo_path == L"ms-settings:" || base == L"systemsettings" ||
        lo_aumid.find(L"immersivecontrolpanel") != std::wstring::npos ||
        (is_afh && (lo_title == L"设置" || lo_title == L"settings" || lo_title == L"設定"))) {
        return L"ms-settings:";
    }
    if (base == L"wt" || base == L"windowsterminal" ||
        lo_aumid.find(L"windowsterminal") != std::wstring::npos) {
        return L"alias:terminal";
    }
    if (base == L"calc" || base == L"calculatorapp" || base == L"calculator" ||
        lo_aumid.find(L"windowscalculator") != std::wstring::npos ||
        (is_afh && (lo_title == L"计算器" || lo_title == L"calculator" || lo_title == L"計算機"))) {
        return L"alias:calculator";
    }
    if (base == L"notepad" || lo_aumid.find(L"windowsnotepad") != std::wstring::npos) {
        return L"alias:notepad";
    }
    if (base == L"mspaint" || lo_aumid.find(L"microsoft.paint") != std::wstring::npos) {
        return L"alias:paint";
    }
    if (base == L"snippingtool" || base == L"screenclippinghost" ||
        lo_aumid.find(L"screensketch") != std::wstring::npos) {
        return L"alias:snippingtool";
    }
    if (base == L"wechat" || base == L"weixin") {
        return L"alias:wechat";
    }
    return {};
}

bool IsWindowCloaked(HWND hwnd) noexcept {
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return cloaked != 0;
    }
    return false;
}

// Mirrors WacOS apps::LoneOwned: catches visible top-level windows whose owner is an invisible
// helper/message window (common in Qt, MFC, Delphi, CAD, and Electron apps) so they are never
// dropped from the Dock while real owned child dialogs of an already-visible app stay grouped.
bool IsLoneOwnedTaskbarWindow(HWND hwnd) noexcept {
    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (!owner || !IsWindowVisible(hwnd)) return false;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const LONG_PTR st = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || !(st & WS_CAPTION)) return false;
    HWND root = GetAncestor(hwnd, GA_ROOTOWNER);
    if (!root || root == hwnd) return false;
    for (HWND o = owner; o; o = GetWindow(o, GW_OWNER)) {
        if (!IsWindowVisible(o) || IsWindowCloaked(o)) continue;
        const LONG_PTR oex = GetWindowLongPtrW(o, GWL_EXSTYLE);
        if ((oex & WS_EX_APPWINDOW) ||
            (!(oex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) && !GetWindow(o, GW_OWNER) &&
             GetWindowTextLengthW(o) > 0)) {
            return false;
        }
    }
    return true;
}

bool IsTaskbarAppWindow(HWND hwnd, DWORD own_pid) noexcept {
    if (!hwnd || !IsWindowVisible(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == own_pid) return false;

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    HWND owner = GetWindow(hwnd, GW_OWNER);
    bool lone_owned = false;
    if (ex & WS_EX_APPWINDOW) {
        // Explicitly opted into Windows Taskbar.
    } else if (owner && IsLoneOwnedTaskbarWindow(hwnd)) {
        lone_owned = true;
    } else if (owner != nullptr || (ex & WS_EX_TOOLWINDOW) || (ex & WS_EX_NOACTIVATE)) {
        return false;
    }
    if (IsWindowCloaked(hwnd)) return false;

    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
        wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0) {
        return false;
    }

    if (GetWindowTextLengthW(hwnd) <= 0 && !lone_owned) return false;

    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return false;
    if (!IsIconic(hwnd) && ((r.right - r.left) < 40 || (r.bottom - r.top) < 30)) return false;
    return true;
}

// Resolves real UWP executable behind ApplicationFrameHost.exe when possible, plus its real PID.
std::wstring ResolveWindowExePath(HWND hwnd, DWORD* out_real_pid = nullptr, bool* out_is_afh = nullptr) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (out_real_pid) *out_real_pid = pid;
    if (out_is_afh) *out_is_afh = false;

    std::wstring path = GetProcessExePath(pid);
    if (ToLower(BaseNameNoExt(path)) == L"applicationframehost") {
        if (out_is_afh) *out_is_afh = true;
        struct ChildSearch { DWORD host_pid; DWORD child_pid; } cs{pid, 0};
        EnumChildWindows(hwnd, [](HWND ch, LPARAM lp) -> BOOL {
            auto* s = reinterpret_cast<ChildSearch*>(lp);
            DWORD cpid = 0;
            GetWindowThreadProcessId(ch, &cpid);
            if (cpid != 0 && cpid != s->host_pid) {
                s->child_pid = cpid;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&cs));
        if (cs.child_pid != 0) {
            if (out_real_pid) *out_real_pid = cs.child_pid;
            std::wstring real_path = GetProcessExePath(cs.child_pid);
            if (!real_path.empty()) return real_path;
        }
    }
    return path;
}

// Extracts high-res (up to 256px) transparent HICON via Windows Shell Jumbo ImageList.
HICON ExtractBestShellIcon(const std::wstring& path, PCIDLIST_ABSOLUTE pidl = nullptr) noexcept {
    SHFILEINFOW sfi{};
    UINT flags = SHGFI_SYSICONINDEX;
    LPCWSTR target = path.c_str();
    if (pidl) {
        flags |= SHGFI_PIDL;
        target = reinterpret_cast<LPCWSTR>(pidl);
    }

    if (SHGetFileInfoW(target, 0, &sfi, sizeof(sfi), flags)) {
        for (int list_idx : {SHIL_JUMBO, SHIL_EXTRALARGE, SHIL_LARGE}) {
            IImageList* iml = nullptr;
            if (SUCCEEDED(SHGetImageList(list_idx, __uuidof(IImageList), reinterpret_cast<void**>(&iml))) && iml) {
                HICON hicon = nullptr;
                const HRESULT hr = iml->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hicon);
                iml->Release();
                if (SUCCEEDED(hr) && hicon) return hicon;
            }
        }
    }

    SHFILEINFOW sfi2{};
    const UINT flags2 = SHGFI_ICON | SHGFI_LARGEICON | (pidl ? SHGFI_PIDL : 0u);
    if (SHGetFileInfoW(target, 0, &sfi2, sizeof(sfi2), flags2) && sfi2.hIcon) {
        return sfi2.hIcon;
    }
    return nullptr;
}

HICON ExtractWindowIcon(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return nullptr;
    DWORD_PTR res = 0;
    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 60, &res) && res) {
        return CopyIcon(reinterpret_cast<HICON>(res));
    }
    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 60, &res) && res) {
        return CopyIcon(reinterpret_cast<HICON>(res));
    }
    HICON cls_icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
    if (cls_icon) return CopyIcon(cls_icon);
    return nullptr;
}

ComPtr<ID2D1Bitmap> CreateD2DBitmapFromWicSource(ID2D1DeviceContext* dc,
                                                 IWICImagingFactory* wic,
                                                 IWICBitmapSource* src,
                                                 bool allow_zero_alpha_opaque_fix = false) noexcept {
    if (!dc || !wic || !src) return {};

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) || !conv) return {};
    if (FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return {};
    }

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (w == 0 || h == 0 || w > 512 || h > 512) return {};

    std::vector<uint32_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h));
    if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size() * 4),
                                reinterpret_cast<BYTE*>(pixels.data())))) {
        return {};
    }

    if (allow_zero_alpha_opaque_fix) {
        bool any_alpha = false;
        bool any_rgb = false;
        for (uint32_t px : pixels) {
            if ((px >> 24) > 0u) { any_alpha = true; break; }
            if ((px & 0x00FFFFFFu) != 0u) any_rgb = true;
        }
        if (!any_alpha && any_rgb) {
            for (uint32_t& px : pixels) px |= 0xFF000000u;
        }
    }

    int min_x = static_cast<int>(w), min_y = static_cast<int>(h);
    int max_x = -1, max_y = -1;
    for (UINT y = 0; y < h; ++y) {
        const uint32_t* row = pixels.data() + static_cast<size_t>(y) * w;
        for (UINT x = 0; x < w; ++x) {
            if ((row[x] >> 24) > 8u) {
                if (static_cast<int>(x) < min_x) min_x = static_cast<int>(x);
                if (static_cast<int>(x) > max_x) max_x = static_cast<int>(x);
                if (static_cast<int>(y) < min_y) min_y = static_cast<int>(y);
                if (static_cast<int>(y) > max_y) max_y = static_cast<int>(y);
            }
        }
    }

    const int bw = (max_x >= min_x) ? (max_x - min_x + 1) : 0;
    const int bh = (max_y >= min_y) ? (max_y - min_y + 1) : 0;
    if (bw <= 4 || bh <= 4) return {};

    const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ComPtr<ID2D1Bitmap> result;
    const bool needs_crop = (bw < static_cast<int>(static_cast<float>(w) * 0.62f) &&
                             bh < static_cast<int>(static_cast<float>(h) * 0.62f));
    if (needs_crop) {
        const int side = std::max(bw, bh);
        const int pad = std::max(2, side / 18);
        const int dst_side = side + pad * 2;
        std::vector<uint32_t> cropped(static_cast<size_t>(dst_side) * static_cast<size_t>(dst_side), 0u);
        const int ox = pad + (side - bw) / 2;
        const int oy = pad + (side - bh) / 2;
        for (int y = 0; y < bh; ++y) {
            std::memcpy(cropped.data() + static_cast<size_t>(oy + y) * dst_side + ox,
                        pixels.data() + static_cast<size_t>(min_y + y) * w + min_x,
                        static_cast<size_t>(bw) * 4);
        }
        dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(dst_side), static_cast<UINT32>(dst_side)),
                         cropped.data(), static_cast<UINT32>(dst_side * 4), bp, &result);
    } else {
        dc->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4, bp, &result);
    }
    return result;
}

// Converts HICON to premultiplied BGRA ID2D1Bitmap, automatically cropping Windows SHIL_JUMBO
// top-left legacy 32px/48px padding so every app icon fills its Dock slot cleanly.
ComPtr<ID2D1Bitmap> CreateD2DBitmapFromIcon(ID2D1DeviceContext* dc, IWICImagingFactory* wic, HICON hicon) noexcept {
    if (!dc || !wic || !hicon) return {};
    ComPtr<IWICBitmap> wic_bmp;
    if (FAILED(wic->CreateBitmapFromHICON(hicon, &wic_bmp)) || !wic_bmp) return {};
    return CreateD2DBitmapFromWicSource(dc, wic, wic_bmp.Get(), false);
}

// Loads official high-res Store/MSIX/UWP app logos via shell:AppsFolder\<AUMID> IShellItemImageFactory.
ComPtr<ID2D1Bitmap> ExtractAppsFolderBitmap(ID2D1DeviceContext* dc,
                                            IWICImagingFactory* wic,
                                            const std::wstring& aumid) noexcept {
    if (!dc || !wic || aumid.empty()) return {};
    const std::wstring parsing = L"shell:AppsFolder\\" + aumid;
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(&factory))) || !factory) {
        return {};
    }
    HBITMAP hbmp = nullptr;
    HRESULT hr = factory->GetImage(SIZE{256, 256}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp);
    if (FAILED(hr) || !hbmp) {
        hr = factory->GetImage(SIZE{256, 256}, SIIGBF_BIGGERSIZEOK, &hbmp);
    }
    if (FAILED(hr) || !hbmp) return {};

    ComPtr<IWICBitmap> wic_bmp;
    const HRESULT cr = wic->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapUseAlpha, &wic_bmp);
    DeleteObject(hbmp);
    if (FAILED(cr) || !wic_bmp) return {};
    return CreateD2DBitmapFromWicSource(dc, wic, wic_bmp.Get(), true);
}

void DrawGenericAppSquircle(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
                            const D2D1_RECT_F& r, const std::wstring& title,
                            const DockBarTextFn& draw_text) noexcept {
    const float w = r.right - r.left;
    const float rad = w * 0.225f;
    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, D2D1::ColorF(0.32f, 0.38f, 0.48f, 0.96f)},
        {1.0f, D2D1::ColorF(0.18f, 0.21f, 0.28f, 0.96f)},
    };
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1LinearGradientBrush> lg;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)),
            sc.Get(), &lg)) && lg) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg.Get());
    } else {
        br->SetColor(D2D1::ColorF(0.24f, 0.28f, 0.36f, 0.96f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
    }
    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br, 1.0f);
    if (draw_text) {
        const std::wstring ch = title.empty() ? L"A" : title.substr(0, 1);
        draw_text(ch, r, w * 0.42f, true, 0xF5FFFFFFu, 1.0f);
    }
}

void DrawExplorerVectorIcon(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const D2D1_RECT_F& r) noexcept {
    const float w = r.right - r.left;
    const float rad = w * 0.225f;
    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, D2D1::ColorF(0.98f, 0.78f, 0.24f, 1.0f)},
        {1.0f, D2D1::ColorF(0.88f, 0.58f, 0.10f, 1.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1LinearGradientBrush> lg;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)),
            sc.Get(), &lg)) && lg) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg.Get());
    }
    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.36f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.6f, r.top + 0.6f, r.right - 0.6f, r.bottom - 0.6f), rad, rad), br, 1.0f);
    // Crisp folder tab & front pocket
    const float pad = w * 0.16f;
    br->SetColor(D2D1::ColorF(1.0f, 0.96f, 0.80f, 0.92f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + pad, r.top + w * 0.34f, r.right - pad, r.bottom - pad), w * 0.10f, w * 0.10f), br);
    br->SetColor(D2D1::ColorF(0.16f, 0.52f, 0.96f, 0.95f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + w * 0.32f, r.bottom - pad - w * 0.12f, r.right - w * 0.32f, r.bottom - pad + w * 0.04f), w * 0.05f, w * 0.05f), br);
}

void DrawTrashVectorIcon(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const D2D1_RECT_F& r, bool full) noexcept {
    const float w = r.right - r.left;
    const float rad = w * 0.225f;
    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, D2D1::ColorF(0.84f, 0.87f, 0.92f, 0.96f)},
        {1.0f, D2D1::ColorF(0.56f, 0.61f, 0.68f, 0.96f)},
    };
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1LinearGradientBrush> lg;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)),
            sc.Get(), &lg)) && lg) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg.Get());
    }
    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.6f, r.top + 0.6f, r.right - 0.6f, r.bottom - 0.6f), rad, rad), br, 1.0f);
    // Basket lid + body + vertical slats
    const float cx = (r.left + r.right) * 0.5f;
    br->SetColor(D2D1::ColorF(0.20f, 0.24f, 0.30f, 0.86f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - w * 0.26f, r.top + w * 0.24f, cx + w * 0.26f, r.top + w * 0.31f), 2.0f, 2.0f), br);
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - w * 0.11f, r.top + w * 0.17f, cx + w * 0.11f, r.top + w * 0.24f), 2.0f, 2.0f), br);
    if (full) {
        br->SetColor(D2D1::ColorF(0.98f, 0.99f, 1.0f, 0.95f));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - w * 0.08f, r.top + w * 0.36f), w * 0.10f, w * 0.07f), br);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + w * 0.08f, r.top + w * 0.37f), w * 0.09f, w * 0.06f), br);
    }
    br->SetColor(D2D1::ColorF(0.20f, 0.24f, 0.30f, 0.82f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - w * 0.21f, r.top + w * 0.34f, cx + w * 0.21f, r.bottom - w * 0.17f), w * 0.06f, w * 0.06f), br, std::max(1.4f, w * 0.045f));
    for (float ox : {-0.08f, 0.0f, 0.08f}) {
        dc->DrawLine(D2D1::Point2F(cx + w * ox, r.top + w * 0.43f), D2D1::Point2F(cx + w * ox, r.bottom - w * 0.24f), br, std::max(1.2f, w * 0.036f));
    }
}

void DrawDownloadsVectorIcon(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const D2D1_RECT_F& r) noexcept {
    const float w = r.right - r.left;
    const float rad = w * 0.225f;
    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, D2D1::ColorF(0.22f, 0.68f, 1.00f, 1.0f)},
        {1.0f, D2D1::ColorF(0.08f, 0.42f, 0.90f, 1.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1LinearGradientBrush> lg;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)),
            sc.Get(), &lg)) && lg) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg.Get());
    }
    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.34f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.6f, r.top + 0.6f, r.right - 0.6f, r.bottom - 0.6f), rad, rad), br, 1.0f);
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));
    const float st = std::max(1.8f, w * 0.075f);
    dc->DrawLine(D2D1::Point2F(cx, r.top + w * 0.23f), D2D1::Point2F(cx, cy + w * 0.14f), br, st);
    dc->DrawLine(D2D1::Point2F(cx - w * 0.16f, cy - w * 0.01f), D2D1::Point2F(cx, cy + w * 0.15f), br, st);
    dc->DrawLine(D2D1::Point2F(cx + w * 0.16f, cy - w * 0.01f), D2D1::Point2F(cx, cy + w * 0.15f), br, st);
    dc->DrawLine(D2D1::Point2F(cx - w * 0.22f, r.bottom - w * 0.22f), D2D1::Point2F(cx + w * 0.22f, r.bottom - w * 0.22f), br, st);
}

// 应用程序 stack: a blue folder-style tile carrying the four-square apps mark.
void DrawAppsStackVectorIcon(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const D2D1_RECT_F& r) noexcept {
    const float w = r.right - r.left;
    const float rad = w * 0.225f;
    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, D2D1::ColorF(0.42f, 0.56f, 1.00f, 1.0f)},
        {1.0f, D2D1::ColorF(0.20f, 0.30f, 0.92f, 1.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1LinearGradientBrush> lg;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)),
            sc.Get(), &lg)) && lg) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg.Get());
    }
    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.34f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.6f, r.top + 0.6f, r.right - 0.6f, r.bottom - 0.6f), rad, rad), br, 1.0f);
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float cell = w * 0.17f, gap = w * 0.06f, cr = cell * 0.30f;
    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));
    for (int iy = 0; iy < 2; ++iy) {
        for (int ix = 0; ix < 2; ++ix) {
            const float l = cx - cell - gap * 0.5f + ix * (cell + gap);
            const float t = cy - cell - gap * 0.5f + iy * (cell + gap);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l, t, l + cell, t + cell), cr, cr), br);
        }
    }
}

void PaintDockTooltip(ID2D1Factory* factory, ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
                      const DockBarItem& item, float alpha, float canvas_w, float canvas_h,
                      uint8_t dock_position,
                      const DockBarMeasureFn& measure_text, const DockBarTextFn& draw_text) noexcept {
    if (!dc || !br || alpha <= 0.01f || item.title.empty()) return;
    const float a = std::clamp(alpha, 0.0f, 1.0f);
    const float text_w = measure_text ? std::max(28.0f, measure_text(item.title, 12.0f)) : 64.0f;

    const float pad_x = 11.0f;
    const float tip_w = std::clamp(text_w + pad_x * 2.0f, 44.0f, 220.0f);
    const float tip_h = 24.0f;
    const float arrow_h = 5.5f;
    const float tip_rad = tip_h * 0.48f;
    float tip_left = 0.0f, tip_top = 0.0f, tip_right = 0.0f, tip_bottom = 0.0f;
    if (dock_position == 1) {
        // Left Dock: tooltip pops cleanly to the right of the hovered icon
        const float icon_right = item.draw_cx + item.draw_size * 0.5f;
        tip_left = std::clamp(icon_right + 12.0f, 8.0f, std::max(8.0f, canvas_w - tip_w - 8.0f));
        tip_right = tip_left + tip_w;
        tip_top = std::clamp(item.draw_cy - tip_h * 0.5f, 8.0f, std::max(8.0f, canvas_h - tip_h - 8.0f));
        tip_bottom = tip_top + tip_h;
    } else if (dock_position == 2) {
        // Right Dock: tooltip pops cleanly to the left of the hovered icon
        const float icon_left = item.draw_cx - item.draw_size * 0.5f;
        tip_right = std::clamp(icon_left - 12.0f, tip_w + 8.0f, std::max(tip_w + 8.0f, canvas_w - 8.0f));
        tip_left = tip_right - tip_w;
        tip_top = std::clamp(item.draw_cy - tip_h * 0.5f, 8.0f, std::max(8.0f, canvas_h - tip_h - 8.0f));
        tip_bottom = tip_top + tip_h;
    } else {
        // Bottom Dock: tooltip floats centered above the hovered icon
        const float icon_top = item.draw_cy - item.draw_size * 0.5f;
        tip_bottom = std::max(tip_h + 3.0f, icon_top - 12.0f);
        tip_top = tip_bottom - tip_h;
        tip_left = std::clamp(item.draw_cx - tip_w * 0.5f, 8.0f, std::max(8.0f, canvas_w - tip_w - 8.0f));
        tip_right = tip_left + tip_w;
    }

    // Soft drop shadow beneath tooltip capsule
    br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.34f * a));
    dc->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(tip_left - 1.0f, tip_top + 2.0f, tip_right + 1.0f, tip_bottom + 3.0f), tip_rad, tip_rad),
        br);

    // Dark frosted glass tooltip pill (exact liquid_dock.cpp:1560)
    br->SetColor(D2D1::ColorF(0.09f, 0.10f, 0.14f, 0.84f * a));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tip_left, tip_top, tip_right, tip_bottom), tip_rad, tip_rad), br);

    // Directional pointer notch facing the Dock icon
    if (factory) {
        ComPtr<ID2D1PathGeometry> tri;
        if (SUCCEEDED(factory->CreatePathGeometry(&tri)) && tri) {
            ComPtr<ID2D1GeometrySink> sk;
            if (SUCCEEDED(tri->Open(&sk)) && sk) {
                if (dock_position == 1) {
                    const float ay = std::clamp(item.draw_cy, tip_top + 6.0f, tip_bottom - 6.0f);
                    sk->BeginFigure(D2D1::Point2F(tip_left + 0.5f, ay - 5.2f), D2D1_FIGURE_BEGIN_FILLED);
                    sk->AddLine(D2D1::Point2F(tip_left + 0.5f, ay + 5.2f));
                    sk->AddLine(D2D1::Point2F(tip_left - arrow_h, ay));
                } else if (dock_position == 2) {
                    const float ay = std::clamp(item.draw_cy, tip_top + 6.0f, tip_bottom - 6.0f);
                    sk->BeginFigure(D2D1::Point2F(tip_right - 0.5f, ay - 5.2f), D2D1_FIGURE_BEGIN_FILLED);
                    sk->AddLine(D2D1::Point2F(tip_right - 0.5f, ay + 5.2f));
                    sk->AddLine(D2D1::Point2F(tip_right + arrow_h, ay));
                } else {
                    const float arrow_x = std::clamp(item.draw_cx, tip_left + tip_rad + 4.0f, tip_right - tip_rad - 4.0f);
                    sk->BeginFigure(D2D1::Point2F(arrow_x - 5.5f, tip_bottom - 0.5f), D2D1_FIGURE_BEGIN_FILLED);
                    sk->AddLine(D2D1::Point2F(arrow_x + 5.5f, tip_bottom - 0.5f));
                    sk->AddLine(D2D1::Point2F(arrow_x, tip_bottom + arrow_h));
                }
                sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                sk->Close();
                dc->FillGeometry(tri.Get(), br);
            }
        }
    }

    // Crisp specular hairline border
    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f * a));
    dc->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(tip_left + 0.4f, tip_top + 0.4f, tip_right - 0.4f, tip_bottom - 0.4f), tip_rad, tip_rad),
        br, 0.85f);

    if (draw_text) {
        draw_text(item.title,
                  D2D1::RectF(tip_left + 4.0f, tip_top + 0.5f, tip_right - 4.0f, tip_bottom - 0.5f),
                  11.8f, true, 0xF8FFFFFFu, a);
    }
}

} // namespace

// ============================================================================
// Apple-style Custom Direct2D Vector Squircle Icons (from liquid_dock.cpp)
// ============================================================================

void DrawDockWidgetsVectorIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) noexcept {
    if (!dc) return;
    ComPtr<ID2D1SolidColorBrush> br;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br)) || !br) return;

    const float w = r.right - r.left;
    const float rad = w * 0.225f;
    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, D2D1::ColorF(0.24f, 0.62f, 1.00f, 1.0f)},
        {1.0f, D2D1::ColorF(0.32f, 0.26f, 0.92f, 1.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1LinearGradientBrush> lg;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)),
            sc.Get(), &lg)) && lg) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg.Get());
    }
    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.32f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.6f, r.top + 0.6f, r.right - 0.6f, r.bottom - 0.6f), rad, rad), br.Get(), 1.0f);

    const float pad = w * 0.16f;
    const float gap = w * 0.08f;
    const float tile = (w - pad * 2.0f - gap) * 0.5f;
    const float tr = tile * 0.28f;
    const float x0 = r.left + pad;
    const float y0 = r.top + pad;

    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y0, x0 + tile, y0 + tile), tr, tr), br.Get());
    br->SetColor(D2D1::ColorF(1.0f, 0.86f, 0.34f, 0.95f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0 + tile + gap, y0, x0 + tile * 2.0f + gap, y0 + tile), tr, tr), br.Get());
    br->SetColor(D2D1::ColorF(0.45f, 0.95f, 0.72f, 0.92f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y0 + tile + gap, x0 + tile, y0 + tile * 2.0f + gap), tr, tr), br.Get());
    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0 + tile + gap, y0 + tile + gap, x0 + tile * 2.0f + gap, y0 + tile * 2.0f + gap), tr, tr), br.Get());
}

ComPtr<ID2D1PathGeometry> BuildMachinedGearGeometry(ID2D1Factory* factory,
                                                    float cx,
                                                    float cy,
                                                    int teeth,
                                                    float r_root,
                                                    float r_tip,
                                                    float r_hole,
                                                    float phase_rad) noexcept {
    if (!factory || teeth < 6) return {};
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(factory->CreatePathGeometry(&geo)) || !geo) return {};
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink)) || !sink) return {};
    sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);

    const float step = (2.0f * kPi) / static_cast<float>(teeth);
    auto polar = [&](float radius, float ang) {
        return D2D1::Point2F(cx + std::cos(ang) * radius, cy + std::sin(ang) * radius);
    };

    // Precision involute-tapered gear wheel with gently rounded tooth crowns and valley fillets.
    bool started = false;
    for (int i = 0; i < teeth; ++i) {
        const float a0 = phase_rad + static_cast<float>(i) * step;
        const float a_rise_base = a0 + step * 0.14f;
        const float a_rise_tip = a0 + step * 0.27f;
        const float a_tip_mid = a0 + step * 0.41f;
        const float a_fall_tip = a0 + step * 0.55f;
        const float a_fall_base = a0 + step * 0.68f;
        const float a_valley_end = a0 + step * 0.999f;

        const D2D1_POINT_2F p_base0 = polar(r_root, a_rise_base);
        const D2D1_POINT_2F p_tip0 = polar(r_tip * 0.992f, a_rise_tip);
        const D2D1_POINT_2F p_tip_ctrl = polar(r_tip * 1.022f, a_tip_mid);
        const D2D1_POINT_2F p_tip1 = polar(r_tip * 0.992f, a_fall_tip);
        const D2D1_POINT_2F p_base1 = polar(r_root, a_fall_base);
        const D2D1_POINT_2F p_val_end = polar(r_root, a_valley_end);

        if (!started) {
            sink->BeginFigure(p_base0, D2D1_FIGURE_BEGIN_FILLED);
            started = true;
        } else {
            sink->AddLine(p_base0);
        }
        sink->AddLine(p_tip0);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(p_tip_ctrl, p_tip1));
        sink->AddLine(p_base1);
        sink->AddArc(D2D1::ArcSegment(p_val_end, D2D1::SizeF(r_root, r_root), 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);

    // Concentric inner bore cutout (ALTERNATE fill mode hollows out the gear cleanly).
    if (r_hole > 0.5f) {
        sink->BeginFigure(D2D1::Point2F(cx + r_hole, cy), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx - r_hole, cy), D2D1::SizeF(r_hole, r_hole), 0.0f,
                                      D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r_hole, cy), D2D1::SizeF(r_hole, r_hole), 0.0f,
                                      D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    }

    if (FAILED(sink->Close())) return {};
    return geo;
}

ComPtr<ID2D1PathGeometry> BuildSettingsSpokeHubGeometry(ID2D1Factory* factory,
                                                        float cx,
                                                        float cy,
                                                        float r_ring_join,
                                                        float r_hub,
                                                        float spoke_w_outer,
                                                        float spoke_w_inner) noexcept {
    if (!factory) return {};
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(factory->CreatePathGeometry(&geo)) || !geo) return {};
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink)) || !sink) return {};
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);

    // Three Apple macOS Sequoia/Tahoe signature tapered radial spokes (0 deg, +120 deg, -120 deg)
    // joining the machined outer gear rim cleanly into the central axle hub.
    constexpr float kAngles[3] = {0.0f, 2.0f * kPi / 3.0f, -2.0f * kPi / 3.0f};
    for (float ang : kAngles) {
        const float ca = std::cos(ang);
        const float sa = std::sin(ang);
        const float px = -sa;
        const float py = ca;
        const float r0 = r_hub * 0.72f;
        const float r1 = r_ring_join;
        const D2D1_POINT_2F p0{cx + ca * r0 + px * spoke_w_inner, cy + sa * r0 + py * spoke_w_inner};
        const D2D1_POINT_2F p1{cx + ca * r1 + px * spoke_w_outer, cy + sa * r1 + py * spoke_w_outer};
        const D2D1_POINT_2F p2{cx + ca * r1 - px * spoke_w_outer, cy + sa * r1 - py * spoke_w_outer};
        const D2D1_POINT_2F p3{cx + ca * r0 - px * spoke_w_inner, cy + sa * r0 - py * spoke_w_inner};
        sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(p1);
        sink->AddLine(p2);
        sink->AddLine(p3);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    }

    // Central circular hub disk
    sink->BeginFigure(D2D1::Point2F(cx + r_hub, cy), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx - r_hub, cy), D2D1::SizeF(r_hub, r_hub), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r_hub, cy), D2D1::SizeF(r_hub, r_hub), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);

    if (FAILED(sink->Close())) return {};
    return geo;
}

void DrawDockSettingsVectorIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) noexcept {
    if (!dc) return;
    ComPtr<ID2D1SolidColorBrush> br;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br)) || !br) return;
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);

    const float w = r.right - r.left;
    if (w <= 2.0f) return;
    const float rad = w * 0.225f;
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;

    // 1. macOS 26 Tahoe brushed slate-platinum squircle base
    {
        const D2D1_GRADIENT_STOP stops[3] = {
            {0.00f, D2D1::ColorF(0.65f, 0.66f, 0.69f, 1.0f)},
            {0.52f, D2D1::ColorF(0.49f, 0.51f, 0.54f, 1.0f)},
            {1.00f, D2D1::ColorF(0.35f, 0.36f, 0.40f, 1.0f)},
        };
        ComPtr<ID2D1GradientStopCollection> sc;
        ComPtr<ID2D1LinearGradientBrush> lg;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 3, &sc)) && sc &&
            SUCCEEDED(dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx, r.top), D2D1::Point2F(cx, r.bottom)),
                sc.Get(), &lg)) && lg) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg.Get());
        }
    }

    // Subtle radial depth vignette inside squircle so the machined aluminum gears pop cleanly
    {
        const D2D1_GRADIENT_STOP rstops[2] = {
            {0.0f, D2D1::ColorF(0.06f, 0.07f, 0.09f, 0.16f)},
            {1.0f, D2D1::ColorF(0.06f, 0.07f, 0.09f, 0.0f)},
        };
        ComPtr<ID2D1GradientStopCollection> sc;
        ComPtr<ID2D1RadialGradientBrush> rg;
        if (SUCCEEDED(dc->CreateGradientStopCollection(rstops, 2, &sc)) && sc &&
            SUCCEEDED(dc->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), w * 0.44f, w * 0.44f),
                sc.Get(), &rg)) && rg) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), rg.Get());
        }
    }

    // Crisp metallic specular Liquid Glass rim bevel (matching Tahoe Settings.png)
    {
        const float inset = std::max(0.75f, w * 0.018f);
        const float rim_w = std::max(1.1f, w * 0.028f);
        const D2D1_GRADIENT_STOP rim_stops[4] = {
            {0.00f, D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.78f)},
            {0.35f, D2D1::ColorF(0.78f, 0.80f, 0.84f, 0.24f)},
            {0.72f, D2D1::ColorF(0.60f, 0.62f, 0.66f, 0.18f)},
            {1.00f, D2D1::ColorF(0.94f, 0.96f, 0.99f, 0.66f)},
        };
        ComPtr<ID2D1GradientStopCollection> sc;
        ComPtr<ID2D1LinearGradientBrush> lg;
        if (SUCCEEDED(dc->CreateGradientStopCollection(rim_stops, 4, &sc)) && sc &&
            SUCCEEDED(dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)),
                sc.Get(), &lg)) && lg) {
            dc->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(r.left + inset, r.top + inset, r.right - inset, r.bottom - inset),
                                  std::max(1.0f, rad - inset), std::max(1.0f, rad - inset)),
                lg.Get(), rim_w);
        }
    }

    D2D1_MATRIX_3X2_F saved_xf{};
    dc->GetTransform(&saved_xf);

    // 2. Recessed secondary inner gear wheel (20 machined teeth peeking behind the 3 spokes)
    if (factory) {
        const float in_root = w * 0.205f;
        const float in_tip = w * 0.252f;
        const float in_hole = w * 0.146f;
        if (auto inner_gear = BuildMachinedGearGeometry(factory.Get(), cx, cy, 20, in_root, in_tip, in_hole, kPi / 20.0f)) {
            // Soft cast shadow behind inner gear
            dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, w * 0.017f) * saved_xf);
            br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.34f));
            dc->FillGeometry(inner_gear.Get(), br.Get());
            dc->SetTransform(saved_xf);

            // Brushed steel gradient fill for recessed inner gear
            const D2D1_GRADIENT_STOP ig_stops[3] = {
                {0.0f, D2D1::ColorF(0.76f, 0.78f, 0.82f, 0.96f)},
                {0.55f, D2D1::ColorF(0.60f, 0.63f, 0.68f, 0.96f)},
                {1.0f, D2D1::ColorF(0.47f, 0.50f, 0.55f, 0.96f)},
            };
            ComPtr<ID2D1GradientStopCollection> sc;
            ComPtr<ID2D1LinearGradientBrush> lg;
            if (SUCCEEDED(dc->CreateGradientStopCollection(ig_stops, 3, &sc)) && sc &&
                SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx, cy - in_tip), D2D1::Point2F(cx, cy + in_tip)),
                    sc.Get(), &lg)) && lg) {
                dc->FillGeometry(inner_gear.Get(), lg.Get());
            }
            br->SetColor(D2D1::ColorF(0.14f, 0.16f, 0.20f, 0.28f));
            dc->DrawGeometry(inner_gear.Get(), br.Get(), std::max(0.55f, w * 0.011f));
        }

        // 3. Primary foreground precision gear (28 crisp teeth) + 3 Apple tapered Y-spokes + central hub
        const float out_root = w * 0.330f;
        const float out_tip = w * 0.402f;
        const float out_hole = w * 0.270f;
        const float hub_r = w * 0.092f;
        auto outer_gear = BuildMachinedGearGeometry(factory.Get(), cx, cy, 28, out_root, out_tip, out_hole, 0.0f);
        auto spokes_hub = BuildSettingsSpokeHubGeometry(factory.Get(), cx, cy, out_hole + w * 0.020f, hub_r,
                                                        w * 0.036f, w * 0.025f);
        if (outer_gear && spokes_hub) {
            // Layered cast shadow beneath foreground gear & spokes for machined 3D relief
            dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, w * 0.024f) * saved_xf);
            br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.36f));
            dc->FillGeometry(outer_gear.Get(), br.Get());
            dc->FillGeometry(spokes_hub.Get(), br.Get());
            dc->SetTransform(saved_xf);

            // Luminous brushed aluminum gradient across gear wheel & spokes
            const D2D1_GRADIENT_STOP fg_stops[3] = {
                {0.00f, D2D1::ColorF(0.98f, 0.99f, 1.00f, 1.0f)},
                {0.52f, D2D1::ColorF(0.86f, 0.88f, 0.91f, 1.0f)},
                {1.00f, D2D1::ColorF(0.71f, 0.74f, 0.78f, 1.0f)},
            };
            ComPtr<ID2D1GradientStopCollection> sc;
            ComPtr<ID2D1LinearGradientBrush> lg;
            if (SUCCEEDED(dc->CreateGradientStopCollection(fg_stops, 3, &sc)) && sc &&
                SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx - w * 0.14f, cy - out_tip),
                                                        D2D1::Point2F(cx + w * 0.14f, cy + out_tip)),
                    sc.Get(), &lg)) && lg) {
                dc->FillGeometry(outer_gear.Get(), lg.Get());
                dc->FillGeometry(spokes_hub.Get(), lg.Get());
            }

            // Diagonal brushed-metal specular glint sweeping across the spokes & gear rim
            const D2D1_GRADIENT_STOP sheen_stops[4] = {
                {0.00f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
                {0.42f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
                {0.50f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f)},
                {0.58f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
            };
            ComPtr<ID2D1GradientStopCollection> sh_sc;
            ComPtr<ID2D1LinearGradientBrush> sh_lg;
            if (SUCCEEDED(dc->CreateGradientStopCollection(sheen_stops, 4, &sh_sc)) && sh_sc &&
                SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)),
                    sh_sc.Get(), &sh_lg)) && sh_lg) {
                dc->FillGeometry(outer_gear.Get(), sh_lg.Get());
                dc->FillGeometry(spokes_hub.Get(), sh_lg.Get());
            }

            // Crisp micro-chamfer highlight along outer rim & inner ring
            br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.50f));
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), (out_root + out_hole) * 0.5f, (out_root + out_hole) * 0.5f),
                            br.Get(), std::max(0.6f, w * 0.012f));
        }
    }

    // 4. Recessed dark graphite axle bore + subtle silver inner collar
    const float bore_r = w * 0.044f;
    {
        const D2D1_GRADIENT_STOP bore_stops[2] = {
            {0.0f, D2D1::ColorF(0.24f, 0.26f, 0.30f, 1.0f)},
            {1.0f, D2D1::ColorF(0.42f, 0.44f, 0.49f, 1.0f)},
        };
        ComPtr<ID2D1GradientStopCollection> sc;
        ComPtr<ID2D1LinearGradientBrush> lg;
        if (SUCCEEDED(dc->CreateGradientStopCollection(bore_stops, 2, &sc)) && sc &&
            SUCCEEDED(dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx, cy - bore_r), D2D1::Point2F(cx, cy + bore_r)),
                sc.Get(), &lg)) && lg) {
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), bore_r, bore_r), lg.Get());
        } else {
            br->SetColor(D2D1::ColorF(0.28f, 0.30f, 0.35f, 1.0f));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), bore_r, bore_r), br.Get());
        }
        br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f));
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), bore_r + w * 0.008f, bore_r + w * 0.008f),
                        br.Get(), std::max(0.6f, w * 0.013f));
    }
}

void DrawDockTerminalVectorIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) noexcept {
    if (!dc) return;
    ComPtr<ID2D1SolidColorBrush> br;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br)) || !br) return;

    const float w = r.right - r.left;
    const float rad = w * 0.225f;
    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, D2D1::ColorF(0.20f, 0.23f, 0.29f, 1.0f)},
        {1.0f, D2D1::ColorF(0.08f, 0.09f, 0.13f, 1.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1LinearGradientBrush> lg;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)),
            sc.Get(), &lg)) && lg) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg.Get());
    }
    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.34f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.7f, r.top + 0.7f, r.right - 0.7f, r.bottom - 0.7f), rad, rad), br.Get(), 1.15f);

    const float dot_r = w * 0.036f;
    const float dy = r.top + w * 0.17f;
    br->SetColor(D2D1::ColorF(1.0f, 0.38f, 0.34f, 0.85f));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + w * 0.20f, dy), dot_r, dot_r), br.Get());
    br->SetColor(D2D1::ColorF(1.0f, 0.76f, 0.22f, 0.85f));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + w * 0.30f, dy), dot_r, dot_r), br.Get());
    br->SetColor(D2D1::ColorF(0.26f, 0.84f, 0.44f, 0.85f));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + w * 0.40f, dy), dot_r, dot_r), br.Get());

    const float stroke = w * 0.068f;
    const float px = r.left + w * 0.24f;
    const float py = r.top + w * 0.56f;
    br->SetColor(D2D1::ColorF(0.32f, 0.88f, 1.0f, 0.96f));
    dc->DrawLine(D2D1::Point2F(px, py - w * 0.12f), D2D1::Point2F(px + w * 0.15f, py), br.Get(), stroke);
    dc->DrawLine(D2D1::Point2F(px + w * 0.15f, py), D2D1::Point2F(px, py + w * 0.12f), br.Get(), stroke);
    br->SetColor(D2D1::ColorF(0.95f, 0.97f, 1.0f, 0.94f));
    dc->DrawLine(D2D1::Point2F(px + w * 0.22f, py + w * 0.11f), D2D1::Point2F(px + w * 0.46f, py + w * 0.11f), br.Get(), stroke);
}

void DrawDockCalculatorVectorIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& r) noexcept {
    if (!dc) return;
    ComPtr<ID2D1SolidColorBrush> br;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br)) || !br) return;

    const float w = r.right - r.left;
    const float rad = w * 0.225f;
    const D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, D2D1::ColorF(0.24f, 0.25f, 0.29f, 1.0f)},
        {1.0f, D2D1::ColorF(0.11f, 0.12f, 0.15f, 1.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1LinearGradientBrush> lg;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)),
            sc.Get(), &lg)) && lg) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg.Get());
    }
    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.6f, r.top + 0.6f, r.right - 0.6f, r.bottom - 0.6f), rad, rad), br.Get(), 1.0f);

    const float pad = w * 0.14f;
    br->SetColor(D2D1::ColorF(0.06f, 0.07f, 0.09f, 0.85f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + pad, r.top + pad, r.right - pad, r.top + w * 0.34f), w * 0.06f, w * 0.06f), br.Get());

    const float grid_top = r.top + w * 0.43f;
    const float cell = (w - pad * 2.0f) / 3.0f;
    const float btn_r = cell * 0.37f;
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            const float cx = r.left + pad + (static_cast<float>(col) + 0.5f) * cell;
            const float cy = grid_top + (static_cast<float>(row) + 0.5f) * (cell * 0.94f);
            if (col == 2) br->SetColor(D2D1::ColorF(1.0f, 0.58f, 0.08f, 0.98f));
            else br->SetColor(D2D1::ColorF(0.42f, 0.44f, 0.49f, 0.92f));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), btn_r, btn_r), br.Get());
        }
    }
}

// ============================================================================
// DockBarIconCache
// ============================================================================

void DockBarIconCache::Clear() noexcept {
    cache_.clear();
    bound_dc_ = nullptr;
}

ID2D1Bitmap* DockBarIconCache::GetOrLoad(ID2D1DeviceContext* dc, const DockBarItem& item, bool trash_not_empty) noexcept {
    if (!dc) return nullptr;
    if (bound_dc_ && bound_dc_ != dc) {
        Clear();
    }
    bound_dc_ = dc;

    std::wstring cache_key = item.key;
    if (item.kind == DockBarItemKind::Trash) {
        cache_key = trash_not_empty ? L"special:trash:full" : L"special:trash:empty";
    }
    if (auto it = cache_.find(cache_key); it != cache_.end()) {
        return it->second.Get();
    }

    if (!wic_) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    }
    if (!wic_) return nullptr;

    HICON hicon = nullptr;
    ComPtr<ID2D1Bitmap> bmp;
    if (item.kind == DockBarItemKind::Trash) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &pidl)) && pidl) {
            hicon = ExtractBestShellIcon(L"", pidl);
            CoTaskMemFree(pidl);
        }
    } else if (item.kind == DockBarItemKind::Folder) {
        if (item.key == L"special:downloads") {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_Downloads, 0, nullptr, &pidl)) && pidl) {
                hicon = ExtractBestShellIcon(L"", pidl);
                CoTaskMemFree(pidl);
            }
        } else if (!item.launch_path.empty() && FileExists(item.launch_path)) {
            hicon = ExtractBestShellIcon(item.launch_path);
        }
    } else if (item.kind == DockBarItemKind::App) {
        if (item.key == L"ms-settings:") {
            cache_[cache_key] = nullptr;
            return nullptr;
        }
        std::wstring effective_aumid = item.aumid;
        if (effective_aumid.empty()) {
            const std::wstring lp_lo = ToLower(item.launch_path);
            constexpr const wchar_t* kPrefix = L"shell:appsfolder\\";
            if (lp_lo.rfind(kPrefix, 0) == 0 && item.launch_path.size() > 17) {
                effective_aumid = item.launch_path.substr(17);
            }
        }
        if (!effective_aumid.empty()) {
            bmp = ExtractAppsFolderBitmap(dc, wic_.Get(), effective_aumid);
        }
        if (!bmp && !item.launch_path.empty() && FileExists(item.launch_path)) {
            hicon = ExtractBestShellIcon(item.launch_path);
        }
        if (!bmp && !hicon) {
            for (HWND wh : item.windows) {
                hicon = ExtractWindowIcon(wh);
                if (hicon) break;
            }
        }
    }

    if (!bmp && hicon) {
        bmp = CreateD2DBitmapFromIcon(dc, wic_.Get(), hicon);
    }
    if (hicon) {
        DestroyIcon(hicon);
    }
    ID2D1Bitmap* raw = bmp.Get();
    // Only cache successfully loaded bitmaps so apps whose window icon isn't ready yet can retry.
    if (bmp) {
        cache_[cache_key] = std::move(bmp);
    }
    return raw;
}

ID2D1Bitmap* DockBarIconCache::GetOrLoadExeIcon(ID2D1DeviceContext* dc, const std::wstring& exe_path,
                                                HWND fallback_hwnd) noexcept {
    if (!dc || (exe_path.empty() && !fallback_hwnd)) return nullptr;
    if (bound_dc_ != dc) {
        Clear();
        bound_dc_ = dc;
    }
    if (!wic_) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    }
    if (!wic_) return nullptr;

    const std::wstring cache_key = !exe_path.empty()
        ? (L"tray:" + ToLower(exe_path))
        : (L"trayhwnd:" + std::to_wstring(reinterpret_cast<uintptr_t>(fallback_hwnd)));
    if (auto it = cache_.find(cache_key); it != cache_.end()) {
        return it->second.Get();
    }

    HICON hicon = nullptr;
    if (!exe_path.empty() && FileExists(exe_path)) {
        hicon = ExtractBestShellIcon(exe_path);
    }
    if (!hicon && fallback_hwnd && IsWindow(fallback_hwnd)) {
        hicon = ExtractWindowIcon(fallback_hwnd);
    }
    ComPtr<ID2D1Bitmap> bmp;
    if (hicon) {
        bmp = CreateD2DBitmapFromIcon(dc, wic_.Get(), hicon);
        DestroyIcon(hicon);
    }
    ID2D1Bitmap* raw = bmp.Get();
    if (bmp) {
        cache_[cache_key] = std::move(bmp);
    }
    return raw;
}

// ============================================================================
// LiquidDockBarState
// ============================================================================

std::vector<std::wstring> LiquidDockBarState::DiscoverDefaultPinnedApps() {
    std::vector<std::wstring> list;
    auto add_if_exists = [&](const std::wstring& p) {
        if (!p.empty() && FileExists(p)) list.push_back(p);
    };

    add_if_exists(ExpandEnv(L"%SystemRoot%\\explorer.exe"));

    const std::wstring chrome_paths[] = {
        ExpandEnv(L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe"),
        ExpandEnv(L"%ProgramFiles(x86)%\\Google\\Chrome\\Application\\chrome.exe"),
        ExpandEnv(L"%LocalAppData%\\Google\\Chrome\\Application\\chrome.exe"),
    };
    for (const auto& p : chrome_paths) {
        if (FileExists(p)) { list.push_back(p); break; }
    }
    const std::wstring edge_paths[] = {
        ExpandEnv(L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe"),
        ExpandEnv(L"%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe"),
    };
    for (const auto& p : edge_paths) {
        if (FileExists(p)) { list.push_back(p); break; }
    }

    std::wstring wt = FindExecutableOnPath(L"wt.exe");
    if (!wt.empty()) {
        list.push_back(wt);
    } else {
        add_if_exists(ExpandEnv(L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"));
    }

    std::wstring np = FindExecutableOnPath(L"notepad.exe");
    add_if_exists(np.empty() ? ExpandEnv(L"%SystemRoot%\\System32\\notepad.exe") : np);
    add_if_exists(ExpandEnv(L"%SystemRoot%\\System32\\calc.exe"));
    list.push_back(L"ms-settings:");
    return list;
}

std::wstring LiquidDockBarState::NormalizePathForPin(const std::wstring& raw_path) {
    if (raw_path.empty()) return {};
    std::wstring resolved = ResolveShortcutTargetIfValid(raw_path);
    return resolved.empty() ? raw_path : resolved;
}

std::wstring LiquidDockBarState::FriendlyAppName(const std::wstring& path) {
    const std::wstring lo = ToLower(path);
    if (lo == L"ms-settings:") return L"系统设置";
    if (lo == L"special:widgets") return L"桌面小组件";
    if (lo == L"special:trash") return L"回收站";
    if (lo == L"special:downloads") return L"下载";
    if (lo == L"special:apps") return std::wstring(l10n::Get(l10n::StringId::DockStackApps));

    if (lo.rfind(L"shell:appsfolder\\", 0) == 0) {
        ComPtr<IShellItem> si;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&si))) && si) {
            PWSTR disp = nullptr;
            if (SUCCEEDED(si->GetDisplayName(SIGDN_NORMALDISPLAY, &disp)) && disp) {
                std::wstring out(disp);
                CoTaskMemFree(disp);
                if (!out.empty()) return out;
            }
        }
    }

    const std::wstring base = ToLower(BaseNameNoExt(path));
    if (base == L"explorer") return L"文件资源管理器";
    if (base == L"msedge") return L"Microsoft Edge";
    if (base == L"chrome") return L"Google Chrome";
    if (base == L"firefox") return L"Firefox";
    if (base == L"wt" || base == L"windowsterminal") return L"终端";
    if (base == L"powershell" || base == L"pwsh") return L"PowerShell";
    if (base == L"cmd") return L"命令提示符";
    if (base == L"notepad") return L"记事本";
    if (base == L"calc" || base == L"calculatorapp") return L"计算器";
    if (base == L"code") return L"VS Code";
    if (base == L"devenv") return L"Visual Studio";
    if (base == L"wechat" || base == L"weixin") return L"微信";
    if (base == L"qq") return L"QQ";
    if (base == L"acad") return L"AutoCAD";
    if (base == L"chatgpt") return L"ChatGPT";
    if (base == L"flclash") return L"FlClash";
    if (base == L"pulse") return L"Pulse";
    if (base == L"antigravity") return L"Antigravity";
    if (base == L"taskmgr") return L"任务管理器";
    if (base == L"mspaint") return L"画图";
    if (base == L"snippingtool") return L"截图工具";
    if (base == L"systemsettings") return L"系统设置";

    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_DISPLAYNAME) && sfi.szDisplayName[0] != L'\0') {
        std::wstring dn = sfi.szDisplayName;
        const size_t ext = dn.rfind(L".exe");
        if (ext != std::wstring::npos && ext + 4 == dn.size()) dn.resize(ext);
        const size_t lnk = dn.rfind(L".lnk");
        if (lnk != std::wstring::npos && lnk + 4 == dn.size()) dn.resize(lnk);
        return dn;
    }
    std::wstring fallback = BaseNameNoExt(path);
    if (!fallback.empty()) fallback[0] = static_cast<wchar_t>(std::towupper(fallback[0]));
    return fallback.empty() ? L"应用" : fallback;
}

bool LiquidDockBarState::HasActiveBounce(double now_ms) const noexcept {
    for (const auto& it : items) {
        if (it.bounce_start_ms > 0.0 && (now_ms - it.bounce_start_ms) < 1150.0) return true;
    }
    return false;
}

bool LiquidDockBarState::HasActivePoof(double now_ms) const noexcept {
    return poof.start_ms > 0.0 && (now_ms - poof.start_ms) >= 0.0 && (now_ms - poof.start_ms) < kPoofDurationMs;
}

bool LiquidDockBarState::HasActiveSlotMotion(double now_ms) const noexcept {
    if (now_ms <= 0.0) return false;
    for (const auto& [k, t0] : item_appear_ms) {
        if ((now_ms - t0) >= 0.0 && (now_ms - t0) < kSlotTransitionMs + 16.0) return true;
    }
    for (const auto& gh : leaving_ghosts) {
        if ((now_ms - gh.start_ms) >= 0.0 && (now_ms - gh.start_ms) < kSlotTransitionMs + 16.0) return true;
    }
    return false;
}

void LiquidDockBarState::TriggerPoof(float x_dip, float y_dip, double now_ms) noexcept {
    poof.x = x_dip;
    poof.y = y_dip;
    poof.start_ms = now_ms;
}

int LiquidDockBarState::FindItemIndexByKey(const std::wstring& key) const noexcept {
    if (key.empty()) return -1;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!items[i].is_ghost && items[i].key == key) return static_cast<int>(i);
    }
    return -1;
}

int LiquidDockBarState::FindItemIndexForHwnd(HWND target) const noexcept {
    if (!target) return -1;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        if (it.is_ghost || it.kind != DockBarItemKind::App) continue;
        for (HWND wh : it.windows) {
            if (wh == target) return static_cast<int>(i);
        }
    }
    DWORD real_pid = 0;
    bool is_afh = false;
    const std::wstring exe = ResolveWindowExePath(target, &real_pid, &is_afh);
    const std::wstring aumid = GetWindowAumid(target, real_pid);
    wchar_t wtitle[128]{};
    GetWindowTextW(target, wtitle, ARRAYSIZE(wtitle));
    const std::wstring family = CanonicalAppFamily(exe, aumid, wtitle, is_afh);
    const std::wstring lo_exe = ToLower(exe);
    const std::wstring base = ToLower(BaseNameNoExt(exe));
    const std::wstring lo_aumid = ToLower(aumid);

    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        if (it.is_ghost || it.kind != DockBarItemKind::App) continue;
        if (!family.empty() && CanonicalAppFamily(it.launch_path, it.aumid) == family) return static_cast<int>(i);
        if (!lo_aumid.empty() && !it.aumid.empty() && ToLower(it.aumid) == lo_aumid) return static_cast<int>(i);
        if (!lo_exe.empty() && it.key == lo_exe) return static_cast<int>(i);
        if (!base.empty() && !IsSharedHostExe(base) && ToLower(BaseNameNoExt(it.launch_path)) == base) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool LiquidDockBarState::IsWindowLiveForActivation(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (!IsWindowVisible(hwnd)) return false;
    if (IsWindowCloaked(hwnd)) return false;
    return true;
}

model::RectI LiquidDockBarState::RestingObstacleRect() const noexcept {
    if (items.empty() || pill_rect.right <= pill_rect.left || pill_rect.bottom <= pill_rect.top) return {};
    if (IsVertical()) {
        if (rest_pill_h <= 1.0f) return {};
        const float cy = (pill_rect.top + pill_rect.bottom) * 0.5f;
        const float w = pill_rect.right - pill_rect.left;
        return model::RectI{
            static_cast<int>(std::floor(pill_rect.left - 8.0f)),
            static_cast<int>(std::floor(cy - rest_pill_h * 0.5f - 8.0f)),
            static_cast<int>(std::ceil(w + 16.0f)),
            static_cast<int>(std::ceil(rest_pill_h + 16.0f)),
        };
    }
    if (rest_pill_w <= 1.0f) return {};
    const float cx = (pill_rect.left + pill_rect.right) * 0.5f;
    const float h = pill_rect.bottom - pill_rect.top;
    return model::RectI{
        static_cast<int>(std::floor(cx - rest_pill_w * 0.5f - 8.0f)),
        static_cast<int>(std::floor(pill_rect.top - 8.0f)),
        static_cast<int>(std::ceil(rest_pill_w + 16.0f)),
        static_cast<int>(std::ceil(h + 16.0f)),
    };
}

model::RectI LiquidDockBarState::DamageRect(const model::RectI& work_area) const noexcept {
    if (pill_rect.right <= pill_rect.left || pill_rect.bottom <= pill_rect.top) return {};
    float pad_l = 120.0f, pad_t = 136.0f, pad_r = 120.0f, pad_b = 36.0f;
    if (dock_position == 1) {
        pad_l = 36.0f; pad_r = 260.0f; pad_t = 120.0f; pad_b = 120.0f;
    } else if (dock_position == 2) {
        pad_l = 260.0f; pad_r = 36.0f; pad_t = 120.0f; pad_b = 120.0f;
    }
    int x0 = std::max(work_area.x, static_cast<int>(std::floor(pill_rect.left - pad_l)));
    int y0 = std::max(work_area.y, static_cast<int>(std::floor(pill_rect.top - pad_t)));
    int x1 = std::min(work_area.right(), static_cast<int>(std::ceil(pill_rect.right + pad_r)));
    int y1 = std::min(work_area.bottom(), static_cast<int>(std::ceil(pill_rect.bottom + pad_b)));
    if (poof.start_ms > 0.0) {
        x0 = std::max(work_area.x, std::min(x0, static_cast<int>(std::floor(poof.x - 84.0f))));
        y0 = std::max(work_area.y, std::min(y0, static_cast<int>(std::floor(poof.y - 84.0f))));
        x1 = std::min(work_area.right(), std::max(x1, static_cast<int>(std::ceil(poof.x + 84.0f))));
        y1 = std::min(work_area.bottom(), std::max(y1, static_cast<int>(std::ceil(poof.y + 84.0f))));
    }
    return model::RectI{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

bool LiquidDockBarState::RefreshSystemItems(std::vector<std::wstring>& pinned_paths,
                                            DWORD own_pid,
                                            int trash_override,
                                            double now_ms) {
    if (downloads_path.empty()) {
        PWSTR dl_path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &dl_path)) && dl_path) {
            downloads_path = dl_path;
            CoTaskMemFree(dl_path);
        }
    }

    if (pinned_paths.empty()) {
        pinned_paths = DiscoverDefaultPinnedApps();
    }

    std::unordered_map<std::wstring, double> prev_bounce;
    std::unordered_map<std::wstring, float> prev_draw_cx;
    for (const auto& it : items) {
        if (it.bounce_start_ms > 0.0) prev_bounce[it.key] = it.bounce_start_ms;
        if (it.draw_cx > 0.0f) prev_draw_cx[it.key] = it.draw_cx;
    }

    struct RunningGroup {
        std::wstring exe_path;
        std::wstring launch_path;
        std::wstring aumid;
        std::wstring family;
        std::wstring key;
        std::wstring title;
        std::vector<HWND> hwnds;
        bool active = false;
        int badge_count = 0;
    };
    std::vector<RunningGroup> running_order;
    std::unordered_map<std::wstring, size_t> running_idx;

    const HWND fg_wnd = GetForegroundWindow();
    struct EnumCtx {
        DWORD own_pid;
        HWND fg_wnd;
        std::vector<RunningGroup>* groups;
        std::unordered_map<std::wstring, size_t>* idx_map;
    } ectx{own_pid, fg_wnd, &running_order, &running_idx};

    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(lp);
        if (!IsTaskbarAppWindow(h, c->own_pid)) return TRUE;

        DWORD real_pid = 0;
        bool is_afh = false;
        std::wstring exe = ResolveWindowExePath(h, &real_pid, &is_afh);
        std::wstring aumid = GetWindowAumid(h, real_pid);

        wchar_t wtitle[160]{};
        GetWindowTextW(h, wtitle, ARRAYSIZE(wtitle));
        const std::wstring title_str(wtitle);

        const std::wstring base = ToLower(BaseNameNoExt(exe));
        if (base == L"explorer") {
            wchar_t cls[64]{};
            GetClassNameW(h, cls, ARRAYSIZE(cls));
            if (wcscmp(cls, L"CabinetWClass") != 0 && wcscmp(cls, L"ExploreWClass") != 0) return TRUE;
        }

        const std::wstring family = CanonicalAppFamily(exe, aumid, title_str, is_afh);
        const std::wstring lo_exe = ToLower(exe);
        const bool is_windows_apps = (lo_exe.find(L"\\windowsapps\\") != std::wstring::npos);

        std::wstring key;
        std::wstring launch_path = exe;
        if (family == L"ms-settings:") {
            key = L"ms-settings:";
            launch_path = L"ms-settings:";
        } else if (!family.empty()) {
            key = family;
            if (!aumid.empty() && (is_afh || is_windows_apps)) {
                launch_path = L"shell:AppsFolder\\" + aumid;
            }
        } else if ((is_afh || is_windows_apps) && !aumid.empty()) {
            key = L"aumid:" + ToLower(aumid);
            launch_path = L"shell:AppsFolder\\" + aumid;
        } else if (is_afh && base == L"applicationframehost") {
            // Minimized UWP app without a mounted CoreWindow or AUMID: isolate by title so
            // unrelated Store apps never merge into a single ApplicationFrameHost slot.
            key = L"afh:" + ToLower(title_str);
            launch_path.clear();
        } else if (!lo_exe.empty()) {
            key = lo_exe;
        } else if (!aumid.empty()) {
            key = L"aumid:" + ToLower(aumid);
            launch_path = L"shell:AppsFolder\\" + aumid;
        } else {
            // Protected/elevated process where OpenProcess failed: still show on Dock via PID!
            key = L"pid:" + std::to_wstring(real_pid);
            launch_path.clear();
        }

        const int badge = ParseTitleBadgeCount(wtitle);
        const bool is_active = (h == c->fg_wnd) ||
                               (c->fg_wnd && GetAncestor(c->fg_wnd, GA_ROOTOWNER) == h);

        auto it = c->idx_map->find(key);
        if (it == c->idx_map->end()) {
            RunningGroup rg;
            rg.exe_path = exe;
            rg.launch_path = launch_path;
            rg.aumid = aumid;
            rg.family = family;
            rg.key = key;
            if (!launch_path.empty()) {
                rg.title = LiquidDockBarState::FriendlyAppName(launch_path);
            } else if (!exe.empty() && !is_afh) {
                rg.title = LiquidDockBarState::FriendlyAppName(exe);
            } else {
                rg.title = title_str.empty() ? L"应用" : title_str;
            }
            rg.hwnds.push_back(h);
            rg.active = is_active;
            rg.badge_count = badge;
            (*c->idx_map)[key] = c->groups->size();
            c->groups->push_back(std::move(rg));
        } else {
            auto& rg = (*c->groups)[it->second];
            rg.hwnds.push_back(h);
            if (is_active) rg.active = true;
            if (badge > rg.badge_count) rg.badge_count = badge;
            if (rg.aumid.empty() && !aumid.empty()) rg.aumid = aumid;
            if (rg.launch_path.empty() && !launch_path.empty()) rg.launch_path = launch_path;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ectx));

    bool next_trash_not_empty = trash_not_empty;
    if (trash_override >= 0) {
        next_trash_not_empty = (trash_override != 0);
    } else {
        SHQUERYRBINFO rb_info{sizeof(rb_info)};
        if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &rb_info))) {
            next_trash_not_empty = (rb_info.i64NumItems > 0);
        }
    }

    std::vector<DockBarItem> next;
    std::set<std::wstring> used_keys;
    bool has_pinned_items = false;

    // 1. Pinned items (apps or folders pinned by the user)
    for (const auto& p : pinned_paths) {
        if (p.empty()) continue;
        const std::wstring key = ToLower(p);
        if (used_keys.count(key)) continue;
        used_keys.insert(key);
        has_pinned_items = true;

        DockBarItem di;
        const bool is_uri = (key == L"ms-settings:" || key.rfind(L"shell:", 0) == 0);
        const DWORD attrs = is_uri ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(p.c_str());
        const bool is_dir = (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
        di.kind = is_dir ? DockBarItemKind::Folder : DockBarItemKind::App;
        di.key = key;
        di.launch_path = p;
        di.title = FriendlyAppName(p);
        di.pinned = true;

        if (di.kind == DockBarItemKind::App) {
            std::wstring resolved_for_match;
            std::wstring pin_aumid;
            ReadShortcutTargetAndAumid(p, resolved_for_match, pin_aumid);
            if (pin_aumid.empty() && key.rfind(L"shell:appsfolder\\", 0) == 0 && p.size() > 17) {
                pin_aumid = p.substr(17);
            }
            di.aumid = pin_aumid;
            const std::wstring resolved_key = ToLower(resolved_for_match);
            const std::wstring pin_base = ToLower(BaseNameNoExt(resolved_for_match));
            const std::wstring pin_family = CanonicalAppFamily(resolved_for_match, pin_aumid);
            const std::wstring lo_pin_aumid = ToLower(pin_aumid);

            for (const auto& rg : running_order) {
                const bool match_key = (rg.key == key || rg.key == resolved_key);
                const bool match_family = (!pin_family.empty() && rg.family == pin_family);
                const bool match_aumid = (!lo_pin_aumid.empty() && !rg.aumid.empty() &&
                                          ToLower(rg.aumid) == lo_pin_aumid);
                const bool match_base = (!pin_base.empty() && !IsSharedHostExe(pin_base) &&
                                         !rg.exe_path.empty() &&
                                         ToLower(BaseNameNoExt(rg.exe_path)) == pin_base);
                if (match_key || match_family || match_aumid || match_base) {
                    di.running = true;
                    di.active = di.active || rg.active;
                    if (rg.badge_count > di.badge_count) di.badge_count = rg.badge_count;
                    if (di.aumid.empty() && !rg.aumid.empty()) di.aumid = rg.aumid;
                    di.windows.insert(di.windows.end(), rg.hwnds.begin(), rg.hwnds.end());
                    used_keys.insert(rg.key);
                }
            }
        }
        if (auto bt = prev_bounce.find(di.key); bt != prev_bounce.end()) di.bounce_start_ms = bt->second;
        if (auto cx = prev_draw_cx.find(di.key); cx != prev_draw_cx.end()) di.draw_cx = cx->second;
        next.push_back(std::move(di));
    }

    // 2. Unpinned running taskbar applications (ALL open apps shown; stable insertion order)
    std::unordered_map<std::wstring, const RunningGroup*> unpinned_running;
    for (const auto& rg : running_order) {
        if (!used_keys.count(rg.key)) unpinned_running[rg.key] = &rg;
    }
    unpinned_order.erase(
        std::remove_if(unpinned_order.begin(), unpinned_order.end(),
                       [&](const std::wstring& k) { return unpinned_running.find(k) == unpinned_running.end(); }),
        unpinned_order.end());
    for (const auto& rg : running_order) {
        if (unpinned_running.find(rg.key) != unpinned_running.end() &&
            std::find(unpinned_order.begin(), unpinned_order.end(), rg.key) == unpinned_order.end()) {
            unpinned_order.push_back(rg.key);
        }
    }

    // WacOS / macOS Sonoma+ divider between pinned apps and unpinned running apps (issue #49).
    if (has_pinned_items && !unpinned_order.empty()) {
        DockBarItem sep_run;
        sep_run.kind = DockBarItemKind::Divider;
        sep_run.key = L"special:divider:running";
        next.push_back(std::move(sep_run));
    }

    int extra_running = 0;
    for (const auto& key : unpinned_order) {
        auto it_rg = unpinned_running.find(key);
        if (it_rg == unpinned_running.end()) continue;
        if (extra_running >= 48) break;
        const auto& rg = *it_rg->second;
        used_keys.insert(rg.key);
        ++extra_running;

        DockBarItem di;
        di.kind = DockBarItemKind::App;
        di.key = rg.key;
        di.launch_path = rg.launch_path.empty() ? rg.exe_path : rg.launch_path;
        di.aumid = rg.aumid;
        di.title = rg.title;
        di.pinned = false;
        di.running = true;
        di.active = rg.active;
        di.badge_count = rg.badge_count;
        di.windows = rg.hwnds;
        if (auto bt = prev_bounce.find(di.key); bt != prev_bounce.end()) di.bounce_start_ms = bt->second;
        if (auto cx = prev_draw_cx.find(di.key); cx != prev_draw_cx.end()) di.draw_cx = cx->second;
        next.push_back(std::move(di));
    }

    // 3. Translucent vertical glass divider before right utility shelf
    {
        DockBarItem div;
        div.kind = DockBarItemKind::Divider;
        div.key = L"special:divider";
        next.push_back(std::move(div));
    }

    // 4. 应用程序 stack: the Start menu's apps in a grid (Dock stacks), before 下载 as on the Mac.
    if (apps_stack_enabled) {
        DockBarItem ap;
        ap.kind = DockBarItemKind::Folder;
        ap.key = L"special:apps";
        ap.title = std::wstring(l10n::Get(l10n::StringId::DockStackApps));
        ap.pinned = false; // not a user pin: no drag-to-reorder / drag-out; removed from its menu
        if (auto bt = prev_bounce.find(ap.key); bt != prev_bounce.end()) ap.bounce_start_ms = bt->second;
        if (auto cx = prev_draw_cx.find(ap.key); cx != prev_draw_cx.end()) ap.draw_cx = cx->second;
        next.push_back(std::move(ap));
    }

    // 5. Downloads folder
    if (!downloads_path.empty()) {
        DockBarItem dl;
        dl.kind = DockBarItemKind::Folder;
        dl.key = L"special:downloads";
        dl.launch_path = downloads_path;
        dl.title = L"下载";
        dl.pinned = true;
        if (auto bt = prev_bounce.find(dl.key); bt != prev_bounce.end()) dl.bounce_start_ms = bt->second;
        if (auto cx = prev_draw_cx.find(dl.key); cx != prev_draw_cx.end()) dl.draw_cx = cx->second;
        next.push_back(std::move(dl));
    }

    // 5. Desktop Widgets quick toggle
    {
        DockBarItem wt;
        wt.kind = DockBarItemKind::WidgetsToggle;
        wt.key = L"special:widgets";
        wt.title = L"桌面小组件";
        wt.pinned = true;
        if (auto bt = prev_bounce.find(wt.key); bt != prev_bounce.end()) wt.bounce_start_ms = bt->second;
        if (auto cx = prev_draw_cx.find(wt.key); cx != prev_draw_cx.end()) wt.draw_cx = cx->second;
        next.push_back(std::move(wt));
    }

    // 6. Recycle Bin
    {
        DockBarItem tr;
        tr.kind = DockBarItemKind::Trash;
        tr.key = L"special:trash";
        tr.title = next_trash_not_empty ? L"回收站 (有项目)" : L"回收站";
        tr.pinned = true;
        if (auto bt = prev_bounce.find(tr.key); bt != prev_bounce.end()) tr.bounce_start_ms = bt->second;
        if (auto cx = prev_draw_cx.find(tr.key); cx != prev_draw_cx.end()) tr.draw_cx = cx->second;
        next.push_back(std::move(tr));
    }

    // Smooth icon enter/exit slot expand & collapse tracking (WacOS dock.cpp:298-328).
    if (now_ms > 0.0) {
        auto is_anim_item = [](const DockBarItem& it) noexcept {
            return !it.is_ghost && (it.kind == DockBarItemKind::App ||
                                    (it.kind == DockBarItemKind::Folder && it.key != L"special:downloads"));
        };
        if (built_once) {
            std::set<std::wstring> prev_keys;
            for (const auto& p : prev_snapshot) {
                if (is_anim_item(p)) prev_keys.insert(p.key);
            }
            std::set<std::wstring> cur_keys;
            for (const auto& c : next) {
                if (is_anim_item(c)) cur_keys.insert(c.key);
            }
            for (const auto& c : next) {
                if (is_anim_item(c) && !prev_keys.count(c.key)) {
                    item_appear_ms[c.key] = now_ms;
                    leaving_ghosts.erase(
                        std::remove_if(leaving_ghosts.begin(), leaving_ghosts.end(),
                                       [&](const DockBarLeavingGhost& gh) { return gh.item.key == c.key; }),
                        leaving_ghosts.end());
                }
            }
            for (size_t pi = 0; pi < prev_snapshot.size(); ++pi) {
                const auto& p = prev_snapshot[pi];
                if (!is_anim_item(p) || cur_keys.count(p.key)) continue;
                const bool already = std::any_of(leaving_ghosts.begin(), leaving_ghosts.end(),
                                                 [&](const DockBarLeavingGhost& gh) { return gh.item.key == p.key; });
                if (!already) {
                    DockBarLeavingGhost gh{};
                    gh.item = p;
                    gh.item.is_ghost = true;
                    gh.item.running = false;
                    gh.item.active = false;
                    gh.item.badge_count = 0;
                    gh.slot_pos = pi;
                    gh.start_ms = now_ms;
                    leaving_ghosts.push_back(std::move(gh));
                }
            }
        }
        built_once = true;
        prev_snapshot = next;

        for (auto it = item_appear_ms.begin(); it != item_appear_ms.end();) {
            it = ((now_ms - it->second) >= kSlotTransitionMs + 24.0) ? item_appear_ms.erase(it) : std::next(it);
        }
        leaving_ghosts.erase(
            std::remove_if(leaving_ghosts.begin(), leaving_ghosts.end(),
                           [&](const DockBarLeavingGhost& gh) {
                               return (now_ms - gh.start_ms) >= kSlotTransitionMs + 24.0;
                           }),
            leaving_ghosts.end());

        // Splice shrinking ghost items back into their former slots so neighboring icons glide closed.
        for (const auto& gh : leaving_ghosts) {
            const size_t pos = std::min(gh.slot_pos, next.size());
            next.insert(next.begin() + static_cast<std::ptrdiff_t>(pos), gh.item);
        }
    }

    bool changed = (next_trash_not_empty != trash_not_empty) || (next.size() != items.size());
    if (!changed) {
        for (size_t i = 0; i < next.size(); ++i) {
            if (next[i].key != items[i].key || next[i].running != items[i].running ||
                next[i].active != items[i].active || next[i].pinned != items[i].pinned ||
                next[i].is_ghost != items[i].is_ghost ||
                next[i].badge_count != items[i].badge_count || next[i].title != items[i].title) {
                changed = true;
                break;
            }
        }
    }
    trash_not_empty = next_trash_not_empty;
    items = std::move(next);
    return changed;
}

void LiquidDockBarState::BuildDemoItems(bool trash_full) {
    trash_not_empty = trash_full;
    items.clear();
    auto push = [&](DockBarItemKind kind, const wchar_t* key, const wchar_t* path, const wchar_t* title,
                    bool pinned, bool running, bool active) {
        DockBarItem it{};
        it.kind = kind;
        it.key = key;
        it.launch_path = path;
        it.title = title;
        it.pinned = pinned;
        it.running = running;
        it.active = active;
        items.push_back(std::move(it));
    };

    const std::wstring explorer = ExpandEnv(L"%SystemRoot%\\explorer.exe");
    const std::wstring edge = ExpandEnv(L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe");
    push(DockBarItemKind::App, L"demo:explorer", explorer.c_str(), L"文件资源管理器", true, true, false);
    push(DockBarItemKind::App, L"demo:edge", edge.c_str(), L"Microsoft Edge", true, true, false);
    push(DockBarItemKind::App, L"demo:wt", L"wt.exe", L"终端", true, true, true);
    push(DockBarItemKind::App, L"demo:calc", L"calc.exe", L"计算器", true, false, false);
    push(DockBarItemKind::App, L"ms-settings:", L"ms-settings:", L"系统设置", true, false, false);
    push(DockBarItemKind::Divider, L"special:divider", L"", L"", false, false, false);
    if (apps_stack_enabled) {
        push(DockBarItemKind::Folder, L"special:apps", L"",
             std::wstring(l10n::Get(l10n::StringId::DockStackApps)).c_str(), false, false, false);
    }
    push(DockBarItemKind::Folder, L"special:downloads", L"", L"下载", true, false, false);
    push(DockBarItemKind::WidgetsToggle, L"special:widgets", L"", L"桌面小组件", true, false, false);
    push(DockBarItemKind::Trash, L"special:trash", L"", trash_full ? L"回收站 (有项目)" : L"回收站", true, false, false);
}

void LiquidDockBarState::BuildDemoSystemSnapshot(bool has_battery, bool wired_ethernet) {
    live::DockSystemSnapshot s{};
    s.valid = true;
    s.tray_apps = {
        {L"demo:wechat", L"微信 (3)", L"", nullptr, 0, 0, 0, 3, true, 0xFF07C160u, false},
        {L"demo:cloudmusic", L"网易云音乐", L"", nullptr, 0, 0, 0, 0, true, 0xFFE60026u, false},
        {L"demo:clash", L"Clash Verge", L"", nullptr, 0, 0, 0, 0, false, 0xFF3B82F6u, false},
        {L"demo:everything", L"Everything", L"", nullptr, 0, 0, 0, 0, false, 0xFFFF8C00u, false},
        {L"demo:snipaste", L"Snipaste", L"", nullptr, 0, 0, 0, 0, false, 0xFF00B4D8u, false},
        {L"demo:powertoys", L"PowerToys", L"", nullptr, 0, 0, 0, 0, false, 0xFF0078D4u, false},
    };
    s.ime_chinese = true;
    s.ime_badge = L"中";
    s.ime_layouts = {
        {1, 0x0804, L"中", L"微软拼音", L"简体中文 · 输入法", true},
        {2, 0x0409, L"英", L"英语 (美国)", L"美式键盘", false},
    };
    if (wired_ethernet) {
        s.network_kind = live::DockNetworkKind::Ethernet;
        s.ethernet_connected = true;
        s.ethernet_name = L"以太网 2";
        s.ethernet_desc = L"Realtek Gaming 2.5GbE Family Controller";
        s.ethernet_speed = L"1 Gbps";
        s.ethernet_ipv4 = L"192.168.1.102";
        s.wifi_available = false;
        s.wifi_enabled = false;
        s.wifi_connected = false;
    } else {
        s.network_kind = live::DockNetworkKind::Wifi;
        s.wifi_available = true;
        s.wifi_enabled = true;
        s.wifi_connected = true;
        s.wifi_ssid = L"Studio-5G";
        s.wifi_signal_pct = 86;
        s.wifi_bars = 3;
        s.wifi_networks = {
            {L"Studio-5G", 86, 3, true, true, true, L"Studio-5G"},
            {L"Office-Guest", 62, 2, true, false, false, L""},
            {L"CoffeeLab", 34, 1, false, false, false, L""},
        };
    }
    s.audio_available = true;
    s.volume_pct = 68;
    s.muted = false;
    s.default_audio_name = L"扬声器 (Realtek Audio)";
    s.audio_devices = {
        {L"dev:spk", L"扬声器 (Realtek Audio)", true},
        {L"dev:airpods", L"AirPods Pro", false},
    };
    s.bt_available = !wired_ethernet;
    s.bt_enabled = !wired_ethernet;
    if (s.bt_available) {
        s.bt_devices = {
            {L"AirPods Pro", 80, 1, true, 1},
            {L"MX Master 3S", 45, 2, true, 2},
            {L"Keychron K2", 18, 3, false, 3},
        };
    }
    s.has_battery = has_battery;
    s.battery_pct = 76;
    s.battery_charging = true;
    s.ac_online = true;
    s.battery_remaining_min = 195;
    s.power_mode = 1;
    s.power_plan_name = L"平衡";
    s.time_text = L"23:41";
    s.date_text = L"4/19";
    s.notification_count = 2;
    sys_snap = std::move(s);
}

void LiquidDockBarState::ComputeGeometry(const model::RectI& work_area,
                                         const model::BeautifySettings& settings,
                                         float smooth_cursor_axis,
                                         float hover_presence,
                                         double now_ms,
                                         DockBarDragState* drag,
                                         float dt_sec,
                                         float reveal) noexcept {
    sys_slots.clear();
    sys_capsule_rect = {};
    sys_divider_rect = {};
    sys_area_enabled = false;
    if (items.empty() || work_area.empty()) {
        pill_rect = {};
        rest_pill_w = 0.0f;
        rest_pill_h = 0.0f;
        return;
    }

    dock_position = std::min<uint8_t>(settings.dock_position, 2);
    const bool vertical = IsVertical();
    reveal_progress = std::clamp(reveal, 0.0f, 1.0f);

    const float wa_left = static_cast<float>(work_area.x);
    const float wa_top = static_cast<float>(work_area.y);
    const float wa_w = static_cast<float>(work_area.w);
    const float wa_h = static_cast<float>(work_area.h);
    const float wa_right = static_cast<float>(work_area.right());
    const float wa_bottom = static_cast<float>(work_area.bottom());
    const float center_x = wa_left + wa_w * 0.5f;
    const float center_y = wa_top + wa_h * 0.5f;

    // Build .dk-sys slot blueprint when enabled (desktop_widgets_redesign.html)
    struct SysSlotSpec {
        DockSysSlotKind kind;
        int tray_index;
        float len;
        std::wstring tooltip;
    };
    std::vector<SysSlotSpec> sys_specs;
    const float sys_div_len = 14.0f;
    const float sys_pad = 6.0f;
    const float sys_slot_gap = 2.0f;
    float sys_capsule_len = 0.0f;
    float sys_total_len = 0.0f;
    if (settings.dock_system_area) {
        sys_area_enabled = true;
        int pinned_shown = 0;
        for (size_t ti = 0; ti < sys_snap.tray_apps.size() && pinned_shown < 3; ++ti) {
            if (!sys_snap.tray_apps[ti].pinned) continue;
            sys_specs.push_back({DockSysSlotKind::PinnedTrayApp, static_cast<int>(ti),
                                 26.0f, sys_snap.tray_apps[ti].title});
            ++pinned_shown;
        }
        sys_specs.push_back({DockSysSlotKind::TrayOverflow, -1, vertical ? 22.0f : 22.0f, L"托盘图标"});
        const std::wstring ime_tip = L"输入法 · " + (sys_snap.ime_badge.empty() ? L"英" : sys_snap.ime_badge) + L"（右键切换）";
        sys_specs.push_back({DockSysSlotKind::Ime, -1, vertical ? 24.0f : 26.0f, ime_tip});
        std::wstring net_tip;
        if (sys_snap.network_kind == live::DockNetworkKind::Ethernet || sys_snap.ethernet_connected) {
            net_tip = sys_snap.ethernet_name.empty() ? L"以太网" : sys_snap.ethernet_name;
            net_tip += L" · 已连接";
            if (!sys_snap.ethernet_speed.empty()) {
                net_tip += L"（" + sys_snap.ethernet_speed + L"）";
            }
        } else if (sys_snap.wifi_connected && !sys_snap.wifi_ssid.empty()) {
            net_tip = L"Wi-Fi · " + sys_snap.wifi_ssid;
        } else if (sys_snap.wifi_available) {
            net_tip = sys_snap.wifi_enabled ? L"Wi-Fi 未连接" : L"Wi-Fi 已关闭";
        } else {
            net_tip = L"网络 · 未连接";
        }
        sys_specs.push_back({DockSysSlotKind::Wifi, -1, vertical ? 24.0f : 26.0f, net_tip});
        std::wstring vol_tip = (sys_snap.muted || sys_snap.volume_pct <= 0)
            ? L"音量已静音（滚轮调节）"
            : (L"音量 " + std::to_wstring(sys_snap.volume_pct) + L"%（滚轮调节）");
        sys_specs.push_back({DockSysSlotKind::Volume, -1, vertical ? 24.0f : 26.0f, vol_tip});
        // Desktop PCs without Bluetooth hardware hide the Bluetooth slot automatically.
        if (sys_snap.bt_available || !sys_snap.bt_devices.empty()) {
            std::wstring bt_tip = !sys_snap.bt_enabled ? L"蓝牙已关闭" : L"蓝牙";
            if (sys_snap.bt_enabled) {
                int conn = 0;
                for (const auto& d : sys_snap.bt_devices) conn += d.connected ? 1 : 0;
                if (conn > 0) bt_tip = L"蓝牙 · 已连接 " + std::to_wstring(conn) + L" 台设备";
            }
            sys_specs.push_back({DockSysSlotKind::Bluetooth, -1, vertical ? 22.0f : 24.0f, bt_tip});
        }
        // Desktop PCs without a battery hide the battery slot completely (design rule).
        if (sys_snap.has_battery) {
            std::wstring bat_tip = L"电池 " + std::to_wstring(sys_snap.battery_pct) + L"%";
            if (sys_snap.battery_charging) bat_tip += L" · 正在充电";
            sys_specs.push_back({DockSysSlotKind::Battery, -1, vertical ? 24.0f : 30.0f, bat_tip});
        }
        sys_specs.push_back({DockSysSlotKind::Clock, -1, vertical ? 34.0f : 48.0f, L"日历与通知"});

        float sum = sys_pad * 2.0f;
        for (size_t i = 0; i < sys_specs.size(); ++i) {
            if (i > 0) sum += sys_slot_gap;
            sum += sys_specs[i].len;
        }
        sys_capsule_len = sum;
        sys_total_len = sys_div_len + sys_capsule_len;
    }

    float base = std::clamp(static_cast<float>(settings.dock_icon_size), 36.0f, 64.0f);
    const float div_len = 14.0f;
    const float pad_end = 14.0f;
    const float wa_span = vertical ? wa_h : wa_w;
    const float max_pill_len = std::max(180.0f, wa_span - 40.0f);

    // Gracefully shrink base icon size if many apps are pinned so no icon or .dk-sys ever clips.
    int icon_count = 0;
    int div_count = 0;
    for (const auto& it : items) {
        if (it.kind == DockBarItemKind::Divider) ++div_count;
        else ++icon_count;
    }
    const int gap_count = std::max(0, static_cast<int>(items.size()) - 1);
    const float est_gap = std::round(base * 0.21f);
    const float est_rest_len = static_cast<float>(icon_count) * base +
                               static_cast<float>(div_count) * div_len +
                               static_cast<float>(gap_count) * est_gap +
                               sys_total_len + (sys_area_enabled ? est_gap : 0.0f) +
                               pad_end * 2.0f;
    if (est_rest_len > max_pill_len && icon_count > 0) {
        const float avail = std::max(100.0f, max_pill_len - pad_end * 2.0f -
                                     static_cast<float>(div_count) * div_len - sys_total_len);
        const float per_unit = static_cast<float>(icon_count) + static_cast<float>(gap_count + (sys_area_enabled ? 1 : 0)) * 0.21f;
        base = std::clamp(avail / std::max(1.0f, per_unit), 20.0f, base);
    }
    effective_base_icon = base;

    const float gap = std::round(base * 0.21f);
    const float pill_thick = base + 20.0f;
    pill_radius = std::min(pill_thick * 0.31f, 22.0f);
    const float edge_margin = 12.0f;
    // Smooth macOS EaseInOutCubic edge slide for Dock collapse (reveal=0) <-> expand (reveal=1).
    const float hide_slide = (1.0f - EaseInOutCubic01(reveal_progress)) * (pill_thick + edge_margin + 18.0f);

    // Pass 1: Resting centers along primary axis + smooth enter/exit slot factor
    float rest_content_len = 0.0f;
    for (size_t i = 0; i < items.size(); ++i) {
        auto& it = items[i];
        float sf = 1.0f;
        if (now_ms > 0.0) {
            if (it.is_ghost) {
                sf = 0.0f;
                for (const auto& gh : leaving_ghosts) {
                    if (gh.item.key == it.key) {
                        const float u = static_cast<float>((now_ms - gh.start_ms) / kSlotTransitionMs);
                        sf = 1.0f - EaseOutCubic01(u);
                        break;
                    }
                }
            } else if (auto ait = item_appear_ms.find(it.key); ait != item_appear_ms.end()) {
                const float u = static_cast<float>((now_ms - ait->second) / kSlotTransitionMs);
                sf = EaseOutCubic01(u);
            }
        }
        it.slot_factor = std::clamp(sf, 0.0f, 1.0f);
        if (i > 0) rest_content_len += gap * std::max(it.slot_factor, items[i - 1].slot_factor);
        rest_content_len += (it.kind == DockBarItemKind::Divider) ? div_len : (base * it.slot_factor);
    }
    if (sys_area_enabled) {
        rest_content_len += gap + sys_total_len;
    }
    const float rest_len = rest_content_len + pad_end * 2.0f;
    rest_pill_w = vertical ? pill_thick : rest_len;
    rest_pill_h = vertical ? rest_len : pill_thick;

    const float axis_center = vertical ? center_y : center_x;
    const float cross_rest_center = (dock_position == 1)
        ? (wa_left + edge_margin + pill_thick * 0.5f)
        : ((dock_position == 2) ? (wa_right - edge_margin - pill_thick * 0.5f)
                                : (wa_bottom - edge_margin - pill_thick * 0.5f));
    float ra = axis_center - rest_content_len * 0.5f;
    for (size_t i = 0; i < items.size(); ++i) {
        const float w = (items[i].kind == DockBarItemKind::Divider) ? div_len : (base * items[i].slot_factor);
        if (vertical) {
            items[i].rest_cx = cross_rest_center;
            items[i].rest_cy = ra + w * 0.5f;
        } else {
            items[i].rest_cx = ra + w * 0.5f;
            items[i].rest_cy = cross_rest_center;
        }
        ra += w + gap * items[i].slot_factor;
    }

    // Check active drag-to-reorder / drag-out-to-unpin (modeled on WacOS dock.cpp:412-467)
    const bool dragging = (drag != nullptr && drag->active &&
                           drag->source_index >= 0 &&
                           static_cast<size_t>(drag->source_index) < items.size());
    int drag_idx = dragging ? drag->source_index : -1;
    if (drag) drag->target_pinned_slot = -1;

    // Pass 2: Smooth macOS raised-cosine fisheye magnification wave (paused during drag-reorder or collapse)
    const float presence = (dragging || reveal_progress < 0.7f) ? 0.0f : std::clamp(hover_presence, 0.0f, 1.0f);
    const float max_scale = settings.dock_magnification
        ? std::clamp(static_cast<float>(settings.dock_max_scale_x100) / 100.0f, 1.0f, 1.95f)
        : 1.0f;
    const float influence = base * 2.45f;
    const float axis_min = vertical ? wa_top : wa_left;

    for (size_t i = 0; i < items.size(); ++i) {
        auto& it = items[i];
        float sc = 1.0f;
        if (!it.is_ghost && presence > 0.001f && smooth_cursor_axis >= axis_min && max_scale > 1.01f) {
            const float item_rest = vertical ? it.rest_cy : it.rest_cx;
            const float dist = std::fabs(smooth_cursor_axis - item_rest);
            if (dist < influence) {
                const float u = dist / influence;
                const float wave = 0.5f * (1.0f + std::cos(kPi * u));
                if (it.kind == DockBarItemKind::Divider) {
                    sc = 1.0f + presence * 0.24f * std::pow(wave, 1.35f);
                } else {
                    sc = 1.0f + presence * (max_scale - 1.0f) * std::pow(wave, 1.35f);
                }
            }
        }
        it.scale = sc;
        it.draw_size = (it.kind == DockBarItemKind::Divider) ? (div_len * sc) : (base * sc * it.slot_factor);
    }

    // Build visual slot order (with live gap opening during drag-to-reorder / drag-out)
    std::vector<int> order;
    order.reserve(items.size());
    for (int i = 0; i < static_cast<int>(items.size()); ++i) order.push_back(i);

    auto is_user_pinned_item = [&](int idx) -> bool {
        const auto& it = items[static_cast<size_t>(idx)];
        return it.pinned &&
               (it.kind == DockBarItemKind::App ||
                (it.kind == DockBarItemKind::Folder && it.key != L"special:downloads"));
    };

    if (dragging && drag_idx >= 0) {
        const bool src_pinned = is_user_pinned_item(drag_idx);
        const float drag_axis = vertical ? drag->cursor_y : drag->cursor_x;
        order.erase(std::remove(order.begin(), order.end(), drag_idx), order.end());

        int first_pin = -1;
        int last_pin = -1;
        for (int k = 0; k < static_cast<int>(order.size()); ++k) {
            if (is_user_pinned_item(order[static_cast<size_t>(k)])) {
                if (first_pin < 0) first_pin = k;
                last_pin = k;
            }
        }

        const bool keep_in_rest = (!src_pinned && drag->drag_out);
        bool far_from_pinned = false;
        if (!src_pinned && !drag->drag_out && last_pin >= 0) {
            const auto& last_it = items[static_cast<size_t>(order[static_cast<size_t>(last_pin)])];
            const float pin_end_edge = (vertical ? last_it.rest_cy : last_it.rest_cx) + base * 0.9f;
            far_from_pinned = (drag_axis > pin_end_edge);
        }

        if (keep_in_rest || far_from_pinned) {
            order.insert(order.begin() + std::min(drag_idx, static_cast<int>(order.size())), drag_idx);
        } else if (!drag->drag_out) {
            int insert_pos = -1;
            int pinned_before = 0;
            for (int k = 0; k < static_cast<int>(order.size()); ++k) {
                const int oi = order[static_cast<size_t>(k)];
                if (!is_user_pinned_item(oi)) continue;
                const float oi_rest = vertical ? items[static_cast<size_t>(oi)].rest_cy
                                               : items[static_cast<size_t>(oi)].rest_cx;
                if (insert_pos < 0 && drag_axis < oi_rest) {
                    insert_pos = k;
                    break;
                }
                ++pinned_before;
            }
            if (first_pin < 0) {
                insert_pos = 0;
                pinned_before = 0;
            } else if (insert_pos < 0) {
                insert_pos = last_pin + 1;
            }
            order.insert(order.begin() + std::clamp(insert_pos, 0, static_cast<int>(order.size())), drag_idx);
            drag->target_pinned_slot = pinned_before;
        }
    }

    float live_content_len = 0.0f;
    for (size_t k = 0; k < order.size(); ++k) {
        const auto& it = items[static_cast<size_t>(order[k])];
        if (k > 0) {
            const auto& prev = items[static_cast<size_t>(order[k - 1])];
            const float pair_scale = 0.5f * (prev.scale + it.scale);
            live_content_len += gap * (0.72f + 0.28f * pair_scale);
        }
        live_content_len += it.draw_size;
    }
    if (sys_area_enabled) {
        live_content_len += gap + sys_total_len;
    }

    // Dynamically expand/contract the glass capsule around center along primary axis
    const float pill_len = std::min(wa_span - 24.0f, live_content_len + pad_end * 2.0f);
    if (dock_position == 1) {
        const float pill_left = wa_left + edge_margin - hide_slide;
        const float pill_top = center_y - pill_len * 0.5f;
        pill_rect = D2D1::RectF(pill_left, pill_top, pill_left + pill_thick, pill_top + pill_len);
    } else if (dock_position == 2) {
        const float pill_right = wa_right - edge_margin + hide_slide;
        const float pill_top = center_y - pill_len * 0.5f;
        pill_rect = D2D1::RectF(pill_right - pill_thick, pill_top, pill_right, pill_top + pill_len);
    } else {
        const float pill_top = wa_bottom - edge_margin - pill_thick + hide_slide;
        const float pill_left = center_x - pill_len * 0.5f;
        pill_rect = D2D1::RectF(pill_left, pill_top, pill_left + pill_len, pill_top + pill_thick);
    }

    const float slide_alpha = 1.0f - std::exp(-std::clamp(dt_sec, 0.001f, 0.05f) * 24.0f);
    float pos = axis_center - live_content_len * 0.5f;
    for (size_t k = 0; k < order.size(); ++k) {
        auto& it = items[static_cast<size_t>(order[k])];
        if (k > 0) {
            const auto& prev = items[static_cast<size_t>(order[k - 1])];
            const float pair_scale = 0.5f * (prev.scale + it.scale);
            pos += gap * (0.72f + 0.28f * pair_scale);
        }
        const float target_axis = pos + it.draw_size * 0.5f;
        const float lift = (it.kind == DockBarItemKind::Divider) ? 0.0f : ((it.scale - 1.0f) * (base * 0.16f));

        float bounce = 0.0f;
        if (it.bounce_start_ms > 0.0) {
            const float elapsed_sec = static_cast<float>((now_ms - it.bounce_start_ms) * 0.001);
            if (elapsed_sec >= 0.0f && elapsed_sec < kBounceHopDurSec * kBounceHops) {
                const int hop = static_cast<int>(elapsed_sec / kBounceHopDurSec);
                const float u = (elapsed_sec - static_cast<float>(hop) * kBounceHopDurSec) / kBounceHopDurSec;
                const float amp = 24.0f * std::pow(0.58f, static_cast<float>(hop));
                bounce = amp * 4.0f * u * (1.0f - u);
            } else if (elapsed_sec >= kBounceHopDurSec * kBounceHops) {
                it.bounce_start_ms = -10000.0;
            }
        }

        if (vertical) {
            if (dragging && it.draw_cy > 0.0f) {
                it.draw_cy += (target_axis - it.draw_cy) * slide_alpha;
            } else {
                it.draw_cy = target_axis;
            }
            if (it.kind == DockBarItemKind::Divider) {
                it.draw_cx = (pill_rect.left + pill_rect.right) * 0.5f;
            } else if (dock_position == 1) {
                const float base_left = pill_rect.left + 12.5f;
                it.draw_cx = base_left + it.draw_size * 0.5f + lift + bounce;
            } else {
                const float base_right = pill_rect.right - 12.5f;
                it.draw_cx = base_right - it.draw_size * 0.5f - lift - bounce;
            }
        } else {
            if (dragging && it.draw_cx > 0.0f) {
                it.draw_cx += (target_axis - it.draw_cx) * slide_alpha;
            } else {
                it.draw_cx = target_axis;
            }
            if (it.kind == DockBarItemKind::Divider) {
                it.draw_cy = (pill_rect.top + pill_rect.bottom) * 0.5f;
            } else {
                const float baseline_bottom = pill_rect.bottom - 12.5f;
                it.draw_cy = baseline_bottom - it.draw_size * 0.5f - lift - bounce;
            }
        }
        pos += it.draw_size;
    }

    // Pass 3: Lay out the quieter unified .dk-sys system status & tray capsule (no fisheye distortion)
    if (sys_area_enabled && !sys_specs.empty()) {
        pos += gap;
        if (vertical) {
            const float div_y = pos + sys_div_len * 0.5f;
            sys_divider_rect = D2D1::RectF(pill_rect.left + 13.0f, div_y, pill_rect.right - 13.0f, div_y + 1.0f);
            pos += sys_div_len;
            const float cap_w = std::clamp(pill_thick - 16.0f, 40.0f, 52.0f);
            const float cap_left = (pill_rect.left + pill_rect.right - cap_w) * 0.5f;
            sys_capsule_rect = D2D1::RectF(cap_left, pos, cap_left + cap_w, pos + sys_capsule_len);
            float sy = sys_capsule_rect.top + sys_pad;
            for (const auto& sp : sys_specs) {
                sys_slots.push_back({sp.kind, sp.tray_index,
                                     D2D1::RectF(cap_left + 3.0f, sy, cap_left + cap_w - 3.0f, sy + sp.len),
                                     sp.tooltip});
                sy += sp.len + sys_slot_gap;
            }
        } else {
            const float div_x = pos + sys_div_len * 0.5f;
            sys_divider_rect = D2D1::RectF(div_x, pill_rect.top + 13.0f, div_x + 1.0f, pill_rect.bottom - 13.0f);
            pos += sys_div_len;
            const float cap_h = std::clamp(pill_thick - 22.0f, 36.0f, 46.0f);
            const float cap_top = (pill_rect.top + pill_rect.bottom - cap_h) * 0.5f;
            sys_capsule_rect = D2D1::RectF(pos, cap_top, pos + sys_capsule_len, cap_top + cap_h);
            float sx = sys_capsule_rect.left + sys_pad;
            for (const auto& sp : sys_specs) {
                sys_slots.push_back({sp.kind, sp.tray_index,
                                     D2D1::RectF(sx, cap_top + 3.0f, sx + sp.len, cap_top + cap_h - 3.0f),
                                     sp.tooltip});
                sx += sp.len + sys_slot_gap;
            }
        }
    }

    if (dragging && drag_idx >= 0 && drag->drag_out && is_user_pinned_item(drag_idx)) {
        items[static_cast<size_t>(drag_idx)].draw_cx = -1000.0f;
        items[static_cast<size_t>(drag_idx)].draw_cy = -1000.0f;
    }
}

bool LiquidDockBarState::HitTestBar(float x_dip, float y_dip, bool currently_hovering) const noexcept {
    if (pill_rect.right <= pill_rect.left || pill_rect.bottom <= pill_rect.top) return false;
    const float grow_pad = currently_hovering ? 52.0f : 10.0f;
    if (dock_position == 1) {
        return x_dip >= pill_rect.left - 14.0f && x_dip <= pill_rect.right + grow_pad &&
               y_dip >= pill_rect.top - 10.0f && y_dip <= pill_rect.bottom + 10.0f;
    }
    if (dock_position == 2) {
        return x_dip >= pill_rect.left - grow_pad && x_dip <= pill_rect.right + 14.0f &&
               y_dip >= pill_rect.top - 10.0f && y_dip <= pill_rect.bottom + 10.0f;
    }
    return x_dip >= pill_rect.left - 10.0f && x_dip <= pill_rect.right + 10.0f &&
           y_dip >= pill_rect.top - grow_pad && y_dip <= pill_rect.bottom + 14.0f;
}

int LiquidDockBarState::HitTestDivider(float x_dip, float y_dip) const noexcept {
    if (pill_rect.right <= pill_rect.left || pill_rect.bottom <= pill_rect.top) return -1;
    if (IsVertical()) {
        if (x_dip < pill_rect.left - 4.0f || x_dip > pill_rect.right + 4.0f) return -1;
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& it = items[i];
            if (it.kind != DockBarItemKind::Divider) continue;
            if (std::fabs(y_dip - it.draw_cy) <= 7.5f) return static_cast<int>(i);
        }
        return -1;
    }
    if (y_dip < pill_rect.top - 2.0f || y_dip > pill_rect.bottom + 4.0f) return -1;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        if (it.kind != DockBarItemKind::Divider) continue;
        if (std::fabs(x_dip - it.draw_cx) <= 7.5f) return static_cast<int>(i);
    }
    return -1;
}

bool LiquidDockBarState::IsPointInSysArea(float x_dip, float y_dip) const noexcept {
    if (!sys_area_enabled || reveal_progress < 0.25f) return false;
    if (sys_capsule_rect.right <= sys_capsule_rect.left || sys_capsule_rect.bottom <= sys_capsule_rect.top) return false;
    return x_dip >= sys_capsule_rect.left - 4.0f && x_dip <= sys_capsule_rect.right + 4.0f &&
           y_dip >= sys_capsule_rect.top - 4.0f && y_dip <= sys_capsule_rect.bottom + 4.0f;
}

const DockSysSlotRect* LiquidDockBarState::HitTestSysSlot(float x_dip, float y_dip) const noexcept {
    if (!sys_area_enabled || reveal_progress < 0.25f) return nullptr;
    for (const auto& s : sys_slots) {
        if (x_dip >= s.rect.left - 1.5f && x_dip <= s.rect.right + 1.5f &&
            y_dip >= s.rect.top - 2.0f && y_dip <= s.rect.bottom + 2.0f) {
            return &s;
        }
    }
    return nullptr;
}

const DockSysSlotRect* LiquidDockBarState::FindSysSlot(DockSysSlotKind kind, int tray_index) const noexcept {
    for (const auto& s : sys_slots) {
        if (s.kind == kind && (kind != DockSysSlotKind::PinnedTrayApp || s.tray_index == tray_index)) {
            return &s;
        }
    }
    return nullptr;
}

int LiquidDockBarState::HitTestItem(float x_dip, float y_dip) const noexcept {
    if (reveal_progress < 0.25f) return -1;
    if (pill_rect.right <= pill_rect.left || pill_rect.bottom <= pill_rect.top) return -1;
    if (IsPointInSysArea(x_dip, y_dip)) return -1;

    int best = -1;
    float best_dist = 1e9f;
    if (IsVertical()) {
        const bool left_dock = (dock_position == 1);
        if (y_dip < pill_rect.top - 10.0f || y_dip > pill_rect.bottom + 10.0f) return -1;
        if (left_dock && (x_dip < pill_rect.left - 14.0f || x_dip > pill_rect.right + 52.0f)) return -1;
        if (!left_dock && (x_dip < pill_rect.left - 52.0f || x_dip > pill_rect.right + 14.0f)) return -1;
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& it = items[i];
            if (it.kind == DockBarItemKind::Divider || it.is_ghost || it.slot_factor < 0.25f) continue;
            const float half = it.draw_size * 0.5f + 6.0f;
            const bool in_y = (y_dip >= it.draw_cy - half && y_dip <= it.draw_cy + half);
            const bool in_x = left_dock
                ? (x_dip >= pill_rect.left - 14.0f && x_dip <= it.draw_cx + half + 6.0f)
                : (x_dip >= it.draw_cx - half - 6.0f && x_dip <= pill_rect.right + 14.0f);
            if (in_x && in_y) {
                const float d = std::fabs(y_dip - it.draw_cy);
                if (d < best_dist) {
                    best_dist = d;
                    best = static_cast<int>(i);
                }
            }
        }
        return best;
    }

    if (y_dip < pill_rect.top - 52.0f || y_dip > pill_rect.bottom + 14.0f) return -1;
    if (x_dip < pill_rect.left - 10.0f || x_dip > pill_rect.right + 10.0f) return -1;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        if (it.kind == DockBarItemKind::Divider || it.is_ghost || it.slot_factor < 0.25f) continue;
        const float half = it.draw_size * 0.5f + 6.0f;
        if (x_dip >= it.draw_cx - half && x_dip <= it.draw_cx + half &&
            y_dip >= it.draw_cy - half - 6.0f && y_dip <= pill_rect.bottom + 14.0f) {
            const float d = std::fabs(x_dip - it.draw_cx);
            if (d < best_dist) {
                best_dist = d;
                best = static_cast<int>(i);
            }
        }
    }
    return best;
}

namespace {

void DrawDockItemArtwork(ID2D1DeviceContext* dc,
                         ID2D1SolidColorBrush* br,
                         DockBarIconCache& icon_cache,
                         const DockBarItem& it,
                         bool trash_not_empty,
                         const D2D1_RECT_F& icon_rect,
                         float icon_op,
                         const DockBarTextFn& draw_text) noexcept {
    const std::wstring base_exe = ToLower(BaseNameNoExt(it.launch_path));
    const std::wstring family = (it.kind == DockBarItemKind::App)
        ? CanonicalAppFamily(it.launch_path, it.aumid)
        : std::wstring{};
    if (it.kind == DockBarItemKind::WidgetsToggle) {
        DrawDockWidgetsVectorIcon(dc, icon_rect);
    } else if (it.key == L"special:apps") {
        DrawAppsStackVectorIcon(dc, br, icon_rect);
    } else if (it.key == L"ms-settings:" || family == L"ms-settings:") {
        DrawDockSettingsVectorIcon(dc, icon_rect);
    } else if (it.kind == DockBarItemKind::App &&
               (family == L"alias:terminal" || base_exe == L"wt" || base_exe == L"windowsterminal" ||
                base_exe == L"powershell" || base_exe == L"pwsh")) {
        DrawDockTerminalVectorIcon(dc, icon_rect);
    } else if (it.kind == DockBarItemKind::App &&
               (family == L"alias:calculator" || base_exe == L"calc" || base_exe == L"calculatorapp")) {
        DrawDockCalculatorVectorIcon(dc, icon_rect);
    } else if (ID2D1Bitmap* bmp = icon_cache.GetOrLoad(dc, it, trash_not_empty)) {
        dc->DrawBitmap(bmp, &icon_rect, icon_op, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
    } else if (it.kind == DockBarItemKind::Trash) {
        DrawTrashVectorIcon(dc, br, icon_rect, trash_not_empty);
    } else if (it.kind == DockBarItemKind::Folder) {
        DrawDownloadsVectorIcon(dc, br, icon_rect);
    } else if (base_exe == L"explorer") {
        DrawExplorerVectorIcon(dc, br, icon_rect);
    } else {
        DrawGenericAppSquircle(dc, br, icon_rect, it.title, draw_text);
    }
}

void DrawDockBadgePill(ID2D1DeviceContext* dc,
                       ID2D1SolidColorBrush* br,
                       const D2D1_RECT_F& icon_rect,
                       int count,
                       const DockBarTextFn& draw_text) noexcept {
    if (!dc || !br || count <= 0) return;
    const float sz = icon_rect.right - icon_rect.left;
    const float d = std::clamp(sz * 0.36f, 15.0f, 24.0f);
    const std::wstring label = (count > 99) ? L"99+" : std::to_wstring(count);
    const float pill_w = (label.size() <= 1) ? d : std::max(d, d + static_cast<float>(label.size() - 1) * (d * 0.36f));
    const float bx = icon_rect.right - d * 0.22f;
    const float by = icon_rect.top + d * 0.22f;
    const D2D1_RECT_F r{bx - pill_w * 0.5f, by - d * 0.5f, bx + pill_w * 0.5f, by + d * 0.5f};

    br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.24f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + 1.2f, r.right, r.bottom + 1.5f), d * 0.5f, d * 0.5f), br);
    br->SetColor(D2D1::ColorF(1.0f, 0.231f, 0.188f, 0.98f)); // macOS #FF3B30
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, d * 0.5f, d * 0.5f), br);
    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.32f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(r, d * 0.5f, d * 0.5f), br, 0.7f);
    if (draw_text) {
        draw_text(label, D2D1::RectF(r.left, r.top - 0.5f, r.right, r.bottom - 0.5f),
                  std::max(9.5f, d * 0.56f), true, 0xFFFFFFFFu, 1.0f);
    }
}

D2D1_COLOR_F ArgbToColorF(uint32_t argb, float mul_alpha = 1.0f) noexcept {
    const float a = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
    const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(argb & 0xFFu) / 255.0f;
    return D2D1::ColorF(r, g, b, std::clamp(a * mul_alpha, 0.0f, 1.0f));
}

void DrawSysEthernetGlyph(ID2D1Factory* factory, ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
                          ID2D1StrokeStyle* round_st, float cx, float cy,
                          bool connected, uint32_t pri_argb, uint32_t dim_argb, float alpha) noexcept {
    // Crisp Windows-style wired Ethernet glyph: monitor screen + stand on left, RJ45 plug & cable on right.
    const uint32_t col = connected ? pri_argb : dim_argb;
    br->SetColor(ArgbToColorF(col, alpha));
    const float stroke = 1.45f;

    const float ml = cx - 8.2f;
    const float mt = cy - 5.4f;
    const float mr = cx + 2.4f;
    const float mb = cy + 3.2f;
    const float plug_l = cx + 3.2f;
    const float plug_r = cx + 7.8f;
    const float plug_t = cy - 5.6f;
    const float plug_b = cy - 0.4f;
    const float plug_cx = (plug_l + plug_r) * 0.5f;

    // Monitor open frame leaving clean visual gap beside the RJ45 plug (matches Windows 11 taskbar Ethernet icon)
    if (factory) {
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sk;
        if (SUCCEEDED(factory->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sk))) {
            sk->BeginFigure(D2D1::Point2F(mr - 0.6f, mt), D2D1_FIGURE_BEGIN_HOLLOW);
            sk->AddLine(D2D1::Point2F(ml + 1.4f, mt));
            sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(ml, mt + 1.4f), D2D1::SizeF(1.4f, 1.4f), 0.0f,
                                        D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sk->AddLine(D2D1::Point2F(ml, mb - 1.4f));
            sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(ml + 1.4f, mb), D2D1::SizeF(1.4f, 1.4f), 0.0f,
                                        D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sk->AddLine(D2D1::Point2F(mr - 0.6f, mb));
            sk->EndFigure(D2D1_FIGURE_END_OPEN);
            sk->Close();
            dc->DrawGeometry(geo.Get(), br, stroke, round_st);
        }
    } else {
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ml, mt, mr, mb), 1.6f, 1.6f), br, stroke);
    }

    // Monitor stand neck + horizontal base
    const float mcx = (ml + mr) * 0.5f - 0.3f;
    dc->DrawLine(D2D1::Point2F(mcx, mb + 0.2f), D2D1::Point2F(mcx, cy + 5.8f), br, stroke, round_st);
    dc->DrawLine(D2D1::Point2F(mcx - 3.2f, cy + 5.8f), D2D1::Point2F(mcx + 3.2f, cy + 5.8f), br, stroke, round_st);

    // RJ45 plug housing + internal clip line + vertical cable descending to base level
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(plug_l, plug_t, plug_r, plug_b), 1.3f, 1.3f), br, 1.35f);
    dc->DrawLine(D2D1::Point2F(plug_l + 1.1f, plug_t + 2.1f), D2D1::Point2F(plug_r - 1.1f, plug_t + 2.1f), br, 1.15f, round_st);
    dc->DrawLine(D2D1::Point2F(plug_cx, plug_b + 0.2f), D2D1::Point2F(plug_cx, cy + 5.8f), br, stroke, round_st);

    if (!connected) {
        br->SetColor(ArgbToColorF(0xFFFF453Au, alpha * 0.92f));
        dc->DrawLine(D2D1::Point2F(cx - 6.8f, cy - 5.2f), D2D1::Point2F(cx + 6.8f, cy + 5.6f), br, 1.5f, round_st);
    }
}

std::wstring FirstVisibleDockChar(const std::wstring& s) {
    for (wchar_t ch : s) {
        if (ch <= 0x0020 || ch == 0x00A0 || ch == 0xFEFF || ch == 0x3000) continue;
        if ((ch >= 0x200B && ch <= 0x200F) ||
            (ch >= 0x202A && ch <= 0x202E) ||
            (ch >= 0x2060 && ch <= 0x206F)) continue;
        return std::wstring(1, ch);
    }
    return L"•";
}

void DrawSysWifiGlyph(ID2D1Factory* factory, ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
                      ID2D1StrokeStyle* round_st, float cx, float cy,
                      bool enabled, bool connected, int bars,
                      uint32_t pri_argb, uint32_t dim_argb, float alpha) noexcept {
    const float dot_y = cy + 4.8f;
    const int lit = (!enabled || !connected) ? 0 : std::clamp(bars, 0, 3);
    br->SetColor(ArgbToColorF(lit >= 1 ? pri_argb : dim_argb, alpha));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, dot_y), 1.35f, 1.35f), br);

    if (factory) {
        constexpr float kRadii[3] = {4.0f, 7.1f, 10.2f};
        constexpr float kHalfAng = 0.74f; // ~42 deg each side of top
        for (int i = 0; i < 3; ++i) {
            const float r = kRadii[i];
            const float x0 = cx - r * std::sin(kHalfAng);
            const float y0 = dot_y - r * std::cos(kHalfAng);
            const float x1 = cx + r * std::sin(kHalfAng);
            const float y1 = y0;
            ComPtr<ID2D1PathGeometry> geo;
            ComPtr<ID2D1GeometrySink> sk;
            if (SUCCEEDED(factory->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sk))) {
                sk->BeginFigure(D2D1::Point2F(x0, y0), D2D1_FIGURE_BEGIN_HOLLOW);
                sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(x1, y1), D2D1::SizeF(r, r), 0.0f,
                                            D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sk->EndFigure(D2D1_FIGURE_END_OPEN);
                sk->Close();
                br->SetColor(ArgbToColorF(lit > i ? pri_argb : dim_argb, alpha));
                dc->DrawGeometry(geo.Get(), br, 1.55f, round_st);
            }
        }
    }
    if (!enabled) {
        br->SetColor(ArgbToColorF(pri_argb, alpha * 0.85f));
        dc->DrawLine(D2D1::Point2F(cx - 6.5f, cy - 5.5f), D2D1::Point2F(cx + 6.5f, cy + 5.8f), br, 1.5f, round_st);
    }
}

void DrawSysVolumeGlyph(ID2D1Factory* factory, ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
                        ID2D1StrokeStyle* round_st, float cx, float cy,
                        int vol_pct, bool muted,
                        uint32_t pri_argb, uint32_t dim_argb, float alpha) noexcept {
    // Speaker body shifted slightly left to leave clean room for waves or mute cross
    const float sx = cx - 2.2f;
    if (factory) {
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sk;
        if (SUCCEEDED(factory->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sk))) {
            sk->BeginFigure(D2D1::Point2F(sx - 5.0f, cy - 2.4f), D2D1_FIGURE_BEGIN_FILLED);
            sk->AddLine(D2D1::Point2F(sx - 2.2f, cy - 2.4f));
            sk->AddLine(D2D1::Point2F(sx + 1.4f, cy - 5.2f));
            sk->AddLine(D2D1::Point2F(sx + 1.4f, cy + 5.2f));
            sk->AddLine(D2D1::Point2F(sx - 2.2f, cy + 2.4f));
            sk->AddLine(D2D1::Point2F(sx - 5.0f, cy + 2.4f));
            sk->EndFigure(D2D1_FIGURE_END_CLOSED);
            sk->Close();
            br->SetColor(ArgbToColorF(muted ? dim_argb : pri_argb, alpha));
            dc->FillGeometry(geo.Get(), br);
        }
    }
    if (muted || vol_pct <= 0) {
        br->SetColor(ArgbToColorF(0xFFFF453Au, alpha * 0.95f));
        dc->DrawLine(D2D1::Point2F(sx + 3.8f, cy - 2.8f), D2D1::Point2F(sx + 8.6f, cy + 2.8f), br, 1.55f, round_st);
        dc->DrawLine(D2D1::Point2F(sx + 8.6f, cy - 2.8f), D2D1::Point2F(sx + 3.8f, cy + 2.8f), br, 1.55f, round_st);
        return;
    }
    if (factory) {
        const int waves = (vol_pct > 66) ? 2 : 1;
        constexpr float kWR[2] = {3.4f, 6.2f};
        for (int i = 0; i < 2; ++i) {
            const float r = kWR[i];
            const float x0 = sx + 1.8f + r * 0.65f;
            const float y0 = cy - r * 0.72f;
            const float y1 = cy + r * 0.72f;
            ComPtr<ID2D1PathGeometry> geo;
            ComPtr<ID2D1GeometrySink> sk;
            if (SUCCEEDED(factory->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sk))) {
                sk->BeginFigure(D2D1::Point2F(x0, y0), D2D1_FIGURE_BEGIN_HOLLOW);
                sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(x0, y1), D2D1::SizeF(r, r), 0.0f,
                                            D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sk->EndFigure(D2D1_FIGURE_END_OPEN);
                sk->Close();
                br->SetColor(ArgbToColorF(i < waves ? pri_argb : dim_argb, alpha));
                dc->DrawGeometry(geo.Get(), br, 1.45f, round_st);
            }
        }
    }
}

void DrawSysBluetoothGlyph(ID2D1Factory* factory, ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
                           ID2D1StrokeStyle* round_st, float cx, float cy,
                           bool enabled, bool any_connected,
                           uint32_t pri_argb, uint32_t muted_argb, uint32_t dim_argb, float alpha) noexcept {
    if (!factory) return;
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sk;
    if (FAILED(factory->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sk))) return;
    sk->BeginFigure(D2D1::Point2F(cx - 3.6f, cy - 3.0f), D2D1_FIGURE_BEGIN_HOLLOW);
    sk->AddLine(D2D1::Point2F(cx + 3.6f, cy + 3.0f));
    sk->AddLine(D2D1::Point2F(cx, cy + 6.0f));
    sk->AddLine(D2D1::Point2F(cx, cy - 6.0f));
    sk->AddLine(D2D1::Point2F(cx + 3.6f, cy - 3.0f));
    sk->AddLine(D2D1::Point2F(cx - 3.6f, cy + 3.0f));
    sk->EndFigure(D2D1_FIGURE_END_OPEN);
    sk->Close();
    const uint32_t col = !enabled ? dim_argb : (any_connected ? pri_argb : muted_argb);
    br->SetColor(ArgbToColorF(col, alpha));
    dc->DrawGeometry(geo.Get(), br, 1.45f, round_st);
}

void DrawSysBatteryGlyph(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
                         float cx, float cy, int pct, bool charging, uint8_t low_pct,
                         uint32_t pri_argb, uint32_t dim_argb, float alpha) noexcept {
    const float bw = 19.0f;
    const float bh = 10.5f;
    const float x0 = cx - bw * 0.5f - 1.0f;
    const float y0 = cy - bh * 0.5f;
    const D2D1_RECT_F body{x0, y0, x0 + bw, y0 + bh};
    br->SetColor(ArgbToColorF(pri_argb, alpha * 0.85f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(body, 2.8f, 2.8f), br, 1.25f);
    // Positive battery cap nub
    br->SetColor(ArgbToColorF(dim_argb, alpha * 1.4f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.right + 1.0f, cy - 2.3f, body.right + 3.0f, cy + 2.3f), 1.0f, 1.0f), br);

    const float frac = std::clamp(static_cast<float>(pct) / 100.0f, 0.0f, 1.0f);
    const float inner_max = bw - 4.0f;
    const float fw = std::max(frac > 0.0f ? 2.0f : 0.0f, inner_max * frac);
    const uint32_t fill_col = charging ? 0xFF30D158u : (pct <= static_cast<int>(low_pct) ? 0xFFFF453Au : pri_argb);
    if (fw > 0.5f) {
        br->SetColor(ArgbToColorF(fill_col, alpha));
        dc->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(body.left + 2.0f, body.top + 2.0f, body.left + 2.0f + fw, body.bottom - 2.0f),
                              1.4f, 1.4f),
            br);
    }
}

void PaintDockSysTooltip(ID2D1Factory* factory, ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
                         const DockSysSlotRect& slot, float canvas_w, float canvas_h,
                         uint8_t dock_position,
                         const DockBarMeasureFn& measure_text, const DockBarTextFn& draw_text) noexcept {
    if (!dc || !br || slot.tooltip.empty()) return;
    DockBarItem fake{};
    fake.title = slot.tooltip;
    fake.draw_cx = (slot.rect.left + slot.rect.right) * 0.5f;
    fake.draw_cy = (slot.rect.top + slot.rect.bottom) * 0.5f;
    fake.draw_size = (dock_position == 0) ? (slot.rect.bottom - slot.rect.top) : (slot.rect.right - slot.rect.left);
    PaintDockTooltip(factory, dc, br, fake, 1.0f, canvas_w, canvas_h, dock_position, measure_text, draw_text);
}

void DrawDockSysArea(ID2D1Factory* factory,
                     ID2D1DeviceContext* dc,
                     ID2D1SolidColorBrush* br,
                     DockBarIconCache& icon_cache,
                     const LiquidDockBarState& dock,
                     bool light_text,
                     float dock_alpha,
                     DockSysSlotKind hovered_kind,
                     int hovered_tray_idx,
                     DockSysSlotKind open_kind,
                     int open_tray_idx,
                     uint8_t battery_low_pct,
                     const DockBarTextFn& draw_text) noexcept {
    if (!dock.sys_area_enabled || dock.sys_slots.empty() || dock_alpha <= 0.01f) return;
    const bool vertical = dock.IsVertical();
    ComPtr<ID2D1StrokeStyle> round_st;
    if (factory) {
        factory->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
            nullptr, 0, &round_st);
    }

    // 1. Hairline glass divider separating Trash from .dk-sys
    if (vertical) {
        br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, (light_text ? 0.22f : 0.14f) * dock_alpha));
        dc->DrawLine(D2D1::Point2F(dock.sys_divider_rect.left, dock.sys_divider_rect.top + 1.0f),
                     D2D1::Point2F(dock.sys_divider_rect.right, dock.sys_divider_rect.top + 1.0f), br, 1.0f);
        br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, (light_text ? 0.34f : 0.48f) * dock_alpha));
        dc->DrawLine(D2D1::Point2F(dock.sys_divider_rect.left, dock.sys_divider_rect.top),
                     D2D1::Point2F(dock.sys_divider_rect.right, dock.sys_divider_rect.top), br, 1.0f);
    } else {
        br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, (light_text ? 0.22f : 0.14f) * dock_alpha));
        dc->DrawLine(D2D1::Point2F(dock.sys_divider_rect.left + 1.0f, dock.sys_divider_rect.top),
                     D2D1::Point2F(dock.sys_divider_rect.left + 1.0f, dock.sys_divider_rect.bottom), br, 1.0f);
        br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, (light_text ? 0.34f : 0.48f) * dock_alpha));
        dc->DrawLine(D2D1::Point2F(dock.sys_divider_rect.left, dock.sys_divider_rect.top),
                     D2D1::Point2F(dock.sys_divider_rect.left, dock.sys_divider_rect.bottom), br, 1.0f);
    }

    // 2. Quieter unified recessed .dk-sys inner capsule
    const float cap_rad = vertical ? 11.0f : 13.0f;
    br->SetColor(light_text ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f * dock_alpha)
                            : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f * dock_alpha));
    dc->FillRoundedRectangle(D2D1::RoundedRect(dock.sys_capsule_rect, cap_rad, cap_rad), br);
    br->SetColor(light_text ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f * dock_alpha)
                            : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f * dock_alpha));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(dock.sys_capsule_rect, cap_rad, cap_rad), br, 0.85f);

    const uint32_t pri_argb = light_text ? 0xF2FFFFFFu : 0xEB1D1D1Fu;
    const uint32_t muted_argb = light_text ? 0x9EFFFFFFu : 0x961D1D1Fu;
    const uint32_t dim_argb = light_text ? 0x48FFFFFFu : 0x401D1D1Fu;

    // 3. Draw every .dk-sys button & live glyph
    for (const auto& slot : dock.sys_slots) {
        const bool is_open = (open_kind == slot.kind &&
                              (slot.kind != DockSysSlotKind::PinnedTrayApp || open_tray_idx == slot.tray_index));
        const bool is_hot = (hovered_kind == slot.kind &&
                             (slot.kind != DockSysSlotKind::PinnedTrayApp || hovered_tray_idx == slot.tray_index));
        if (is_open || is_hot) {
            const float op = is_open ? (light_text ? 0.24f : 0.16f) : (light_text ? 0.15f : 0.09f);
            br->SetColor(light_text ? D2D1::ColorF(1.0f, 1.0f, 1.0f, op * dock_alpha)
                                    : D2D1::ColorF(0.0f, 0.0f, 0.0f, op * dock_alpha));
            dc->FillRoundedRectangle(D2D1::RoundedRect(slot.rect, 9.0f, 9.0f), br);
        }

        const float cx = (slot.rect.left + slot.rect.right) * 0.5f;
        const float cy = (slot.rect.top + slot.rect.bottom) * 0.5f;

        switch (slot.kind) {
        case DockSysSlotKind::PinnedTrayApp: {
            if (slot.tray_index < 0 || static_cast<size_t>(slot.tray_index) >= dock.sys_snap.tray_apps.size()) break;
            const auto& app = dock.sys_snap.tray_apps[static_cast<size_t>(slot.tray_index)];
            const float isz = 21.0f;
            const D2D1_RECT_F ir{cx - isz * 0.5f, cy - isz * 0.5f, cx + isz * 0.5f, cy + isz * 0.5f};
            if (ID2D1Bitmap* bmp = icon_cache.GetOrLoadExeIcon(dc, app.exe_path, app.hwnd)) {
                dc->DrawBitmap(bmp, &ir, dock_alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
            } else {
                const uint32_t bg = app.accent_argb ? app.accent_argb : 0xFF0A84FFu;
                br->SetColor(ArgbToColorF(bg, dock_alpha * 0.94f));
                dc->FillRoundedRectangle(D2D1::RoundedRect(ir, 5.5f, 5.5f), br);
                br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.25f * dock_alpha));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(ir, 5.5f, 5.5f), br, 0.75f);
                if (draw_text) {
                    draw_text(FirstVisibleDockChar(app.title), ir, 11.0f, true, 0xFFFFFFFFu, dock_alpha);
                }
            }
            if (app.badge_count > 0) {
                const float bd = 11.0f;
                const D2D1_RECT_F br_rc{ir.right - bd * 0.62f, ir.top - bd * 0.25f,
                                        ir.right + bd * 0.38f, ir.top + bd * 0.75f};
                br->SetColor(D2D1::ColorF(1.0f, 0.231f, 0.188f, dock_alpha));
                dc->FillRoundedRectangle(D2D1::RoundedRect(br_rc, bd * 0.5f, bd * 0.5f), br);
                if (draw_text && app.badge_count < 10) {
                    draw_text(std::to_wstring(app.badge_count), br_rc, 8.2f, true, 0xFFFFFFFFu, dock_alpha);
                }
            }
            break;
        }
        case DockSysSlotKind::TrayOverflow: {
            br->SetColor(ArgbToColorF(muted_argb, dock_alpha));
            if (dock.dock_position == 1) {
                dc->DrawLine(D2D1::Point2F(cx - 2.0f, cy - 4.0f), D2D1::Point2F(cx + 2.5f, cy), br, 1.55f, round_st.Get());
                dc->DrawLine(D2D1::Point2F(cx + 2.5f, cy), D2D1::Point2F(cx - 2.0f, cy + 4.0f), br, 1.55f, round_st.Get());
            } else if (dock.dock_position == 2) {
                dc->DrawLine(D2D1::Point2F(cx + 2.0f, cy - 4.0f), D2D1::Point2F(cx - 2.5f, cy), br, 1.55f, round_st.Get());
                dc->DrawLine(D2D1::Point2F(cx - 2.5f, cy), D2D1::Point2F(cx + 2.0f, cy + 4.0f), br, 1.55f, round_st.Get());
            } else {
                dc->DrawLine(D2D1::Point2F(cx - 4.0f, cy + 1.8f), D2D1::Point2F(cx, cy - 2.4f), br, 1.55f, round_st.Get());
                dc->DrawLine(D2D1::Point2F(cx, cy - 2.4f), D2D1::Point2F(cx + 4.0f, cy + 1.8f), br, 1.55f, round_st.Get());
            }
            break;
        }
        case DockSysSlotKind::Ime: {
            if (draw_text) {
                const std::wstring badge = dock.sys_snap.ime_badge.empty() ? L"英" : dock.sys_snap.ime_badge;
                draw_text(badge, slot.rect, 12.2f, true, pri_argb, dock_alpha);
            }
            break;
        }
        case DockSysSlotKind::Wifi:
            if (dock.sys_snap.network_kind == live::DockNetworkKind::Ethernet ||
                (dock.sys_snap.ethernet_connected && !dock.sys_snap.wifi_connected)) {
                DrawSysEthernetGlyph(factory, dc, br, round_st.Get(), cx, cy, true, pri_argb, dim_argb, dock_alpha);
            } else if (dock.sys_snap.network_kind == live::DockNetworkKind::Wifi || dock.sys_snap.wifi_available) {
                DrawSysWifiGlyph(factory, dc, br, round_st.Get(), cx, cy,
                                 dock.sys_snap.wifi_enabled, dock.sys_snap.wifi_connected, dock.sys_snap.wifi_bars,
                                 pri_argb, dim_argb, dock_alpha);
            } else {
                DrawSysEthernetGlyph(factory, dc, br, round_st.Get(), cx, cy, false, pri_argb, dim_argb, dock_alpha);
            }
            break;
        case DockSysSlotKind::Volume:
            DrawSysVolumeGlyph(factory, dc, br, round_st.Get(), cx, cy,
                               dock.sys_snap.volume_pct, dock.sys_snap.muted,
                               pri_argb, dim_argb, dock_alpha);
            break;
        case DockSysSlotKind::Bluetooth: {
            bool any_conn = false;
            for (const auto& d : dock.sys_snap.bt_devices) {
                if (d.connected) { any_conn = true; break; }
            }
            DrawSysBluetoothGlyph(factory, dc, br, round_st.Get(), cx, cy,
                                  dock.sys_snap.bt_enabled, any_conn,
                                  pri_argb, muted_argb, dim_argb, dock_alpha);
            break;
        }
        case DockSysSlotKind::Battery:
            DrawSysBatteryGlyph(dc, br, cx, cy, dock.sys_snap.battery_pct, dock.sys_snap.battery_charging,
                                battery_low_pct, pri_argb, dim_argb, dock_alpha);
            break;
        case DockSysSlotKind::Clock: {
            std::wstring tm = dock.sys_snap.time_text;
            std::wstring dt = dock.sys_snap.date_text;
            if (tm.empty() || dt.empty()) {
                SYSTEMTIME st{};
                GetLocalTime(&st);
                wchar_t tb[12]{}, db[12]{};
                std::swprintf(tb, ARRAYSIZE(tb), L"%02u:%02u", st.wHour, st.wMinute);
                std::swprintf(db, ARRAYSIZE(db), L"%u/%u", st.wMonth, st.wDay);
                if (tm.empty()) tm = tb;
                if (dt.empty()) dt = db;
            }
            if (draw_text) {
                const float mid = (slot.rect.top + slot.rect.bottom) * 0.5f + 1.0f;
                draw_text(tm, D2D1::RectF(slot.rect.left, slot.rect.top + 1.0f, slot.rect.right, mid + 1.0f),
                          vertical ? 11.2f : 12.0f, true, pri_argb, dock_alpha);
                draw_text(dt, D2D1::RectF(slot.rect.left, mid - 1.0f, slot.rect.right, slot.rect.bottom - 1.0f),
                          9.5f, false, muted_argb, dock_alpha);
            }
            if (dock.sys_snap.notification_count > 0) {
                br->SetColor(D2D1::ColorF(0.29f, 0.72f, 1.0f, dock_alpha * 0.92f));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(slot.rect.right - 3.5f, slot.rect.top + 4.5f), 2.2f, 2.2f), br);
            }
            break;
        }
        default:
            break;
        }
    }
}

void DrawDockPoofEffect(ID2D1DeviceContext* dc,
                        ID2D1SolidColorBrush* br,
                        const DockBarPoof& poof,
                        float base_icon,
                        bool dark_ink,
                        double now_ms) noexcept {
    if (!dc || !br || poof.start_ms <= 0.0) return;
    const double elapsed = now_ms - poof.start_ms;
    if (elapsed < 0.0 || elapsed >= kPoofDurationMs) return;
    const float u = static_cast<float>(elapsed / kPoofDurationMs);
    const float e = 1.0f - (1.0f - u) * (1.0f - u);
    const float r_base = base_icon * (0.28f + 0.56f * e);
    const float alpha = (1.0f - u) * (1.0f - u);
    constexpr int kPuffs = 7;
    for (int k = 0; k < kPuffs; ++k) {
        const float ang = static_cast<float>(k) * (2.0f * kPi / static_cast<float>(kPuffs)) + 0.4f;
        const float dist = base_icon * (0.10f + 0.44f * e);
        const float px = poof.x + std::cos(ang) * dist;
        const float py = poof.y + std::sin(ang) * dist * 0.82f;
        const float pr = r_base * (0.40f + 0.12f * static_cast<float>((k * 37) % 5) / 4.0f);
        br->SetColor(dark_ink ? D2D1::ColorF(0.90f, 0.92f, 0.95f, 0.85f * alpha)
                              : D2D1::ColorF(0.98f, 0.98f, 1.00f, 0.94f * alpha));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), pr, pr), br);
        br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f * alpha));
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), pr, pr), br, 0.8f);
    }
    br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f * alpha));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(poof.x, poof.y), r_base * 0.54f, r_base * 0.48f), br);
}

} // namespace

// ============================================================================
// DrawLiquidDockBarContents (liquid_dock.cpp + renzyxx/WacOS dock.cpp + .dk-sys)
// ============================================================================

void DrawLiquidDockBarContents(ID2D1Factory* factory,
                               ID2D1DeviceContext* dc,
                               DockBarIconCache& icon_cache,
                               const LiquidDockBarState& dock,
                               int pressed_index,
                               int tooltip_index,
                               float tooltip_alpha,
                               int drop_target_mode,
                               bool dark_ink,
                               float canvas_w_dip,
                               const DockBarMeasureFn& measure_text,
                               const DockBarTextFn& draw_text,
                               const DockBarDragState* drag,
                               double now_ms,
                               float canvas_h_dip,
                               DockSysSlotKind hovered_sys_kind,
                               int hovered_sys_tray_idx,
                               DockSysSlotKind open_sys_kind,
                               int open_sys_tray_idx,
                               uint8_t battery_low_pct) noexcept {
    if (!dc || dock.items.empty() || dock.pill_rect.right <= dock.pill_rect.left) return;

    ComPtr<ID2D1SolidColorBrush> br;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br)) || !br) return;

    const D2D1_RECT_F pill = dock.pill_rect;
    const float rad = dock.pill_radius;
    const bool vertical = dock.IsVertical();
    const bool left_dock = (dock.dock_position == 1);
    const bool dragging = (drag != nullptr && drag->active &&
                           drag->source_index >= 0 &&
                           static_cast<size_t>(drag->source_index) < dock.items.size());

    // 1. Subtle inner specular reflection strip across capsule facing edge
    {
        const D2D1_GRADIENT_STOP stops[3] = {
            {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
            {0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, dark_ink ? 0.22f : 0.34f)},
            {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)},
        };
        ComPtr<ID2D1GradientStopCollection> sc;
        ComPtr<ID2D1LinearGradientBrush> lg;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 3, &sc)) && sc) {
            if (vertical) {
                const float ex = left_dock ? (pill.right - 1.0f) : (pill.left + 1.0f);
                if (SUCCEEDED(dc->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, pill.top + rad),
                                                            D2D1::Point2F(0.0f, pill.bottom - rad)),
                        sc.Get(), &lg)) && lg) {
                    dc->DrawLine(D2D1::Point2F(ex, pill.top + rad * 0.85f),
                                 D2D1::Point2F(ex, pill.bottom - rad * 0.85f), lg.Get(), 1.0f);
                }
            } else if (SUCCEEDED(dc->CreateLinearGradientBrush(
                           D2D1::LinearGradientBrushProperties(D2D1::Point2F(pill.left + rad, 0.0f),
                                                               D2D1::Point2F(pill.right - rad, 0.0f)),
                           sc.Get(), &lg)) && lg) {
                dc->DrawLine(D2D1::Point2F(pill.left + rad * 0.85f, pill.top + 1.0f),
                             D2D1::Point2F(pill.right - rad * 0.85f, pill.top + 1.0f), lg.Get(), 1.0f);
            }
        }
    }

    // Optional luminous drop-target outline when dragging files over the Dock ("固定到程序坞")
    if (drop_target_mode == 1) {
        br->SetColor(D2D1::ColorF(0.38f, 0.82f, 1.0f, 0.72f));
        dc->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(pill.left + 1.0f, pill.top + 1.0f, pill.right - 1.0f, pill.bottom - 1.0f),
                              std::max(1.0f, rad - 1.0f), std::max(1.0f, rad - 1.0f)),
            br.Get(), 2.0f);
    }

    // 2. Render dividers + icons + dynamic contact shadows + notification badges + running dots
    const float dot_bottom_y = pill.bottom - 5.2f;
    const float dot_side_x = left_dock ? (pill.left + 5.2f) : (pill.right - 5.2f);
    for (size_t i = 0; i < dock.items.size(); ++i) {
        const auto& it = dock.items[i];
        if (it.kind == DockBarItemKind::Divider) {
            if (vertical) {
                const float dx0 = pill.left + 13.0f;
                const float dx1 = pill.right - 13.0f;
                br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, dark_ink ? 0.22f : 0.14f));
                dc->DrawLine(D2D1::Point2F(dx0, it.draw_cy + 1.0f), D2D1::Point2F(dx1, it.draw_cy + 1.0f), br.Get(), 1.0f);
                br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, dark_ink ? 0.34f : 0.48f));
                dc->DrawLine(D2D1::Point2F(dx0, it.draw_cy), D2D1::Point2F(dx1, it.draw_cy), br.Get(), 1.0f);
            } else {
                const float dy0 = pill.top + 13.0f;
                const float dy1 = pill.bottom - 13.0f;
                br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, dark_ink ? 0.22f : 0.14f));
                dc->DrawLine(D2D1::Point2F(it.draw_cx + 1.0f, dy0), D2D1::Point2F(it.draw_cx + 1.0f, dy1), br.Get(), 1.0f);
                br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, dark_ink ? 0.34f : 0.48f));
                dc->DrawLine(D2D1::Point2F(it.draw_cx, dy0), D2D1::Point2F(it.draw_cx, dy1), br.Get(), 1.0f);
            }
            continue;
        }
        if (dragging && static_cast<int>(i) == drag->source_index) {
            continue; // Drawn floating under cursor below
        }
        if (it.slot_factor < 0.03f || it.draw_size < 2.0f) {
            continue;
        }

        const bool is_pressed = (!dragging && !it.is_ghost && pressed_index == static_cast<int>(i));
        const float press_factor = is_pressed ? 0.92f : 1.0f;
        const float sz = it.draw_size * press_factor;
        const float half = sz * 0.5f;
        const float item_alpha = std::clamp(it.slot_factor * 1.25f, 0.0f, 1.0f) *
                                 std::clamp(dock.reveal_progress * 2.2f, 0.0f, 1.0f);
        const D2D1_RECT_F icon_rect = D2D1::RectF(it.draw_cx - half, it.draw_cy - half,
                                                  it.draw_cx + half, it.draw_cy + half);

        // Warm red drop halo around Trash when dragging files onto Recycle Bin
        if (drop_target_mode == 2 && it.kind == DockBarItemKind::Trash) {
            br->SetColor(D2D1::ColorF(1.0f, 0.35f, 0.32f, 0.34f * item_alpha));
            dc->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(icon_rect.left - 5.0f, icon_rect.top - 5.0f,
                                              icon_rect.right + 5.0f, icon_rect.bottom + 5.0f),
                                  sz * 0.26f, sz * 0.26f),
                br.Get());
        }

        // Soft elevation contact shadow beneath/beside icon
        if (vertical) {
            const float elev = left_dock ? std::max(0.0f, (it.draw_cx - half) - (pill.left + 12.5f))
                                         : std::max(0.0f, (pill.right - 12.5f) - (it.draw_cx + half));
            const float sh_alpha = std::clamp(0.17f - elev * 0.004f, 0.04f, 0.17f) * item_alpha;
            br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, sh_alpha));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(it.draw_cx, it.draw_cy + half + 1.5f),
                                          half * 0.68f, 2.4f),
                            br.Get());
        } else {
            const float elev = std::max(0.0f, (pill.bottom - 12.5f) - (it.draw_cy + half));
            const float sh_alpha = std::clamp(0.19f - elev * 0.004f, 0.05f, 0.19f) * item_alpha;
            br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, sh_alpha));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(it.draw_cx, pill.bottom - 11.5f),
                                          half * (0.72f + elev * 0.007f), 2.8f + elev * 0.04f),
                            br.Get());
        }

        // Real high-res cropped icon bitmap or crisp Apple-style vector squircle
        DrawDockItemArtwork(dc, br.Get(), icon_cache, it, dock.trash_not_empty,
                            icon_rect, (is_pressed ? 0.88f : 1.0f) * item_alpha, draw_text);

        // WacOS / macOS press darkening mask for crisp tactile click feedback
        if (is_pressed) {
            br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.24f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(icon_rect, sz * 0.225f, sz * 0.225f), br.Get());
        }

        // Unread notification badge pill (WacOS dock.cpp)
        if (it.badge_count > 0) {
            DrawDockBadgePill(dc, br.Get(), icon_rect, it.badge_count, draw_text);
        }

        // Glowing running indicator dot on the outer edge (bottom / left / right)
        if (it.running) {
            const D2D1_POINT_2F dot_pt = vertical
                ? D2D1::Point2F(dot_side_x, it.draw_cy)
                : D2D1::Point2F(it.draw_cx, dot_bottom_y);
            if (it.active) {
                br->SetColor(D2D1::ColorF(0.42f, 0.82f, 1.0f, 0.48f));
                dc->FillEllipse(D2D1::Ellipse(dot_pt, vertical ? 3.6f : 5.2f, vertical ? 5.2f : 3.6f), br.Get());
                br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f));
                dc->FillEllipse(D2D1::Ellipse(dot_pt, 2.4f, 2.4f), br.Get());
            } else {
                const float halo_r = dark_ink ? 1.0f : 0.12f;
                const float halo_g = dark_ink ? 1.0f : 0.14f;
                const float halo_b = dark_ink ? 1.0f : 0.20f;
                br->SetColor(D2D1::ColorF(halo_r, halo_g, halo_b, 0.28f));
                dc->FillEllipse(D2D1::Ellipse(dot_pt, vertical ? 3.0f : 4.0f, vertical ? 4.0f : 3.0f), br.Get());
                br->SetColor(D2D1::ColorF(halo_r, halo_g, halo_b, 0.86f));
                dc->FillEllipse(D2D1::Ellipse(dot_pt, 2.1f, 2.1f), br.Get());
            }
        }
    }

    // 2.2. Unified .dk-sys system status & tray area at right/bottom end of Dock
    const float dock_alpha = std::clamp(dock.reveal_progress * 2.2f, 0.0f, 1.0f);
    DrawDockSysArea(factory, dc, br.Get(), icon_cache, dock, dark_ink, dock_alpha,
                    hovered_sys_kind, hovered_sys_tray_idx,
                    open_sys_kind, open_sys_tray_idx,
                    battery_low_pct, draw_text);

    // 2.5. macOS "Poof" smoke cloud animation when unpinning an icon by dragging out
    if (dock.HasActivePoof(now_ms)) {
        DrawDockPoofEffect(dc, br.Get(), dock.poof, dock.effective_base_icon, dark_ink, now_ms);
    }

    // 2.8. Floating icon being dragged (reorder or drag-out-to-unpin with "移除" pill)
    if (dragging) {
        const auto& src = dock.items[static_cast<size_t>(drag->source_index)];
        const float sz = dock.effective_base_icon * (drag->drag_out ? 0.96f : 1.08f);
        const float half = sz * 0.5f;
        const D2D1_RECT_F dr{drag->cursor_x - half, drag->cursor_y - half,
                             drag->cursor_x + half, drag->cursor_y + half};
        // Lifted drop shadow
        br->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, drag->drag_out ? 0.14f : 0.28f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(dr.left + 1.0f, dr.top + 6.0f, dr.right + 1.0f, dr.bottom + 7.0f),
                                                   sz * 0.225f, sz * 0.225f), br.Get());
        DrawDockItemArtwork(dc, br.Get(), icon_cache, src, dock.trash_not_empty,
                            dr, (drag->drag_out && src.pinned) ? 0.52f : 0.96f, draw_text);
        if (drag->drag_out && src.pinned) {
            const std::wstring rm_label = L"移除";
            const float tw = (measure_text ? std::max(30.0f, measure_text(rm_label, 12.0f)) : 34.0f) + 20.0f;
            const float th = 23.0f;
            const D2D1_RECT_F badge{drag->cursor_x - tw * 0.5f, dr.top - th - 10.0f,
                                    drag->cursor_x + tw * 0.5f, dr.top - 10.0f};
            br->SetColor(D2D1::ColorF(0.09f, 0.10f, 0.14f, 0.88f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(badge, th * 0.48f, th * 0.48f), br.Get());
            br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(badge, th * 0.48f, th * 0.48f), br.Get(), 0.85f);
            if (draw_text) {
                draw_text(rm_label, badge, 11.8f, true, 0xF8FFFFFFu, 1.0f);
            }
        }
    }

    // 3. Floating dark frosted tooltip pill facing hovered icon or .dk-sys slot (hidden while dragging or panel open)
    if (!dragging && open_sys_kind == DockSysSlotKind::None) {
        if (hovered_sys_kind != DockSysSlotKind::None) {
            if (const auto* s = dock.FindSysSlot(hovered_sys_kind, hovered_sys_tray_idx)) {
                PaintDockSysTooltip(factory, dc, br.Get(), *s, canvas_w_dip, canvas_h_dip,
                                    dock.dock_position, measure_text, draw_text);
            }
        } else if (tooltip_index >= 0 && tooltip_index < static_cast<int>(dock.items.size()) &&
                   dock.items[static_cast<size_t>(tooltip_index)].kind != DockBarItemKind::Divider) {
            PaintDockTooltip(factory, dc, br.Get(), dock.items[static_cast<size_t>(tooltip_index)],
                             tooltip_alpha, canvas_w_dip, canvas_h_dip, dock.dock_position,
                             measure_text, draw_text);
        }
    }
}

} // namespace pulse::desktop::render

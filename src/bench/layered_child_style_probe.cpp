#include <windows.h>
#include <cstdio>
#include <cstdint>

// Native API observations only. No ShowWindow, compatibility activation,
// manifest replacement, DPI mutation, registry writes or foreign windows.
// Primary contracts:
// https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features
// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowlongptrw
// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setlayeredwindowattributes
namespace {
constexpr wchar_t kClass[] = L"Pulse.Owned.LayeredStyleProbe";
unsigned steps = 0;
void Bool(const char* route, const char* api, BOOL result, DWORD error) {
    ++steps;
    std::printf("OBS route=%s api=%s result=%d error=%lu\n", route, api, result != FALSE, error);
}
void Styles(const char* route, HWND window) {
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const DWORD extended_error = GetLastError();
    SetLastError(ERROR_SUCCESS);
    const ULONG_PTR klass = GetClassLongPtrW(window, GCL_STYLE);
    const DWORD class_error = GetLastError();
    ++steps;
    std::printf("OBS route=%s api=GetStyles hwnd=%p exstyle=0x%llX exerror=%lu classstyle=0x%llX classerror=%lu child=%d visible=%d\n",
        route, static_cast<void*>(window), static_cast<unsigned long long>(extended), extended_error,
        static_cast<unsigned long long>(klass), class_error, (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) != 0,
        IsWindowVisible(window) != FALSE);
}
bool ChangeLayered(const char* route, HWND window, bool set) {
    SetLastError(ERROR_SUCCESS);
    const ULONG_PTR klass = GetClassLongPtrW(window, GCL_STYLE);
    const DWORD class_error = GetLastError();
    // Do not remove system class styles to evade documented restrictions.
    if ((!klass && class_error) || (klass & (CS_CLASSDC | CS_PARENTDC | CS_OWNDC))) {
        ++steps;
        std::printf("UNSUPPORTED route=%s operation=SetLayeredStyle classstyle=0x%llX error=%lu documented_dc_class_restriction=1\n",
            route, static_cast<unsigned long long>(klass), class_error);
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR before = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (!before && GetLastError()) return false;
    const LONG_PTR desired = set ? before | WS_EX_LAYERED : before & ~static_cast<LONG_PTR>(WS_EX_LAYERED);
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR prior = SetWindowLongPtrW(window, GWL_EXSTYLE, desired);
    const DWORD error = GetLastError();
    const LONG_PTR actual = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const bool changed = (prior != 0 || error == ERROR_SUCCESS) && actual == desired;
    ++steps;
    std::printf("OBS route=%s api=SetWindowLongPtrW set=%d prior=0x%llX error=%lu requested=0x%llX actual=0x%llX exact=%d\n",
        route, set, static_cast<unsigned long long>(prior), error, static_cast<unsigned long long>(desired),
        static_cast<unsigned long long>(actual), changed);
    SetLastError(ERROR_SUCCESS);
    const BOOL positioned = SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    const DWORD position_error = GetLastError();
    Bool(route, "SetWindowPos(FRAMECHANGED,no-show)", positioned, position_error);
    return changed && positioned;
}
bool Alpha(const char* route, HWND window) {
    SetLastError(ERROR_SUCCESS);
    const BOOL set = SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
    const DWORD set_error = GetLastError();
    Bool(route, "SetLayeredWindowAttributes(alpha=255)", set, set_error);
    COLORREF key = 0; BYTE alpha = 0; DWORD flags = 0;
    SetLastError(ERROR_SUCCESS);
    const BOOL get = GetLayeredWindowAttributes(window, &key, &alpha, &flags);
    const DWORD get_error = GetLastError();
    ++steps;
    std::printf("OBS route=%s api=GetLayeredWindowAttributes result=%d error=%lu alpha=%u flags=0x%lX\n",
        route, get != FALSE, get_error, static_cast<unsigned>(alpha), flags);
    return set && get && alpha == 255 && (flags & LWA_ALPHA);
}
struct Surface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    ~Surface() {
        if (dc && previous && previous != HGDI_ERROR) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    bool Create() {
        SetLastError(ERROR_SUCCESS);
        dc = CreateCompatibleDC(nullptr);
        const DWORD dc_error = GetLastError();
        Bool("surface", "CreateCompatibleDC", dc != nullptr, dc_error);
        if (!dc) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 64; info.bmiHeader.biHeight = -32;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        SetLastError(ERROR_SUCCESS);
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        const DWORD bitmap_error = GetLastError();
        Bool("surface", "CreateDIBSection(opaque-premultiplied-BGRA)", bitmap && pixels, bitmap_error);
        if (!bitmap || !pixels) return false;
        auto* values = static_cast<uint32_t*>(pixels);
        for (unsigned i = 0; i != 64 * 32; ++i) values[i] = 0xFF2070C0u;
        SetLastError(ERROR_SUCCESS);
        previous = SelectObject(dc, bitmap);
        const DWORD select_error = GetLastError();
        Bool("surface", "SelectObject", previous && previous != HGDI_ERROR, select_error);
        return previous && previous != HGDI_ERROR;
    }
};
bool Upload(const char* route, HWND window, Surface& surface, bool indirect) {
    POINT source{}; SIZE size{64, 32};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    SetLastError(ERROR_SUCCESS);
    BOOL result = FALSE;
    if (indirect) {
        UPDATELAYEREDWINDOWINFO info{}; info.cbSize = sizeof(info);
        info.psize = &size; info.hdcSrc = surface.dc; info.pptSrc = &source;
        info.pblend = &blend; info.dwFlags = ULW_ALPHA;
        result = UpdateLayeredWindowIndirect(window, &info);
    } else result = UpdateLayeredWindow(window, nullptr, nullptr, &size, surface.dc, &source, 0, &blend, ULW_ALPHA);
    const DWORD error = GetLastError();
    Bool(route, indirect ? "UpdateLayeredWindowIndirect" : "UpdateLayeredWindow", result, error);
    return result != FALSE;
}
HWND Create(const char* route, HWND parent, const wchar_t* klass, DWORD extended) {
    SetLastError(ERROR_SUCCESS);
    HWND window = CreateWindowExW(extended, klass, L"owned probe", WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 64, 32, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    const DWORD error = GetLastError();
    ++steps;
    std::printf("OBS route=%s api=CreateWindowExW exstyle=0x%lX hwnd=%p error=%lu\n",
        route, extended, static_cast<void*>(window), error);
    if (window) Styles(route, window);
    return window;
}
void Destroy(const char* route, HWND window) {
    if (!window) return;
    SetLastError(ERROR_SUCCESS);
    const BOOL destroyed = DestroyWindow(window);
    const DWORD error = GetLastError();
    Bool(route, "DestroyWindow", destroyed, error);
}
void Routes(HWND parent, const wchar_t* klass, const char* label, Surface& surface) {
    std::printf("CLASS_BEGIN %s\n", label);
    HWND direct = Create("direct-layered", parent, klass, WS_EX_LAYERED);
    if (direct) Alpha("direct-layered", direct);
    Destroy("direct-layered", direct);
    HWND redirected = Create("plain-then-style-alpha", parent, klass, 0);
    if (redirected && ChangeLayered("plain-then-style-alpha", redirected, true)) {
        const bool redirected_ready = Alpha("plain-then-style-alpha", redirected);
        // Documented mode transition: after SLA, ULW requires clearing and
        // resetting WS_EX_LAYERED. Record both outcomes; do not infer visuals.
        Upload("after-alpha-without-reset", redirected, surface, false);
        const bool reset = ChangeLayered("reset-clear", redirected, false) && ChangeLayered("reset-set", redirected, true);
        const bool uploaded = reset && Upload("after-style-reset", redirected, surface, false);
        std::printf("CAPABILITY class=%s redirected_api=%d upload_after_reset_api=%d\n", label, redirected_ready, uploaded);
    }
    Destroy("plain-then-style-alpha", redirected);
    HWND upload = Create("plain-then-style-indirect", parent, klass, 0);
    if (upload && ChangeLayered("plain-then-style-indirect", upload, true)) {
        const bool uploaded = Upload("plain-then-style-indirect", upload, surface, true);
        std::printf("CAPABILITY class=%s direct_upload_api=%d\n", label, uploaded);
    }
    Destroy("plain-then-style-indirect", upload);
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("OBSERVATION_ONLY pid=%lu hidden_owned_windows=1 manifest_not_modified=1 dpi_not_modified=1\n", GetCurrentProcessId());
    SetLastError(ERROR_SUCCESS);
    const HRSRC manifest = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24));
    const DWORD manifest_error = GetLastError();
    std::printf("OBS embedded_manifest_id1=%d lookup_error=%lu (no activation-context or manifest mutation)\n",
        manifest != nullptr, manifest_error);
    WNDCLASSEXW klass{}; klass.cbSize = sizeof(klass);
    klass.lpfnWndProc = DefWindowProcW; klass.hInstance = GetModuleHandleW(nullptr); klass.lpszClassName = kClass;
    SetLastError(ERROR_SUCCESS);
    const ATOM registered = RegisterClassExW(&klass);
    const DWORD register_error = GetLastError();
    Bool("setup", "RegisterClassExW(style=0)", registered != 0, register_error);
    if (!registered) return 2;
    SetLastError(ERROR_SUCCESS);
    HWND parent = CreateWindowExW(0, kClass, L"Hidden owned probe parent", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, nullptr, nullptr, klass.hInstance, nullptr);
    const DWORD parent_error = GetLastError();
    Bool("setup", "CreateHiddenParent", parent != nullptr, parent_error);
    int result = 0;
    {
        Surface surface;
        if (!parent || !surface.Create()) result = 2;
        else {
            Routes(parent, L"EDIT", "system-EDIT", surface);
            Routes(parent, kClass, "own-class-style-zero", surface);
        }
    }
    Destroy("parent", parent);
    SetLastError(ERROR_SUCCESS);
    const BOOL unregistered = UnregisterClassW(kClass, klass.hInstance);
    const DWORD unregister_error = GetLastError();
    Bool("cleanup", "UnregisterClassW", unregistered, unregister_error);
    if (!unregistered) result = 2;
    std::printf("SAMPLING_COMPLETE steps=%u setup_cleanup_status=%d visual_equivalence=NOT_TESTED luma_backend=NOT_TESTED\n", steps, result);
    std::fflush(stdout);
    return result;
}

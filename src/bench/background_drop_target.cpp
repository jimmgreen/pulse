// Standalone, isolated OLE receiver for the background file-drag probe.
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <cstdio>

namespace {
int drops = 0, files = 0, enters = 0, lost_foreground = 0, null_foreground = 0;
FILE* log_file = nullptr;
void Trace(HWND hwnd, const char* stage, UINT msg, WPARAM wp) {
    if (!log_file) return;
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    fprintf(log_file, "[RECEIVER] stage=%s msg=%u wp=%llu foreground=%p active=%p self=%p above=%p below=%p rect=(%ld,%ld,%ld,%ld) visible=%d\n",
        stage, msg, static_cast<unsigned long long>(wp), static_cast<void*>(GetForegroundWindow()),
        static_cast<void*>(GetActiveWindow()), static_cast<void*>(hwnd),
        static_cast<void*>(GetWindow(hwnd, GW_HWNDPREV)), static_cast<void*>(GetWindow(hwnd, GW_HWNDNEXT)),
        rect.left, rect.top, rect.right, rect.bottom, IsWindowVisible(hwnd));
    fflush(log_file);
}
class Target final : public IDropTarget {
    ULONG refs_ = 1;
    HWND hwnd_;
public:
    explicit Target(HWND hwnd) : hwnd_(hwnd) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDropTarget) return E_NOINTERFACE;
        *out = static_cast<IDropTarget*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_; if (!refs) delete this; return refs;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL, DWORD* effect) override {
        Trace(hwnd_, "DragEnter", 0, 0);
        ++enters; return DragOver(0, {}, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        HWND foreground = GetForegroundWindow();
        if (!foreground) ++null_foreground;
        else if (foreground != hwnd_) ++lost_foreground;
        *effect &= DROPEFFECT_COPY; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        Trace(hwnd_, "Drop", 0, 0);
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        if (SUCCEEDED(data->GetData(&format, &medium))) {
            ++drops;
            files = static_cast<int>(DragQueryFileW(static_cast<HDROP>(medium.hGlobal), 0xffffffff, nullptr, 0));
            ReleaseStgMedium(&medium);
        }
        return DragOver(0, {}, effect);
    }
};
LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP + 1) return drops;
    if (msg == WM_APP + 2) return files;
    if (msg == WM_APP + 3) return enters;
    if (msg == WM_APP + 4) return lost_foreground;
    if (msg == WM_APP + 5) return null_foreground;
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    const bool tracked = msg == WM_ACTIVATE || msg == WM_ACTIVATEAPP || msg == WM_WINDOWPOSCHANGED;
    if (tracked) Trace(hwnd, "before", msg, wp);
    const LRESULT result = DefWindowProcW(hwnd, msg, wp, lp);
    if (tracked) Trace(hwnd, "after", msg, wp);
    return result;
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (FAILED(OleInitialize(nullptr))) return 2;
    wchar_t log_path[32768]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_BACKGROUND_DROP_LOG", log_path, ARRAYSIZE(log_path)))
        _wfopen_s(&log_file, log_path, L"w");
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc; wc.hInstance = instance;
    wc.lpszClassName = L"PulseBackgroundDropProbe";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Pulse isolated drop receiver (no files written)",
        WS_OVERLAPPEDWINDOW, 600, 200, 400, 400, nullptr, nullptr, instance, nullptr);
    if (!hwnd) { OleUninitialize(); return 3; }
    auto* target = new Target(hwnd);
    const HRESULT registered = RegisterDragDrop(hwnd, target);
    target->Release();
    if (FAILED(registered)) { DestroyWindow(hwnd); OleUninitialize(); return 4; }
    ShowWindow(hwnd, SW_SHOW);
    // A hidden launcher may override the first ShowWindow via STARTUPINFO.
    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNORMAL);
    if (!IsWindowVisible(hwnd)) {
        RevokeDragDrop(hwnd);
        DestroyWindow(hwnd);
        OleUninitialize();
        return 5;
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    RevokeDragDrop(hwnd);
    OleUninitialize();
    if (log_file) fclose(log_file);
    return 0;
}

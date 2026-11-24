#include "host/overlays/ole_drag_drop.h"
#include <cstdio>
#include <shellapi.h>
#include <filesystem>
#include <fstream>

using namespace pulse::desktop::overlays;
namespace {
int failures = 0;
std::vector<std::wstring> expected;
bool cancelled = false;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}

// Independent receiver: deliberately does not use DesktopDropTarget or the
// production extraction helper, as third-party applications decode CF_HDROP.
class FixtureTarget final : public IDropTarget {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = (iid == IID_IUnknown || iid == IID_IDropTarget) ? static_cast<IDropTarget*>(this) : nullptr;
        if (*out) { AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        Check(data->QueryGetData(&format) == S_OK, "independent target accepts CF_HDROP");
        IEnumFORMATETC* formats = nullptr;
        Check(data->EnumFormatEtc(DATADIR_GET, &formats) == S_OK && formats, "target enumerates source formats");
        if (formats) formats->Release();
        *effect &= DROPEFFECT_COPY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        const HRESULT hr = data->GetData(&format, &medium);
        Check(hr == S_OK, "target reads drop payload");
        if (FAILED(hr)) return hr;
        HDROP drop = static_cast<HDROP>(medium.hGlobal);
        const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        std::vector<std::wstring> received;
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1); path.resize(length);
            received.push_back(std::move(path));
        }
        Check(received == expected, "independent target receives exact Unicode multi-file paths");
        for (const auto& path : received) Check(std::filesystem::exists(path), "fixture file remains available");
        ReleaseStgMedium(&medium);
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }
};
class CancelSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = (iid == IID_IUnknown || iid == IID_IDropSource) ? static_cast<IDropSource*>(this) : nullptr;
        if (*out) { AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL, DWORD) override { return DRAGDROP_S_CANCEL; }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};
HRESULT WINAPI CancelOsDragLoop(IDataObject* data, IDropSource*, DWORD allowed, DWORD* effect) {
    // Exercise the real OLE modal loop, cancelling before any target can receive
    // a drop. No synthesized input or user cursor movement is needed.
    CancelSource source;
    PostThreadMessageW(GetCurrentThreadId(), WM_KEYDOWN, VK_ESCAPE, 0);
    const HRESULT hr = DoDragDrop(data, &source, allowed, effect);
    Check(hr == DRAGDROP_S_CANCEL, "real OS DoDragDrop cancels before delivery");
    return hr;
}
HRESULT WINAPI FixtureDragLoop(IDataObject* data, IDropSource* source, DWORD allowed, DWORD* effect) {
    Check(source->QueryContinueDrag(FALSE, MK_LBUTTON) == S_OK, "left drag continues without Alt");
    Check(source->GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS, "OLE feedback uses system cursor");
    if (cancelled) {
        *effect = DROPEFFECT_NONE;
        return source->QueryContinueDrag(TRUE, MK_LBUTTON);
    }
    FixtureTarget target;
    *effect = allowed;
    target.DragEnter(data, MK_LBUTTON, {}, effect);
    Check(source->QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP, "release commits external drop");
    const HRESULT hr = target.Drop(data, 0, {}, effect);
    return SUCCEEDED(hr) ? DRAGDROP_S_DROP : hr;
}
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2 || argc > 3) return 2;
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return 3;
    const auto dir = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(dir);
    for (const auto* name : {L"拖出 样本.txt", L"second.txt"}) {
        const auto file = dir / name;
        std::ofstream stream(file); stream << "isolated drag fixture";
        expected.push_back(file.wstring());
    }
    HWND desktop = CreateWindowExW(0, L"STATIC", L"Fixture desktop", WS_POPUP, 0, 0, 40, 40,
                                    nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND child = CreateWindowExW(0, L"STATIC", L"Fixture child", WS_CHILD, 0, 0, 10, 10,
                                  desktop, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND external = CreateWindowExW(0, L"STATIC", L"Fixture receiver", WS_POPUP, 50, 0, 40, 40,
                                     nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(desktop && child && external, "create hidden isolated window hierarchy");
    Check(!DesktopDropManager::IsExternalDragWindow(desktop, desktop), "desktop hit stays internal");
    Check(!DesktopDropManager::IsExternalDragWindow(desktop, child), "desktop child hit stays internal");
    Check(!DesktopDropManager::IsExternalDragWindow(desktop, nullptr), "missing target does not start OLE");
    Check(DesktopDropManager::IsExternalDragWindow(desktop, external), "third-party window hands drag to OLE without Alt");
    DWORD effect = 0;
    Check(DesktopDropManager::StartFileDragOut(expected, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect, FixtureDragLoop)
          && effect == DROPEFFECT_COPY, "production source transfers to independent fixture target");
    cancelled = true;
    Check(!DesktopDropManager::StartFileDragOut(expected, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect, FixtureDragLoop)
          && effect == DROPEFFECT_NONE, "escape cancels without performing a move");
    if (argc == 3 && std::wstring_view(argv[2]) == L"--native-cancel") {
        Check(!DesktopDropManager::StartFileDragOut(expected, DROPEFFECT_COPY, &effect, CancelOsDragLoop)
              && effect == DROPEFFECT_NONE, "native OLE cancellation reports no performed effect");
    }
    Check(!DesktopDropManager::StartFileDragOut({}, DROPEFFECT_COPY, &effect, FixtureDragLoop), "empty drag rejected");
    DestroyWindow(external); DestroyWindow(child); DestroyWindow(desktop);
    OleUninitialize();
    std::printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}

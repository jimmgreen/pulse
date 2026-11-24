#include "host/overlays/ole_drag_drop.h"

#include <shellapi.h>
#include <cstring>
#include <utility>

namespace pulse::desktop::overlays {
namespace {

using Microsoft::WRL::ComPtr;

HGLOBAL BuildHDropGlobal(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return nullptr;
    size_t total_wchars = 1; // trailing double-NUL
    for (const auto& p : paths) {
        if (!p.empty()) {
            total_wchars += p.size() + 1;
        }
    }
    if (total_wchars <= 1) return nullptr;

    const size_t total_bytes = sizeof(DROPFILES) + total_wchars * sizeof(wchar_t);
    HGLOBAL hmem = GlobalAlloc(GHND | GMEM_SHARE, total_bytes);
    if (!hmem) return nullptr;

    auto* raw = static_cast<uint8_t*>(GlobalLock(hmem));
    if (!raw) {
        GlobalFree(hmem);
        return nullptr;
    }

    auto* df = reinterpret_cast<DROPFILES*>(raw);
    df->pFiles = sizeof(DROPFILES);
    df->pt = {0, 0};
    df->fNC = FALSE;
    df->fWide = TRUE;

    auto* dst = reinterpret_cast<wchar_t*>(raw + sizeof(DROPFILES));
    for (const auto& p : paths) {
        if (p.empty()) continue;
        std::memcpy(dst, p.data(), p.size() * sizeof(wchar_t));
        dst += p.size();
        *dst++ = L'\0';
    }
    *dst = L'\0';

    GlobalUnlock(hmem);
    return hmem;
}

class FileDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape_pressed, DWORD key_state) override {
        if (escape_pressed) return DRAGDROP_S_CANCEL;
        if ((key_state & (MK_LBUTTON | MK_RBUTTON)) == 0) return DRAGDROP_S_DROP;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD /*effect*/) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    std::atomic<ULONG> ref_count_{1};
};

class HDropDataObject final : public IDataObject {
public:
    explicit HDropDataObject(std::vector<std::wstring> paths) : paths_(std::move(paths)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fmt, STGMEDIUM* med) override {
        if (!fmt || !med) return E_POINTER;
        if (fmt->cfFormat != CF_HDROP || (fmt->tymed & TYMED_HGLOBAL) == 0 || fmt->dwAspect != DVASPECT_CONTENT) {
            return DV_E_FORMATETC;
        }
        HGLOBAL hmem = BuildHDropGlobal(paths_);
        if (!hmem) return E_OUTOFMEMORY;
        med->tymed = TYMED_HGLOBAL;
        med->hGlobal = hmem;
        med->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fmt) override {
        if (!fmt) return E_POINTER;
        if (fmt->cfFormat == CF_HDROP && (fmt->tymed & TYMED_HGLOBAL) != 0 && fmt->dwAspect == DVASPECT_CONTENT) {
            return S_OK;
        }
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (!out) return E_POINTER;
        out->ptd = nullptr;
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** ppenum) override {
        if (!ppenum) return E_POINTER;
        if (direction != DATADIR_GET) {
            *ppenum = nullptr;
            return E_NOTIMPL;
        }
        FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return SHCreateStdEnumFmtEtc(1, &fmt, ppenum);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    std::atomic<ULONG> ref_count_{1};
    std::vector<std::wstring> paths_;
};

} // namespace

DesktopDropTarget::DesktopDropTarget(HWND hwnd, DropTargetCallbacks callbacks)
    : hwnd_(hwnd), callbacks_(std::move(callbacks)) {}

HRESULT STDMETHODCALLTYPE DesktopDropTarget::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *ppv = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DesktopDropTarget::AddRef() {
    return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE DesktopDropTarget::Release() {
    const ULONG n = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (n == 0) delete this;
    return n;
}

model::PointI DesktopDropTarget::ToClientPoint(POINTL screen_pt) const noexcept {
    if (callbacks_.to_client) return callbacks_.to_client(screen_pt);
    POINT pt{screen_pt.x, screen_pt.y};
    if (hwnd_ && IsWindow(hwnd_)) {
        ScreenToClient(hwnd_, &pt);
    }
    return model::PointI{pt.x, pt.y};
}

DWORD DesktopDropTarget::EvaluateHover(model::PointI client_pt, DWORD key_state,
                                       DropZoneQueryResult& out_zone,
                                       std::wstring& out_hint) const {
    out_zone = {};
    if (callbacks_.hit_test_zone) {
        out_zone = callbacks_.hit_test_zone(client_pt);
    }
    const bool force_copy = (key_state & MK_CONTROL) != 0;

    switch (out_zone.kind) {
    case DropZoneKind::StagingTrayTile:
        out_hint = L"加入 Pulse 暂存区";
        return DROPEFFECT_COPY;
    case DropZoneKind::RecycleBinTile:
        out_hint = L"移入回收站";
        return DROPEFFECT_MOVE;
    case DropZoneKind::PinnedFolderTile:
        out_hint = (force_copy ? L"复制到文件夹 · " : L"移动到文件夹 · ") + out_zone.target_title;
        return force_copy ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
    case DropZoneKind::GroupCard:
        out_hint = L"移动到分组 · " + out_zone.target_title;
        return force_copy ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
    case DropZoneKind::BareDesktop:
        out_hint = force_copy ? L"复制到桌面" : L"放到桌面";
        return force_copy ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
    case DropZoneKind::None:
    default:
        out_hint.clear();
        return DROPEFFECT_NONE;
    }
}

HRESULT STDMETHODCALLTYPE DesktopDropTarget::DragEnter(IDataObject* data_obj, DWORD key_state,
                                                       POINTL pt, DWORD* effect) {
    if (!effect) return E_POINTER;
    cached_paths_ = DesktopDropManager::ExtractHDropPaths(data_obj);
    has_hdrop_ = !cached_paths_.empty();
    if (!has_hdrop_) {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }

    const model::PointI cpt = ToClientPoint(pt);
    DropZoneQueryResult zone{};
    std::wstring hint;
    const DWORD desired = EvaluateHover(cpt, key_state, zone, hint);
    *effect = (*effect & desired) != 0 ? desired : ((*effect & DROPEFFECT_COPY) ? DROPEFFECT_COPY : DROPEFFECT_NONE);
    if (callbacks_.on_drag_hint) {
        callbacks_.on_drag_hint(*effect != DROPEFFECT_NONE, cpt, hint);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DesktopDropTarget::DragOver(DWORD key_state, POINTL pt, DWORD* effect) {
    if (!effect) return E_POINTER;
    if (!has_hdrop_) {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }

    const model::PointI cpt = ToClientPoint(pt);
    DropZoneQueryResult zone{};
    std::wstring hint;
    const DWORD desired = EvaluateHover(cpt, key_state, zone, hint);
    *effect = (*effect & desired) != 0 ? desired : ((*effect & DROPEFFECT_COPY) ? DROPEFFECT_COPY : DROPEFFECT_NONE);
    if (callbacks_.on_drag_hint) {
        callbacks_.on_drag_hint(*effect != DROPEFFECT_NONE, cpt, hint);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DesktopDropTarget::DragLeave() {
    has_hdrop_ = false;
    cached_paths_.clear();
    if (callbacks_.on_drag_hint) {
        callbacks_.on_drag_hint(false, {0, 0}, {});
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DesktopDropTarget::Drop(IDataObject* data_obj, DWORD key_state,
                                                  POINTL pt, DWORD* effect) {
    if (!effect) return E_POINTER;
    if (callbacks_.on_drag_hint) {
        callbacks_.on_drag_hint(false, {0, 0}, {});
    }

    std::vector<std::wstring> paths = DesktopDropManager::ExtractHDropPaths(data_obj);
    if (paths.empty()) paths = std::move(cached_paths_);
    has_hdrop_ = false;
    cached_paths_.clear();

    if (paths.empty()) {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }

    const model::PointI cpt = ToClientPoint(pt);
    DropZoneQueryResult zone{};
    std::wstring hint;
    DWORD resolved_effect = EvaluateHover(cpt, key_state, zone, hint);
    if ((*effect & resolved_effect) == 0) {
        resolved_effect = (*effect & DROPEFFECT_COPY) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    }
    if (resolved_effect != DROPEFFECT_NONE && callbacks_.on_drop_files) {
        callbacks_.on_drop_files(zone, cpt, paths, key_state, resolved_effect);
    }
    *effect = resolved_effect;
    return S_OK;
}

DesktopDropManager::~DesktopDropManager() {
    Detach();
}

bool DesktopDropManager::Attach(HWND hwnd, DropTargetCallbacks callbacks) {
    Detach();
    if (!hwnd || !IsWindow(hwnd)) return false;

    const HRESULT ole_hr = OleInitialize(nullptr);
    if (SUCCEEDED(ole_hr)) {
        ole_initialized_ = true;
    } else if (ole_hr != RPC_E_CHANGED_MODE) {
        return false;
    }

    target_.Attach(new DesktopDropTarget(hwnd, std::move(callbacks)));
    if (FAILED(RegisterDragDrop(hwnd, target_.Get()))) {
        target_.Reset();
        if (ole_initialized_) {
            OleUninitialize();
            ole_initialized_ = false;
        }
        return false;
    }
    hwnd_ = hwnd;
    return true;
}

void DesktopDropManager::Detach() noexcept {
    if (hwnd_ && IsWindow(hwnd_)) {
        RevokeDragDrop(hwnd_);
    }
    hwnd_ = nullptr;
    target_.Reset();
    if (ole_initialized_) {
        OleUninitialize();
        ole_initialized_ = false;
    }
}

bool DesktopDropManager::IsExternalDragWindow(HWND desktop, HWND under_pointer) noexcept {
    return desktop && under_pointer && desktop != under_pointer && !IsChild(desktop, under_pointer);
}

bool DesktopDropManager::StartFileDragOut(const std::vector<std::wstring>& file_paths,
                                          DWORD allowed_effects,
                                          DWORD* out_performed_effect, DragDropRunner run_drag_drop) {
    if (out_performed_effect) *out_performed_effect = DROPEFFECT_NONE;
    if (file_paths.empty() || !run_drag_drop) return false;

    ComPtr<IDataObject> data_obj;
    data_obj.Attach(new HDropDataObject(file_paths));
    ComPtr<IDropSource> drop_src;
    drop_src.Attach(new FileDropSource());

    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = run_drag_drop(data_obj.Get(), drop_src.Get(), allowed_effects, &effect);
    if (out_performed_effect) *out_performed_effect = effect;
    return hr == DRAGDROP_S_DROP;
}

std::vector<std::wstring> DesktopDropManager::ExtractHDropPaths(IDataObject* data_obj) {
    std::vector<std::wstring> out;
    if (!data_obj) return out;

    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM med{};
    if (FAILED(data_obj->GetData(&fmt, &med))) return out;

    HDROP hdrop = static_cast<HDROP>(med.hGlobal);
    if (hdrop) {
        const UINT count = DragQueryFileW(hdrop, 0xFFFFFFFFu, nullptr, 0);
        out.reserve(count);
        for (UINT i = 0; i < count; ++i) {
            const UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
            if (len == 0) continue;
            std::wstring path(len + 1, L'\0');
            const UINT copied = DragQueryFileW(hdrop, i, path.data(), len + 1);
            path.resize(copied);
            if (!path.empty()) {
                out.push_back(std::move(path));
            }
        }
    }
    ReleaseStgMedium(&med);
    return out;
}

} // namespace pulse::desktop::overlays

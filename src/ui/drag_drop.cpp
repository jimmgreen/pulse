// drag_drop.cpp — See drag_drop.h for the contract.
#include "drag_drop.h"
#include <shellapi.h>
#include <shlobj.h>
#include <cwctype>
#include <cstring>

namespace pulse::ui {

namespace {

UINT PreferredEffectFormat() {
    static UINT fmt = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    return fmt;
}

UINT PerformedEffectFormat() {
    static UINT fmt = RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT);
    return fmt;
}

} // namespace

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------
std::wstring VolumeRoot(const std::wstring& path) {
    std::wstring p = path;
    if (p.starts_with(L"\\\\?\\UNC\\")) p = L"\\\\" + p.substr(8);
    else if (p.starts_with(L"\\\\?\\")) p = p.substr(4);
    if (p.size() >= 3 && std::iswalpha(p[0]) && p[1] == L':') return p.substr(0, 3);
    if (p.size() >= 5 && p[0] == L'\\' && p[1] == L'\\') {
        auto s3 = p.find(L'\\', 2);
        if (s3 == std::wstring::npos || s3 + 1 >= p.size()) return L"";
        auto s4 = p.find(L'\\', s3 + 1);
        return p.substr(0, s4); // \\server\share
    }
    return L"";
}

DWORD ComputeDropEffect(DWORD key_state, const std::wstring& source_sample,
                        const std::wstring& dest_dir, DWORD allowed) {
    const bool ctrl = (key_state & MK_CONTROL) != 0;
    const bool shift = (key_state & MK_SHIFT) != 0;
    DWORD want;
    if (ctrl && !shift) {
        want = DROPEFFECT_COPY;
    } else if (shift && !ctrl) {
        want = DROPEFFECT_MOVE;
    } else if (ctrl && shift) {
        want = DROPEFFECT_LINK;
    } else {
        std::wstring sv = VolumeRoot(source_sample);
        std::wstring dv = VolumeRoot(dest_dir);
        want = (!sv.empty() && sv == dv) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    }
    if (want & allowed) return want;
    // Fall back to whatever the source allows.
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return DROPEFFECT_NONE;
}

// ---------------------------------------------------------------------------
// IDataObject inspection
// ---------------------------------------------------------------------------
bool ExtractHDropPaths(IDataObject* obj, std::vector<std::wstring>& out) {
    out.clear();
    if (!obj) return false;
    FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    if (FAILED(obj->GetData(&fmt, &medium)) || !medium.hGlobal) return false;
    HDROP hdrop = static_cast<HDROP>(medium.hGlobal);
    UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
        std::wstring path(len, L'\0');
        DragQueryFileW(hdrop, i, path.data(), len + 1);
        out.push_back(std::move(path));
    }
    ReleaseStgMedium(&medium);
    return !out.empty();
}

DWORD PreferredDropEffect(IDataObject* obj) {
    if (!obj) return 0;
    FORMATETC fmt{ (CLIPFORMAT)PreferredEffectFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    if (FAILED(obj->GetData(&fmt, &medium)) || !medium.hGlobal) return 0;
    DWORD effect = 0;
    if (DWORD* p = static_cast<DWORD*>(GlobalLock(medium.hGlobal))) {
        effect = *p;
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return effect;
}

// ---------------------------------------------------------------------------
// Format enumerator for FileDataObject.
// ---------------------------------------------------------------------------
namespace {

class FormatEnumerator final : public IEnumFORMATETC {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
            *out = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    IFACEMETHODIMP Next(ULONG count, FORMATETC* out, ULONG* fetched) override {
        ULONG n = 0;
        while (n < count && pos_ < 3) {
            out[n] = Format(pos_);
            ++pos_;
            ++n;
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG count) override {
        ULONG skipped = (std::min)(count, 3u - pos_);
        pos_ += skipped;
        return skipped == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Reset() override { pos_ = 0; return S_OK; }
    IFACEMETHODIMP Clone(IEnumFORMATETC** out) override {
        if (!out) return E_POINTER;
        auto* clone = new FormatEnumerator();
        clone->pos_ = pos_;
        *out = clone;
        return S_OK;
    }

private:
    static FORMATETC Format(ULONG i) {
        FORMATETC f{};
        f.dwAspect = DVASPECT_CONTENT;
        f.lindex = -1;
        f.tymed = TYMED_HGLOBAL;
        f.cfFormat = i == 0 ? CF_HDROP
                   : i == 1 ? (CLIPFORMAT)PreferredEffectFormat()
                            : (CLIPFORMAT)PerformedEffectFormat();
        return f;
    }
    LONG ref_ = 1;
    ULONG pos_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// FileDataObject
// ---------------------------------------------------------------------------
FileDataObject::FileDataObject(const std::vector<std::wstring>& paths) : paths_(paths) {}

FileDataObject* FileDataObject::Create(const std::vector<std::wstring>& paths) {
    return new FileDataObject(paths);
}

HRESULT FileDataObject::QueryInterface(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
        *out = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG FileDataObject::AddRef() {
    return ++ref_;
}

ULONG FileDataObject::Release() {
    LONG r = --ref_;
    if (r == 0) delete this;
    return r;
}

HGLOBAL FileDataObject::RenderHDrop() const {
    size_t chars = 1;
    for (const auto& p : paths_) chars += p.size() + 1;
    size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!h) return nullptr;
    auto* df = static_cast<DROPFILES*>(GlobalLock(h));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<uint8_t*>(df) + sizeof(DROPFILES));
    for (const auto& p : paths_) {
        std::memcpy(dst, p.c_str(), p.size() * sizeof(wchar_t));
        dst += p.size() + 1;
    }
    GlobalUnlock(h);
    return h;
}

HGLOBAL FileDataObject::RenderDword(DWORD v) const {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (!h) return nullptr;
    *static_cast<DWORD*>(GlobalLock(h)) = v;
    GlobalUnlock(h);
    return h;
}

bool FileDataObject::IsSupportedFormat(const FORMATETC* fmt) const {
    if (!fmt || fmt->tymed != TYMED_HGLOBAL || fmt->dwAspect != DVASPECT_CONTENT) return false;
    return fmt->cfFormat == CF_HDROP ||
           fmt->cfFormat == (CLIPFORMAT)PreferredEffectFormat() ||
           fmt->cfFormat == (CLIPFORMAT)PerformedEffectFormat();
}

HRESULT FileDataObject::QueryGetData(FORMATETC* fmt) {
    return IsSupportedFormat(fmt) ? S_OK : DV_E_FORMATETC;
}

HRESULT FileDataObject::GetData(FORMATETC* fmt, STGMEDIUM* out) {
    if (!out) return E_POINTER;
    std::memset(out, 0, sizeof(*out));
    if (!IsSupportedFormat(fmt)) return DV_E_FORMATETC;
    HGLOBAL h = nullptr;
    if (fmt->cfFormat == CF_HDROP) h = RenderHDrop();
    else if (fmt->cfFormat == (CLIPFORMAT)PreferredEffectFormat())
        h = RenderDword(DROPEFFECT_COPY | DROPEFFECT_MOVE);
    else h = RenderDword(performed_effect_);
    if (!h) return E_OUTOFMEMORY;
    out->tymed = TYMED_HGLOBAL;
    out->hGlobal = h;
    return S_OK;
}

HRESULT FileDataObject::SetData(FORMATETC* fmt, STGMEDIUM* medium, BOOL release) {
    if (!fmt || !medium) return E_INVALIDARG;
    if (fmt->cfFormat != (CLIPFORMAT)PerformedEffectFormat() &&
        fmt->cfFormat != (CLIPFORMAT)PreferredEffectFormat())
        return DV_E_FORMATETC;
    if (medium->tymed != TYMED_HGLOBAL || !medium->hGlobal) return DV_E_TYMED;
    if (DWORD* p = static_cast<DWORD*>(GlobalLock(medium->hGlobal))) {
        performed_effect_ = *p;
        GlobalUnlock(medium->hGlobal);
    }
    if (release) ReleaseStgMedium(medium);
    return S_OK;
}

HRESULT FileDataObject::EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) {
    if (!out) return E_POINTER;
    if (dir != DATADIR_GET) return E_NOTIMPL;
    *out = new FormatEnumerator();
    return S_OK;
}

// ---------------------------------------------------------------------------
// ListDropSource
// ---------------------------------------------------------------------------
HRESULT ListDropSource::QueryInterface(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropSource) {
        *out = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG ListDropSource::Release() {
    LONG r = --ref_;
    if (r == 0) delete this;
    return r;
}

HRESULT ListDropSource::QueryContinueDrag(BOOL escape_pressed, DWORD key_state) {
    if (escape_pressed) {
        if (esc_hook_ && esc_hook_()) return S_OK; // target consumed Esc (spring-back)
        return DRAGDROP_S_CANCEL;
    }
    if (!(key_state & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
    return S_OK;
}

DWORD DoFileDragDrop(const std::vector<std::wstring>& paths, DWORD allowed_effects,
                     std::function<bool()> esc_consumed) {
    if (paths.empty()) return DROPEFFECT_NONE;
    FileDataObject* data = FileDataObject::Create(paths);
    auto* source = new ListDropSource();
    source->SetEscHook(std::move(esc_consumed));
    DWORD effect = DROPEFFECT_NONE;
    ::DoDragDrop(data, source, allowed_effects, &effect);
    if (effect == DROPEFFECT_NONE && data->PerformedEffect() != 0)
        effect = data->PerformedEffect();
    source->Release();
    data->Release();
    return effect;
}

// ---------------------------------------------------------------------------
// WindowDropTarget
// ---------------------------------------------------------------------------
WindowDropTarget::WindowDropTarget(HWND hwnd, DropTargetCallbacks cb)
    : hwnd_(hwnd), cb_(std::move(cb)) {}

HRESULT WindowDropTarget::QueryInterface(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *out = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG WindowDropTarget::Release() {
    LONG r = --ref_;
    if (r == 0) delete this;
    return r;
}

POINT WindowDropTarget::ClientPoint(POINTL pt) const {
    POINT p{ pt.x, pt.y };
    ScreenToClient(hwnd_, &p);
    return p;
}

HRESULT WindowDropTarget::DragEnter(IDataObject* obj, DWORD key_state, POINTL pt,
                                    DWORD* effect) {
    sources_.clear();
    preferred_ = 0;
    if (!ExtractHDropPaths(obj, sources_)) {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    preferred_ = PreferredDropEffect(obj);
    DWORD allowed = *effect;
    *effect = cb_.drag_over ? cb_.drag_over(sources_, ClientPoint(pt), key_state, allowed)
                            : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT WindowDropTarget::DragOver(DWORD key_state, POINTL pt, DWORD* effect) {
    if (sources_.empty()) {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    DWORD allowed = *effect;
    *effect = cb_.drag_over ? cb_.drag_over(sources_, ClientPoint(pt), key_state, allowed)
                            : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT WindowDropTarget::DragLeave() {
    sources_.clear();
    preferred_ = 0;
    if (cb_.drag_leave) cb_.drag_leave();
    return S_OK;
}

HRESULT WindowDropTarget::Drop(IDataObject* obj, DWORD key_state, POINTL pt, DWORD* effect) {
    std::vector<std::wstring> sources;
    if (!ExtractHDropPaths(obj, sources)) {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    DWORD performed = cb_.drop ? cb_.drop(sources, ClientPoint(pt), key_state, preferred_)
                               : DROPEFFECT_NONE;
    *effect = performed;
    // Tell the source what we did (Explorer uses this for move semantics).
    FORMATETC fmt{ (CLIPFORMAT)PerformedEffectFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (medium.hGlobal) {
        *static_cast<DWORD*>(GlobalLock(medium.hGlobal)) = performed;
        GlobalUnlock(medium.hGlobal);
        if (FAILED(obj->SetData(&fmt, &medium, TRUE))) ReleaseStgMedium(&medium);
    }
    sources_.clear();
    preferred_ = 0;
    return S_OK;
}

} // namespace pulse::ui

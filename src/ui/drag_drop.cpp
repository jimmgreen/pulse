// drag_drop.cpp — See drag_drop.h for the contract.
#include "drag_drop.h"
#include "../fs/fs_recycle.h"
#include "../common/shell_id.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <vector>

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

UINT LogicalPerformedEffectFormat() {
    static UINT fmt = RegisterClipboardFormatW(CFSTR_LOGICALPERFORMEDDROPEFFECT);
    return fmt;
}

UINT FileNameWFormat() {
    static UINT fmt = RegisterClipboardFormatW(CFSTR_FILENAMEW);
    return fmt;
}

UINT FileNameFormat() {
    static UINT fmt = RegisterClipboardFormatW(CFSTR_FILENAME);
    return fmt;
}

UINT ShellIdListFormat() {
    static UINT fmt = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
    return fmt;
}

DWORD ReadDwordFormat(IDataObject* obj, CLIPFORMAT format) {
    if (!obj) return 0;
    FORMATETC fmt{ format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    if (FAILED(obj->GetData(&fmt, &medium)) || !medium.hGlobal) return 0;
    DWORD value = 0;
    if (DWORD* p = static_cast<DWORD*>(GlobalLock(medium.hGlobal))) {
        value = *p;
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return value;
}

void WriteDwordFormat(IDataObject* obj, CLIPFORMAT format, DWORD value) {
    if (!obj) return;
    FORMATETC fmt{ format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (!medium.hGlobal) return;
    *static_cast<DWORD*>(GlobalLock(medium.hGlobal)) = value;
    GlobalUnlock(medium.hGlobal);
    if (FAILED(obj->SetData(&fmt, &medium, TRUE))) ReleaseStgMedium(&medium);
}

HRESULT CreateShellFilesDataObject(const std::vector<std::wstring>& paths,
                                   IDataObject** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (paths.empty()) return E_INVALIDARG;
    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(paths.size());
    for (const auto& path : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(shell_id::ParseId(path, &pidl)) || !pidl) {
            for (auto* p : pidls) CoTaskMemFree(p);
            return E_FAIL;
        }
        pidls.push_back(pidl);
    }
    IShellItemArray* array = nullptr;
    HRESULT hr = SHCreateShellItemArrayFromIDLists(
        static_cast<UINT>(pidls.size()),
        const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()),
        &array);
    for (auto* p : pidls) CoTaskMemFree(p);
    if (FAILED(hr) || !array) return FAILED(hr) ? hr : E_FAIL;
    hr = array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(out));
    array->Release();
    return hr;
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
                        const std::wstring& dest_dir, DWORD allowed,
                        DWORD preferred_effect) {
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
        // No modifier: a source that names exactly one of copy/move gets it.
        // Browser download lists, Electron-style staged drags and archive
        // tools offer COPY|MOVE but ask for COPY; moving those deletes the
        // user's original. COPY|MOVE itself means "either is fine" (Pulse's
        // own drags publish it), so it keeps Explorer's same-volume default.
        const DWORD preferred = preferred_effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE);
        if (preferred == DROPEFFECT_COPY || preferred == DROPEFFECT_MOVE) {
            want = preferred;
        } else {
            std::wstring sv = VolumeRoot(source_sample);
            std::wstring dv = VolumeRoot(dest_dir);
            want = (!sv.empty() && sv == dv) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        }
    }
    if (want & allowed) return want;
    // Fall back to whatever the source allows.
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return DROPEFFECT_NONE;
}

std::wstring FirstDroppableFolder(const std::vector<std::wstring>& sources) {
    for (const auto& path : sources) {
        if (path.empty()) continue;
        const DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
            return path;
    }
    return {};
}

bool LooksLikeFolderShortcut(const std::wstring& path) {
    if (path.size() < 4) return false;
    return _wcsicmp(path.c_str() + (path.size() - 4), L".lnk") == 0;
}

bool DropIntoOwnFolder(const std::vector<std::wstring>& sources, const std::wstring& dest_dir) {
    const auto comparable = [](std::wstring path) {
        if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) path = L"\\\\" + path.substr(8);
        else if (path.rfind(L"\\\\?\\", 0) == 0) path = path.substr(4);
        for (auto& c : path) if (c == L'/') c = L'\\';
        while (path.size() > 3 && path.back() == L'\\') path.pop_back();
        return path;
    };
    const std::wstring dest = comparable(dest_dir);
    if (dest.empty()) return false;
    for (const auto& item : sources) {
        const std::wstring source = comparable(item);
        if (source.empty() || source.size() > dest.size() ||
            _wcsnicmp(dest.c_str(), source.c_str(), source.size()) != 0) continue;
        if (dest.size() == source.size() || dest[source.size()] == L'\\' || source.back() == L'\\')
            return true;
    }
    return false;
}

bool IsDropLaunchProgram(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    const size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return false;
    for (const wchar_t* program : {L".exe", L".com", L".bat", L".cmd"})
        if (_wcsicmp(path.c_str() + dot, program) == 0) return true;
    return false;
}

std::wstring DropLaunchArguments(const std::vector<std::wstring>& sources) {
    std::wstring arguments;
    for (std::wstring path : sources) {
        if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) path = L"\\\\" + path.substr(8);
        else if (path.rfind(L"\\\\?\\", 0) == 0) path = path.substr(4);
        if (path.empty()) continue;
        if (!arguments.empty()) arguments += L' ';
        // File names cannot contain '"'; only a trailing backslash needs care.
        if (path.find_first_of(L" \t") == std::wstring::npos) {
            arguments += path;
            continue;
        }
        arguments += L'"';
        arguments += path;
        if (path.back() == L'\\') arguments += L'\\';
        arguments += L'"';
    }
    return arguments;
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
        std::wstring path(static_cast<size_t>(len) + 1, L'\0');
        const UINT copied = DragQueryFileW(hdrop, i, path.data(), len + 1);
        if (copied != len) continue;
        path.resize(len);
        out.push_back(std::move(path));
    }
    ReleaseStgMedium(&medium);
    return !out.empty();
}

bool IsShellItemId(const std::wstring& path) {
    return path.size() > 2 && path[0] == L':' && path[1] == L':';
}

bool ExtractShellItemIds(IDataObject* obj, std::vector<std::wstring>& out) {
    out.clear();
    if (!obj) return false;
    IShellItemArray* array = nullptr;
    if (FAILED(SHCreateShellItemArrayFromDataObject(obj, IID_PPV_ARGS(&array))) || !array) return false;
    DWORD count = 0;
    if (FAILED(array->GetCount(&count))) count = 0;
    for (DWORD i = 0; i < count; ++i) {
        IShellItem* item = nullptr;
        LPWSTR name = nullptr;
        if (SUCCEEDED(array->GetItemAt(i, &item)) && item &&
            SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name) {
            // A phone item travels as its ID list: its parsing name does not parse back.
            out.emplace_back(IsShellItemId(name) ? shell_id::FromItem(item) : std::wstring(name));
        }
        if (name) CoTaskMemFree(name);
        if (item) item->Release();
        if (out.size() != i + 1 || !IsShellItemId(out.back())) {
            out.clear();
            break;
        }
    }
    array->Release();
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

constexpr ULONG kFormatCount = 6;

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
        while (n < count && pos_ < kFormatCount) {
            out[n] = Format(pos_);
            ++pos_;
            ++n;
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG count) override {
        const ULONG remaining = pos_ < kFormatCount ? kFormatCount - pos_ : 0;
        ULONG skipped = (std::min)(count, remaining);
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
        switch (i) {
        case 0: f.cfFormat = CF_HDROP; break;
        case 1: f.cfFormat = (CLIPFORMAT)PreferredEffectFormat(); break;
        case 2: f.cfFormat = (CLIPFORMAT)PerformedEffectFormat(); break;
        case 3: f.cfFormat = (CLIPFORMAT)FileNameWFormat(); break;
        case 4: f.cfFormat = (CLIPFORMAT)FileNameFormat(); break;
        default: f.cfFormat = (CLIPFORMAT)ShellIdListFormat(); break;
        }
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

HGLOBAL FileDataObject::RenderFileNameW() const {
    if (paths_.empty()) return nullptr;
    const std::wstring& w = paths_[0];
    const SIZE_T bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!h) return nullptr;
    std::memcpy(GlobalLock(h), w.c_str(), bytes);
    GlobalUnlock(h);
    return h;
}

HGLOBAL FileDataObject::RenderFileNameA() const {
    if (paths_.empty()) return nullptr;
    const int n = WideCharToMultiByte(CP_ACP, 0, paths_[0].c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return nullptr;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, static_cast<SIZE_T>(n));
    if (!h) return nullptr;
    WideCharToMultiByte(CP_ACP, 0, paths_[0].c_str(), -1,
                        static_cast<char*>(GlobalLock(h)), n, nullptr, nullptr);
    GlobalUnlock(h);
    return h;
}

HGLOBAL FileDataObject::RenderShellIdList() const {
    if (paths_.empty()) return nullptr;
    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(paths_.size());
    for (const auto& path : paths_) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(shell_id::ParseId(path, &pidl)) || !pidl) {
            for (auto* p : pidls) CoTaskMemFree(p);
            return nullptr;
        }
        pidls.push_back(pidl);
    }

    PIDLIST_ABSOLUTE parent = ILClone(pidls[0]);
    if (parent) ILRemoveLastID(parent);
    bool same_parent = parent != nullptr;
    for (size_t i = 1; same_parent && i < pidls.size(); ++i) {
        PIDLIST_ABSOLUTE other = ILClone(pidls[i]);
        if (!other) {
            same_parent = false;
            break;
        }
        ILRemoveLastID(other);
        same_parent = ILIsEqual(parent, other) != FALSE;
        CoTaskMemFree(other);
    }

    const UINT cidl = static_cast<UINT>(pidls.size());
    const UINT header = static_cast<UINT>(sizeof(UINT) * (cidl + 2));
    auto pidl_size = [](PCUIDLIST_RELATIVE p) -> UINT {
        return p ? ILGetSize(p) : 2;
    };

    std::vector<PCUIDLIST_RELATIVE> children;
    children.reserve(pidls.size());
    UINT pidl_bytes = same_parent ? pidl_size(parent) : 2;
    if (same_parent) {
        for (auto* p : pidls) children.push_back(ILFindLastID(p));
    } else {
        for (auto* p : pidls) children.push_back(p);
    }
    for (auto* c : children) pidl_bytes += pidl_size(c);

    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, header + pidl_bytes);
    if (!h) {
        if (parent) CoTaskMemFree(parent);
        for (auto* p : pidls) CoTaskMemFree(p);
        return nullptr;
    }
    auto* base = static_cast<BYTE*>(GlobalLock(h));
    auto* ida = reinterpret_cast<CIDA*>(base);
    ida->cidl = cidl;
    UINT offset = header;
    ida->aoffset[0] = offset;
    if (same_parent && parent) {
        const UINT sz = ILGetSize(parent);
        std::memcpy(base + offset, parent, sz);
        offset += sz;
    } else {
        offset += 2;
    }
    for (UINT i = 0; i < cidl; ++i) {
        ida->aoffset[i + 1] = offset;
        const UINT sz = pidl_size(children[i]);
        std::memcpy(base + offset, children[i], sz);
        offset += sz;
    }
    GlobalUnlock(h);
    if (parent) CoTaskMemFree(parent);
    for (auto* p : pidls) CoTaskMemFree(p);
    return h;
}

bool FileDataObject::IsSupportedFormat(const FORMATETC* fmt) const {
    if (!fmt || fmt->dwAspect != DVASPECT_CONTENT) return false;
    if ((fmt->tymed & TYMED_HGLOBAL) == 0) return false;
    return fmt->cfFormat == CF_HDROP ||
           fmt->cfFormat == (CLIPFORMAT)PreferredEffectFormat() ||
           fmt->cfFormat == (CLIPFORMAT)PerformedEffectFormat() ||
           fmt->cfFormat == (CLIPFORMAT)LogicalPerformedEffectFormat() ||
           fmt->cfFormat == (CLIPFORMAT)FileNameWFormat() ||
           fmt->cfFormat == (CLIPFORMAT)FileNameFormat() ||
           fmt->cfFormat == (CLIPFORMAT)ShellIdListFormat();
}

HRESULT FileDataObject::QueryGetData(FORMATETC* fmt) {
    return IsSupportedFormat(fmt) ? S_OK : DV_E_FORMATETC;
}

HRESULT FileDataObject::GetData(FORMATETC* fmt, STGMEDIUM* out) {
    if (!out) return E_POINTER;
    std::memset(out, 0, sizeof(*out));
    if (!IsSupportedFormat(fmt)) return DV_E_FORMATETC;
    HGLOBAL h = nullptr;
    if (fmt->cfFormat == CF_HDROP) {
        h = RenderHDrop();
    } else if (fmt->cfFormat == (CLIPFORMAT)PreferredEffectFormat()) {
        h = RenderDword(DROPEFFECT_COPY | DROPEFFECT_MOVE);
    } else if (fmt->cfFormat == (CLIPFORMAT)PerformedEffectFormat()) {
        h = RenderDword(performed_effect_);
    } else if (fmt->cfFormat == (CLIPFORMAT)LogicalPerformedEffectFormat()) {
        h = RenderDword(logical_effect_);
    } else if (fmt->cfFormat == (CLIPFORMAT)FileNameWFormat()) {
        h = RenderFileNameW();
    } else if (fmt->cfFormat == (CLIPFORMAT)FileNameFormat()) {
        h = RenderFileNameA();
    } else {
        h = RenderShellIdList();
        if (!h) return E_FAIL;
    }
    if (!h) return E_OUTOFMEMORY;
    out->tymed = TYMED_HGLOBAL;
    out->hGlobal = h;
    return S_OK;
}

HRESULT FileDataObject::SetData(FORMATETC* fmt, STGMEDIUM* medium, BOOL release) {
    if (!fmt || !medium) return E_INVALIDARG;
    const bool logical = fmt->cfFormat == (CLIPFORMAT)LogicalPerformedEffectFormat();
    if (fmt->cfFormat != (CLIPFORMAT)PerformedEffectFormat() &&
        fmt->cfFormat != (CLIPFORMAT)PreferredEffectFormat() && !logical)
        return DV_E_FORMATETC;
    if (medium->tymed != TYMED_HGLOBAL || !medium->hGlobal) return DV_E_TYMED;
    if (DWORD* p = static_cast<DWORD*>(GlobalLock(medium->hGlobal))) {
        (logical ? logical_effect_ : performed_effect_) = *p;
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

namespace {
DragVisualFactory g_drag_visual_factory = nullptr;
thread_local DragVisual* g_active_drag_visual = nullptr;
} // namespace

void SetDragVisualFactory(DragVisualFactory factory) { g_drag_visual_factory = factory; }

bool SetDragVisualFeedback(const std::wstring& text) {
    if (!g_active_drag_visual) return false;
    g_active_drag_visual->Feedback(text);
    return true;
}

bool DragVisualActive() { return g_active_drag_visual != nullptr; }


namespace {
// UI threads only, but windows may run on their own: keep it locked.
std::mutex g_land_mu;
bool g_land_expect = false;
ULONGLONG g_land_expect_at = 0;
HWND g_land_hwnd = nullptr;
std::vector<RECT> g_land_rects;
} // namespace

void ExpectDragLanding() {
    std::lock_guard<std::mutex> lock(g_land_mu);
    g_land_expect = true;
    g_land_expect_at = GetTickCount64();
}

bool TakeDragLandingExpectation() {
    std::lock_guard<std::mutex> lock(g_land_mu);
    const bool fresh = g_land_expect && GetTickCount64() - g_land_expect_at < 2000;
    g_land_expect = false;
    return fresh;
}

void SetDragLandingWindow(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_land_mu);
    g_land_hwnd = hwnd;
    if (!hwnd) g_land_rects.clear();
}

bool DragLandingAwaiting() {
    std::lock_guard<std::mutex> lock(g_land_mu);
    return g_land_hwnd != nullptr;
}

bool LandDragStack(const std::vector<RECT>& screen_rects) {
    std::lock_guard<std::mutex> lock(g_land_mu);
    if (!g_land_hwnd || screen_rects.empty()) return false;
    g_land_rects = screen_rects;
    const HWND hwnd = g_land_hwnd;
    g_land_hwnd = nullptr;  // one answer per drop
    return PostMessageW(hwnd, kDragLandMessage, 0, 0) != FALSE;
}

std::vector<RECT> TakeDragLandingRects() {
    std::lock_guard<std::mutex> lock(g_land_mu);
    return std::move(g_land_rects);
}

HRESULT ListDropSource::QueryContinueDrag(BOOL escape_pressed, DWORD key_state) {
    if (visual_) visual_->Track();
    if (escape_pressed) {
        if (esc_hook_ && esc_hook_()) return S_OK; // target consumed Esc (spring-back)
        cancelled_ = true;
        return DRAGDROP_S_CANCEL;
    }
    if (!(key_state & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
    return S_OK;
}

DWORD DoFileDragDrop(const std::vector<std::wstring>& paths, DWORD allowed_effects,
                     std::function<bool()> esc_consumed, const std::wstring& source_view, bool dark) {
    // Recycle rows name the original path, which may now belong to a new file.
    if (paths.empty() || fs::IsRecycleViewPath(source_view)) return DROPEFFECT_NONE;

    IDataObject* data = nullptr;
    FileDataObject* fallback = nullptr;
    if (FAILED(CreateShellFilesDataObject(paths, &data)) || !data) {
        // Shell item ids have no CF_HDROP form: without the shell's object
        // there is nothing a target could read.
        if (IsShellItemId(paths.front())) return DROPEFFECT_NONE;
        fallback = FileDataObject::Create(paths);
        data = fallback;
    } else {
        FORMATETC fmt{ (CLIPFORMAT)PreferredEffectFormat(), nullptr, DVASPECT_CONTENT, -1,
                       TYMED_HGLOBAL };
        STGMEDIUM medium{};
        medium.tymed = TYMED_HGLOBAL;
        medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
        if (medium.hGlobal) {
            *static_cast<DWORD*>(GlobalLock(medium.hGlobal)) = allowed_effects;
            GlobalUnlock(medium.hGlobal);
            if (FAILED(data->SetData(&fmt, &medium, TRUE))) ReleaseStgMedium(&medium);
        }
    }

    auto* source = new ListDropSource();
    source->SetEscHook(std::move(esc_consumed));
    DragVisual* visual = g_drag_visual_factory ? g_drag_visual_factory(paths, dark) : nullptr;
    source->SetVisual(visual);
    g_active_drag_visual = visual;
    DWORD effect = DROPEFFECT_NONE;
    // Explorer's file drags also allow LINK. Apps that open a dropped file
    // often answer LINK, and without it they show the no-drop cursor (#235).
    // Preferred DropEffect above keeps only the caller's copy/move, so drop
    // targets that pick a default (Explorer, Pulse) still copy or move.
    const HRESULT drag_hr = ::DoDragDrop(data, source, allowed_effects | DROPEFFECT_LINK, &effect);
    g_active_drag_visual = nullptr;
    if (effect == DROPEFFECT_NONE && drag_hr == DRAGDROP_S_DROP) {
        // Optimized move (Pulse, Explorer): the target moved the items itself
        // and reports NONE so that we do not delete them; it still moved.
        const DWORD logical = fallback ? fallback->LogicalEffect()
            : ReadDwordFormat(data, (CLIPFORMAT)LogicalPerformedEffectFormat());
        if (logical == DROPEFFECT_MOVE) effect = DROPEFFECT_MOVE;
    }
    if (visual) {
        // Hand off before the effect fix-up below: a drop that Explorer
        // performed asynchronously still landed, so only a refused or
        // cancelled drag flies the stack back.
        source->SetVisual(nullptr);
        const bool dropped = drag_hr == DRAGDROP_S_DROP && !source->Cancelled();
        visual->Finish(effect, !dropped);
        visual = nullptr;
    }
    if (effect == DROPEFFECT_NONE) {
        if (fallback && fallback->PerformedEffect() != 0) {
            effect = fallback->PerformedEffect();
        } else if (!fallback) {
            const DWORD performed =
                ReadDwordFormat(data, (CLIPFORMAT)PerformedEffectFormat());
            if (performed != 0) effect = performed;
        }
    }
    source->Release();
    data->Release();
    return effect;
}

// ---------------------------------------------------------------------------
// WindowDropTarget
// ---------------------------------------------------------------------------
WindowDropTarget::WindowDropTarget(HWND hwnd, DropTargetCallbacks cb)
    : hwnd_(hwnd), cb_(std::move(cb)) {}

WindowDropTarget::~WindowDropTarget() { StopScroll(); }

void WindowDropTarget::StopScroll() {
    if (scrolling_) KillTimer(hwnd_, kScrollTimer);
    scrolling_ = false;
}

void WindowDropTarget::TickScroll() {
    if (!scrolling_ || sources_.empty() || !cb_.drag_scroll) return;
    POINT point{};
    if (!GetCursorPos(&point) || WindowFromPoint(point) != hwnd_) return;
    ScreenToClient(hwnd_, &point);
    if (cb_.drag_scroll(point) && cb_.drag_over)
        cb_.drag_over(sources_, point, keys_, allowed_, preferred_);
}

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
    if (!effect) return E_POINTER;
    StopScroll();
    sources_.clear();
    preferred_ = 0;
    if (!ExtractHDropPaths(obj, sources_) && !ExtractShellItemIds(obj, sources_)) {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    preferred_ = PreferredDropEffect(obj);
    const DWORD allowed = *effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    allowed_ = allowed;
    keys_ = key_state;
    if (hwnd_ && allowed && cb_.drag_scroll && !scrolling_)
        scrolling_ = SetTimer(hwnd_, kScrollTimer, 50, nullptr) != 0;
    *effect = allowed && cb_.drag_over ? cb_.drag_over(sources_, ClientPoint(pt), key_state, allowed, preferred_) & allowed
                            : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT WindowDropTarget::DragOver(DWORD key_state, POINTL pt, DWORD* effect) {
    if (!effect) return E_POINTER;
    if (sources_.empty()) {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    const DWORD allowed = *effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    allowed_ = allowed;
    keys_ = key_state;
    if (!allowed) StopScroll();
    else if (hwnd_ && cb_.drag_scroll && !scrolling_)
        scrolling_ = SetTimer(hwnd_, kScrollTimer, 50, nullptr) != 0;
    *effect = allowed && cb_.drag_over ? cb_.drag_over(sources_, ClientPoint(pt), key_state, allowed, preferred_) & allowed
                            : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT WindowDropTarget::DragLeave() {
    StopScroll();
    sources_.clear();
    preferred_ = 0;
    if (cb_.drag_leave) cb_.drag_leave();
    return S_OK;
}

HRESULT WindowDropTarget::Drop(IDataObject* obj, DWORD key_state, POINTL pt, DWORD* effect) {
    StopScroll();
    if (!effect) return E_POINTER;
    const DWORD allowed = *effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    std::vector<std::wstring> sources;
    if (!allowed || (!ExtractHDropPaths(obj, sources) && !ExtractShellItemIds(obj, sources))) {
        *effect = DROPEFFECT_NONE;
        DragLeave();
        return S_OK;
    }
    // Read the preference from the dropped object itself rather than the
    // DragEnter cache: Drop is the call that decides whether originals move.
    const DWORD preferred = PreferredDropEffect(obj);
    DWORD performed = cb_.drop ? cb_.drop(sources, ClientPoint(pt), key_state, allowed, preferred) & allowed
                               : DROPEFFECT_NONE;
    // Pulse never leaves deleting to the source: a move is queued as an ops
    // Move that removes the originals only after their copy landed. So a move
    // is reported as an optimized move (pdwEffect and Performed DropEffect
    // NONE, Logical Performed DropEffect MOVE). Reporting MOVE would ask the
    // source for an unoptimized move, and Explorer then deletes the originals
    // while the queued move has not read them yet, e.g. Shift+drag to a share.
    const DWORD reported = performed == DROPEFFECT_MOVE ? DROPEFFECT_NONE : performed;
    *effect = reported;
    WriteDwordFormat(obj, (CLIPFORMAT)PerformedEffectFormat(), reported);
    WriteDwordFormat(obj, (CLIPFORMAT)LogicalPerformedEffectFormat(), performed);
    sources_.clear();
    return S_OK;
}

} // namespace pulse::ui

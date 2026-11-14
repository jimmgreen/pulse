// clipboard.cpp — CF_HDROP + CFSTR_PREFERREDDROPEFFECT interop.
#include "clipboard.h"
#include "../common/path_utils.h"
#include "../common/shell_id.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <cstring>

namespace pulse::ops {

namespace {

HWND ClipboardOwner() {
    if (HWND owner = GetActiveWindow()) return owner;
    return GetConsoleWindow();
}

bool OpenClipboardWithRetry(HWND owner) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (OpenClipboard(owner)) return true;
        Sleep(5);
    }
    return false;
}

} // namespace

bool PreferredEffectIsCut(uint32_t preferred_effect) {
    return preferred_effect == DROPEFFECT_MOVE;
}

bool WriteClipboard(const std::vector<std::wstring>& paths, bool cut) {
    if (paths.empty()) return false;

    // DROPFILES + double-null-terminated wide path list.
    size_t chars = 1; // final terminator
    for (const auto& p : paths) chars += p.size() + 1;
    size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);

    HGLOBAL hdrop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!hdrop) return false;
    auto* df = static_cast<DROPFILES*>(GlobalLock(hdrop));
    if (!df) {
        GlobalFree(hdrop);
        return false;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<uint8_t*>(df) + sizeof(DROPFILES));
    for (const auto& p : paths) {
        std::memcpy(dst, p.c_str(), p.size() * sizeof(wchar_t));
        dst += p.size() + 1;
    }
    GlobalUnlock(hdrop);

    // EmptyClipboard assigns ownership to the window passed to OpenClipboard;
    // using nullptr can leave no owner and make SetClipboardData fail.
    if (!OpenClipboardWithRetry(ClipboardOwner())) {
        GlobalFree(hdrop);
        return false;
    }
    if (!EmptyClipboard() || !SetClipboardData(CF_HDROP, hdrop)) {
        CloseClipboard();
        GlobalFree(hdrop);
        return false;
    }
    // Ownership of hdrop transferred to the clipboard.

    UINT fmtEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    HGLOBAL heffect = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    bool effect_set = false;
    if (fmtEffect != 0 && heffect) {
        DWORD* effect = static_cast<DWORD*>(GlobalLock(heffect));
        if (effect) {
            *effect = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            GlobalUnlock(heffect);
            if (SetClipboardData(fmtEffect, heffect)) effect_set = true;
            else GlobalFree(heffect);
        } else {
            GlobalFree(heffect);
        }
    }
    if (!effect_set) {
        // CF_HDROP ownership already transferred; clearing the clipboard lets
        // the system release it without a double-free.
        EmptyClipboard();
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

bool WriteClipboardText(const std::wstring& text) {
    if (text.empty()) return false;
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return false;
    void* memory = GlobalLock(h);
    if (!memory) {
        GlobalFree(h);
        return false;
    }
    std::memcpy(memory, text.c_str(), bytes);
    GlobalUnlock(h);
    if (!OpenClipboardWithRetry(ClipboardOwner())) {
        GlobalFree(h);
        return false;
    }
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, h)) {
        CloseClipboard();
        GlobalFree(h);
        return false;
    }
    // Ownership of h transferred to the clipboard.
    CloseClipboard();
    return true;
}

bool ReadClipboard(ClipboardData& out) {
    out = {};
    if (!IsClipboardFormatAvailable(CF_HDROP)) return false;
    if (!OpenClipboardWithRetry(nullptr)) return false;

    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        if (HDROP hdrop = static_cast<HDROP>(GlobalLock(h))) {
            UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
                std::wstring path(static_cast<size_t>(len) + 1, L'\0');
                const UINT copied = DragQueryFileW(hdrop, i, path.data(), len + 1);
                if (copied == 0 && len != 0) continue;
                path.resize(copied);
                out.paths.push_back(std::move(path));
            }
            GlobalUnlock(h);
        }
        ok = !out.paths.empty();
    }
    UINT fmtEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (HANDLE h = GetClipboardData(fmtEffect)) {
        if (DWORD* effect = static_cast<DWORD*>(GlobalLock(h))) {
            out.cut = PreferredEffectIsCut(*effect);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (ok) out.sequence = GetClipboardSequenceNumber();
    return ok;
}

bool DropShellDataObject(const std::wstring& dir, IDataObject* data, DWORD wanted) {
    if (!data || (wanted != DROPEFFECT_COPY && wanted != DROPEFFECT_MOVE)) return false;
    IDropTarget* drop = nullptr;
    IShellItem* folder = nullptr;
    // The shell does not parse Pulse's \\?\ form.
    if (SUCCEEDED(SHCreateItemFromParsingName(path::StripExtendedPathPrefix(dir).c_str(), nullptr,
                                              IID_PPV_ARGS(&folder)))) {
        folder->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&drop));
        folder->Release();
    }
    bool ok = false;
    if (drop) {
        DWORD effect = wanted;
        POINTL pt{ 0, 0 };
        if (SUCCEEDED(drop->DragEnter(data, MK_LBUTTON, pt, &effect)) && (effect & wanted)) {
            effect = wanted;
            ok = SUCCEEDED(drop->Drop(data, MK_LBUTTON, pt, &effect));
        } else {
            drop->DragLeave();
        }
        drop->Release();
    }
    return ok;
}

bool PasteShellClipboard(const std::wstring& dir) {
    IDataObject* data = nullptr;
    if (FAILED(OleGetClipboard(&data)) || !data) return false;
    // A cut moves, as in Explorer; anything else copies.
    DWORD effect = DROPEFFECT_COPY;
    FORMATETC fmt{ static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)),
                   nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    if (SUCCEEDED(data->GetData(&fmt, &medium))) {
        if (const DWORD* preferred = static_cast<const DWORD*>(GlobalLock(medium.hGlobal))) {
            if (PreferredEffectIsCut(*preferred)) effect = DROPEFFECT_MOVE;
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    const bool ok = DropShellDataObject(dir, data, effect);
    data->Release();
    return ok;
}

namespace {

bool ReadPreferredCut(IDataObject* data) {
    FORMATETC fmt{ static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)),
                   nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    bool cut = false;
    if (SUCCEEDED(data->GetData(&fmt, &medium))) {
        if (const DWORD* preferred = static_cast<const DWORD*>(GlobalLock(medium.hGlobal))) {
            cut = PreferredEffectIsCut(*preferred);
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }
    return cut;
}

} // namespace

bool WriteShellItemsClipboard(const std::vector<std::wstring>& shell_ids, bool cut) {
    if (shell_ids.empty()) return false;
    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(shell_ids.size());
    bool parsed = true;
    for (const auto& id : shell_ids) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(shell_id::ParseId(id, &pidl)) || !pidl) {
            parsed = false;
            break;
        }
        pidls.push_back(pidl);
    }
    IShellItemArray* items = nullptr;
    if (parsed)
        SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
                                          const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &items);
    for (auto pidl : pidls) CoTaskMemFree(pidl);
    if (!items) return false;
    IDataObject* data = nullptr;
    items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
    items->Release();
    if (!data) return false;
    // Explorer's own copy says COPY | LINK; a cut says MOVE (PreferredEffectIsCut).
    FORMATETC fmt{ static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)),
                   nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (medium.hGlobal) {
        if (auto* effect = static_cast<DWORD*>(GlobalLock(medium.hGlobal))) {
            *effect = cut ? DROPEFFECT_MOVE : (DROPEFFECT_COPY | DROPEFFECT_LINK);
            GlobalUnlock(medium.hGlobal);
        }
        if (FAILED(data->SetData(&fmt, &medium, TRUE))) ReleaseStgMedium(&medium);
    }
    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 20; ++attempt) {  // another app may hold the clipboard briefly
        hr = OleSetClipboard(data);
        if (hr != CLIPBRD_E_CANT_OPEN) break;
        Sleep(5);
    }
    data->Release();
    return SUCCEEDED(hr);
}

bool ReadClipboardShellItems(std::vector<std::wstring>& shell_ids, bool& cut) {
    shell_ids.clear();
    cut = false;
    if (IsClipboardFormatAvailable(CF_HDROP) ||
        !IsClipboardFormatAvailable(RegisterClipboardFormatW(CFSTR_SHELLIDLIST)))
        return false;
    IDataObject* data = nullptr;
    if (FAILED(OleGetClipboard(&data)) || !data) return false;
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items))) && items) {
        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            IShellItem* item = nullptr;
            if (FAILED(items->GetItemAt(i, &item)) || !item) continue;
            PWSTR raw = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &raw)) && raw) {
                const bool namespace_item = raw[0] == L':' && raw[1] == L':';
                CoTaskMemFree(raw);
                // Kept as its ID list: a phone item's parsing name does not parse back.
                if (namespace_item) {
                    std::wstring id = shell_id::FromItem(item);
                    if (!id.empty()) shell_ids.push_back(std::move(id));
                }
            }
            item->Release();
        }
        items->Release();
        // All or nothing: a mixed selection stays with the shell's own paste.
        if (shell_ids.size() != count) shell_ids.clear();
    }
    if (!shell_ids.empty()) cut = ReadPreferredCut(data);
    data->Release();
    return !shell_ids.empty();
}

bool CompleteCutClipboard(uint32_t sequence, const std::vector<std::wstring>& expected_paths) {
    if (sequence == 0 || expected_paths.empty() || GetClipboardSequenceNumber() != sequence)
        return false;
    ClipboardData current;
    if (!ReadClipboard(current) || !current.cut || current.sequence != sequence ||
        current.paths.size() != expected_paths.size()) return false;
    for (size_t i = 0; i < expected_paths.size(); ++i) {
        if (_wcsicmp(current.paths[i].c_str(), expected_paths[i].c_str()) != 0) return false;
    }
    if (!OpenClipboardWithRetry(ClipboardOwner())) return false;
    if (GetClipboardSequenceNumber() != sequence || !EmptyClipboard()) {
        CloseClipboard();
        return false;
    }
    const UINT format = RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT);
    HGLOBAL value = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    bool ok = false;
    if (format && value) {
        auto* effect = static_cast<DWORD*>(GlobalLock(value));
        if (effect) {
            *effect = DROPEFFECT_MOVE;
            GlobalUnlock(value);
            ok = SetClipboardData(format, value) != nullptr;
        }
        if (!ok) GlobalFree(value);
    }
    CloseClipboard();
    return ok;
}

} // namespace pulse::ops

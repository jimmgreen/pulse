// drag_drop.h — OLE drag & drop for the file list.
//
// Drag-out: FileDataObject (CF_HDROP + CFSTR_PREFERREDDROPEFFECT) +
// ListDropSource, so drags land in Explorer or any OLE target.
// Drop-in: WindowDropTarget registered on the main window; all hit
// decisions (folder row / breadcrumb segment / sidebar item / tray) are
// delegated to the app through DropTargetCallbacks.
//
// The effect helpers are pure functions so the console self-test can cover
// the Explorer-compatible modifier semantics (same volume => move,
// cross volume => copy, Ctrl => copy, Shift => move) without a GUI.
#pragma once
#include <windows.h>
#include <objidl.h>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

// ---------------------------------------------------------------------------
// Pure helpers (unit-testable, no OLE).
// ---------------------------------------------------------------------------

// "C:\" for drive paths, "\\server\share" for UNC, "" when undetermined.
std::wstring VolumeRoot(const std::wstring& path);

// Explorer modifier semantics. allowed is the DoDragDrop dwOKEffects mask.
DWORD ComputeDropEffect(DWORD key_state, const std::wstring& source_sample,
                        const std::wstring& dest_dir, DWORD allowed);

// ---------------------------------------------------------------------------
// IDataObject inspection (drop-in side).
// ---------------------------------------------------------------------------
bool ExtractHDropPaths(IDataObject* obj, std::vector<std::wstring>& out);
DWORD PreferredDropEffect(IDataObject* obj); // 0 when absent

// ---------------------------------------------------------------------------
// Drag-out objects.
// ---------------------------------------------------------------------------
class FileDataObject final : public IDataObject {
public:
    // ref-counted; initial ref = 1. paths must be plain (no \\?\ prefix).
    static FileDataObject* Create(const std::vector<std::wstring>& paths);

    DWORD PerformedEffect() const { return performed_effect_; }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;
    IFACEMETHODIMP GetData(FORMATETC* fmt, STGMEDIUM* out) override;
    IFACEMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    IFACEMETHODIMP QueryGetData(FORMATETC* fmt) override;
    IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP SetData(FORMATETC* fmt, STGMEDIUM* medium, BOOL release) override;
    IFACEMETHODIMP EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override;
    IFACEMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    IFACEMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    explicit FileDataObject(const std::vector<std::wstring>& paths);

    HGLOBAL RenderHDrop() const;
    HGLOBAL RenderDword(DWORD v) const;
    bool IsSupportedFormat(const FORMATETC* fmt) const;

    LONG ref_ = 1;
    std::vector<std::wstring> paths_;
    DWORD performed_effect_ = 0;
};

class ListDropSource final : public IDropSource {
public:
    // When set and returning true, Esc is consumed by the drop target
    // (spring-loaded "go back") instead of cancelling the drag.
    void SetEscHook(std::function<bool()> hook) { esc_hook_ = std::move(hook); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override;
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    IFACEMETHODIMP_(ULONG) Release() override;
    IFACEMETHODIMP QueryContinueDrag(BOOL escape_pressed, DWORD key_state) override;
    IFACEMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    LONG ref_ = 1;
    std::function<bool()> esc_hook_;
};

// Runs the modal DoDragDrop loop. Returns the performed DROPEFFECT_*.
DWORD DoFileDragDrop(const std::vector<std::wstring>& paths, DWORD allowed_effects,
                     std::function<bool()> esc_consumed);

// ---------------------------------------------------------------------------
// Drop-in target.
// ---------------------------------------------------------------------------
struct DropTargetCallbacks {
    // pt in client coordinates; sources already extracted from CF_HDROP.
    // Return the DROPEFFECT_* to show (badge + cursor follow it).
    std::function<DWORD(const std::vector<std::wstring>& sources, POINT pt,
                        DWORD key_state, DWORD allowed)> drag_over;
    std::function<void()> drag_leave;
    // Returns the performed effect.
    std::function<DWORD(const std::vector<std::wstring>& sources, POINT pt,
                        DWORD key_state, DWORD preferred)> drop;
};

class WindowDropTarget final : public IDropTarget {
public:
    WindowDropTarget(HWND hwnd, DropTargetCallbacks cb);

    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override;
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    IFACEMETHODIMP_(ULONG) Release() override;
    IFACEMETHODIMP DragEnter(IDataObject* obj, DWORD key_state, POINTL pt, DWORD* effect) override;
    IFACEMETHODIMP DragOver(DWORD key_state, POINTL pt, DWORD* effect) override;
    IFACEMETHODIMP DragLeave() override;
    IFACEMETHODIMP Drop(IDataObject* obj, DWORD key_state, POINTL pt, DWORD* effect) override;

private:
    POINT ClientPoint(POINTL pt) const;

    LONG ref_ = 1;
    HWND hwnd_ = nullptr;
    DropTargetCallbacks cb_;
    std::vector<std::wstring> sources_; // cached from DragEnter
    DWORD preferred_ = 0;
};

} // namespace pulse::ui

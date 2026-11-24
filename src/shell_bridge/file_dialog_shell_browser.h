#pragma once

#include "file_dialog_events.h"

#include <atomic>
#include <cstdio>
#include <wrl/client.h>

namespace pulse::shell_bridge::detail {

// Read-only shell view of the current picker. No native view is created and no
// reference to the dialog is retained by a sink that caches this service.
class FileDialogShellBrowser final : public IShellBrowser, public IShellView, public IFolderView2 {
public:
    using Validate = HRESULT (*)(IFileDialog*, uint64_t) noexcept;
    using Selection = HRESULT (*)(IFileDialog*, BOOL, IShellItemArray**) noexcept;
    using Trace = void (*)(IFileDialog*, const char*, const IID*) noexcept;

    FileDialogShellBrowser(const std::shared_ptr<FileDialogEventOwner>& owner,
                           uint64_t sequence, Validate validate, Selection selection, Trace trace) noexcept
        : owner_(owner), sequence_(sequence), validate_(validate), selection_(selection), trace_(trace) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return Record("QueryInterface", E_POINTER, 0, &iid);
        *output = nullptr;
        if (iid == IID_IUnknown || iid == IID_IShellBrowser || iid == IID_IOleWindow)
            *output = static_cast<IShellBrowser*>(this);
        else if (iid == IID_IShellView)
            *output = static_cast<IShellView*>(this);
        else if (iid == IID_IFolderView)
            *output = static_cast<IFolderView*>(this);
        else if (iid == IID_IFolderView2)
            *output = static_cast<IFolderView2*>(this);
        else
            return Record("QueryInterface", E_NOINTERFACE, 0, &iid);
        AddRef();
        return Record("QueryInterface", S_OK, 0, &iid);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetWindow(HWND* output) override {
        if (!output) return Record("GetWindow", E_POINTER);
        *output = nullptr;
        Microsoft::WRL::ComPtr<IFileDialog> dialog;
        HRESULT hr = ReadOwner(dialog);
        Microsoft::WRL::ComPtr<IOleWindow> window;
        if (SUCCEEDED(hr)) hr = dialog.As(&window);
        if (SUCCEEDED(hr)) hr = window->GetWindow(output);
        return Record("GetWindow", hr, *output != nullptr);
    }
    HRESULT STDMETHODCALLTYPE QueryActiveShellView(IShellView** output) override {
        if (!output) return Record("QueryActiveShellView", E_POINTER);
        *output = nullptr;
        Microsoft::WRL::ComPtr<IFileDialog> dialog;
        const HRESULT hr = ReadOwner(dialog);
        if (SUCCEEDED(hr)) {
            *output = static_cast<IShellView*>(this);
            AddRef();
        }
        return Record("QueryActiveShellView", hr);
    }
    HRESULT STDMETHODCALLTYPE GetFolder(REFIID iid, void** output) override {
        if (!output) return Record("GetFolder", E_POINTER, 0, &iid);
        *output = nullptr;
        Microsoft::WRL::ComPtr<IFolderView> folder;
        HRESULT hr = ReadFolderView(folder);
        if (SUCCEEDED(hr)) hr = folder->GetFolder(iid, output);
        return Record("GetFolder", hr, 0, &iid);
    }
    HRESULT STDMETHODCALLTYPE GetItemObject(UINT flags, REFIID iid, void** output) override {
        return ReadItems("GetItemObject", flags, iid, output);
    }
    HRESULT STDMETHODCALLTYPE Items(UINT flags, REFIID iid, void** output) override {
        return ReadItems("Items", flags, iid, output);
    }
    HRESULT STDMETHODCALLTYPE ItemCount(UINT flags, int* count) override {
        if (!count) return Record("ItemCount", E_POINTER, flags);
        *count = 0;
        if (!SelectionFlags(flags)) return Record("ItemCount", E_NOTIMPL, flags);
        Microsoft::WRL::ComPtr<IFolderView> folder;
        HRESULT hr = ReadFolderView(folder);
        if (SUCCEEDED(hr)) hr = folder->ItemCount(SVGIO_SELECTION, count);
        return Record("ItemCount", hr, flags);
    }

    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return Record("ContextSensitiveHelp", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE InsertMenusSB(HMENU, LPOLEMENUGROUPWIDTHS) override { return Record("InsertMenusSB", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SetMenuSB(HMENU, HOLEMENU, HWND) override { return Record("SetMenuSB", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE RemoveMenusSB(HMENU) override { return Record("RemoveMenusSB", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SetStatusTextSB(LPCWSTR) override { return Record("SetStatusTextSB", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE EnableModelessSB(BOOL) override { return Record("EnableModelessSB", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE TranslateAcceleratorSB(MSG*, WORD) override { return Record("TranslateAcceleratorSB", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE BrowseObject(PCUIDLIST_RELATIVE, UINT) override { return Record("BrowseObject", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetViewStateStream(DWORD, IStream** output) override { if (output) *output = nullptr; return Record("GetViewStateStream", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetControlWindow(UINT id, HWND* output) override { if (output) *output = nullptr; return Record("GetControlWindow", E_NOTIMPL, id); }
    HRESULT STDMETHODCALLTYPE SendControlMsg(UINT, UINT message, WPARAM, LPARAM, LRESULT* result) override { if (result) *result = 0; return Record("SendControlMsg", E_NOTIMPL, message); }
    HRESULT STDMETHODCALLTYPE OnViewWindowActive(IShellView*) override { return Record("OnViewWindowActive", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SetToolbarItems(LPTBBUTTONSB, UINT, UINT) override { return Record("SetToolbarItems", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG*) override { return Record("TranslateAccelerator", S_FALSE); }
    HRESULT STDMETHODCALLTYPE EnableModeless(BOOL) override { return Record("EnableModeless", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE UIActivate(UINT) override { return Record("UIActivate", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE Refresh() override { return Record("Refresh", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE CreateViewWindow(IShellView*, LPCFOLDERSETTINGS, IShellBrowser*, RECT*, HWND* output) override { if (output) *output = nullptr; return Record("CreateViewWindow", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE DestroyViewWindow() override { return Record("DestroyViewWindow", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetCurrentInfo(LPFOLDERSETTINGS) override { return Record("GetCurrentInfo", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE AddPropertySheetPages(DWORD, LPFNSVADDPROPSHEETPAGE, LPARAM) override { return Record("AddPropertySheetPages", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SaveViewState() override { return Record("SaveViewState", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SelectItem(PCUITEMID_CHILD, SVSIF) override { return Record("SelectItem", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetCurrentViewMode(UINT*) override { return Record("GetCurrentViewMode", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SetCurrentViewMode(UINT) override { return Record("SetCurrentViewMode", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE Item(int, PITEMID_CHILD* output) override { if (output) *output = nullptr; return Record("Item", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetSelectionMarkedItem(int*) override { return Record("GetSelectionMarkedItem", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetFocusedItem(int*) override { return Record("GetFocusedItem", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetItemPosition(PCUITEMID_CHILD, POINT*) override { return Record("GetItemPosition", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetSpacing(POINT*) override { return Record("GetSpacing", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetDefaultSpacing(POINT*) override { return Record("GetDefaultSpacing", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetAutoArrange() override { return Record("GetAutoArrange", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SelectItem(int, DWORD) override { return Record("SelectItem", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SelectAndPositionItems(UINT, PCUITEMID_CHILD_ARRAY, POINT*, DWORD) override { return Record("SelectAndPositionItems", E_NOTIMPL); }

    HRESULT STDMETHODCALLTYPE GetSelection(BOOL none_implies_folder, IShellItemArray** output) override {
        const UINT fallback = none_implies_folder ? 1u : 0u;
        if (!output) return Record("GetSelection", E_POINTER, fallback);
        *output = nullptr;
        Microsoft::WRL::ComPtr<IFileDialog> dialog;
        HRESULT hr = ReadOwner(dialog);
        if (SUCCEEDED(hr)) hr = selection_(dialog.Get(), none_implies_folder, output);
        return Record("GetSelection", hr, fallback);
    }

    HRESULT STDMETHODCALLTYPE SetGroupBy(REFPROPERTYKEY, BOOL) override { return Record("SetGroupBy", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetGroupBy(PROPERTYKEY*, BOOL*) override { return Record("GetGroupBy", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SetViewProperty(PCUITEMID_CHILD, REFPROPERTYKEY, REFPROPVARIANT) override { return Record("SetViewProperty", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetViewProperty(PCUITEMID_CHILD, REFPROPERTYKEY, PROPVARIANT*) override { return Record("GetViewProperty", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SetTileViewProperties(PCUITEMID_CHILD, LPCWSTR) override { return Record("SetTileViewProperties", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SetExtendedTileViewProperties(PCUITEMID_CHILD, LPCWSTR) override { return Record("SetExtendedTileViewProperties", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SetText(FVTEXTTYPE, LPCWSTR) override { return Record("SetText", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SetCurrentFolderFlags(DWORD, DWORD) override { return Record("SetCurrentFolderFlags", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetCurrentFolderFlags(DWORD* flags) override {
        if (!flags) return Record("GetCurrentFolderFlags", E_POINTER);
        *flags = 0;
        Microsoft::WRL::ComPtr<IFileDialog> dialog;
        HRESULT hr = ReadOwner(dialog);
        FILEOPENDIALOGOPTIONS options{};
        if (SUCCEEDED(hr)) hr = dialog->GetOptions(&options);
        // Do not advertise Explorer-only presentation or shell search features.
        if (SUCCEEDED(hr) && !(options & FOS_ALLOWMULTISELECT)) *flags = FWF_SINGLESEL;
        return Record("GetCurrentFolderFlags", hr, *flags);
    }
    HRESULT STDMETHODCALLTYPE GetSortColumnCount(int*) override { return Record("GetSortColumnCount", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SetSortColumns(const SORTCOLUMN*, int) override { return Record("SetSortColumns", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetSortColumns(SORTCOLUMN*, int) override { return Record("GetSortColumns", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetItem(int, REFIID iid, void** output) override {
        if (output) *output = nullptr;
        return Record("GetItem", E_NOTIMPL, 0, &iid);
    }
    HRESULT STDMETHODCALLTYPE GetVisibleItem(int, BOOL, int*) override { return Record("GetVisibleItem", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetSelectedItem(int, int*) override { return Record("GetSelectedItem", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetSelectionState(PCUITEMID_CHILD, DWORD*) override { return Record("GetSelectionState", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE InvokeVerbOnSelection(LPCSTR) override { return Record("InvokeVerbOnSelection", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SetViewModeAndIconSize(FOLDERVIEWMODE, int) override { return Record("SetViewModeAndIconSize", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetViewModeAndIconSize(FOLDERVIEWMODE*, int*) override { return Record("GetViewModeAndIconSize", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SetGroupSubsetCount(UINT) override { return Record("SetGroupSubsetCount", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE GetGroupSubsetCount(UINT*) override { return Record("GetGroupSubsetCount", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE SetRedraw(BOOL) override { return Record("SetRedraw", E_NOTIMPL); }
    HRESULT STDMETHODCALLTYPE IsMoveInSameFolder() override { return Record("IsMoveInSameFolder", S_FALSE); }
    HRESULT STDMETHODCALLTYPE DoRename() override { return Record("DoRename", E_NOTIMPL); }

private:
    HRESULT PinOwner(Microsoft::WRL::ComPtr<IFileDialog>& dialog) {
        const auto owner = owner_.lock();
        if (!owner) return CO_E_OBJNOTCONNECTED;
        if (GetCurrentThreadId() != owner->thread) return RPC_E_WRONG_THREAD;
        {
            std::lock_guard lock(owner->mutex);
            if (!owner->dialog) return CO_E_OBJNOTCONNECTED;
            dialog = owner->dialog;
        }
        return S_OK;
    }

    HRESULT ReadOwner(Microsoft::WRL::ComPtr<IFileDialog>& dialog) {
        const HRESULT hr = PinOwner(dialog);
        return FAILED(hr) ? hr : validate_(dialog.Get(), sequence_);
    }

    HRESULT ReadFolderView(Microsoft::WRL::ComPtr<IFolderView>& folder) {
        Microsoft::WRL::ComPtr<IFileDialog> dialog;
        const HRESULT hr = ReadOwner(dialog);
        return FAILED(hr) ? hr : dialog.As(&folder);
    }

    static bool SelectionFlags(UINT flags) noexcept {
        return (flags & ~static_cast<UINT>(SVGIO_FLAG_VIEWORDER)) == SVGIO_SELECTION;
    }

    HRESULT ReadItems(const char* method, UINT flags, REFIID iid, void** output) {
        if (!output) return Record(method, E_POINTER, flags, &iid);
        *output = nullptr;
        if (!SelectionFlags(flags)) return Record(method, E_NOTIMPL, flags, &iid);
        Microsoft::WRL::ComPtr<IFolderView> folder;
        HRESULT hr = ReadFolderView(folder);
        if (SUCCEEDED(hr)) hr = folder->Items(SVGIO_SELECTION, iid, output);
        return Record(method, hr, flags, &iid);
    }

    HRESULT Record(const char* method, HRESULT hr, UINT arg = 0, const IID* iid = nullptr) {
        Microsoft::WRL::ComPtr<IFileDialog> dialog;
        if (SUCCEEDED(PinOwner(dialog))) {
            char line[144]{};
            std::snprintf(line, sizeof(line), "picker-browser-call method=%s hr=0x%08lX arg=%u show=%llu",
                          method, static_cast<unsigned long>(hr), arg, static_cast<unsigned long long>(sequence_));
            trace_(dialog.Get(), line, iid);
        }
        return hr;
    }

    std::atomic<ULONG> references_{1};
    std::weak_ptr<FileDialogEventOwner> owner_;
    uint64_t sequence_;
    Validate validate_;
    Selection selection_;
    Trace trace_;
};

} // namespace pulse::shell_bridge::detail

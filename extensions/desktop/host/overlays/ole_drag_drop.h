// ole_drag_drop.h - COM IDropTarget, IDropSource, and CF_HDROP IDataObject for
// bidirectional drag-and-drop between Pulse/Explorer and the Desktop Enhancement surface.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleidl.h>
#include <shlobj.h>
#include <wrl/client.h>

#include "host/model/desktop_model.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::desktop::overlays {

enum class DropZoneKind : uint8_t {
    None = 0,
    BareDesktop = 1,
    GroupCard = 2,
    PinnedFolderTile = 3,
    RecycleBinTile = 4,
    StagingTrayTile = 5,
    DockBar = 6,
    DockTrash = 7,
    DockApp = 8,     // an app icon on the Dock: the files open with it
};

struct DropZoneQueryResult {
    DropZoneKind kind = DropZoneKind::BareDesktop;
    std::string target_id;          // group_id or tile_id
    std::wstring target_title;      // group title or folder name
    std::wstring target_path;       // destination folder path when applicable
    std::wstring hint;              // the pill text when the host words it (DockApp: 用「…」打开)
};

struct DropTargetCallbacks {
    // Hit-tests client-space point and returns which zone is under the cursor.
    std::function<DropZoneQueryResult(model::PointI client_pt)> hit_test_zone;

    // Updates pointer-following pill ("移动到分组…" / "加入 Pulse 暂存区" / "复制到文件夹…").
    std::function<void(bool visible, model::PointI client_pt, const std::wstring& label)> on_drag_hint;

    // Commits dropped files onto the resolved zone.
    std::function<void(const DropZoneQueryResult& zone, model::PointI client_pt,
                       const std::vector<std::wstring>& paths, DWORD key_state, DWORD& io_effect)> on_drop_files;
    // Screen pixels to the host's client coordinates (DIPs); ScreenToClient pixels when unset.
    std::function<model::PointI(POINTL)> to_client;
};

class DesktopDropTarget final : public IDropTarget {
public:
    DesktopDropTarget(HWND hwnd, DropTargetCallbacks callbacks);
    ~DesktopDropTarget() = default;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDropTarget
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data_obj, DWORD key_state, POINTL pt, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD key_state, POINTL pt, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data_obj, DWORD key_state, POINTL pt, DWORD* effect) override;

private:
    model::PointI ToClientPoint(POINTL screen_pt) const noexcept;
    DWORD EvaluateHover(model::PointI client_pt, DWORD key_state, DropZoneQueryResult& out_zone,
                        std::wstring& out_hint) const;

    std::atomic<ULONG> ref_count_{1};
    HWND hwnd_ = nullptr;
    DropTargetCallbacks callbacks_;
    bool has_hdrop_ = false;
    std::vector<std::wstring> cached_paths_;
};

class DesktopDropManager {
public:
    DesktopDropManager() = default;
    ~DesktopDropManager();
    DesktopDropManager(const DesktopDropManager&) = delete;
    DesktopDropManager& operator=(const DesktopDropManager&) = delete;

    bool Attach(HWND hwnd, DropTargetCallbacks callbacks);
    void Detach() noexcept;

    // Capture does not change the actual window under the pointer. Descendants
    // remain internal, but an overlapping application is an external target.
    static bool IsExternalDragWindow(HWND desktop, HWND under_pointer) noexcept;

    using DragDropRunner = decltype(&DoDragDrop);
    // Initiates an outbound OLE CF_HDROP drag from Desktop items to other applications.
    static bool StartFileDragOut(const std::vector<std::wstring>& file_paths,
                                 DWORD allowed_effects,
                                 DWORD* out_performed_effect = nullptr,
                                 DragDropRunner run_drag_drop = &DoDragDrop);

    // Helper to extract CF_HDROP paths from any IDataObject.
    static std::vector<std::wstring> ExtractHDropPaths(IDataObject* data_obj);

private:
    HWND hwnd_ = nullptr;
    bool ole_initialized_ = false;
    Microsoft::WRL::ComPtr<DesktopDropTarget> target_;
};

} // namespace pulse::desktop::overlays

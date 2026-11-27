#pragma once
#include "../app/extensions/ui_tree.h"
#include "extension_data_controller.h"
#include <d2d1.h>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <windows.h>
namespace pulse::ui {
enum class ExtensionStateSource : uint32_t { User = 1, Programmatic = 2 };
struct ExtensionStateChange {
    std::string key, value;
    uint64_t revision = 0;
    uint32_t source_mask = 0;
};
struct ExtensionTreeCallbacks {
    std::function<void(const std::string &)> on_command;
    std::function<bool(const std::string &)> command_enabled;
    std::function<std::optional<std::string>(const std::string &)> read_binding;
    UiDataProvider request_data;
    std::function<void(uint64_t, const std::vector<ExtensionStateChange> &)> on_state_changed;
};
// Padding (physical px) the view keeps around its content on every side.
constexpr int kExtensionTreeContentInset = 8;
struct ExtensionTreePaintStats {
    uint64_t frames = 0;
    uint64_t partial_frames = 0;
    uint64_t drawn_items = 0;
};
class ExtensionTreeView {
  public:
    ExtensionTreeView();
    ~ExtensionTreeView();
    bool Create(HWND parent, const extensions::UiTree &tree, ExtensionTreeCallbacks callbacks, bool dark,
                D2D1_COLOR_F accent, float scale);
    void SetBounds(RECT bounds);
    void SetTheme(bool dark, D2D1_COLOR_F accent, float scale);
    // Leave the background unpainted so the host page (Settings' Mica) shows between cards.
    void SetTransparentBackground(bool transparent);
    // With a transparent background: the host page's wash to clear with (straight alpha).
    void SetBackgroundWash(D2D1_COLOR_F wash);
    void Refresh();
    void Close();
    HWND Hwnd() const;
    HWND NodeWindow(const std::string &id) const;
    std::optional<std::string> StateValue(const std::string &key) const;
    bool SetState(const std::string &key, const std::string &value);
    // Host-driven edit: commits like a user change (USER source reaches subscribers).
    bool CommitUserState(const std::string &key, const std::string &value);
    uint64_t StateRevision() const;
    bool InputComposing() const;
    // Height of the laid-out content in physical px, excluding the content inset.
    int ContentHeight() const;
    std::vector<extensions::UiStateEntry> StateValues() const;
    bool SetStates(const std::vector<extensions::UiStateEntry> &changes);
    // Host-only patch entry point: replacement must first pass extensions::ParseUiTree.
    // Runtime checks below additionally protect state and stable HWND identity.
    bool UpdateTree(const extensions::UiTree &tree);
    bool RevealNode(const std::string &id);
    std::optional<extensions::UiNode> DataNode(const std::string &id) const;
    // Host-owned bindings attach to existing empty data controls, never create widgets.
    bool BindDataSource(const std::string &id, const std::optional<extensions::UiDataSource> &source);
    bool RequestData(const std::string &id, const std::string &operation);
    extensions::UiTree SnapshotTree() const;
    void CopySnapshotViewState(const ExtensionTreeView &source);
    bool PaintTo(HDC target, int x, int y);
    ExtensionTreePaintStats PaintStats() const;
    std::optional<D2D1_RECT_F> NodeBounds(const std::string &id) const;
    // Reads the last submitted frame, only when Compositor frame copying is enabled.
    // Does not schedule or perform a render.
    bool CopyPresentedPixels(std::vector<uint32_t> &bgra, UINT &width, UINT &height) const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace pulse::ui

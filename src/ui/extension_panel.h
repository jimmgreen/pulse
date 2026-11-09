#pragma once
#include <windows.h>
#include <d2d1.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "extension_tree.h"

namespace pulse::ui {
enum class ExtensionPanelLayout { Auto, Stack, Grid };
enum class ExtensionPanelDensity { Comfortable, Compact };
enum class ExtensionPanelTitleTone { Default, Accent };
D2D1_COLOR_F ExtensionPanelTitleColor(ExtensionPanelTitleTone tone, bool high_contrast,
    D2D1_COLOR_F accent, D2D1_COLOR_F background, D2D1_COLOR_F text);
struct ExtensionPanelGroup {
    std::wstring title;
    std::wstring description;
    std::vector<size_t> buttons;
};
inline int ExtensionPanelColumns(ExtensionPanelLayout layout, size_t buttons, float width_dip) {
    return layout != ExtensionPanelLayout::Stack && width_dip >= 420.0f &&
        (layout == ExtensionPanelLayout::Grid || buttons > 4) ? 2 : 1;
}

struct ExtensionPanelField {
    std::wstring label;
    std::wstring value;
};

struct ExtensionPanelSpec {
    // Host preview surfaces own their chrome and close/navigation controls.
    bool content_only = false;
    std::optional<extensions::UiTree> tree;
    ExtensionTreeCallbacks tree_callbacks;
    std::wstring title;
    std::wstring description;
    std::vector<std::wstring> buttons;
    std::function<void(size_t)> on_command;
    std::function<std::vector<bool>()> read_enabled;
    std::vector<ExtensionPanelField> fields;
    std::function<std::vector<std::wstring>()> read_fields;
    ExtensionPanelLayout layout = ExtensionPanelLayout::Auto;
    ExtensionPanelDensity density = ExtensionPanelDensity::Comfortable;
    std::function<void()> on_closed;
    std::vector<ExtensionPanelGroup> groups;
    std::optional<size_t> primary_button;
    ExtensionPanelTitleTone title_tone = ExtensionPanelTitleTone::Default;
};

// UI-thread owned. Closing the owner also closes this owned tool window.
class ExtensionPanel {
public:
    ExtensionPanel();
    ~ExtensionPanel();
    ExtensionPanel(const ExtensionPanel&) = delete;
    ExtensionPanel& operator=(const ExtensionPanel&) = delete;
    bool Show(HWND owner, ExtensionPanelSpec spec, bool dark, D2D1_COLOR_F accent, float scale);
    bool ShowDocked(HWND parent, const RECT& bounds, ExtensionPanelSpec spec, bool dark, D2D1_COLOR_F accent, float scale, bool visible = true);
    bool IsDocked() const;
    void UpdateDocked(const RECT& bounds, bool dark, D2D1_COLOR_F accent, float scale);
    // Moves the same HWND/tree between owned-window and child hosting.
    bool Remount(HWND owner, const RECT* dock_bounds, bool dark, D2D1_COLOR_F accent, float scale);
    void UpdateAppearance(bool dark, D2D1_COLOR_F accent, float scale);
    bool Undock(bool dark, D2D1_COLOR_F accent, float scale);
    bool SaveCurrentFrame(const wchar_t* path);
    void Close();
    bool IsOpen() const;
    HWND Hwnd() const;
    HWND TreeNodeWindow(const std::string& id) const;
    std::optional<std::string> TreeStateValue(const std::string& key) const;
    uint64_t Instance() const;
    float DpiScale() const;
    uint64_t TreeStateRevision() const;
    bool SetTreeStates(const std::vector<extensions::UiStateEntry>& values);
    // The caller must validate its complete typed/manifest tree before this transaction.
    bool UpdateTree(const extensions::UiTree& tree);
    bool RevealTreeNode(const std::string& id);
    std::optional<extensions::UiNode> TreeDataNode(const std::string& id) const;
    bool BindTreeData(const std::string& id, const std::optional<extensions::UiDataSource>& source);
    bool RequestTreeData(const std::string& id, const std::string& operation);
    // Mouse, keyboard and probes share this index/open/callback gate.
    bool InvokeButton(size_t index);
    RECT ButtonBounds(size_t index) const;
    // True for a valid complete read, including unchanged values.
    bool RefreshFields();
    bool RefreshEnabled();
    bool ButtonEnabled(size_t index) const;
    std::vector<std::wstring> FieldValues() const;
    bool SaveDebugSnapshot(const wchar_t* path, bool dark, float scale, const std::string& reveal_node = {});
    // Static authoring preview: callbacks are discarded before a hidden window is created.
    static bool SavePreviewSnapshot(HWND owner, ExtensionPanelSpec spec, const wchar_t* path,
        bool dark, D2D1_COLOR_F accent, float scale);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

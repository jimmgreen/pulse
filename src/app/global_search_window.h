#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "../ui/window_material.h"

namespace pulse {
namespace app { class JumpHistory; }
// What the quick search window hands to the main window when the user asks to
// continue there (Shift+Enter or the footer link). folder is empty when the
// search covers every indexed location.
struct GlobalSearchHandoff {
    std::wstring query;
    bool content = false;
    std::wstring folder;
    // Filter tokens from the type / date chips (" type:image dm:today"), each
    // with a leading space so they append straight onto the query.
    std::wstring filters;
};
using GlobalSearchHandoffHandler = std::function<void(const GlobalSearchHandoff&)>;

struct GlobalSearchRecentFile {
    std::wstring path;
    bool directory = false;
};
// What the popup shows before anything is typed, plus its remembered size.
// The controller refreshes it before every Show.
struct GlobalSearchContext {
    std::vector<std::wstring> pinned_folders;   // Quick access pins (first few become scopes)
    std::vector<std::wstring> recent_queries;
    std::vector<GlobalSearchRecentFile> recent_files;
    // Folders Pulse already knows, most relevant first: open tabs, Quick
    // access, recently opened folders. Typed text matches them instantly
    // (跳转 group, above the index results).
    struct JumpFolder { std::wstring path, label; };  // label: shown name when not the folder name
    std::vector<JumpFolder> jump_folders;
    // 应用 group: Start menu programs right after the best matches, the
    // most started first among equal matches (history: may be null).
    bool show_apps = true;
    std::shared_ptr<app::JumpHistory> history;
    int width = 0, height = 0;                  // DIP; 0 = default
};
struct GlobalSearchEvents {
    // After hiding the popup; true means the host owns this open request.
    std::function<bool(const std::wstring& path, bool directory, bool location)> open_path;
    std::function<void(const std::vector<std::wstring>& paths)> stage_paths;
    std::function<std::vector<std::wstring>()> staged_paths;
    std::function<void(const std::wstring& path)> unstage_path;
    std::function<void()> clear_staging;
    std::function<void()> toggle_theme;
    std::function<void()> clear_search_history;
    std::function<void(int width, int height)> resized;          // DIP, after a user resize
    std::function<void(const std::wstring& path)> pin_folder;     // 添加常用位置…
    std::function<void(const GlobalSearchHandoff& search, const std::wstring& path, bool directory)> opened;
};

class GlobalSearchWindow {
public:
    GlobalSearchWindow();
    ~GlobalSearchWindow();
    GlobalSearchWindow(const GlobalSearchWindow&) = delete;
    GlobalSearchWindow& operator=(const GlobalSearchWindow&) = delete;
    bool Show(HWND owner, bool dark, float scale, const std::wstring& current_folder, bool search_pinyin = true);
    void SetAppearance(bool dark, ui::WindowEffect effect, const std::wstring& background_image, D2D1_COLOR_F accent);
    // Called on the UI thread after the popup has hidden itself.
    void SetHandoffHandler(GlobalSearchHandoffHandler handler);
    void SetContext(GlobalSearchContext context);
    void SetEvents(GlobalSearchEvents events);
    void Hide();
    // Creates the hidden popup and renders one frame `delay_ms` from now,
    // once and only while no input is pending, so the first summon is warm.
    // `prepare` runs first, on the UI thread (context and appearance).
    void Prewarm(uint32_t delay_ms, std::function<void()> prepare);
    void Shutdown();
    bool Visible() const;
private:
    friend struct GlobalSearchWindowTestPeer;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

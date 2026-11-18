// connect_server_dialog.h - 连接服务器: one
// address field with a live "将被理解为" line, server suggestions, 连接后固定
// and 同时映射为驱动器 with a drive letter. The same window, in rename mode,
// renames a network pin (one field and a note).
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

struct ConnectServerSuggestion {
    std::wstring path;  // what the row fills in (\\host or \\host\share)
    std::wstring hint;  // where it came from: 已固定 / 局域网设备 / 最近访问
};

struct ConnectServerPreview {
    bool ok = false;       // 连接 is allowed
    bool can_map = false;  // a share root: 映射为驱动器 is offered
    std::wstring text;     // the line under the field (empty: nothing to say)
};

struct ConnectServerDialogSpec {
    bool rename = false;  // rename mode: label, field, note, 取消 / confirm
    std::wstring title;
    std::wstring label;
    std::wstring note;  // rename mode only
    std::wstring initial_text;
    std::wstring confirm_text;
    // Called on every edit; both run on the UI thread and must not block.
    std::function<std::vector<ConnectServerSuggestion>(const std::wstring&)> suggestions;
    std::function<ConnectServerPreview(const std::wstring&)> preview;
    std::vector<wchar_t> free_letters;  // offered drive letters, Z: first
    bool pin = true;
};

struct ConnectServerDialogResult {
    bool accepted = false;
    std::wstring text;
    bool pin = false;
    bool map = false;
    wchar_t letter = 0;
};

ConnectServerDialogResult ShowConnectServerDialog(HWND owner, const ConnectServerDialogSpec& spec,
                                                  bool dark, D2D1_COLOR_F accent);

} // namespace pulse::ui

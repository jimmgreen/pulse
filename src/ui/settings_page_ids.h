// settings_page_ids.h - Settings page numbers named in both app/ and ui/
// that are not part of the sidebar order (kSettingsNavOrder).
#pragma once

namespace pulse::ui {

// Settings > 增强包 > AI 与 MCP: a sub-page of 增强包 (8), so the sidebar keeps
// 增强包 selected. It follows the grouped general pages 9..12 (文件列表,
// 标签页与窗口, 启动与集成, 文件操作).
inline constexpr int kSettingsPageAi = 13;

// Settings > AI 助手: Pulse's own assistant (a sidebar row under 增强包).
inline constexpr int kSettingsPageAssistant = 14;
inline constexpr int kSettingsPageNativeExtensions = 15;

} // namespace pulse::ui

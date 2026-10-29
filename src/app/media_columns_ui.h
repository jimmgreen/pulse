#pragma once
// 媒体文件夹专用列 on top of the plain pane model: which folders get them,
// asking media_props for the values, the cell texts, sorting by them.
#include <cstdint>

namespace pulse { struct AppState; }

namespace pulse::app {

// Registers the pane decorator and the sort key source (once per process).
void InstallMediaColumns();
// 设置 › 文件列表 › 媒体文件夹使用专用列.
void SetMediaColumnsEnabled(bool on) noexcept;

}  // namespace pulse::app

namespace pulse {

// New values came in (media::Revision moved): re-sorts listings ordered by a
// media column, paced like the folder-size re-sort (#58). True while one is
// still waiting (motion, a press, the pace).
bool ResortForMediaProps(AppState& s);

}  // namespace pulse

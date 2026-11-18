#pragma once
// 媒体文件夹专用列 on top of the plain pane model: which folders get them,
// asking media_props for the values, the cell texts, sorting by them.
#include <cstdint>

namespace pulse { struct AppState; }

namespace pulse::app {

struct Tab;

// Registers the pane decorator, the sort key source and the media group
// source (once per process).
void InstallMediaColumns();
// 设置 › 文件列表 › 媒体文件夹使用专用列.
void SetMediaColumnsEnabled(bool on) noexcept;
// The media::Kind value of the tab's folder (0: not a media folder, or the
// setting is off): the group wheel offers that kind's media groups.
int MediaFolderKindFor(const Tab& tab);

}  // namespace pulse::app

namespace pulse {

// New values came in (media::Revision moved): re-sorts listings ordered or
// grouped by a media value, paced like the folder-size re-sort (#58). True
// while one is still waiting (motion, a press, the pace).
bool ResortForMediaProps(AppState& s);

}  // namespace pulse

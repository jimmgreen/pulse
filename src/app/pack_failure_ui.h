// pack_failure_ui.h - The corner card of a failed AI / content pack install:
// why it failed in words, and the next step as buttons (网络: 重试 / 下载源设置,
// 不匹配: 检查更新 / 查看详情, 写入失败: 打开文件夹 / 重试).
#pragma once
#include "../common/windows_compat.h"
#include "pack_installer.h"

namespace pulse {
struct AppState;

enum class FailedPack : uint8_t { Ai, Content };

void ShowPackFailure(AppState& s, FailedPack pack, app::PackFailure failure, DWORD error);

// WM_PACK_RETRY .. WM_PACK_OPEN_FOLDER, from that card's buttons.
void HandlePackFailureMessage(AppState& s, UINT message);

// Settings > 增强包, scrolled to 下载源.
void OpenDownloadSourceSetting(AppState& s);

} // namespace pulse

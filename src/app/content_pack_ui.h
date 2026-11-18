// content_pack_ui.h - Asking for, downloading and using the content
// enhancement pack (content_pack.h) from the app window.
#pragma once
#include "../common/windows_compat.h"
#include <string>

namespace pulse {
struct AppState;

// When this build needs the pack and it is missing, outdated or damaged,
// asks whether to download it and starts the download on yes (a toast shows
// progress; the search re-runs once it is in). With `once_per_session` it
// asks at most once per run - switching to content search; explicit actions
// (Enter, starting a duplicate scan) always ask. True when the agent can
// start right now.
bool OfferContentPack(AppState& s, bool once_per_session = false);

// From BuildVm: download progress and outcome, and the silent update of an
// installed pack after Pulse itself was updated (Settings > auto update).
void TickContentPackInstall(AppState& s);

// 重试 on the failure card: downloads again, without asking.
bool RetryContentPackInstall(AppState& s);

// Banner for a content search that failed because the pack is not usable;
// false when that is not why it failed.
bool ContentPackBanner(DWORD error, std::wstring& title, std::wstring& message);

} // namespace pulse

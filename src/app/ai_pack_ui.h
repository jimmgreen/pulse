// ai_pack_ui.h - Downloading, updating and removing the AI enhancement pack
// (ai_pack.h) from the app window. Its card lives on 设置 › 增强包; the
// card shows the download progress itself, toasts only report the outcome.
#pragma once
#include "../common/windows_compat.h"

namespace pulse {
struct AppState;

// Asks whether to download (or update / repair) the pack and starts the
// download on yes. Does nothing while it is ready or already downloading.
void OfferAiPack(AppState& s);

// Stops a running download; the partial files are discarded.
void CancelAiPackInstall();

// Asks, then deletes the installed pack. True when it is gone.
bool ConfirmRemoveAiPack(AppState& s);

// For the settings card.
bool AiPackInstalling() noexcept;
float AiPackInstallProgress() noexcept;  // 0..1

// From BuildVm: the download's outcome, and the silent update of an installed
// pack after Pulse itself was updated (Settings > auto update).
void TickAiPackInstall(AppState& s);

// 重试 on the failure card: downloads again, without asking.
bool RetryAiPackInstall(AppState& s);

} // namespace pulse

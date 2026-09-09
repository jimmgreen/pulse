#pragma once
#include "app_runtime.h"

namespace pulse {
std::vector<std::wstring> QuickAccessTargets(const app::Tab* tab, bool background);
void AppendQuickAccessCommand(AppState& s, std::vector<ui::FluentMenuItem>& items,
                              const std::vector<std::wstring>& paths);
bool HandleQuickAccessCommand(AppState& s, int command,
                              const std::vector<std::wstring>& paths);
void ShowQuickAccessMenu(AppState& s, const std::wstring& path, POINT point);
}

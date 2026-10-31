#pragma once
#include <string>
namespace pulse {
struct AppState;
int RunWindowManagementTest(AppState &state, const std::wstring &report_path);
} // namespace pulse

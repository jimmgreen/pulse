#pragma once
#include <string>
namespace pulse {
struct AppState;
// Explicit isolated test entry. PULSE_TEST_EXTERNAL_OPEN_ONLY=1 selects only
// external-open cases plus their setup/cleanup; other cases are reported skipped.
// Additionally PULSE_TEST_EXTERNAL_OPEN_NOTICE_INSPECT=1 seeds visual notice
// counts through real TickExternalOpen, captures the normal rendered surface in
// the absolute PULSE_TEST_DATA_DIR, and pumps up to 60s for human review. Closing
// the owned fixture ends inspection early. Default tests have no extra delay.
int RunTrayForegroundTestHost(const wchar_t *title);
int RunWindowManagementTest(AppState &state, const std::wstring &report_path);
} // namespace pulse

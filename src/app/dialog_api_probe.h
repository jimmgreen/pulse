#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <string_view>

namespace pulse::app {

inline constexpr wchar_t kDialogApiProbeSwitch[] = L"--windows-dialog-api-test";

// A separate instance so the test exercises the real Windows entry points and
// never blocks the settings window or changes the parent's current directory.
enum class DialogApiScenario { Open = 100, Save, Folder, Legacy };
DWORD LaunchDialogApiProbe(HANDLE& process, DialogApiScenario scenario, bool dark);
using DialogApiResultPresenter = std::function<void(const std::wstring&, bool)>;
int RunDialogApiProbe(std::wstring_view scenario = {}, const DialogApiResultPresenter& present = {});

}

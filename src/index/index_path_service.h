#pragma once
#include <string>

namespace pulse::index {
// Called by the elevated configuration helper, never by the UI thread.
int ConfigureServiceIndexPath(const std::wstring& path);
}

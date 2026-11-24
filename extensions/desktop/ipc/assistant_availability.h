#pragma once

#include <string>
#include <windows.h>

namespace pulse::desktop::ai {

// The desktop UI follows the built-in assistant setting, independently of
// provider, credentials, MCP, and the desktop's rule-based organization.
class AssistantAvailability {
public:
    explicit AssistantAvailability(HANDLE pulse_process = nullptr, std::wstring ai_dir = {});
    bool Enabled();

private:
    std::wstring path_;
    bool default_enabled_ = true;
    bool enabled_ = true;
    bool file_read_ = false;
    ULARGE_INTEGER size_{};
    FILETIME last_write_{};
};

} // namespace pulse::desktop::ai

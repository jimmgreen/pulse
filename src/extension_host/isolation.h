#pragma once
#include <string>
#include <windows.h>

namespace pulse::extension_host {
// PoC only: a process token is the security boundary; a Job supplies resource containment.
class Isolation {
  public:
    Isolation() = default;
    ~Isolation();
    Isolation(const Isolation &) = delete;
    Isolation &operator=(const Isolation &) = delete;
    bool Create();
    bool Launch(const std::wstring &executable, const std::wstring &arguments,
                DWORD diagnostic_memory_mib = 0, bool diagnostic_profile_environment = false,
                HANDLE standard_input = nullptr, HANDLE standard_output = nullptr);
    bool Wait(DWORD milliseconds, DWORD &exit_code);
    bool CloseJobAndWait();
    bool RemoveContainerProfile();
    PSID Sid() const { return sid_; }
    const std::wstring &Stage() const { return stage_; }
    DWORD Error() const { return error_; }

  private:
    bool Fail(const wchar_t *stage, DWORD error);
    PSID sid_ = nullptr;
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    std::wstring profile_;
    std::wstring stage_;
    DWORD error_ = 0;
};
} // namespace pulse::extension_host

#pragma once
#include "update_checker.h"
#include <memory>

namespace pulse::app {
// The caller must keep this read-only handle open until launching the installer.
bool VerifyUpdateInstaller(HANDLE file, std::wstring_view expected_hash);

class UpdateInstaller {
public:
    UpdateInstaller() = default;
    ~UpdateInstaller();
    UpdateInstaller(const UpdateInstaller&) = delete;
    UpdateInstaller& operator=(const UpdateInstaller&) = delete;
    bool Start(const UpdateResult& update, HWND notify, UINT message);
    bool downloading() const noexcept;
    bool installing() const noexcept;
    bool TakeResult(DWORD& error);
    bool Launch(HWND owner, DWORD& error);
    void Stop();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}

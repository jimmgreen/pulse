#pragma once
#include <windows.h>
#include <commdlg.h>
#include <functional>
#include <vector>
#include <span>

namespace pulse::shell_bridge {
// Caller-STA child dialog. No callback or template pointer crosses a process.
class LegacyHookHost {
public:
    explicit LegacyHookHost(OPENFILENAMEW* input) noexcept;
    explicit LegacyHookHost(OPENFILENAMEA* input) noexcept;
    ~LegacyHookHost();
    LegacyHookHost(const LegacyHookHost&) = delete;
    LegacyHookHost& operator=(const LegacyHookHost&) = delete;
    HRESULT Prepare();
    static bool AppendTemplateSupported(std::span<const BYTE> bytes) noexcept;
    using ParentMessage = std::function<LRESULT(UINT, WPARAM, LPARAM)>;
    HRESULT Create(HWND parent, ParentMessage message = {});
    bool Notify(UINT code);
    bool FileOk();
    HWND Window() const noexcept { return child_; }
    bool IsAnsi() const noexcept { return ansi_; }
private:
    static INT_PTR CALLBACK DialogProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ParentProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    ParentMessage parent_message_;
    std::vector<BYTE> template_;
    bool prepared_ = false;
    void* input_ = nullptr;
    LPOFNHOOKPROC hook_ = nullptr;
    HWND child_ = nullptr;
    HWND parent_ = nullptr;
    bool attached_ = false;
    DWORD thread_ = 0;
    bool ansi_ = false;
    UINT_PTR notification_return_ = 0;
    bool notifying_ = false;
};
}

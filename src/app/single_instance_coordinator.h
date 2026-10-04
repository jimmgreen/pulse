#pragma once

#include <windows.h>
#include <string>
#include <string_view>
#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace pulse::app {

class SingleInstanceCoordinator {
public:
    enum class AcquireResult { Primary, Existing, Failed };

    SingleInstanceCoordinator() = default;
    ~SingleInstanceCoordinator();
    SingleInstanceCoordinator(const SingleInstanceCoordinator&) = delete;
    SingleInstanceCoordinator& operator=(const SingleInstanceCoordinator&) = delete;

    AcquireResult Acquire(std::wstring_view mutex_name = {});
    void Release();
    bool ForwardOpenPath(const std::wstring& path, DWORD timeout_ms = 2000) const;
    enum class OpenAcceptance { Invalid, New, Duplicate };
    struct OpenRequest {
        std::array<unsigned char, 16> id{};
        uint64_t deadline = 0;
        std::wstring path;
    };
    static std::vector<unsigned char> EncodeOpenRequest(const OpenRequest& request);
    static bool DecodeOpenRequest(const COPYDATASTRUCT* data, OpenRequest& request);
    OpenAcceptance AcceptOpenRequest(const OpenRequest& request, uint64_t now);
    static ULONG_PTR OpenRequestMessageId() noexcept;
    // The same hand-off to a window the caller already located - a tab dropped
    // on another Pulse window. The target opens the folder as its own tab.
    static bool SendOpenPathToWindow(HWND target, const std::wstring& path,
                                     DWORD timeout_ms = 2000);
    // A tab dropped on another window, together with what the user had selected
    // there: the receiving window puts the same rows back under the cursor and
    // its details preview follows the same file. Names, not indices, because the
    // receiving window sorts and filters on its own.
    struct TabTransfer {
        std::wstring path;
        std::wstring focus_name;
        std::vector<std::wstring> selected_names;
    };
    // The last tab of a window, dropped on another one: the source has nothing
    // left to show and closes, so the target also takes over the singleton
    // resources the source is about to release (mutex, tray, hotkey, session).
    static bool SendTabTransfer(HWND target, const std::wstring& path,
                                DWORD timeout_ms = 2000);
    static bool DecodeTabTransfer(const COPYDATASTRUCT* data, std::wstring& path);
    // The same hand-off carrying the selection. Both shapes are accepted on the
    // receiving side: a window of an older build sends the path-only message.
    static bool SendTabTransfer(HWND target, const TabTransfer& transfer,
                                DWORD timeout_ms = 2000);
    static bool DecodeTabTransfer(const COPYDATASTRUCT* data, TabTransfer& transfer);
    // Turning the multi-window mode off collects the extra windows back into the
    // primary: each of them is asked to hand over every tab and close. The message
    // carries only the receiving window; the sender drains its own tabs in order.
    // The timeout is deliberately short: this is a notification, not a call - the
    // receiver answers by handing its tabs back through this window, so waiting
    // for it would block the window the user is looking at for the whole drain.
    // A timeout still counts as delivered (the message is queued and processed
    // later, see SendBlobPayload), only a dead window fails.
    static bool SendDrainRequest(HWND target, HWND sink, DWORD timeout_ms = 50);
    static bool DecodeDrainRequest(const COPYDATASTRUCT* data, HWND& sink);
    static ULONG_PTR DrainRequestMessageId() noexcept;

    static bool DecodeOpenPath(const COPYDATASTRUCT* data, std::wstring& path);
    static ULONG_PTR OpenPathMessageId() noexcept;
    static const wchar_t* WindowClassName() noexcept;

private:
    HANDLE mutex_ = nullptr;
    std::map<std::array<unsigned char, 16>, OpenRequest> accepted_;
};

} // namespace pulse::app

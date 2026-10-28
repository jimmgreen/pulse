#pragma once
#include <windows.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace pulse {
struct AppState;
namespace app { struct Tab; }

// A slow share may outlive its window. The worker owns only this mailbox.
struct AddressPathOpen {
    struct Result { std::wstring path; DWORD attrs = INVALID_FILE_ATTRIBUTES; uint64_t generation = 0; };
    std::mutex mutex;
    std::optional<Result> pending, result;
    bool running = false;
    uint64_t generation = 0;
    const app::Tab* tab = nullptr;
    uint64_t view_generation = 0;
    std::wstring origin;
};
inline constexpr UINT_PTR kAddressPathOpenTimer = 0x41504F;
void OpenAddressPath(AppState& s, const std::wstring& path);
void PollAddressPathOpen(AppState& s);
}

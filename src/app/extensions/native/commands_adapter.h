#pragma once
#include "../../../../sdk/native/include/pulse/commands.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace pulse::extensions::native {
struct CommandInfo {
    int token = 0;
    std::string identity, title;
    uint32_t icon = 0, key = 0, modifiers = 0;
    bool enabled = false;
};
class CommandsAdapter {
public:
    CommandsAdapter(HWND owner, std::string package, PulseContextV1 context, std::function<bool()> live);
    ~CommandsAdapter();
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;
    static std::vector<CommandInfo> Entries(HWND owner);
    static std::string Identity(HWND owner, int token);
    static bool Invoke(HWND owner, int token);
    static bool Shortcut(HWND owner, uint32_t key, uint32_t modifiers);
private:
    struct State;
    std::shared_ptr<State> state_;
};
}

#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace pulse::seven_zip {

inline constexpr std::string_view kPreferencesKey = "7zip.preferences";
inline constexpr std::array<std::string_view, 6> kMenuCommands{
    "extract", "extract-to", "test-archive", "quick-compress", "add-to-archive", "checksums"};
inline constexpr unsigned kAllMenuCommands = (1u << kMenuCommands.size()) - 1;

struct CommandPreferences {
    int mode = 0, location = 0, format = 0, level = 3;
    unsigned commands = kAllMenuCommands;
    bool remember = true, submenu = false, takeover = false, preview_on_open = false;
    std::string_view groups = "fff", folder;
};

inline std::optional<CommandPreferences> ParseCommandPreferences(std::string_view input) {
    auto field = [&]() -> std::string_view {
        const auto at = input.find('\n');
        if (at == input.npos) return {};
        const auto value = input.substr(0, at);
        input.remove_prefix(at + 1);
        return value;
    };
    auto digit = [&](char max) -> int {
        const auto value = field();
        return value.size() == 1 && value.front() >= '0' && value.front() <= max ? value.front() - '0' : -1;
    };
    auto hex = [](std::string_view value, size_t length) -> int {
        if (value.size() != length) return -1;
        int result = 0;
        for (const char c : value) {
            if (c >= '0' && c <= '9') result = result * 16 + c - '0';
            else if (c >= 'a' && c <= 'f') result = result * 16 + c - 'a' + 10;
            else return -1;
        }
        return result;
    };
    const int version = digit('5');
    if (version < 3) return {};
    CommandPreferences prefs;
    prefs.mode = digit('2'); prefs.location = digit('1'); prefs.format = digit('5'); prefs.level = digit('5');
    const int remember = digit('1');
    if (prefs.mode < 0 || prefs.location < 0 || prefs.format < 0 || prefs.level < 0 || remember < 0) return {};
    prefs.remember = remember != 0;
    if (version >= 4) {
        const int placement = digit('1');
        const int commands = hex(field(), 2);
        if (placement < 0 || commands < 0 || static_cast<unsigned>(commands) > kAllMenuCommands) return {};
        prefs.submenu = placement != 0;
        prefs.commands = static_cast<unsigned>(commands);
    }
    if (version == 5) {
        const int takeover = digit('1'), behavior = digit('1');
        prefs.groups = field();
        if (takeover < 0 || behavior < 0 || hex(prefs.groups, 3) < 0) return {};
        prefs.takeover = takeover != 0;
        prefs.preview_on_open = behavior != 0;
    }
    prefs.folder = input;
    return prefs;
}

} // namespace pulse::seven_zip

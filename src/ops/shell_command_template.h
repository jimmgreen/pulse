#pragma once
#include "../common/command_line.h"
#include "../common/path_utils.h"
#include <string>
#include <string_view>

namespace pulse::ops {
inline std::wstring ExpandShellCommand(std::wstring_view command, const std::wstring& path,
                                      bool* replaced = nullptr) {
    if (replaced) *replaced = false;
    const std::wstring clean_path = path::StripExtendedPathPrefix(path);
    const std::wstring quoted = QuoteWindowsArgument(clean_path);
    std::wstring dir = clean_path;
    if (const size_t slash = clean_path.find_last_of(L"\\/"); slash != std::wstring::npos) {
        const bool drive_root = slash == 2 && clean_path[1] == L':';
        dir.resize(drive_root ? 3 : slash);
    }
    const std::wstring quoted_dir = QuoteWindowsArgument(dir);
    const auto placeholder = [&](size_t offset) {
        return offset + 1 < command.size() && command[offset] == L'%' &&
            std::wstring_view(L"1LlVvWw*").find(command[offset + 1]) != std::wstring_view::npos;
    };
    std::wstring expanded;
    expanded.reserve(command.size() + quoted.size());
    for (size_t i = 0; i < command.size();) {
        // Consume only tokens in the original template; inserted filenames
        // can themselves contain percent sequences and must remain literal.
        if (command[i] == L'"' && placeholder(i + 1) &&
            i + 3 < command.size() && command[i + 3] == L'"') {
            const wchar_t token = command[i + 2];
            expanded += (token == L'W' || token == L'w') ? quoted_dir : quoted;
            if (replaced) *replaced = true;
            i += 4;
        } else if (placeholder(i)) {
            const wchar_t token = command[i + 1];
            expanded += (token == L'W' || token == L'w') ? quoted_dir : quoted;
            if (replaced) *replaced = true;
            i += 2;
        } else {
            expanded += command[i++];
        }
    }
    return expanded;
}
}

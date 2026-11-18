#pragma once
#include "image_neighbor_launch.h"
#include "shell_command.h"
#include "../common/preview_extensions.h"
#include <cwctype>

namespace pulse::ops {
struct ImageOpenResult {
    ShellItemResult shell;
    bool neighbor_attempted = false;
    bool neighbor_launched = false;
    HRESULT neighbor_error = S_OK;
    ImageNeighborLaunchStage neighbor_stage{};
    bool photos_protocol_attempted = false;
    HRESULT photos_protocol_error = S_OK;
};
inline bool IsImageOpenCandidate(const std::wstring& file, const std::wstring& verb,
    const std::wstring& arguments, bool explicit_app) {
    if (explicit_app || !verb.empty() || !arguments.empty()) return false;
    const auto dot = file.find_last_of(L'.');
    if (dot == file.npos || file.find_first_of(L"\\/:", dot) != file.npos) return false;
    auto extension = file.substr(dot);
    for (auto& c : extension) c = static_cast<wchar_t>(towlower(c));
    return preview::IsImageExtension(extension);
}
// Invoked only on the existing open worker. A neighboring query never opens
// additional files; failure falls through once to the original single-file path.
inline ImageOpenResult LaunchImageOrShellItem(const std::wstring& file, const std::wstring& verb,
    const std::wstring& arguments, const std::wstring& directory, HWND owner, bool explicit_app,
    decltype(&LaunchImageWithNeighbors) neighbor = LaunchImageWithNeighbors, const ShellCommandApi& api = {}) {
    ImageOpenResult result;
    if (IsImageOpenCandidate(file, verb, arguments, explicit_app)) {
        result.neighbor_attempted = true;
        const auto launched = neighbor(file, owner);
        result.neighbor_launched = launched.launched;
        result.neighbor_error = launched.error;
        result.neighbor_stage = launched.stage;
        result.photos_protocol_attempted = launched.photos_protocol_attempted;
        result.photos_protocol_error = launched.photos_protocol_error;
        if (launched.launched) return result;
        if (launched.error == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            result.shell.error = ERROR_CANCELLED;
            return result;
        }
    }
    result.shell = LaunchShellItem(file, verb, arguments, directory, owner, api);
    return result;
}
}

#pragma once
#include "setup_common.h"
#include <functional>
#include <string>

namespace pulse::setup {
inline bool NestedInstallDirectories(const std::wstring& first, const std::wstring& second) {
    const auto a = NormalizeDir(first), b = NormalizeDir(second);
    return !SameDirectory(a, b) && (PathInDirectories(a, {b}) || PathInDirectories(b, {a}));
}

// Call only after the new installation's files are committed. A failed shell
// migration can point different entries at either copy; keep both copies then.
inline bool CompleteDirectoryMove(const std::function<bool(std::wstring&)>& migrate,
                                  const std::function<bool(std::wstring&)>& remove_previous,
                                  const std::function<bool(std::wstring&)>& register_runtime,
                                  std::wstring& error) {
    return migrate(error) && remove_previous(error) && register_runtime(error);
}
}

// direct_reveal_targets.h — applications Pulse loads the reveal bridge into so
// their "show in folder" (explorer.exe /select) opens Pulse directly.
//
// Built-in names plus applications learned when the Explorer takeover catches
// a reveal they launched. Learned images persist per user. Every member is
// thread-safe; storage I/O never runs on the UI thread.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app::direct_reveal_targets {

bool IsBuiltIn(std::wstring_view image_name) noexcept;
// Built-in or learned; matched by image file name, case-insensitively.
bool IsTarget(std::wstring_view image_name) noexcept;
// Whether a learner may record this full image path: a local, non-system,
// non-Pulse executable. Exposed for tests.
bool Learnable(std::wstring_view image_path) noexcept;
// Records `image_path` and persists it. True only when newly learned.
bool Learn(const std::wstring& image_path) noexcept;
std::vector<std::wstring> LearnedPaths();
// Test seam: replaces the storage file (empty restores the default) and
// forgets the in-memory state.
void SetStorePathForTest(std::wstring path);

} // namespace pulse::app::direct_reveal_targets

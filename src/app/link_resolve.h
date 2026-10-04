// link_resolve.h — Resolve .lnk shortcuts to their targets (SLGP_RAWPATH: no
// disk tracking, no UI). Runs on worker threads; COM is initialized lazily
// per thread. Resolution never touches the UI thread. The reparse pass below
// resolves filesystem symlinks and junctions and needs no COM.
#pragma once
#include "../fs/fs_enum.h"
#include <functional>
#include <string>
#include <vector>

namespace pulse::app {

// Fill e.link_target* when lnk_path is a readable shortcut whose target
// currently exists. Returns false (and leaves e untouched) otherwise.
bool ResolveLink(const std::wstring& lnk_path, fs::DirEntry& e);

// Resolve every non-directory *.lnk entry in place. parent_path is the
// enumerated directory (entries with full_path set ignore it). cancel()
// returning true aborts early (entries resolved so far keep their link_target).
void ResolveLinksInPlace(const std::wstring& parent_path,
                         std::vector<fs::DirEntry>& entries,
                         const std::function<bool()>& cancel);

// Set e.reparse_kind from FSCTL_GET_REPARSE_POINT, and e.link_target* for a
// symlink or junction whose target exists. Returns false when the point cannot
// be read or its target is missing; a dangling link keeps its kind and leaves
// link_target empty.
bool ResolveReparsePoint(const std::wstring& path, fs::DirEntry& e);

// Read the reparse tag of every entry already flagged is_reparse, in place.
// Same parent_path / cancel contract as ResolveLinksInPlace.
void ResolveReparsePointsInPlace(const std::wstring& parent_path,
                                 std::vector<fs::DirEntry>& entries,
                                 const std::function<bool()>& cancel);

} // namespace pulse::app

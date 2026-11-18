// link_resolve.h — Resolve .lnk shortcuts to their targets (SLGP_RAWPATH: no
// disk tracking, no UI). Runs on worker threads; COM is initialized lazily
// per thread. Resolution never touches the UI thread.
#pragma once
#include "../fs/fs_enum.h"
#include <functional>
#include <string>
#include <vector>

namespace pulse::app {

// Fill e.link_target* when lnk_path is a readable shortcut whose target
// currently exists. Clears prior target metadata before reading, returns false
// otherwise. link_destination records a readable
// shortcut's saved path even when the target is missing; it is presentation only.
// MSI advertised shortcuts (Office, Visual Studio, ...) resolve through
// Windows Installer to the installed program; GetPath would report the product
// icon cached in %windir%\Installer, which only carries an .exe name.
bool ResolveLink(const std::wstring& lnk_path, fs::DirEntry& e);

// True for files in Windows Installer's cache (<windows_dir>\Installer\...):
// product icons and cached packages, never what a shortcut is meant to start.
bool IsInstallerCachePath(const std::wstring& path, const std::wstring& windows_dir);

// Read immediate display destinations for .lnk, .url, symlink and junction entries,
// and preserve local .lnk penetration behavior. Network targets are not probed.
// Optional metadata has a 100 ms batch budget and an independent provider quota;
// unfinished entries retain their original metadata. No recursive resolution or UI.
// parent_path is the
// enumerated directory (entries with full_path set ignore it). cancel()
// returning true aborts early (entries resolved so far keep their link_target).
void ResolveLinksInPlace(const std::wstring& parent_path,
                         std::vector<fs::DirEntry>& entries,
                         const std::function<bool()>& cancel);

} // namespace pulse::app

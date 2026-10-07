// shell_namespace_test.cpp — the shell namespace pieces that can be checked
// without a shell host: the pulse:shell: path primitives, and the answer
// routing/caching the sidebar depends on.
//
// The live enumeration itself cannot be unit-tested (it needs pulse_shell and a
// real device); shell_items_probe drives that over the pipe instead.
#include "../fs/fs_enum.h"
#include "../app/shell_namespace_cache.h"
#include "../ipc/protocol.h"
#include <cstdio>
#include <string>

static int g_pass = 0, g_fail = 0;

static void Check(bool condition, const wchar_t* name) {
    if (condition) { ++g_pass; std::wprintf(L"[PASS] %ls\n", name); }
    else { ++g_fail; std::wprintf(L"[FAIL] %ls\n", name); }
}

int wmain() {
    using pulse::fs::IsShellPath;
    using pulse::fs::IsVirtualPath;
    using pulse::fs::MakeShellPath;
    using pulse::fs::ShellParsingName;
    using pulse::ipc::ShellItem;

    // --- path primitives ---------------------------------------------------
    const std::wstring device = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::{67486EAA-ED7F-4F84-82EB-26F23F57D690}";
    const std::wstring path = MakeShellPath(device);
    Check(path == L"pulse:shell:" + device, L"shell path wraps the parsing name");
    Check(IsShellPath(path), L"a wrapped name is a shell path");
    Check(IsVirtualPath(path), L"a shell path is virtual, so it is not enumerated by Win32");
    Check(ShellParsingName(path) == device, L"the parsing name round-trips out of the path");

    Check(!IsShellPath(L"C:\\Windows"), L"a plain directory is not a shell path");
    Check(!IsShellPath(L"pulse:recycle"), L"another pulse: scheme is not a shell path");
    Check(ShellParsingName(L"C:\\Windows").empty(), L"a non-shell path has no parsing name");
    // "shell:" alone is a command-line form (shell:MyComputerFolder), so the two
    // must not be confused.
    Check(!IsShellPath(L"shell:MyComputerFolder"), L"the command-line shell: form is not ours");

    // A cloud-drive folder has a real path and is still a shell row: the path
    // only has to be navigable, it does not have to look like a device.
    const std::wstring cloud = MakeShellPath(L"C:\\Users\\x\\WPSDrive");
    Check(ShellParsingName(cloud) == L"C:\\Users\\x\\WPSDrive",
          L"a filesystem-backed shell row keeps its real path");

    // --- answer routing ----------------------------------------------------
    namespace app = pulse::app;
    std::vector<ShellItem> roots;
    for (int i = 0; i < 2; ++i) {
        ShellItem item;
        item.parsing_name = i == 0 ? device : L"C:\\Users\\x\\WPSDrive";
        item.display_name = i == 0 ? L"Phone" : L"WPS Drive";
        item.is_dir = true;
        item.has_filesystem_path = i != 0;
        item.total = i == 0 ? 0 : 2047236632576ull;
        item.free = i == 0 ? 0 : 1000000000000ull;
        roots.push_back(item);
    }

    // A successful roots answer populates the cache and reports a change.
    Check(app::DispatchShellItems(1, roots, true), L"a roots answer is accepted");
    Check(app::TakeShellRootsChanged(), L"the first roots answer reports a change");
    const auto cached = app::CachedShellRoots();
    Check(cached.size() == 2 && cached[0].parsing_name == device,
          L"the cache holds the answered rows");
    Check(!app::TakeShellRootsChanged(), L"the change flag clears once taken");

    // The same rows again must not look like a change, or the sidebar would
    // rebuild on every tick.
    app::DispatchShellItems(2, roots, true);
    Check(!app::TakeShellRootsChanged(), L"an identical answer reports no change");

    // A failed refresh keeps the last good rows: This PC must not go empty
    // because one request was not answered.
    app::DispatchShellItems(3, {}, false);
    Check(app::CachedShellRoots().size() == 2, L"a failed refresh keeps the previous rows");

    // Removing a device is a real change.
    app::DispatchShellItems(4, { roots[0] }, true);
    Check(app::TakeShellRootsChanged(), L"a shorter answer reports a change");

    std::wprintf(L"\n== shell namespace test: %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// cloud_roots_test.cpp - navigation-pane and sync-root cloud folders, read from
// a private HKCU fixture (HKCU\Software\PulseTest\cloud_roots_<pid>). Never
// writes Explorer's own keys; the fixture tree and temp folders are removed.
#include "../app/cloud_roots.h"
#include "../fs/cloud_state.h"
#include "../app/cloud_sync_probe.h"
#include "../fs/cloud_pin.h"
#include "../ui/cloud_swap.h"
#include "../fs/cloud_hydrate.h"
#include "../app/cloud_download.h"
#include <algorithm>
#include <atomic>
#include <vector>
#include <objbase.h>
#include <unordered_map>

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace pulse::app;

namespace {
bool g_passed = true;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    g_passed = g_passed && ok;
}

void SetString(const std::wstring& key, const wchar_t* name, const std::wstring& value,
               DWORD type = REG_SZ) {
    HKEY h = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr);
    if (!h) return;
    RegSetValueExW(h, name, 0, type, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
}

void SetDword(const std::wstring& key, const wchar_t* name, DWORD value) {
    HKEY h = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr);
    if (!h) return;
    RegSetValueExW(h, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(h);
}

void MakeKey(const std::wstring& key) {
    HKEY h = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr);
    if (h) RegCloseKey(h);
}

const CloudRoot* Find(const std::vector<CloudRoot>& roots, const std::wstring& id) {
    for (const auto& r : roots) if (_wcsicmp(r.id.c_str(), id.c_str()) == 0) return &r;
    return nullptr;
}
} // namespace

int main() {
    const std::wstring sid = L"S-1-5-21-pulse-test";
    const std::wstring prefix = L"Software\\PulseTest\\cloud_roots_" + std::to_wstring(GetCurrentProcessId());
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring base = NormalizeCloudRootPath(std::wstring(temp) + L"pulse_cloud_roots_" +
                                                     std::to_wstring(GetCurrentProcessId()));
    const std::wstring wps = base + L"\\WPSDrive";
    const std::wstring baidu = base + L"\\BaiduSyncdisk";
    const std::wstring od = base + L"\\OneDrive";
    const std::wstring gone = base + L"\\Gone";
    CreateDirectoryW(base.c_str(), nullptr);
    for (const auto* dir : {&wps, &baidu, &od}) CreateDirectoryW(dir->c_str(), nullptr);

    const std::wstring ns = prefix + L"\\NameSpace\\";
    const std::wstring cls = prefix + L"\\CLSID\\";
    // A: delegate folder, TargetFolderPath as REG_EXPAND_SZ, name in the default value.
    const std::wstring a = L"{7AE6DE87-C956-4B40-9C89-3D166C9841D3}";
    MakeKey(ns + a);
    SetString(cls + a, nullptr, L"WPS\x7F51\x76D8");
    SetDword(cls + a, L"System.IsPinnedToNameSpaceTree", 1);
    const std::wstring wps_expand = L"%PULSE_CLOUD_ROOTS_BASE%\\WPSDrive\\";
    SetEnvironmentVariableW(L"PULSE_CLOUD_ROOTS_BASE", base.c_str());
    SetString(cls + a + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", wps_expand, REG_EXPAND_SZ);
    // B: FolderShortcut spelling ("Target"), LocalizedString wins over the default value.
    const std::wstring b = L"{18701C81-34AB-498E-8C7F-5D26B9C36732}";
    MakeKey(ns + b);
    SetString(cls + b, nullptr, L"ignored default");
    SetDword(cls + b, L"System.IsPinnedToNameSpaceTree", 1);
    SetDword(cls + b, L"SortOrderIndex", 0x50);
    SetDword(cls + a, L"SortOrderIndex", 0x40);
    SetString(cls + b, L"LocalizedString", L"\x767E\x5EA6\x7F51\x76D8\x540C\x6B65\x7A7A\x95F4");
    SetString(cls + b + L"\\Instance\\InitPropertyBag", L"Target", baidu);
    // C: switched off in the tree.
    const std::wstring c = L"{00000000-0000-0000-0000-00000000000C}";
    MakeKey(ns + c);
    SetDword(cls + c, L"System.IsPinnedToNameSpaceTree", 0);
    SetString(cls + c + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", od);
    // D: a shell extension without a folder target (Libraries, Home, ...).
    const std::wstring d = L"{00000000-0000-0000-0000-00000000000D}";
    MakeKey(ns + d);
    SetString(cls + d, nullptr, L"Shell extension");
    // E: hidden by the NonEnum policy.
    const std::wstring e = L"{00000000-0000-0000-0000-00000000000E}";
    MakeKey(ns + e);
    SetDword(cls + e, L"System.IsPinnedToNameSpaceTree", 1);
    SetString(cls + e + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", od);
    SetDword(prefix + L"\\NonEnum", e.c_str(), 1);
    // F: listed in NameSpace, but no CLSID registration at all.
    MakeKey(ns + L"{00000000-0000-0000-0000-00000000000F}");
    // H: Windows' own entries - a folder target but no IsPinnedToNameSpaceTree.
    const std::wstring h = L"{00000000-0000-0000-0000-000000000008}";
    MakeKey(ns + h);
    SetString(cls + h, nullptr, L"Pictures");
    SetString(cls + h + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", od);
    // K: known-folder target (Documents, "This Device") is never followed.
    const std::wstring k = L"{00000000-0000-0000-0000-00000000000B}";
    MakeKey(ns + k);
    SetDword(cls + k, L"System.IsPinnedToNameSpaceTree", 1);
    SetString(cls + k + L"\\Instance\\InitPropertyBag", L"TargetKnownFolder",
              L"{FDD39AD0-238F-46AF-ADB4-6C85480369C7}");
    // G: points at a folder that no longer exists.
    const std::wstring g = L"{00000000-0000-0000-0000-000000000006}";
    MakeKey(ns + g);
    SetString(cls + g, nullptr, L"Old drive");
    SetDword(cls + g, L"System.IsPinnedToNameSpaceTree", 1);
    SetDword(cls + g, L"SortOrderIndex", 0x60);
    SetString(cls + g + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", gone);

    // This PC program icons (MyComputer\NameSpace).
    const std::wstring pc = prefix + L"\\MyComputer\\";
    const std::wstring l_baidu = L"{679F137C-3162-45da-BE3C-2F9C3D093F64}";
    MakeKey(pc + l_baidu);
    SetString(cls + l_baidu, nullptr, L"\x767E\x5EA6\x7F51\x76D8");
    SetString(cls + l_baidu + L"\\InprocServer32", nullptr, L"C:\\Program Files\\BaiduNetdisk\\YunShellExt64.dll");
    const std::wstring l_wps = L"{5FCD4425-CA3A-48F4-A57C-B8A75C32ACB1}";
    MakeKey(pc + l_wps);
    SetString(cls + l_wps, nullptr, L"ignored default");
    SetString(cls + l_wps, L"LocalizedString", L"WPS\x7F51\x76D8");
    SetString(cls + l_wps + L"\\InprocServer32", nullptr, L"\"%ProgramFiles%\\WPS\\wpsext.dll\"", REG_EXPAND_SZ);
    const std::wstring l_sys = L"{00000000-0000-0000-0000-0000000000A1}";  // Windows' own (Documents)
    MakeKey(pc + l_sys);
    SetString(cls + l_sys, nullptr, L"Documents");
    SetString(cls + l_sys + L"\\InprocServer32", nullptr, L"%SystemRoot%\\system32\\shell32.dll", REG_EXPAND_SZ);
    SetString(cls + l_sys + L"\\Instance\\InitPropertyBag", L"TargetKnownFolder", L"{FDD39AD0-238F-46AF-ADB4-6C85480369C7}");
    const std::wstring l_bare = L"{00000000-0000-0000-0000-0000000000A2}";  // bare system DLL name
    MakeKey(pc + l_bare);
    SetString(cls + l_bare, nullptr, L"3D Objects");
    SetString(cls + l_bare + L"\\InprocServer32", nullptr, L"shell32.dll");
    const std::wstring l_dir = L"{00000000-0000-0000-0000-0000000000A3}";  // delegate folder
    MakeKey(pc + l_dir);
    SetString(cls + l_dir, nullptr, L"Sync space");
    SetString(cls + l_dir + L"\\InprocServer32", nullptr, L"%SystemRoot%\\system32\\shell32.dll", REG_EXPAND_SZ);
    SetString(cls + l_dir + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", baidu);
    const std::wstring l_hidden = L"{00000000-0000-0000-0000-0000000000A4}";
    MakeKey(pc + l_hidden);
    SetString(cls + l_hidden, nullptr, L"Hidden app");
    SetString(cls + l_hidden + L"\\InprocServer32", nullptr, L"D:\\Apps\\hidden.dll");
    SetDword(prefix + L"\\NonEnum", l_hidden.c_str(), 1);
    const std::wstring l_unreg = L"{00000000-0000-0000-0000-0000000000A5}";  // no CLSID key
    MakeKey(pc + l_unreg);
    const std::wstring l_noname = L"{00000000-0000-0000-0000-0000000000A6}";
    MakeKey(pc + l_noname);
    SetString(cls + l_noname + L"\\InprocServer32", nullptr, L"D:\\Apps\\noname.dll");
    const std::wstring l_gone = L"{00000000-0000-0000-0000-0000000000A7}";  // folder gone, system DLL
    MakeKey(pc + l_gone);
    SetString(cls + l_gone, nullptr, L"Gone folder");
    SetString(cls + l_gone + L"\\InprocServer32", nullptr, L"%SystemRoot%\\system32\\shell32.dll", REG_EXPAND_SZ);
    SetString(cls + l_gone + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", gone);
    MakeKey(pc + L"not-a-clsid");

    const std::wstring srm = prefix + L"\\SyncRootManager\\";
    const std::wstring od_id = L"OneDrive!" + sid + L"!Personal";
    SetString(srm + od_id + L"\\UserSyncRoots", sid.c_str(), od + L"\\");
    const std::wstring other_id = L"Dropbox!S-1-5-21-someone-else!1";
    SetString(srm + other_id + L"\\UserSyncRoots", L"S-1-5-21-someone-else", base);
    const std::wstring dup_id = L"WPSCloud!" + sid + L"!1";
    SetString(srm + dup_id, L"DisplayNameResource", L"WPS Cloud");
    SetString(srm + dup_id + L"\\UserSyncRoots", sid.c_str(), wps + L"\\");
    MakeKey(srm + L"Empty!" + sid);

    const CloudRegistry registry = CloudRegistry::Fixture(prefix, sid);
    {
        const auto launchers = ReadThisPcLaunchers(registry);
        const auto find = [&](const std::wstring& id) -> const ThisPcLauncher* {
            for (const auto& l : launchers) if (_wcsicmp(l.clsid.c_str(), id.c_str()) == 0) return &l;
            return nullptr;
        };
        const ThisPcLauncher* lb = find(l_baidu);
        const ThisPcLauncher* lw = find(l_wps);
        const ThisPcLauncher* ld = find(l_dir);
        Check(lb && lb->label == L"\x767E\x5EA6\x7F51\x76D8" && lb->target.empty() && lb->brand == 2,
              "launchers: a program's DLL makes a launcher; default value names it; Baidu tint");
        Check(lw && lw->label == L"WPS\x7F51\x76D8" && lw->brand == 3,
              "launchers: quoted REG_EXPAND_SZ server, LocalizedString wins; WPS tint");
        Check(ld && ld->target == baidu && ld->brand == 1, "launchers: a delegate folder keeps its target");
        Check(!find(l_sys) && !find(l_bare), "launchers: Windows' own entries are left out");
        Check(!find(l_hidden), "launchers: NonEnum hides one");
        Check(!find(l_unreg) && !find(l_noname) && !find(l_gone),
              "launchers: unregistered, unnamed or gone-folder entries are left out");
        Check(launchers.size() == 3, "launchers: exactly three");
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", L"x");
        SetEnvironmentVariableW(L"PULSE_TEST_CLOUD_REG", prefix.c_str());
        const auto current = CurrentThisPcLaunchers();
        SetEnvironmentVariableW(L"PULSE_TEST_CLOUD_REG", nullptr);
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr);
        Check(current.size() == 3, "launchers: a test instance reads its fixture");
    }
    const auto pane = ReadNavigationPaneCloudRoots(registry);
    const CloudRoot* ra = Find(pane, a);
    Check(ra && ra->path == wps && ra->label == L"WPS\x7F51\x76D8",
          "TargetFolderPath (REG_EXPAND_SZ) resolves, default value names the row");
    const CloudRoot* rb = Find(pane, b);
    Check(rb && rb->path == baidu && rb->label == L"\x767E\x5EA6\x7F51\x76D8\x540C\x6B65\x7A7A\x95F4",
          "FolderShortcut Target and LocalizedString are honoured");
    Check(!Find(pane, c), "IsPinnedToNameSpaceTree = 0 hides the entry");
    Check(!Find(pane, d), "shell extensions without a folder target are skipped");
    Check(!Find(pane, e), "NonEnum policy hides the entry");
    Check(!Find(pane, L"{00000000-0000-0000-0000-00000000000F}"), "unregistered CLSID is skipped");
    Check(Find(pane, g) != nullptr, "reader itself does not touch the file system");
    Check(!Find(pane, h), "entries without IsPinnedToNameSpaceTree (Windows' own) are skipped");
    Check(!Find(pane, k), "TargetKnownFolder entries are skipped");
    Check(pane.size() == 3 && pane[0].id == a && pane[1].id == b,
          "navigation pane order follows SortOrderIndex");

    const auto sync = ReadSyncRootCloudRoots(registry);
    const CloudRoot* rod = Find(sync, od_id);
    Check(rod && rod->path == od && rod->label == L"OneDrive",
          "sync root for this SID, provider prefix as fallback label");
    Check(!Find(sync, other_id), "another user's sync root is ignored");
    Check(!Find(sync, L"Empty!" + sid), "sync root without UserSyncRoots is ignored");
    const CloudRoot* rdup = Find(sync, dup_id);
    Check(rdup && rdup->label == L"WPS Cloud", "DisplayNameResource names the sync root");
    Check(ReadSyncRootCloudRoots(CloudRegistry::Fixture(prefix, L"")).empty(),
          "no SID, no sync roots");

    std::vector<CloudRoot> merged;
    merged.push_back({od, L"SS - Personal", {}, CloudRootSource::Seed}); // OneDrive account row
    const auto is_dir = [](const std::wstring& p) {
        const DWORD attrs = GetFileAttributesW(p.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
    };
    MergeCloudRoots(merged, pane, is_dir);
    MergeCloudRoots(merged, sync, is_dir);
    Check(merged.size() == 3 && merged[0].source == CloudRootSource::Seed &&
              merged[1].id == a && merged[2].id == b,
          "merge keeps order, drops duplicates (case, trailing slash) and missing folders");

    Check(NormalizeCloudRootPath(L"C:\\Foo\\\\") == L"C:\\Foo", "trailing separators trimmed");
    Check(NormalizeCloudRootPath(L"D:\\") == L"D:\\", "drive root keeps its separator");
    Check(NormalizeCloudRootPath(L"D:/Baidu/Sync") == L"D:\\Baidu\\Sync", "forward slashes normalized");
    Check(SameCloudRootPath(L"c:\\users\\user\\WPSDrive\\", L"C:\\Users\\User\\wpsdrive"),
          "path comparison is case-insensitive");
    Check(NormalizeCloudRootPath(L"").empty(), "empty path stays empty");
    Check(SameCloudRootPath(L"\\\\?\\C:\\Users\\User\\OneDrive", L"C:\\Users\\User\\OneDrive\\"),
          "extended account path matches ordinary Shell root");
    Check(SameCloudRootPath(L"\\\\?\\UNC\\server\\share\\Cloud", L"\\\\server\\share\\cloud\\"),
          "extended UNC path matches ordinary UNC root");
    std::vector<CloudRoot> account_roots{{L"\\\\?\\C:\\Users\\User\\OneDrive", L"User - Personal", {}, CloudRootSource::Seed}};
    MergeCloudRoots(account_roots, {{L"C:\\Users\\User\\OneDrive", L"OneDrive", {}, CloudRootSource::SyncRoot}}, {});
    Check(account_roots.size() == 1 && account_roots[0].label == L"User - Personal",
          "account and sync registration merge preserving account label");
    MergeCloudRoots(account_roots, {{L"C:\\Users\\User\\OneDrive - Work", L"Work", {}, CloudRootSource::SyncRoot}}, {});
    Check(account_roots.size() == 2, "different account directories remain separate");

    Check(CurrentUserSidString().rfind(L"S-1-", 0) == 0, "process SID string");
    Check(CloudRootColor({L"", L"OneDrive - Personal", L"", CloudRootSource::Seed}) == 0x2B88D8 &&
              CloudRootColor({L"", L"WPS\x7F51\x76D8", a, CloudRootSource::NavigationPane}) == 0x2F6FD6 &&
              CloudRootColor({L"", L"\x767E\x5EA6\x7F51\x76D8", b, CloudRootSource::NavigationPane}) == 0x2F8CF0 &&
              CloudRootColor({L"", L"Other", L"x", CloudRootSource::SyncRoot}) == 0x60A5FA,
          "brand tints");

    using pulse::fs::CloudState;
    using pulse::fs::CloudStateFromAttrs;
    Check(CloudStateFromAttrs(FILE_ATTRIBUTE_ARCHIVE) == CloudState::Local &&
              CloudStateFromAttrs(pulse::fs::kAttrUnpinned) == CloudState::Local &&
              CloudStateFromAttrs(pulse::fs::kAttrRecallOnDataAccess) == CloudState::Online &&
              CloudStateFromAttrs(FILE_ATTRIBUTE_DIRECTORY | pulse::fs::kAttrRecallOnOpen) == CloudState::Online &&
              CloudStateFromAttrs(FILE_ATTRIBUTE_OFFLINE) == CloudState::Online &&
              CloudStateFromAttrs(pulse::fs::kAttrPinned) == CloudState::Pinned &&
              CloudStateFromAttrs(pulse::fs::kAttrPinned | pulse::fs::kAttrRecallOnDataAccess) == CloudState::Pinned,
          "cloud state from attributes");
    Check(!IsUnderCloudFilesRoot(L"C:\\Users\\User\\OneDrive"), "nothing published yet");
    PublishCloudFilesRoots({L"C:\\Users\\User\\OneDrive\\", L"", L"D:\\"});
    Check(IsUnderCloudFilesRoot(L"C:\\Users\\User\\OneDrive") &&
              IsUnderCloudFilesRoot(L"c:\\users\\user\\onedrive\\Docs\\a") &&
              !IsUnderCloudFilesRoot(L"C:\\Users\\User\\OneDriveOld") &&
              !IsUnderCloudFilesRoot(L"C:\\Users\\User") && IsUnderCloudFilesRoot(L"D:\\x") &&
              IsUnderCloudFilesRoot(L"\\\\?\\C:\\Users\\User\\OneDrive\\Docs") &&
              !IsUnderCloudFilesRoot(L"\\\\?\\C:\\Users\\User\\OneDriveOld"),
          "cloud files root prefix matching respects folder boundaries");
    PublishCloudFilesRoots({L"\\\\nas\\share\\Sync"});
    Check(IsUnderCloudFilesRoot(L"\\\\?\\UNC\\nas\\share\\Sync\\a") &&
              IsUnderCloudFilesRoot(L"\\\\nas\\share\\Sync"),
          "long-path and UNC prefixes match the same root");
    PublishCloudFilesRoots({});
    Check(!IsUnderCloudFilesRoot(L"D:\\x"), "republishing replaces the set");

    using pulse::fs::CloudSync;
    using pulse::fs::CloudSyncFromProviderState;
    Check(CloudSyncFromProviderState(0) == CloudSync::Idle && CloudSyncFromProviderState(2) == CloudSync::Idle &&
              CloudSyncFromProviderState(3) == CloudSync::Idle && CloudSyncFromProviderState(9) == CloudSync::Idle &&
              CloudSyncFromProviderState(4) == CloudSync::Syncing && CloudSyncFromProviderState(5) == CloudSync::Syncing &&
              CloudSyncFromProviderState(6) == CloudSync::Syncing && CloudSyncFromProviderState(10) == CloudSync::Syncing &&
              CloudSyncFromProviderState(7) == CloudSync::Error && CloudSyncFromProviderState(8) == CloudSync::Error,
          "storage provider states map to idle / syncing / error");
    {
        using pulse::app::CloudSyncProbe;
        std::unordered_map<std::wstring, uint32_t> fake{{L"a", 6}, {L"b", 7}, {L"c", 2}};
        int calls = 0;
        CloudSyncProbe probe([&](const std::wstring& p) {
            ++calls;
            const auto it = fake.find(p);
            return it == fake.end() ? 0u : it->second;
        }, CloudSyncProbe::Mode::Manual);
        const ULONGLONG t0 = GetTickCount64();
        Check(probe.Lookup(L"a", 1, t0) == CloudSync::Idle && probe.Lookup(L"b", 1, t0) == CloudSync::Idle &&
                  probe.Lookup(L"c", 1, t0) == CloudSync::Idle && probe.QueuedForTest() == 3,
              "probe: unknown rows answer idle and are queued");
        probe.Lookup(L"a", 1, t0);
        Check(probe.QueuedForTest() == 3 && probe.Lookup(L"", 0, t0) == CloudSync::Idle && probe.QueuedForTest() == 3,
              "probe: a queued row is not queued twice, empty paths are ignored");
        const uint64_t r0 = probe.Revision();
        Check(probe.RunPendingForTest() == 3 && calls == 3, "probe: each pending row is read once");
        Check(probe.Lookup(L"a", 1, t0) == CloudSync::Syncing && probe.Lookup(L"b", 1, t0) == CloudSync::Error &&
                  probe.Lookup(L"c", 1, t0) == CloudSync::Idle && probe.Revision() != r0 && probe.QueuedForTest() == 0,
              "probe: answers are cached and move the revision");
        const ULONGLONG later = GetTickCount64() + CloudSyncProbe::kSyncingRecheckMs + 10;
        probe.Lookup(L"a", 1, later);
        probe.Lookup(L"b", 1, later);
        probe.Lookup(L"c", 1, later);
        Check(probe.QueuedForTest() == 1, "probe: only syncing rows are re-read on a timer");
        fake[L"a"] = 2;
        const uint64_t r1 = probe.Revision();
        probe.RunPendingForTest();
        Check(probe.Lookup(L"a", 1, later) == CloudSync::Idle && probe.Revision() != r1,
              "probe: a finished transfer settles and repaints");
        probe.Lookup(L"c", 2, later);
        Check(probe.QueuedForTest() == 1, "probe: a modified row is read again");
        const uint64_t r2 = probe.Revision();
        probe.RunPendingForTest();
        Check(probe.Revision() == r2 && calls == 5, "probe: an unchanged answer does not repaint");
    }
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        wchar_t windir[MAX_PATH]{};
        GetWindowsDirectoryW(windir, MAX_PATH);
        Check(CloudSyncFromProviderState(pulse::app::QueryStorageProviderState(windir)) == CloudSync::Idle &&
                  pulse::app::QueryStorageProviderState(L"Z:\\no\\such\\file") == 0,
              "real query: a local folder is idle, a missing path reads as none");
        if (SUCCEEDED(com)) CoUninitialize();
    }

    {
        // 始终保留 / 释放空间: attribute arithmetic, then a real tree on NTFS
        // (PINNED / UNPINNED are plain settable attributes without a provider).
        using pulse::fs::CloudPinAction;
        using pulse::fs::CloudPinAttrs;
        using pulse::fs::CloudPinChanges;
        constexpr DWORD P = pulse::fs::kAttrPinned, U = pulse::fs::kAttrUnpinned;
        Check(CloudPinAttrs(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_ARCHIVE | U, CloudPinAction::Keep) ==
                  (FILE_ATTRIBUTE_ARCHIVE | P),
              "pin: keep swaps UNPINNED for PINNED and drops DIRECTORY");
        Check(CloudPinAttrs(pulse::fs::kAttrRecallOnDataAccess | FILE_ATTRIBUTE_REPARSE_POINT |
                                FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_READONLY | P,
                            CloudPinAction::Free) == (FILE_ATTRIBUTE_READONLY | U),
              "pin: free keeps READONLY, masks recall / reparse / sparse");
        Check(!CloudPinChanges(P, CloudPinAction::Keep) && CloudPinChanges(P | U, CloudPinAction::Keep) &&
                  CloudPinChanges(0, CloudPinAction::Free) && !CloudPinChanges(U, CloudPinAction::Free),
              "pin: only real changes are written");
        Check(pulse::fs::IsCloudReparseTag(0x9000001A) && pulse::fs::IsCloudReparseTag(0x9000F01A) &&
                  !pulse::fs::IsCloudReparseTag(0xA0000003) && !pulse::fs::IsCloudReparseTag(0xA000000C),
              "pin: cloud reparse tags recognised, junction / symlink not");

        wchar_t tmp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring top = std::wstring(tmp) + L"pulse_cloud_pin_" + std::to_wstring(GetCurrentProcessId());
        const std::wstring root = top + L"\\root", outside = top + L"\\outside";
        const auto touch = [](const std::wstring& path) {
            const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD n = 0;
                WriteFile(h, "pulse", 5, &n, nullptr);
                CloseHandle(h);
            }
        };
        for (const auto& folder : {top, root, outside, root + L"\\sub", root + L"\\online"}) CreateDirectoryW(folder.c_str(), nullptr);
        touch(root + L"\\a.txt");
        touch(root + L"\\sub\\b.txt");
        touch(root + L"\\online\\c.txt");
        touch(outside + L"\\x.txt");
        SetFileAttributesW((root + L"\\a.txt").c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN);
        const std::wstring junction = root + L"\\link";
        const std::wstring mk = L"cmd /c mklink /J \"" + junction + L"\" \"" + outside + L"\" >nul 2>nul";
        _wsystem(mk.c_str());
        const bool have_junction = (GetFileAttributesW(junction.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
                                   GetFileAttributesW(junction.c_str()) != INVALID_FILE_ATTRIBUTES;
        const auto attrs = [](const std::wstring& path) { return GetFileAttributesW(path.c_str()); };
        const DWORD link_before = attrs(junction);

        auto r = pulse::fs::ApplyCloudPin({root}, CloudPinAction::Keep);
        bool pinned = true;
        for (const auto& f : {root, root + L"\\a.txt", root + L"\\sub", root + L"\\sub\\b.txt", root + L"\\online",
                              root + L"\\online\\c.txt"})
            pinned = pinned && (attrs(f) & P) && !(attrs(f) & U);
        Check(pinned && r.changed == 6 && r.failed == 0 && !r.cancelled, "pin: keep pins the folder and all below it");
        Check((attrs(root + L"\\a.txt") & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN)) ==
                  (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN),
              "pin: other attributes survive");
        Check(!(attrs(outside + L"\\x.txt") & P) && (!have_junction || attrs(junction) == link_before),
              have_junction ? "pin: a junction is neither touched nor entered" : "pin: junction case skipped (mklink failed)");

        SetFileAttributesW((root + L"\\online").c_str(), FILE_ATTRIBUTE_OFFLINE | P); // an online-only folder
        r = pulse::fs::ApplyCloudPin({root}, CloudPinAction::Free);
        bool freed = true;
        for (const auto& f : {root, root + L"\\a.txt", root + L"\\sub", root + L"\\sub\\b.txt", root + L"\\online"})
            freed = freed && (attrs(f) & U) && !(attrs(f) & P);
        Check(freed && r.changed == 5 && r.failed == 0, "pin: free unpins the folder and all below it");
        Check((attrs(root + L"\\online\\c.txt") & P) && (attrs(root + L"\\online") & FILE_ATTRIBUTE_OFFLINE),
              "pin: free does not enter an online-only folder");
        Check(attrs(root + L"\\a.txt") != INVALID_FILE_ATTRIBUTES && attrs(root + L"\\sub\\b.txt") != INVALID_FILE_ATTRIBUTES,
              "pin: no file is ever removed");
        r = pulse::fs::ApplyCloudPin({root + L"\\sub\\"}, CloudPinAction::Free);
        Check(r.changed == 0 && r.visited == 2, "pin: a second free changes nothing (trailing slash accepted)");
        std::atomic<bool> stop{true};
        r = pulse::fs::ApplyCloudPin({root}, CloudPinAction::Keep, &stop);
        Check(r.cancelled && r.changed == 0, "pin: a cancelled run writes nothing");
        r = pulse::fs::ApplyCloudPin({root + L"\\gone.txt"}, CloudPinAction::Keep);
        Check(r.failed == 1 && r.changed == 0, "pin: a missing item counts as failed");

        if (have_junction) RemoveDirectoryW(junction.c_str());
        for (const auto& f : {root + L"\\a.txt", root + L"\\sub\\b.txt", root + L"\\online\\c.txt", outside + L"\\x.txt"}) {
            SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(f.c_str());
        }
        for (const auto& folder : {root + L"\\sub", root + L"\\online", root, outside, top}) {
            SetFileAttributesW(folder.c_str(), FILE_ATTRIBUTE_NORMAL);
            RemoveDirectoryW(folder.c_str());
        }
        Check(attrs(top) == INVALID_FILE_ATTRIBUTES && attrs(outside + L"\\x.txt") == INVALID_FILE_ATTRIBUTES,
              "pin fixture removed");
    }

    {
        // Status icon swap (design .cbadge.swap / .st.swap: newf .5s cubic-bezier(.3,1.5,.5,1)).
        using pulse::ui::CubicBezierAt;
        Check(std::fabs(CubicBezierAt(0.3f, 1.5f, 0.5f, 1.0f, 0.0f)) < 1e-3f &&
                  std::fabs(CubicBezierAt(0.3f, 1.5f, 0.5f, 1.0f, 1.0f) - 1.0f) < 1e-3f &&
                  std::fabs(CubicBezierAt(0.25f, 0.25f, 0.75f, 0.75f, 0.3f) - 0.3f) < 1e-3f,
              "swap: bezier ends at 0 and 1, the linear curve is linear");
        float peak = 0.0f;
        for (int i = 1; i < 100; ++i) peak = std::max(peak, CubicBezierAt(0.3f, 1.5f, 0.5f, 1.0f, i / 100.0f));
        Check(peak > 1.05f && peak < 1.3f, "swap: newf overshoots like the CSS curve");
        pulse::ui::CloudSwapTracker swaps;
        auto pose = swaps.Pose(7, L'\xE930', 1000, true);
        Check(!pose.running && pose.scale == 1.0f, "swap: first sighting does not animate");
        pose = swaps.Pose(7, L'\xE930', 1100, true);
        Check(!pose.running, "swap: an unchanged icon stays still");
        pose = swaps.Pose(7, L'\xE895', 2000, true);
        Check(pose.running && std::fabs(pose.scale - 0.6f) < 1e-3f && pose.alpha < 1e-3f,
              "swap: a changed icon starts at 0.6, transparent");
        float max_scale = 0.0f;
        for (uint64_t t = 2000; t < 2500; t += 10) max_scale = std::max(max_scale, swaps.Pose(7, L'\xE895', t, true).scale);
        Check(max_scale > 1.0f, "swap: the pop passes full size before settling");
        pose = swaps.Pose(7, L'\xE895', 2500, true);
        Check(!pose.running && pose.scale == 1.0f && pose.alpha == 1.0f, "swap: settled after 500 ms");
        pose = swaps.Pose(7, L'\xEC61', 3000, false);
        Check(!pose.running && pose.scale == 1.0f, "swap: animations off settles at once");
        pose = swaps.Pose(8, L'\xE753', 3000, true);
        Check(!pose.running && swaps.Size() == 2, "swap: rows are tracked apart");
    }

    {
        // C4b 下载并预览: provider names for the well.
        PublishCloudRootLabels({{L"C:\\Users\\User\\WPSDrive", L"WPS\u7f51\u76d8"},
                                {L"C:\\Users\\User\\WPSDrive\\Team", L"Team"},
                                {L"D:\\", L"Disk"}});
        Check(CloudRootLabelFor(L"c:\\users\\user\\wpsdrive\\a.docx") == L"WPS\u7f51\u76d8", "label: file under a root");
        Check(CloudRootLabelFor(L"C:\\Users\\User\\WPSDrive\\Team\\x.txt") == L"Team", "label: the deepest root wins");
        Check(CloudRootLabelFor(L"C:\\Users\\User\\WPSDriveOld\\x").empty() && CloudRootLabelFor(L"C:\\Users").empty(),
              "label: name prefixes and parents are not inside");
        Check(CloudRootLabelFor(L"\\\\?\\C:\\Users\\User\\WPSDrive\\a") == L"WPS\u7f51\u76d8" &&
                  CloudRootLabelFor(L"D:\\x") == L"Disk",
              "label: long-path prefix and drive roots");
    }
    {
        // C4b: the download is a plain front-to-back read; nothing else.
        wchar_t tmp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring dir = std::wstring(tmp) + L"pulse_hydrate_" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring file = dir + L"\\big.bin", second = dir + L"\\second.bin";
        const uint64_t bytes = (uint64_t{5} << 20) + 12345;
        auto write = [](const std::wstring& path, uint64_t size) {
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            std::vector<char> chunk(size_t{1} << 20, 'x');
            for (uint64_t left = size; left && h != INVALID_HANDLE_VALUE;) {
                const DWORD n = static_cast<DWORD>(std::min<uint64_t>(left, chunk.size()));
                DWORD written = 0;
                if (!WriteFile(h, chunk.data(), n, &written, nullptr) || !written) break;
                left -= written;
            }
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        };
        write(file, bytes);
        write(second, uint64_t{3} << 20);
        SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_OFFLINE);
        WIN32_FILE_ATTRIBUTE_DATA before{}, after{};
        GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &before);
        std::atomic<bool> cancel{false};
        std::vector<uint64_t> seen;
        auto result = pulse::fs::HydrateByReading(file, cancel, [&](uint64_t b) { seen.push_back(b); });
        Check(result == pulse::fs::HydrateResult::Done && seen.size() == 6 &&
                  std::is_sorted(seen.begin(), seen.end()) && seen.back() == bytes,
              "hydrate: reads front to back in 1 MB chunks");
        GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &after);
        Check((after.dwFileAttributes & FILE_ATTRIBUTE_OFFLINE) &&
                  CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0,
              "hydrate: attributes and write time untouched (the provider changes them, not Pulse)");
        cancel = true;
        seen.clear();
        Check(pulse::fs::HydrateByReading(file, cancel, [&](uint64_t b) { seen.push_back(b); }) ==
                  pulse::fs::HydrateResult::Cancelled && seen.empty(),
              "hydrate: a cancelled read stops before the first chunk");
        cancel = false;
        DWORD error = 0;
        Check(pulse::fs::HydrateByReading(dir, cancel, nullptr, &error) == pulse::fs::HydrateResult::Failed && error,
              "hydrate: a folder is refused (its listing is never fetched)");
        Check(pulse::fs::HydrateByReading(dir + L"\\missing.bin", cancel, nullptr) == pulse::fs::HydrateResult::Failed,
              "hydrate: a missing file fails");

        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PulseCloudDownloadTest";
        RegisterClassW(&wc);
        HWND sink = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        constexpr UINT kMsg = WM_APP + 98;
        std::unordered_map<LPARAM, WPARAM> ends;
        bool progress = false;
        auto pump = [&](size_t want, DWORD ms) {
            const ULONGLONG end = GetTickCount64() + ms;
            MSG m;
            do {
                while (PeekMessageW(&m, sink, kMsg, kMsg, PM_REMOVE)) {
                    if (m.wParam == kCloudDownloadProgress) progress = true;
                    else ends[m.lParam] = m.wParam;
                }
                if (ends.size() >= want) break;
                Sleep(5);
            } while (GetTickCount64() < end);
        };
        Check(CloudDownloadPercent(file) == -1, "download: idle before start");
        // Isolated-instance hooks: the provider stand-in clears OFFLINE once done.
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", dir.c_str());
        SetEnvironmentVariableW(L"PULSE_TEST_HYDRATE_CLEAR", L"1");
        Check(sink && StartCloudDownload(file, bytes, sink, kMsg), "download: starts");
        pump(1, 15000);
        const LPARAM first = ends.empty() ? 0 : ends.begin()->first;
        Check(ends.size() == 1 && ends.begin()->second == kCloudDownloadDone, "download: done event");
        Check(CloudDownloadPercent(file) == -1 && CloudDownloadPathFor(first) == file,
              "download: a finished job maps back to its file");
        Check(!(GetFileAttributesW(file.c_str()) & FILE_ATTRIBUTE_OFFLINE), "download: provider stand-in cleared OFFLINE");
        SetEnvironmentVariableW(L"PULSE_TEST_HYDRATE_CLEAR", nullptr);
        SetEnvironmentVariableW(L"PULSE_TEST_HYDRATE_DELAY_MS", L"200");
        ends.clear();
        progress = false;
        StartCloudDownload(file, bytes, sink, kMsg);
        Sleep(650);
        pump(0, 50);
        const int midway = CloudDownloadPercent(file);
        Check(midway > 0 && midway < 100 && progress, "download: percent and progress events while reading");
        Check(StartCloudDownload(second, uint64_t{3} << 20, sink, kMsg) && CloudDownloadPercent(file) == -1 &&
                  CloudDownloadPercent(second) >= 0,
              "download: a new one replaces the running one");
        pump(1, 5000);
        Check(ends.size() == 1 && ends.begin()->second == kCloudDownloadCancelled, "download: the replaced one ends cancelled");
        CancelCloudDownloads();
        pump(2, 5000);
        size_t cancelled = 0;
        for (const auto& [job, event] : ends) cancelled += event == kCloudDownloadCancelled;
        Check(ends.size() == 2 && cancelled == 2 && CloudDownloadPercent(second) == -1,
              "download: closing the app cancels the rest");
        SetEnvironmentVariableW(L"PULSE_TEST_HYDRATE_DELAY_MS", nullptr);
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr);
        if (sink) DestroyWindow(sink);
        Sleep(300);  // let the cancelled workers close their handles
        SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
        const bool removed = DeleteFileW(file.c_str()) && DeleteFileW(second.c_str()) && RemoveDirectoryW(dir.c_str());
        Check(removed, "download: fixture files removed");
    }

    RegDeleteTreeW(HKEY_CURRENT_USER, prefix.c_str());
    for (const auto* dir : {&wps, &baidu, &od}) RemoveDirectoryW(dir->c_str());
    RemoveDirectoryW(base.c_str());
    HKEY left = nullptr;
    Check(RegOpenKeyExW(HKEY_CURRENT_USER, prefix.c_str(), 0, KEY_READ, &left) != ERROR_SUCCESS,
          "fixture registry tree removed");
    if (left) RegCloseKey(left);
    return g_passed ? 0 : 1;
}

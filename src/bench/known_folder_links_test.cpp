#include "../app/known_folder_links.h"
#include "../app/app_model.h"
#include "../app/places.h"
#include "../app/session.h"
#include "../common/localization.h"
#include "../fs/fs_enum.h"

#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// In-memory resolver mock for testing
std::wstring MockResolver(REFKNOWNFOLDERID fid) {
    if (InlineIsEqualGUID(fid, FOLDERID_Desktop)) return L"C:\\Users\\TestUser\\Desktop";
    if (InlineIsEqualGUID(fid, FOLDERID_Downloads)) return L"C:\\Users\\TestUser\\Downloads";
    if (InlineIsEqualGUID(fid, FOLDERID_Documents)) return L"C:\\Users\\TestUser\\Documents";
    if (InlineIsEqualGUID(fid, FOLDERID_Pictures)) return L"C:\\Users\\TestUser\\Pictures";
    if (InlineIsEqualGUID(fid, FOLDERID_Music)) return L"C:\\Users\\TestUser\\Music";
    if (InlineIsEqualGUID(fid, FOLDERID_Videos)) return L"C:\\Users\\TestUser\\Videos";
    return {};
}

// Resolver that simulates failure for Videos and Music
std::wstring FlakyResolver(REFKNOWNFOLDERID fid) {
    if (InlineIsEqualGUID(fid, FOLDERID_Documents)) return L"C:\\Users\\TestUser\\Documents";
    return {};
}

void TestDescriptors() {
    using namespace pulse::app;
    Check(static_cast<int>(BuiltinQuickAccess::Count) == 8,
          "BuiltinQuickAccess count is exactly 8");

    // Display order check
    constexpr BuiltinQuickAccess expected_order[] = {
        BuiltinQuickAccess::Recent,
        BuiltinQuickAccess::Desktop,
        BuiltinQuickAccess::Downloads,
        BuiltinQuickAccess::Documents,
        BuiltinQuickAccess::Pictures,
        BuiltinQuickAccess::Music,
        BuiltinQuickAccess::Videos,
        BuiltinQuickAccess::RecycleBin,
    };
    bool order_matches = (sizeof(kQuickAccessDisplayOrder) == sizeof(expected_order));
    for (size_t i = 0; i < std::size(expected_order); ++i) {
        if (kQuickAccessDisplayOrder[i] != expected_order[i]) order_matches = false;
    }
    Check(order_matches, "kQuickAccessDisplayOrder matches required sequence");

    // Descriptors validation
    auto all = GetAllKnownFolderDescriptors();
    Check(all.size() == 8, "8 known folder descriptors available");
    for (const auto& d : all) {
        Check(d.glyph != nullptr && d.glyph[0] != L'\0', "descriptor has valid glyph");
        Check(d.fallback != nullptr && d.fallback[0] != L'\0', "descriptor has valid fallback");
        Check(d.color_rgb > 0, "descriptor has non-zero color RGB");
    }
}

void TestResolutionAndPathMatching() {
    using namespace pulse::app;
    KnownFolderSnapshot snap = ResolveKnownFolders(nullptr, MockResolver);

    // Root exact matches
    const auto* doc_desc = snap.FindByPath(L"C:\\Users\\TestUser\\Documents");
    Check(doc_desc && doc_desc->id == BuiltinQuickAccess::Documents,
          "Documents root path matches descriptor");

    // Case-insensitivity
    const auto* doc_case = snap.FindByPath(L"c:\\users\\testuser\\DOCUMENTS");
    Check(doc_case && doc_case->id == BuiltinQuickAccess::Documents,
          "Documents path matching is case-insensitive");

    // Trailing slash normalization
    const auto* doc_slash = snap.FindByPath(L"C:/Users/TestUser/Documents/");
    Check(doc_slash && doc_slash->id == BuiltinQuickAccess::Documents,
          "Documents path matching normalizes forward slashes and trailing slash");

    // All four new known folders match
    Check(snap.FindByPath(L"C:\\Users\\TestUser\\Music")->id == BuiltinQuickAccess::Music,
          "Music root path matches");
    Check(snap.FindByPath(L"C:\\Users\\TestUser\\Pictures")->id == BuiltinQuickAccess::Pictures,
          "Pictures root path matches");
    Check(snap.FindByPath(L"C:\\Users\\TestUser\\Videos")->id == BuiltinQuickAccess::Videos,
          "Videos root path matches");

    // Non-matches: Subdirectories must NOT match
    Check(snap.FindByPath(L"C:\\Users\\TestUser\\Documents\\SubProject") == nullptr,
          "Subdirectory under Documents does NOT match KnownFolder");
    Check(snap.FindByPath(L"C:\\Users\\TestUser\\Music\\Album") == nullptr,
          "Subdirectory under Music does NOT match KnownFolder");

    // Non-matches: Folders with same name in other locations
    Check(snap.FindByPath(L"D:\\OtherFolder\\Documents") == nullptr,
          "Arbitrary folder named Documents does NOT match KnownFolder");
    Check(snap.FindByPath(L"C:\\Backup\\Pictures") == nullptr,
          "Arbitrary folder named Pictures does NOT match KnownFolder");
}

void TestRedirectionDeduplication() {
    using namespace pulse::app;
    // When multiple known folders redirect to the same path, the earlier one in
    // kQuickAccessDisplayOrder wins.
    KnownFolderSnapshot snap;
    snap.SetPath(BuiltinQuickAccess::Documents, L"C:\\Users\\TestUser\\SharedFolder");
    snap.SetPath(BuiltinQuickAccess::Pictures, L"C:\\Users\\TestUser\\SharedFolder");

    const auto* match = snap.FindByPath(L"C:\\Users\\TestUser\\SharedFolder");
    Check(match && match->id == BuiltinQuickAccess::Documents,
          "Redirection to same path chooses Documents (earlier in display order)");
}

void TestFailureAndPreviousRetention() {
    using namespace pulse::app;
    // Resolver fails for some items without previous snapshot
    KnownFolderSnapshot flaky = ResolveKnownFolders(nullptr, FlakyResolver);
    Check(!flaky.GetPath(BuiltinQuickAccess::Documents).empty(),
          "Successful resolution populates path");
    Check(flaky.GetPath(BuiltinQuickAccess::Videos).empty(),
          "Failed resolution leaves path empty and does NOT fabricate path");
    Check(flaky.FindByPath(L"C:\\Users\\TestUser\\Videos") == nullptr,
          "Failed item has no match");

    // Resolver fails, but previous snapshot had valid path: retains previous snapshot
    KnownFolderSnapshot valid = ResolveKnownFolders(nullptr, MockResolver);
    KnownFolderSnapshot recovered = ResolveKnownFolders(&valid, FlakyResolver);
    Check(recovered.GetPath(BuiltinQuickAccess::Videos) == L"C:\\Users\\TestUser\\Videos",
          "Transient resolver failure retains previous valid snapshot");
}

void TestExplicitPinPrecedence() {
    using namespace pulse::app;
    // Test that explicitly pinned KnownFolder path suppresses the built-in row
    // and displays with the custom KnownFolder metadata in the pinned area.
    SidebarModel sidebar;
    sidebar.known_folders = ResolveKnownFolders(nullptr, MockResolver);

    // Build quick access entries in sidebar
    for (BuiltinQuickAccess id : kQuickAccessDisplayOrder) {
        if (id == BuiltinQuickAccess::Recent || id == BuiltinQuickAccess::RecycleBin) continue;
        const auto* desc = GetKnownFolderDescriptor(id);
        const std::wstring& path = sidebar.known_folders.GetPath(id);
        if (path.empty()) continue;
        SidebarEntry entry;
        entry.path = path;
        entry.builtin = static_cast<int>(id);
        entry.label = desc->fallback;
        sidebar.quick_access.push_back(std::move(entry));
    }

    PlacesCatalog places;
    places.persist = false;
    const std::wstring doc_path = L"C:\\Users\\TestUser\\Documents";

    // Before pin: Documents is in built-in rows
    bool found_doc_builtin = false;
    for (const auto& e : sidebar.quick_access) {
        if (e.path == doc_path) found_doc_builtin = true;
    }
    Check(found_doc_builtin, "Documents is initially in built-in quick access");

    // User explicitly pins Documents
    places.SetQuickAccessPinned({ doc_path }, true);
    Check(places.IsQuickAccessPinned(doc_path), "PlacesCatalog records explicit pin");

    // Verify ViewModel filtering: built-in row is suppressed
    std::vector<SidebarEntry> filtered;
    for (const auto& e : sidebar.quick_access) {
        if (places.IsQuickAccessPinned(e.path)) continue;
        filtered.push_back(e);
    }
    bool doc_in_filtered = false;
    for (const auto& e : filtered) {
        if (e.path == doc_path) doc_in_filtered = true;
    }
    Check(!doc_in_filtered, "Explicit pin suppresses duplicate built-in row");

    // Verify pinned row lookup resolves to KnownFolder descriptor
    const auto* kf = sidebar.known_folders.FindByPath(doc_path);
    Check(kf && kf->id == BuiltinQuickAccess::Documents,
          "Explicitly pinned path resolves to Documents descriptor for icon and label");

    // Unpin: restores built-in row
    places.SetQuickAccessPinned({ doc_path }, false);
    Check(!places.IsQuickAccessPinned(doc_path), "PlacesCatalog records unpin");
    filtered.clear();
    for (const auto& e : sidebar.quick_access) {
        if (places.IsQuickAccessPinned(e.path)) continue;
        filtered.push_back(e);
    }
    doc_in_filtered = false;
    for (const auto& e : filtered) {
        if (e.path == doc_path) doc_in_filtered = true;
    }
    Check(doc_in_filtered, "Unpinning restores built-in row");
}

void TestLocalization() {
    using namespace pulse::l10n;
    Initialize(GetModuleHandleW(nullptr), L"zh-CN");

    SetLanguage(L"zh-CN");
    Check(Get(StringId::Documents) == L"文档", "zh-CN Documents is 文档");
    Check(Get(StringId::Pictures) == L"图片", "zh-CN Pictures is 图片");
    Check(Get(StringId::Music) == L"音乐", "zh-CN Music is 音乐");
    Check(Get(StringId::Videos) == L"视频", "zh-CN Videos is 视频");

    SetLanguage(L"en-US");
    Check(Get(StringId::Documents) == L"Documents", "en-US Documents is Documents");
    Check(Get(StringId::Pictures) == L"Pictures", "en-US Pictures is Pictures");
    Check(Get(StringId::Music) == L"Music", "en-US Music is Music");
    Check(Get(StringId::Videos) == L"Videos", "en-US Videos is Videos");

    SetLanguage(L"zh-TW");
    Check(Get(StringId::Documents) == L"文件", "zh-TW Documents is 文件");
    Check(Get(StringId::Pictures) == L"圖片", "zh-TW Pictures is 圖片");
    Check(Get(StringId::Music) == L"音樂", "zh-TW Music is 音樂");
    Check(Get(StringId::Videos) == L"影片", "zh-TW Videos is 影片");
}

void TestSessionMigration() {
    using namespace pulse::app;
    // Version 6: pre-v7 session drops quickAccessHidden to 0
    SessionSnapshot s6;
    const std::wstring json6 = L"{\"version\":6,\"quickAccessHidden\":255,\"windows\":[{\"quickAccessHidden\":255}]}";
    Check(ParseSessionJson(json6, s6) && s6.quick_access_hidden == 0 &&
              s6.windows[0].layout.quick_access_hidden == 0,
          "Session migration: v6 drops mask to 0");

    // Version 8: retains low 4 bits (0x0F), clears bit 4+
    SessionSnapshot s8;
    const std::wstring json8 = L"{\"version\":8,\"quickAccessHidden\":245,\"windows\":[{\"quickAccessHidden\":245}],"
                               L"\"closedWindows\":[{\"closedName\":\"c\",\"quickAccessHidden\":245,"
                               L"\"layoutTabs\":[{\"title\":\"t\",\"layout\":0,\"panes\":[{\"path\":\"C:\\\\\"}]}]}]}";
    Check(ParseSessionJson(json8, s8) && s8.quick_access_hidden == (245 & 0x0F) &&
              s8.windows[0].layout.quick_access_hidden == (245 & 0x0F) &&
              s8.closed_windows[0].window.layout.quick_access_hidden == (245 & 0x0F),
          "Session migration: v8 masks with 0x0F across all 3 tiers");

    // Version 9: preserves full 8 bits (0xFF)
    SessionSnapshot s9;
    const std::wstring json9 = L"{\"version\":9,\"quickAccessHidden\":245,\"windows\":[{\"quickAccessHidden\":245}],"
                               L"\"closedWindows\":[{\"closedName\":\"c\",\"quickAccessHidden\":245,"
                               L"\"layoutTabs\":[{\"title\":\"t\",\"layout\":0,\"panes\":[{\"path\":\"C:\\\\\"}]}]}]}";
    Check(ParseSessionJson(json9, s9) && s9.quick_access_hidden == 245 &&
              s9.windows[0].layout.quick_access_hidden == 245 &&
              s9.closed_windows[0].window.layout.quick_access_hidden == 245,
          "Session migration: v9 retains full 8-bit mask across all 3 tiers");
}

} // namespace

int main() {
    TestDescriptors();
    TestResolutionAndPathMatching();
    TestRedirectionDeduplication();
    TestFailureAndPreviousRetention();
    TestExplicitPinPrecedence();
    TestLocalization();
    TestSessionMigration();

    std::printf("known_folder_links_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

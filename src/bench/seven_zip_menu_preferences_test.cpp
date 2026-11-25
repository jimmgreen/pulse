#include "../app/extensions/resource_settings.h"
#include "../app/extensions/seven_zip_menu_projection.h"
#include "../app/extensions/seven_zip_menu_settings.h"
#include <cstdio>
#include <filesystem>
#include <string_view>

namespace {
using namespace pulse::extensions;
namespace menu = pulse::extensions::seven_zip_menu;
namespace prefs = pulse::seven_zip;
constexpr auto kSaved = "5\n2\n1\n1\n5\n1\n1\n1b\n0\n0\nfff\nC:\\Test Output";
int failures = 0;

void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const std::wstring_view mode(argv[1]);
    const auto profile = std::filesystem::absolute(argv[2]);
    if (mode == L"--archive-open") {
        std::filesystem::create_directories(profile);
        Check(menu::PreviewOnOpen(), "cold startup never extracts before preferences load");
        menu::Publish(ReadOwnSettings(profile.wstring(), "pulse.7zip"), true);
        Check(!menu::PreviewOnOpen(), "a loaded fresh profile keeps smart extract as default");
        const auto save = [&](char choice, uint64_t version) {
            const auto encoded = std::string("5\n2\n1\n1\n5\n1\n1\n1b\n1\n") +
                choice + "\nfff\nC:\\Test Output";
            const auto result = WriteOwnSetting(profile.wstring(), "pulse.7zip", prefs::kPreferencesKey,
                encoded, version, [] { return true; });
            Check(result.find("data") != nullptr, "archive open choice saves to the test profile");
        };
        save('1', 0);
        const auto preview = ReadOwnSettings(profile.wstring(), "pulse.7zip");
        menu::Publish(preview);
        Check(menu::PreviewOnOpen(), "saved In Pulse option selects preview rather than smart extract");
        save('0', 1);
        menu::Publish(ReadOwnSettings(profile.wstring(), "pulse.7zip"));
        Check(!menu::PreviewOnOpen(), "saved Smart extract option still selects extraction");
        menu::SetPreviewOnOpen(true);
        menu::Publish(preview, true);
        Check(menu::PreviewOnOpen(), "a new selection applies before its write and beats a late read");
        menu::SetPreviewOnOpen(false);
        Check(!menu::PreviewOnOpen(), "switching back takes effect immediately too");
        return failures ? 1 : 0;
    }
    if (mode == L"--seed") {
        std::filesystem::create_directories(profile);
        const auto result = WriteOwnSetting(profile.wstring(), "pulse.7zip", prefs::kPreferencesKey, kSaved, 0, [] { return true; });
        Check(result.find("data") != nullptr, "saved non-default settings to a fresh profile");
        return failures ? 1 : 0;
    }
    if (mode != L"--baseline" && mode != L"--cold") return 2;
    Check(!menu::Snapshot() && menu::BeginRead() && !menu::BeginRead(), "cold cache has no default settings and starts one read");
    menu::Publish(ReadOwnSettings(profile.wstring(), "pulse.7zip"), true);
    const auto snapshot = menu::Snapshot();
    Check(snapshot && *snapshot == kSaved, "fresh process reads the persisted settings");
    if (!snapshot) return 1;
    const auto settings = prefs::ParseCommandPreferences(*snapshot);
    Check(settings.has_value(), "v5 preferences decoded");
    if (!settings) return 1;

    ManifestCommandContribution quick; quick.id = "quick-compress";
    quick.title.en = "Quick compress to 7z";
    quick.detail.en = "Saves {base}.7z · Normal";
    ManifestCommandContribution extract; extract.id = "extract";
    ManifestCommandContribution test; test.id = "test-archive";
    bool test_visible = true;
    if (mode == L"--cold") {
        menu::ApplyPreferences(quick, *settings);
        menu::ApplyPreferences(extract, *settings);
        test_visible = menu::ApplyPreferences(test, *settings);
    }
    Check(quick.title.en == "Quick compress to zip" && quick.detail.en == "Saves {base}.zip · Ultra",
        "first menu uses saved ZIP format and level, not manifest defaults");
    Check(quick.submenu && extract.submenu && test_visible,
        "saved submenu applies to first menu; an old hidden bit is left to the host (migrated once)");
    Check(extract.detail.en.find("C:\\Test Output") != std::string::npos, "saved fixed extraction folder applies to first menu");
    const auto v3 = prefs::ParseCommandPreferences("3\n1\n0\n4\n2\n1\n");
    const auto v4 = prefs::ParseCommandPreferences("4\n1\n0\n4\n2\n1\n1\n0f\n");
    Check(v3 && v3->format == 4, "v3 legacy settings retained");
    Check(v4 && v4->commands == 15, "v4 legacy command mask retained");
    const auto v5 = prefs::ParseCommandPreferences("5\n1\n0\n0\n3\n1\n0\n3f\n1\n0\nfff\nD:\\Out");
    const auto v6 = prefs::ParseCommandPreferences("6\n1\n0\n0\n3\n1\n0\n3f\n1\n0\nfff\n2\nD:\\Out");
    Check(v5 && v5->takeover && v5->icon_style == 0 && v5->folder == "D:\\Out", "v5 settings keep the document icons");
    Check(v6 && v6->takeover && v6->icon_style == 2 && v6->folder == "D:\\Out", "v6 carries the icon style before the folder");
    Check(!prefs::ParseCommandPreferences("6\n1\n0\n0\n3\n1\n0\n3f\n1\n0\nfff\n9\n"), "unknown icon style rejected");
    Check(!prefs::ParseCommandPreferences("5\n0\n1\n99\n"), "malformed data rejected");

    const auto stale = ReadOwnSettings(profile.wstring(), "pulse.7zip");
    const auto fresh = WriteOwnSetting(profile.wstring(), "pulse.7zip", prefs::kPreferencesKey,
        "5\n1\n0\n0\n3\n1\n0\n3f\n0\n0\nfff\n", 1, [] { return true; });
    if (fresh.find("data")) {
        menu::Publish(fresh);
        menu::Publish(stale, true);
        const auto current = menu::Snapshot();
        Check(current && *current != kSaved, "late startup read does not replace newer committed settings");
    } else Check(false, "settings update writes to the fixture");
    std::printf("Failures=%d\n", failures);
    return failures ? 1 : 0;
}
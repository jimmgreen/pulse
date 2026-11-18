// drive_capacity_test - how full a drive reads (drive_capacity.h): the three
// colour levels and their edges, the whole percent shown next to the bar, and
// the sidebar / This PC tile wording, including where the bold free size sits.
#include "../ui/drive_capacity.h"

#include <windows.h>

#include <cstdio>
#include <initializer_list>
#include <string>

using namespace pulse;
using namespace pulse::ui;

namespace {
int g_failed = 0, g_passed = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    ok ? ++g_passed : ++g_failed;
}

constexpr std::uint64_t kGB = std::uint64_t{1} << 30;
}  // namespace

int main() {
    // Levels: accent under 80%, amber 80-94%, red from 95%.
    Check(CapacityLevelFor(0.0f) == CapacityLevel::Normal, "an empty drive is normal");
    Check(CapacityLevelFor(0.794f) == CapacityLevel::Normal, "79.4% (shown 79%) is still normal");
    Check(CapacityLevelFor(0.7996f) == CapacityLevel::Warning, "79.96% shows 80%, so it is amber");
    Check(CapacityLevelFor(0.80f) == CapacityLevel::Warning, "80% turns amber");
    Check(CapacityLevelFor(0.85f) == CapacityLevel::Warning, "85% is amber");
    Check(CapacityLevelFor(0.944f) == CapacityLevel::Warning, "94.4% stays amber");
    Check(CapacityLevelFor(0.949f) == CapacityLevel::Critical, "94.9% shows 95%, so it is red");
    Check(CapacityLevelFor(0.95f) == CapacityLevel::Critical, "95% turns red");
    Check(CapacityLevelFor(1.0f) == CapacityLevel::Critical, "a full drive is red");
    // Under 1 GB left only counts once the drive is also 80% used.
    Check(CapacityLevelFor(0.81f, kGB - 1) == CapacityLevel::Critical, "81% with under 1 GB left is red");
    Check(CapacityLevelFor(0.79f, kGB - 1) == CapacityLevel::Normal, "79% with under 1 GB left is not red");
    Check(CapacityLevelFor(0.86f, kGB - 1) == CapacityLevel::Critical, "86% with under 1 GB left is red");
    Check(CapacityLevelFor(0.86f, kGB) == CapacityLevel::Warning, "86% with exactly 1 GB left is amber");
    Check(CapacityLevelFor(0.20f, kGB / 4) == CapacityLevel::Normal, "a small stick at 20% is not red");
    Check(CapacityLevelFor(0.90f, kCapacityUnknownFree) == CapacityLevel::Warning, "unknown free bytes: by percent");

    // Percent: rounded, never 0% while used, never 100% while something is left.
    Check(CapacityPercent(0.0f) == 0, "nothing used is 0%");
    Check(CapacityPercent(0.001f) == 1, "a sliver used shows 1%");
    Check(CapacityPercent(0.844f) == 84, "84.4% rounds to 84");
    Check(CapacityPercent(0.999f) == 99, "almost full still shows 99%");
    Check(CapacityPercent(1.0f) == 100, "full is 100%");
    Check(CapacityPercent(-1.0f) == 0, "unknown (-1) is 0%");
    Check(CapacityPercentText(0.62f) == L"62%", "percent text");

    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    for (const wchar_t* lang : {L"zh-CN", L"en-US", L"zh-TW"}) {
        l10n::SetLanguage(lang);
        const std::string tag = lang[0] == L'e' ? "en" : lang[3] == L'T' ? "tw" : "zh";
        const auto label = [&](const char* what) { return tag + ": " + what; };

        const std::uint64_t total = 476ull * kGB, free = 75ull * kGB;
        const std::wstring side = DriveSidebarDetail(free, total);
        const std::wstring free_compact = format::ByteSize(free, false, format::ByteSizeStyle::Compact);
        Check(side.find(free_compact) != std::wstring::npos && side.find(L'{') == std::wstring::npos,
              label("sidebar shows the compact free size").c_str());
        const std::wstring side_red = DriveSidebarDetail(10ull * kGB, total);
        Check(side_red != side && side_red.find(L'{') == std::wstring::npos &&
                  side_red.find(format::ByteSize(10ull * kGB, false, format::ByteSizeStyle::Compact)) != std::wstring::npos,
              label("sidebar words a red drive differently").c_str());
        Check(DriveSidebarDetail(1, 0).empty(), label("no total, no sidebar text").c_str());

        const DriveSpaceText tile = DriveTileSpaceText(free, total);
        const std::wstring free_text = format::ByteSize(free);
        Check(tile.text.find(L'{') == std::wstring::npos &&
                  tile.text.find(format::ByteSize(total)) != std::wstring::npos,
              label("tile shows the total").c_str());
        Check(tile.bold_length == free_text.size() && tile.text.compare(tile.bold_start, tile.bold_length, free_text) == 0,
              label("tile bolds exactly the free size").c_str());
        const DriveSpaceText red = DriveTileSpaceText(5ull * kGB, 325ull * kGB);
        const std::wstring red_free = format::ByteSize(5ull * kGB);
        Check(red.text != tile.text && red.bold_length == red_free.size() &&
                  red.text.compare(red.bold_start, red.bold_length, red_free) == 0,
              label("a red tile says only X left, X bold").c_str());
        Check(DriveTileSpaceText(free * 2, free).text.find(format::ByteSize(free)) != std::wstring::npos,
              label("free above total is clamped").c_str());
    }

    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#include "../ui/picker_save_feedback.h"
#include <cstdio>
int main() {
    using namespace pulse::ui;
    unsigned failures = 0, checks = 0;
    const auto check = [&](bool ok, const char* label) { ++checks; if (!ok) ++failures; std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); };
    const auto key = PickerSaveSeriesKey(L"banner-v4.txt");
    for (const auto* name : {L"banner.txt", L"banner-v1.png", L"banner_v02.jpg", L"BANNERV9.txt", L"banner v3.txt"})
        check(PickerSaveSeriesMatches(key, name), "version family ignores final extension and case");
    for (const auto* name : {L"banner2.txt", L"banner-2026.txt", L"banner-vx.txt", L"banner.tar.gz", L"banner-copy.txt"})
        check(!PickerSaveSeriesMatches(key, name), "plain numbers and unrelated suffixes stay distinct");
    check(!PickerSaveSeriesMatches(key, L"banner-v3.txt", true), "directories excluded");
    for (const auto* name : {L"", L"bad?.txt", L"CON.txt", L"banner.", L"banner.txt ", L"v1.txt", L"-v2.txt", L"x.txt:stream"})
        check(PickerSaveSeriesKey(name).empty(), "invalid or empty family rejected");
    for (const auto* name : {LR"(C:\work\banner-v3.txt)", LR"(\\server\share\banner-v3.txt)", LR"(\\?\C:\work\banner-v3.txt)", L"child/banner-v3.txt"})
        check(PickerSaveSeriesMatches(key, name), "absolute UNC extended and relative leaf extraction");
    check(PickerSaveSeriesMatches(PickerSaveSeriesKey(L"设计-v3.png"), L"设计_v4.txt"), "Unicode family");
    check(!PickerSaveSeriesMatches(PickerSaveSeriesKey(L".env"), L".other"), "dotfiles distinct");
    std::printf("CHECKS=%u FAILURES=%u\n", checks, failures); return failures ? 1 : 0;
}

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../app/file_click_gesture.h"
#include "../app/app_prefs.h"
#include <cstdio>

int main() {
    using namespace pulse::app;
    int pane = 0, tab = 0, snapshot = 0;
    const FileClickIdentity a{&pane, &tab, &snapshot, 4, 0, L"C:\\fixture\\A"};
    auto b = a; b.path = L"C:\\fixture\\B";
    FileClickGesture click;
    int failures = 0;
    const auto check = [&](bool pass, const char* text) {
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", text);
        if (!pass) ++failures;
    };
    check(AppPrefs{}.double_click_interval_ms == 0, "double click preference defaults to following Windows");
    check(NormalizeDoubleClickInterval(0) == 0, "zero interval preserves the system setting");
    check(NormalizeDoubleClickInterval(-1) == 200 && NormalizeDoubleClickInterval(199) == 200,
        "nonzero intervals below range clamp to 200ms");
    check(NormalizeDoubleClickInterval(200) == 200 && NormalizeDoubleClickInterval(625) == 625 &&
        NormalizeDoubleClickInterval(1000) == 1000, "valid custom intervals retain exact values");
    check(NormalizeDoubleClickInterval(1001) == 1000, "intervals above range clamp to 1000ms");
    click.Begin(a, 100, false, true);
    check(click.Release(a, 120, 500, true, true, true) == FileClickAction::None && click.RenamePending(),
        "selected name release waits for rename");
    click.ExpectCaptureRelease(); click.CaptureChanged();
    check(click.RenamePending(), "ordinary mouse-up capture release preserves pending rename");
    check(click.DoubleClick(a, 250, 500) == FileClickAction::Open && !click.RenamePending(),
        "selected double click opens once and cancels rename");
    click.Begin(a, 100, false, true);
    click.Release(a, 2000, 500, true, true, true);
    check(click.RenameDue() == 2000, "v58 long hold makes rename due on release");
    check(click.Tick(a, 2500, true, false) == FileClickAction::None, "queued input has priority over rename");
    check(click.Tick(a, 2500, true, true) == FileClickAction::Rename, "quiet selected name renames after system interval");
    click.Begin(a, 100, false, true); click.Release(a, 120, 500, true, true, true);
    check(click.Tick(a, 620, false, true) == FileClickAction::None && !click.RenamePending(), "deactivation cancels rename");
    click.Begin(a, 100, false, true); click.Release(a, 120, 500, true, true, true);
    click.CaptureChanged();
    check(!click.Item().Valid(), "unexpected capture loss cancels identity and rename");
    click.Begin(a, 100, true, false);
    check(click.Release(b, 120, 500, true, true, true) == FileClickAction::None, "replacement at same index never opens");
    click.Begin(a, 100, false, true); click.Release(a, 120, 500, true, true, true);
    auto refreshed = a; ++refreshed.generation;
    check(click.DoubleClick(refreshed, 250, 500) == FileClickAction::Open, "v58 double click opens current hit after view refresh");
    int other_pane = 0, other_tab = 0, other_snapshot = 0;
    auto moved_pane = a; moved_pane.pane = &other_pane;
    click.Begin(a, 100, true, false);
    check(click.Release(moved_pane, 120, 500, true, true, true) == FileClickAction::None,
        "different pane with the same file and index cancels single open");
    auto moved_tab = a; moved_tab.tab = &other_tab;
    click.Begin(a, 100, false, false); click.Release(a, 120, 500, true, true, true);
    check(click.DoubleClick(moved_tab, 250, 500) == FileClickAction::Open,
        "v58 native double click opens current hit");
    auto replaced_snapshot = a; replaced_snapshot.snapshot = &other_snapshot;
    click.Begin(a, 100, false, true); click.Release(a, 120, 500, true, true, true);
    check(click.Tick(replaced_snapshot, 620, true, true) == FileClickAction::None && !click.RenamePending(),
        "replacement snapshot alone cancels rename even when generation and path match");
    click.Begin(a, 100, false, true); click.Release(a, 120, 500, true, true, true);
    check(click.Tick(b, 620, true, true) == FileClickAction::None, "rename revalidates stable file identity");
    click.Begin(a, 100, true, true);
    check(click.Release(a, 120, 500, true, true, true) == FileClickAction::Open && !click.RenamePending(),
        "single-click mode opens and never arms rename");
    click.Cancel(); // Navigation tears down the old view.
    click.SuppressDoubleClick(120);
    check(click.DoubleClick(b, 250, 500) == FileClickAction::None, "single-click folder navigation cannot double-open next directory");
    click.Begin(a, 100, false, false); click.Release(a, 120, 500, true, true, true);
    check(!click.RenamePending(), "modified selection click never renames");
    click.Begin(a, 100, false, true);
    click.Release(a, 120, 500, true, false, true);
    check(!click.RenamePending() && click.DoubleClick(a, 250, 500) == FileClickAction::Open,
        "row whitespace double click remains open-only");
    click.Begin(a, 100, false, true); click.Cancel();
    check(click.Release(a, 120, 500, true, true, true) == FileClickAction::None && !click.RenamePending(),
        "drag or keyboard cancellation cannot later rename");
    click.Begin(a, 0xfffffff0u, false, false); click.Release(a, 120, 500, true, false, true);
    check(click.DoubleClick(a, 0x20u, 500) == FileClickAction::Open, "message time wrap uses unsigned elapsed interval");
    for (const uint32_t interval : {200u, 500u, 900u}) {
        click.Cancel();
        click.Begin(a, 100, false, false, interval, 10, 10);
        click.Release(a, 1000, interval, true, true, true, 120);
        check(click.MatchesDoublePress(a, 100 + interval, interval, 11, 11, 2, 2),
            "custom double press includes interval boundary independently of native message type");
        check(!click.MatchesDoublePress(a, 101 + interval, interval, 10, 10, 2, 2),
            "custom double press rejects beyond configured interval");
        check(!click.MatchesDoublePress(a, 150, interval, 13, 10, 2, 2),
            "custom double press rejects displaced pointer");
        check(!click.MatchesDoublePress(b, 150, interval, 10, 10, 2, 2),
            "custom double press rejects a different file");
        click.InterruptForPress();
        click.Begin(a, 101 + interval, false, true, interval);
        click.Release(a, 2000, interval, true, true, true, 121 + interval);
        check(click.RenameDue() == 2000 + interval - 20,
            "v58 rename deadline counts from press rather than release");
        check(click.Tick(a, 2000 + interval, true, true) == FileClickAction::Rename,
            "v58 selected name click renames without rapid-repeat suppression");
    }
    click.Cancel();
    click.Begin(a, 0xfffffff0u, false, false, 200, 10, 10);
    click.Release(a, 1000, 200, true, true, true, 0xfffffff8u);
    check(click.MatchesDoublePress(a, 0x20u, 200, 10, 10, 2, 2),
        "custom double press handles event clock wrap");
    return failures ? 1 : 0;
}

#include "../app/file_click_gesture.h"
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
    click.Begin(a, 100, false, true);
    check(click.Release(a, 120, 500, true, true, true) == FileClickAction::None && click.RenamePending(),
        "selected name release waits for rename");
    click.ExpectCaptureRelease(); click.CaptureChanged();
    check(click.RenamePending(), "ordinary mouse-up capture release preserves pending rename");
    check(click.DoubleClick(a, 250, 500) == FileClickAction::Open && !click.RenamePending(),
        "selected double click opens once and cancels rename");
    click.Begin(a, 100, false, true);
    click.Release(a, 2000, 500, true, true, true);
    check(click.Tick(a, 2001, true, true) == FileClickAction::None, "long hold does not rename immediately on release");
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
    check(click.DoubleClick(refreshed, 250, 500) == FileClickAction::None, "changed view cancels file double click");
    int other_pane = 0, other_tab = 0, other_snapshot = 0;
    auto moved_pane = a; moved_pane.pane = &other_pane;
    click.Begin(a, 100, true, false);
    check(click.Release(moved_pane, 120, 500, true, true, true) == FileClickAction::None,
        "different pane with the same file and index cancels single open");
    auto moved_tab = a; moved_tab.tab = &other_tab;
    click.Begin(a, 100, false, false); click.Release(a, 120, 500, true, true, true);
    check(click.DoubleClick(moved_tab, 250, 500) == FileClickAction::None,
        "different tab with the same file and index cancels double open");
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
    for (const bool selected : {false, true}) {
        click.Cancel();
        click.Begin(a, 100, false, selected);
        click.Release(a, 120, 500, true, true, true);
        // Two normal downs: Windows did not recognize a double click after
        // pointer drift. The previous click must still prevent a rename.
        click.Begin(a, 200, false, true);
        click.Release(a, 220, 500, true, true, true);
        check(!click.RenamePending() && click.Tick(a, 800, true, true) == FileClickAction::None,
            selected ? "fast displaced selected clicks cannot rename" : "fast displaced selection clicks cannot rename");
    }
    for (const uint32_t interval : {200u, 500u, 900u}) {
        click.Cancel();
        click.Begin(a, 100, false, false, interval);
        click.Release(a, 1000, interval, true, true, true, 120);
        click.InterruptForPress();
        click.Begin(a, 120 + interval, false, true, interval);
        click.Release(a, 2000, interval, true, true, true, 140 + interval);
        check(!click.RenamePending(), "rapid repeat uses event time and configured system interval, not delayed dispatch time");
        click.InterruptForPress();
        click.Begin(a, 141 + interval * 2, false, true, interval);
        click.Release(a, 3000, interval, true, true, true, 161 + interval * 2);
        check(click.Tick(a, 3000 + interval, true, true) == FileClickAction::Rename,
            "deliberate slow name click still renames after rapid-repeat guard expires");
    }
    click.Cancel();
    click.Begin(a, 0xffffff00u, false, false);
    click.Release(a, 0xfffffff0u, 500, true, true, true, 0xfffffff0u);
    click.InterruptForPress(); click.Begin(a, 0x20u, false, true);
    click.Release(a, 0x40u, 500, true, true, true, 0x40u);
    check(!click.RenamePending(), "rapid-repeat protection handles message-time wrap");
    click.Cancel();
    click.Begin(b, 0x60u, false, true); click.Release(b, 0x80u, 500, true, true, true);
    check(click.RenamePending(), "cancelled gesture and different file do not inherit repeat suppression");
    return failures ? 1 : 0;
}

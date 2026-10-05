// quick_preview_follow_test.cpp — Regression cases for the quick preview
// follow decision (#91).
#include "../app/quick_preview_follow.h"

#include <cstdio>

int main() {
    using pulse::app::DecideQuickPreviewFollow;
    using pulse::app::QuickPreviewFollow;
    using pulse::ui::QuickPreviewItem;

    int failures = 0;
    int total = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        ++total;
        if (!ok) ++failures;
    };

    QuickPreviewItem shown;
    shown.path = L"C:\\photos\\a.png";
    shown.modified = 100;
    shown.size = 2048;
    shown.attrs = FILE_ATTRIBUTE_ARCHIVE;

    const auto decide = [&](const wchar_t* path, uint64_t modified, uint64_t size,
                            DWORD attrs) {
        return DecideQuickPreviewFollow(shown, path ? path : L"", modified, size, attrs);
    };

    // A row with nothing previewable must leave the window as it is.
    check(decide(L"", 0, 0, 0) == QuickPreviewFollow::Stay,
        "an unpreviewable focused row keeps the current preview");
    check(decide(nullptr, 100, 2048, FILE_ATTRIBUTE_ARCHIVE) == QuickPreviewFollow::Stay,
        "a null path keeps the current preview");

    // Same entry, nothing changed: holding an arrow key must not restart playback.
    check(decide(L"C:\\photos\\a.png", 100, 2048, FILE_ATTRIBUTE_ARCHIVE) ==
        QuickPreviewFollow::Stay, "the same row with identical fields stays put");

    // Same path, content changed on disk: reload rather than show a stale frame.
    check(decide(L"C:\\photos\\a.png", 101, 2048, FILE_ATTRIBUTE_ARCHIVE) ==
        QuickPreviewFollow::Switch, "the same path with a new mtime reloads");
    check(decide(L"C:\\photos\\a.png", 100, 4096, FILE_ATTRIBUTE_ARCHIVE) ==
        QuickPreviewFollow::Switch, "the same path with a new size reloads");
    check(decide(L"C:\\photos\\a.png", 100, 2048,
        FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY) == QuickPreviewFollow::Switch,
        "the same path with a flipped attribute bit reloads");

    // A different focused row always switches.
    check(decide(L"C:\\photos\\b.png", 1, 1, FILE_ATTRIBUTE_ARCHIVE) ==
        QuickPreviewFollow::Switch, "a different path switches");
    QuickPreviewItem empty_shown;
    check(DecideQuickPreviewFollow(empty_shown, L"C:\\photos\\b.png", 0, 0, 0) ==
        QuickPreviewFollow::Switch, "an empty shown entry switches to any previewable row");

    std::printf("%d passed, %d failed\n", total - failures, failures);
    return failures == 0 ? 0 : 1;
}

#include "../ui/quick_preview_command.h"
#include "../ui/preview_notice.h"
#include <cstdio>

int main() {
    using namespace pulse::ui;
    int failed = 0;
    const auto check = [&](bool pass, const char* text) {
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", text);
        failed += !pass;
    };
    QuickPreviewItem root;
    root.path = L"C:\\fixtures\\parent";
    root.name = L"parent";
    root.attrs = FILE_ATTRIBUTE_DIRECTORY;
    root.read_only = true;
    const auto child = FolderPreviewTarget(root, L"nested\\selected.txt", false);
    check(child.path == L"C:\\fixtures\\parent\\nested\\selected.txt" && child.name == L"selected.txt",
          "folder action targets the selected descendant, not the parent");
    check(child.read_only && !(child.attrs & FILE_ATTRIBUTE_DIRECTORY), "child inherits read-only policy and file kind");
    check(FolderPreviewTarget(root, L"subfolder", true).attrs & FILE_ATTRIBUTE_DIRECTORY,
          "folder row retains directory kind");
    check(FolderPreviewTarget(root, L"", false).path == root.path, "blank-space target is the previewed folder");
    check(CanApplyPreviewFileAction(true, true) && CanApplyPreviewFileAction(false, false) &&
          !CanApplyPreviewFileAction(false, true), "archive entries cannot invoke destructive operations on their container");
    check(FolderPreviewTarget(root, L"..\\outside", false).path.empty() &&
          FolderPreviewTarget(root, L"C:\\outside", false).path.empty() &&
          FolderPreviewTarget(root, L"\\outside", false).path.empty(), "invalid relative paths cannot escape the preview folder");
    QuickPreviewCommands commands;
    QuickPreviewCommand first;
    first.target = child;
    first.action = QuickPreviewAction::Delete;
    first.shift = true;
    first.folder_child = true;
    const auto token = commands.Push(first);
    first.target.path = L"C:\\different-selection";
    const auto second = commands.Push(first);
    QuickPreviewCommand received;
    check(commands.Take(token, received) && received.target.path == child.path && received.shift && received.folder_child,
          "queued operation captures immutable path, modifier and origin");
    check(!commands.Take(token, received), "a command can only be consumed once");
    commands.Clear();
    const auto third = commands.Push(first);
    check(!commands.Take(second, received) && third != second && commands.Take(third, received),
          "closing or switching preview invalidates stale commands without reusing tokens");
    check(!PreviewNotice(L"svg-unsupported:text").empty(), "SVG source fallback has a visible explanation");
    check(PreviewNotice(L"image-preview-incomplete:frame-limit") !=
          PreviewNotice(L"image-preview-incomplete:canvas-limit"), "truncated animation and static fallback have distinct notices");
    check(PreviewNotice(L"image-preview-incomplete:frame-count-mismatch", L"PULSEIMAGE\t1\nT\tanimation\nF\t2\t3\t0\n").find(L"2 / 3") != std::wstring::npos,
          "partial playable animation reports loaded and declared frame counts");
    check(!PreviewNotice(L"docx-reading").empty(), "DOCX reading limitations stay outside document text");
    check(!PreviewNotice(L"udf-preview-unavailable").empty() &&
          !PreviewNotice(L"compressed-stream-preview-read-failed").empty(), "archive failures have explicit notices");
    pulse::preview::Integrity partial;
    partial.state = pulse::preview::IntegrityState::Partial;
    partial.reason = pulse::preview::IntegrityReason::Limit;
    const auto dependency_notice = PreviewNotice(L"", L"Portable Executable (PE32+)\n[LIMIT]\n", partial);
    const auto report_notice = PreviewNotice(L"", L"Portable Executable (PE32+)\n[Preview truncated]\n", partial);
    const auto damaged_notice = PreviewNotice(L"", L"Portable Executable (PE32+)\nSome metadata is invalid\n", partial);
    check(dependency_notice != PreviewNotice(L"", L"ordinary text", partial) &&
          dependency_notice != report_notice && report_notice != damaged_notice,
          "PE dependency budgets, report truncation and unreadable metadata have distinct notices");
    check(PreviewNotice(L"", L"Portable Executable (PE32+)\n", {}).empty(),
          "complete PE preview does not gain a partial warning");
    return failed ? 1 : 0;
}

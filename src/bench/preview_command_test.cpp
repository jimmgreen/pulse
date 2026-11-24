#include "../ui/quick_preview_command.h"
#include "../ui/preview_notice.h"
#include "../common/localization.h"
#include <cstdio>

int main() {
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");  // notices come from the string table
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
    QuickPreviewCommands other_window;
    QuickPreviewCommand pin_cmd;
    pin_cmd.action = QuickPreviewAction::PinDetach;
    pin_cmd.target = root;
    QuickPreviewCommand close_cmd;
    close_cmd.action = QuickPreviewAction::DetachedClosed;
    const auto t_a = commands.Push(pin_cmd);
    const auto t_b = other_window.Push(close_cmd);
    QuickPreviewCommand out_a, out_b;
    check(t_a != 0 && t_b != 0 && t_a != t_b &&
          !commands.Take(t_b, out_b) && !other_window.Take(t_a, out_a) &&
          commands.Take(t_a, out_a) && out_a.action == QuickPreviewAction::PinDetach &&
          other_window.Take(t_b, out_b) && out_b.action == QuickPreviewAction::DetachedClosed,
          "multi-window preview commands allocate globally unique tokens across instances");
    check(!PreviewNotice(L"svg-unsupported:text").empty(), "SVG source fallback has a visible explanation");
    check(PreviewNotice(L"image-preview-incomplete:frame-limit") !=
          PreviewNotice(L"image-preview-incomplete:canvas-limit"), "truncated animation and static fallback have distinct notices");
    check(PreviewNotice(L"image-preview-incomplete:frame-count-mismatch", L"PULSEIMAGE\t1\nT\tanimation\nF\t2\t3\t0\n").find(L"2 / 3") != std::wstring::npos,
          "partial playable animation reports loaded and declared frame counts");
    check(PreviewNotice(L"offline-placeholder").empty(), "cloud placeholders show the download card, not a failure notice");
    check(!PreviewNotice(L"docx-reading").empty(), "DOCX reading limitations stay outside document text");
    check(!PreviewNotice(L"udf-preview-unavailable").empty() &&
          !PreviewNotice(L"compressed-stream-preview-read-failed").empty(), "archive failures have explicit notices");
    return failed ? 1 : 0;
}

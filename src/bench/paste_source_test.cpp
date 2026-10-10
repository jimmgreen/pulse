// paste_source_test.cpp — Ctrl+V source choice (tray batch vs clipboard).
// Regression: copying a file elsewhere and pasting in Pulse released the
// previous tray batch instead of the file just copied.
#include "../app/paste_source.h"

#include <cstdio>

using namespace pulse::app;

static int g_pass = 0, g_fail = 0;

static void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    ok ? ++g_pass : ++g_fail;
}

static PasteSource Choose(const std::vector<std::wstring>& batch, bool batch_move, bool has_files,
                          const std::vector<std::wstring>& clip, bool cut, bool written) {
    PasteChoiceInput in;
    in.batch_paths = &batch;
    in.batch_move = batch_move;
    in.clipboard_has_files = has_files;
    in.clipboard_paths = &clip;
    in.clipboard_cut = cut;
    in.clipboard_written_for_batch = written;
    return ChoosePasteSource(in);
}

int main() {
    const std::vector<std::wstring> old_batch{L"C:\\Work\\old-a.txt", L"C:\\Work\\old-b.txt"};
    const std::vector<std::wstring> new_copy{L"D:\\Photos\\new.jpg"};
    const std::vector<std::wstring> none;

    Check(Choose(old_batch, false, true, new_copy, false, false) == PasteSource::Clipboard,
          "a newer copy made outside the tray is pasted, not the previous batch");
    Check(Choose(old_batch, false, false, none, false, false) == PasteSource::TrayBatch,
          "no files on the clipboard: the newest batch is released as before");
    Check(Choose(old_batch, false, true, old_batch, false, true) == PasteSource::TrayBatch,
          "clipboard still holds the value written for the batch");
    Check(Choose({L"C:\\Work\\old-a.txt"}, true, true, old_batch, false, true) == PasteSource::TrayBatch,
          "unchanged clipboard sequence keeps the tray even after the card was edited");
    Check(Choose(old_batch, false, true,
                 {L"c:\\work\\OLD-B.TXT", L"\\\\?\\C:\\Work\\old-a.txt", L"C:/Work/old-a.txt"}, false, false) ==
              PasteSource::TrayBatch,
          "same files in another order, case, prefix and separator match the batch");
    Check(Choose(old_batch, false, true, old_batch, true, false) == PasteSource::Clipboard,
          "same files cut elsewhere follow the clipboard intent instead of the copy batch");
    Check(Choose(old_batch, true, true, old_batch, false, false) == PasteSource::Clipboard,
          "a copy of the same files never reuses a move batch");
    Check(Choose(old_batch, false, true, {L"C:\\Work\\old-a.txt"}, false, false) == PasteSource::Clipboard,
          "a subset of the batch is a different copy");
    Check(Choose(old_batch, false, true, {L"C:\\Work\\old-a.txt", L"C:\\Work\\old-b.txt", L"C:\\Work\\c.txt"},
                 false, false) == PasteSource::Clipboard,
          "a superset of the batch is a different copy");
    Check(Choose(none, false, true, new_copy, false, false) == PasteSource::Clipboard,
          "an empty batch never shadows clipboard files");

    Check(SamePathSet({L"C:\\Dir\\"}, {L"c:\\dir"}), "trailing separator is ignored");
    Check(SamePathSet({L"C:\\"}, {L"c:\\"}), "drive roots compare equal");
    Check(!SamePathSet({L"C:\\"}, {L"C:"}), "a drive root is not the drive-relative path");
    Check(SamePathSet({L"\\\\?\\UNC\\srv\\share\\a.txt"}, {L"\\\\SRV\\Share\\A.TXT"}), "UNC extended prefix");
    Check(SamePathSet({L"C:\\a", L"C:\\a"}, {L"C:\\A"}), "duplicates collapse");
    Check(!SamePathSet({L"C:\\a"}, {L"C:\\b"}), "different files differ");

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

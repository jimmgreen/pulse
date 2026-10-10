// paste_source.h — Ctrl+V picks between the newest staging-tray batch and the
// system clipboard.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app {

enum class PasteSource { TrayBatch, Clipboard };

// Ctrl+C / Ctrl+X in Pulse collect a tray batch and mirror it onto the system
// clipboard, but tray batches outlive that clipboard value (copy batches stay
// after a release and the tray is saved with the session). A later copy in
// Explorer, another application or a Pulse command that only writes the
// clipboard must win, otherwise Ctrl+V pastes the previous batch. The newest
// batch is used when:
//   - the clipboard holds no files (text, image, empty), as before;
//   - the clipboard is still the value Pulse wrote for that batch (sequence
//     number unchanged), even if the card was edited afterwards;
//   - the clipboard files are exactly the batch items with the same intent
//     (a clipboard manager rewrote it, or Pulse restarted).
struct PasteChoiceInput {
    const std::vector<std::wstring>* batch_paths = nullptr;
    bool batch_move = false;
    bool clipboard_has_files = false;
    const std::vector<std::wstring>* clipboard_paths = nullptr;
    bool clipboard_cut = false;
    bool clipboard_written_for_batch = false;
};

PasteSource ChoosePasteSource(const PasteChoiceInput& input);

// Same set of paths, ignoring order, duplicates, case, a \\?\ prefix, '/'
// separators and a trailing separator (drive roots keep theirs).
bool SamePathSet(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b);

} // namespace pulse::app

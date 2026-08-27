// clipboard.h — System clipboard interop with Explorer (CF_HDROP + drop effect).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::ops {

struct ClipboardData {
    std::vector<std::wstring> paths;
    bool cut = false; // CFSTR_PREFERREDDROPEFFECT == DROPEFFECT_MOVE
    uint32_t sequence = 0;
};

// Writes paths as CF_HDROP + CFSTR_PREFERREDDROPEFFECT so Explorer can paste them.
// Must be called on the UI thread (OpenClipboard affinity).
bool WriteClipboard(const std::vector<std::wstring>& paths, bool cut);

// Writes plain text (Ctrl+Shift+C "copy path"). UI thread only.
bool WriteClipboardText(const std::wstring& text);

// Reads an Explorer copy/cut (CF_HDROP). Returns false if no files present.
bool ReadClipboard(ClipboardData& out);

// Clears a completed cut only if the clipboard still contains the exact cut
// captured by ReadClipboard. This never clears a newer clipboard value.
bool CompleteCutClipboard(uint32_t sequence, const std::vector<std::wstring>& expected_paths);

} // namespace pulse::ops

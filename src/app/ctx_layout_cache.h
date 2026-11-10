#pragma once

// Persistent layout of Explorer context menus (右键菜单优化方案 §4.2, C4a).
//
// The rows of the last complete host answer per menu key survive restarts in
// %LOCALAPPDATA%\Pulse\ctx_layout.bin, so the first right-click of a type in
// a session is painted at its final size instead of growing as the handlers
// answer. Labels are stored with the selected item's names replaced by
// markers ({name}: 压缩到 "a.zip" -> 压缩到 "<stem>.zip") and filled with the
// current names when painted. Each key carries a fingerprint of the registry
// keys its handlers come from; a key whose fingerprint changed is dropped on
// load. Parsing and templating are pure; only the *File / Fingerprint
// functions touch the disk or the registry and they never run on the UI
// thread.
//
// C4b: rows are grouped by the handler (CLSID) that gave them. An answer
// that carries handler states replaces the rows of the handlers that
// finished and keeps the rows of the ones still running, so partial answers
// update the layout too, and each key remembers how long each handler took.

#include "../ops/ops_manager.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulse::app {

// Names a handler may build labels from. One selected item: its file name and
// stem. Several: the folder they are in. Background menus: the folder itself.
struct CtxNames {
    std::wstring file;   // a.docx
    std::wstring stem;   // a
    std::wstring folder; // parent folder name (multi-select only)
};

CtxNames CtxNamesFor(const std::vector<std::wstring>& paths, bool background);

// Markers stand in for the names inside stored labels (private-use chars).
inline constexpr wchar_t kCtxNameFile = L'\xE101';
inline constexpr wchar_t kCtxNameStem = L'\xE102';
inline constexpr wchar_t kCtxNameFolder = L'\xE103';

// A name is replaced only where a handler visibly quotes or extends it:
// right after a quote or bracket, or right before '.', '\', '/' or a closing
// quote, and never inside a longer word. "Git Bash Here" in a folder named Git
// stays as it is; 压缩到 "a.zip" becomes 压缩到 "<stem>.zip".
std::wstring TemplateCtxLabel(const std::wstring& text, const CtxNames& names);
std::wstring ExpandCtxLabel(const std::wstring& text, const CtxNames& names);

// Menu keys: ".docx", ":folder", ":drive", ":bg", ":file"; several selected
// items add "|m" (handlers such as "open in new window" are single-only).
std::wstring CtxLayoutBaseKey(const std::wstring& key);

using CtxLayoutRows = std::vector<ops::ShellMenuItem>;

// Last known state of one handler under one key, in menu order.
struct CtxHandlerStat {
    std::wstring clsid;
    uint32_t elapsed_ms = 0;  // last finished query; for one never seen finishing, time waited
    uint32_t rows = 0;        // rows it gave when it last finished
    bool done = false;        // it finished at least once: its rows are known
};
using CtxHandlerStats = std::vector<CtxHandlerStat>;

struct CtxLayoutEntry {
    CtxLayoutRows rows;
    CtxHandlerStats handlers;
};
using CtxLayoutMap = std::unordered_map<std::wstring, CtxLayoutEntry>;
using CtxFingerprintFn = std::function<uint64_t(const std::wstring& base_key)>;

// Shard merge: walks `handlers` (menu order) and takes each finished
// handler's rows from `live` and each unfinished one's from `previous`. Rows
// of handlers the list does not name are dropped from `previous` (no longer
// queried) and kept from `live`. With every handler finished the result is
// `live` itself.
CtxLayoutRows MergeCtxHandlerShards(const CtxLayoutRows& previous, const CtxLayoutRows& live,
                                    const std::vector<ops::ShellMenuHandlerStatus>& handlers);
// Updates `stats` to the handler list of an answer. True when something worth
// saving changed: the handler set, a finished handler's row count, or its time
// moving to another bucket (<50, <100, <200, <500, <1000, <2000 ms, more).
bool MergeCtxHandlerStats(CtxHandlerStats& stats,
                          const std::vector<ops::ShellMenuHandlerStatus>& handlers);

inline constexpr uint32_t kCtxLayoutVersion = 3;
inline constexpr size_t kCtxLayoutMaxKeys = 512;

struct CtxLayoutRecord {
    uint64_t fingerprint = 0;
    CtxLayoutRows rows;
    CtxHandlerStats handlers;
};
using CtxLayoutRecords = std::unordered_map<std::wstring, CtxLayoutRecord>;

// `build` identifies the writer; a file from another build is ignored (the
// handler row parsing may have changed with it).
std::vector<uint8_t> SerializeCtxLayout(const CtxLayoutRecords& records, std::wstring_view build);
bool ParseCtxLayout(const uint8_t* data, size_t size, std::wstring_view build,
                    CtxLayoutRecords& out);

// Hash of the existence and last-write time of every registry key the menu of
// `base_key` is built from (HKCU and HKLM Classes, Blocked/Approved lists).
// About 30 RegQueryInfoKey calls; background threads only.
uint64_t CtxLayoutFingerprint(const std::wstring& base_key);

// %LOCALAPPDATA%\Pulse\ctx_layout.bin; PULSE_CTX_LAYOUT_PATH overrides it.
std::wstring CtxLayoutCachePath();

// Reads the file and keeps the keys whose fingerprint still matches.
// False when there is no usable file (missing, damaged, another build).
bool LoadCtxLayoutFile(const std::wstring& path, std::wstring_view build,
                       const CtxFingerprintFn& fingerprint, CtxLayoutMap& out);
// Fingerprints every key now and replaces the file atomically. `keep_going`
// is asked right before the replace; false abandons the write.
bool SaveCtxLayoutFile(const std::wstring& path, std::wstring_view build,
                       const CtxLayoutMap& layout, const CtxFingerprintFn& fingerprint,
                       const std::function<bool()>& keep_going = {});

} // namespace pulse::app

#pragma once
#include <windows.h>
#include <objidl.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app {

enum class TrayContentKind : uint8_t {
    File = 0,
    Pic = 1,
    Text = 2,
    Link = 3,
};

enum class ShelfDropPayloadKind : uint8_t {
    None = 0,
    Files = 1,
    Content = 2,
};

struct ShelfContentItem {
    std::wstring path;
    TrayContentKind kind = TrayContentKind::File;
    std::wstring display_name;
    std::wstring domain;
    std::wstring source_label;
    bool owned_snippet = false;
};

// Default directory for materialized snippets (%LOCALAPPDATA%\Pulse\shelf).
std::wstring DefaultShelfSnippetsDir();

// Fast non-blocking query (DragEnter / DragOver): whether `obj` carries files
// or web/text/image/link content. Performs no disk I/O.
ShelfDropPayloadKind ProbeShelfDropPayload(IDataObject* obj);

// Materializes dropped files or content into `shelf_dir` (for content or
// volatile temp browser images) and returns ready-to-stage items.
bool ExtractShelfDropItems(IDataObject* obj, const std::wstring& shelf_dir,
                           std::vector<ShelfContentItem>& out,
                           bool* out_is_content = nullptr);

// Materializes clipboard files, bitmap/PNG, URL, or text (`Ctrl+V` on shelf).
bool ExtractClipboardShelfItems(HWND owner, const std::wstring& shelf_dir,
                                std::vector<ShelfContentItem>& out);

// Deletes `path` only if it lives directly inside `shelf_dir`.
void DeleteOwnedSnippetFile(const std::wstring& path, const std::wstring& shelf_dir);

// Removes unreferenced snippet files left in `shelf_dir`.
void SweepOrphanedShelfSnippets(const std::wstring& shelf_dir,
                                const std::vector<std::wstring>& referenced_paths);

// Extension suffix shown in the Floating Shelf snippet note (".png", ".txt", ".url").
const wchar_t* SnippetMaterializedExt(TrayContentKind kind) noexcept;

} // namespace pulse::app

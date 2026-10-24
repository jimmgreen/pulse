#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pulse::preview {

// Inspects file bytes only: never maps an image for execution or loads a DLL.
// Invalid headers return false; damaged directories produce a partial report.
bool MakePeDocument(std::span<const uint8_t> bytes, std::wstring& text, bool& truncated,
                    size_t text_limit = 32000);

struct PeImport {
    std::wstring name;
    bool normal = false;
    bool delay = false;
};
struct PeImports {
    uint16_t machine = 0;
    std::vector<PeImport> modules;
    bool incomplete = false;
};
bool ReadPeImports(std::span<const uint8_t> bytes, PeImports& imports);

} // namespace pulse::preview

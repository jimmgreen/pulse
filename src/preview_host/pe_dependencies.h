#pragma once

#include "pe_document.h"

namespace pulse::preview {

// Adds a bounded, static dependency tree. Reads PE files as data only; the
// candidate paths are not a promise of the runtime loader's actual bindings.
bool MakePeDocument(const std::wstring& path, std::span<const uint8_t> bytes,
                    std::wstring& text, bool& truncated);

} // namespace pulse::preview

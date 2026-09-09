// search_kinds.h — Named file kinds for type:/kind:/类型: query tokens.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace pulse::index {

enum class SearchKind : uint8_t {
    Any = 0,
    Folder,
    Document,
    Image,
    Video,
    Audio,
    Archive,
    Code,
    Custom,
};

const wchar_t* KindId(SearchKind kind);
SearchKind ParseKindId(std::wstring_view raw);
std::wstring KindExtensions(SearchKind kind); // folded, no leading dots, ;-separated
bool ExpandKindToken(std::wstring_view raw, std::vector<std::wstring>& exts, bool& folders_only);
std::wstring TextExtensionQueryToken();

} // namespace pulse::index

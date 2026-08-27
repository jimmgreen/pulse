#include "batch_rename.h"
#include "../fs/fs_enum.h"
#include <algorithm>
#include <cwctype>
#include <string_view>
#include <unordered_map>

namespace pulse::app {
namespace {

std::wstring FileNameOf(const std::wstring& path) {
    std::wstring_view view = path;
    while (view.size() > 1 && (view.back() == L'\\' || view.back() == L'/'))
        view.remove_suffix(1);
    const size_t slash = view.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? std::wstring(view)
                                            : std::wstring(view.substr(slash + 1));
}

std::wstring ParentOf(const std::wstring& path) {
    std::wstring copy = path;
    while (copy.size() > 1 && (copy.back() == L'\\' || copy.back() == L'/')) copy.pop_back();
    const size_t slash = copy.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return copy.substr(0, slash);
}

std::wstring JoinName(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

wchar_t Fold(wchar_t c) {
    return static_cast<wchar_t>(towlower(c));
}

std::wstring ReplaceInsensitive(const std::wstring& text, const std::wstring& find,
                                const std::wstring& replace) {
    if (find.empty()) return text;
    std::wstring out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (i + find.size() <= text.size()) {
            bool match = true;
            for (size_t n = 0; n < find.size(); ++n) {
                if (Fold(text[i + n]) != Fold(find[n])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                out += replace;
                i += find.size();
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

std::wstring FormatNumber(int value, int digits) {
    wchar_t buf[32]{};
    if (digits > 0) swprintf_s(buf, L"%0*d", digits, value);
    else swprintf_s(buf, L"%d", value);
    return buf;
}

bool ConsumeToken(std::wstring_view pattern, size_t& i, std::wstring_view token) {
    if (pattern.size() - i < token.size()) return false;
    if (pattern.substr(i, token.size()) != token) return false;
    i += token.size();
    return true;
}

std::wstring ExpandPattern(std::wstring_view pattern, std::wstring_view stem,
                           std::wstring_view ext, int value) {
    std::wstring out;
    out.reserve(pattern.size() + stem.size() + ext.size() + 8);
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == L'{') {
            if (ConsumeToken(pattern, i, L"{name}")) {
                out.append(stem);
                continue;
            }
            if (ConsumeToken(pattern, i, L"{ext}")) {
                out.append(ext);
                continue;
            }
            if (ConsumeToken(pattern, i, L"{n}")) {
                out += FormatNumber(value, 0);
                continue;
            }
            if (pattern.size() - i >= 3 && pattern.substr(i, 3) == L"{n:") {
                size_t j = i + 3;
                int pad = 0;
                while (j < pattern.size() && pattern[j] >= L'0' && pattern[j] <= L'9') {
                    pad = pad * 10 + (pattern[j] - L'0');
                    ++j;
                }
                if (j < pattern.size() && pattern[j] == L'}') {
                    i = j + 1;
                    out += FormatNumber(value, std::clamp(pad, 0, 9));
                    continue;
                }
            }
        }
        out += pattern[i++];
    }
    return out;
}

bool PatternUsesIndex(std::wstring_view pattern) {
    size_t i = 0;
    while (i < pattern.size()) {
        if (ConsumeToken(pattern, i, L"{name}") || ConsumeToken(pattern, i, L"{ext}"))
            continue;
        if (ConsumeToken(pattern, i, L"{n}")) return true;
        if (pattern.size() - i >= 3 && pattern.substr(i, 3) == L"{n:") {
            size_t j = i + 3;
            while (j < pattern.size() && pattern[j] >= L'0' && pattern[j] <= L'9') ++j;
            if (j < pattern.size() && pattern[j] == L'}') return true;
        }
        ++i;
    }
    return false;
}

} // namespace

bool BatchRenamePatternUsesIndex(std::wstring_view pattern) {
    return PatternUsesIndex(pattern);
}

void SplitFileName(const std::wstring& name, std::wstring& stem, std::wstring& ext) {
    stem = name;
    ext.clear();
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return;
    stem = name.substr(0, dot);
    ext = name.substr(dot);
}

bool IsValidFileName(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L"..") return false;
    if (name.back() == L' ' || name.back() == L'.') return false;
    for (const wchar_t c : name) {
        if (c < 32) return false;
        switch (c) {
        case L'<': case L'>': case L':': case L'"':
        case L'/': case L'\\': case L'|': case L'?': case L'*':
            return false;
        default:
            break;
        }
    }
    return true;
}

std::wstring ApplyBatchRenameRule(const std::wstring& name, size_t index,
                                  const BatchRenameRule& rule) {
    std::wstring working = name;
    if (!rule.find.empty()) {
        if (rule.replace_in_extension) {
            working = ReplaceInsensitive(working, rule.find, rule.replace);
        } else {
            std::wstring stem, ext;
            SplitFileName(working, stem, ext);
            working = ReplaceInsensitive(stem, rule.find, rule.replace) + ext;
        }
    }
    std::wstring stem, ext;
    SplitFileName(working, stem, ext);
    const int value = rule.start + static_cast<int>(index);
    if (!rule.pattern.empty()) {
        return ExpandPattern(rule.pattern, stem, ext, value);
    }
    stem = rule.prefix + stem + rule.suffix;
    if (rule.insert_number) {
        stem += L" (" + FormatNumber(value, rule.digits) + L")";
    }
    return stem + ext;
}

std::vector<BatchRenameItem> PreviewBatchRename(const std::vector<std::wstring>& paths,
                                                const BatchRenameRule& rule) {
    std::vector<BatchRenameItem> items;
    items.reserve(paths.size());
    std::unordered_map<std::wstring, int> new_counts;
    for (size_t i = 0; i < paths.size(); ++i) {
        BatchRenameItem item;
        item.source_path = paths[i];
        item.original_name = FileNameOf(paths[i]);
        item.new_name = ApplyBatchRenameRule(item.original_name, i, rule);
        if (item.new_name.empty()) item.status = BatchRenameStatus::Empty;
        else if (!IsValidFileName(item.new_name)) item.status = BatchRenameStatus::Invalid;
        else if (_wcsicmp(item.new_name.c_str(), item.original_name.c_str()) == 0)
            item.status = BatchRenameStatus::Unchanged;
        items.push_back(std::move(item));
        if (items.back().status == BatchRenameStatus::Ok) {
            std::wstring key = JoinName(ParentOf(paths[i]), items.back().new_name);
            for (auto& c : key) c = Fold(c);
            new_counts[key] += 1;
        }
    }
    for (auto& item : items) {
        if (item.status != BatchRenameStatus::Ok) continue;
        std::wstring key = JoinName(ParentOf(item.source_path), item.new_name);
        const std::wstring dest = key;
        for (auto& c : key) c = Fold(c);
        if (new_counts[key] > 1) {
            item.status = BatchRenameStatus::Collision;
            continue;
        }
        const std::wstring normalized = fs::NormalizePath(dest);
        if (GetFileAttributesW(normalized.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        const std::wstring source = fs::NormalizePath(item.source_path);
        if (_wcsicmp(normalized.c_str(), source.c_str()) != 0)
            item.status = BatchRenameStatus::Collision;
    }
    return items;
}

} // namespace pulse::app

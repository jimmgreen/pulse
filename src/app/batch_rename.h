// batch_rename.h — Preview-only rename rules (no filesystem writes).
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

struct BatchRenameRule {
    std::wstring find;
    std::wstring replace;
    std::wstring prefix;
    std::wstring suffix;
    std::wstring pattern; // empty = prefix/suffix/(n) legacy path
    bool insert_number = false;
    bool replace_in_extension = false;
    int start = 1;
    int digits = 0; // 0 = unpadded (legacy insert_number only)
};

enum class BatchRenameStatus {
    Ok,
    Unchanged,
    Empty,
    Invalid,
    Collision,
};

struct BatchRenameItem {
    std::wstring source_path;
    std::wstring original_name;
    std::wstring new_name;
    BatchRenameStatus status = BatchRenameStatus::Ok;
};

void SplitFileName(const std::wstring& name, std::wstring& stem, std::wstring& ext);
bool IsValidFileName(const std::wstring& name);
bool BatchRenamePatternUsesIndex(std::wstring_view pattern);
std::wstring ApplyBatchRenameRule(const std::wstring& name, size_t index,
                                  const BatchRenameRule& rule);
std::vector<BatchRenameItem> PreviewBatchRename(const std::vector<std::wstring>& paths,
                                                const BatchRenameRule& rule);

} // namespace pulse::app

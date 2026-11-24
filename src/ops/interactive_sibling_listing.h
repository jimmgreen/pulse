#pragma once
#include "source_listing.h"
#include "interactive_tree_watch.h"
#include "../common/rename_filename.h"
#include <map>

// One admission only. A completed listing can replace N immediate path queries;
// this is not a persistent absence claim and is never used for executing writes.
namespace pulse::ops {
class InteractiveSiblingListing {
public:
    enum class Kind { Absent, PlainFile, Directory, Unsafe };
    bool Read(const std::wstring& folder, const std::vector<std::wstring>& wanted,
              ULONGLONG deadline) {
        slots_.clear();
        if (wanted.size() < SourceListing::kMinSourcesPerFolder || wanted.size() > 65536) return false;
        for (const auto& leaf : wanted) {
            if (!IsRenameFilename(leaf) || leaf.find(L'~') != leaf.npos) return false;
            slots_.try_emplace(leaf, Kind::Absent);
        }
        const auto pattern = ListingPattern(folder);
        if (pattern.empty()) return false;
        // FindNextFile is not an atomic snapshot: a rename/reparse change may
        // move names behind the enumeration cursor. Only trust the complete
        // batch while the same directory has remained quiet during its walk.
        InteractiveTreeWatch watch;
        watch.Arm(1, folder);
        if (!watch.Unchanged(1, folder)) return false;
        WIN32_FIND_DATAW data{};
        // Standard includes alternate names; Basic hides them. An 8.3 match
        // is never proof of independence from the corresponding long path.
        HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoStandard, &data,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) return false;
        size_t budget = (std::min)(size_t{65536}, (std::max)(size_t{256}, wanted.size() * 8));
        bool complete = false;
        do {
            if (!budget-- || GetTickCount64() > deadline) break;
            const auto slot = slots_.find(data.cFileName);
            if (slot != slots_.end()) {
                // Case-sensitive directories can contain distinct long names
                // equal under Windows ordinal comparison. Don't pick either.
                if (slot->second != Kind::Absent || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    slot->second = Kind::Unsafe;
                else slot->second = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? Kind::Directory : Kind::PlainFile;
            }
            if (*data.cAlternateFileName) {
                const auto alias = slots_.find(data.cAlternateFileName);
                if (alias != slots_.end() && alias != slot) alias->second = Kind::Unsafe;
            }
            if (!FindNextFileW(find, &data)) {
                complete = GetLastError() == ERROR_NO_MORE_FILES;
                break;
            }
        } while (true);
        FindClose(find);
        return complete && watch.Unchanged(1, folder);
    }
    Kind Get(const std::wstring& leaf) const {
        const auto it = slots_.find(leaf);
        return it == slots_.end() ? Kind::Unsafe : it->second;
    }
private:
    struct OrdinalLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const {
            return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
        }
    };
    std::map<std::wstring, Kind, OrdinalLess> slots_;
};
} // namespace pulse::ops

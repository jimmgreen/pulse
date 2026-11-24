#pragma once
#include "../common/path_utils.h"
#include <unordered_set>
#include <vector>

namespace pulse::app {
// A completion invalidates directories; only visible views need an immediate
// enumeration. Background views retain their dirty mark until opened.
class OperationRefreshPlan {
public:
    void Add(const std::wstring& directory) {
        if (!directory.empty()) directories_.insert(path::FoldedKey(directory));
    }
    void NoteRenamedDrive(const std::wstring& source) {
        const auto plain = path::StripExtendedPathPrefix(source);
        if ((plain.size() == 2 || (plain.size() == 3 && plain[2] == L'\\')) &&
            plain[1] == L':' && ((plain[0] >= L'A' && plain[0] <= L'Z') ||
                                 (plain[0] >= L'a' && plain[0] <= L'z'))) computer_ = true;
    }
    template<class Refresh>
    void RefreshVisible(const std::vector<std::wstring>& visible, Refresh&& refresh) const {
        std::unordered_set<std::wstring> dispatched;
        for (const auto& directory : visible) {
            const auto key = path::FoldedKey(directory);
            if ((directories_.contains(key) || (computer_ && key.empty())) &&
                dispatched.insert(key).second) refresh(directory);
        }
    }
private:
    bool computer_ = false;
    std::unordered_set<std::wstring> directories_;
};
}

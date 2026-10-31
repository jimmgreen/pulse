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
    template<class Refresh>
    void RefreshVisible(const std::vector<std::wstring>& visible, Refresh&& refresh) const {
        std::unordered_set<std::wstring> dispatched;
        for (const auto& directory : visible) {
            const auto key = path::FoldedKey(directory);
            if (directories_.contains(key) && dispatched.insert(key).second) refresh(directory);
        }
    }
private:
    std::unordered_set<std::wstring> directories_;
};
}

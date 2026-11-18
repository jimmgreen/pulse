#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::shell {
// One request owns this index. Revalidate matching payloads on each lookup:
// an earlier explicit restore in the same batch may remove an ambiguous version.
class RecycleRestoreIndex {
public:
    using Entries = std::unordered_map<std::wstring, std::vector<std::wstring>>;
    template<class Load, class Exists>
    std::wstring Resolve(const std::wstring& root, const std::wstring& original,
                         Load load, Exists exists) {
        auto [it, inserted] = roots_.try_emplace(root);
        if (inserted) load(root, it->second);
        const auto found = it->second.find(original);
        if (found == it->second.end()) return {};
        std::wstring match;
        for (const auto& payload : found->second) {
            if (!exists(payload)) continue;
            if (!match.empty()) return {};
            match = payload;
        }
        return match;
    }
private:
    std::unordered_map<std::wstring, Entries> roots_;
};
}

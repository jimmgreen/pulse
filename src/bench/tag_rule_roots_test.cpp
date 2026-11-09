#include "../app/tag_rule_roots.h"
#include <cstdio>
#include <numeric>

int main() {
    using namespace pulse::tag_rules_detail;
    const std::vector<std::wstring> paths{L"C:\\A\\B", L"c:/a/", L"C:\\A2", L"C:\\A\\B\\C"};
    std::vector<int> order{0, 1, 2, 3};
    bool ok = true;
    do {
        std::vector<std::wstring> permutation;
        for (const int i : order) permutation.push_back(paths[i]);
        const auto roots = MinimalRoots(std::move(permutation));
        ok = ok && roots.size() == 2;
        for (const auto& root : roots)
            ok = ok && (SameRoot(root, L"C:\\A") || SameRoot(root, L"C:\\A2"));
    } while (std::next_permutation(order.begin(), order.end()));
    const auto children = MinimalRoots({L"C:\\A\\B", L"C:\\A\\B\\C"});
    ok = ok && children.size() == 1 && SameRoot(children.front(), L"C:\\A\\B");
    ok = ok && MinimalRoots({L"C:\\", L"C:\\A"}).size() == 1;
    std::vector<std::wstring> hits;
    for (int i = 0; i < 1024; ++i)
        ok = ok && EnqueueHit(hits, L"C:\\A\\" + std::to_wstring(i)) == EnqueueResult::Added;
    ok = ok && EnqueueHit(hits, L"c:\\a\\0") == EnqueueResult::Duplicate;
    ok = ok && EnqueueHit(hits, L"C:\\A\\overflow") == EnqueueResult::Full && hits.size() == 1024;
    hits.clear();
    ok = ok && EnqueueHit(hits, L"C:\\A\\overflow") == EnqueueResult::Added;
    std::puts(ok ? "[PASS] Root permutations, boundaries, normalization, parent removal" : "[FAIL] Roots");
    return ok ? 0 : 1;
}

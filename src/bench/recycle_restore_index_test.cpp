#include "../shell_host/recycle_restore_index.h"
#include <cstdio>
#include <unordered_set>
int main() {
    pulse::shell::RecycleRestoreIndex index;
    int loads = 0, failed = 0;
    std::unordered_set<std::wstring> present{L"a1", L"a2", L"b1"};
    auto load = [&](const std::wstring&, auto& entries) {
        ++loads;
        entries[L"a"] = {L"a1", L"a2"};
        entries[L"b"] = {L"b1", L"missing"};
    };
    auto exists = [&](const std::wstring& path) { return present.contains(path); };
    auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failed += !ok;
    };
    check(index.Resolve(L"C", L"a", load, exists).empty(), "duplicate versions remain ambiguous");
    check(index.Resolve(L"C", L"b", load, exists) == L"b1", "missing payload does not cause ambiguity");
    present.erase(L"a1"); // An explicit $R restore earlier in this batch.
    check(index.Resolve(L"C", L"a", load, exists) == L"a2", "mixed explicit and legacy restores revalidate ambiguity");
    present.erase(L"a2");
    check(index.Resolve(L"C", L"a", load, exists).empty(), "already restored payload is not reused");
    check(index.Resolve(L"C", L"unknown", load, exists).empty(), "unmatched original stays unresolved");
    for (int i = 0; i < 100; ++i) index.Resolve(L"C", L"b", load, exists);
    check(loads == 1, "one scan for all originals on one volume");
    index.Resolve(L"D", L"b", load, exists);
    check(loads == 2, "different recycle roots are indexed separately");
    return failed ? 1 : 0;
}

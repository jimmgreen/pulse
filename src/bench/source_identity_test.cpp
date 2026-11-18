#include "../ui/preview_text_identity.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    pulse::ui::PreviewTextIdentity identity;
    int table = 0, tree = 0, failures = 0;
    auto check = [&](bool ok, const char* name) {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    std::wstring shown = L"rendered", source = L"raw source";
    check(identity.Changed(&table, 1, source, shown, true), "source differs from displayed model");
    shown = source;
    check(!identity.Changed(&table, 1, source, shown, true), "unchanged source reuses identity");
    check(identity.Changed(&table, 1, L"rendered", shown), "source to rendered switches despite same owner/revision");
    shown = L"rendered";
    check(identity.Changed(&table, 1, source, shown, true), "rendered to source switches back");
    shown = source;
    check(identity.Changed(&tree, 1, L"tree source", shown, true), "different view cannot reuse source token");
    shown = L"tree source";
    check(identity.Changed(&tree, 2, L"same path new content", shown, true), "same path refresh revision updates source");
    shown = L"same path new content";
    check(!identity.Changed(&tree, 3, shown, shown, true), "new revision with equal text preserves selection");
    identity.Reset();
    check(identity.Changed(&tree, 3, L"replacement", shown, true), "clear or unavailable layout resets acknowledgement");
    identity.Reset();
    check(identity.Changed(&tree, 3, L"", shown, true), "empty source is detected");
    identity.Reset();
    check(identity.Changed(&tree, 3, L"replacement", shown, true), "retry after unavailable layout still compares content");
    if (argc > 1 && std::strcmp(argv[1], "--perf") == 0 && !failures) {
        source.assign(2 * 1024 * 1024, L'x'); shown = source;
        std::array<double, 5> old{}, fresh{};
        size_t changed = 0;
        auto run = [&](bool fast) {
            identity.Reset(); identity.Changed(&table, 7, source, shown, true);
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 500; ++i) {
                std::atomic_signal_fence(std::memory_order_seq_cst);
                changed += fast ? identity.Changed(&table, 7, source, shown, true) : source != shown;
            }
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        };
        for (size_t i = 0; i < old.size(); ++i) {
            if (i % 2) { fresh[i] = run(true); old[i] = run(false); }
            else { old[i] = run(false); fresh[i] = run(true); }
            printf("pair=%zu frames=500 bytes=%zu old_ms=%.6f source_identity_ms=%.6f\n", i,
                source.size() * sizeof(wchar_t), old[i], fresh[i]);
        }
        std::sort(old.begin(), old.end()); std::sort(fresh.begin(), fresh.end());
        printf("median old_ms=%.6f source_identity_ms=%.6f changed=%zu\n", old[2], fresh[2], changed);
        check(changed == 0, "old/new unchanged-text decisions agree");
    }
    return failures ? 1 : 0;
}

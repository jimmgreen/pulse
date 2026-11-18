#include "../app/operation_refresh.h"
#include <cstdio>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    pulse::app::OperationRefreshPlan plan;
    plan.Add(L"C:\\restored");
    plan.Add(L"c:\\RESTORED");
    plan.Add(L"pulse:recycle");
    plan.Add(L"C:\\background");
    plan.Add(L"");
    std::vector<std::wstring> refreshed;
    const auto refresh = [&](const std::wstring& path) { refreshed.push_back(path); };
    // The focused recycle tab and the restored folder in another split.
    plan.RefreshVisible({L"pulse:recycle", L"C:\\restored", L"C:\\restored", L"C:\\unrelated"}, refresh);
    check(refreshed == std::vector<std::wstring>{L"pulse:recycle", L"C:\\restored"},
          "refresh nonfocused restore destination once; skip unrelated and hidden views");
    refreshed.clear();
    plan.RefreshVisible({L"c:\\RESTORED"}, refresh);
    check(refreshed.size() == 1, "same destination in another window also refreshes");
    refreshed.clear();
    pulse::app::OperationRefreshPlan failed;
    failed.RefreshVisible({L"pulse:recycle", L"C:\\restored"}, refresh);
    check(refreshed.empty(), "no successful completions produce no destination refresh or fabricated rows");
    plan.Add(L"\\\\server\\share\\restored");
    plan.RefreshVisible({L"\\\\SERVER\\share\\restored", L"\\\\server\\share\\restored"}, refresh);
    check(refreshed.size() == 1, "UNC visible destinations deduplicate case-insensitively");
    return failures ? 1 : 0;
}

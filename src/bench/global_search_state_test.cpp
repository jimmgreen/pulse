#include "../app/global_search_result_state.h"
#include <iostream>
#include <vector>
struct Row { std::wstring path; };
int main() {
    using namespace pulse::app;
    bool ok = true;
    auto check = [&](bool value, const char* label) { std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= value; };
    for (const bool network_first : {false, true}) {
        const std::wstring chosen = network_first ? L"\\\\server\\share\\same.txt" : L"C:\\local\\same.txt";
        std::vector<Row> first{{L"C:\\a.txt"}, {chosen}, {L"C:\\z.txt"}};
        const auto position = GlobalSearchPosition::Capture(first, 1, 1);
        std::vector<Row> merged{{L"C:\\best-exact.txt"}, {L"D:\\other\\same.txt"}, {L"C:\\a.txt"}, {chosen}, {L"C:\\z.txt"}};
        const auto selected = position.Selection(merged);
        check(selected == 3 && merged[selected].path == chosen && position.First(merged) == 3,
              "provider arrival keeps selected and scroll-anchor paths despite reranking and same names");
        merged.erase(merged.begin() + 3);
        check(position.Selection(merged) == -1, "top-N eviction clears selection instead of opening another path");
        const auto cleared = GlobalSearchPosition::Capture(merged, -1, 0);
        check(cleared.Selection(first) == -1, "late provider cannot silently revive an explicitly cleared selection");
    }
    std::vector<Row> normalized{{L"C:\\Fixture\\A.txt"}};
    check(GlobalSearchPosition::Find(normalized, L"\\\\?\\c:\\fixture\\a.TXT") == 0,
          "selection uses Windows path identity including extended prefix and case");
    for (DWORD error : {DWORD{ERROR_CONNECTION_ABORTED}, DWORD{ERROR_INVALID_DATA}}) {
        for (bool network_first : {false, true}) {
            GlobalProviderStatus state;
            if (network_first) { state.Accept(true, error); state.Accept(false, 0); }
            else { state.Accept(false, 0); state.Accept(true, error); }
            check(state.Error() == error && !state.Busy(), "provider failure remains visible after either completion order");
            state.Accept(true, 0);
            check(!state.Error() && !state.Busy(), "only successful retry of failed provider clears its failure");
        }
    }
    GlobalProviderStatus timeout; timeout.Timeout(); timeout.Accept(false, 0);
    check(timeout.Error() == ERROR_TIMEOUT && !timeout.Busy(), "late local success preserves pending network timeout");
    timeout.Accept(true, 0);
    check(!timeout.Error() && !timeout.Busy(), "late network success resolves its own timeout");
    GlobalProviderStatus empty; empty.Accept(false, 0); empty.Accept(true, 0);
    check(!empty.Error() && !empty.Busy(), "two successful empty providers remain a true empty result");
    // 1236: without network roots the agent is optional and cannot fail the search.
    for (bool network_first : {false, true}) {
        GlobalProviderStatus optional; optional.ExpectNetwork(false);
        check(optional.Busy(), "optional network still waits for the local index");
        if (network_first) { optional.Accept(true, ERROR_CONNECTION_ABORTED); optional.Accept(false, 0); }
        else { optional.Accept(false, 0); optional.Accept(true, ERROR_CONNECTION_ABORTED); }
        check(!optional.Error() && !optional.Busy(),
              "unreachable network agent without network roots never reports search incomplete");
    }
    GlobalProviderStatus optional_timeout; optional_timeout.ExpectNetwork(false);
    optional_timeout.Timeout();
    check(optional_timeout.Error() == ERROR_TIMEOUT && !optional_timeout.Busy(),
          "optional network keeps a silent local index timeout visible");
    GlobalProviderStatus optional_local; optional_local.ExpectNetwork(false);
    optional_local.Accept(false, ERROR_INVALID_DATA);
    check(optional_local.Error() == ERROR_INVALID_DATA, "local index failures stay visible");
    GlobalProviderStatus expected; expected.ExpectNetwork(true);
    expected.Accept(false, 0); expected.Accept(true, ERROR_CONNECTION_ABORTED);
    check(expected.Error() == ERROR_CONNECTION_ABORTED,
          "configured network roots still report an unreachable agent");
    check(CountsProviderError(false, false) && CountsProviderError(false, true) &&
          CountsProviderError(true, true) && !CountsProviderError(true, false),
          "main window counts network errors only when network results were expected");
    return ok ? 0 : 1;
}

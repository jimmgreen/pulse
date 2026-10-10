#pragma once
#include "../common/path_utils.h"
#include <string>

namespace pulse::app {
// A network provider failure only makes a search incomplete when network
// results were expected: a configured network root or a live network walk.
// Without either, the network agent has nothing to contribute, and an agent
// that is starting, busy or blocked must not mark complete local results as
// "search incomplete" (error 1236).
inline bool CountsProviderError(bool network, bool network_expected) {
    return !network || network_expected;
}
struct GlobalProviderStatus {
    bool local_ready = false, network_ready = false;
    bool network_expected = true;
    DWORD local_error = 0, network_error = 0;
    void ExpectNetwork(bool expected) {
        network_expected = expected;
        if (!expected) { network_ready = true; network_error = 0; }
    }
    void Accept(bool network, DWORD error) {
        if (!CountsProviderError(network, network_expected)) return;
        (network ? network_ready : local_ready) = true;
        (network ? network_error : local_error) = error;
    }
    void Timeout() {
        if (!local_ready) local_error = ERROR_TIMEOUT;
        if (!network_ready) network_error = ERROR_TIMEOUT;
    }
    DWORD Error() const { return local_error ? local_error : network_error; }
    bool Busy() const { return (!local_ready && !local_error) || (!network_ready && !network_error); }
};
struct GlobalSearchPosition {
    bool had_rows = false;
    std::wstring selected_path, first_path;
    template<class Rows> static GlobalSearchPosition Capture(const Rows& rows, int selected, int first) {
        GlobalSearchPosition position; position.had_rows = !rows.empty();
        if (selected >= 0 && static_cast<size_t>(selected) < rows.size()) position.selected_path = rows[selected].path;
        if (first >= 0 && static_cast<size_t>(first) < rows.size()) position.first_path = rows[first].path;
        return position;
    }
    template<class Rows> static int Find(const Rows& rows, const std::wstring& wanted) {
        if (wanted.empty()) return -1;
        const auto normalized = path::StripExtendedPathPrefix(wanted);
        for (size_t i = 0; i < rows.size(); ++i)
            if (path::EqualInsensitive(path::StripExtendedPathPrefix(rows[i].path), normalized)) return static_cast<int>(i);
        return -1;
    }
    template<class Rows> int Selection(const Rows& rows) const { return had_rows ? Find(rows, selected_path) : 0; }
    template<class Rows> int First(const Rows& rows) const { const int found = Find(rows, first_path); return found < 0 ? 0 : found; }
};
}

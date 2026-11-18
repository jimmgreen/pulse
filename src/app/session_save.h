#pragma once

#include <string>
#include <utility>
#include "session.h"

namespace pulse::app {

// A failed save must remain dirty so a later close can retry it.
template<typename Writer>
bool SaveChangedSession(const std::wstring& json, std::wstring& saved,
                        bool force, Writer&& write) {
    if (!force && json == saved) return true;
    if (!write(json)) return false;
    saved = json;
    return true;
}

// Compare small window state and immutable history identity before constructing
// the full session. Commit comparison state only after a successful write.
template<typename Writer>
bool SaveChangedSessionSnapshot(SessionSnapshot snapshot, std::wstring& saved,
                                std::wstring& saved_layout,
                                std::shared_ptr<const std::wstring>& saved_undo,
                                bool force, Writer&& write) {
    auto undo = std::move(snapshot.undo_snapshot);
    const auto layout = SessionToJson(snapshot);
    if (!force && layout == saved_layout && undo == saved_undo) return true;
    snapshot.undo_snapshot = undo;
    const auto json = SessionToJson(snapshot);
    if (!SaveChangedSession(json, saved, force, std::forward<Writer>(write))) return false;
    saved_layout = layout;
    saved_undo = std::move(undo);
    return true;
}

} // namespace pulse::app

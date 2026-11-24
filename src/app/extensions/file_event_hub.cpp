#include "file_event_hub.h"

#include <windows.h>

#include <algorithm>
#include <utility>

namespace pulse::extensions::fileevents {
namespace {

// Plugins get the paths the user sees: "\\?\C:\x" -> "C:\x", "\\?\UNC\srv\share" -> "\\srv\share".
// Paths of MAX_PATH or more keep the prefix: without it Win32 calls would fail on them.
std::wstring Plain(const std::wstring& path) {
    if (path.size() >= MAX_PATH + 4) return path;
    if (path.size() >= 8 && path.compare(0, 4, LR"(\\?\)") == 0 && _wcsnicmp(path.c_str() + 4, L"UNC\\", 4) == 0)
        return L"\\\\" + path.substr(8);
    if (path.size() >= 6 && (path.compare(0, 4, LR"(\\?\)") == 0 || path.compare(0, 4, LR"(\\.\)") == 0) && path[5] == L':')
        return path.substr(4);
    return path;
}

std::string Utf8(const std::wstring& raw) {
    const std::wstring text = Plain(raw);
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

PulseStringView View(const std::string& text) {
    return PulseStringView{text.data(), static_cast<uint32_t>(text.size())};
}

bool SourceOnly(uint32_t kind) {
    return kind == PULSE_FILE_EVENT_RECYCLE || kind == PULSE_FILE_EVENT_DELETE || kind == PULSE_FILE_EVENT_EMPTY_RECYCLE;
}
bool DestinationOnly(uint32_t kind) {
    return kind == PULSE_FILE_EVENT_CREATE_FOLDER || kind == PULSE_FILE_EVENT_CREATE_FILE;
}

} // namespace

Hub& Hub::Instance() {
    static Hub hub;
    return hub;
}

uint64_t Hub::Add(uint64_t owner, uint32_t kinds, Listener listener) {
    Entry entry;
    entry.id = next_id_++;
    entry.owner = owner;
    entry.kinds = kinds == 0 ? PULSE_FILE_EVENT_ALL_KINDS : kinds;
    entry.listener = std::move(listener);
    entries_.push_back(std::move(entry));
    return entries_.back().id;
}

bool Hub::Remove(uint64_t id) {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.id == id; });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
}

void Hub::RemoveOwner(uint64_t owner) {
    std::erase_if(entries_, [&](const Entry& e) { return e.owner == owner; });
}

size_t Hub::CountForOwner(uint64_t owner) const {
    return static_cast<size_t>(std::count_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.owner == owner; }));
}

bool Hub::Wants(uint32_t kind) const {
    if (kind < PULSE_FILE_EVENT_COPY || kind > PULSE_FILE_EVENT_EMPTY_RECYCLE) return false;
    const uint32_t bit = PULSE_FILE_EVENT_MASK(kind);
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return (e.kinds & bit) != 0; });
}

void Hub::Publish(uint32_t kind, const std::vector<std::wstring>& sources, const std::vector<std::wstring>& destinations) {
    if (!Wants(kind)) return;
    const size_t total = std::max(sources.size(), destinations.size());
    if (total == 0 && kind != PULSE_FILE_EVENT_EMPTY_RECYCLE) return;
    const size_t count = std::min(total, kMaxItems);
    // UTF-8 copies owned here for the whole dispatch; items point into them.
    std::vector<std::string> from(count), to(count);
    for (size_t i = 0; i < count; ++i) {
        const std::wstring* src = i < sources.size() ? &sources[i] : nullptr;
        const std::wstring* dst = i < destinations.size() ? &destinations[i] : nullptr;
        if (DestinationOnly(kind)) {
            if (!dst) dst = src;
            src = nullptr;
        } else if (SourceOnly(kind)) {
            dst = nullptr;
        }
        if (src) from[i] = Utf8(*src);
        if (dst) to[i] = Utf8(*dst);
    }
    std::vector<PulseFileEventItem> items(count);
    for (size_t i = 0; i < count; ++i)
        items[i] = PulseFileEventItem{sizeof(PulseFileEventItem), 0u, View(from[i]), View(to[i])};
    PulseFileEvent event{};
    event.size = sizeof(event);
    event.kind = kind;
    event.sequence = ++sequence_;
    event.items = items.empty() ? nullptr : items.data();
    event.item_count = static_cast<uint32_t>(count);
    event.total_count = static_cast<uint32_t>(std::min<size_t>(total, UINT32_MAX));
    // Listeners may subscribe or unsubscribe while we dispatch: walk a snapshot of ids and
    // look each one up again before calling it.
    const uint32_t bit = PULSE_FILE_EVENT_MASK(kind);
    std::vector<uint64_t> ids;
    for (const auto& e : entries_)
        if (e.kinds & bit) ids.push_back(e.id);
    for (const uint64_t id : ids) {
        const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.id == id; });
        if (it == entries_.end() || !it->listener) continue;
        const auto listener = it->listener;   // a copy: the callback may remove itself
        try { listener(event); } catch (...) {}
    }
}

} // namespace pulse::extensions::fileevents
#include "column_registry.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulse::extensions::columns {

bool Utf8ToWide(std::string_view utf8, std::wstring& out) {
    out.clear();
    if (utf8.empty()) return true;
    if (utf8.size() > 0x7FFFFFFF) return false;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                               out.data(), n) == n;
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty() || wide.size() > 0x7FFFFFFF) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ProviderInfo::Key() const {
    std::wstring pkg, local;
    Utf8ToWide(package_id, pkg);
    Utf8ToWide(id, local);
    return pkg + L"/" + local;
}

namespace {

std::wstring Lower(std::wstring_view text) {
    std::wstring out(text);
    for (auto& ch : out) ch = static_cast<wchar_t>(std::towlower(ch));
    return out;
}

std::wstring_view NameOf(std::wstring_view path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.remove_suffix(1);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

int64_t TicksToUnixMs(uint64_t ticks) {
    // FILETIME counts 100 ns from 1601-01-01; Unix time starts 11644473600 s later.
    constexpr uint64_t kEpoch = 116444736000000000ull;
    if (ticks <= kEpoch) return 0;
    return static_cast<int64_t>((ticks - kEpoch) / 10000ull);
}

bool ValidText(std::string_view utf8, size_t max_bytes, std::wstring& out) {
    out.clear();
    if (utf8.size() > max_bytes) return false;
    for (const unsigned char ch : utf8)
        if (ch < 0x20u || ch == 0x7Fu) return false;
    return Utf8ToWide(utf8, out);
}

std::string_view View(PulseStringView view) {
    return view.data && view.length ? std::string_view(view.data, view.length) : std::string_view{};
}

} // namespace

bool ParseExtensions(std::wstring_view text, std::vector<std::wstring>& out) {
    out.clear();
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L';', start);
        if (end == std::wstring_view::npos) end = text.size();
        std::wstring_view part = text.substr(start, end - start);
        while (!part.empty() && part.front() == L' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == L' ') part.remove_suffix(1);
        if (!part.empty()) {
            std::wstring ext = Lower(part);
            if (ext.front() != L'.') ext.insert(ext.begin(), L'.');
            if (ext.size() < 2 || ext.size() > 32) return false;
            for (size_t i = 1; i < ext.size(); ++i) {
                const wchar_t c = ext[i];
                if (c < 0x20 || c == L'.' || c == L'\\' || c == L'/' || c == L':' || c == L'*' ||
                    c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|')
                    return false;
            }
            if (std::find(out.begin(), out.end(), ext) == out.end()) out.push_back(std::move(ext));
            if (out.size() > 64) return false;
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    return true;
}

bool Applies(const ProviderInfo& info, const Item& item) {
    const uint32_t applies = info.applies_to ? info.applies_to : PULSE_COLUMN_APPLIES_ALL;
    if (item.is_dir) return (applies & PULSE_COLUMN_APPLIES_FOLDERS) != 0;
    if ((applies & PULSE_COLUMN_APPLIES_FILES) == 0) return false;
    if (info.extensions.empty()) return true;
    const std::wstring_view name = NameOf(item.path);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0) return false;
    const std::wstring ext = Lower(name.substr(dot));
    return std::find(info.extensions.begin(), info.extensions.end(), ext) != info.extensions.end();
}

struct Registry::Provider {
    // info.title changes under Registry::mutex_; the rest is fixed.
    ProviderInfo info;
    PulseColumnQueryFn query = nullptr;
    void* user = nullptr;
    std::function<uint32_t(std::string_view)> resolve_icon;
    std::shared_ptr<std::atomic<uint32_t>> session_active_calls;
    std::function<void()> on_call_finished;
    std::shared_ptr<std::atomic<uint64_t>> revision;

    struct Cached {
        uint64_t size = 0;
        uint64_t mtime = 0;
        CellValue cell;
        BadgeValue badge;
    };

    mutable std::mutex mutex;
    std::condition_variable cv;
    mutable std::condition_variable idle_cv;
    std::deque<Item> queue;                    // newest at the back, asked first
    std::unordered_set<std::wstring> waiting;  // queued or running paths
    std::unordered_map<std::wstring, Cached> cache;
    size_t running = 0;
    uint64_t epoch = 0;                        // invalidate: drops older batches
    bool removed = false;
    bool worker = false;
};

namespace {

struct BatchState {
    Kind kind = Kind::Column;
    std::function<bool()> cancelled;
    std::vector<std::optional<CellValue>> cells;
    std::vector<std::optional<std::pair<std::string, uint32_t>>> badges;   // icon name, tone
};

uint32_t PULSE_CALL BatchCancelled(void* context) {
    auto* state = static_cast<BatchState*>(context);
    try {
        return state && state->cancelled && state->cancelled() ? 1u : 0u;
    } catch (...) {
        return 1u;
    }
}

PulseStatus PULSE_CALL BatchSetText(void* context, uint32_t index, const PulseColumnValue* value) {
    auto* state = static_cast<BatchState*>(context);
    if (!state || !value || value->size < sizeof(PulseColumnValue)) return PULSE_ERROR_ARGUMENT;
    if (state->kind != Kind::Column) return PULSE_ERROR_STATE;
    if (index >= state->cells.size() || state->cells[index]) return PULSE_ERROR_ARGUMENT;
    if (value->tone > PULSE_COLUMN_TONE_DANGER) return PULSE_ERROR_ARGUMENT;
    if (value->text.length && !value->text.data) return PULSE_ERROR_ARGUMENT;
    try {
        CellValue cell;
        if (!ValidText(View(value->text), kMaxTextBytes, cell.text)) return PULSE_ERROR_ARGUMENT;
        cell.tone = value->tone;
        state->cells[index] = std::move(cell);
        return PULSE_OK;
    } catch (...) {
        return PULSE_ERROR_STATE;
    }
}

PulseStatus PULSE_CALL BatchSetBadge(void* context, uint32_t index, const PulseBadgeValue* value) {
    auto* state = static_cast<BatchState*>(context);
    if (!state || !value || value->size < sizeof(PulseBadgeValue)) return PULSE_ERROR_ARGUMENT;
    if (state->kind != Kind::Badge) return PULSE_ERROR_STATE;
    if (index >= state->badges.size() || state->badges[index]) return PULSE_ERROR_ARGUMENT;
    if (value->tone > PULSE_COLUMN_TONE_DANGER) return PULSE_ERROR_ARGUMENT;
    if (value->icon.length && !value->icon.data) return PULSE_ERROR_ARGUMENT;
    const std::string_view icon = View(value->icon);
    if (icon.size() > 96) return PULSE_ERROR_ARGUMENT;
    for (const unsigned char ch : icon)
        if (ch <= 0x20u || ch >= 0x7Fu) return PULSE_ERROR_ARGUMENT;
    try {
        state->badges[index] = std::make_pair(std::string(icon), value->tone);
        return PULSE_OK;
    } catch (...) {
        return PULSE_ERROR_STATE;
    }
}

} // namespace

Registry& Registry::Instance() {
    static Registry* registry = new Registry();   // workers may outlive static destruction
    return *registry;
}

Registry::~Registry() {
    std::map<uint64_t, std::shared_ptr<Provider>> providers;
    {
        std::lock_guard lock(mutex_);
        providers.swap(providers_);
    }
    for (auto& [handle, provider] : providers) {
        std::lock_guard lock(provider->mutex);
        provider->removed = true;
        provider->cv.notify_all();
    }
}

std::shared_ptr<Registry::Provider> Registry::Find(uint64_t handle) const {
    std::lock_guard lock(mutex_);
    const auto it = providers_.find(handle);
    return it == providers_.end() ? nullptr : it->second;
}

PulseStatus Registry::Register(Registration reg, uint64_t& out_handle) {
    out_handle = 0;
    if (reg.package_id.empty() || reg.id.empty() || !reg.query) return PULSE_ERROR_ARGUMENT;
    auto provider = std::make_shared<Provider>();
    provider->info.kind = reg.kind;
    provider->info.package_id = std::move(reg.package_id);
    provider->info.id = std::move(reg.id);
    provider->info.title = std::move(reg.title);
    provider->info.width_dip = std::clamp(reg.width_dip, 48.0f, 480.0f);
    provider->info.align_end = reg.align_end;
    provider->info.applies_to = reg.applies_to ? (reg.applies_to & PULSE_COLUMN_APPLIES_ALL) : PULSE_COLUMN_APPLIES_ALL;
    provider->info.extensions = std::move(reg.extensions);
    provider->query = reg.query;
    provider->user = reg.user;
    provider->resolve_icon = std::move(reg.resolve_icon);
    provider->session_active_calls = std::move(reg.session_active_calls);
    provider->on_call_finished = std::move(reg.on_call_finished);
    {
        std::lock_guard lock(mutex_);
        size_t same_kind = 0;
        for (const auto& [handle, existing] : providers_) {
            if (existing->info.package_id != provider->info.package_id) continue;
            if (existing->info.id == provider->info.id) return PULSE_ERROR_STATE;
            if (existing->info.kind == provider->info.kind) ++same_kind;
        }
        const size_t limit = provider->info.kind == Kind::Column ? kMaxColumnsPerPackage : kMaxBadgesPerPackage;
        if (same_kind >= limit) return PULSE_ERROR_STATE;
        provider->revision = revision_;
        out_handle = next_handle_++;
        provider->info.handle = out_handle;
        providers_.emplace(out_handle, provider);
    }
    Bump();
    return PULSE_OK;
}

bool Registry::Unregister(uint64_t handle, bool wait) {
    std::shared_ptr<Provider> provider;
    {
        std::lock_guard lock(mutex_);
        const auto it = providers_.find(handle);
        if (it == providers_.end()) return false;
        provider = std::move(it->second);
        providers_.erase(it);
    }
    {
        std::lock_guard lock(provider->mutex);
        provider->removed = true;
        provider->queue.clear();
        provider->cache.clear();
        provider->cv.notify_all();
        provider->idle_cv.notify_all();
    }
    if (wait) {
        std::unique_lock lock(provider->mutex);
        provider->idle_cv.wait(lock, [&] { return provider->running == 0; });
    }
    Bump();
    return true;
}

bool Registry::SetTitle(uint64_t handle, std::wstring title) {
    {
        std::lock_guard lock(mutex_);
        const auto it = providers_.find(handle);
        if (it == providers_.end()) return false;
        if (it->second->info.title == title) return true;
        it->second->info.title = std::move(title);
    }
    Bump();
    return true;
}

bool Registry::Invalidate(uint64_t handle, std::wstring_view path) {
    const auto provider = Find(handle);
    if (!provider) return false;
    {
        std::lock_guard lock(provider->mutex);
        if (path.empty()) {
            provider->cache.clear();
        } else {
            // The listing may spell the path with other letter case.
            const std::wstring wanted = Lower(path);
            for (auto it = provider->cache.begin(); it != provider->cache.end();) {
                if (Lower(it->first) == wanted) it = provider->cache.erase(it);
                else ++it;
            }
        }
        ++provider->epoch;
    }
    Bump();
    return true;
}

bool Registry::Owns(uint64_t handle, std::string_view package_id) const {
    std::lock_guard lock(mutex_);
    const auto it = providers_.find(handle);
    return it != providers_.end() && it->second->info.package_id == package_id;
}

std::vector<ProviderInfo> Registry::List(Kind kind) const {
    std::vector<ProviderInfo> out;
    std::lock_guard lock(mutex_);
    for (const auto& [handle, provider] : providers_)   // handles grow: registration order
        if (provider->info.kind == kind) out.push_back(provider->info);
    return out;
}

template <class Value, class Out>
Lookup Registry::LookupValue(uint64_t handle, Kind kind, const Item& item, Out& out) {
    const auto provider = Find(handle);
    if (!provider || provider->info.kind != kind || item.path.empty()) return Lookup::NotApplicable;
    if (!Applies(provider->info, item)) return Lookup::NotApplicable;
    bool start = false;
    {
        std::lock_guard lock(provider->mutex);
        if (provider->removed) return Lookup::NotApplicable;
        if (const auto it = provider->cache.find(item.path); it != provider->cache.end()) {
            if (it->second.size == item.size && it->second.mtime == item.mtime) {
                if constexpr (std::is_same_v<Value, CellValue>) out = it->second.cell;
                else out = it->second.badge;
                return Lookup::Ready;
            }
        }
        if (provider->waiting.contains(item.path)) return Lookup::Pending;
        provider->queue.push_back(item);
        provider->waiting.insert(item.path);
        // Rows scrolled past long ago are not worth asking for any more.
        while (provider->queue.size() > kMaxQueued) {
            provider->waiting.erase(provider->queue.front().path);
            provider->queue.pop_front();
        }
        if (!provider->worker) {
            provider->worker = true;
            start = true;
        }
        provider->cv.notify_one();
    }
    if (start) {
        try {
            std::thread(&Registry::Run, this, provider).detach();
        } catch (...) {
            std::lock_guard lock(provider->mutex);
            provider->worker = false;
        }
    }
    return Lookup::Pending;
}

Lookup Registry::Cell(uint64_t handle, const Item& item, CellValue& out) {
    return LookupValue<CellValue>(handle, Kind::Column, item, out);
}

Lookup Registry::Badge(uint64_t handle, const Item& item, BadgeValue& out) {
    return LookupValue<BadgeValue>(handle, Kind::Badge, item, out);
}

size_t Registry::Pending(uint64_t handle) const {
    const auto provider = Find(handle);
    if (!provider) return 0;
    std::lock_guard lock(provider->mutex);
    return provider->queue.size() + provider->running;
}

bool Registry::WaitUntilIdle(uint64_t handle, uint32_t timeout_ms) const {
    const auto provider = Find(handle);
    if (!provider) return true;
    std::unique_lock lock(provider->mutex);
    return provider->idle_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
        return provider->removed || (provider->queue.empty() && provider->running == 0);
    });
}

// One worker per provider. It only touches the provider (shared) and the
// revision counter, so it may outlive the registration.
void Registry::Run(Registry* /*self*/, std::shared_ptr<Provider> p) {
    for (;;) {
        std::vector<Item> batch;
        uint64_t epoch = 0;
        {
            std::unique_lock lock(p->mutex);
            const bool woke = p->cv.wait_for(lock, std::chrono::seconds(30),
                                             [&] { return p->removed || !p->queue.empty(); });
            if (p->removed || !woke) {
                // Removed, or idle for a while: the next request starts a new worker.
                p->worker = false;
                p->idle_cv.notify_all();
                return;
            }
            while (!p->queue.empty() && batch.size() < kMaxBatch) {
                batch.push_back(std::move(p->queue.back()));
                p->queue.pop_back();
            }
            p->running = batch.size();
            epoch = p->epoch;
            // Counted under the lock that Unregister takes: once the session
            // sees no active calls after unregistering, none can start.
            if (p->session_active_calls) p->session_active_calls->fetch_add(1, std::memory_order_acq_rel);
        }

        std::vector<std::string> paths(batch.size());
        std::vector<PulseColumnItem> items(batch.size());
        for (size_t i = 0; i < batch.size(); ++i) {
            paths[i] = WideToUtf8(batch[i].path);
            PulseColumnItem& item = items[i];
            item.size = sizeof(PulseColumnItem);
            item.flags = batch[i].is_dir ? PULSE_COLUMN_ITEM_DIRECTORY : 0u;
            item.path = PulseStringView{paths[i].data(), static_cast<uint32_t>(paths[i].size())};
            item.size_bytes = batch[i].is_dir ? 0 : batch[i].size;
            item.modified_unix_ms = TicksToUnixMs(batch[i].mtime);
        }
        BatchState state;
        state.kind = p->info.kind;
        state.cancelled = [p, epoch] {
            std::lock_guard lock(p->mutex);
            return p->removed || p->epoch != epoch;
        };
        state.cells.resize(batch.size());
        state.badges.resize(batch.size());
        PulseColumnBatch call{};
        call.size = sizeof(PulseColumnBatch);
        call.count = static_cast<uint32_t>(batch.size());
        call.items = items.data();
        call.context = &state;
        call.is_cancelled = BatchCancelled;
        call.set_text = BatchSetText;
        call.set_badge = BatchSetBadge;
        PulseStatus status = PULSE_ERROR_STATE;
        try {
            status = p->query(p->user, &call);
        } catch (...) {
            status = PULSE_ERROR_STATE;
        }
        if (p->session_active_calls) p->session_active_calls->fetch_sub(1, std::memory_order_acq_rel);

        // Badge icons resolve outside the provider lock (the icon registry has its own).
        std::vector<BadgeValue> badges(batch.size());
        if (status == PULSE_OK && state.kind == Kind::Badge) {
            for (size_t i = 0; i < batch.size(); ++i) {
                if (!state.badges[i] || state.badges[i]->first.empty()) continue;
                uint32_t icon = 0;
                try {
                    icon = p->resolve_icon ? p->resolve_icon(state.badges[i]->first) : 0u;
                } catch (...) {
                    icon = 0;
                }
                badges[i].icon = icon;
                badges[i].tone = state.badges[i]->second;
                badges[i].shown = icon != 0;   // unknown names (and "default") draw nothing
            }
        }
        {
            std::lock_guard lock(p->mutex);
            const bool keep = !p->removed && p->epoch == epoch;
            if (keep && p->cache.size() + batch.size() > kMaxCachedPerProvider) p->cache.clear();
            for (size_t i = 0; i < batch.size(); ++i) {
                p->waiting.erase(batch[i].path);
                if (!keep) continue;
                Provider::Cached cached;
                cached.size = batch[i].size;
                cached.mtime = batch[i].mtime;
                // A failed batch reads as blank until the file or the plugin changes.
                if (status == PULSE_OK && state.cells[i]) cached.cell = std::move(*state.cells[i]);
                cached.badge = badges[i];
                p->cache[batch[i].path] = std::move(cached);
            }
            p->running = 0;
            p->idle_cv.notify_all();
        }
        if (p->revision) p->revision->fetch_add(1, std::memory_order_acq_rel);
        if (p->on_call_finished) {
            try { p->on_call_finished(); } catch (...) {}
        }
    }
}

} // namespace pulse::extensions::columns

#pragma once
// Extension columns and icon badges (pulse.columns): the providers plugins
// registered, the values they returned, and the workers that ask them.
// Windowless and thread-safe: the UI asks for values while painting
// (Cell / Badge return at once and queue a miss), one worker per provider
// calls the plugin in batches, and Revision() moves when values arrive.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../../../sdk/native/include/pulse/columns.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::extensions::columns {

enum class Kind : uint8_t { Column, Badge };

inline constexpr size_t kMaxColumnsPerPackage = 4;
inline constexpr size_t kMaxBadgesPerPackage = 4;
inline constexpr uint32_t kMaxBatch = 64;
inline constexpr size_t kMaxTextBytes = 256;
inline constexpr size_t kMaxQueued = 512;
inline constexpr size_t kMaxCachedPerProvider = 20000;

struct ProviderInfo {
    uint64_t handle = 0;
    Kind kind = Kind::Column;
    std::string package_id;
    std::string id;
    std::wstring title;
    float width_dip = 120.0f;
    bool align_end = false;
    uint32_t applies_to = PULSE_COLUMN_APPLIES_ALL;
    std::vector<std::wstring> extensions;   // lower case, with the dot
    // "<package_id>/<id>": what the hidden list (AppPrefs) names.
    std::wstring Key() const;
};

struct Registration {
    Kind kind = Kind::Column;
    std::string package_id;
    std::string id;
    std::wstring title;
    float width_dip = 120.0f;
    bool align_end = false;
    uint32_t applies_to = PULSE_COLUMN_APPLIES_ALL;
    std::vector<std::wstring> extensions;
    PulseColumnQueryFn query = nullptr;
    void* user = nullptr;
    // Badges: icon name -> PulseIconId (0 = unknown); called on the worker.
    std::function<uint32_t(std::string_view icon)> resolve_icon;
    std::shared_ptr<std::atomic<uint32_t>> session_active_calls;
    std::function<void()> on_call_finished;
};

struct Item {
    std::wstring path;          // as the listing spells it
    bool is_dir = false;
    uint64_t size = 0;
    uint64_t mtime = 0;         // FILETIME ticks (UTC), 0 = unknown
};

struct CellValue {
    std::wstring text;
    uint32_t tone = PULSE_COLUMN_TONE_DEFAULT;
};

struct BadgeValue {
    uint32_t icon = 0;          // PulseIconId; meaningful only when shown
    bool shown = false;
    uint32_t tone = PULSE_COLUMN_TONE_DEFAULT;
};

enum class Lookup : uint8_t { Ready, Pending, NotApplicable };

// "c:\a\b.JPG" applies to {".jpg"}: the item matches the provider's filters.
bool Applies(const ProviderInfo& info, const Item& item);
// ".jpg;.PNG; tif" -> {".jpg", ".png", ".tif"}; false on a malformed list.
bool ParseExtensions(std::wstring_view text, std::vector<std::wstring>& out);

class Registry {
public:
    static Registry& Instance();

    Registry() = default;
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // PULSE_ERROR_STATE: same package already has this id (any kind) or is
    // at its limit for the kind.
    PulseStatus Register(Registration reg, uint64_t& out_handle);
    // wait: also block until a query call already running for it returns
    // (the provider's user data may be freed right after).
    bool Unregister(uint64_t handle, bool wait = false);
    bool SetTitle(uint64_t handle, std::wstring title);
    // Empty path: every value of the provider.
    bool Invalidate(uint64_t handle, std::wstring_view path);
    bool Owns(uint64_t handle, std::string_view package_id) const;

    // Registration order.
    std::vector<ProviderInfo> List(Kind kind) const;

    // Cached value of this exact file version, or Pending (queued) /
    // NotApplicable (filters exclude it). Never blocks on the plugin.
    Lookup Cell(uint64_t handle, const Item& item, CellValue& out);
    Lookup Badge(uint64_t handle, const Item& item, BadgeValue& out);

    // Moves whenever values arrive or providers change.
    uint64_t Revision() const noexcept { return revision_->load(std::memory_order_acquire); }
    // Queued plus running items of a provider (tests and diagnostics).
    size_t Pending(uint64_t handle) const;
    // Blocks until the provider has nothing queued or running.
    bool WaitUntilIdle(uint64_t handle, uint32_t timeout_ms = 5000) const;

private:
    struct Provider;
    std::shared_ptr<Provider> Find(uint64_t handle) const;
    template <class Value, class Out>
    Lookup LookupValue(uint64_t handle, Kind kind, const Item& item, Out& out);
    static void Run(Registry* self, std::shared_ptr<Provider> provider);
    void Bump() noexcept { revision_->fetch_add(1, std::memory_order_acq_rel); }

    mutable std::mutex mutex_;
    uint64_t next_handle_ = 1;
    std::map<uint64_t, std::shared_ptr<Provider>> providers_;
    // Shared with the workers, which may outlive a registration (or a test registry).
    std::shared_ptr<std::atomic<uint64_t>> revision_ = std::make_shared<std::atomic<uint64_t>>(1);
};

// UTF-8 -> UTF-16 and back; false / empty on invalid input.
bool Utf8ToWide(std::string_view utf8, std::wstring& out);
std::string WideToUtf8(std::wstring_view wide);

} // namespace pulse::extensions::columns

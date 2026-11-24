#include "previews_adapter.h"
#include "crash_guard.h"
#include "ui_model.h"
#include "worker_budget.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include "../../../preview_host/archive_serialization.h"
namespace pulse::extensions::native {
namespace {
std::atomic<uint64_t> next_id{0};
std::wstring Wide(PulseStringView text, size_t limit) {
    if (text.length > limit || (text.length && !text.data)) throw std::invalid_argument("text");
    if (!text.length) return {};
    if (std::memchr(text.data, 0, text.length)) throw std::invalid_argument("nul");
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data, static_cast<int>(text.length), nullptr, 0);
    if (!count) throw std::invalid_argument("utf8");
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data, static_cast<int>(text.length), result.data(), count);
    return result;
}
bool AbsoluteFilePath(std::wstring_view path) {
    if (path.empty() || path.size() > 32768 || path.find(L'\0') != std::wstring_view::npos) return false;
    std::wstring normalized(path);
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    path = normalized;
    if (path.starts_with(L"\\\\?\\UNC\\")) { normalized = L"\\\\" + normalized.substr(8); path = normalized; }
    else if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
    if (path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && path[2] == L'\\') return path.find(L':', 2) == std::wstring_view::npos;
    if (!path.starts_with(L"\\\\") || path.find(L':') != std::wstring_view::npos) return false;
    path.remove_prefix(2);
    const auto server = path.find(L'\\');
    if (server == std::wstring_view::npos || !server || path.substr(0, server) == L"." || path.substr(0, server) == L"?") return false;
    path.remove_prefix(server + 1);
    const auto share = path.find(L'\\');
    return share != std::wstring_view::npos && share > 0 && share + 1 < path.size() &&
        path.substr(0, share) != L"." && path.substr(0, share) != L"..";
}
std::wstring LowerExtension(const std::wstring& path) {
    const auto dot = path.find_last_of(L'.');
    const auto slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    auto extension = path.substr(dot);
    for (auto& c : extension) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return extension;
}
// Box filter of premultiplied BGRA down to `edge` on the longer side; compacts the stride.
void ScaleToEdge(const uint8_t* source, uint32_t width, uint32_t height, uint32_t stride,
                 uint32_t edge, std::vector<uint8_t>& out, uint32_t& out_width, uint32_t& out_height) {
    const uint32_t longest = (std::max)(width, height);
    if (longest <= edge) {
        out_width = width; out_height = height;
        out.resize(static_cast<size_t>(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(out.data() + static_cast<size_t>(y) * width * 4, source + static_cast<size_t>(y) * stride, width * 4u);
        return;
    }
    out_width = (std::max)(1u, static_cast<uint32_t>(static_cast<uint64_t>(width) * edge / longest));
    out_height = (std::max)(1u, static_cast<uint32_t>(static_cast<uint64_t>(height) * edge / longest));
    out.resize(static_cast<size_t>(out_width) * out_height * 4);
    for (uint32_t y = 0; y < out_height; ++y) {
        const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * height / out_height);
        const uint32_t y1 = (std::max)(y0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * height / out_height));
        for (uint32_t x = 0; x < out_width; ++x) {
            const uint32_t x0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * width / out_width);
            const uint32_t x1 = (std::max)(x0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * width / out_width));
            uint64_t sums[4]{};
            for (uint32_t sy = y0; sy < y1; ++sy) {
                const uint8_t* row = source + static_cast<size_t>(sy) * stride;
                for (uint32_t sx = x0; sx < x1; ++sx) for (size_t c = 0; c < 4; ++c) sums[c] += row[sx * 4 + c];
            }
            const uint64_t count = static_cast<uint64_t>(x1 - x0) * (y1 - y0);
            uint8_t* pixel = out.data() + (static_cast<size_t>(y) * out_width + x) * 4;
            for (size_t c = 0; c < 4; ++c) pixel[c] = static_cast<uint8_t>((sums[c] + count / 2) / count);
        }
    }
}
std::string Utf8(const std::wstring& text) {
    if (text.empty() || text.size() > 32768 || text.find(L'\0') != std::wstring::npos) throw std::invalid_argument("path");
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::invalid_argument("path");
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
}
struct PreviewsAdapter::State : std::enable_shared_from_this<State> {
    // Thumbnail calls in flight for one provider (Unregister reports STATE until they return).
    struct ThumbSlot { std::atomic<int> active{0}; std::atomic<bool> retired{false}; };
    struct Provider {
        uint64_t id; std::wstring extension; PulsePreviewFn callback; void* user; bool retiring = false;
        std::shared_ptr<ThumbSlot> thumb;  // set when registered with PULSE_PREVIEW_PROVIDER_THUMBNAIL
    };
    struct ThumbEntry {
        std::weak_ptr<State> state;
        uint64_t id = 0;
        std::wstring extension;
        PulsePreviewFn callback = nullptr;
        void* user = nullptr;
        std::shared_ptr<ThumbSlot> slot;
        std::string package_id;
    };
    // Unlike `registry` (owner UI thread only) this one is read from thumbnail threads.
    static inline std::mutex thumb_mutex;
    static inline std::vector<ThumbEntry> thumbs;
    static void RemoveThumb(uint64_t id) {
        std::lock_guard lock(thumb_mutex);
        std::erase_if(thumbs, [id](const ThumbEntry& entry) { return entry.id == id || entry.state.expired(); });
    }
    struct Job {
        std::wstring path;
        std::string utf8;
        uint64_t modified = 0, size = 0, sequence = 0;
        uint32_t pixels = 0;
        HWND notify = nullptr;
        Provider provider{};
        std::atomic<bool> cancelled{false}, done{false};
        bool started = false;
        DWORD worker = 0;
        std::mutex mutex;
        std::shared_ptr<PreviewResult> result;
        PulseStatus error = PULSE_ERROR_STATE;
        std::function<bool()> poll;  // thumbnail jobs: closed / retired / caller cancelled
        static uint32_t PULSE_CALL Cancelled(void* context) {
            auto& job = *static_cast<Job*>(context);
            if (job.cancelled.load()) return 1u;
            try { return job.poll && job.poll() ? 1u : 0u; } catch (...) { return 1u; }
        }
        static PulseStatus PULSE_CALL Publish(void* context, const PulsePreviewResult* source) {
            auto& job = *static_cast<Job*>(context);
            if (job.cancelled || GetCurrentThreadId() != job.worker) return PULSE_ERROR_STATE;
            if (!source || source->size < sizeof(*source)) return PULSE_ERROR_ARGUMENT;
            try {
                auto result = std::make_shared<PreviewResult>(); result->kind = source->kind;
                if (source->kind == PULSE_PREVIEW_ARCHIVE) {
                    if (source->entry_count > 4096 || (source->entry_count && !source->entries) || source->incomplete > 1) return PULSE_ERROR_ARGUMENT;
                    result->format = Wide(source->format, 128);
                    if (result->format.empty() || std::any_of(result->format.begin(), result->format.end(), [](wchar_t c) { return c <= 31 || c == 127; }))
                        return PULSE_ERROR_ARGUMENT;
                    result->incomplete = source->incomplete != 0;
                    size_t bytes = source->format.length;
                    for (uint32_t i = 0; i < source->entry_count; ++i) {
                        const auto& entry = source->entries[i]; bytes += entry.path.length;
                        if (bytes > 4 * 1024 * 1024 || !entry.path.length || (entry.flags & ~3u)) return PULSE_ERROR_ARGUMENT;
                        result->entries.push_back({Wide(entry.path, 32768), entry.size_bytes, entry.packed_bytes, entry.modified_ticks,
                            (entry.flags & PULSE_PREVIEW_ENTRY_DIRECTORY) != 0, (entry.flags & PULSE_PREVIEW_ENTRY_ENCRYPTED) != 0});
                    }
                    std::vector<preview::ArchiveListingEntry> archive;
                    uint64_t packed = 0;
                    for (const auto& entry : result->entries) {
                        preview::ArchiveListingEntry row;
                        row.path = entry.path; row.size = entry.size; row.packed = entry.packed;
                        row.dir = entry.directory; row.encrypted = entry.encrypted;
                        FILETIME time{static_cast<DWORD>(entry.modified), static_cast<DWORD>(entry.modified >> 32)};
                        row.has_time = entry.modified && FileTimeToSystemTime(&time, &row.time);
                        packed = entry.packed > UINT64_MAX - packed ? UINT64_MAX : packed + entry.packed;
                        archive.push_back(std::move(row));
                    }
                    preview::archive_serialization::Listing listing;
                    listing.entries = std::move(archive); listing.format = result->format;
                    listing.incomplete = result->incomplete; listing.packed_total = packed; listing.has_packed = true;
                    result->archive_text = std::make_shared<const std::wstring>(preview::archive_serialization::Serialize(listing));
                    result->incomplete = listing.incomplete;
                } else if (source->kind == PULSE_PREVIEW_BITMAP) {
                    if (!source->width || !source->height || source->width > 4096 || source->height > 4096 ||
                        source->stride < source->width * 4 || !source->pixels || source->byte_count > 16 * 1024 * 1024 ||
                        static_cast<uint64_t>(source->stride) * source->height != source->byte_count) return PULSE_ERROR_ARGUMENT;
                    result->width = source->width; result->height = source->height; result->stride = source->stride;
                    result->pixels.assign(source->pixels, source->pixels + source->byte_count);
                    for (uint32_t y = 0; y < source->height; ++y) for (uint32_t x = 0; x < source->width; ++x) {
                        const auto* pixel = result->pixels.data() + static_cast<size_t>(y) * source->stride + x * 4;
                        if (pixel[0] > pixel[3] || pixel[1] > pixel[3] || pixel[2] > pixel[3]) return PULSE_ERROR_ARGUMENT;
                    }
                } else if (source->kind == PULSE_PREVIEW_UI_MODEL) {
                    if (!source->model || source->model->size < PULSE_UI_MODEL_TREE_SIZE_V1) return PULSE_ERROR_ARGUMENT;
                    if (source->model->command_count) return PULSE_ERROR_UNAVAILABLE;
                    auto model = std::make_shared<UiTree>();
                    std::set<std::string> commands;
                    std::string error;
                    const auto status = ConvertUiModel(source->model, *model, commands, error);
                    if (status != PULSE_OK) return status;
                    if (!commands.empty()) return PULSE_ERROR_UNAVAILABLE;
                    result->model = std::move(model);
                } else return PULSE_ERROR_UNAVAILABLE;
                std::lock_guard lock(job.mutex);
                if (job.cancelled || job.result) return PULSE_ERROR_STATE;
                job.result = std::move(result); return PULSE_OK;
            } catch (...) { return PULSE_ERROR_ARGUMENT; }
        }
    };
    static inline thread_local std::vector<std::weak_ptr<State>> registry;
    HWND owner;
    DWORD thread = GetCurrentThreadId();
    std::function<bool()> live;
    std::function<void()> wake;
    std::string package_id;
    std::atomic<bool> closed{false};
    std::atomic<unsigned> workers{0};
    std::atomic<size_t> queued_count{0};
    std::atomic<uint64_t> waiter_id{0};
    uint64_t next_sequence = 0;
    std::map<uint64_t, Provider> providers;
    std::map<uint64_t, std::shared_ptr<Job>> surfaces;
    std::vector<std::weak_ptr<Job>> running;
    State(HWND window, std::function<bool()> check, std::function<void()> on_wake, std::string pkg_id)
        : owner(window), live(std::move(check)), wake(std::move(on_wake)), package_id(std::move(pkg_id)) {}
    ~State() { UnregisterPoolWaiter(); }
    void UnregisterPoolWaiter() noexcept {
        const uint64_t id = waiter_id.exchange(0, std::memory_order_acq_rel);
        if (id != 0) GlobalPreviewWorkerPool().UnregisterWaiter(id);
    }
    void NotifyWake() const noexcept {
        if (!wake) return;
        try { wake(); } catch (...) {}
    }
    bool Ready() const { return owner && !closed.load(std::memory_order_relaxed) && GetCurrentThreadId() == thread && live && live(); }
    size_t CountUnstarted() const noexcept {
        size_t count = 0;
        for (const auto& [surface, job] : surfaces) {
            (void)surface;
            if (!job->started && !job->done && !job->cancelled.load(std::memory_order_acquire)) ++count;
        }
        return count;
    }
    void CancelSurface(uint64_t surface) {
        auto it = surfaces.find(surface); if (it == surfaces.end()) return;
        it->second->cancelled = true;
        if (it->second->notify) InvalidateRect(it->second->notify, nullptr, FALSE);
        surfaces.erase(it);
        queued_count.store(CountUnstarted(), std::memory_order_seq_cst);
    }
    void Close() {
        if (closed.load(std::memory_order_relaxed)) return;
        closed.store(true, std::memory_order_release);
        queued_count.store(0, std::memory_order_seq_cst);
        UnregisterPoolWaiter();
        for (auto& [surface, job] : surfaces) {
            (void)surface;
            job->cancelled = true;
            if (job->notify) InvalidateRect(job->notify, nullptr, FALSE);
        }
        for (auto& weak : running) if (auto job = weak.lock()) job->cancelled = true;
        for (auto& [id, provider] : providers) if (provider.thumb) { provider.thumb->retired = true; RemoveThumb(id); }
        surfaces.clear(); providers.clear();
    }
    void StartWithPermit(const std::shared_ptr<Job>& job, WorkerPermit permit) {
        std::erase_if(running, [](const auto& weak) { return weak.expired(); }); running.push_back(job);
        auto self = shared_from_this();
        job->started = true; ++workers;
        try {
            std::thread([self, job, permit = std::move(permit)]() mutable {
                job->worker = GetCurrentThreadId();
                PulseStatus status = PULSE_ERROR_STATE;
                try {
                    if (!job->cancelled) {
                        const crash_guard::CallScope call_scope(self->package_id.c_str());
                        PulsePreviewRequest request{sizeof(request), {job->utf8.data(), static_cast<uint32_t>(job->utf8.size())},
                            job->modified, job->size, job->pixels, job.get(), Job::Cancelled, Job::Publish};
                        status = job->provider.callback(job->provider.user, &request);
                    }
                } catch (...) { status = PULSE_ERROR_STATE; }
                { std::lock_guard lock(job->mutex); job->error = status == PULSE_OK && !job->result ? PULSE_ERROR_STATE : status;
                  if (job->error != PULSE_OK || job->cancelled) job->result.reset(); }
                job->done = true;
                --self->workers;
                permit.Release();
                if (job->notify) InvalidateRect(job->notify, nullptr, FALSE);
                self->NotifyWake();
            }).detach();
        } catch (...) {
            --workers;
            job->error = PULSE_ERROR_INITIALIZATION;
            job->done = true;
            permit.Release();
            NotifyWake();
        }
    }
    void StartPending() {
        if (closed.load(std::memory_order_relaxed)) {
            queued_count.store(0, std::memory_order_seq_cst);
            return;
        }
        std::vector<std::shared_ptr<Job>> pending;
        for (const auto& [surface, job] : surfaces) {
            (void)surface;
            if (job->started || job->done) continue;
            if (job->cancelled.load(std::memory_order_acquire)) continue;
            pending.push_back(job);
        }
        if (pending.empty()) {
            queued_count.store(0, std::memory_order_seq_cst);
            return;
        }
        std::sort(pending.begin(), pending.end(), [](const auto& a, const auto& b) { return a->sequence > b->sequence; });
        queued_count.store(pending.size(), std::memory_order_seq_cst);
        for (const auto& job : pending) {
            if (job->started || job->done || job->cancelled.load(std::memory_order_acquire)) continue;
            if (workers.load(std::memory_order_acquire) >= kMaxSessionPreviewWorkers || closed.load(std::memory_order_relaxed)) break;
            auto permit = GlobalPreviewWorkerPool().TryAcquire();
            if (!permit) break;
            StartWithPermit(job, std::move(permit));
        }
        queued_count.store(CountUnstarted(), std::memory_order_seq_cst);
    }
    static PulseStatus PULSE_CALL Register(void* context, const PulsePreviewProvider* source, PulsePreviewProviderId* out) {
        if (out) *out = 0;
        try {
            auto* self = static_cast<State*>(context);
            constexpr uint32_t size_v1 = static_cast<uint32_t>(offsetof(PulsePreviewProvider, flags));
            if (!self || !source || source->size < size_v1 || !source->callback || !out) return PULSE_ERROR_ARGUMENT;
            const uint32_t flags = source->size >= size_v1 + sizeof(uint32_t) ? source->flags : 0u;
            if (flags & ~static_cast<uint32_t>(PULSE_PREVIEW_PROVIDER_THUMBNAIL)) return PULSE_ERROR_ARGUMENT;
            if (!self->Ready()) return PULSE_ERROR_STATE;
            if (self->providers.size() >= 16) return PULSE_ERROR_UNAVAILABLE;
            auto extension = Wide(source->extension, 32);
            if (extension.size() < 2 || extension[0] != L'.' || !std::all_of(extension.begin() + 1, extension.end(), [](wchar_t c) {
                return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'); })) return PULSE_ERROR_ARGUMENT;
            for (const auto& weak : registry) if (auto state = weak.lock(); state && state->owner == self->owner && state->Ready())
                for (const auto& [id, provider] : state->providers) { (void)id; if (provider.extension == extension) return PULSE_ERROR_STATE; }
            auto id = next_id.load();
            do { if (id == UINT64_MAX) return PULSE_ERROR_UNAVAILABLE; } while (!next_id.compare_exchange_weak(id, id + 1));
            std::shared_ptr<ThumbSlot> thumb;
            if (flags & PULSE_PREVIEW_PROVIDER_THUMBNAIL) {
                thumb = std::make_shared<ThumbSlot>();
                ThumbEntry entry;
                entry.state = self->weak_from_this(); entry.id = id + 1; entry.extension = extension;
                entry.callback = source->callback; entry.user = source->user; entry.slot = thumb;
                entry.package_id = self->package_id;
                std::lock_guard lock(thumb_mutex);
                std::erase_if(thumbs, [](const ThumbEntry& e) { return e.state.expired(); });
                thumbs.push_back(std::move(entry));
            }
            self->providers.emplace(id + 1, Provider{id + 1, std::move(extension), source->callback, source->user, false, std::move(thumb)});
            *out = id + 1; return PULSE_OK;
        } catch (...) { return PULSE_ERROR_ARGUMENT; }
    }
    static PulseStatus PULSE_CALL Unregister(void* context, PulsePreviewProviderId id) {
        auto* self = static_cast<State*>(context);
        if (!self || !id) return PULSE_ERROR_ARGUMENT;
        if (GetCurrentThreadId() != self->thread || self->closed.load(std::memory_order_relaxed)) return PULSE_ERROR_STATE;
        const auto provider = self->providers.find(id);
        if (provider == self->providers.end()) return PULSE_ERROR_STATE;
        provider->second.retiring = true;
        if (provider->second.thumb) { provider->second.thumb->retired = true; RemoveThumb(id); }
        for (auto it = self->surfaces.begin(); it != self->surfaces.end();) {
            if (it->second->provider.id == id) {
                it->second->cancelled = true;
                if (it->second->notify) InvalidateRect(it->second->notify, nullptr, FALSE);
                it = self->surfaces.erase(it);
            } else ++it;
        }
        self->queued_count.store(self->CountUnstarted(), std::memory_order_seq_cst);
        bool active = false;
        for (auto& weak : self->running) if (auto job = weak.lock(); job && job->provider.id == id) {
            job->cancelled = true; active = active || !job->done;
        }
        if (provider->second.thumb && provider->second.thumb->active.load() > 0) active = true;
        if (active) return PULSE_ERROR_STATE;
        self->providers.erase(provider);
        return PULSE_OK;
    }
};
PreviewsAdapter::PreviewsAdapter(HWND owner, std::function<bool()> live,
                                 std::function<void()> wake, std::string package_id)
    : state_(std::make_shared<State>(owner, std::move(live), std::move(wake), std::move(package_id))) {
    std::erase_if(State::registry, [](const auto& weak) { return weak.expired(); }); State::registry.push_back(state_);
    std::weak_ptr<State> weak = state_;
    state_->waiter_id.store(
        GlobalPreviewWorkerPool().RegisterWaiter([weak]() noexcept {
            if (const auto s = weak.lock()) {
                if (!s->closed.load(std::memory_order_acquire) &&
                    s->queued_count.load(std::memory_order_seq_cst) > 0) {
                    s->NotifyWake();
                }
            }
        }),
        std::memory_order_release);
}
PreviewsAdapter::~PreviewsAdapter() {
    Close();
    if (state_) state_->UnregisterPoolWaiter();
}
void PreviewsAdapter::Close() noexcept { state_->Close(); }
void PreviewsAdapter::Poll() noexcept {
    try {
        if (GetCurrentThreadId() != state_->thread) return;
        if (!state_->Ready()) { state_->Close(); return; }
        if (!state_->closed.load(std::memory_order_relaxed) && !state_->surfaces.empty()) {
            state_->StartPending();
        } else {
            state_->queued_count.store(0, std::memory_order_seq_cst);
        }
    } catch (...) { state_->Close(); }
}
bool PreviewsAdapter::CallbackActive() const noexcept { return state_->workers != 0; }
size_t PreviewsAdapter::ActiveWorkerCount() const noexcept {
    return state_->workers.load(std::memory_order_acquire);
}
size_t PreviewsAdapter::QueuedCount() const noexcept {
    return state_->queued_count.load(std::memory_order_acquire);
}
PulseStatus PreviewsAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    if (version != PULSE_PREVIEW_VERSION_1) return PULSE_ERROR_ABI;
    if (!out || size < sizeof(PulsePreviewV1)) return PULSE_ERROR_ARGUMENT;
    PulsePreviewV1 table{sizeof(table), PULSE_PREVIEW_VERSION_1, state_.get(), State::Register, State::Unregister};
    std::memcpy(out, &table, sizeof(table)); return PULSE_OK;
}
bool PreviewsAdapter::Available(HWND owner, const std::wstring& path) {
    if (!AbsoluteFilePath(path)) return false;
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    auto extension = path.substr(dot);
    for (auto& c : extension) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    for (const auto& weak : State::registry) if (auto state = weak.lock(); state && state->owner == owner && state->Ready())
        for (const auto& [id, provider] : state->providers) { (void)id; if (!provider.retiring && provider.extension == extension) return true; }
    return false;
}
void PreviewsAdapter::Cancel(HWND owner, uint64_t surface) {
    for (const auto& weak : State::registry) if (auto state = weak.lock(); state && state->owner == owner && GetCurrentThreadId() == state->thread) state->CancelSurface(surface);
}
PreviewResponse PreviewsAdapter::Request(HWND owner, const std::wstring& path, uint64_t modified, uint64_t size, uint32_t pixels, uint64_t surface, HWND notify) {
    try {
        if (!surface || !pixels || pixels > 4096) { Cancel(owner, surface); return {PreviewStatus::Failed, {}, PULSE_ERROR_ARGUMENT}; }
        if (!AbsoluteFilePath(path)) { Cancel(owner, surface); return {PreviewStatus::Failed, {}, PULSE_ERROR_ARGUMENT}; }
        const auto utf8 = Utf8(path);
        const auto dot = path.find_last_of(L'.');
        auto extension = dot == std::wstring::npos ? L"" : path.substr(dot);
        for (auto& c : extension) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
        for (const auto& weak : State::registry) {
            const auto state = weak.lock(); if (!state || state->owner != owner || !state->Ready()) continue;
            for (const auto& [id, provider] : state->providers) {
                if (provider.retiring || provider.extension != extension) continue;
                auto found = state->surfaces.find(surface);
                std::shared_ptr<State::Job> job = found == state->surfaces.end() ? nullptr : found->second;
                if (!job || job->provider.id != id || job->path != path || job->modified != modified || job->size != size || job->pixels != pixels) {
                    Cancel(owner, surface);
                    size_t surface_count = 0;
                    for (const auto& registered : State::registry) if (auto peer = registered.lock(); peer && peer->owner == owner)
                        surface_count += peer->surfaces.size();
                    if (surface_count >= 16) return {PreviewStatus::Failed, {}, PULSE_ERROR_UNAVAILABLE};
                    job = std::make_shared<State::Job>(); job->path = path; job->utf8 = utf8; job->modified = modified; job->size = size;
                    job->pixels = pixels; job->sequence = ++state->next_sequence; job->provider = provider; job->notify = notify ? notify : owner; state->surfaces[surface] = job;
                }
                if (!job->started && !job->done) state->StartPending();
                if (!job->done) return {PreviewStatus::Pending, {}, PULSE_OK};
                std::lock_guard lock(job->mutex);
                return {job->error == PULSE_OK ? PreviewStatus::Ready : PreviewStatus::Failed, job->result, job->error};
            }
        }
        Cancel(owner, surface); return {};
    } catch (...) { Cancel(owner, surface); return {PreviewStatus::Failed, {}, PULSE_ERROR_ARGUMENT}; }
}
bool PreviewsAdapter::ThumbnailAvailable(const std::wstring& path) noexcept {
    try {
        if (!AbsoluteFilePath(path)) return false;
        const auto extension = LowerExtension(path);
        if (extension.empty()) return false;
        std::lock_guard lock(State::thumb_mutex);
        for (const auto& entry : State::thumbs) {
            if (entry.extension != extension || entry.slot->retired.load()) continue;
            if (const auto state = entry.state.lock(); state && !state->closed.load()) return true;
        }
    } catch (...) {}
    return false;
}
PreviewsAdapter::ThumbnailStatus PreviewsAdapter::Thumbnail(const std::wstring& path, uint64_t modified, uint64_t size,
    uint32_t pixels, const std::function<bool()>& cancelled, ThumbnailBitmap& out) noexcept {
    try {
        out = {};
        if (!pixels || pixels > 4096 || !AbsoluteFilePath(path)) return ThumbnailStatus::NotHandled;
        const auto extension = LowerExtension(path);
        if (extension.empty()) return ThumbnailStatus::NotHandled;
        State::ThumbEntry entry;
        std::shared_ptr<State> state;
        {
            std::lock_guard lock(State::thumb_mutex);
            for (const auto& candidate : State::thumbs) {
                if (candidate.extension != extension || candidate.slot->retired.load()) continue;
                auto alive = candidate.state.lock();
                if (!alive || alive->closed.load()) continue;
                entry = candidate; state = std::move(alive); break;
            }
        }
        if (!state) return ThumbnailStatus::NotHandled;
        const auto utf8 = Utf8(path);
        const auto stop = [&]() -> bool {
            return state->closed.load() || entry.slot->retired.load() || (cancelled && cancelled());
        };
        // One of the session's provider slots and a global preview permit, as for previews.
        WorkerPermit permit;
        const auto until = GetTickCount64() + 2000;
        for (;;) {
            if (stop()) return ThumbnailStatus::Failed;
            unsigned current = state->workers.load();
            if (current < kMaxSessionPreviewWorkers && state->workers.compare_exchange_weak(current, current + 1)) {
                permit = GlobalPreviewWorkerPool().TryAcquire();
                if (permit) break;
                --state->workers;
            }
            if (GetTickCount64() >= until) return ThumbnailStatus::Busy;
            Sleep(15);
        }
        ++entry.slot->active;
        struct Release {
            State& state; State::ThumbSlot& slot; WorkerPermit& permit;
            ~Release() { --slot.active; --state.workers; permit.Release(); state.NotifyWake(); }
        } release{*state, *entry.slot, permit};
        // Re-check after taking the slot: Close / Unregister may have just retired it.
        if (stop()) return ThumbnailStatus::Failed;
        auto job = std::make_shared<State::Job>();
        job->path = path; job->utf8 = utf8; job->modified = modified; job->size = size; job->pixels = pixels;
        job->provider = State::Provider{entry.id, entry.extension, entry.callback, entry.user};
        job->started = true; job->worker = GetCurrentThreadId();
        job->poll = stop;
        PulseStatus status = PULSE_ERROR_STATE;
        {
            const crash_guard::CallScope call_scope(entry.package_id.c_str());
            PulsePreviewRequest request{sizeof(request), {job->utf8.data(), static_cast<uint32_t>(job->utf8.size())},
                modified, size, pixels, job.get(), State::Job::Cancelled, State::Job::Publish, PULSE_PREVIEW_PURPOSE_THUMBNAIL};
            try { status = entry.callback(entry.user, &request); } catch (...) { status = PULSE_ERROR_STATE; }
        }
        job->cancelled = true;  // late publish calls from leaked threads are refused
        job->done = true;
        std::shared_ptr<PreviewResult> result;
        { std::lock_guard lock(job->mutex); result = job->result; }
        if (status != PULSE_OK || !result || result->kind != PULSE_PREVIEW_BITMAP || stop())
            return ThumbnailStatus::Failed;
        ScaleToEdge(result->pixels.data(), result->width, result->height, result->stride, pixels,
                    out.pixels, out.width, out.height);
        return ThumbnailStatus::Ready;
    } catch (...) {
        out = {};
        return ThumbnailStatus::Failed;
    }
}
}
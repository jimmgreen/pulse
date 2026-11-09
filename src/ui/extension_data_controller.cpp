#include "extension_data_controller.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <utility>
namespace pulse::ui {
namespace {
std::string ValidateRows(const std::vector<extensions::UiDataRow> &rows, size_t *total = nullptr) {
    std::vector<const extensions::UiDataRow *> pending;
    for (const auto &row : rows)
        pending.push_back(&row);
    std::set<std::string> ids;
    size_t count = 0;
    while (!pending.empty()) {
        const auto *row = pending.back();
        pending.pop_back();
        if (++count > 4096)
            return "row_limit_exceeded";
        if (!row->id.empty() && !ids.insert(row->id).second)
            return "duplicate_row_id";
        for (const auto &child : row->children)
            pending.push_back(&child);
    }
    if (total)
        *total = count;
    return {};
}
void UpsertRows(std::vector<extensions::UiDataRow> &rows, const std::vector<extensions::UiDataRow> &updates) {
    for (const auto &row : updates) {
        if (row.id.empty())
            continue;
        auto existing = std::find_if(rows.begin(), rows.end(), [&](const auto &old) { return old.id == row.id; });
        if (existing == rows.end())
            rows.push_back(row);
        else
            *existing = row;
    }
}
} // namespace
struct ExtensionDataController::Inbox {
    std::mutex mutex;
    uint64_t generation = 0;
    bool closed = false;
    bool failed = false;
    std::vector<UiDataPage> pages;
    std::function<void()> wake;
};
ExtensionDataController::ExtensionDataController(UiDataProvider provider, std::function<void()> wake, Apply apply)
    : inbox_(std::make_shared<Inbox>()), provider_(std::move(provider)), apply_(std::move(apply)) {
    inbox_->wake = std::move(wake);
}
ExtensionDataController::~ExtensionDataController() { Close(); }
void ExtensionDataController::Cancel() {
    {
        std::lock_guard lock(inbox_->mutex);
        static std::atomic<uint64_t> next_generation{0};
        inbox_->generation = ++next_generation;
        inbox_->pages.clear();
    }
    auto cancel = std::exchange(cancel_, {});
    if (cancel)
        cancel();
}
void ExtensionDataController::Close() {
    {
        std::lock_guard lock(inbox_->mutex);
        inbox_->closed = true;
        inbox_->wake = {};
    }
    Cancel();
    apply_ = {};
}
void ExtensionDataController::Request(UiDataRequest request, bool append) {
    Cancel();
    append_ = append;
    {
        std::lock_guard lock(inbox_->mutex);
        if (inbox_->closed)
            return;
        if (!append) {
            inbox_->failed = false;
            next_cursor_.clear();
        } else if (inbox_->failed) {
            UiDataPage page;
            page.status = "error";
            page.error = "page_queue_full";
            inbox_->pages.push_back(std::move(page));
            if (inbox_->wake)
                inbox_->wake();
            return;
        }
        request.generation = inbox_->generation;
    }
    std::weak_ptr<Inbox> weak = inbox_;
    auto completion = [weak, generation = request.generation](UiDataPage page) {
        auto inbox = weak.lock();
        if (!inbox)
            return;
        std::lock_guard lock(inbox->mutex);
        if (inbox->closed || inbox->failed || inbox->generation != generation)
            return;
        if (page.status != "idle" && page.status != "loading" && page.status != "ready" && page.status != "error" &&
            page.status != "cancelled")
            return;
        if (inbox->pages.size() >= 64) {
            inbox->pages.clear();
            inbox->failed = true;
            page = {};
            page.status = "error";
            page.error = "page_queue_full";
        }
        inbox->pages.push_back(std::move(page));
        // wake only posts an owning-window message; it must never invoke Drain synchronously.
        if (inbox->wake)
            inbox->wake();
    };
    if (provider_) {
        auto cancel = provider_(request, std::move(completion));
        bool current;
        {
            std::lock_guard lock(inbox_->mutex);
            current = !inbox_->closed && inbox_->generation == request.generation;
        }
        if (current)
            cancel_ = std::move(cancel);
        else if (cancel)
            cancel();
    } else {
        UiDataPage page;
        page.status = "error";
        completion(std::move(page));
    }
}
void ExtensionDataController::Drain() {
    std::vector<UiDataPage> pages;
    uint64_t generation;
    {
        std::lock_guard lock(inbox_->mutex);
        if (inbox_->closed)
            return;
        generation = inbox_->generation;
        pages.swap(inbox_->pages);
    }
    for (auto &page : pages) {
        {
            std::lock_guard lock(inbox_->mutex);
            if (inbox_->closed || generation != inbox_->generation)
                return;
        }
        page.progress = std::clamp(page.progress, 0, 100);
        if (page.status == "idle") {
            rows_.clear();
            next_cursor_.clear();
            revision_.clear();
            has_snapshot_ = false;
            page.rows.clear();
        }
        if (page.status == "ready") {
            std::string error;
            if (page.delta && append_)
                error = "invalid_delta";
            else if (page.delta && (!has_snapshot_ || page.base_revision != revision_))
                error = "stale_revision";
            else if (append_ && has_snapshot_ && page.revision != revision_)
                error = "revision_changed";
            else
                error = ValidateRows(page.rows);
            if (error.empty()) {
                auto updated = (append_ || page.delta) ? rows_ : std::vector<extensions::UiDataRow>{};
                if (page.delta) {
                    const std::set<std::string> removed(page.removed_ids.begin(), page.removed_ids.end());
                    std::erase_if(updated, [&](const auto &row) { return removed.contains(row.id); });
                }
                UpsertRows(updated, page.rows);
                size_t row_count = 0;
                error = ValidateRows(updated, &row_count);
                if (error.empty() && row_count == 4096 && !page.delta && !page.next_cursor.empty())
                    error = "row_limit_exceeded";
                if (error.empty()) {
                    rows_ = std::move(updated);
                    revision_ = page.revision;
                    has_snapshot_ = true;
                    next_cursor_ = page.delta ? std::string{} : page.next_cursor;
                }
            }
            if (!error.empty()) {
                page.status = "error";
                page.error = std::move(error);
            }
        }
        page.rows = rows_;
        page.revision = revision_;
        page.next_cursor = next_cursor_;
        if (page.status == "error") {
            next_cursor_.clear();
            page.next_cursor.clear();
        }
        auto apply = apply_;
        if (apply)
            apply(page);
    }
}
} // namespace pulse::ui

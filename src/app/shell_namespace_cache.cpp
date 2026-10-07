// shell_namespace_cache.cpp — see shell_namespace_cache.h.
#include "shell_namespace_cache.h"
#include "../ipc/shell_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>

namespace pulse::app {

namespace {

std::mutex g_mutex;
std::vector<ipc::ShellItem> g_items;
bool g_loaded = false;            // a refresh has completed at least once
std::atomic<bool> g_in_flight{false};
std::atomic<bool> g_changed{false};

// One outstanding blocking listing, so the reader thread can hand its answer to
// the worker waiting on it. Keyed by request id, which the protocol echoes back.
struct PendingListing {
    bool done = false;
    bool ok = false;
    std::vector<ipc::ShellItem> items;
};
std::condition_variable g_listing_cv;
std::map<uint32_t, PendingListing> g_listings;

// Fields the sidebar cares about, so a periodic refresh does not rebuild it
// when nothing moved.
bool SameItems(const std::vector<ipc::ShellItem>& left,
               const std::vector<ipc::ShellItem>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].parsing_name != right[i].parsing_name) return false;
        if (left[i].display_name != right[i].display_name) return false;
        if (left[i].is_dir != right[i].is_dir) return false;
        if (left[i].total != right[i].total) return false;
    }
    return true;
}

} // namespace

std::vector<ipc::ShellItem> CachedShellRoots() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_items;
}

void RequestShellRootsRefresh() {
    if (g_in_flight.exchange(true)) return;   // one request at a time

    // The answer arrives on the client's reader thread, so nothing here blocks
    // the sidebar worker.
    const uint32_t id = ipc::ShellClient::Instance().ShellRoots();
    if (!id) g_in_flight.store(false);
}

bool TakeShellRootsChanged() {
    return g_changed.exchange(false);
}

bool DispatchShellItems(uint32_t request_id, std::vector<ipc::ShellItem> items, bool ok) {
    // A folder listing somebody is waiting on? Hand it over and let the waiter
    // erase the entry, so the id cannot be reused while it is being read.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto found = g_listings.find(request_id);
        if (found != g_listings.end()) {
            found->second.done = true;
            found->second.ok = ok;
            found->second.items = std::move(items);
            g_listing_cv.notify_all();
            return true;
        }
    }

    // Otherwise it is the roots refresh, the only other shell request the app
    // makes. Keeping the previous list on failure matters: This PC must not go
    // empty because one request was not answered.
    if (!ok) {
        g_in_flight.store(false);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!SameItems(g_items, items)) g_changed.store(true);
        g_items = std::move(items);
        g_loaded = true;
    }
    g_in_flight.store(false);
    return true;
}

bool ListShellFolderBlocking(const std::wstring& shell_path,
                             std::vector<ipc::ShellItem>& out, int timeout_ms) {
    out.clear();
    if (shell_path.empty()) return false;

    const uint32_t id = ipc::ShellClient::Instance().ListShellFolder(shell_path);
    if (!id) return false;

    std::unique_lock<std::mutex> lock(g_mutex);
    g_listings[id];   // register before the answer can arrive
    const bool ready = g_listing_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
        const auto found = g_listings.find(id);
        return found != g_listings.end() && found->second.done;
    });

    bool ok = false;
    const auto found = g_listings.find(id);
    if (ready && found != g_listings.end()) {
        ok = found->second.ok;
        if (ok) out = std::move(found->second.items);
    }
    if (found != g_listings.end()) g_listings.erase(found);
    return ok;
}

} // namespace pulse::app

#include <pulse/ui_data.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern "C" PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1*, PulsePluginV1*) noexcept;
namespace {
using namespace std::chrono_literals;
PulseStringView View(std::string_view value) { return {value.data(), static_cast<uint32_t>(value.size())}; }
std::string Copy(PulseStringView value) { return value.length ? std::string(value.data, value.length) : std::string(); }
struct Page {
    uint64_t request = 0;
    uint32_t status = 0, mode = 0;
    std::string revision, base, cursor, error;
    std::vector<std::string> ids, names, removed;
};
struct Host {
    PulseUiDataRequestFn requested = nullptr;
    PulseUiDataCancelFn canceled = nullptr;
    void* user = nullptr;
    uint64_t request_id = 0, serial = 0;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Page> pages;
    std::set<std::thread::id> publishers;
    std::string cursor;
    std::map<std::string, std::string> states;
    std::map<std::string, std::pair<PulseUiClickFn, void*>> commands;
    uint32_t bound_page_size = 0;
    bool table_found = false, provider_removed = false, closed = false;
    static Host& Self(void* context) { return *static_cast<Host*>(context); }
    static PulseStatus PULSE_CALL Query(void* context, PulseStringView name, uint32_t version, void* out, uint32_t size) {
        if (version != 1) return PULSE_ERROR_ABI;
        const auto key = Copy(name);
        if (key == PULSE_UI_MODEL_INTERFACE && size == sizeof(PulseUiModelV1)) {
            const PulseUiModelV1 table{sizeof(table), 1, context, Create, nullptr, Subscribe, SetStates, nullptr, Unsubscribe, Close};
            std::memcpy(out, &table, sizeof(table)); return PULSE_OK;
        }
        if (key == PULSE_UI_DATA_INTERFACE && size == sizeof(PulseUiDataV1)) {
            const PulseUiDataV1 table{sizeof(table), 1, context, Register, Unregister, Bind, Unbind, Request, Publish};
            std::memcpy(out, &table, sizeof(table)); return PULSE_OK;
        }
        return PULSE_ERROR_UNAVAILABLE;
    }
    static bool HasEmptyTable(const PulseUiModelNode& node) {
        if (node.type == PULSE_UI_MODEL_DATA_TABLE && Copy(node.id) == "items") return node.rows.present && node.rows.count == 0;
        for (uint32_t i = 0; i < node.children.count; ++i) if (HasEmptyTable(node.children.data[i])) return true;
        return false;
    }
    static PulseStatus PULSE_CALL Create(void* context, PulseStringView, const PulseUiModelTree* tree, uint64_t* panel) {
        auto& self = Self(context); self.table_found = HasEmptyTable(*tree->root);
        for (uint32_t i = 0; i < tree->state_count; ++i) self.states[Copy(tree->states[i].key)] = Copy(tree->states[i].value);
        *panel = 1; return PULSE_OK;
    }
    static PulseStatus PULSE_CALL SetStates(void* context, uint64_t, const PulseUiState* states, uint32_t count) {
        auto& self = Self(context); bool reload = false;
        for (uint32_t i = 0; i < count; ++i) {
            const auto key = Copy(states[i].key); self.states[key] = Copy(states[i].value);
            reload = reload || key == "filter" || key == "sort";
        }
        if (reload) return Request(context, 1, View("items"), PULSE_UI_DATA_REFRESH);
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Subscribe(void* context, uint64_t, PulseStringView command, PulseUiClickFn callback, void* user, uint64_t* subscription) {
        auto& self = Self(context); self.commands[Copy(command)] = {callback, user}; *subscription = self.commands.size(); return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Unsubscribe(void*, uint64_t) { return PULSE_OK; }
    static PulseStatus PULSE_CALL Close(void* context, uint64_t) { Self(context).closed = true; return PULSE_OK; }
    static PulseStatus PULSE_CALL Register(void* context, PulseUiDataRequestFn requested, PulseUiDataCancelFn canceled, void* user, uint64_t* provider) {
        auto& self = Self(context); self.requested = requested; self.canceled = canceled; self.user = user; *provider = 1; return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Unregister(void* context, uint64_t) {
        auto& self = Self(context); Request(context, 1, View("items"), PULSE_UI_DATA_CANCEL); self.provider_removed = true; return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Bind(void* context, uint64_t, PulseStringView, uint64_t, const PulseUiDataBinding* binding) {
        Self(context).bound_page_size = binding->page_size; return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Unbind(void* context, uint64_t panel, PulseStringView node) { return Request(context, panel, node, PULSE_UI_DATA_CANCEL); }
    static PulseStatus PULSE_CALL Request(void* context, uint64_t, PulseStringView, uint32_t operation) {
        auto& self = Self(context); uint64_t old = 0, next = 0; std::string cursor;
        {
            std::lock_guard lock(self.mutex);
            if (operation == PULSE_UI_DATA_NEXT_PAGE && self.cursor.empty()) return PULSE_ERROR_STATE;
            old = self.request_id; self.request_id = 0;
            if (operation != PULSE_UI_DATA_CANCEL) {
                self.request_id = next = ++self.serial;
                if (operation == PULSE_UI_DATA_NEXT_PAGE) cursor = self.cursor;
            }
        }
        if (old && self.canceled) self.canceled(self.user, old);
        if (!next) return PULSE_OK;
        const PulseUiDataRequest request{sizeof(request), 128, next, 1, 1, View("items"), View(self.states["filter"]), View(self.states["sort"]), View(cursor)};
        self.requested(self.user, &request); return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Publish(void* context, uint64_t request, const PulseUiDataPage* page) {
        auto& self = Self(context); std::lock_guard lock(self.mutex);
        self.publishers.insert(std::this_thread::get_id());
        if (request != self.request_id) return PULSE_ERROR_STATE;
        Page copy; copy.request = request; copy.status = page->status; copy.mode = page->mode;
        copy.revision = Copy(page->revision); copy.base = Copy(page->base_revision); copy.cursor = Copy(page->next_cursor); copy.error = Copy(page->error);
        for (uint32_t i = 0; i < page->row_count; ++i) { copy.ids.push_back(Copy(page->rows[i].id)); copy.names.push_back(Copy(page->rows[i].text.value)); }
        for (uint32_t i = 0; i < page->removed_count; ++i) copy.removed.push_back(Copy(page->removed_ids[i]));
        if (page->status == PULSE_UI_DATA_READY) self.cursor = copy.cursor;
        self.pages.push_back(std::move(copy)); self.changed.notify_all(); return PULSE_OK;
    }
    size_t Mark() { std::lock_guard lock(mutex); return pages.size(); }
    bool Wait(size_t after, uint32_t status, uint32_t mode, Page& result) {
        std::unique_lock lock(mutex);
        const auto find = [&]() {
            for (size_t i = after; i < pages.size(); ++i) if (pages[i].status == status && pages[i].mode == mode) { result = pages[i]; return true; }
            return false;
        };
        return changed.wait_for(lock, 3s, find);
    }
    void Click(std::string_view command) {
        const auto found = commands.find(std::string(command));
        if (found != commands.end()) found->second.first(found->second.second, 1, View(command));
    }
    void Filter(std::string_view value) {
        const PulseUiState state{View("filter"), View(value)}; SetStates(this, 1, &state, 1);
    }
};
bool Check(bool result, const char* name) { std::cout << (result ? "[PASS] " : "[FAIL] ") << name << '\n'; return result; }
}
int main() {
    Host fake; const PulseHostV1 host{sizeof(host), 1, &fake, Host::Query}; PulsePluginV1 plugin{}; plugin.size = sizeof(plugin); plugin.version = 1;
    bool ok = Check(PulsePluginEntryV1(&host, &plugin) == PULSE_OK && plugin.activate(plugin.instance) == PULSE_OK, "public SDK activation");
    Page first;
    ok &= Check(fake.Wait(0, PULSE_UI_DATA_READY, PULSE_UI_DATA_SNAPSHOT, first) && first.ids.size() == 128 && first.ids.front() == "item-1" && first.ids.back() == "item-128" && !first.cursor.empty() && fake.table_found && fake.bound_page_size == 128, "real empty DataTable binding and first 128 stable IDs");
    size_t mark = fake.Mark(); fake.Click("next"); Page next;
    ok &= Check(fake.Wait(mark, PULSE_UI_DATA_READY, PULSE_UI_DATA_SNAPSHOT, next) && next.ids.size() == 128 && next.ids.front() == "item-129" && next.revision == first.revision, "revision-matched next 128 page");
    std::set<std::string> all_ids(first.ids.begin(), first.ids.end());
    all_ids.insert(next.ids.begin(), next.ids.end());
    bool paged_all = true;
    while (!next.cursor.empty() && paged_all) {
        mark = fake.Mark(); fake.Click("next");
        paged_all = fake.Wait(mark, PULSE_UI_DATA_READY, PULSE_UI_DATA_SNAPSHOT, next) && next.ids.size() == 128 && next.revision == first.revision;
        all_ids.insert(next.ids.begin(), next.ids.end());
    }
    ok &= Check(paged_all && all_ids.size() == 4096 && all_ids.contains("item-4096"), "all 4096 generated stable IDs paginate without duplicates");
    mark = fake.Mark(); fake.Filter("0001"); Page filtered;
    ok &= Check(fake.Wait(mark, PULSE_UI_DATA_READY, PULSE_UI_DATA_SNAPSHOT, filtered) && filtered.ids.size() == 11 && filtered.ids.front() == "item-1", "background substring filter");
    mark = fake.Mark(); fake.Click("descending"); Page sorted;
    ok &= Check(fake.Wait(mark, PULSE_UI_DATA_READY, PULSE_UI_DATA_SNAPSHOT, sorted) && sorted.ids.size() == 11 && sorted.ids.front() == "item-19", "background descending sort");
    mark = fake.Mark(); fake.Filter(""); fake.Click("ascending"); fake.Click("refresh"); Page live;
    ok &= Check(fake.Wait(mark, PULSE_UI_DATA_READY, PULSE_UI_DATA_SNAPSHOT, live) && live.ids.size() == 128, "refresh restores generated data");
    std::this_thread::sleep_for(20ms); mark = fake.Mark(); fake.Click("mutate"); Page delta;
    ok &= Check(fake.Wait(mark, PULSE_UI_DATA_READY, PULSE_UI_DATA_DELTA, delta) && delta.base == live.revision && delta.revision != delta.base && delta.ids.size() == 2 && delta.removed.size() == 1 && delta.ids.front() == "item-1" && delta.ids.back() == "item-4097" && delta.removed.front() == "item-128" && delta.cursor.empty(), "stable-ID add/change/remove delta clears cursor");
    mark = fake.Mark(); fake.Click("empty"); Page empty;
    ok &= Check(fake.Wait(mark, PULSE_UI_DATA_READY, PULSE_UI_DATA_SNAPSHOT, empty) && empty.ids.empty() && !empty.revision.empty(), "valid empty snapshot");
    mark = fake.Mark(); fake.Click("error"); Page error;
    ok &= Check(fake.Wait(mark, PULSE_UI_DATA_ERROR, PULSE_UI_DATA_SNAPSHOT, error) && !error.error.empty() && error.ids.empty(), "explicit provider error");
    mark = fake.Mark(); fake.Click("refresh"); fake.Click("cancel"); std::this_thread::sleep_for(120ms);
    bool no_ready = true;
    { std::lock_guard lock(fake.mutex); for (size_t i = mark; i < fake.pages.size(); ++i) if (fake.pages[i].status == PULSE_UI_DATA_READY) no_ready = false; }
    ok &= Check(no_ready, "cancel signals without accepting a stale snapshot");
    mark = fake.Mark();
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 100; ++i) fake.Click("refresh");
    const bool prompt = std::chrono::steady_clock::now() - start < 500ms;
    Page refreshed;
    ok &= Check(prompt && fake.Wait(mark, PULSE_UI_DATA_READY, PULSE_UI_DATA_SNAPSHOT, refreshed), "100 refresh callbacks use bounded latest-request mailbox");
    plugin.deactivate(plugin.instance); plugin.destroy(plugin.instance);
    size_t completed = fake.Mark(); std::this_thread::sleep_for(30ms);
    ok &= Check(fake.provider_removed && fake.closed && fake.Mark() == completed && fake.publishers.size() == 1, "one worker joined before destroy returns");
    return ok ? 0 : 1;
}

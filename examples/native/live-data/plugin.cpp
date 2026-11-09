#include <pulse/sdk.hpp>
#include <pulse/ui_data.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
using pulse::sdk::View;
using namespace std::chrono_literals;
PulseUiModelString Text(std::string_view value) { return {1, View(value)}; }
std::string Copy(PulseStringView value) { return value.length ? std::string(value.data, value.length) : std::string(); }
std::string Lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
const char* commands[] = {"refresh", "next", "cancel", "mutate", "empty", "error", "ascending", "descending"};
enum class Mode { Normal, Empty, Error };
struct Item { uint64_t id; std::string name; uint64_t score = 0; };
struct Task { uint64_t request; uint32_t page_size; std::string filter, sort, cursor; Mode mode; };

struct Plugin {
    pulse::sdk::Host host;
    PulseUiModelV1 model{};
    PulseUiDataV1 data{};
    uint64_t panel = 0, provider = 0;
    std::array<uint64_t, 8> subscriptions{};
    bool active = false;
    std::mutex mutex;
    std::condition_variable_any changed;
    std::optional<Task> pending;
    std::atomic_uint64_t active_request{0}, mutations{0};
    std::atomic<Mode> mode{Mode::Normal};
    std::atomic_bool stream_live{false};
    std::jthread worker;

    explicit Plugin(const PulseHostV1& h) : host(h) {}
    bool Current(uint64_t request, std::stop_token stop) const noexcept {
        return !stop.stop_requested() && active_request.load() == request;
    }
    void Message(std::string_view value) noexcept {
        if (!active || !panel) return;
        const PulseUiState state{View("message"), View(value)};
        model.set_states(model.context, panel, &state, 1);
    }
    PulseStatus PublishStatus(const Task& task, uint32_t status, std::string_view error = {}) noexcept {
        PulseUiDataPage page{}; page.size = sizeof(page); page.status = status;
        page.progress = status == PULSE_UI_DATA_LOADING ? 10u : 0u; page.error = View(error);
        return data.publish(data.context, task.request, &page);
    }
    PulseStatus PublishRows(const Task& task, const std::vector<Item>& items,
        std::string_view revision, std::string_view cursor, std::string_view base = {},
        std::string_view removed = {}) {
        std::vector<std::array<std::string, 3>> values;
        std::vector<std::string> ids;
        for (const auto& item : items) {
            ids.push_back("item-" + std::to_string(item.id));
            values.push_back({item.name, std::to_string(item.score), std::string(revision)});
        }
        std::vector<std::array<PulseStringView, 3>> cells;
        for (const auto& value : values) cells.push_back({View(value[0]), View(value[1]), View(value[2])});
        std::vector<PulseUiModelRow> rows;
        for (size_t i = 0; i < items.size(); ++i) {
            PulseUiModelRow row{}; row.size = sizeof(row); row.id = View(ids[i]);
            row.text = Text(values[i][0]); row.cells = {1, cells[i].data(), 3}; rows.push_back(row);
        }
        const PulseStringView removed_id = View(removed);
        PulseUiDataPage page{}; page.size = sizeof(page); page.status = PULSE_UI_DATA_READY;
        page.mode = base.empty() ? PULSE_UI_DATA_SNAPSHOT : PULSE_UI_DATA_DELTA;
        page.progress = 100; page.revision = View(revision); page.base_revision = View(base);
        page.next_cursor = View(cursor); page.rows = rows.data(); page.row_count = static_cast<uint32_t>(rows.size());
        page.removed_ids = removed.empty() ? nullptr : &removed_id; page.removed_count = removed.empty() ? 0u : 1u;
        return data.publish(data.context, task.request, &page);
    }
    static bool Matches(const Item& item, std::string_view filter) {
        return filter.empty() || Lower(item.name).find(filter) != std::string::npos;
    }
    void Process(const Task& task, std::stop_token stop, std::vector<Item>& source, uint64_t& revision, uint64_t& next_id) {
        if (!Current(task.request, stop)) return;
        if (PublishStatus(task, PULSE_UI_DATA_LOADING) != PULSE_OK) return;
        {
            std::unique_lock lock(mutex);
            changed.wait_for(lock, stop, 80ms, [&] { return !Current(task.request, stop); });
        }
        if (!Current(task.request, stop)) return;
        if (task.mode == Mode::Error) { PublishStatus(task, PULSE_UI_DATA_ERROR, "Deliberate provider error. Refresh restores generated data."); return; }
        const std::string current_revision = "r" + std::to_string(revision);
        size_t offset = 0;
        if (!task.cursor.empty()) {
            const auto separator = task.cursor.find('@');
            if (separator == std::string::npos || task.cursor.substr(0, separator) != current_revision) {
                PublishStatus(task, PULSE_UI_DATA_ERROR, "Stale cursor. Refresh before paging again."); return;
            }
            const auto text = std::string_view(task.cursor).substr(separator + 1);
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), offset);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
                PublishStatus(task, PULSE_UI_DATA_ERROR, "Invalid generated-data cursor."); return;
            }
        }
        std::vector<Item> filtered;
        const auto filter = Lower(task.filter);
        if (task.mode != Mode::Empty) {
            for (const auto& item : source) {
                if (!Current(task.request, stop)) return;
                if (Matches(item, filter)) filtered.push_back(item);
            }
            const bool descending = task.sort == "nameDesc";
            std::sort(filtered.begin(), filtered.end(), [descending](const Item& a, const Item& b) {
                return descending ? a.name > b.name : a.name < b.name;
            });
        }
        if (!Current(task.request, stop)) return;
        if (offset > filtered.size()) { PublishStatus(task, PULSE_UI_DATA_ERROR, "Cursor exceeds the current result."); return; }
        const auto end = (std::min)(offset + task.page_size, filtered.size());
        std::vector<Item> displayed(filtered.begin() + static_cast<std::ptrdiff_t>(offset), filtered.begin() + static_cast<std::ptrdiff_t>(end));
        const std::string cursor = end < filtered.size() ? current_revision + "@" + std::to_string(end) : "";
        if (PublishRows(task, displayed, current_revision, cursor) != PULSE_OK || !task.cursor.empty() || task.mode != Mode::Normal) return;
        stream_live = true;
        auto observed_mutations = mutations.load();
        std::string base = current_revision;
        while (Current(task.request, stop)) {
            {
                std::unique_lock lock(mutex);
                changed.wait_for(lock, stop, 30s, [&] {
                    return !Current(task.request, stop) || mutations.load() != observed_mutations;
                });
            }
            if (!Current(task.request, stop)) break;
            observed_mutations = mutations.load();
            if (displayed.empty()) continue;
            // Dataset ownership and all query/mutation work stay on this one worker.
            const auto removed_number = displayed.back().id;
            const std::string removed = "item-" + std::to_string(removed_number);
            source.erase(std::remove_if(source.begin(), source.end(), [&](const Item& item) { return item.id == removed_number; }), source.end());
            displayed.pop_back();
            std::vector<Item> upserts;
            if (!displayed.empty()) {
                const auto changed_id = displayed.front().id;
                auto found = std::find_if(source.begin(), source.end(), [&](const Item& item) { return item.id == changed_id; });
                if (found != source.end()) { ++found->score; displayed.front() = *found; upserts.push_back(*found); }
            }
            const auto added_id = next_id++;
            Item added{added_id, "Item " + std::to_string(added_id) + " (added)", revision};
            source.push_back(added);
            if (Matches(added, filter)) { displayed.push_back(added); upserts.push_back(added); }
            const std::string new_revision = "r" + std::to_string(++revision);
            if (!Current(task.request, stop) || PublishRows(task, upserts, new_revision, "", base, removed) != PULSE_OK) break;
            base = new_revision;
        }
        stream_live = false;
    }
    void Work(std::stop_token stop) noexcept {
        try {
            std::vector<Item> source;
            source.reserve(4096);
            for (uint64_t i = 1; i <= 4096; ++i) {
                const auto number = std::to_string(i);
                source.push_back({i, "Item " + std::string(5 - number.size(), '0') + number + ".txt", i * 17 % 1000});
            }
            uint64_t revision = 1, next_id = 4097;
            while (!stop.stop_requested()) {
                std::optional<Task> task;
                {
                    std::unique_lock lock(mutex);
                    if (!changed.wait(lock, stop, [&] { return pending.has_value(); })) return;
                    task = std::move(pending); pending.reset();
                }
                try { Process(*task, stop, source, revision, next_id); }
                catch (...) { PublishStatus(*task, PULSE_UI_DATA_ERROR, "Generated-data worker failed. Refresh retries the request."); }
                stream_live = false;
            }
        } catch (...) {}
    }
    static void PULSE_CALL Requested(void* user, const PulseUiDataRequest* request) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        if (!request) return;
        try {
            Task task{request->request, request->page_size, Copy(request->filter), Copy(request->sort), Copy(request->cursor), self.mode.load()};
            {
                std::lock_guard lock(self.mutex);
                self.active_request = request->request; self.pending = std::move(task);
            }
            self.changed.notify_one();
        } catch (...) {
            PulseUiDataPage page{}; page.size = sizeof(page); page.status = PULSE_UI_DATA_ERROR; page.error = View("Could not copy the provider request.");
            self.data.publish(self.data.context, request->request, &page);
        }
    }
    static void PULSE_CALL Canceled(void* user, uint64_t request) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        if (self.active_request.compare_exchange_strong(request, 0)) self.stream_live = false;
        self.changed.notify_all();
    }
    static void PULSE_CALL Click(void* user, uint64_t panel_id, PulseStringView command) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        if (!self.active || panel_id != self.panel) return;
        try {
            const auto id = Copy(command);
            if (id == "mutate") {
                if (!self.stream_live.load()) { self.Message("Refresh to resume live deltas before using Mutate."); return; }
                ++self.mutations; self.changed.notify_all(); self.Message("Queued a generated add / change / remove delta."); return;
            }
            if (id == "ascending" || id == "descending") {
                const PulseUiState state{View("sort"), View(id == "ascending" ? "nameAsc" : "nameDesc")};
                self.model.set_states(self.model.context, self.panel, &state, 1); return;
            }
            uint32_t operation = PULSE_UI_DATA_REFRESH;
            if (id == "next") operation = PULSE_UI_DATA_NEXT_PAGE;
            else if (id == "cancel") operation = PULSE_UI_DATA_CANCEL;
            else self.mode = id == "empty" ? Mode::Empty : id == "error" ? Mode::Error : Mode::Normal;
            const auto code = self.data.request(self.data.context, self.panel, View("items"), operation);
            self.Message(code == PULSE_OK ? (id == "cancel" ? "Canceled. Refresh starts a new stream." : "Request accepted.") : "No valid next page or active binding. Refresh starts again.");
        } catch (...) { self.Message("Generated-data action failed."); }
    }
    PulseStatus Render() {
        PulseUiModelNode input{}; input.size = sizeof(input); input.type = PULSE_UI_MODEL_TEXT_INPUT;
        input.id = View("filter-input"); input.text = Text("Filter generated names"); input.state = Text("filter"); input.placeholder = Text("Try 0001 or added");
        const char* labels[] = {"Refresh", "Next 128", "Cancel", "Mutate now", "Empty", "Error", "A to Z", "Z to A"};
        std::array<PulseUiModelNode, 8> buttons{};
        std::array<PulseStringView, 8> declared{};
        for (size_t i = 0; i < buttons.size(); ++i) {
            auto& button = buttons[i]; button.size = sizeof(button); button.type = PULSE_UI_MODEL_BUTTON;
            button.id = View(commands[i]); button.text = Text(labels[i]); button.command = Text(commands[i]); declared[i] = View(commands[i]);
        }
        PulseUiModelNode toolbar{}; toolbar.size = sizeof(toolbar); toolbar.type = PULSE_UI_MODEL_GRID;
        toolbar.id = View("controls"); toolbar.columns = {1, 3}; toolbar.gap = {1, 6}; toolbar.children = {1, buttons.data(), static_cast<uint32_t>(buttons.size())};
        std::array<PulseUiModelColumn, 3> columns{};
        const char* column_ids[] = {"name", "score", "revision"}; const char* titles[] = {"Generated name", "Score", "Revision"};
        for (size_t i = 0; i < columns.size(); ++i) {
            auto& column = columns[i]; column.size = sizeof(column); column.id = View(column_ids[i]); column.text = View(titles[i]); column.width = {1, i == 0 ? 210 : i == 1 ? 70 : 90};
        }
        PulseUiModelNode table{}; table.size = sizeof(table); table.type = PULSE_UI_MODEL_DATA_TABLE;
        table.id = View("items"); table.state = Text("selected"); table.rows = {1, nullptr, 0};
        table.data_columns = {1, columns.data(), static_cast<uint32_t>(columns.size())}; table.min_height = {1, 260};
        std::array<PulseUiModelNode, 4> messages{};
        const char* message_ids[] = {"status-label", "error-label", "progress-label", "message-label"};
        const char* bindings[] = {"state.status", "state.error", "state.progress", "state.message"};
        for (size_t i = 0; i < messages.size(); ++i) {
            messages[i].size = sizeof(PulseUiModelNode); messages[i].type = PULSE_UI_MODEL_TEXT;
            messages[i].id = View(message_ids[i]); messages[i].binding = Text(bindings[i]);
        }
        const PulseUiModelNode children[] = {input, toolbar, messages[0], messages[1], messages[2], messages[3], table};
        PulseUiModelNode stack{}; stack.size = sizeof(stack); stack.type = PULSE_UI_MODEL_STACK;
        stack.id = View("content"); stack.gap = {1, 8}; stack.children = {1, children, 7};
        PulseUiModelNode root{}; root.size = sizeof(root); root.type = PULSE_UI_MODEL_SCROLL_VIEW;
        root.id = View("root"); root.children = {1, &stack, 1};
        const PulseUiState states[] = {
            {View("filter"), View("")}, {View("sort"), View("nameAsc")}, {View("selected"), View("")},
            {View("status"), View("")}, {View("error"), View("")}, {View("progress"), View("0")},
            {View("message"), View("4096 generated items; live deltas every 30 seconds. Next before a delta, or Refresh to restart.")}};
        const PulseUiModelTree tree{sizeof(tree), &root, states, 7, nullptr, 0, declared.data(), static_cast<uint32_t>(declared.size())};
        return model.create(model.context, View("Live generated data"), &tree, &panel);
    }
    PulseStatus Start() {
        if (worker.joinable()) return PULSE_ERROR_STATE;
        auto code = host.Query(PULSE_UI_MODEL_INTERFACE, 1, model); if (code != PULSE_OK) return code;
        code = host.Query(PULSE_UI_DATA_INTERFACE, 1, data); if (code != PULSE_OK) return code;
        code = Render(); if (code != PULSE_OK) return code;
        active = true;
        code = data.register_provider(data.context, Requested, Canceled, this, &provider); if (code != PULSE_OK) return code;
        const PulseUiDataBinding binding{sizeof(binding), 128, View("filter"), View("sort"), View("status"), View("error"), View("progress")};
        code = data.bind(data.context, panel, View("items"), provider, &binding); if (code != PULSE_OK) return code;
        for (size_t i = 0; i < subscriptions.size(); ++i) {
            code = model.subscribe_command(model.context, panel, View(commands[i]), Click, this, &subscriptions[i]); if (code != PULSE_OK) return code;
        }
        worker = std::jthread([this](std::stop_token stop) { Work(stop); });
        return data.request(data.context, panel, View("items"), PULSE_UI_DATA_REFRESH);
    }
    void Stop() noexcept {
        active = false; active_request = 0; stream_live = false;
        worker.request_stop(); changed.notify_all();
        for (auto& subscription : subscriptions) if (subscription) { model.unsubscribe(model.context, subscription); subscription = 0; }
        if (provider) {
            if (panel) data.unbind(data.context, panel, View("items"));
            data.unregister_provider(data.context, provider); provider = 0;
        }
        if (panel) { model.close(model.context, panel); panel = 0; }
    }
    ~Plugin() { Stop(); if (worker.joinable()) worker.join(); }
};
PulseStatus PULSE_CALL Activate(void* instance) noexcept { try { return static_cast<Plugin*>(instance)->Start(); } catch (...) { return PULSE_ERROR_INITIALIZATION; } }
void PULSE_CALL Deactivate(void* instance) noexcept { static_cast<Plugin*>(instance)->Stop(); }
void PULSE_CALL Destroy(void* instance) noexcept { delete static_cast<Plugin*>(instance); }
}
PULSE_PLUGIN_SDK_PREVIEW()
extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host, PulsePluginV1* out) noexcept {
    if (!host || !out || host->size < sizeof(*host) || out->size < sizeof(*out) || host->version != 1 || out->version != 1) return PULSE_ERROR_ABI;
    try { *out = {sizeof(*out), 1, new Plugin(*host), Activate, Deactivate, Destroy}; return PULSE_OK; } catch (...) { return PULSE_ERROR_INITIALIZATION; }
}

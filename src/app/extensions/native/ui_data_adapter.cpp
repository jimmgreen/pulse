#include "ui_data_adapter.h"
#include "ui_model.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <windows.h>

namespace pulse::extensions::native {
namespace {
std::atomic<uint64_t> next_data_id{0};
uint64_t Identity() {
    uint64_t old = next_data_id.load();
    while (old != UINT64_MAX)
        if (next_data_id.compare_exchange_weak(old, old + 1))
            return old + 1;
    return 0;
}
PulseStringView View(const std::string &text) { return {text.data(), static_cast<uint32_t>(text.size())}; }
bool Text(PulseStringView input, std::string &output, uint32_t limit) {
    if (input.length > limit || (input.length && !input.data))
        return false;
    output.assign(input.data ? input.data : "", input.length);
    return output.find('\0') == std::string::npos && (output.empty() ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, output.data(), static_cast<int>(output.size()), nullptr, 0));
}
bool Key(PulseStringView input, std::string &output, bool empty = false) {
    return Text(input, output, 64) && (empty || !output.empty()) &&
        std::all_of(output.begin(), output.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '.' || c == '-' || c == '_';
        });
}
} // namespace
struct UiDataAdapter::State : std::enable_shared_from_this<State> {
    struct Provider {
        uint64_t id;
        PulseUiDataRequestFn request;
        PulseUiDataCancelFn cancel;
        void *user;
    };
    struct Binding {
        uint64_t panel;
        std::shared_ptr<Provider> provider;
        UiNode node;
        UiDataSource source;
        std::set<std::string> commands;
    };
    struct Pending {
        std::shared_ptr<const Binding> binding;
        ui::UiDataCompletion completion;
    };
    const DWORD thread = GetCurrentThreadId();
    UiDataBackend backend;
    std::map<uint64_t, std::shared_ptr<Provider>> providers;
    std::map<std::string, std::shared_ptr<Binding>> bindings;
    std::mutex mutex;
    std::map<uint64_t, Pending> pending;
    bool stopped = false, changing = false;
    size_t callbacks = 0;
    explicit State(UiDataBackend value) : backend(std::move(value)) {}
    bool Valid() const {
        return GetCurrentThreadId() == thread && !stopped && !changing && !callbacks && backend.alive && backend.alive();
    }
    struct Change {
        bool &flag;
        explicit Change(bool &value) : flag(value) { flag = true; }
        ~Change() { flag = false; }
    };
    template<class F> static PulseStatus Guard(void *context, F operation) noexcept {
        if (!context)
            return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<State *>(context)->shared_from_this();
            return self->Valid() ? operation(*self) : PULSE_ERROR_STATE;
        } catch (...) {
            return PULSE_ERROR_INITIALIZATION;
        }
    }
    void Cancel(uint64_t id) {
        Pending old;
        {
            std::lock_guard lock(mutex);
            const auto found = pending.find(id);
            if (found == pending.end())
                return;
            old = std::move(found->second);
            pending.erase(found);
        }
        const auto provider = old.binding->provider;
        ++callbacks;
        try { provider->cancel(provider->user, id); } catch (...) {}
        --callbacks;
    }
    void Reset() {
        bindings.clear();
        std::vector<uint64_t> ids;
        {
            std::lock_guard lock(mutex);
            for (const auto &[id, work] : pending) {
                (void)work;
                ids.push_back(id);
            }
        }
        for (auto id : ids)
            Cancel(id);
    }
    static PulseStatus PULSE_CALL Register(void *context, PulseUiDataRequestFn request,
        PulseUiDataCancelFn cancel, void *user, uint64_t *id) noexcept {
        if (id)
            *id = 0;
        return Guard(context, [&](State &self) -> PulseStatus {
            if (!id || !request || !cancel || self.providers.size() >= 16)
                return PULSE_ERROR_ARGUMENT;
            const auto value = Identity();
            if (!value)
                return PULSE_ERROR_STATE;
            self.providers.emplace(value, std::make_shared<Provider>(Provider{value, request, cancel, user}));
            *id = value;
            return PULSE_OK;
        });
    }
    static PulseStatus PULSE_CALL Unregister(void *context, uint64_t provider) noexcept {
        return Guard(context, [&](State &self) -> PulseStatus {
            if (!self.providers.erase(provider))
                return PULSE_ERROR_STATE;
            Change change(self.changing);
            std::vector<std::string> nodes;
            for (const auto &[id, binding] : self.bindings)
                if (binding->provider->id == provider)
                    nodes.push_back(id);
            for (const auto &id : nodes) {
                self.bindings.erase(id);
                self.backend.bind(id, std::nullopt);
            }
            return PULSE_OK;
        });
    }
    static PulseStatus PULSE_CALL Bind(void *context, uint64_t panel, PulseStringView node,
        uint64_t provider, const PulseUiDataBinding *input) noexcept {
        return Guard(context, [&](State &self) -> PulseStatus {
            if (!self.backend.valid(panel))
                return PULSE_ERROR_STATE;
            std::string id;
            if (!input || input->size != sizeof(*input) || !input->page_size || input->page_size > 256 ||
                !Key(node, id) || self.bindings.contains(id) || self.bindings.size() >= 16)
                return PULSE_ERROR_ARGUMENT;
            const auto found = self.providers.find(provider);
            if (found == self.providers.end())
                return PULSE_ERROR_STATE;
            const auto target = self.backend.node(id);
            if (!target || !target->rows.empty() || target->data_source)
                return PULSE_ERROR_ARGUMENT;
            auto value = std::make_shared<Binding>();
            value->panel = panel;
            value->provider = found->second;
            value->node = *target;
            value->commands = self.backend.commands();
            auto &source = value->source;
            source.source = "native.provider." + std::to_string(provider);
            source.page_size = static_cast<int>(input->page_size);
            const std::pair<PulseStringView, std::string *> keys[] = {
                {input->filter_state, &source.filter_state}, {input->sort_state, &source.sort_state},
                {input->status_state, &source.status_state}, {input->error_state, &source.error_state},
                {input->progress_state, &source.progress_state}};
            std::set<std::string> used;
            if (!target->state.empty())
                used.insert(target->state);
            for (const auto &[view, output] : keys) {
                if (!Key(view, *output, true))
                    return PULSE_ERROR_ARGUMENT;
                if (!output->empty() && (!used.insert(*output).second || !self.backend.state(*output)))
                    return PULSE_ERROR_ARGUMENT;
            }
            if (!source.sort_state.empty()) {
                const auto sort = self.backend.state(source.sort_state);
                if (sort != "nameAsc" && sort != "nameDesc")
                    return PULSE_ERROR_ARGUMENT;
            }
            Change change(self.changing);
            const bool bound = self.backend.bind(id, source);
            if (!bound)
                return PULSE_ERROR_ARGUMENT;
            self.bindings.emplace(id, std::move(value));
            return PULSE_OK;
        });
    }
    static PulseStatus PULSE_CALL Unbind(void *context, uint64_t panel, PulseStringView node) noexcept {
        return Guard(context, [&](State &self) -> PulseStatus {
            if (!self.backend.valid(panel))
                return PULSE_ERROR_STATE;
            std::string id;
            if (!Key(node, id))
                return PULSE_ERROR_ARGUMENT;
            auto found = self.bindings.find(id);
            if (found == self.bindings.end() || found->second->panel != panel)
                return PULSE_ERROR_STATE;
            self.bindings.erase(found);
            Change change(self.changing);
            const bool result = self.backend.bind(id, std::nullopt);
            return result ? PULSE_OK : PULSE_ERROR_STATE;
        });
    }
    static PulseStatus PULSE_CALL RequestData(void *context, uint64_t panel, PulseStringView node, uint32_t operation) noexcept {
        return Guard(context, [&](State &self) -> PulseStatus {
            if (!self.backend.valid(panel))
                return PULSE_ERROR_STATE;
            std::string id;
            if (!Key(node, id) || operation > PULSE_UI_DATA_CANCEL)
                return PULSE_ERROR_ARGUMENT;
            auto found = self.bindings.find(id);
            if (found == self.bindings.end() || found->second->panel != panel)
                return PULSE_ERROR_STATE;
            const char *name = operation == PULSE_UI_DATA_REFRESH ? "refresh" :
                operation == PULSE_UI_DATA_NEXT_PAGE ? "nextPage" : "cancel";
            return self.backend.request(id, name) ? PULSE_OK : PULSE_ERROR_STATE;
        });
    }
    static bool ConvertPage(const Binding &binding, const PulseUiDataPage *input, ui::UiDataPage &page) {
        if (!input || input->size != sizeof(*input) || input->status > PULSE_UI_DATA_ERROR ||
            input->mode > PULSE_UI_DATA_DELTA || input->progress > 100 ||
            input->row_count > static_cast<uint32_t>(binding.source.page_size) ||
            (input->row_count && !input->rows) || input->removed_count > 256 ||
            (input->removed_count && !input->removed_ids) ||
            !Text(input->revision, page.revision, 128) || !Text(input->base_revision, page.base_revision, 128) ||
            !Text(input->next_cursor, page.next_cursor, 256) || !Text(input->error, page.error, 1024))
            return false;
        page.status = input->status == PULSE_UI_DATA_LOADING ? "loading" : input->status == PULSE_UI_DATA_READY ? "ready" : "error";
        page.progress = static_cast<int>(input->progress);
        page.delta = input->mode == PULSE_UI_DATA_DELTA;
        if (input->status != PULSE_UI_DATA_READY)
            return !input->row_count && !input->removed_count && !page.delta && page.base_revision.empty() &&
                page.revision.empty() && page.next_cursor.empty() &&
                (input->status == PULSE_UI_DATA_ERROR ? !page.error.empty() : page.error.empty());
        if (page.revision.empty() || !page.error.empty() ||
            (page.delta ? page.base_revision.empty() || !page.next_cursor.empty() : !page.base_revision.empty() || input->removed_count))
            return false;
        std::set<std::string> removed;
        for (uint32_t i = 0; i < input->removed_count; ++i) {
            std::string id;
            if (!Key(input->removed_ids[i], id) || !removed.insert(id).second)
                return false;
            page.removed_ids.push_back(std::move(id));
        }
        PulseUiModelNode root{};
        root.size = sizeof(root);
        root.type = static_cast<uint32_t>(binding.node.type);
        root.id = View(binding.node.id);
        root.rows = {1, input->rows, input->row_count};
        root.state = {1, {"selection", 9}};
        const PulseUiState selection{{"selection", 9}, {nullptr, 0}};
        std::vector<PulseUiModelColumn> columns;
        for (const auto &column : binding.node.data_columns)
            columns.push_back({sizeof(PulseUiModelColumn), View(column.id), View(column.text), {1, column.width}});
        if (!columns.empty())
            root.data_columns = {1, columns.data(), static_cast<uint32_t>(columns.size())};
        std::vector<PulseStringView> commands;
        for (const auto &command : binding.commands)
            commands.push_back(View(command));
        PulseUiModelTree tree{};
        tree.size = sizeof(tree);
        tree.root = &root;
        tree.states = &selection;
        tree.state_count = 1;
        tree.commands = commands.data();
        tree.command_count = static_cast<uint32_t>(commands.size());
        UiTree converted;
        std::set<std::string> validated_commands;
        std::string error;
        if (ConvertUiModel(&tree, converted, validated_commands, error) != PULSE_OK)
            return false;
        for (const auto &row : converted.root.rows)
            if (removed.contains(row.id))
                return false;
        size_t bytes = page.revision.size() + page.base_revision.size() + page.next_cursor.size() + page.error.size();
        for (const auto &id : page.removed_ids)
            bytes += id.size();
        std::function<void(const std::vector<UiDataRow> &)> count = [&](const auto &rows) {
            for (const auto &row : rows) {
                bytes += row.id.size() + row.text.size() + row.glyph.size() + row.command.size();
                for (const auto &cell : row.cells)
                    bytes += cell.size();
                count(row.children);
            }
        };
        count(converted.root.rows);
        if (bytes > 65536)
            return false;
        page.rows = std::move(converted.root.rows);
        return true;
    }
    static PulseStatus PULSE_CALL Publish(void *context, uint64_t id, const PulseUiDataPage *input) noexcept {
        if (!context || !id)
            return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<State *>(context)->shared_from_this();
            std::shared_ptr<const Binding> binding;
            {
                std::lock_guard lock(self->mutex);
                const auto found = self->pending.find(id);
                if (self->stopped || found == self->pending.end())
                    return PULSE_ERROR_STATE;
                binding = found->second.binding;
            }
            ui::UiDataPage page;
            if (!ConvertPage(*binding, input, page))
                return PULSE_ERROR_ARGUMENT;
            std::lock_guard lock(self->mutex);
            const auto found = self->pending.find(id);
            if (self->stopped || found == self->pending.end())
                return PULSE_ERROR_STATE;
            // Host completion only queues a copy and posts a window message.
            found->second.completion(std::move(page));
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_INITIALIZATION;
        }
    }
};
UiDataAdapter::UiDataAdapter(UiDataBackend backend) : state_(std::make_shared<State>(std::move(backend))) {}
UiDataAdapter::~UiDataAdapter() { Stop(); }
PulseStatus UiDataAdapter::Query(uint32_t version, void *table, uint32_t size) {
    if (version != PULSE_UI_DATA_VERSION || !table || size != sizeof(PulseUiDataV1))
        return PULSE_ERROR_ABI;
    if (!state_->Valid())
        return PULSE_ERROR_STATE;
    const PulseUiDataV1 value{sizeof(value), PULSE_UI_DATA_VERSION, state_.get(), State::Register, State::Unregister,
        State::Bind, State::Unbind, State::RequestData, State::Publish};
    std::memcpy(table, &value, sizeof(value));
    return PULSE_OK;
}
ui::UiDataCancel UiDataAdapter::Request(const ui::UiDataRequest &request, ui::UiDataCompletion completion) {
    auto self = state_;
    const auto found = self->bindings.find(request.node_id);
    if (!self->Valid() || found == self->bindings.end() || !self->backend.valid(found->second->panel) ||
        request.source != found->second->source.source) {
        ui::UiDataPage page;
        page.status = "error";
        page.error = "provider_unavailable";
        completion(std::move(page));
        return {};
    }
    const auto binding = found->second;
    const auto id = Identity();
    if (!id)
        return {};
    {
        std::lock_guard lock(self->mutex);
        self->pending.emplace(id, State::Pending{binding, std::move(completion)});
    }
    const PulseUiDataRequest input{sizeof(input), static_cast<uint32_t>(request.page_size), id,
        binding->provider->id, binding->panel, View(request.node_id), View(request.filter), View(request.sort), View(request.cursor)};
    ++self->callbacks;
    try {
        binding->provider->request(binding->provider->user, &input);
    } catch (...) {
        // A failing author callback leaves a visible error, not a permanently loading page.
        ui::UiDataPage page;
        page.status = "error";
        page.error = "provider_callback_failed";
        std::lock_guard lock(self->mutex);
        const auto active = self->pending.find(id);
        if (active != self->pending.end())
            active->second.completion(std::move(page));
    }
    --self->callbacks;
    return [weak = std::weak_ptr<State>(self), id] {
        if (auto current = weak.lock())
            current->Cancel(id);
    };
}
void UiDataAdapter::ResetBindings() { state_->Reset(); }
void UiDataAdapter::Pause(bool value) { state_->changing = value; }
void UiDataAdapter::Stop() {
    if (GetCurrentThreadId() != state_->thread)
        return;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopped = true;
    }
    state_->Reset();
    state_->providers.clear();
}
bool UiDataAdapter::InCallback() const { return state_->callbacks != 0; }
} // namespace pulse::extensions::native

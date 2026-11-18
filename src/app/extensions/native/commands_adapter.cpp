#include "commands_adapter.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <stdexcept>
namespace pulse::extensions::native {
namespace {
constexpr int kFirst = 0x20000000, kLast = 0x2fffffff;
std::atomic<int> next_token{kFirst};
std::string Copy(PulseStringView value, size_t limit, bool identifier = false) {
    if (!value.data || !value.length || value.length > limit) throw std::invalid_argument("text");
    std::string text(value.data, value.length);
    if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0))
        throw std::invalid_argument("utf8");
    if (identifier && !std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    })) throw std::invalid_argument("id");
    return text;
}
PulseStringView View(const std::string& text) { return {text.data(), static_cast<uint32_t>(text.size())}; }
struct Selection {
    std::string directory;
    uint64_t revision = 0;
    struct Entry { std::string path, name; uint32_t flags; };
    std::vector<Entry> entries;
    bool valid = false;
    static void PULSE_CALL Capture(void* user, const PulseContextSelectionSnapshot* source) {
        auto& result = *static_cast<Selection*>(user);
        try {
            result.directory.assign(source->directory.data, source->directory.length);
            result.revision = source->selection_revision;
            for (uint32_t i = 0; i < source->entry_count; ++i) {
                const auto& row = source->entries[i];
                result.entries.push_back({std::string(row.path.data, row.path.length), std::string(row.name.data, row.name.length), row.flags});
            }
            result.valid = true;
        } catch (...) { result.valid = false; }
    }
};
}
struct CommandsAdapter::State : std::enable_shared_from_this<State> {
    struct Command {
        CommandInfo info;
        uint32_t kinds = 0, minimum = 0, maximum = 0;
        bool exclude = false;
        std::vector<std::string> extensions;
        PulseCommandFn callback = nullptr;
        void* user = nullptr;
    };
    static inline std::map<int, std::weak_ptr<State>> registry;
    HWND owner;
    std::string package;
    PulseContextV1 context;
    std::function<bool()> live;
    DWORD thread = GetCurrentThreadId();
    bool closed = false, active = false;
    std::map<int, Command> commands;
    std::shared_ptr<const Selection> cached_selection;
    uint64_t cached_revision = 0;
    State(HWND window, std::string id, PulseContextV1 table, std::function<bool()> valid)
        : owner(window), package(std::move(id)), context(table), live(std::move(valid)) {}
    bool Ready() const { return GetCurrentThreadId() == thread && !closed && live && live(); }
    std::shared_ptr<const Selection> Capture(bool force = false) {
        struct Summary { uint64_t revision = 0; bool delivered = false, loading = false; } summary;
        const bool summary_ok = Ready() && context.get && context.get(context.context,
            [](void* user, const PulseContextSnapshot* value) {
                auto& result = *static_cast<Summary*>(user);
                result.revision = value->selection_revision; result.loading = value->loading != 0; result.delivered = true;
            }, &summary) == PULSE_OK && summary.delivered;
        if (!force && summary_ok && !summary.loading && cached_selection && cached_revision == summary.revision)
            return cached_selection;
        auto result = std::make_shared<Selection>();
        if (!Ready() || !context.get_selection || context.get_selection(context.context, Selection::Capture, result.get()) != PULSE_OK)
            result->valid = false;
        cached_selection = result;
        cached_revision = summary.revision;
        return result;
    }
    static bool Matches(const Command& command, const Selection& selected) {
        if (!selected.valid || selected.entries.size() < command.minimum || selected.entries.size() > command.maximum) return false;
        for (const auto& entry : selected.entries) {
            const bool folder = (entry.flags & PULSE_CONTEXT_ENTRY_DIRECTORY) != 0;
            if (!(command.kinds & (folder ? PULSE_COMMAND_DIRECTORIES : PULSE_COMMAND_FILES))) return false;
            if (!command.extensions.empty()) {
                if (folder) return false;
                const auto dot = entry.name.rfind('.');
                std::string extension = dot == std::string::npos ? "" : entry.name.substr(dot);
                for (auto& c : extension) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                const bool contains = std::find(command.extensions.begin(), command.extensions.end(), extension) != command.extensions.end();
                if (contains == command.exclude) return false;
            }
        }
        return true;
    }
    static PulseStatus PULSE_CALL Register(void* context, const PulseCommandDescriptor* source, PulseCommandId* out) {
        if (out) *out = 0;
        try {
            auto* self = static_cast<State*>(context);
            if (!self || !source || !out || source->size < sizeof(*source) || !source->invoke) return PULSE_ERROR_ARGUMENT;
            if (!self->Ready()) return PULSE_ERROR_STATE;
            if (self->commands.size() >= 32) return PULSE_ERROR_UNAVAILABLE;
            if (source->icon > PULSE_COMMAND_ICON_EXTRACT || !source->kinds || (source->kinds & ~3u) ||
                source->max_selection > 4096 || source->min_selection > source->max_selection || source->extension_count > 32 ||
                (source->extension_count && !source->extensions) || source->exclude_extensions > 1 || (source->modifiers & ~7u)) return PULSE_ERROR_ARGUMENT;
            const auto key = source->virtual_key;
            if (key && !((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || (key >= VK_F1 && key <= VK_F24))) return PULSE_ERROR_ARGUMENT;
            if ((!key && source->modifiers) || (key && key < VK_F1 && !(source->modifiers & (PULSE_COMMAND_CTRL | PULSE_COMMAND_ALT)))) return PULSE_ERROR_ARGUMENT;
            Command command;
            command.info.identity = "native:" + self->package + ":" + Copy(source->id, 64, true);
            command.info.title = Copy(source->title, 128);
            command.info.icon = source->icon; command.info.key = key; command.info.modifiers = source->modifiers;
            command.kinds = source->kinds; command.minimum = source->min_selection; command.maximum = source->max_selection;
            command.exclude = source->exclude_extensions != 0; command.callback = source->invoke; command.user = source->user;
            for (uint32_t i = 0; i < source->extension_count; ++i) {
                auto ext = Copy(source->extensions[i], 32, true);
                if (ext.size() < 2 || ext[0] != '.' || std::any_of(ext.begin(), ext.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) return PULSE_ERROR_ARGUMENT;
                command.extensions.push_back(std::move(ext));
            }
            for (const auto& [token, weak] : registry) {
                auto existing = weak.lock();
                if (!existing || existing->owner != self->owner || !existing->Ready()) continue;
                const auto found = existing->commands.find(token);
                if (found != existing->commands.end() && (found->second.info.identity == command.info.identity ||
                    (key && found->second.info.key == key && found->second.info.modifiers == source->modifiers))) return PULSE_ERROR_STATE;
            }
            int token = next_token.load();
            while (token < kLast && !next_token.compare_exchange_weak(token, token + 1)) {}
            if (token >= kLast) return PULSE_ERROR_UNAVAILABLE;
            command.info.token = token;
            self->commands.emplace(token, std::move(command));
            try { registry.emplace(token, self->shared_from_this()); }
            catch (...) { self->commands.erase(token); throw; }
            *out = static_cast<PulseCommandId>(token);
            return PULSE_OK;
        } catch (...) { return PULSE_ERROR_ARGUMENT; }
    }
    static PulseStatus PULSE_CALL Unregister(void* context, PulseCommandId id) {
        auto* self = static_cast<State*>(context);
        if (!self || id < kFirst || id >= kLast) return PULSE_ERROR_ARGUMENT;
        if (GetCurrentThreadId() != self->thread || self->closed) return PULSE_ERROR_STATE;
        if (!self->commands.erase(static_cast<int>(id))) return PULSE_ERROR_STATE;
        registry.erase(static_cast<int>(id)); return PULSE_OK;
    }
    void Close() { closed = true; cached_selection.reset(); for (const auto& [id, command] : commands) { (void)command; registry.erase(id); } commands.clear(); }
};
CommandsAdapter::CommandsAdapter(HWND owner, std::string package, PulseContextV1 context, std::function<bool()> live)
    : state_(std::make_shared<State>(owner, std::move(package), context, std::move(live))) {}
CommandsAdapter::~CommandsAdapter() { Close(); }
void CommandsAdapter::Close() noexcept { state_->Close(); }
bool CommandsAdapter::CallbackActive() const noexcept { return state_->active; }
PulseStatus CommandsAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    if (version != PULSE_COMMANDS_VERSION_1) return PULSE_ERROR_ABI;
    if (!out || size < sizeof(PulseCommandsV1)) return PULSE_ERROR_ARGUMENT;
    const PulseCommandsV1 table{sizeof(table), PULSE_COMMANDS_VERSION_1, state_.get(), State::Register, State::Unregister};
    std::memcpy(out, &table, sizeof(table)); return PULSE_OK;
}
std::vector<CommandInfo> CommandsAdapter::Entries(HWND owner) {
    std::vector<CommandInfo> result;
    std::shared_ptr<const Selection> selection;
    const auto registry = State::registry;
    for (const auto& [token, weak] : registry) {
        const auto self = weak.lock();
        if (!self || self->owner != owner || !self->Ready()) continue;
        if (!selection) selection = self->Capture();
        if (!self->Ready()) continue;
        const auto found = self->commands.find(token);
        if (found == self->commands.end()) continue;
        auto info = found->second.info;
        info.enabled = State::Matches(found->second, *selection);
        result.push_back(std::move(info));
    }
    return result;
}
std::string CommandsAdapter::Identity(HWND owner, int token) {
    const auto found = State::registry.find(token);
    if (found == State::registry.end()) return {};
    const auto state = found->second.lock();
    if (!state || state->owner != owner || !state->Ready()) return {};
    const auto item = state->commands.find(token);
    return item == state->commands.end() ? std::string{} : item->second.info.identity;
}
bool CommandsAdapter::Invoke(HWND owner, int token) {
    if (token < kFirst || token >= kLast) return false;
    const auto found = State::registry.find(token);
    if (found == State::registry.end()) return true;
    const auto self = found->second.lock();
    if (!self || self->owner != owner || !self->Ready() || self->active) return true;
    const auto item = self->commands.find(token);
    if (item == self->commands.end()) return true;
    const auto command = item->second;
    const auto selected = self->Capture(true);
    if (!self->Ready() || !self->commands.contains(token) || !State::Matches(command, *selected)) return true;
    std::vector<PulseContextSelectionEntry> rows;
    rows.reserve(selected->entries.size());
    for (const auto& row : selected->entries) rows.push_back({sizeof(PulseContextSelectionEntry), row.flags, View(row.path), View(row.name)});
    const PulseContextSelectionSnapshot snapshot{sizeof(snapshot), selected->revision, View(selected->directory), rows.data(), static_cast<uint32_t>(rows.size())};
    self->active = true;
    try { command.callback(command.user, static_cast<PulseCommandId>(token), &snapshot); }
    catch (...) { self->Close(); }
    self->active = false;
    return true;
}
bool CommandsAdapter::Shortcut(HWND owner, uint32_t key, uint32_t modifiers) {
    for (const auto& info : Entries(owner)) if (info.enabled && info.key == key && info.modifiers == modifiers) return Invoke(owner, info.token);
    return false;
}
}

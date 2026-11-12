#include "commands_adapter.h"
#include "crash_guard.h"
#include "../icon_registry.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <utility>
namespace pulse::extensions::native {
namespace {
constexpr int kFirst = 0x20000000, kLast = kFirstStaticCommandToken;
std::atomic<int> next_token{kFirst};

struct StaticKey {
    HWND owner = nullptr;
    size_t package = 0;
    std::string command_id;
    bool operator<(const StaticKey& other) const noexcept {
        return std::tie(owner, package, command_id) < std::tie(other.owner, other.package, other.command_id);
    }
};
struct StaticEntry {
    HWND owner = nullptr;
    StaticCommandTarget target;
};
std::mutex static_mutex;
int next_static_token = kFirstStaticCommandToken;
std::map<StaticKey, int> static_tokens_by_key;
std::map<int, StaticEntry> static_entries_by_token;

int EnsureStaticToken(HWND owner, size_t package, const std::string& package_id, const std::string& command_id) {
    std::lock_guard lock(static_mutex);
    const StaticKey key{owner, package, command_id};
    const std::string identity = "native:" + package_id + ":" + command_id;
    const auto found = static_tokens_by_key.find(key);
    if (found != static_tokens_by_key.end()) {
        auto it = static_entries_by_token.find(found->second);
        if (it != static_entries_by_token.end()) {
            it->second.target.package_id = package_id;
            it->second.target.identity = identity;
        }
        return found->second;
    }
    if (next_static_token > kLastStaticCommandToken) return 0;
    const int token = next_static_token++;
    static_tokens_by_key.emplace(key, token);
    static_entries_by_token.emplace(token, StaticEntry{owner, StaticCommandTarget{package, package_id, command_id, identity}});
    return token;
}
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
// Optional text: empty is allowed, otherwise the same rules as Copy.
std::string CopyOptional(PulseStringView value, size_t limit) {
    if (!value.length) return {};
    return Copy(value, limit);
}
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
        std::string detail; // template; {stem}/{base} resolve per selection
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
        if ((command.info.flags & PULSE_COMMAND_HIDDEN) != 0) return false;
        if (!selected.valid || selected.entries.size() < command.minimum || selected.entries.size() > command.maximum) return false;
        if (selected.entries.empty()) {
            return command.extensions.empty() || command.exclude || (command.kinds & PULSE_COMMAND_BACKGROUND) != 0;
        }
        for (const auto& entry : selected.entries) {
            const bool folder = (entry.flags & PULSE_CONTEXT_ENTRY_DIRECTORY) != 0;
            if (!(command.kinds & (folder ? PULSE_COMMAND_DIRECTORIES : PULSE_COMMAND_FILES))) return false;
            if (!command.extensions.empty()) {
                if (folder) {
                    if (!command.exclude) return false;
                    continue;
                }
                const auto dot = entry.name.rfind('.');
                std::string extension = dot == std::string::npos ? "" : entry.name.substr(dot);
                for (auto& c : extension) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                const bool contains = std::find(command.extensions.begin(), command.extensions.end(), extension) != command.extensions.end();
                if (contains == command.exclude) return false;
            }
        }
        return true;
    }
    // {stem}: the single entry's name without its last extension (directories keep
    // the whole name); {base}: {stem} for one entry, else the directory's name.
    static std::string Detail(const std::string& pattern, const Selection& selected) {
        if (pattern.empty() || !selected.valid) return {};
        const auto stem = [](const Selection::Entry& entry) {
            if (entry.flags & PULSE_CONTEXT_ENTRY_DIRECTORY) return entry.name;
            const auto dot = entry.name.rfind('.');
            return dot == std::string::npos || dot == 0 ? entry.name : entry.name.substr(0, dot);
        };
        const bool single = selected.entries.size() == 1;
        std::string base;
        if (single) base = stem(selected.entries.front());
        else {
            std::string folder = selected.directory;
            while (!folder.empty() && (folder.back() == '\\' || folder.back() == '/')) folder.pop_back();
            const auto slash = folder.find_last_of("\\/");
            base = slash == std::string::npos ? folder : folder.substr(slash + 1);
        }
        std::string out;
        for (size_t i = 0; i < pattern.size();) {
            if (pattern.compare(i, 6, "{stem}") == 0) {
                if (!single) return {};
                out += base; i += 6;
            } else if (pattern.compare(i, 6, "{base}") == 0) {
                if (base.empty()) return {};
                out += base; i += 6;
            } else out += pattern[i++];
        }
        return out;
    }
    PulseStatus BuildCommand(const PulseCommandDescriptor* source, Command& command) const {
        if (!source || source->size < PULSE_COMMAND_DESCRIPTOR_SIZE_V1 || !source->invoke) return PULSE_ERROR_ARGUMENT;
        const bool has_v2 = source->size >= PULSE_COMMAND_DESCRIPTOR_SIZE_V2;
        const bool has_v3 = source->size >= sizeof(*source);
        if (!icons::GlobalRegistry().Exists(source->icon) || !source->kinds || (source->kinds & ~7u) ||
            source->max_selection > 4096 || source->min_selection > source->max_selection ||
            (source->kinds == PULSE_COMMAND_BACKGROUND && source->min_selection > 0) ||
            source->extension_count > 32 || (source->extension_count && !source->extensions) ||
            source->exclude_extensions > 1 || (source->modifiers & ~7u)) return PULSE_ERROR_ARGUMENT;
        const auto key = source->virtual_key;
        if (key && !((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || (key >= VK_F1 && key <= VK_F24))) return PULSE_ERROR_ARGUMENT;
        if ((!key && source->modifiers) || (key && key < VK_F1 && !(source->modifiers & (PULSE_COMMAND_CTRL | PULSE_COMMAND_ALT)))) return PULSE_ERROR_ARGUMENT;
        command = {};
        command.info.identity = "native:" + package + ":" + Copy(source->id, 64, true);
        command.info.title = Copy(source->title, 128);
        command.info.icon = source->icon;
        command.info.key = key;
        command.info.modifiers = source->modifiers;
        command.info.package = package;
        if (has_v2) {
            if (source->flags & ~static_cast<uint32_t>(PULSE_COMMAND_ALL_FLAGS)) return PULSE_ERROR_ARGUMENT;
            command.detail = CopyOptional(source->detail, 160);
            command.info.group = CopyOptional(source->group, 32);
            command.info.flags = source->flags;
        }
        if (has_v3) {
            if (source->reserved_v3 != 0) return PULSE_ERROR_ARGUMENT;
            const auto icon_name = CopyOptional(source->icon_name, 96);
            if (!icon_name.empty()) {
                const auto resolved = icons::GlobalRegistry().ResolveName(package, icon_name);
                if (!resolved) return PULSE_ERROR_ARGUMENT;
                command.info.icon = *resolved;
            }
            if (source->group_icon != 0) {
                if (!icons::GlobalRegistry().Exists(source->group_icon)) return PULSE_ERROR_ARGUMENT;
                command.info.group_icon = source->group_icon;
            }
        }
        command.kinds = source->kinds;
        command.minimum = source->min_selection;
        command.maximum = source->max_selection;
        command.exclude = source->exclude_extensions != 0;
        command.callback = source->invoke;
        command.user = source->user;
        for (uint32_t i = 0; i < source->extension_count; ++i) {
            auto ext = Copy(source->extensions[i], 32, true);
            if (ext.size() < 2 || ext[0] != '.' || std::any_of(ext.begin(), ext.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) return PULSE_ERROR_ARGUMENT;
            command.extensions.push_back(std::move(ext));
        }
        return PULSE_OK;
    }
    bool HasConflict(const Command& command, int ignore_token) const {
        for (const auto& [token, weak] : registry) {
            if (token == ignore_token) continue;
            auto existing = weak.lock();
            if (!existing || existing->owner != owner || !existing->Ready()) continue;
            const auto found = existing->commands.find(token);
            if (found != existing->commands.end() &&
                (found->second.info.identity == command.info.identity ||
                 (command.info.key && found->second.info.key == command.info.key &&
                  found->second.info.modifiers == command.info.modifiers))) {
                return true;
            }
        }
        return false;
    }
    static void NotifyOwnerChanged(HWND owner) noexcept {
        if (!owner) return;
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32) return;
        using IsWindowFn = BOOL(WINAPI*)(HWND);
        using InvalidateRectFn = BOOL(WINAPI*)(HWND, const RECT*, BOOL);
        const auto is_window = reinterpret_cast<IsWindowFn>(GetProcAddress(user32, "IsWindow"));
        const auto invalidate = reinterpret_cast<InvalidateRectFn>(GetProcAddress(user32, "InvalidateRect"));
        if (is_window && invalidate && is_window(owner)) invalidate(owner, nullptr, FALSE);
    }
    static PulseStatus PULSE_CALL Register(void* context, const PulseCommandDescriptor* source, PulseCommandId* out) {
        if (out) *out = 0;
        try {
            auto* self = static_cast<State*>(context);
            if (!self || !source || !out) return PULSE_ERROR_ARGUMENT;
            if (!self->Ready()) return PULSE_ERROR_STATE;
            if (self->commands.size() >= 32) return PULSE_ERROR_UNAVAILABLE;
            Command command;
            const auto built = self->BuildCommand(source, command);
            if (built != PULSE_OK) return built;
            if (self->HasConflict(command, 0)) return PULSE_ERROR_STATE;
            int token = next_token.load();
            while (token < kLast && !next_token.compare_exchange_weak(token, token + 1)) {}
            if (token >= kLast) return PULSE_ERROR_UNAVAILABLE;
            command.info.token = token;
            self->commands.emplace(token, std::move(command));
            try { registry.emplace(token, self->shared_from_this()); }
            catch (...) { self->commands.erase(token); throw; }
            *out = static_cast<PulseCommandId>(token);
            NotifyOwnerChanged(self->owner);
            return PULSE_OK;
        } catch (...) { return PULSE_ERROR_ARGUMENT; }
    }
    static PulseStatus PULSE_CALL UpdateCommand(void* context, PulseCommandId id, const PulseCommandDescriptor* source) {
        try {
            auto* self = static_cast<State*>(context);
            if (!self || !source || id < kFirst || id >= kLast) return PULSE_ERROR_ARGUMENT;
            if (!self->Ready()) return PULSE_ERROR_STATE;
            const int token = static_cast<int>(id);
            const auto it = self->commands.find(token);
            if (it == self->commands.end()) return PULSE_ERROR_STATE;
            Command updated;
            const auto built = self->BuildCommand(source, updated);
            if (built != PULSE_OK) return built;
            if (self->HasConflict(updated, token)) return PULSE_ERROR_STATE;
            updated.info.token = token;
            it->second = std::move(updated);
            NotifyOwnerChanged(self->owner);
            return PULSE_OK;
        } catch (...) { return PULSE_ERROR_ARGUMENT; }
    }
    static PulseStatus PULSE_CALL SetCommandFlags(void* context, PulseCommandId id, uint32_t flags) {
        auto* self = static_cast<State*>(context);
        if (!self || id < kFirst || id >= kLast || (flags & ~static_cast<uint32_t>(PULSE_COMMAND_ALL_FLAGS)) != 0)
            return PULSE_ERROR_ARGUMENT;
        if (!self->Ready()) return PULSE_ERROR_STATE;
        const auto it = self->commands.find(static_cast<int>(id));
        if (it == self->commands.end()) return PULSE_ERROR_STATE;
        it->second.info.flags = flags;
        NotifyOwnerChanged(self->owner);
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Unregister(void* context, PulseCommandId id) {
        auto* self = static_cast<State*>(context);
        if (!self || id < kFirst || id >= kLast) return PULSE_ERROR_ARGUMENT;
        if (GetCurrentThreadId() != self->thread || self->closed) return PULSE_ERROR_STATE;
        if (!self->commands.erase(static_cast<int>(id))) return PULSE_ERROR_STATE;
        registry.erase(static_cast<int>(id));
        NotifyOwnerChanged(self->owner);
        return PULSE_OK;
    }
    void Close() { closed = true; cached_selection.reset(); for (const auto& [id, command] : commands) { (void)command; registry.erase(id); } commands.clear(); }
};
CommandsAdapter::CommandsAdapter(HWND owner, std::string package, PulseContextV1 context, std::function<bool()> live)
    : state_(std::make_shared<State>(owner, std::move(package), context, std::move(live))) {}
CommandsAdapter::~CommandsAdapter() { Close(); }
void CommandsAdapter::Close() noexcept { state_->Close(); }
bool CommandsAdapter::CallbackActive() const noexcept { return state_->active; }
PulseStatus CommandsAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    if (version == PULSE_COMMANDS_VERSION_1) {
        if (!out || size < sizeof(PulseCommandsV1)) return PULSE_ERROR_ARGUMENT;
        const PulseCommandsV1 table{sizeof(table), PULSE_COMMANDS_VERSION_1, state_.get(), State::Register, State::Unregister};
        std::memcpy(out, &table, sizeof(table));
        return PULSE_OK;
    }
    if (version == PULSE_COMMANDS_VERSION_2) {
        if (!out || size < sizeof(PulseCommandsV2)) return PULSE_ERROR_ARGUMENT;
        const PulseCommandsV2 table{sizeof(table), PULSE_COMMANDS_VERSION_2, state_.get(),
                                    State::Register, State::Unregister, State::UpdateCommand, State::SetCommandFlags};
        std::memcpy(out, &table, sizeof(table));
        return PULSE_OK;
    }
    return PULSE_ERROR_ABI;
}
std::vector<PackageCommandSummary> CommandsAdapter::LivePackageCommands(HWND owner, const std::string& package) {
    std::vector<PackageCommandSummary> result;
    const auto registry = State::registry;
    for (const auto& [token, weak] : registry) {
        const auto self = weak.lock();
        if (!self || self->owner != owner || self->package != package || !self->Ready()) continue;
        const auto found = self->commands.find(token);
        if (found == self->commands.end()) continue;
        result.push_back(PackageCommandSummary{
            found->second.info.identity,
            found->second.info.title,
            found->second.info.icon,
            (found->second.info.flags & PULSE_COMMAND_HIDDEN) != 0,
        });
    }
    return result;
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
        if (info.enabled) info.detail = State::Detail(found->second.detail, *selection);
        result.push_back(std::move(info));
    }
    return result;
}
std::vector<CommandInfo> CommandsAdapter::Preview(HWND owner, const std::string& package,
                                                   const std::vector<PreviewEntry>& entries, const std::string& directory) {
    Selection sample;
    sample.directory = directory;
    sample.valid = !entries.empty() || !directory.empty();
    for (const auto& entry : entries)
        sample.entries.push_back({directory + "\\" + entry.name, entry.name, entry.directory ? static_cast<uint32_t>(PULSE_CONTEXT_ENTRY_DIRECTORY) : 0u});
    std::vector<CommandInfo> result;
    const auto registry = State::registry;
    for (const auto& [token, weak] : registry) {
        const auto self = weak.lock();
        if (!self || self->owner != owner || self->package != package || !self->Ready()) continue;
        const auto found = self->commands.find(token);
        if (found == self->commands.end() || !State::Matches(found->second, sample)) continue;
        auto info = found->second.info;
        info.enabled = true;
        info.detail = State::Detail(found->second.detail, sample);
        result.push_back(std::move(info));
    }
    return result;
}
std::string CommandsAdapter::ResolveLocalizedText(const ManifestLocalizedText& text,
                                                  StaticCommandLanguage language) {
    switch (language) {
    case StaticCommandLanguage::ZhTW:
        if (!text.tw.empty()) return text.tw;
        if (!text.zh.empty()) return text.zh;
        return text.en;
    case StaticCommandLanguage::ZhCN:
        if (!text.zh.empty()) return text.zh;
        if (!text.en.empty()) return text.en;
        return text.tw;
    case StaticCommandLanguage::EnUS:
        if (!text.en.empty()) return text.en;
        if (!text.zh.empty()) return text.zh;
        return text.tw;
    }
    return text.en;
}
CommandInfo CommandsAdapter::EvaluateStaticContribution(HWND owner, size_t package,
                                                        const std::string& package_id,
                                                        const ManifestCommandContribution& contribution,
                                                        StaticCommandLanguage language,
                                                        const CommandSelection& selection) {
    State::Command command;
    command.info.token = EnsureStaticToken(owner, package, package_id, contribution.id);
    command.info.identity = "native:" + package_id + ":" + contribution.id;
    command.info.package = package_id;
    command.info.title = ResolveLocalizedText(contribution.title, language);
    command.info.group = ResolveLocalizedText(contribution.group, language);
    command.detail = ResolveLocalizedText(contribution.detail, language);
    command.info.flags = contribution.flags | (contribution.submenu ? static_cast<uint32_t>(PULSE_COMMAND_GROUP_SUBMENU) : 0u);
    command.info.icon = contribution.icon;
    if (!contribution.icon_name.empty()) {
        if (const auto resolved = icons::GlobalRegistry().ResolveName(package_id, contribution.icon_name)) {
            command.info.icon = *resolved;
        }
    }
    command.info.group_icon = contribution.group_icon;
    if (!contribution.group_icon_name.empty()) {
        if (const auto resolved = icons::GlobalRegistry().ResolveName(package_id, contribution.group_icon_name)) {
            command.info.group_icon = *resolved;
        }
    }
    command.info.key = contribution.virtual_key;
    command.info.modifiers = contribution.modifiers;
    command.kinds = contribution.kinds;
    command.minimum = contribution.min_selection;
    command.maximum = contribution.max_selection;
    command.exclude = contribution.exclude_extensions;
    command.extensions.reserve(contribution.extensions.size());
    for (std::string ext : contribution.extensions) {
        for (char& c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        command.extensions.push_back(std::move(ext));
    }

    Selection converted;
    converted.directory = selection.directory;
    converted.valid = selection.valid;
    converted.entries.reserve(selection.entries.size());
    for (const auto& entry : selection.entries) {
        converted.entries.push_back({
            entry.path,
            entry.name,
            entry.directory ? static_cast<uint32_t>(PULSE_CONTEXT_ENTRY_DIRECTORY) : 0u,
        });
    }

    command.info.enabled = command.info.token != 0 && State::Matches(command, converted);
    if (command.info.enabled) command.info.detail = State::Detail(command.detail, converted);
    return command.info;
}
std::optional<StaticCommandTarget> CommandsAdapter::LookupStaticToken(HWND owner, int token) {
    if (token < kFirstStaticCommandToken || token > kLastStaticCommandToken) return std::nullopt;
    std::lock_guard lock(static_mutex);
    const auto found = static_entries_by_token.find(token);
    if (found == static_entries_by_token.end() || found->second.owner != owner) return std::nullopt;
    return found->second.target;
}
void CommandsAdapter::ClearStaticOwner(HWND owner) {
    std::lock_guard lock(static_mutex);
    std::erase_if(static_tokens_by_key, [owner](const auto& item) { return item.first.owner == owner; });
    std::erase_if(static_entries_by_token, [owner](const auto& item) { return item.second.owner == owner; });
}
std::string CommandsAdapter::Identity(HWND owner, int token) {
    const auto found = State::registry.find(token);
    if (found != State::registry.end()) {
        const auto state = found->second.lock();
        if (!state || state->owner != owner || !state->Ready()) return {};
        const auto item = state->commands.find(token);
        return item == state->commands.end() ? std::string{} : item->second.info.identity;
    }
    if (const auto target = LookupStaticToken(owner, token)) return target->identity;
    return {};
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
    try {
        const crash_guard::CallScope call_scope(self->package.c_str());
        command.callback(command.user, static_cast<PulseCommandId>(token), &snapshot);
    }
    catch (...) { self->Close(); }
    self->active = false;
    return true;
}
bool CommandsAdapter::InvokeIdentity(HWND owner, const std::string& identity) {
    if (identity.empty()) return false;
    const auto registry = State::registry;
    for (const auto& [token, weak] : registry) {
        const auto self = weak.lock();
        if (!self || self->owner != owner || !self->Ready()) continue;
        const auto item = self->commands.find(token);
        if (item != self->commands.end() && item->second.info.identity == identity)
            return Invoke(owner, token);
    }
    return false;
}
bool CommandsAdapter::Shortcut(HWND owner, uint32_t key, uint32_t modifiers) {
    for (const auto& info : Entries(owner)) if (info.enabled && info.key == key && info.modifiers == modifiers) return Invoke(owner, info.token);
    return false;
}
}

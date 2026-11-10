// plugin.cpp - the official Pulse 7-Zip extension: context-menu commands for
// extracting, testing, compressing and checksums, plus archive previews.
#include <windows.h>
#include <pulse/client.hpp>
#include <pulse/commands.h>
#include <pulse/dialogs.h>
#include <pulse/dispatch.h>
#include <pulse/preview.h>
#include <pulse/staging.h>
#include <pulse/tasks.h>
#include "dialogs.h"
#include "engine.h"
#include "jobs.h"
#include "settings.h"
#include "text.h"
#include <array>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {
using pulse::sdk::View;
using T = text::Id;
namespace fs = std::filesystem;

enum class Command { Extract, To, Test, Quick, Add, Hash, Count };
constexpr const char* kFormatNames[] = {"7z", "zip", "tar", "tar.gz", "tar.xz", "tar.bz2"};
constexpr engine::Format kFormats[] = {engine::Format::SevenZip, engine::Format::Zip, engine::Format::Tar,
                                      engine::Format::TarGz, engine::Format::TarXz, engine::Format::TarBz2};
constexpr unsigned kLevels[] = {0, 1, 3, 5, 7, 9};
constexpr const char* kPreviewExtensions[] = {".7z", ".rar", ".tar", ".gz", ".tgz", ".xz", ".bz2", ".iso", ".cab", ".wim", ".001"};

struct Plugin {
    pulse::sdk::Host host;
    PulseCommandsV1 commands{};
    PulseStagingV1 staging{};
    PulseDialogsV1 dialogs_api{};
    PulseDispatchV1 dispatch_api{};
    PulsePreviewV1 preview{};
    std::array<pulse::sdk::CommandRegistration, static_cast<size_t>(Command::Count)> registrations;
    std::array<PulseCommandId, static_cast<size_t>(Command::Count)> ids{};
    std::array<std::string, static_cast<size_t>(Command::Count)> titles;
    std::vector<PulsePreviewProviderId> previews;
    std::unique_ptr<pulse::sdk::DispatchQueue> dispatch;
    std::unique_ptr<Dialogs> dialogs;
    std::unique_ptr<Jobs> jobs;
    std::unique_ptr<Settings> settings;
    explicit Plugin(const PulseHostV1& value) : host(value) {}
};

std::wstring Wide(PulseStringView value) {
    if (!value.data || !value.length) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data, static_cast<int>(value.length), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data, static_cast<int>(value.length), out.data(), n);
    return out;
}
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring BaseName(const std::vector<std::wstring>& paths) {
    if (paths.size() != 1) {
        const auto parent = fs::path(paths.front()).parent_path().filename().wstring();
        return parent.empty() ? L"Archive" : parent;
    }
    const fs::path path(paths.front());
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) return path.filename().wstring();
    const auto stem = path.stem().wstring();
    return stem.empty() ? path.filename().wstring() : stem;
}

PulseStatus PULSE_CALL Activate(void*) {
    return engine::SupportedFormats().empty() ? PULSE_ERROR_INITIALIZATION : PULSE_OK;
}
void PULSE_CALL Deactivate(void*) {}

struct Destination {
    PulseStatus status = PULSE_ERROR_STATE;
    std::string path;
    static void PULSE_CALL Chosen(void* user, PulseStatus status, PulseStringView path) {
        auto& self = *static_cast<Destination*>(user);
        self.status = status;
        try { if (status == PULSE_OK && path.data && path.length) self.path.assign(path.data, path.length); } catch (...) { self.status = PULSE_ERROR_STATE; }
    }
};

Jobs::ExtractMode ModeOf(const Settings& settings) {
    switch (settings.Mode()) {
    case Settings::ExtractMode::Folder: return Jobs::ExtractMode::Folder;
    case Settings::ExtractMode::Here: return Jobs::ExtractMode::Into;
    default: return Jobs::ExtractMode::Smart;
    }
}

// Everything comes from the extension settings: one click, no dialog, except
// "Extract to…" (a folder picker by design) and "Add to archive…" (options).
void Run(Plugin& plugin, Command command, const PulseContextSelectionSnapshot& selection) {
    if (!selection.entry_count || !selection.entries) return;
    std::vector<std::wstring> paths;
    for (uint32_t i = 0; i < selection.entry_count; ++i) {
        auto path = Wide(selection.entries[i].path);
        if (!path.empty()) paths.push_back(std::move(path));
    }
    if (paths.empty()) return;
    const auto parent = Utf8(fs::path(paths.front()).parent_path().wstring());
    auto& settings = *plugin.settings;
    auto& jobs = *plugin.jobs;
    switch (command) {
    case Command::Extract: {
        const auto fixed = settings.FixedFolder();
        for (const auto& path : paths)
            jobs.Extract(path, fixed.empty() ? Utf8(fs::path(path).parent_path().wstring()) : fixed, ModeOf(settings));
        break;
    }
    case Command::To: {
        const auto fixed = settings.FixedFolder();
        Destination chosen;
        if (plugin.staging.choose_destination(plugin.staging.context, View(fixed.empty() ? parent : fixed), Destination::Chosen, &chosen) != PULSE_OK ||
            chosen.status != PULSE_OK || chosen.path.empty()) return;
        for (const auto& path : paths) jobs.Extract(path, chosen.path, ModeOf(settings));
        break;
    }
    case Command::Test:
        for (const auto& path : paths) jobs.Test(path);
        break;
    case Command::Quick: {
        CompressChoice choice;
        choice.options.format = kFormats[settings.Format()];
        choice.options.level = kLevels[settings.Level()];
        choice.name = BaseName(paths) + engine::FormatExtension(choice.options.format);
        jobs.Compress(paths, parent, std::move(choice));
        break;
    }
    case Command::Add: {
        Plugin* self = &plugin;
        Dialogs::CompressSource source;
        source.base = BaseName(paths);
        const auto first = Utf8(fs::path(paths.front()).filename().wstring());
        source.title = paths.size() == 1 ? first : text::Format(T::SourceMore, first, std::to_string(paths.size() - 1));
        const auto folder = fs::path(paths.front()).parent_path();
        source.detail = text::Format(T::SourceSaveTo, Utf8(folder.filename().empty() ? folder.wstring() : folder.filename().wstring()));
        plugin.dialogs->ChooseCompression(source, settings.Format(), settings.Level(), {},
            [self, paths, parent](bool ok, CompressChoice choice) { if (ok) self->jobs->Compress(paths, parent, std::move(choice)); });
        break;
    }
    case Command::Hash:
        jobs.Hash(paths);
        break;
    default:
        break;
    }
}

std::string Title(const Plugin& plugin, Command command) {
    const auto& settings = *plugin.settings;
    switch (command) {
    case Command::Extract:
        if (!settings.FixedFolder().empty()) return text::Get(T::CmdFixed);
        return text::Get(settings.Mode() == Settings::ExtractMode::Folder ? T::CmdFolder
                         : settings.Mode() == Settings::ExtractMode::Here ? T::CmdHere : T::CmdSmart);
    case Command::To: return text::Get(T::CmdTo);
    case Command::Test: return text::Get(T::CmdTest);
    case Command::Quick: return text::Format(T::CmdQuick, kFormatNames[settings.Format()]);
    case Command::Add: return text::Get(T::CmdAdd);
    default: return text::Get(T::CmdHash);
    }
}

PulseStatus Register(Plugin& plugin, Command command) {
    static const auto archive_list = engine::ArchiveExtensions();
    static const auto archive_views = [] {
        std::vector<PulseStringView> views;
        for (const auto& ext : archive_list) views.push_back(View(ext));
        return views;
    }();
    struct Spec { const char* id; uint32_t icon, kinds, max; bool archives; };
    static constexpr Spec kSpecs[] = {
        {"extract", PULSE_COMMAND_ICON_EXTRACT, PULSE_COMMAND_FILES, 64, true},
        {"extract-to", PULSE_COMMAND_ICON_EXTRACT, PULSE_COMMAND_FILES, 64, true},
        {"test-archive", PULSE_COMMAND_ICON_DEFAULT, PULSE_COMMAND_FILES, 64, true},
        {"quick-compress", PULSE_COMMAND_ICON_ARCHIVE, PULSE_COMMAND_FILES | PULSE_COMMAND_DIRECTORIES, 4096, false},
        {"add-to-archive", PULSE_COMMAND_ICON_ARCHIVE, PULSE_COMMAND_FILES | PULSE_COMMAND_DIRECTORIES, 4096, false},
        {"checksums", PULSE_COMMAND_ICON_DEFAULT, PULSE_COMMAND_FILES, 64, false},
    };
    const auto index = static_cast<size_t>(command);
    const auto& spec = kSpecs[index];
    const auto title = Title(plugin, command);
    PulseCommandDescriptor descriptor{};
    descriptor.size = sizeof(descriptor);
    descriptor.id = View(spec.id);
    descriptor.title = View(title);
    descriptor.icon = spec.icon;
    descriptor.kinds = spec.kinds;
    descriptor.min_selection = 1;
    descriptor.max_selection = spec.max;
    if (spec.archives) {
        descriptor.extensions = archive_views.data();
        descriptor.extension_count = static_cast<uint32_t>(archive_views.size());
    }
    Plugin* self = &plugin;
    const auto status = plugin.registrations[index].Register(plugin.commands, descriptor,
        [self, command](PulseCommandId, const PulseContextSelectionSnapshot& selection) {
            try { Run(*self, command, selection); } catch (...) {}
        });
    if (status == PULSE_OK) {
        plugin.ids[index] = plugin.registrations[index].Id();
        plugin.titles[index] = title;
    }
    return status;
}

PulseStatus RegisterCommands(Plugin& plugin) {
    for (size_t i = 0; i < static_cast<size_t>(Command::Count); ++i) {
        const auto status = Register(plugin, static_cast<Command>(i));
        if (status != PULSE_OK) return status;
    }
    return PULSE_OK;
}

// Settings changed: menu titles that name the behavior follow it.
void Refresh(Plugin& plugin) {
    plugin.jobs->RememberPasswords(plugin.settings->RememberPasswords());
    bool changed = false;
    for (const auto command : {Command::Extract, Command::Quick}) {
        const auto index = static_cast<size_t>(command);
        changed = changed || (plugin.ids[index] && plugin.titles[index] != Title(plugin, command));
    }
    // The host lists commands in registration order, so re-register all of them to keep the menu order.
    if (changed) RegisterCommands(plugin);
}

PulseStatus PULSE_CALL Preview(void*, const PulsePreviewRequest* request) {
    if (!request || request->size < sizeof(*request) || !request->publish || !request->cancelled || !request->path.data ||
        !request->path.length || request->path.length > 131072) return PULSE_ERROR_ARGUMENT;
    try {
        const auto cancelled = [&] { return request->cancelled(request->context) != 0; };
        const auto listing = engine::List(Wide(request->path), cancelled, {}, 4096);
        if (cancelled() || listing.status == engine::Status::Cancelled) return PULSE_ERROR_STATE;
        if (listing.status != engine::Status::Ok) return PULSE_ERROR_UNAVAILABLE;
        std::vector<std::string> names;
        names.reserve(listing.entries.size());
        std::vector<PulsePreviewArchiveEntry> entries;
        entries.reserve(listing.entries.size());
        for (const auto& entry : listing.entries) {
            auto name = Utf8(entry.path);
            std::replace(name.begin(), name.end(), '\\', '/');
            names.push_back(std::move(name));
        }
        for (size_t i = 0; i < listing.entries.size(); ++i) {
            const auto& entry = listing.entries[i];
            entries.push_back({View(names[i]), entry.size, entry.packed, entry.modified,
                               (entry.dir ? PULSE_PREVIEW_ENTRY_DIRECTORY : 0u) | (entry.encrypted ? PULSE_PREVIEW_ENTRY_ENCRYPTED : 0u)});
        }
        const auto format = Utf8(listing.format);
        PulsePreviewResult result{};
        result.size = sizeof(result);
        result.kind = PULSE_PREVIEW_ARCHIVE;
        result.format = View(format);
        result.incomplete = listing.truncated ? 1u : 0u;
        result.entries = entries.data();
        result.entry_count = static_cast<uint32_t>(entries.size());
        return request->publish(request->context, &result);
    } catch (...) { return PULSE_ERROR_INITIALIZATION; }
}

PulseStatus RegisterPreviews(Plugin& plugin) {
    if (plugin.host.Query(PULSE_PREVIEW_INTERFACE_NAME, PULSE_PREVIEW_VERSION_1, plugin.preview) != PULSE_OK) return PULSE_OK;  // optional
    for (const auto* ext : kPreviewExtensions) {
        PulsePreviewProvider provider{};
        provider.size = sizeof(provider);
        provider.callback = Preview;
        provider.user = &plugin;
        provider.extension = View(ext);
        PulsePreviewProviderId id = 0;
        if (plugin.preview.register_provider(plugin.preview.context, &provider, &id) == PULSE_OK) plugin.previews.push_back(id);
    }
    return PULSE_OK;
}

void PULSE_CALL Destroy(void* value) {
    auto* plugin = static_cast<Plugin*>(value);
    if (plugin->jobs) plugin->jobs->CancelAll();
    for (auto& registration : plugin->registrations) registration.Reset();
    if (plugin->preview.context) for (const auto id : plugin->previews) plugin->preview.unregister_provider(plugin->preview.context, id);
    if (plugin->dispatch) plugin->dispatch->Close();
    plugin->jobs.reset();
    plugin->settings.reset();
    delete plugin;
}
} // namespace

PULSE_PLUGIN_SDK_PREVIEW()
extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host, PulsePluginV1* out) {
    if (!host || !out || host->size < sizeof(*host) || out->size < sizeof(*out) ||
        host->version != PULSE_ABI_VERSION_1 || out->version != PULSE_ABI_VERSION_1) return PULSE_ERROR_ABI;
    auto* plugin = new (std::nothrow) Plugin(*host);
    if (!plugin) return PULSE_ERROR_INITIALIZATION;
    *out = {sizeof(*out), PULSE_ABI_VERSION_1, plugin, Activate, Deactivate, Destroy};
    PulseTasksV1 tasks{};
    if (plugin->host.Query(PULSE_TASKS_INTERFACE_NAME, PULSE_TASKS_VERSION_1, tasks) != PULSE_OK ||
        plugin->host.Query(PULSE_STAGING_INTERFACE_NAME, PULSE_STAGING_VERSION_1, plugin->staging) != PULSE_OK ||
        plugin->host.Query(PULSE_COMMANDS_INTERFACE_NAME, PULSE_COMMANDS_VERSION_1, plugin->commands) != PULSE_OK ||
        plugin->host.Query(PULSE_DIALOGS_INTERFACE_NAME, PULSE_DIALOGS_VERSION_1, plugin->dialogs_api) != PULSE_OK ||
        plugin->host.Query(PULSE_DISPATCH_INTERFACE, PULSE_DISPATCH_VERSION, plugin->dispatch_api) != PULSE_OK)
        return PULSE_ERROR_UNAVAILABLE;
    try {
        plugin->dispatch = std::make_unique<pulse::sdk::DispatchQueue>(plugin->dispatch_api);
        plugin->dialogs = std::make_unique<Dialogs>(plugin->dialogs_api);
        plugin->jobs = std::make_unique<Jobs>(tasks, plugin->staging, *plugin->dispatch, *plugin->dialogs);
        plugin->settings = std::make_unique<Settings>();
    } catch (...) { return PULSE_ERROR_INITIALIZATION; }
    // Commands first (default titles), then settings; their load renames them.
    auto status = RegisterCommands(*plugin);
    if (status != PULSE_OK) return status;
    try {
        status = plugin->settings->Initialize(plugin->host, plugin->staging, [plugin] { try { Refresh(*plugin); } catch (...) {} });
        if (status != PULSE_OK) return status;
    } catch (...) { return PULSE_ERROR_INITIALIZATION; }
    return status == PULSE_OK ? RegisterPreviews(*plugin) : status;
}

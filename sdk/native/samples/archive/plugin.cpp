#include <windows.h>
#include <pulse/client.hpp>
#include <array>
#include <pulse/ui_model.h>
#include <pulse/commands.h>
#include <pulse/preview.h>
#include "archive_reader.h"
#include "archive_tasks.h"
#include "archive_settings.h"
#include <new>
#include <string>
#include <filesystem>
#include <stdexcept>
extern "C" __declspec(dllimport) HRESULT WINAPI GetNumberOfFormats(unsigned int* count);
namespace {
using pulse::sdk::View;
struct Plugin {
    pulse::sdk::Host host;
    PulseUiModelV1 model{};
    PulseCommandsV1 commands{};
    std::array<pulse::sdk::CommandRegistration, 4> command_handles;
    PulsePreviewV1 preview{};
    PulseStagingV1 staging{};
    PulsePreviewProviderId preview_7z = 0, preview_rar = 0;
    PulseCommandId extract_here = 0, extract_to = 0, compress = 0, verify = 0;
    std::unique_ptr<ArchiveTasks> tasks;
    std::unique_ptr<ArchiveSettings> settings;
    uint64_t panel = 0;
    explicit Plugin(const PulseHostV1& value) : host(value) {}
};
PulseStatus PULSE_CALL Activate(void* value) {
    auto& plugin = *static_cast<Plugin*>(value);
    if (plugin.panel) return PULSE_OK;
    unsigned int formats = 0;
    if (FAILED(GetNumberOfFormats(&formats)) || formats == 0) return PULSE_ERROR_INITIALIZATION;
    try {
        const std::string status = "7-Zip engine loaded. Supported formats: " + std::to_string(formats);
        if (plugin.host.Query(PULSE_UI_MODEL_INTERFACE, PULSE_UI_MODEL_VERSION, plugin.model) != PULSE_OK) return PULSE_ERROR_UNAVAILABLE;
        using pulse::sdk::ModelNode;
        auto tree = pulse::sdk::ModelTree(ModelNode(PULSE_UI_MODEL_STACK, "root")
            .Int(&PulseUiModelNode::gap, 12).Children({
                ModelNode(PULSE_UI_MODEL_TEXT, "engine-status").String(&PulseUiModelNode::binding, "state.status")}))
            .State("status", status);
        return plugin.model.create(plugin.model.context, View("Archive engine"), &tree.Build(), &plugin.panel);
    } catch (...) { return PULSE_ERROR_INITIALIZATION; }
}
void PULSE_CALL Deactivate(void* value) {
    auto& plugin = *static_cast<Plugin*>(value);
    if (plugin.panel) { plugin.model.close(plugin.model.context, plugin.panel); plugin.panel = 0; }
}
std::wstring Wide(PulseStringView value) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data,
        static_cast<int>(value.length), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8 path");
    std::wstring path(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data, static_cast<int>(value.length), path.data(), count);
    return path;
}
std::string Utf8(const std::wstring& value) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid destination path");
    std::string text(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        text.data(), count, nullptr, nullptr);
    return text;
}
struct Destination {
    PulseStatus status = PULSE_ERROR_STATE;
    std::string path;
    static void PULSE_CALL Chosen(void* value, PulseStatus status, PulseStringView path) {
        auto& result = *static_cast<Destination*>(value);
        result.status = status;
        try { if (status == PULSE_OK && path.data && path.length) result.path.assign(path.data, path.length); }
        catch (...) { result.status = PULSE_ERROR_INITIALIZATION; }
    }
};
void PULSE_CALL Invoke(void* value, PulseCommandId command, const PulseContextSelectionSnapshot* selection) {
    auto& plugin = *static_cast<Plugin*>(value);
    if (Activate(value) != PULSE_OK) return;
    try {
        std::string text;
        if (command == plugin.compress) {
            if (!selection || !selection->entry_count || !selection->entries) return;
            if (!plugin.settings || !plugin.settings->Ready()) {
                text = "Compression preferences are still loading or saving. Retry after settings are ready.";
            } else {
                std::vector<std::wstring> paths;
                paths.reserve(selection->entry_count);
                for (uint32_t i = 0; i < selection->entry_count; ++i) paths.push_back(Wide(selection->entries[i].path));
                const bool zip = plugin.settings->Zip();
                const auto level = plugin.settings->Level();
                const auto name = (paths.size() == 1 ? std::filesystem::path(paths.front()).filename().wstring() : L"Archive") +
                    std::wstring(zip ? L".zip" : L".7z");
                const auto initial = Utf8(std::filesystem::path(paths.front()).parent_path().wstring());
                Destination chosen;
                const auto selected = plugin.staging.choose_destination(plugin.staging.context, View(initial), Destination::Chosen, &chosen);
                if (selected != PULSE_OK || chosen.status != PULSE_OK || chosen.path.empty()) {
                    text = "Compress: no destination selected; no compression task started.";
                } else {
                    const auto status = plugin.tasks->Compress(paths, chosen.path, name,
                        zip ? archive_writer::Format::Zip : archive_writer::Format::SevenZip, level);
                    text = status == PULSE_OK
                        ? "Archive compression started. The host will confirm publication and handle file conflicts."
                        : "Archive compression could not start.";
                }
            }
        } else {
            if (!selection || selection->entry_count != 1 || !selection->entries) return;
            const auto path = Wide(selection->entries[0].path);
            PulseStatus status = PULSE_ERROR_ARGUMENT;
            if (command == plugin.verify) {
                status = plugin.tasks->Verify(path);
                text = status == PULSE_OK
                    ? "Archive verification started. Private extraction is cleaned after completion or cancellation."
                    : "Archive verification could not start.";
            } else if (command == plugin.extract_here || command == plugin.extract_to) {
                // Path syntax only: the worker and host own all filesystem I/O.
                auto destination = Utf8(std::filesystem::path(path).parent_path().wstring());
                if (command == plugin.extract_to) {
                    if (plugin.settings && !plugin.settings->Destination().empty()) destination = plugin.settings->Destination();
                    Destination chosen;
                    const auto selected = plugin.staging.choose_destination(plugin.staging.context, View(destination), Destination::Chosen, &chosen);
                    if (selected != PULSE_OK || chosen.status != PULSE_OK || chosen.path.empty()) {
                        text = "Extract to: no destination selected; no extraction task started.";
                    } else destination = std::move(chosen.path);
                }
                if (text.empty()) {
                    status = plugin.tasks->Extract(path, destination);
                    text = status == PULSE_OK
                        ? "Archive extraction started. The host will confirm publication and handle file conflicts."
                        : "Archive extraction could not start.";
                }
            } else return;
        }
        const PulseUiState state{View("status"), View(text)};
        plugin.model.set_states(plugin.model.context, plugin.panel, &state, 1);
    } catch (...) {}
}
PulseStatus PULSE_CALL Preview(void*, const PulsePreviewRequest* request) {
    if (!request || request->size < sizeof(*request) || !request->publish || !request->cancelled ||
        !request->path.data || !request->path.length || request->path.length > 131072) return PULSE_ERROR_ARGUMENT;
    try {
        const auto cancelled = [&] { return request->cancelled(request->context) != 0; };
        if (cancelled()) return PULSE_ERROR_STATE;
        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, request->path.data,
            static_cast<int>(request->path.length), nullptr, 0);
        if (!size) return PULSE_ERROR_ARGUMENT;
        std::wstring path(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, request->path.data,
            static_cast<int>(request->path.length), path.data(), size);
        const auto listing = archive_reader::Read(path, cancelled);
        if (cancelled() || listing.status == archive_reader::Status::Cancelled) return PULSE_ERROR_STATE;
        if (listing.status != archive_reader::Status::Ok && listing.status != archive_reader::Status::LimitReached)
            return PULSE_ERROR_UNAVAILABLE;
        if (listing.status == archive_reader::Status::LimitReached && listing.entries.empty()) return PULSE_ERROR_UNAVAILABLE;
        std::vector<PulsePreviewArchiveEntry> entries;
        entries.reserve(listing.entries.size());
        for (const auto& entry : listing.entries) {
            entries.push_back({View(entry.path), entry.size, entry.packed, entry.modified,
                (entry.dir ? PULSE_PREVIEW_ENTRY_DIRECTORY : 0u) | (entry.encrypted ? PULSE_PREVIEW_ENTRY_ENCRYPTED : 0u)});
        }
        PulsePreviewResult result{};
        result.size = sizeof(result); result.kind = PULSE_PREVIEW_ARCHIVE;
        result.format = View(listing.format); result.incomplete = listing.incomplete ? 1u : 0u;
        result.entries = entries.data(); result.entry_count = static_cast<uint32_t>(entries.size());
        return request->publish(request->context, &result);
    } catch (...) { return PULSE_ERROR_INITIALIZATION; }
}
PulseStatus RegisterPreviews(Plugin& plugin) {
    if (plugin.host.Query(PULSE_PREVIEW_INTERFACE_NAME, PULSE_PREVIEW_VERSION_1, plugin.preview) != PULSE_OK)
        return PULSE_ERROR_UNAVAILABLE;
    PulsePreviewProvider provider{};
    provider.size = sizeof(provider); provider.callback = Preview; provider.user = &plugin;
    provider.extension = View(".7z");
    auto status = plugin.preview.register_provider(plugin.preview.context, &provider, &plugin.preview_7z);
    if (status != PULSE_OK) return status;
    provider.extension = View(".rar");
    return plugin.preview.register_provider(plugin.preview.context, &provider, &plugin.preview_rar);
}
PulseStatus Register(Plugin& plugin) {
    if (plugin.host.Query(PULSE_COMMANDS_INTERFACE_NAME, PULSE_COMMANDS_VERSION_1, plugin.commands) != PULSE_OK) return PULSE_ERROR_UNAVAILABLE;
    const auto bind = [&](size_t index, const PulseCommandDescriptor& descriptor, PulseCommandId& id) {
        const auto result = plugin.command_handles[index].Register(plugin.commands, descriptor,
            [&plugin](PulseCommandId command, const PulseContextSelectionSnapshot& selection) { Invoke(&plugin, command, &selection); });
        id = plugin.command_handles[index].Id();
        return result;
    };
    const PulseStringView extensions[]{View(".7z"), View(".rar")};
    PulseCommandDescriptor command{}; command.size = sizeof(command); command.kinds = PULSE_COMMAND_FILES;
    command.min_selection = 1; command.max_selection = 1;
    command.extensions = extensions; command.extension_count = 2;
    command.icon = PULSE_COMMAND_ICON_EXTRACT; command.invoke = Invoke; command.user = &plugin;
    command.id = View("extract-here"); command.title = View("解压到此处");
    auto status = bind(0, command, plugin.extract_here);
    if (status != PULSE_OK) return status;
    command.id = View("extract-to"); command.title = View("解压到…"); command.virtual_key = VK_F24;
    status = bind(1, command, plugin.extract_to);
    if (status != PULSE_OK) return status;
    command.id = View("compress"); command.title = View("添加到压缩包…"); command.icon = PULSE_COMMAND_ICON_ARCHIVE;
    const PulseStringView compressed_extensions[]{View(".zip"), View(".7z"), View(".rar")};
    command.extensions = compressed_extensions; command.extension_count = 3;
    command.max_selection = 4096; command.exclude_extensions = 1; command.virtual_key = 0;
    command.kinds = PULSE_COMMAND_FILES | PULSE_COMMAND_DIRECTORIES;
    status = bind(2, command, plugin.compress);
    if (status != PULSE_OK) return status;
    command.id = View("verify-archive"); command.title = View("验证压缩包");
    command.exclude_extensions = 0; command.max_selection = 1;
    command.kinds = PULSE_COMMAND_FILES;
    const PulseStringView preview_extensions[]{View(".7z"), View(".rar")};
    command.extensions = preview_extensions; command.extension_count = 2;
    return bind(3, command, plugin.verify);
}
void PULSE_CALL Destroy(void* value) {
    auto& plugin = *static_cast<Plugin*>(value);
    for (auto& command : plugin.command_handles) command.Reset();
    if (plugin.preview.context) for (const auto id : {plugin.preview_7z, plugin.preview_rar})
        if (id) plugin.preview.unregister_provider(plugin.preview.context, id);
    delete &plugin;
}
}
PULSE_PLUGIN_SDK_PREVIEW()
extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host, PulsePluginV1* out) {
    if (!host || !out || host->size < sizeof(*host) || out->size < sizeof(*out) ||
        host->version != PULSE_ABI_VERSION_1 || out->version != PULSE_ABI_VERSION_1) return PULSE_ERROR_ABI;
    auto* instance = new (std::nothrow) Plugin(*host);
    if (!instance) return PULSE_ERROR_INITIALIZATION;
    *out = {sizeof(*out), PULSE_ABI_VERSION_1, instance, Activate, Deactivate, Destroy};
    PulseTasksV1 tasks{};
    if (instance->host.Query(PULSE_TASKS_INTERFACE_NAME, PULSE_TASKS_VERSION_1, tasks) != PULSE_OK) return PULSE_ERROR_UNAVAILABLE;
    if (instance->host.Query(PULSE_STAGING_INTERFACE_NAME, PULSE_STAGING_VERSION_1, instance->staging) != PULSE_OK)
        return PULSE_ERROR_UNAVAILABLE;
    try {
        instance->tasks = std::make_unique<ArchiveTasks>(tasks, instance->staging);
        instance->settings = std::make_unique<ArchiveSettings>();
        const auto initialized = instance->settings->Initialize(instance->host, instance->staging);
        if (initialized != PULSE_OK) return initialized;
    } catch (...) { return PULSE_ERROR_INITIALIZATION; }
    const auto status = Register(*instance);
    return status == PULSE_OK ? RegisterPreviews(*instance) : status;
}

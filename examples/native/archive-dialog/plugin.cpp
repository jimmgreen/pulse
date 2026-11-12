// A context-menu command that opens a host-drawn dialog in the style of the
// 7-Zip extension's "Add to archive": heading, subject card, a reactive form,
// a folder picker bound to a form button, footnote and buttons. UI only: it
// never reads or writes the selected files (no file work on the UI thread).
#include <pulse/client.hpp>
#include <map>
#include <memory>
#include <string>
namespace {
using pulse::sdk::ModelNode;
using pulse::sdk::View;
using N = PulseUiModelNode;
using States = std::map<std::string, std::string>;

constexpr const char* kFormats[] = {"7z", "zip", "tar"};

struct Plugin {
    pulse::sdk::Host host;
    PulseCommandsV1 commands{};
    PulseDispatchV1 dispatch_api{};
    std::unique_ptr<pulse::sdk::DispatchQueue> queue;
    std::optional<pulse::sdk::PickerClient> pickers;
    pulse::sdk::CommandRegistration command;
    pulse::sdk::DialogSession dialog;
    States values;  // the form's latest values, for derived states
    bool active = false;
    explicit Plugin(const PulseHostV1& h) : host(h) {}

    // States computed from the inputs: the badge after the name, which
    // password fields show, and the mismatch warning.
    pulse::sdk::DialogSession::StateList Derived() const {
        const auto get = [&](const char* key) { const auto it = values.find(key); return it == values.end() ? std::string() : it->second; };
        size_t format = 0;
        for (size_t i = 0; i < std::size(kFormats); ++i) if (get("format") == kFormats[i]) format = i;
        const bool encryptable = format != 2;  // tar has no encryption
        const bool protecting = encryptable && get("protect") == "true";
        const bool mismatch = protecting && !get("confirm").empty() && get("password") != get("confirm");
        return {{"ext", std::string(".") + kFormats[format]}, {"encryptable", encryptable ? "true" : "false"},
                {"protecting", protecting ? "true" : "false"}, {"mismatch", mismatch ? "true" : "false"}};
    }

    void Open(const PulseContextSelectionSnapshot& selection) {
        if (!active || dialog.Id() || selection.entry_count == 0) return;
        const auto& first = selection.entries[0];
        const std::string first_name(first.name.data ? first.name.data : "", first.name.length);
        const std::string directory(selection.directory.data ? selection.directory.data : "", selection.directory.length);
        const bool folder = selection.entry_count == 1 && (first.flags & PULSE_CONTEXT_ENTRY_DIRECTORY) != 0;
        std::string stem = first_name;
        if (!folder) if (const auto dot = stem.rfind('.'); dot != std::string::npos && dot > 0) stem.erase(dot);
        const std::string subject = selection.entry_count == 1 ? first_name : std::to_string(selection.entry_count) + " items";
        const std::string archive_name = selection.entry_count == 1 ? stem : "Archive";

        auto secret = [](const char* id, const char* state, const char* label) {
            return ModelNode(PULSE_UI_MODEL_TEXT_INPUT, id).String(&N::text, label).String(&N::placeholder, label)
                .String(&N::state, state).String(&N::kind, "password");
        };
        pulse::sdk::ModelTree tree(ModelNode(PULSE_UI_MODEL_STACK, "form").Int(&N::gap, 14).Children({
            ModelNode(PULSE_UI_MODEL_STACK, "name-block").Int(&N::gap, 6).Children({
                ModelNode(PULSE_UI_MODEL_TEXT, "name-label").String(&N::text, "Archive name"),
                ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "name-input").String(&N::text, "Archive name").String(&N::state, "name")
                    .String(&N::badge_text, "state.ext")}),
            ModelNode(PULSE_UI_MODEL_STACK, "format-block").Int(&N::gap, 6).Children({
                ModelNode(PULSE_UI_MODEL_TEXT, "format-label").String(&N::text, "Format"),
                ModelNode(PULSE_UI_MODEL_SEGMENTED, "format-input").String(&N::state, "format").Options({kFormats, kFormats + 3})}),
            ModelNode(PULSE_UI_MODEL_STACK, "level-block").Int(&N::gap, 6).Children({
                ModelNode(PULSE_UI_MODEL_TEXT, "level-label").String(&N::text, "Compression level"),
                ModelNode(PULSE_UI_MODEL_SLIDER, "level-input").String(&N::state, "level").Int(&N::stops, 4)
                    .Options({"Store", "Fast", "Normal", "Maximum"})}),
            ModelNode(PULSE_UI_MODEL_STACK, "protect-block").Int(&N::gap, 8).String(&N::visible_binding, "state.encryptable").Children({
                ModelNode(PULSE_UI_MODEL_SWITCH, "protect-input").String(&N::state, "protect").String(&N::text, "Protect with a password"),
                ModelNode(PULSE_UI_MODEL_GRID, "password-grid").Int(&N::columns, 2).Int(&N::gap, 10)
                    .String(&N::visible_binding, "state.protecting")
                    .Children({secret("password-input", "password", "Password"), secret("confirm-input", "confirm", "Confirm password")}),
                ModelNode(PULSE_UI_MODEL_INFO_BAR, "mismatch").String(&N::kind, "error")
                    .String(&N::message, "The passwords do not match.").String(&N::visible_binding, "state.mismatch")}),
            ModelNode(PULSE_UI_MODEL_STACK, "output-block").Int(&N::gap, 6).Children({
                ModelNode(PULSE_UI_MODEL_TEXT, "output-label").String(&N::text, "Save in"),
                ModelNode(PULSE_UI_MODEL_STACK, "output-row").String(&N::orientation, "horizontal").Int(&N::gap, 8).Children({
                    ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "output-input").String(&N::text, "Save in").String(&N::state, "output").Int(&N::flex, 1),
                    ModelNode(PULSE_UI_MODEL_BUTTON, "browse").String(&N::text, "Browse...").String(&N::command, "browse")})})}));
        values = {{"name", archive_name}, {"format", "7z"}, {"level", "Normal"}, {"protect", "false"},
                  {"password", ""}, {"confirm", ""}, {"output", directory}};
        for (const auto& [key, value] : values) tree.State(key, value);
        for (const auto& [key, value] : Derived()) tree.State(key, value);

        const PulseDialogButton buttons[] = {
            {sizeof(PulseDialogButton), PULSE_DIALOG_BUTTON_PRIMARY, View("start"), View("Create")},
            {sizeof(PulseDialogButton), PULSE_DIALOG_BUTTON_STANDARD, View("cancel"), View("Cancel")}};
        PulseDialogRequest request{};
        request.size = sizeof(request);
        request.width = 560;
        request.height = 560;  // a ceiling: the host fits the dialog to the visible fields
        request.title = View("Archive dialog example");
        request.heading = View("Add to archive");
        request.icon = PULSE_DIALOG_ICON_ARCHIVE;
        request.subject_kind = folder ? PULSE_DIALOG_SUBJECT_FOLDER : PULSE_DIALOG_SUBJECT_FILE;
        request.subject = View(subject);
        request.subject_detail = View(directory);
        request.footnote = View("Example only: no archive is written.");
        request.footnote_icon = PULSE_DIALOG_ICON_INFO;
        request.buttons = buttons;
        request.button_count = 2;
        request.default_button = 0;
        request.cancel_button = 1;
        request.model = &tree.Build();
        const auto status = dialog.Show(request,
            [this](uint64_t, const pulse::sdk::DialogSession::StateList& changes) {
                for (const auto& [key, value] : changes) values[key] = value;
                (void)dialog.SetStates(Derived());
            },
            [this](uint64_t, PulseStatus status, std::string_view button, const pulse::sdk::DialogSession::StateList& states) {
                if (status != PULSE_OK || button != "start" || !active) return;
                States chosen;
                for (const auto& [key, value] : states) chosen[key] = value;
                // A second dialog may only open once this one has fully closed.
                queue->Post([this, chosen] { if (active) Summary(chosen); });
            });
        // V2 hosts deliver the form button's command; V1 hosts leave the field editable.
        if (status == PULSE_OK && dialog.SupportsV2() && pickers)
            (void)dialog.BindFolderPicker(*pickers, "browse", "output", "Save the archive in");
    }

    void Summary(const States& chosen) {
        const auto get = [&](const char* key) { const auto it = chosen.find(key); return it == chosen.end() ? std::string() : it->second; };
        values = chosen;
        std::string ext = ".7z";
        for (const auto& [key, value] : Derived()) if (key == "ext") ext = value;
        const std::string message = "Would create " + get("name") + ext + " in " + get("output") + " (level " + get("level") +
            (get("protect") == "true" ? ", password protected" : "") + ").";
        const PulseDialogButton ok{sizeof(PulseDialogButton), PULSE_DIALOG_BUTTON_PRIMARY, View("ok"), View("OK")};
        PulseDialogRequest request{};
        request.size = sizeof(request);
        request.title = View("Archive dialog example");
        request.heading = View("Archive settings chosen");
        request.icon = PULSE_DIALOG_ICON_INFO;
        request.message = View(message);
        request.buttons = &ok;
        request.button_count = 1;
        request.default_button = 0;
        request.cancel_button = 0;
        (void)dialog.Show(request, nullptr, [](uint64_t, PulseStatus, std::string_view, const pulse::sdk::DialogSession::StateList&) {});
    }

    PulseStatus Start() {
        PulseStatus code = host.Query(PULSE_COMMANDS_INTERFACE_NAME, PULSE_COMMANDS_VERSION_1, commands); if (code != PULSE_OK) return code;
        code = host.Query(PULSE_DISPATCH_INTERFACE, PULSE_DISPATCH_VERSION, dispatch_api); if (code != PULSE_OK) return code;
        code = dialog.Init(host); if (code != PULSE_OK) return code;
        queue = std::make_unique<pulse::sdk::DispatchQueue>(dispatch_api);
        PulsePickersV1 picker_table{};  // optional: Browse needs pulse.pickers
        if (host.Query(PULSE_PICKERS_INTERFACE_NAME, PULSE_PICKERS_VERSION_1, picker_table) == PULSE_OK) pickers.emplace(picker_table);
        active = true;
        PulseCommandDescriptor descriptor{};
        descriptor.size = sizeof(descriptor);
        descriptor.id = View("add-to-archive");
        descriptor.title = View("Add to archive (example)...");
        descriptor.detail = View("{base}.7z");
        descriptor.icon = PULSE_COMMAND_ICON_ARCHIVE;
        descriptor.kinds = PULSE_COMMAND_FILES | PULSE_COMMAND_DIRECTORIES;
        descriptor.min_selection = 1;
        descriptor.max_selection = 64;
        return command.Register(commands, descriptor, [this](PulseCommandId, const PulseContextSelectionSnapshot& selection) {
            try { Open(selection); } catch (...) {}
        });
    }
    void Stop() noexcept {
        active = false;
        command.Reset();
        dialog.Close();
        if (queue) queue->Close();
    }
    ~Plugin() { Stop(); queue.reset(); }
};
PulseStatus PULSE_CALL Activate(void* p) noexcept { try { return static_cast<Plugin*>(p)->Start(); } catch (...) { return PULSE_ERROR_INITIALIZATION; } }
void PULSE_CALL Deactivate(void* p) noexcept { static_cast<Plugin*>(p)->Stop(); }
void PULSE_CALL Destroy(void* p) noexcept { delete static_cast<Plugin*>(p); }
}
PULSE_PLUGIN_SDK_PREVIEW()
extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host, PulsePluginV1* out) noexcept {
    if (!host || !out || host->size < sizeof(*host) || out->size < sizeof(*out) || host->version != 1 || out->version != 1) return PULSE_ERROR_ABI;
    try { *out = {sizeof(*out), 1, new Plugin(*host), Activate, Deactivate, Destroy}; return PULSE_OK; } catch (...) { return PULSE_ERROR_INITIALIZATION; }
}

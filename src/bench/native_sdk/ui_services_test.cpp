#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../../app/extensions/native/diagnostics_adapter.h"
#include "../../app/extensions/native/dialogs_adapter.h"
#include "../../app/extensions/native/pickers_adapter.h"
#include "../../app/extensions/native/ui_model.h"
#include "../../app/extensions/native/workbench_adapter.h"

#include <pulse/client.hpp>

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace pulse::extensions;
using namespace pulse::extensions::native;
using pulse::sdk::Diagnostic;
using pulse::sdk::DialogSession;
using pulse::sdk::DiagnosticsClient;
using pulse::sdk::ModelNode;
using pulse::sdk::ModelTree;
using pulse::sdk::PickerClient;
using pulse::sdk::PickerResultData;
using pulse::sdk::View;
using pulse::sdk::WorkbenchClient;

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
    }
}

struct MockDialogRuntime {
    bool valid_owner = true;
    bool open = false;
    HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0xD1A106u));
    std::map<std::string, std::string> states;
    std::function<void(const std::vector<UiStateEntry>&)> on_states_changed;
    std::function<void(const std::string&)> on_command;
    std::function<bool(const std::string&)> command_enabled;
    std::function<void(int, std::vector<UiStateEntry>)> on_closed;
    int refresh_calls = 0;
    int update_form_calls = 0;

    DialogsBackend Backend() {
        DialogsBackend b;
        b.valid_owner = [this]() { return valid_owner; };
        b.show = [this](DialogPresentationSpec spec) {
            if (!valid_owner || open) return false;
            open = true;
            states.clear();
            if (spec.form.has_value()) {
                for (const auto& entry : spec.form->state) {
                    states[entry.key] = entry.value;
                }
            }
            on_states_changed = std::move(spec.on_states_changed);
            on_command = std::move(spec.on_command);
            command_enabled = std::move(spec.command_enabled);
            on_closed = std::move(spec.on_closed);
            return true;
        };
        b.set_states = [this](const std::vector<UiStateEntry>& updates) {
            if (!open) return false;
            for (const auto& item : updates) {
                states[item.key] = item.value;
            }
            return true;
        };
        b.update_form = [this](const UiTree& form) {
            if (!open) return false;
            ++update_form_calls;
            std::map<std::string, std::string> merged;
            for (const auto& entry : form.state) {
                const auto it = states.find(entry.key);
                merged[entry.key] = (it != states.end()) ? it->second : entry.value;
            }
            states = std::move(merged);
            return true;
        };
        b.get_state = [this](const std::string& key) -> std::optional<std::string> {
            if (!open) return std::nullopt;
            const auto it = states.find(key);
            if (it == states.end()) return std::nullopt;
            return it->second;
        };
        b.refresh_form = [this]() { ++refresh_calls; };
        b.dismiss = [this]() { Finish(-1); };
        b.destroy = [this]() {
            open = false;
            on_states_changed = nullptr;
            on_command = nullptr;
            command_enabled = nullptr;
            on_closed = nullptr;
            states.clear();
        };
        b.is_open = [this]() { return open; };
        b.active_hwnd = [this]() -> HWND { return open ? hwnd : nullptr; };
        b.set_appearance = [](bool, D2D1_COLOR_F) {};
        return b;
    }

    void Finish(int button_index) {
        if (!open) return;
        open = false;
        std::vector<UiStateEntry> snapshot;
        snapshot.reserve(states.size());
        for (const auto& [k, v] : states) {
            snapshot.push_back(UiStateEntry{k, v});
        }
        auto cb = std::move(on_closed);
        on_states_changed = nullptr;
        on_command = nullptr;
        command_enabled = nullptr;
        on_closed = nullptr;
        if (cb) {
            cb(button_index, std::move(snapshot));
        }
    }
};

struct MockPickerRuntime {
    bool valid_owner = true;
    std::function<HWND()> active_modal_owner;
    HWND last_parent_hwnd = nullptr;
    HWND fallback_owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1001u));
    std::optional<PickerLaunchSpec> pending_spec;
    std::function<void(PickerBackendCompletion)> pending_done;
    uint64_t last_cancelled_id = 0;

    PickersBackend Backend() {
        PickersBackend b;
        b.valid_owner = [this]() { return valid_owner; };
        b.launch = [this](PickerLaunchSpec spec, std::function<void(PickerBackendCompletion)> done) {
            if (!valid_owner) return false;
            const HWND modal = active_modal_owner ? active_modal_owner() : nullptr;
            last_parent_hwnd = modal ? modal : fallback_owner;
            pending_spec = std::move(spec);
            pending_done = std::move(done);
            return true;
        };
        b.cancel = [this](uint64_t request_id) {
            last_cancelled_id = request_id;
        };
        return b;
    }

    void Complete(PickerBackendCompletion result) {
        auto cb = std::move(pending_done);
        pending_spec.reset();
        pending_done = nullptr;
        if (cb) {
            cb(std::move(result));
        }
    }
};

struct MockWorkbenchRuntime {
    bool valid_owner = true;
    std::vector<SnackPresentationSpec> snacks;
    std::vector<NoticePresentationSpec> notices;
    std::wstring last_reveal_path;
    uint32_t last_reveal_mode = 0;
    std::wstring last_diff_left;
    std::wstring last_diff_right;
    std::wstring last_clipboard;

    WorkbenchBackend Backend() {
        WorkbenchBackend b;
        b.valid_owner = [this]() { return valid_owner; };
        b.show_snack = [this](SnackPresentationSpec spec) {
            snacks.push_back(std::move(spec));
            return true;
        };
        b.show_notice = [this](NoticePresentationSpec spec) {
            notices.push_back(std::move(spec));
            return true;
        };
        b.reveal_path = [this](const std::wstring& path, uint32_t mode) {
            last_reveal_path = path;
            last_reveal_mode = mode;
            return PULSE_OK;
        };
        b.show_text_diff = [this](const std::wstring& left, const std::wstring& right) {
            last_diff_left = left;
            last_diff_right = right;
            return PULSE_OK;
        };
        b.set_clipboard_text = [this](const std::wstring& text) {
            last_clipboard = text;
            return PULSE_OK;
        };
        return b;
    }
};

void TestDiagnosticsAndModelErrors() {
    bool alive = true;
    auto sink = std::make_shared<DiagnosticsSink>();
    DiagnosticsAdapter diag_adapter(sink, [&]() { return alive; });

    struct GuardedDiagnostics {
        PulseDiagnosticsV1 table{};
        uint64_t canary = 0xA11CE5A11CE5ULL;
    } guarded;
    Check(diag_adapter.Query(PULSE_DIAGNOSTICS_VERSION_1, &guarded.table, sizeof(guarded)) == PULSE_OK &&
              guarded.canary == 0xA11CE5A11CE5ULL,
          "pulse.diagnostics V1 query preserves trailing canary");
    Check(diag_adapter.Query(2, &guarded.table, sizeof(guarded.table)) == PULSE_ERROR_ABI,
          "pulse.diagnostics rejects unknown version with PULSE_ERROR_ABI");

    DiagnosticsClient client(guarded.table);
    Diagnostic initial{};
    Check(client.LastError(initial) == PULSE_OK && initial.status == PULSE_OK && initial.sequence == 0,
          "initial pulse.diagnostics snapshot is empty (status=PULSE_OK, sequence=0)");

    // Build an invalid UI model C struct (invalid UTF-8 sequence in child text field).
    const char invalid_utf8[] = {'\xFF', '\xFE', 'x'};
    PulseUiModelNode child{};
    child.size = sizeof(child);
    child.type = PULSE_UI_MODEL_TEXT_INPUT;
    child.id = View("dest-input");
    child.text = PulseUiModelString{1u, PulseStringView{invalid_utf8, sizeof(invalid_utf8)}};

    PulseUiModelNode root{};
    root.size = sizeof(root);
    root.type = PULSE_UI_MODEL_STACK;
    root.id = View("root");
    root.children = PulseUiModelNodes{1u, &child, 1u};

    PulseUiModelTree raw_tree{};
    raw_tree.size = sizeof(raw_tree);
    raw_tree.root = &root;

    UiTree parsed_tree;
    std::set<std::string> commands;
    std::string model_err;
    const PulseStatus conv_status = ConvertUiModel(&raw_tree, parsed_tree, commands, model_err);
    Check(conv_status == PULSE_ERROR_ARGUMENT &&
              model_err.find("root(root).children[0](dest-input).text: ") == 0,
          "ConvertUiModel reports exact node path and field name on invalid C-struct input");

    sink->RecordParsed(conv_status, "pulse.ui.model", "create", model_err);
    Diagnostic err1{};
    Check(client.LastError(err1) == PULSE_OK &&
              err1.status == PULSE_ERROR_ARGUMENT &&
              err1.sequence == 1 &&
              err1.interface_name == "pulse.ui.model" &&
              err1.operation == "create" &&
              err1.subject == "root(root).children[0](dest-input)" &&
              err1.field == "text" &&
              !err1.message.empty(),
          "DiagnosticsClient splits subject path, field name, status, and sequence=1");

    // Successful calls leave the last error untouched.
    sink->Record(PULSE_OK, "pulse.ui.model", "get_state", "panel", "key", "ignored");
    Diagnostic still_err1{};
    Check(client.LastError(still_err1) == PULSE_OK && still_err1.sequence == 1,
          "PULSE_OK leaves prior diagnostic intact");

    Diagnostic cleared{};
    Check(client.Clear() == PULSE_OK && client.LastError(cleared) == PULSE_OK &&
              cleared.status == PULSE_OK && cleared.sequence == 0,
          "clear_last_error resets diagnostic snapshot");

    sink->Record(PULSE_ERROR_UNAVAILABLE, "pulse.dialogs", "show", "model", "commands", "V1 forbids commands");
    Diagnostic err2{};
    Check(client.LastError(err2) == PULSE_OK &&
              err2.sequence == 2 &&
              err2.status == PULSE_ERROR_UNAVAILABLE &&
              err2.interface_name == "pulse.dialogs" &&
              err2.field == "commands",
          "monotonic diagnostic sequence advances across clear_last_error");

    diag_adapter.Close();
}

void TestDialogsV1VsV2AndNestedPickerFlow() {
    bool alive = true;
    auto sink = std::make_shared<DiagnosticsSink>();
    DiagnosticsAdapter diag_adapter(sink, [&]() { return alive; });
    PulseDiagnosticsV1 diag_table{};
    diag_adapter.Query(PULSE_DIAGNOSTICS_VERSION_1, &diag_table, sizeof(diag_table));
    DiagnosticsClient diag_client(diag_table);

    MockDialogRuntime dlg_rt;
    DialogsAdapter dialogs(dlg_rt.Backend(), [&]() { return alive; }, sink);

    MockPickerRuntime picker_rt;
    picker_rt.active_modal_owner = [&]() { return dialogs.ActiveModalWindow(); };
    PickersAdapter pickers(picker_rt.Backend(), [&]() { return alive; }, sink);

    struct GuardedV1 {
        PulseDialogsV1 table{};
        uint64_t canary = 0x1122334455667788ULL;
    } v1_guard;
    struct GuardedV2 {
        PulseDialogsV2 table{};
        uint64_t canary = 0x8877665544332211ULL;
    } v2_guard;
    Check(dialogs.Query(PULSE_DIALOGS_VERSION_1, &v1_guard.table, sizeof(v1_guard)) == PULSE_OK &&
              v1_guard.canary == 0x1122334455667788ULL &&
              v1_guard.table.size == sizeof(PulseDialogsV1) &&
              v1_guard.table.version == PULSE_DIALOGS_VERSION_1,
          "PulseDialogsV1 query stamps V1 header and preserves trailing canary");
    Check(dialogs.Query(PULSE_DIALOGS_VERSION_2, &v2_guard.table, sizeof(v2_guard)) == PULSE_OK &&
              v2_guard.canary == 0x8877665544332211ULL &&
              v2_guard.table.size == sizeof(PulseDialogsV2) &&
              v2_guard.table.version == PULSE_DIALOGS_VERSION_2,
          "PulseDialogsV2 query stamps V2 header and preserves trailing canary");

    PulsePickersV1 pickers_table{};
    Check(pickers.Query(PULSE_PICKERS_VERSION_1, &pickers_table, sizeof(pickers_table)) == PULSE_OK,
          "PulsePickersV1 table available");
    PickerClient picker_client(pickers_table);

    // Build wizard form using new composite helpers ModelNode::LabeledField + ModelNode::PathRow.
    ModelTree step1_tree(
        ModelNode(PULSE_UI_MODEL_STACK, "form-root")
            .String(&PulseUiModelNode::orientation, "vertical")
            .Int(&PulseUiModelNode::gap, 8)
            .Children({
                ModelNode::LabeledField(
                    "archive-name-row",
                    "Archive Name",
                    ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "archive-name-input")
                        .String(&PulseUiModelNode::text, "Archive Name")
                        .String(&PulseUiModelNode::state, "archive_name"),
                    "Required output archive filename"),
                ModelNode::PathRow(
                    "dest-row",
                    "Output Folder",
                    "dest_dir",
                    "browse-dest",
                    "Browse...",
                    "Choose target folder"),
            }));
    step1_tree.State("archive_name", "backup.7z")
        .State("dest_dir", "C:\\Initial")
        .Command("browse-dest");

    const PulseUiModelTree built_step1 = step1_tree.Build();
    const PulseDialogButton buttons[2] = {
        {sizeof(PulseDialogButton), PULSE_DIALOG_BUTTON_PRIMARY, View("ok"), View("Create")},
        {sizeof(PulseDialogButton), PULSE_DIALOG_BUTTON_STANDARD, View("cancel"), View("Cancel")},
    };
    PulseDialogRequest req{};
    req.size = sizeof(req);
    req.width = 520;
    req.height = 280;
    req.default_button = 0;
    req.cancel_button = 1;
    req.button_count = 2;
    req.title = View("Create Archive");
    req.message = View("Configure destination and options.");
    req.buttons = buttons;
    req.model = &built_step1;

    // 1) V1 strictly rejects forms declaring commands with PULSE_ERROR_UNAVAILABLE.
    uint64_t rejected_id = 99;
    const PulseStatus v1_status = v1_guard.table.show(
        v1_guard.table.context,
        &req,
        nullptr,
        [](void*, uint64_t, PulseStatus, PulseStringView, const PulseUiState*, uint32_t) {},
        nullptr,
        &rejected_id);
    Diagnostic v1_diag{};
    diag_client.LastError(v1_diag);
    Check(v1_status == PULSE_ERROR_UNAVAILABLE && rejected_id == 0 &&
              v1_diag.interface_name == "pulse.dialogs" && v1_diag.field == "commands",
          "PulseDialogsV1 rejects model commands with PULSE_ERROR_UNAVAILABLE and records diagnostic");

    // 2) V2 accepts commands, exposes ActiveModalWindow(), runs BindFolderPicker, updates model dynamically.
    DialogSession session(v2_guard.table);
    bool closed_called = false;
    std::string closed_button;
    std::map<std::string, std::string> closed_states;

    const PulseStatus show_status = session.Show(
        req,
        nullptr,
        [&](uint64_t, PulseStatus st, std::string_view btn, const DialogSession::StateList& states) {
            closed_called = (st == PULSE_OK);
            closed_button.assign(btn);
            for (const auto& [k, v] : states) {
                closed_states[k] = v;
            }
        });
    Check(show_status == PULSE_OK && session.Id() != 0 && dialogs.ActiveModalWindow() == dlg_rt.hwnd,
          "DialogSession V2 opens modal with commands and exposes ActiveModalWindow()");

    std::string state_buf;
    Check(session.GetState("dest_dir", state_buf) == PULSE_OK && state_buf == "C:\\Initial",
          "PulseDialogsV2::get_state reads live form state");

    Check(dlg_rt.command_enabled && !dlg_rt.command_enabled("browse-dest"),
          "command_enabled is false before subscribe_command");

    Check(session.BindFolderPicker(picker_client, "browse-dest", "dest_dir", "Pick Output Folder") == PULSE_OK &&
              dlg_rt.command_enabled("browse-dest"),
          "BindFolderPicker subscribes in-dialog command and enables button");

    // Trigger in-dialog Browse button -> launches nested folder picker parented to dialog HWND!
    dlg_rt.on_command("browse-dest");
    Check(picker_rt.pending_spec.has_value() &&
              picker_rt.pending_spec->kind == PickerRequestKind::PickFolders &&
              picker_rt.pending_spec->initial_path == L"C:\\Initial" &&
              picker_rt.last_parent_hwnd == dlg_rt.hwnd &&
              session.Id() != 0 && !closed_called,
          "in-dialog Browse command launches nested folder picker parented to dialog HWND without closing dialog");

    PickerBackendCompletion folder_chosen;
    folder_chosen.outcome = PULSE_PICKER_ACCEPTED;
    folder_chosen.paths = {"D:\\Archives\\Out"};
    picker_rt.Complete(std::move(folder_chosen));
    Check(session.GetState("dest_dir", state_buf) == PULSE_OK && state_buf == "D:\\Archives\\Out" && session.Id() != 0,
          "accepted nested folder picker updates dialog state in place");

    // Dynamic wizard step update via PulseDialogsV2::update_model
    int step2_cmd_hits = 0;
    ModelTree step2_tree(
        ModelNode(PULSE_UI_MODEL_STACK, "step2-root")
            .Children({
                ModelNode::PathRow("dest-row", "Output Folder", "dest_dir", "browse-dest", "Browse..."),
                ModelNode(PULSE_UI_MODEL_BUTTON, "verify-btn")
                    .String(&PulseUiModelNode::text, "Test Destination")
                    .String(&PulseUiModelNode::command, "test-dest"),
            }));
    step2_tree.State("dest_dir", "C:\\FallbackIgnoredBecauseExistingPreserved")
        .State("verified", "no")
        .Command("browse-dest")
        .Command("test-dest");

    Check(session.UpdateModel(step2_tree) == PULSE_OK && dlg_rt.update_form_calls == 1,
          "PulseDialogsV2::update_model replaces dialog form dynamically");
    std::string verified_buf;
    Check(session.GetState("dest_dir", state_buf) == PULSE_OK && state_buf == "D:\\Archives\\Out" &&
              session.GetState("verified", verified_buf) == PULSE_OK && verified_buf == "no" &&
              dlg_rt.command_enabled("browse-dest"),
          "update_model preserves existing state values and surviving command subscriptions");

    Check(session.OnCommand("test-dest", [&](uint64_t, std::string_view) {
        ++step2_cmd_hits;
        (void)session.SetState("verified", "yes");
    }) == PULSE_OK, "subscribe_command binds newly added command after update_model");

    dlg_rt.on_command("test-dest");
    Check(step2_cmd_hits == 1 && session.GetState("verified", verified_buf) == PULSE_OK && verified_buf == "yes",
          "newly subscribed in-dialog command mutates dialog state without closing");

    dlg_rt.Finish(0);
    Check(closed_called && closed_button == "ok" &&
              closed_states["dest_dir"] == "D:\\Archives\\Out" &&
              closed_states["verified"] == "yes" &&
              dialogs.ActiveModalWindow() == nullptr,
          "closing dialog delivers final states once and clears ActiveModalWindow()");

    pickers.Close();
    dialogs.Close();
    diag_adapter.Close();
}

void TestPickersValidationCancelAndStop() {
    bool alive = true;
    auto sink = std::make_shared<DiagnosticsSink>();
    DiagnosticsAdapter diag_adapter(sink, [&]() { return alive; });
    PulseDiagnosticsV1 diag_table{};
    diag_adapter.Query(PULSE_DIAGNOSTICS_VERSION_1, &diag_table, sizeof(diag_table));
    DiagnosticsClient diag_client(diag_table);

    MockPickerRuntime picker_rt;
    PickersAdapter pickers(picker_rt.Backend(), [&]() { return alive; }, sink);
    PulsePickersV1 table{};
    pickers.Query(PULSE_PICKERS_VERSION_1, &table, sizeof(table));
    PickerClient client(table);

    // Invalid save_file with MULTISELECT flag rejected + diagnostic recorded.
    PulseSaveFileRequest bad_save{};
    bad_save.size = sizeof(bad_save);
    bad_save.flags = PULSE_PICKER_FLAG_MULTISELECT;
    bad_save.title = View("Save Checksum");
    const PulseStatus save_st = client.SaveFile(bad_save, [](const PickerResultData&) {});
    Diagnostic diag{};
    diag_client.LastError(diag);
    Check(save_st == PULSE_ERROR_ARGUMENT &&
              diag.status == PULSE_ERROR_ARGUMENT &&
              diag.interface_name == "pulse.pickers" &&
              diag.operation == "save_file",
          "save_file rejects PULSE_PICKER_FLAG_MULTISELECT with diagnostic");

    // QuickPick rejects duplicate item IDs and all-disabled item lists.
    const PulseQuickPickItem dup_items[2] = {
        {sizeof(PulseQuickPickItem), 0, View("sha256"), View("SHA-256"), View(""), View("icon:shield"), View("")},
        {sizeof(PulseQuickPickItem), 0, View("sha256"), View("Duplicate"), View(""), View(""), View("")},
    };
    PulseQuickPickRequest dup_qp{};
    dup_qp.size = sizeof(dup_qp);
    dup_qp.item_count = 2;
    dup_qp.items = dup_items;
    const PulseStatus dup_st = client.QuickPick(dup_qp, [](const PickerResultData&) {});
    diag_client.LastError(diag);
    Check(dup_st == PULSE_ERROR_ARGUMENT && diag.field == "id",
          "quick_pick rejects duplicate item IDs with field='id' diagnostic");

    const PulseQuickPickItem disabled_items[1] = {
        {sizeof(PulseQuickPickItem), PULSE_QUICK_PICK_ITEM_DISABLED, View("only"), View("Disabled"), View(""), View(""), View("")},
    };
    dup_qp.item_count = 1;
    dup_qp.items = disabled_items;
    const PulseStatus dis_st = client.QuickPick(dup_qp, [](const PickerResultData&) {});
    diag_client.LastError(diag);
    Check(dis_st == PULSE_ERROR_ARGUMENT && diag.field == "items",
          "quick_pick rejects all-disabled item array");

    // Valid QuickPick + busy guard + cancel_event signaling.
    const PulseQuickPickItem valid_items[2] = {
        {sizeof(PulseQuickPickItem), PULSE_QUICK_PICK_ITEM_SELECTED, View("fmt.7z"), View("7z Archive"), View("LZMA2"), View("icon:archive"), View("Ctrl+1")},
        {sizeof(PulseQuickPickItem), 0, View("fmt.zip"), View("ZIP Archive"), View("Deflate"), View("icon:folder"), View("Ctrl+2")},
    };
    PulseQuickPickRequest qp_req{};
    qp_req.size = sizeof(qp_req);
    qp_req.item_count = 2;
    qp_req.title = View("Choose Archive Format");
    qp_req.placeholder = View("Type format or pinyin...");
    qp_req.items = valid_items;

    std::optional<PickerResultData> observed;
    PulsePickerRequestId req_id = 0;
    Check(client.QuickPick(qp_req, [&](const PickerResultData& r) { observed = r; }, &req_id) == PULSE_OK &&
              req_id != 0 && picker_rt.pending_spec.has_value(),
          "quick_pick validates, copies items synchronously, and launches non-blockingly");

    // Second concurrent picker rejected with PULSE_ERROR_STATE.
    Check(client.QuickPick(qp_req, [](const PickerResultData&) {}) == PULSE_ERROR_STATE,
          "second concurrent picker request rejected with PULSE_ERROR_STATE");

    const auto cancel_ev = picker_rt.pending_spec->cancel_event;
    Check(cancel_ev && cancel_ev->handle && WaitForSingleObject(cancel_ev->handle, 0) == WAIT_TIMEOUT,
          "cancel_event starts unsignaled");

    Check(client.CancelActive() == PULSE_OK &&
              WaitForSingleObject(cancel_ev->handle, 0) == WAIT_OBJECT_0 &&
              picker_rt.last_cancelled_id == req_id &&
              !observed.has_value(),
          "PulsePickersV1::cancel signals cancel_event and notifies backend without synchronous re-entrancy");
    picker_rt.Complete(PickerBackendCompletion{PULSE_PICKER_ACCEPTED, 0, {}, "fmt.7z", {}});
    Check(observed.has_value() && observed->outcome == PULSE_PICKER_CANCELLED && observed->selected_id.empty(),
          "cancelled picker coerces racing backend completion to PULSE_PICKER_CANCELLED");

    // Accepted QuickPick flow.
    observed.reset();
    Check(client.QuickPick(qp_req, [&](const PickerResultData& r) { observed = r; }, &req_id) == PULSE_OK,
          "subsequent quick_pick succeeds after cancellation");
    PickerBackendCompletion qp_done;
    qp_done.outcome = PULSE_PICKER_ACCEPTED;
    qp_done.selected_id = "fmt.zip";
    picker_rt.Complete(std::move(qp_done));
    Check(observed.has_value() && observed->outcome == PULSE_PICKER_ACCEPTED && observed->selected_id == "fmt.zip",
          "accepted quick_pick delivers selected_id");

    // Session stop suppresses callback on an open picker.
    bool post_stop_callback = false;
    Check(client.QuickPick(qp_req, [&](const PickerResultData&) { post_stop_callback = true; }, &req_id) == PULSE_OK,
          "launch picker prior to session stop");
    const auto stop_ev = picker_rt.pending_spec->cancel_event;
    pickers.Close();
    Check(stop_ev && WaitForSingleObject(stop_ev->handle, 0) == WAIT_OBJECT_0,
          "PickersAdapter::Close signals cancel_event for active picker");
    picker_rt.Complete(PickerBackendCompletion{PULSE_PICKER_ACCEPTED, 0, {}, "fmt.7z", {}});
    Check(!post_stop_callback, "session Close() suppresses late picker completion callbacks");

    diag_adapter.Close();
}

void TestWorkbenchNotificationsAndNavigation() {
    bool alive = true;
    auto sink = std::make_shared<DiagnosticsSink>();
    DiagnosticsAdapter diag_adapter(sink, [&]() { return alive; });
    PulseDiagnosticsV1 diag_table{};
    diag_adapter.Query(PULSE_DIAGNOSTICS_VERSION_1, &diag_table, sizeof(diag_table));
    DiagnosticsClient diag_client(diag_table);

    MockWorkbenchRuntime wb_rt;
    WorkbenchAdapter workbench(wb_rt.Backend(), [&]() { return alive; }, sink);

    PulseWorkbenchV1 table{};
    Check(workbench.Query(PULSE_WORKBENCH_VERSION_1, &table, sizeof(table)) == PULSE_OK,
          "PulseWorkbenchV1 table available");
    WorkbenchClient client(table);

    // Snack with duration clamping + single-fire action token.
    const PulseWorkbenchAction undo_action{
        sizeof(PulseWorkbenchAction), 0, View("open-log"), View("View Log")};
    PulseSnackRequest snack{};
    snack.size = sizeof(snack);
    snack.flags = PULSE_SNACK_FLAG_REPLACE;
    snack.kind = PULSE_NOTICE_SUCCESS;
    snack.duration_ms = 200; // clamped to 1500 ms
    snack.text = View("Archive verified");
    snack.detail = View("0 errors");
    snack.actions = &undo_action;
    snack.action_count = 1;

    int action_fires = 0;
    std::string fired_action_id;
    PulseNotificationId snack_id = 0;
    Check(client.ShowSnack(snack, [&](PulseNotificationId id, std::string_view act) {
        if (id == snack_id) {
            ++action_fires;
            fired_action_id.assign(act);
        }
    }, &snack_id) == PULSE_OK && snack_id != 0 && wb_rt.snacks.size() == 1 &&
              wb_rt.snacks[0].duration_ms == 1500 && wb_rt.snacks[0].actions.size() == 1,
          "show_snack clamps duration_ms to 1500..15000 and registers action token");

    const uint64_t action_token = wb_rt.snacks[0].actions[0].token;
    Check(WorkbenchAdapter::DispatchHostActionToken(action_token) &&
              action_fires == 1 && fired_action_id == "open-log",
          "DispatchHostActionToken invokes plugin action callback on UI thread");
    Check(!WorkbenchAdapter::DispatchHostActionToken(action_token) && action_fires == 1,
          "notification action token is single-fire");

    // Notice action token invalidated on Close().
    PulseNoticeRequest notice{};
    notice.size = sizeof(notice);
    notice.kind = PULSE_NOTICE_WARNING;
    notice.title = View("Checksum Mismatch");
    notice.message = View("1 file differed from baseline.");
    notice.actions = &undo_action;
    notice.action_count = 1;

    bool late_notice_fired = false;
    PulseNotificationId notice_id = 0;
    Check(client.ShowNotice(notice, [&](PulseNotificationId, std::string_view) {
        late_notice_fired = true;
    }, &notice_id) == PULSE_OK && wb_rt.notices.size() == 1,
          "show_notice validates and records corner notice");
    const uint64_t notice_token = wb_rt.notices[0].actions[0].token;

    // Path validation, diff viewer, and clipboard.
    const PulseStatus rel_st = client.RevealPath("relative\\item.txt", PULSE_NAVIGATE_REVEAL_ITEM);
    Diagnostic rel_diag{};
    diag_client.LastError(rel_diag);
    Check(rel_st == PULSE_ERROR_ARGUMENT &&
              rel_diag.status == PULSE_ERROR_ARGUMENT &&
              rel_diag.interface_name == "pulse.workbench" &&
              rel_diag.field == "path",
          "reveal_path rejects relative path and records diagnostic");

    Check(client.RevealPath("C:\\Data\\report.pdf", PULSE_NAVIGATE_REVEAL_ITEM) == PULSE_OK &&
              wb_rt.last_reveal_path == L"C:\\Data\\report.pdf" &&
              wb_rt.last_reveal_mode == PULSE_NAVIGATE_REVEAL_ITEM,
          "reveal_path forwards validated local path and mode");

    Check(client.ShowTextDiff("C:\\Data\\a.txt", "C:\\Data\\b.txt") == PULSE_OK &&
              wb_rt.last_diff_left == L"C:\\Data\\a.txt" &&
              wb_rt.last_diff_right == L"C:\\Data\\b.txt",
          "show_text_diff forwards left and right absolute paths");

    Check(client.SetClipboardText("SHA256: deadbeef") == PULSE_OK &&
              wb_rt.last_clipboard == L"SHA256: deadbeef",
          "set_clipboard_text copies UTF-8 payload synchronously");

    workbench.Close();
    Check(!WorkbenchAdapter::DispatchHostActionToken(notice_token) && !late_notice_fired,
          "WorkbenchAdapter::Close invalidates pending action tokens");

    diag_adapter.Close();
}

} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    TestDiagnosticsAndModelErrors();
    TestDialogsV1VsV2AndNestedPickerFlow();
    TestPickersValidationCancelAndStop();
    TestWorkbenchNotificationsAndNavigation();
    std::printf("Summary: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

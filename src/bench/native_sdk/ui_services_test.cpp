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
#include "../../app/extensions/native/search_adapter.h"
#include "../../app/extensions/native/shell_adapter.h"
#include "../../app/extensions/native/tray_adapter.h"
#include "../../app/extensions/native/ui_model.h"
#include "../../app/extensions/native/workbench_adapter.h"

#include <pulse/client.hpp>

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
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
using pulse::sdk::SearchClient;
using pulse::sdk::SearchReplyData;
using pulse::sdk::ShellClient;
using pulse::sdk::ShellMenuReplyData;
using pulse::sdk::ShellPreviewResult;
using pulse::sdk::TrayClient;
using pulse::sdk::TraySnapshot;
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
    std::optional<PulseScreenRect> last_origin_rect;
    std::wstring last_open_search;
    std::wstring last_diff_left;
    std::wstring last_diff_right;
    std::wstring last_clipboard;
    PulseHostAppearance appearance{
        sizeof(PulseHostAppearance),
        PULSE_APPEARANCE_DARK,
        PULSE_BACKDROP_MICA,
        88u,
        0xFF3399FFu,
        1250u,
    };
    PulseHostLocale locale{
        sizeof(PulseHostLocale),
        PULSE_LOCALE_FOLLOW_SYSTEM,
        "zh-CN",
        "system",
    };

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
        b.open_search = [this](const std::wstring& query) {
            last_open_search = query;
            return PULSE_OK;
        };
        b.reveal_path_from_rect = [this](const std::wstring& path, uint32_t mode, const PulseScreenRect* rect) {
            last_reveal_path = path;
            last_reveal_mode = mode;
            if (rect) {
                last_origin_rect = *rect;
            } else {
                last_origin_rect.reset();
            }
            return PULSE_OK;
        };
        b.query_appearance = [this](PulseHostAppearance& out) {
            out = appearance;
            out.size = sizeof(PulseHostAppearance);
            return PULSE_OK;
        };
        b.query_locale = [this](PulseHostLocale& out) {
            out = locale;
            out.size = sizeof(PulseHostLocale);
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

    // Regression: PickerClient, DialogSession, and WorkbenchClient move assignment +
    // WorkbenchClient >64 notification eviction and early destruction never UAF.
    {
        struct RecordedAction {
            PulseWorkbenchActionFn cb = nullptr;
            void* user = nullptr;
            PulseNotificationId id = 0;
        };
        static std::vector<RecordedAction> recorded;
        recorded.clear();
        struct MockHost {
            static PulseStatus PULSE_CALL FakeOpenFiles(void*, const PulseOpenFilesRequest*,
                                                        PulsePickerResultFn, void*,
                                                        PulsePickerRequestId*) noexcept {
                return PULSE_OK;
            }
            static PulseStatus PULSE_CALL FakeShowDialog(void*, const PulseDialogRequest*,
                                                         PulseDialogStateChanged,
                                                         PulseDialogClosed, void*,
                                                         uint64_t*) noexcept {
                return PULSE_OK;
            }
            static PulseStatus PULSE_CALL FakeShowSnack(void*, const PulseSnackRequest*,
                                                        PulseWorkbenchActionFn, void*,
                                                        PulseNotificationId*) noexcept {
                return PULSE_OK;
            }
            static PulseStatus PULSE_CALL FakeShowNotice(void*, const PulseNoticeRequest*,
                                                         PulseWorkbenchActionFn cb, void* user,
                                                         PulseNotificationId* out_id) noexcept {
                const PulseNotificationId id = static_cast<PulseNotificationId>(recorded.size() + 1);
                recorded.push_back({cb, user, id});
                if (out_id) *out_id = id;
                return PULSE_OK;
            }
            static PulseStatus PULSE_CALL Query(void*, PulseStringView name, uint32_t version,
                                                void* out, uint32_t size) noexcept {
                const std::string_view sv(name.data ? name.data : "", name.length);
                if (sv == PULSE_PICKERS_INTERFACE_NAME && version == PULSE_PICKERS_VERSION_1 &&
                    out && size >= sizeof(PulsePickersV1)) {
                    PulsePickersV1 t{};
                    t.size = sizeof(t);
                    t.version = PULSE_PICKERS_VERSION_1;
                    t.open_files = FakeOpenFiles;
                    std::memcpy(out, &t, sizeof(t));
                    return PULSE_OK;
                }
                if (sv == PULSE_DIALOGS_INTERFACE_NAME && version == PULSE_DIALOGS_VERSION_2 &&
                    out && size >= sizeof(PulseDialogsV2)) {
                    PulseDialogsV2 t{};
                    t.size = sizeof(t);
                    t.version = PULSE_DIALOGS_VERSION_2;
                    t.show = FakeShowDialog;
                    std::memcpy(out, &t, sizeof(t));
                    return PULSE_OK;
                }
                if (sv == PULSE_WORKBENCH_INTERFACE_NAME && version == PULSE_WORKBENCH_VERSION_1 &&
                    out && size >= sizeof(PulseWorkbenchV1)) {
                    PulseWorkbenchV1 t{};
                    t.size = sizeof(t);
                    t.version = PULSE_WORKBENCH_VERSION_1;
                    t.show_snack = FakeShowSnack;
                    t.show_notice = FakeShowNotice;
                    std::memcpy(out, &t, sizeof(t));
                    return PULSE_OK;
                }
                return PULSE_ERROR_UNAVAILABLE;
            }
        };
        const PulseHostV1 raw_host{sizeof(PulseHostV1), PULSE_ABI_VERSION_1, nullptr, MockHost::Query};
        const pulse::sdk::Host sdk_host(raw_host);

        std::optional<PickerClient> moved_picker;
        moved_picker = PickerClient::TryQuery(sdk_host);
        std::optional<DialogSession> moved_dialog;
        moved_dialog = DialogSession::TryQuery(sdk_host);
        std::optional<WorkbenchClient> moved_wb;
        moved_wb = WorkbenchClient::TryQuery(sdk_host);
        Check(moved_picker.has_value() && moved_picker->Available() &&
                  moved_dialog.has_value() && moved_dialog->Available() && moved_dialog->SupportsV2() &&
                  moved_wb.has_value() && moved_wb->Available(),
              "PickerClient, DialogSession, and WorkbenchClient support TryQuery move assignment");

        int latest_hit = -1;
        for (int i = 0; i < 70; ++i) {
            PulseWorkbenchAction act{sizeof(PulseWorkbenchAction), 0, View("open"), View("Open")};
            PulseNoticeRequest req{};
            req.size = sizeof(req);
            req.title = View("Title");
            req.message = View("Body");
            req.actions = &act;
            req.action_count = 1;
            moved_wb->ShowNotice(req, [&, i](PulseNotificationId, std::string_view) { latest_hit = i; });
        }
        // Invoking evicted notice #1 must safely no-op without UAF; invoking notice #70 fires cleanly.
        recorded[0].cb(recorded[0].user, recorded[0].id, View("open"));
        Check(latest_hit == -1, "evicted WorkbenchClient notice callback safely no-ops without UAF");
        recorded[69].cb(recorded[69].user, recorded[69].id, View("open"));
        Check(latest_hit == 69, "retained WorkbenchClient notice callback dispatches by notification ID");

        // Destroying WorkbenchClient before late callback must also safely no-op without UAF.
        moved_wb.reset();
        latest_hit = -1;
        recorded[69].cb(recorded[69].user, recorded[69].id, View("open"));
        Check(latest_hit == -1, "destroyed WorkbenchClient safely ignores late host action callback");
    }
}

void TestWorkbenchV2AppearanceSearchAndRectReveal() {
    bool alive = true;
    int wake_count = 0;
    auto sink = std::make_shared<DiagnosticsSink>();
    DiagnosticsAdapter diag_adapter(sink, [&]() { return alive; });
    PulseDiagnosticsV1 diag_table{};
    diag_adapter.Query(PULSE_DIAGNOSTICS_VERSION_1, &diag_table, sizeof(diag_table));
    DiagnosticsClient diag_client(diag_table);

    MockWorkbenchRuntime wb_rt;
    WorkbenchAdapter workbench(wb_rt.Backend(), [&]() { return alive; }, sink, [&]() { ++wake_count; });

    struct GuardedV1 {
        PulseWorkbenchV1 table{};
        uint64_t canary = 0xCAFEBABE11223344ULL;
    } v1_guard;
    struct GuardedV2 {
        PulseWorkbenchV2 table{};
        uint64_t canary = 0x44332211BABECAFEULL;
    } v2_guard;
    struct GuardedV3 {
        PulseWorkbenchV3 table{};
        uint64_t canary = 0x9988776655443322ULL;
    } v3_guard;

    Check(workbench.Query(PULSE_WORKBENCH_VERSION_1, &v1_guard.table, sizeof(v1_guard)) == PULSE_OK &&
              v1_guard.canary == 0xCAFEBABE11223344ULL &&
              v1_guard.table.size == sizeof(PulseWorkbenchV1) &&
              v1_guard.table.version == PULSE_WORKBENCH_VERSION_1,
          "PulseWorkbenchV1 query preserves trailing canary");
    Check(workbench.Query(PULSE_WORKBENCH_VERSION_2, &v2_guard.table, sizeof(v2_guard)) == PULSE_OK &&
              v2_guard.canary == 0x44332211BABECAFEULL &&
              v2_guard.table.size == sizeof(PulseWorkbenchV2) &&
              v2_guard.table.version == PULSE_WORKBENCH_VERSION_2,
          "PulseWorkbenchV2 query preserves trailing canary");
    Check(workbench.Query(PULSE_WORKBENCH_VERSION_3, &v3_guard.table, sizeof(v3_guard)) == PULSE_OK &&
              v3_guard.canary == 0x9988776655443322ULL &&
              v3_guard.table.size == sizeof(PulseWorkbenchV3) &&
              v3_guard.table.version == PULSE_WORKBENCH_VERSION_3,
          "PulseWorkbenchV3 query preserves trailing canary");
    Check(workbench.Query(4, &v3_guard.table, sizeof(v3_guard)) == PULSE_ERROR_ABI &&
              workbench.Query(PULSE_WORKBENCH_VERSION_2, &v2_guard.table, sizeof(PulseWorkbenchV1)) == PULSE_ERROR_ARGUMENT &&
              workbench.Query(PULSE_WORKBENCH_VERSION_3, &v3_guard.table, sizeof(PulseWorkbenchV2)) == PULSE_ERROR_ARGUMENT,
          "WorkbenchAdapter rejects unsupported version (ABI) and undersized V2/V3 buffers (ARGUMENT)");

    // V1/V2 WorkbenchClient graceful degradation.
    WorkbenchClient v1_client(v1_guard.table);
    WorkbenchClient v2_only_client(v2_guard.table);
    PulseHostAppearance dummy_app{};
    PulseHostLocale dummy_loc{};
    Check(v1_client.Available() && !v1_client.SupportsV2() && !v1_client.SupportsV3() &&
              v1_client.OpenSearch("report") == PULSE_ERROR_UNAVAILABLE &&
              v1_client.RevealPathFromRect("C:\\Docs\\a.txt") == PULSE_ERROR_UNAVAILABLE &&
              v1_client.QueryAppearance(dummy_app) == PULSE_ERROR_UNAVAILABLE &&
              v1_client.QueryLocale(dummy_loc) == PULSE_ERROR_UNAVAILABLE &&
              v1_client.QueryLanguageTag().empty() &&
              v2_only_client.SupportsV2() && !v2_only_client.SupportsV3() &&
              v2_only_client.QueryLocale(dummy_loc) == PULSE_ERROR_UNAVAILABLE &&
              v2_only_client.QueryLanguageTag().empty(),
          "V1/V2-bound WorkbenchClient degrades newer operations to PULSE_ERROR_UNAVAILABLE");

    // V3 negotiation via Host::Query.
    static WorkbenchAdapter* s_wb_ptr = nullptr;
    s_wb_ptr = &workbench;
    const PulseHostV1 host_v2{
        sizeof(PulseHostV1),
        PULSE_ABI_VERSION_1,
        nullptr,
        [](void*, PulseStringView name, uint32_t ver, void* out, uint32_t sz) noexcept -> PulseStatus {
            const std::string_view sv(name.data ? name.data : "", name.length);
            if (sv == PULSE_WORKBENCH_INTERFACE_NAME && s_wb_ptr) {
                return s_wb_ptr->Query(ver, out, sz);
            }
            return PULSE_ERROR_UNAVAILABLE;
        },
    };
    auto client_opt = WorkbenchClient::TryQuery(pulse::sdk::Host(host_v2));
    Check(client_opt.has_value() && client_opt->Available() && client_opt->SupportsV2() && client_opt->SupportsV3(),
          "WorkbenchClient::TryQuery negotiates PulseWorkbenchV3 when supported");
    WorkbenchClient& client = *client_opt;

    // open_search validation + execution.
    Diagnostic diag{};
    Check(client.OpenSearch("") == PULSE_ERROR_ARGUMENT &&
              diag_client.LastError(diag) == PULSE_OK &&
              diag.interface_name == "pulse.workbench" &&
              diag.operation == "open_search" &&
              diag.field == "query",
          "open_search rejects empty query and records diagnostic");
    Check(client.OpenSearch("kind:image invoice") == PULSE_OK &&
              wb_rt.last_open_search == L"kind:image invoice",
          "open_search validates UTF-8 query and forwards wide string");

    // reveal_path_from_rect validation + execution.
    const PulseScreenRect bad_rect{10, 20, -5, 40};
    Check(client.RevealPathFromRect("C:\\Users\\Public\\Desktop\\note.txt", PULSE_NAVIGATE_REVEAL_ITEM, &bad_rect) ==
              PULSE_ERROR_ARGUMENT &&
              diag_client.LastError(diag) == PULSE_OK &&
              diag.operation == "reveal_path_from_rect" &&
              diag.field == "origin_rect",
          "reveal_path_from_rect rejects negative origin_rect dimensions");

    const PulseScreenRect ok_rect{120, 240, 88, 96};
    Check(client.RevealPathFromRect("C:\\Users\\Public\\Desktop\\note.txt", PULSE_NAVIGATE_CURRENT_TAB, &ok_rect) ==
              PULSE_OK &&
              wb_rt.last_reveal_path == L"C:\\Users\\Public\\Desktop\\note.txt" &&
              wb_rt.last_reveal_mode == PULSE_NAVIGATE_CURRENT_TAB &&
              wb_rt.last_origin_rect.has_value() &&
              wb_rt.last_origin_rect->x == 120 &&
              wb_rt.last_origin_rect->width == 88,
          "reveal_path_from_rect forwards validated path, mode, and origin_rect");
    for (uint32_t mode : {3u, 4u, 5u}) {
        Check(client.RevealPathFromRect("C:\\Users\\Public\\Desktop\\note.txt", mode, &ok_rect) == PULSE_OK &&
                  wb_rt.last_reveal_mode == mode && wb_rt.last_origin_rect && wb_rt.last_origin_rect->x == 120,
              "V2 preview modes preserve path, mode and screen anchor");
    }
    Check(client.RevealPathFromRect("C:\\fixture\\file.txt", 6u, &ok_rect) == PULSE_ERROR_ARGUMENT,
          "unknown preview navigation mode remains rejected");

    // query_appearance + subscribe_appearance + Sync() coalescing + RAII unsubscribe.
    PulseHostAppearance queried{};
    Check(client.QueryAppearance(queried) == PULSE_OK &&
              queried.flags == PULSE_APPEARANCE_DARK &&
              queried.window_effect == PULSE_BACKDROP_MICA &&
              queried.opacity_percent == 88u &&
              queried.accent_argb == 0xFF3399FFu &&
              queried.font_scale_milli == 1250u,
          "query_appearance populates PulseHostAppearance fields");

    std::vector<PulseHostAppearance> observed_app;
    const int wake_before = wake_count;
    auto sub = client.SubscribeAppearance([&](const PulseHostAppearance& app) {
        observed_app.push_back(app);
    });
    Check(sub.has_value() && sub->Active() && wake_count == wake_before + 1 &&
              observed_app.empty() && workbench.HasSubscriptions(),
          "subscribe_appearance wakes session and defers initial callback until Sync()");

    workbench.Sync();
    Check(observed_app.size() == 1 && observed_app[0].accent_argb == 0xFF3399FFu,
          "WorkbenchAdapter::Sync delivers deferred initial appearance snapshot");

    workbench.Sync();
    Check(observed_app.size() == 1, "WorkbenchAdapter::Sync suppresses unchanged appearance notifications");

    wb_rt.appearance.flags = PULSE_APPEARANCE_LIGHT;
    wb_rt.appearance.window_effect = PULSE_BACKDROP_ACRYLIC;
    wb_rt.appearance.opacity_percent = 72u;
    workbench.Sync();
    Check(observed_app.size() == 2 &&
              observed_app[1].flags == PULSE_APPEARANCE_LIGHT &&
              observed_app[1].window_effect == PULSE_BACKDROP_ACRYLIC &&
              observed_app[1].opacity_percent == 72u,
          "WorkbenchAdapter::Sync delivers coalesced appearance update on theme/backdrop change");

    // Move safety + RAII unsubscribe.
    WorkbenchClient::AppearanceSubscription moved_sub = std::move(*sub);
    sub.reset();
    wb_rt.appearance.opacity_percent = 95u;
    workbench.Sync();
    Check(observed_app.size() == 3 && observed_app[2].opacity_percent == 95u,
          "moved AppearanceSubscription continues receiving updates");

    moved_sub.Reset();
    wb_rt.appearance.opacity_percent = 60u;
    workbench.Sync();
    Check(!workbench.HasSubscriptions() && observed_app.size() == 3,
          "AppearanceSubscription::Reset unsubscribes and stops future callbacks");

    // 16-subscription cap check.
    std::vector<WorkbenchClient::AppearanceSubscription> many_subs;
    for (int i = 0; i < 16; ++i) {
        auto s = client.SubscribeAppearance([](const PulseHostAppearance&) {});
        if (s.has_value()) many_subs.push_back(std::move(*s));
    }
    PulseStatus over_cap = PULSE_OK;
    auto rejected_sub = client.SubscribeAppearance([](const PulseHostAppearance&) {}, &over_cap);
    Check(many_subs.size() == 16 && !rejected_sub.has_value() && over_cap == PULSE_ERROR_STATE,
          "subscribe_appearance enforces 16-subscription limit per session");
    many_subs.clear();

    // PulseWorkbenchV3 query_locale + subscribe_locale + coalescing + RAII unsubscribe + 16-sub cap.
    PulseHostLocale queried_loc{};
    Check(client.QueryLocale(queried_loc) == PULSE_OK &&
              queried_loc.flags == PULSE_LOCALE_FOLLOW_SYSTEM &&
              std::string_view(queried_loc.language_tag) == "zh-CN" &&
              std::string_view(queried_loc.preference_tag) == "system" &&
              client.QueryLanguageTag() == "zh-CN",
          "query_locale and QueryLanguageTag return normalized host UI locale");

    std::vector<std::string> observed_langs;
    std::vector<std::string> observed_prefs;
    const int wake_loc_before = wake_count;
    auto loc_sub = client.SubscribeLocale([&](const PulseHostLocale& loc) {
        observed_langs.emplace_back(loc.language_tag);
        observed_prefs.emplace_back(loc.preference_tag);
    });
    Check(loc_sub.has_value() && loc_sub->Active() && wake_count == wake_loc_before + 1 &&
              observed_langs.empty() && workbench.HasSubscriptions(),
          "subscribe_locale wakes session and defers initial callback until Sync()");

    workbench.Sync();
    Check(observed_langs.size() == 1 && observed_langs[0] == "zh-CN" && observed_prefs[0] == "system",
          "WorkbenchAdapter::Sync delivers deferred initial locale snapshot");

    workbench.Sync();
    Check(observed_langs.size() == 1, "WorkbenchAdapter::Sync suppresses unchanged locale notifications");

    wb_rt.locale.flags = 0;
    std::memcpy(wb_rt.locale.language_tag, "en-US", 6);
    std::memcpy(wb_rt.locale.preference_tag, "en-US", 6);
    workbench.Sync();
    Check(observed_langs.size() == 2 && observed_langs[1] == "en-US" && observed_prefs[1] == "en-US",
          "WorkbenchAdapter::Sync delivers coalesced locale update on language switch");

    WorkbenchClient::LocaleSubscription moved_loc_sub = std::move(*loc_sub);
    loc_sub.reset();
    std::memcpy(wb_rt.locale.language_tag, "zh-TW", 6);
    std::memcpy(wb_rt.locale.preference_tag, "zh-TW", 6);
    workbench.Sync();
    Check(observed_langs.size() == 3 && observed_langs[2] == "zh-TW",
          "moved LocaleSubscription continues receiving updates");

    moved_loc_sub.Reset();
    std::memcpy(wb_rt.locale.language_tag, "zh-CN", 6);
    workbench.Sync();
    Check(!workbench.HasSubscriptions() && observed_langs.size() == 3,
          "LocaleSubscription::Reset unsubscribes and stops future callbacks");

    std::vector<WorkbenchClient::LocaleSubscription> many_loc_subs;
    for (int i = 0; i < 16; ++i) {
        auto s = client.SubscribeLocale([](const PulseHostLocale&) {});
        if (s.has_value()) many_loc_subs.push_back(std::move(*s));
    }
    PulseStatus loc_over_cap = PULSE_OK;
    auto rejected_loc_sub = client.SubscribeLocale([](const PulseHostLocale&) {}, &loc_over_cap);
    Check(many_loc_subs.size() == 16 && !rejected_loc_sub.has_value() && loc_over_cap == PULSE_ERROR_STATE,
          "subscribe_locale enforces 16-subscription limit per session");
    many_loc_subs.clear();

    s_wb_ptr = nullptr;
    workbench.Close();
    diag_adapter.Close();
}

void TestTrayAdapterAndClient() {
    bool alive = true;
    int wake_count = 0;
    auto sink = std::make_shared<DiagnosticsSink>();
    DiagnosticsAdapter diag_adapter(sink, [&]() { return alive; });
    PulseDiagnosticsV1 diag_table{};
    diag_adapter.Query(PULSE_DIAGNOSTICS_VERSION_1, &diag_table, sizeof(diag_table));
    DiagnosticsClient diag_client(diag_table);

    struct MockTrayState {
        uint64_t next_item = 100;
        uint64_t next_batch = 10;
        std::vector<TrayEntryData> items;
        std::wstring last_release_dest;
        uint32_t last_release_mode = 99;
    } tray_rt;

    TrayBackend backend;
    backend.valid_owner = [&]() { return alive; };
    backend.snapshot = [&](TraySnapshotData& out) {
        out.entries = tray_rt.items;
        out.total_bytes = 0;
        for (const auto& e : out.entries) out.total_bytes += e.size_bytes;
        return PULSE_OK;
    };
    backend.collect = [&](const std::vector<std::wstring>& paths, bool move_intent) {
        const uint64_t bid = tray_rt.next_batch++;
        for (const auto& wp : paths) {
            TrayEntryData item;
            item.item_id = tray_rt.next_item++;
            item.batch_id = bid;
            item.size_bytes = 4096;
            item.flags = move_intent ? PULSE_TRAY_ITEM_MOVE_INTENT : PULSE_TRAY_ITEM_NONE;
            int n = WideCharToMultiByte(CP_UTF8, 0, wp.data(), static_cast<int>(wp.size()), nullptr, 0, nullptr, nullptr);
            item.path.assign(static_cast<size_t>(n > 0 ? n : 0), '\0');
            if (n > 0) {
                WideCharToMultiByte(CP_UTF8, 0, wp.data(), static_cast<int>(wp.size()), item.path.data(), n, nullptr, nullptr);
            }
            const auto slash = item.path.find_last_of("\\/");
            item.name = (slash == std::string::npos) ? item.path : item.path.substr(slash + 1);
            tray_rt.items.push_back(std::move(item));
        }
        return PULSE_OK;
    };
    backend.remove_item = [&](uint64_t item_id) {
        for (auto it = tray_rt.items.begin(); it != tray_rt.items.end(); ++it) {
            if (it->item_id == item_id) {
                tray_rt.items.erase(it);
                return PULSE_OK;
            }
        }
        return PULSE_ERROR_STATE;
    };
    backend.clear = [&]() {
        tray_rt.items.clear();
        return PULSE_OK;
    };
    backend.release_to_directory = [&](const std::wstring& dest, uint32_t mode) {
        if (tray_rt.items.empty()) return PULSE_ERROR_STATE;
        tray_rt.last_release_dest = dest;
        tray_rt.last_release_mode = mode;
        tray_rt.items.clear();
        return PULSE_OK;
    };

    TrayAdapter tray(std::move(backend), [&]() { return alive; }, sink, [&]() { ++wake_count; });

    struct GuardedTray {
        PulseTrayV1 table{};
        uint64_t canary = 0x77AA55CC33EE1100ULL;
    } guarded;
    Check(tray.Query(PULSE_TRAY_VERSION_1, &guarded.table, sizeof(guarded)) == PULSE_OK &&
              guarded.canary == 0x77AA55CC33EE1100ULL &&
              guarded.table.version == PULSE_TRAY_VERSION_1,
          "PulseTrayV1 query preserves trailing canary");
    Check(tray.Query(2, &guarded.table, sizeof(guarded)) == PULSE_ERROR_ABI,
          "TrayAdapter rejects unknown version with PULSE_ERROR_ABI");

    TrayClient client(guarded.table);
    std::vector<TraySnapshot> updates;
    auto sub = client.Subscribe([&](const TraySnapshot& snap) {
        updates.push_back(snap);
    });
    Check(sub.has_value() && sub->Active() && tray.HasSubscriptions() && wake_count == 1 && updates.empty(),
          "TrayClient::Subscribe registers deferred subscription and wakes session");

    tray.Sync();
    Check(updates.size() == 1 && updates[0].entries.empty() && updates[0].revision >= 1,
          "TrayAdapter::Sync delivers initial empty snapshot with non-zero revision");

    // Validation checks on collect & release_to_directory.
    Diagnostic diag{};
    Check(client.Collect({}) == PULSE_ERROR_ARGUMENT &&
              diag_client.LastError(diag) == PULSE_OK &&
              diag.interface_name == "pulse.tray" &&
              diag.operation == "collect",
          "TrayClient::Collect rejects empty path list with diagnostic");
    Check(client.Collect({"relative\\file.txt"}) == PULSE_ERROR_ARGUMENT,
          "TrayClient::Collect rejects relative path");

    Check(client.Collect({"C:\\Staging\\alpha.png", "C:\\Staging\\beta.png"}, true) == PULSE_OK,
          "TrayClient::Collect stages two absolute paths with move_intent");

    TraySnapshot sync_snap;
    Check(client.Snapshot(sync_snap) == PULSE_OK &&
              sync_snap.entries.size() == 2 &&
              sync_snap.total_bytes == 8192 &&
              (sync_snap.entries[0].flags & PULSE_TRAY_ITEM_MOVE_INTENT) != 0 &&
              sync_snap.entries[0].name == "alpha.png",
          "TrayClient::Snapshot synchronously copies staged entries and total_bytes");

    tray.Sync();
    Check(updates.size() == 2 && updates[1].entries.size() == 2 && updates[1].revision > updates[0].revision,
          "TrayAdapter::Sync advances monotonic revision and delivers updated items");

    tray.Sync();
    Check(updates.size() == 2, "TrayAdapter::Sync coalesces unchanged tray state");

    Check(client.RemoveItem(999999) == PULSE_ERROR_STATE &&
              client.RemoveItem(sync_snap.entries[0].item_id) == PULSE_OK,
          "TrayClient::RemoveItem removes matching item_id and rejects unknown item_id");

    Check(client.ReleaseToDirectory("C:\\Out", 99u) == PULSE_ERROR_ARGUMENT &&
              client.ReleaseToDirectory("relative\\out", PULSE_TRAY_RELEASE_COPY) == PULSE_ERROR_ARGUMENT,
          "TrayClient::ReleaseToDirectory validates release_mode and absolute destination_dir");

    Check(client.ReleaseToDirectory("C:\\Out", PULSE_TRAY_RELEASE_MOVE) == PULSE_OK &&
              tray_rt.last_release_dest == L"C:\\Out" &&
              tray_rt.last_release_mode == PULSE_TRAY_RELEASE_MOVE,
          "TrayClient::ReleaseToDirectory forwards destination_dir and release_mode");

    tray.Sync();
    Check(updates.size() == 3 && updates[2].entries.empty() && updates[2].revision > updates[1].revision,
          "TrayAdapter::Sync delivers cleared snapshot after release");

    sub->Reset();
    Check(!tray.HasSubscriptions(), "TrayClient::Subscription::Reset unsubscribes cleanly");

    tray.Close();
    diag_adapter.Close();
}

void TestSearchAdapterAndClient() {
    bool alive = true;
    int wake_count = 0;
    auto sink = std::make_shared<DiagnosticsSink>();
    DiagnosticsAdapter diag_adapter(sink, [&]() { return alive; });
    PulseDiagnosticsV1 diag_table{};
    diag_adapter.Query(PULSE_DIAGNOSTICS_VERSION_1, &diag_table, sizeof(diag_table));
    DiagnosticsClient diag_client(diag_table);

    struct PendingSearch {
        SearchQuerySpec spec;
        std::function<void(SearchBackendReply)> complete;
    };
    std::map<uint64_t, PendingSearch> in_flight;
    std::vector<uint64_t> cancelled_ids;

    SearchBackend backend;
    backend.valid_owner = [&]() { return alive; };
    backend.start = [&](SearchQuerySpec spec, std::function<void(SearchBackendReply)> complete) {
        const uint64_t id = spec.request_id;
        in_flight[id] = PendingSearch{std::move(spec), std::move(complete)};
        return PULSE_OK;
    };
    backend.cancel = [&](uint64_t request_id) {
        cancelled_ids.push_back(request_id);
    };

    SearchAdapter search(std::move(backend), [&]() { return alive; }, sink, [&]() { ++wake_count; });

    struct GuardedSearch {
        PulseSearchV1 table{};
        uint64_t canary = 0x5EA2C4015EA2C402ULL;
    } guarded;
    Check(search.Query(PULSE_SEARCH_VERSION_1, &guarded.table, sizeof(guarded)) == PULSE_OK &&
              guarded.canary == 0x5EA2C4015EA2C402ULL,
          "PulseSearchV1 query preserves trailing canary");

    SearchClient client(guarded.table);

    // Validation cases.
    PulseStatus st = PULSE_OK;
    Diagnostic diag{};
    auto bad1 = client.Query("", [](const SearchReplyData&) {}, PULSE_SEARCH_FLAG_NONE, 20, {}, {}, &st);
    diag_client.LastError(diag);
    Check(!bad1.has_value() && st == PULSE_ERROR_ARGUMENT &&
              diag.interface_name == "pulse.search" && diag.field == "query",
          "SearchClient::Query rejects empty query with diagnostic");

    auto bad2 = client.Query(
        "report",
        [](const SearchReplyData&) {},
        PULSE_SEARCH_FLAG_FOLDERS_ONLY | PULSE_SEARCH_FLAG_FILES_ONLY,
        20,
        {},
        {},
        &st);
    Check(!bad2.has_value() && st == PULSE_ERROR_ARGUMENT,
          "SearchClient::Query rejects mutually exclusive FOLDERS_ONLY | FILES_ONLY flags");

    auto bad3 = client.Query("report", [](const SearchReplyData&) {}, PULSE_SEARCH_FLAG_NONE, 20, {}, "rel\\dir", &st);
    Check(!bad3.has_value() && st == PULSE_ERROR_ARGUMENT,
          "SearchClient::Query rejects relative scope_directory");

    // Default max_results (0 -> 20) and clamping (500 -> 50) + async Poll() delivery.
    std::optional<SearchReplyData> reply1;
    auto req1 = client.Query(
        "budget",
        [&](const SearchReplyData& r) { reply1 = r; },
        PULSE_SEARCH_FLAG_PINYIN,
        0,
        "doc",
        "C:\\Users\\Public\\Desktop",
        &st);
    Check(req1.has_value() && req1->Pending() && st == PULSE_OK && search.HasPendingWork(),
          "SearchClient::Query accepts valid scoped kind query");
    const uint64_t id1 = req1->Id();
    Check(in_flight.contains(id1) &&
              in_flight[id1].spec.max_results == 20u &&
              in_flight[id1].spec.kind_token == L"doc" &&
              in_flight[id1].spec.scope_directory == L"C:\\Users\\Public\\Desktop",
          "SearchAdapter defaults max_results=0 to 20 and forwards kind_token + scope_directory");

    SearchBackendReply mock_reply;
    mock_reply.status = PULSE_OK;
    mock_reply.total_matches = 1;
    mock_reply.index_connected = true;
    mock_reply.hits.push_back(SearchHitData{
        "C:\\Users\\Public\\Desktop\\budget_2026.xlsx",
        "budget_2026.xlsx",
        16384,
        1760000000000LL,
        PULSE_SEARCH_HIT_NONE,
    });
    const int wake_before = wake_count;
    in_flight[id1].complete(std::move(mock_reply));
    Check(wake_count == wake_before + 1 && !reply1.has_value(),
          "backend search completion queues result and wakes owner without off-thread callback");

    search.Poll();
    Check(reply1.has_value() &&
              reply1->status == PULSE_OK &&
              reply1->index_connected &&
              reply1->total_matches == 1 &&
              reply1->hits.size() == 1 &&
              reply1->hits[0].name == "budget_2026.xlsx" &&
              !req1->Pending() &&
              !search.HasPendingWork(),
          "SearchAdapter::Poll delivers SearchReplyData on owner thread and clears Pending()");

    // Cancellation suppresses racing completion + RAII auto-cancel.
    bool cancelled_fired = false;
    auto req2 = client.Query("draft", [&](const SearchReplyData&) { cancelled_fired = true; }, 0, 500);
    Check(req2.has_value() && in_flight[req2->Id()].spec.max_results == 50u,
          "SearchAdapter clamps max_results > 50 to 50");
    const uint64_t id2 = req2->Id();
    req2.reset(); // RAII ~Request() calls Cancel()!
    Check(!cancelled_ids.empty() && cancelled_ids.back() == id2,
          "destroying SearchClient::Request cancels active request in backend");
    in_flight[id2].complete(SearchBackendReply{PULSE_OK, 3, true, {}});
    search.Poll();
    Check(!cancelled_fired, "cancelled search request suppresses late backend completion");

    // 16 active request cap.
    std::vector<SearchClient::Request> active_reqs;
    for (int i = 0; i < 16; ++i) {
        auto r = client.Query("q", [](const SearchReplyData&) {});
        if (r.has_value()) active_reqs.push_back(std::move(*r));
    }
    PulseStatus cap_st = PULSE_OK;
    auto cap_rejected = client.Query("overflow", [](const SearchReplyData&) {}, 0, 10, {}, {}, &cap_st);
    Check(active_reqs.size() == 16 && !cap_rejected.has_value() && cap_st == PULSE_ERROR_STATE,
          "SearchAdapter enforces 16 active request limit");
    active_reqs.clear();

    search.Close();
    diag_adapter.Close();
}

void TestShellAdapterAndClient() {
    bool alive = true;
    int wake_count = 0;
    auto sink = std::make_shared<DiagnosticsSink>();
    DiagnosticsAdapter diag_adapter(sink, [&]() { return alive; });

    std::map<std::wstring, std::set<std::wstring>> tagged_paths;
    std::map<uint64_t, std::function<void(ShellMenuBackendReply)>> menu_sessions;
    uint64_t last_invoked_session = 0;
    uint32_t last_invoked_item = 0;
    std::wstring last_invoked_verb;
    uint64_t last_closed_session = 0;
    uint32_t last_preview_thumb_px = 0;
    uint64_t last_cancelled_preview = 0;
    std::vector<std::thread> preview_workers;

    ShellBackend backend;
    backend.valid_owner = [&]() { return alive; };
    backend.get_tag_catalog = [](std::vector<ColorTagInfoData>& out) {
        out.push_back({"work", "Work", 0xFF0078D4u, 1});
        out.push_back({"urgent", "Urgent", 0xFFE81123u, 2});
        return PULSE_OK;
    };
    backend.get_path_tags = [&](const std::wstring& path, std::vector<ColorTagInfoData>& out) {
        const auto it = tagged_paths.find(path);
        if (it != tagged_paths.end()) {
            if (it->second.contains(L"work")) out.push_back({"work", "Work", 0xFF0078D4u, 1});
            if (it->second.contains(L"urgent")) out.push_back({"urgent", "Urgent", 0xFFE81123u, 2});
        }
        return PULSE_OK;
    };
    backend.set_path_tag = [&](const std::wstring& path, const std::wstring& tag_id, bool tagged) {
        if (tag_id != L"work" && tag_id != L"urgent") return PULSE_ERROR_ARGUMENT;
        if (tagged) {
            tagged_paths[path].insert(tag_id);
        } else {
            tagged_paths[path].erase(tag_id);
        }
        return PULSE_OK;
    };
    backend.query_context_menu = [&](uint64_t sid, const std::vector<std::wstring>&, uint32_t,
                                     std::function<void(ShellMenuBackendReply)> deliver) {
        menu_sessions[sid] = std::move(deliver);
        return PULSE_OK;
    };
    backend.invoke_context_menu = [&](uint64_t sid, uint32_t item_id, const std::wstring& verb) {
        last_invoked_session = sid;
        last_invoked_item = item_id;
        last_invoked_verb = verb;
        menu_sessions.erase(sid);
        return PULSE_OK;
    };
    backend.close_context_menu = [&](uint64_t sid) {
        last_closed_session = sid;
        menu_sessions.erase(sid);
        return PULSE_OK;
    };
    backend.read_preview = [&](uint64_t, const std::wstring&, uint32_t max_thumb_px,
                               std::function<void(ShellPreviewData)> complete) {
        last_preview_thumb_px = max_thumb_px;
        preview_workers.emplace_back([complete = std::move(complete)]() mutable {
            ShellPreviewData data;
            data.status = PULSE_OK;
            data.path = "C:\\Users\\Public\\Desktop\\readme.md";
            data.display_type = "Markdown Document";
            data.text_excerpt = "# Desktop Quick Look";
            data.size_bytes = 20;
            data.modified_unix_ms = 1760000100000LL;
            data.image_width = 64;
            data.image_height = 64;
            data.thumb_width = 2;
            data.thumb_height = 2;
            data.thumb_stride = 8;
            data.thumb_bgra.assign(16, 0xCCu);
            complete(std::move(data));
        });
        return PULSE_OK;
    };
    backend.cancel_preview = [&](uint64_t rid) {
        last_cancelled_preview = rid;
    };

    ShellAdapter shell(std::move(backend), [&]() { return alive; }, sink, [&]() { ++wake_count; });

    struct GuardedShell {
        PulseShellV1 table{};
        uint64_t canary = 0x54E11A8154E11A82ULL;
    } guarded;
    Check(shell.Query(PULSE_SHELL_VERSION_1, &guarded.table, sizeof(guarded)) == PULSE_OK &&
              guarded.canary == 0x54E11A8154E11A82ULL,
          "PulseShellV1 query preserves trailing canary");

    ShellClient client(guarded.table);

    // 1) Color tag catalog and per-path tag round-trip.
    std::vector<pulse::sdk::ColorTag> catalog;
    Check(client.GetTagCatalog(catalog) == PULSE_OK &&
              catalog.size() == 2 && catalog[0].id == "work" && catalog[1].favorite_slot == 2,
          "ShellClient::GetTagCatalog copies color tag definitions");

    Check(client.SetPathTag("relative.txt", "work", true) == PULSE_ERROR_ARGUMENT &&
              client.SetPathTag("C:\\Desktop\\spec.md", "", true) == PULSE_ERROR_ARGUMENT,
          "ShellClient::SetPathTag validates path and non-empty tag_id");

    std::vector<pulse::sdk::ColorTag> path_tags;
    Check(client.SetPathTag("C:\\Desktop\\spec.md", "work", true) == PULSE_OK &&
              client.GetPathTags("C:\\Desktop\\spec.md", path_tags) == PULSE_OK &&
              path_tags.size() == 1 && path_tags[0].id == "work" &&
              client.SetPathTag("C:\\Desktop\\spec.md", "work", false) == PULSE_OK &&
              client.GetPathTags("C:\\Desktop\\spec.md", path_tags) == PULSE_OK &&
              path_tags.empty(),
          "ShellClient::SetPathTag and GetPathTags add and remove path color tags");

    // 2) Streaming context menu query + invoke + RAII close.
    ShellClient::MenuSession menu_sess;
    std::vector<ShellMenuReplyData> menu_replies;
    Check(client.QueryContextMenu({"C:\\Desktop\\spec.md"}, PULSE_SHELL_MENU_QUERY_EXTENDED, menu_sess,
                                  [&](const ShellMenuReplyData& r) { menu_replies.push_back(r); }) == PULSE_OK &&
              menu_sess.Open(),
          "ShellClient::QueryContextMenu opens menu session");
    const uint64_t sid1 = menu_sess.Id();

    ShellMenuBackendReply part1;
    part1.status = PULSE_OK;
    part1.partial = true;
    part1.items.push_back(ShellMenuItemData{10u, PULSE_SHELL_MENU_ITEM_ENABLED, 0u, 'O', "open", "Open", ""});
    menu_sessions[sid1](std::move(part1));

    ShellMenuBackendReply part2;
    part2.status = PULSE_OK;
    part2.partial = false;
    part2.items = {
        ShellMenuItemData{10u, PULSE_SHELL_MENU_ITEM_ENABLED, 0u, 'O', "open", "Open", ""},
        ShellMenuItemData{11u, PULSE_SHELL_MENU_ITEM_ENABLED, 0u, 'P', "properties", "Properties", "Shell"},
    };
    menu_sessions[sid1](std::move(part2));

    shell.Poll();
    Check(menu_replies.size() == 2 &&
              menu_replies[0].partial && menu_replies[0].items.size() == 1 &&
              !menu_replies[1].partial && menu_replies[1].items.size() == 2 &&
              menu_sess.Open(),
          "ShellAdapter::Poll delivers partial and final context menu replies while keeping session open for invoke");

    Check(menu_sess.Invoke(11u, "properties") == PULSE_OK &&
              !menu_sess.Open() &&
              last_invoked_session == sid1 &&
              last_invoked_item == 11u &&
              last_invoked_verb == L"properties",
          "MenuSession::Invoke dispatches selected item_id + verb and retires session");

    {
        ShellClient::MenuSession auto_close_sess;
        Check(client.QueryContextMenu({"C:\\Desktop\\spec.md"}, 0, auto_close_sess,
                                      [](const ShellMenuReplyData&) {}) == PULSE_OK,
              "open second context menu session for RAII close verification");
    }
    Check(last_closed_session != 0, "destroying uninvoked MenuSession calls close_context_menu");

    // 3) Asynchronous Quick Look preview + thumb dimension clamping + cancellation.
    std::optional<ShellPreviewResult> preview_out;
    auto prev_req = client.ReadPreview(
        "C:\\Users\\Public\\Desktop\\readme.md",
        8u, // clamped to 32
        [&](const ShellPreviewResult& r) { preview_out = r; });
    Check(prev_req.has_value() && last_preview_thumb_px == 32u,
          "ShellAdapter::read_preview clamps max_thumb_px < 32 to 32");

    for (auto& t : preview_workers) {
        if (t.joinable()) t.join();
    }
    preview_workers.clear();

    shell.Poll();
    Check(preview_out.has_value() &&
              preview_out->status == PULSE_OK &&
              preview_out->display_type == "Markdown Document" &&
              preview_out->text_excerpt == "# Desktop Quick Look" &&
              preview_out->thumb_width == 2 &&
              preview_out->thumb_bgra.size() == 16 &&
              !prev_req->Pending(),
          "ShellAdapter::Poll delivers worker-populated Quick Look summary and BGRA thumbnail");

    // Cancel before Poll() suppresses callback.
    bool cancelled_preview_called = false;
    auto prev_cancel = client.ReadPreview(
        "C:\\Users\\Public\\Desktop\\readme.md",
        2048u, // clamped to 512
        [&](const ShellPreviewResult&) { cancelled_preview_called = true; });
    Check(prev_cancel.has_value() && last_preview_thumb_px == 512u,
          "ShellAdapter::read_preview clamps max_thumb_px > 512 to 512");
    const uint64_t cancelled_rid = prev_cancel->Id();
    prev_cancel->Cancel();
    for (auto& t : preview_workers) {
        if (t.joinable()) t.join();
    }
    preview_workers.clear();
    shell.Poll();
    Check(last_cancelled_preview == cancelled_rid && !cancelled_preview_called,
          "PreviewRequest::Cancel notifies backend and suppresses late worker preview completion");

    shell.Close();
    diag_adapter.Close();
}

} // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc == 2 && std::string_view(argv[1]) == "--workbench") {
        TestWorkbenchNotificationsAndNavigation();
        TestWorkbenchV2AppearanceSearchAndRectReveal();
        std::printf("Summary: %d passed, %d failed\n", g_passed, g_failed);
        return g_failed == 0 ? 0 : 1;
    }
    TestDiagnosticsAndModelErrors();
    TestDialogsV1VsV2AndNestedPickerFlow();
    TestPickersValidationCancelAndStop();
    TestWorkbenchNotificationsAndNavigation();
    TestWorkbenchV2AppearanceSearchAndRectReveal();
    TestTrayAdapterAndClient();
    TestSearchAdapterAndClient();
    TestShellAdapterAndClient();
    std::printf("Summary: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

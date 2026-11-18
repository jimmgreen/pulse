#include "../../app/extensions/native/ui_adapter.h"
#include "../../../sdk/native/include/pulse/ui_data.h"
#include "ui_model_fixture.h"
#include <commctrl.h>
#include <cstdio>
#include <functional>
#include <thread>
using namespace native_ui_fixture;
namespace {
int passes = 0, failures = 0;
void Check(bool result, const char *label) {
    std::printf("[%s] %s\n", result ? "PASS" : "FAIL", label);
    result ? ++passes : ++failures;
}
void Pump() {
    MSG message{};
    for (unsigned n = 0; n < 128 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
struct Fixture {
    PulseUiModelNode root = Node(PULSE_UI_MODEL_STACK, "root");
    std::array<PulseUiModelNode, 3> nodes{Node(PULSE_UI_MODEL_TEXT_INPUT, "input"),
        Node(PULSE_UI_MODEL_DATA_TABLE, "items"), Node(PULSE_UI_MODEL_CHECKBOX, "toggle")};
    std::array<PulseUiState, 7> states{{{S("filter"), S("")}, {S("sort"), S("nameAsc")},
        {S("selected"), S("")}, {S("status"), S("")}, {S("error"), S("")},
        {S("progress"), S("0")}, {S("checked"), S("false")}}};
    PulseUiModelColumn column{sizeof(PulseUiModelColumn), S("name"), S("Name"), {1, 240}};
    PulseUiModelTree tree{};
    Fixture() {
        root.children = {1, nodes.data(), 3};
        nodes[0].text = T("Filter"); nodes[0].state = T("filter");
        nodes[1].state = T("selected"); nodes[1].rows = {1, nullptr, 0};
        nodes[1].data_columns = {1, &column, 1}; nodes[1].min_height = {1, 200};
        nodes[2].text = T("Boolean role"); nodes[2].state = T("checked");
        tree = {sizeof(tree), &root, states.data(), static_cast<uint32_t>(states.size()), nullptr, 0, nullptr, 0};
    }
};
struct Provider {
    uint64_t request = 0, canceled = 0;
    unsigned calls = 0, cancels = 0;
    std::string filter, cursor;
    std::function<void()> action;
    static void PULSE_CALL Read(void *user, const PulseUiDataRequest *input) {
        auto &self = *static_cast<Provider *>(user);
        self.request = input->request; ++self.calls;
        self.filter.assign(input->filter.data, input->filter.length);
        self.cursor.assign(input->cursor.data, input->cursor.length);
        if (self.action) self.action();
    }
    static void PULSE_CALL Cancel(void *user, uint64_t request) {
        auto &self = *static_cast<Provider *>(user);
        self.canceled = request; ++self.cancels;
    }
};
struct Page {
    PulseStringView cell = S("First");
    PulseUiModelRow row{};
    PulseUiDataPage page{};
    Page() {
        row.size = sizeof(row); row.id = S("one"); row.text = T("First"); row.cells = {1, &cell, 1};
        page.size = sizeof(page); page.status = PULSE_UI_DATA_READY; page.progress = 100;
        page.revision = S("r1"); page.rows = &row; page.row_count = 1; page.next_cursor = S("second");
    }
};
std::string Get(const PulseUiModelV1 &ui, uint64_t panel, const char *key) {
    char buffer[1024]; uint32_t size = 0;
    if (ui.get_state(ui.context, panel, S(key), buffer, sizeof(buffer), &size) != PULSE_OK) return "<failed>";
    return {buffer, size};
}
}
int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    HWND owner = CreateWindowExW(0, L"STATIC", L"Native data fixture", WS_OVERLAPPEDWINDOW,
        0, 0, 900, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) return 2;
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    {
        pulse::extensions::native::UiAdapter adapter(owner, false, D2D1::ColorF(0x0078d4), 1, [] { return true; });
        PulseUiModelV1 ui{}; PulseUiModelV1 model{}; PulseUiDataV1 data{};
        Check(adapter.QueryModel(1, &ui, sizeof(ui)) == PULSE_OK &&
            adapter.QueryModel(1, &model, sizeof(model)) == PULSE_OK &&
            adapter.QueryData(1, &data, sizeof(data)) == PULSE_OK, "query separate native data interface");
        Check(adapter.QueryData(2, &data, sizeof(data)) == PULSE_ERROR_ABI &&
            adapter.QueryData(1, &data, sizeof(data)-1) == PULSE_ERROR_ABI, "data ABI version and table size rejected");
        Fixture fixture; uint64_t panel = 0, provider = 0;
        Check(model.create(model.context, S("Live data contract"), &fixture.tree, &panel) == PULSE_OK,
            "create actual empty native table and input");
        Provider author;
        Check(data.register_provider(data.context, Provider::Read, Provider::Cancel, &author, &provider) == PULSE_OK,
            "register developer provider without invoking callback");
        const PulseUiDataBinding binding{sizeof(binding), 128, S("filter"), S("sort"), S("status"), S("error"), S("progress")};
        auto bad = binding; bad.status_state = S("checked");
        Check(data.bind(data.context, panel, S("items"), provider, &bad) == PULSE_ERROR_ARGUMENT,
            "output role constrained by another real control is rejected transactionally");
        bad = binding; bad.error_state = S("filter");
        Check(data.bind(data.context, panel, S("items"), provider, &bad) == PULSE_ERROR_ARGUMENT,
            "conflicting input and output state roles rejected");
        Check(data.bind(data.context, panel, S("items"), provider, &binding) == PULSE_OK && !author.calls,
            "bind real empty table without spontaneous author execution");
        PulseStatus off_thread = PULSE_OK;
        std::thread wrong_thread([&] { off_thread = data.request(data.context, panel, S("items"), 0); });
        wrong_thread.join();
        Check(off_thread == PULSE_ERROR_STATE, "worker cannot call UI-thread data control operations");
        Check(data.request(data.context, panel+1, S("items"), 0) == PULSE_ERROR_STATE &&
            data.request(data.context, panel, S("items"), 9) == PULSE_ERROR_ARGUMENT,
            "wrong panel and invalid operation rejected");
        Check(data.request(data.context, panel, S("items"), PULSE_UI_DATA_REFRESH) == PULSE_OK && author.calls == 1,
            "refresh calls registered author on UI thread");
        Page first; PulseStatus published = PULSE_ERROR_STATE;
        std::thread worker([&] { published = data.publish(data.context, author.request, &first.page); }); worker.join();
        Check(published == PULSE_OK && adapter.DataNode("items")->rows.empty(),
            "worker publishes copied page without synchronous UI mutation");
        first.cell = S("Changed borrowed storage"); Pump();
        Check(adapter.DataNode("items")->rows.size() == 1 && adapter.DataNode("items")->rows[0].cells[0] == "First" &&
            Get(ui, panel, "status") == "ready", "shared renderer drains owned copy into actual table");
        PulseUiState selected{S("selected"), S("one")};
        Check(ui.set_states(ui.context, panel, &selected, 1) == PULSE_OK, "stable data row can be selected");
        const auto old = author.request;
        Check(data.request(data.context, panel, S("items"), PULSE_UI_DATA_NEXT_PAGE) == PULSE_OK && author.cursor == "second" &&
            author.canceled == old && data.publish(data.context, old, &first.page) == PULSE_ERROR_STATE,
            "next page passes cursor and rejects old live-stream submissions");
        Page second; second.row.id = S("two"); second.page.next_cursor = {};
        Check(data.publish(data.context, author.request, &second.page) == PULSE_OK, "second revision-compatible page queued"); Pump();
        Check(adapter.DataNode("items")->rows.size() == 2 && Get(ui, panel, "selected") == "one",
            "pagination appends stable rows and preserves selection");
        data.request(data.context, panel, S("items"), PULSE_UI_DATA_REFRESH);
        Page delta; delta.page.next_cursor = {}; delta.page.mode = PULSE_UI_DATA_DELTA;
        delta.page.base_revision = S("r1"); delta.page.revision = S("r2");
        data.publish(data.context, author.request, &first.page); Pump();
        const PulseStringView removed = S("one"); delta.row.id = S("new");
        delta.page.removed_ids = &removed; delta.page.removed_count = 1;
        Check(data.publish(data.context, author.request, &delta.page) == PULSE_OK, "typed stable-ID insert/remove delta accepted"); Pump();
        Check(adapter.DataNode("items")->rows.size() == 1 && adapter.DataNode("items")->rows[0].id == "new" &&
            Get(ui, panel, "selected").empty(), "delta changes actual table and clears removed selection");
        data.publish(data.context, author.request, &delta.page); Pump();
        Check(Get(ui, panel, "error") == "stale_revision" && adapter.DataNode("items")->rows[0].id == "new",
            "stale delta reports error without replacing display");
        Page invalid; invalid.row.cells.count = 0;
        Check(data.publish(data.context, author.request, &invalid.page) == PULSE_ERROR_ARGUMENT,
            "page validates actual table column shape");
        Page unknown_command; unknown_command.row.command = T("undeclared");
        Check(data.publish(data.context, author.request, &unknown_command.page) == PULSE_ERROR_ARGUMENT,
            "data row cannot invoke undeclared native command");
        auto previous = author.request; SetWindowTextW(adapter.NodeWindow("input"), L"latest"); Pump();
        Check(author.filter == "latest" && author.request != previous && author.canceled == previous,
            "real input event cancels generation and starts filtered author request");
        previous = author.request;
        Check(data.request(data.context, panel, S("items"), PULSE_UI_DATA_CANCEL) == PULSE_OK &&
            data.publish(data.context, previous, &first.page) == PULSE_ERROR_STATE,
            "cancel suppresses late worker results before cancellation returns");
        data.request(data.context, panel, S("items"), 0); previous = author.request;
        Check(data.unbind(data.context, panel, S("items")) == PULSE_OK && adapter.DataNode("items")->rows.empty() &&
            data.publish(data.context, previous, &first.page) == PULSE_ERROR_STATE, "unbind clears actual rows and pending callbacks");
        data.bind(data.context, panel, S("items"), provider, &binding); data.request(data.context, panel, S("items"), 0);
        previous = author.request;
        Check(model.update(model.context, panel, &fixture.tree) == PULSE_OK &&
            data.publish(data.context, previous, &first.page) == PULSE_ERROR_STATE &&
            data.request(data.context, panel, S("items"), 0) == PULSE_ERROR_STATE,
            "successful model update invalidates bindings and pending requests");
        data.bind(data.context, panel, S("items"), provider, &binding); data.request(data.context, panel, S("items"), 0);
        previous = author.request;
        Check(data.unregister_provider(data.context, provider) == PULSE_OK &&
            data.publish(data.context, previous, &first.page) == PULSE_ERROR_STATE,
            "unregister cancels bound streams before author frees provider state");
        data.register_provider(data.context, Provider::Read, Provider::Cancel, &author, &provider);
        data.bind(data.context, panel, S("items"), provider, &binding);
        bool inside = false, reentry_rejected = false;
        author.action = [&] {
            inside = adapter.InCallback();
            reentry_rejected = data.request(data.context, panel, S("items"), 0) == PULSE_ERROR_STATE;
            ui.close(ui.context, panel);
        };
        Check(data.request(data.context, panel, S("items"), 0) == PULSE_OK && inside && !adapter.InCallback() &&
            data.publish(data.context, author.request, &first.page) == PULSE_ERROR_STATE,
            "request callback can close panel through unload gate without late work");
        Check(reentry_rejected, "provider callbacks cannot recursively enter data control operations");
        Check(author.cancels == author.calls, "each started request receives exactly one cancellation at lifecycle end");
        adapter.Stop();
        Check(data.register_provider(data.context, Provider::Read, Provider::Cancel, &author, &provider) == PULSE_ERROR_STATE,
            "stopped instance rejects provider registration");
    }
    for (const auto type : {PULSE_UI_MODEL_VIRTUAL_LIST, PULSE_UI_MODEL_TREE}) {
        pulse::extensions::native::UiAdapter adapter(owner, false, D2D1::ColorF(0x0078d4), 1, [] { return true; });
        PulseUiModelV1 ui{}; PulseUiModelV1 model{}; PulseUiDataV1 data{};
        adapter.QueryModel(1, &ui, sizeof(ui)); adapter.QueryModel(1, &model, sizeof(model)); adapter.QueryData(1, &data, sizeof(data));
        Fixture fixture; fixture.nodes[1].type = type; fixture.nodes[1].data_columns = {};
        uint64_t panel = 0, provider = 0;
        Provider author;
        const PulseUiDataBinding binding{sizeof(binding), 128, S("filter"), S("sort"), S("status"), S("error"), S("progress")};
        Check(model.create(model.context, S("Shared data controls"), &fixture.tree, &panel) == PULSE_OK &&
            data.register_provider(data.context, Provider::Read, Provider::Cancel, &author, &provider) == PULSE_OK &&
            data.bind(data.context, panel, S("items"), provider, &binding) == PULSE_OK,
            type == PULSE_UI_MODEL_TREE ? "bind same provider protocol to real tree" : "bind same provider protocol to real virtualList");
        data.request(data.context, panel, S("items"), 0);
        Page page; page.row.cells = {}; page.page.next_cursor = {};
        PulseUiModelRow child{}; child.size = sizeof(child); child.id = S("child"); child.text = T("Child");
        if (type == PULSE_UI_MODEL_TREE) page.row.children = {1, &child, 1};
        Check(data.publish(data.context, author.request, &page.page) == PULSE_OK,
            "typed row page validates corresponding shared data-control shape"); Pump();
        const auto shown = adapter.DataNode("items");
        Check(shown && shown->rows.size() == 1 &&
            (type != PULSE_UI_MODEL_TREE || shown->rows[0].children.size() == 1),
            "shared control displays provider rows including tree descendants");
        ui.close(ui.context, panel);
        Check(data.publish(data.context, author.request, &page.page) == PULSE_ERROR_STATE,
            "each data-control type rejects result after panel closure");
    }
    DestroyWindow(owner); CoUninitialize();
    std::printf("%d PASS, %d FAIL\n", passes, failures);
    return failures ? 1 : 0;
}

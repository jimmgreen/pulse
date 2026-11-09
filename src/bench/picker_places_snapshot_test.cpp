#include "../common/picker_places_snapshot.h"
#include "../common/picker_places_saved.h"
#include <iostream>
#include <memory>

int main() {
    using namespace pulse::picker_places;
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!ok) ++failed;
    };
    auto valid = std::make_unique<detail::Payload>();
    valid->magic = detail::kMagic;
    valid->version = 1;
    valid->pid = GetCurrentProcessId();
    valid->timestamp = GetTickCount64();
    const std::wstring current = L"C:\\资料\\项目";
    valid->current_length = static_cast<DWORD>(current.size());
    std::memcpy(valid->current, current.data(), current.size() * sizeof(wchar_t));
    valid->count = 1;
    auto& row = valid->rows[0];
    row.path_length = static_cast<DWORD>(current.size());
    std::memcpy(row.path, current.data(), current.size() * sizeof(wchar_t));
    row.label_length = 2;
    row.label[0] = L'资'; row.label[1] = L'料'; row.flags = 3;
    check(detail::Valid(*valid), "absolute Unicode path and both flags");
    auto changed = std::make_unique<detail::Payload>();
    auto rejects = [&](const char* name, auto mutate) {
        *changed = *valid;
        mutate(*changed);
        check(!detail::Valid(*changed), name);
    };
    rejects("wrong magic", [](auto& p) { p.magic = 0; });
    rejects("unknown version", [](auto& p) { p.version = 2; });
    rejects("zero process", [](auto& p) { p.pid = 0; });
    rejects("future timestamp", [](auto& p) { p.timestamp = GetTickCount64() + 60000; });
    if (GetTickCount64() >= 10001)
        rejects("expired snapshot", [](auto& p) { p.timestamp = GetTickCount64() - 10001; });
    rejects("excess rows", [](auto& p) { p.count = 97; });
    rejects("excess current length", [](auto& p) { p.current_length = 1025; });
    rejects("empty row path", [](auto& p) { p.rows[0].path_length = 0; });
    rejects("excess path length", [](auto& p) { p.rows[0].path_length = 1025; });
    rejects("excess label length", [](auto& p) { p.rows[0].label_length = 129; });
    rejects("unknown flags", [](auto& p) { p.rows[0].flags = 4; });
    rejects("embedded current NUL", [](auto& p) { p.current[1] = 0; });
    rejects("embedded path NUL", [](auto& p) { p.rows[0].path[1] = 0; });
    rejects("embedded label NUL", [](auto& p) { p.rows[0].label[1] = 0; });
    *changed = *valid;
    changed->current_length = 1024;
    std::wmemset(changed->current, L'文', 1024);
    changed->count = 96;
    for (auto& r : changed->rows) {
        r.path_length = 1024; r.label_length = 128; r.flags = 0;
        std::wmemset(r.path, L'文', 1024);
        r.path[0] = L'C'; r.path[1] = L':'; r.path[2] = L'\\';
        std::wmemset(r.label, L'文', 128);
    }
    check(detail::Valid(*changed), "maximum rows and Unicode string lengths");
    *changed = *valid;
    changed->count = 0; changed->current_length = 0;
    check(detail::Valid(*changed), "empty snapshot");
    const auto isolated_name = L"Local\\Pulse.PickerPlaces.Test." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    detail::Writer writer(isolated_name);
    check(writer.blob != nullptr && writer.mutex != nullptr, "isolated mapping creation");
    if (writer.blob) {
        valid->timestamp = GetTickCount64();
        writer.blob->payload = *valid;
        InterlockedExchange(&writer.blob->sequence, 2);
        const auto roundtrip = detail::ReadMapping(isolated_name);
        check(roundtrip.current == current && roundtrip.folders.size() == 1 &&
              roundtrip.folders[0].path == current && roundtrip.folders[0].label == L"资料" &&
              roundtrip.folders[0].open && roundtrip.folders[0].pinned, "isolated mapping Unicode and flags roundtrip");
        InterlockedExchange(&writer.blob->sequence, 3);
        check(detail::ReadMapping(isolated_name).folders.empty(), "odd sequence returns without waiting");
        writer.blob->payload.rows[0].path_length = 1025;
        InterlockedExchange(&writer.blob->sequence, 4);
        check(detail::ReadMapping(isolated_name).folders.empty(), "mapping rejects invalid length");
        writer.blob->payload = *valid;
        writer.blob->payload.timestamp = GetTickCount64() >= 10001 ? GetTickCount64() - 10001 : GetTickCount64() + 60000;
        InterlockedExchange(&writer.blob->sequence, 6);
        check(detail::ReadMapping(isolated_name).folders.empty(), "mapping rejects stale or future timestamp");
    }
    check(ValidAddStarred({L"C:\\work", 2, {}}), "favorite folder request");
    check(ValidAddStarred({L"\\\\server\\share\\file.txt", 1, {}}), "UNC favorite request");
    check(ValidAddStarred({L"\\\\?\\C:\\work", 2, {}}) &&
        ValidAddStarred({L"\\\\?\\UNC\\server\\share\\work", 2, {}}), "extended drive and UNC favorite request");
    check(!ValidAddStarred({L"pulse:starred", 2, {}}), "reject virtual favorite");
    check(!ValidAddStarred({L"relative", 2, {}}), "reject relative favorite");
    check(!ValidAddStarred({L"C:\\file.txt", 1, L"group"}), "reject grouped file");
    check(!ValidAddStarred({L"C:\\work", 3, {}}), "reject unknown favorite kind");
    check(!ValidAddStarred({std::wstring(1024, L'x'), 2, {}}), "reject oversized favorite");
    AddStarredWire wire;
    wire.kind = 2;
    wcscpy_s(wire.path, L"C:\\work");
    wcscpy_s(wire.group, L"group");
    COPYDATASTRUCT data{detail::kAddStarredMessage, sizeof(wire), &wire};
    AddStarredRequest request;
    check(DecodeAddStarred(&data, request) && request.path == L"C:\\work" &&
        request.group == L"group" && request.kind == 2, "favorite request roundtrip");
    --data.cbData;
    check(!DecodeAddStarred(&data, request), "reject truncated favorite request");
    ++data.cbData;
    std::wmemset(wire.path, L'x', 1024);
    check(!DecodeAddStarred(&data, request), "reject unterminated favorite path");
    check(!RequestAddStarred({}, L"C:\\work", 2), "offline favorite request fails explicitly");
    wcscpy_s(wire.path, L"C:\\work");
    std::vector<AddStarredWire> batch{wire, wire};
    COPYDATASTRUCT batch_data{kAddStarredBatchMessage, static_cast<DWORD>(batch.size() * sizeof(wire)), batch.data()};
    std::vector<AddStarredRequest> requests;
    check(DecodeAddStarredBatch(&batch_data, requests) && requests.size() == 2,
        "batch favorite request roundtrip including duplicate paths");
    batch[1].kind = 3;
    requests.clear();
    check(!DecodeAddStarredBatch(&batch_data, requests) && requests.empty(), "invalid batch has no partial decode");
    batch_data.cbData = 65 * sizeof(wire);
    check(!DecodeAddStarredBatch(&batch_data, requests), "reject oversized favorite batch before reading");
    HWND endpoint = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    auto shared = std::make_unique<detail::SharedPayload>();
    shared->magic = detail::kMagic; shared->version = 2; shared->pid = GetCurrentProcessId();
    shared->timestamp = GetTickCount64(); shared->endpoint = reinterpret_cast<ULONG_PTR>(endpoint);
    check(endpoint && detail::ValidShared(*shared), "v2 validates live endpoint");
    Snapshot connected;
    connected.shared_available = true; connected.endpoint = endpoint; connected.endpoint_pid = GetCurrentProcessId();
    check(LiveEndpoint(connected), "request endpoint owner matches snapshot");
    ++connected.endpoint_pid;
    check(!LiveEndpoint(connected) && !RequestAddStarred(connected, L"C:\\work", 2), "request rejects changed endpoint owner");
    shared->count = 385;
    check(!detail::ValidShared(*shared), "v2 rejects excess rows");
    shared->count = 0;
    if (endpoint) DestroyWindow(endpoint);
    check(!detail::ValidShared(*shared), "v2 rejects dead endpoint");
    const auto saved = ParseSaved(LR"({"workspaces":[{"root":"C:\\项目","name":"项目","color_rgb":1122867}],"starred_groups":[{"id":"g","name":"组"}],"starred_items":[{"path":"C:\\folder","kind":"folder","group":"g"},{"path":"C:\\file.txt","kind":"file","group":"g"},{"path":"pulse:starred","kind":"folder"}]})");
    check(saved.workspaces.size() == 1 && saved.workspaces[0].color_rgb == 1122867 &&
        saved.starred_groups.size() == 1 && saved.starred_items.size() == 2 &&
        saved.starred_items[0].group == L"g" && saved.starred_items[1].group.empty() &&
        !saved.shared_available, "saved catalog preserves workspace/group and rejects virtual paths");
    check(ParseSaved(LR"({"starred":["C:\\old"]})").starred_items.size() == 1, "saved catalog legacy favorites");
    check(ParseSaved(L"{broken").workspaces.empty(), "saved catalog rejects malformed JSON");
    using Action = ManagePlacesAction;
    check(ValidManagePlaces({Action::Remove, {L"C:\\one", L"\\\\server\\share\\two"}, {}, {}}), "management accepts bounded removal paths");
    check(ValidManagePlaces({Action::SetGroup, {L"C:\\one"}, {}, {}}), "management accepts leave group");
    check(!ValidManagePlaces({Action::Remove, {L"pulse:starred"}, {}, {}}), "management rejects virtual removal");
    check(!ValidManagePlaces({Action::CreateGroup, {L"C:\\one"}, {}, L"  "}), "management rejects whitespace group name");
    check(!ValidManagePlaces({Action::SetGroup, std::vector<std::wstring>(65, L"C:\\one"), {}, {}}), "management rejects oversized batch");
    check(ValidManagePlaces({Action::PinWorkspace, {L"C:\\root"}, {}, {}}) &&
        ValidManagePlaces({Action::RenameWorkspace, {L"C:\\root"}, {}, L"Workspace"}), "workspace pin and rename protocol");
    check(!ValidManagePlaces({Action::MoveWorkspaceUp, {L"C:\\root"}, L"unexpected", {}}), "workspace rejects unused field");
    auto management = std::make_unique<ManagePlacesWire>();
    management->action = static_cast<DWORD>(Action::RenameGroup);
    wcscpy_s(management->group, L"group-id"); wcscpy_s(management->name, L"项目");
    COPYDATASTRUCT management_data{kManagePlacesMessage, sizeof(ManagePlacesWire), management.get()};
    ManagePlacesRequest management_request;
    check(DecodeManagePlaces(&management_data, management_request) && management_request.group == L"group-id" &&
        management_request.name == L"项目", "management wire Unicode roundtrip");
    --management_data.cbData;
    check(!DecodeManagePlaces(&management_data, management_request), "management rejects truncated wire");
    ++management_data.cbData; management->action = 999;
    check(!DecodeManagePlaces(&management_data, management_request), "management rejects unknown action");
    management->action = static_cast<DWORD>(Action::RenameGroup); std::wmemset(management->name, L'x', 128);
    check(!DecodeManagePlaces(&management_data, management_request), "management rejects unterminated name");
    check(!RequestRemoveStarred({}, {L"C:\\one"}) && !RequestPinWorkspace({}, L"C:\\root"), "management refuses offline endpoint");
    return failed ? 1 : 0;
}


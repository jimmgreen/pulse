#include "../ipc/picker_remote_protocol.h"
#include <iostream>
using namespace pulse::ipc::picker::remote;
int failures = 0, checks = 0;
void Check(bool ok) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cout << "[FAIL] " << checks << '\n';
    }
}
void Round(Message m) {
    std::vector<uint8_t> wire;
    Check(Encode(m, wire));
    Message copy;
    Check(Decode(wire, copy));
    std::vector<uint8_t> again;
    Check(Encode(copy, again) && again == wire);
    for (size_t n = 0; n < wire.size(); ++n) {
        auto saved = copy;
        Check(!Decode(std::span(wire).first(n), copy) && copy == saved);
    }
    wire.push_back(0);
    Check(!Decode(wire, copy));
}
int main() {
    Message m;
    m.request_id[0] = 7;
    m.sequence = 0x123456789abcdef0ull;
    m.config.owner = 0xfedcba9876543210ull;
    m.config.initial_folder = L"C:\\test";
    m.config.filename = L"a.txt";
    m.config.title = L"Title";
    m.config.filters = {{L"Text", L"*.txt"}};
    m.config.default_extension = L"txt";
    m.config.label_flags = 7;
    m.config.ok_button_label = L"Choose";
    m.config.file_name_label = L"Name";
    Control c;
    c.kind = 3;
    c.id = 5;
    c.has_parent = 1;
    c.parent_id = 2;
    c.state = 3;
    c.label = L"Combo";
    c.items = {{7, 3, L"Item"}};
    c.has_selected = 1;
    c.selected_item_id = 7;
    c.text = L"Text";
    m.config.controls = {c};
    m.config.places = {{1, L"Place", L"C:\\place"}};
    m.config.properties = {{3, 2, L"Property", {L"one", L"two"}}};
    Round(m);
    auto separator = m;
    separator.config.controls = {{}};
    separator.config.controls[0].kind = PULSE_PICKER_UI_V2_CONTROL_SEPARATOR;
    separator.config.controls[0].id = 2028;
    separator.config.controls[0].state = 3;
    Round(separator);
    OwnedConfig separator_owned(separator.config);
    Config separator_copy;
    Check(FromAbi(separator_owned.Get(), separator_copy) && separator_copy == separator.config);
    separator.kind = Kind::UpdateControl;
    separator.control = separator.config.controls[0];
    Round(separator);
    separator.control.kind = PULSE_PICKER_UI_V2_CONTROL_SEPARATOR + 1;
    std::vector<uint8_t> invalid_separator;
    Check(!Encode(separator, invalid_separator));
    m.kind = Kind::Show;
    Round(m);
    OwnedConfig owned(m.config);
    Config converted;
    Check(FromAbi(owned.Get(), converted) && converted == m.config);
    auto bad = owned.Get();
    bad.reserved[0] = 1;
    Check(!FromAbi(bad, converted) && converted == m.config);
    m.kind = Kind::Event;
    m.event = {14,
               0x1122334455667788ull,
               0x8877665544332211ull,
               0x123456789abcdef0ull,
               L"C:\\next",
               4,
               9,
               L"Edit",
               {L"Value"},
               0x80004005,
               L""};
    Round(m);
    OwnedEvent event(m.event);
    Event converted_event;
    Check(FromAbi(event.Get(), converted_event) && converted_event == m.event);
    m.event.target = L"C:\\target";
    m.kind = Kind::Overwrite;
    Round(m);
    m.kind = Kind::ShareViolation;
    Round(m);
    OwnedOverwriteEvent overwrite(m.event);
    Check(FromAbi(overwrite.Get(), converted_event) && converted_event.target == m.event.target);
    m.show_id = 0x123456789abcdef0ull;
    m.expected_generation = 0xfedcba9876543210ull;
    m.control = c;
    m.folder = L"C:\\navigate";
    m.response = 2;
    for (auto kind :
         {Kind::EventReply, Kind::Query, Kind::GetWindow, Kind::Close, Kind::UpdateControl, Kind::Navigate}) {
        m.kind = kind;
        Round(m);
    }
    m.kind = Kind::Reply;
    m.snapshot = {3,
                  m.show_id,
                  m.expected_generation,
                  m.show_id,
                  0,
                  L"C:\\folder",
                  L"a.txt",
                  L"C:\\a.txt",
                  L"C:\\a.txt",
                  {L"C:\\a.txt"},
                  {L"C:\\a.txt"}};
    Round(m);
    wchar_t folder[100]{L'!'}, filename[100]{L'!'}, selection[100]{L'!'}, candidate[100]{L'!'}, sp[100]{L'!'},
        cp[100]{L'!'};
    PulsePickerUiV2Snapshot abi{};
    abi.size = sizeof(abi);
    abi.folder = {folder, 1, 0};
    abi.filename = {filename, 100, 0};
    abi.selection = {selection, 100, 0};
    abi.candidate = {candidate, 100, 0};
    abi.selection_paths = {sp, 100, 0};
    abi.candidate_paths = {cp, 100, 0};
    Check(CopyToAbi(m.snapshot, abi) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && folder[0] == L'!' &&
          filename[0] == L'!');
    abi.folder.capacity = 100;
    Check(CopyToAbi(m.snapshot, abi) == S_OK);
    Snapshot snapshot;
    Check(FromAbi(abi, snapshot) && snapshot == m.snapshot);
    m.kind = Kind::Terminal;
    m.result = {1, 1, 0, 0, 0, m.show_id, m.expected_generation, L"C:\\a.txt", {L"C:\\a.txt"}};
    Round(m);
    PulsePickerUiV2Result result{};
    result.size = sizeof(result);
    result.path = {selection, 100, 0};
    result.paths = {sp, 100, 0};
    Check(CopyToAbi(m.result, result) == S_OK);
    Result converted_result;
    Check(FromAbi(result, converted_result) && converted_result == m.result);
    auto overlapping = abi;
    overlapping.filename.data = overlapping.folder.data;
    Check(CopyToAbi(m.snapshot, overlapping) == E_INVALIDARG);
    auto overlapping_result = result;
    overlapping_result.paths.data = overlapping_result.path.data;
    Check(CopyToAbi(m.result, overlapping_result) == E_INVALIDARG);
    m.kind = Kind::Hello;
    m.version = 2;
    Round(m);
    m.kind = Kind::Show;
    m.config.filename.assign(1, static_cast<wchar_t>(0xd800));
    std::vector<uint8_t> wire{42};
    Check(!Encode(m, wire) && wire == std::vector<uint8_t>{42});
    m.config.filename.assign(32768, L'x');
    Check(!Encode(m, wire));
    m.config.filename = L"x";
    m.config.controls.resize(17);
    Check(!Encode(m, wire));
    m.config.controls.clear();
    m.config.native_options = 1;
    Check(!Encode(m, wire));
    m.config.native_options = 0;
    m.sequence = 0;
    Check(!Encode(m, wire));
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}

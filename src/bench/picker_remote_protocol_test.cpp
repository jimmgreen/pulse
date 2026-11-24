#include "../ipc/picker_remote_protocol.h"
#include "../picker_inprocess/picker_dynamic_text.h"
#include <algorithm>
#include <iostream>
#include <string_view>
#include <shobjidl.h>
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
int NoOverwriteTest() {
    const auto named = [](bool ok, const char* label) {
        Check(ok); std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
    };
    Message message;
    message.request_id[0] = 7; message.sequence = 1;
    message.config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
    message.config.initial_folder = L"C:\\owned-fixture";
    message.config.filename = L"owned.txt";
    message.config.save_options = PULSE_PICKER_UI_V2_SAVE_NATIVE_DEFAULTS | PULSE_PICKER_UI_V2_SAVE_NO_OVERWRITE_PROMPT;
    for (const auto kind : {Kind::ValidateConfig, Kind::Show}) {
        message.kind = kind;
        std::vector<uint8_t> encoded;
        const bool written = Encode(message, encoded);
        named(written, "no-prompt Save config encodes for ValidateConfig and Show");
        Message decoded;
        named(written && Decode(encoded, decoded) && decoded == message,
            "no-prompt bit roundtrips without inventing overwrite prompt");
    }
    OwnedConfig owned(message.config); Config converted;
    named(FromAbi(owned.Get(), converted) && converted == message.config, "no-prompt ABI config reaches the remote codec unchanged");
    named(SaveOptionsSupportedByPeer(message.config.save_options, PULSE_PICKER_UI_V2_FEATURE_SAVE_NO_OVERWRITE),
        "new peer explicitly certifies no-prompt Save");
    named(!SaveOptionsSupportedByPeer(message.config.save_options, 0), "old peer rejects no-prompt before any Validate/Show frame");
    named(!SaveOptionsSupportedByPeer(message.config.save_options, 0x80000000u), "unknown peer feature cannot borrow no-prompt capability");
    auto legacy = message;
    legacy.config.save_options = PULSE_PICKER_UI_V2_SAVE_NATIVE_DEFAULTS | PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
    std::vector<uint8_t> bytes; Message copy;
    named(SaveOptionsSupportedByPeer(legacy.config.save_options, 0) && Encode(legacy, bytes) && Decode(bytes, copy) && copy == legacy,
        "legacy overwrite-prompt Save remains wire-compatible with old peer");
    auto unknown = message; unknown.config.save_options |= 0x100u;
    named(!Encode(unknown, bytes) && !SaveOptionsSupportedByPeer(unknown.config.save_options, 0xffffffffu), "unknown Save option is rejected before display");
    auto stray_readonly = message; stray_readonly.config.save_options |= PULSE_PICKER_UI_V2_SAVE_NO_READONLY_RETURN;
    named(!Encode(stray_readonly, bytes) && !SaveOptionsSupportedByPeer(stray_readonly.config.save_options, 0xffffffffu),
        "no-readonly-return without skip-access-validation is rejected");
    auto caller_readonly = message;
    caller_readonly.config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT | PULSE_PICKER_UI_V2_SAVE_SKIP_ACCESS_VALIDATION | PULSE_PICKER_UI_V2_SAVE_NO_READONLY_RETURN;
    named(Encode(caller_readonly, bytes) && Decode(bytes, copy) && copy == caller_readonly &&
        SaveOptionsSupportedByPeer(caller_readonly.config.save_options, PULSE_PICKER_UI_V2_FEATURE_SAVE_SKIP_ACCESS) &&
        !SaveOptionsSupportedByPeer(caller_readonly.config.save_options, 0),
        "caller-validated Save no-readonly-return roundtrips with explicit skip capability");
    auto contradictory = message; contradictory.config.save_options |= PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
    named(!Encode(contradictory, bytes) && !SaveOptionsSupportedByPeer(contradictory.config.save_options, 0xffffffffu),
        "contradictory prompt and no-prompt options are rejected before display");
    std::cout << "REMOTE_NO_OVERWRITE checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
void FolderPolicyTest() {
    for (bool cancel : {false, true}) for (uint32_t policy : {0u, 1u, 2u, 4u, 6u, 7u}) {
        PulsePickerUiV2FolderPolicyConfig input{};
        auto& a = input.base.base; a.size = sizeof(input); a.version = 2; a.access_policy = 1;
        input.folder_policy = policy;
        if (cancel) { a.label_flags = PULSE_PICKER_UI_V2_LABEL_CANCEL; input.base.cancel_button_label = {L"Stop", 4}; }
        Config c; Check(FromAbi(a, c));
        Check(c.has_folder_policy && c.folder_policy == policy);
        OwnedConfig owned(c); Config again;
        Check(owned.Get().size == sizeof(input) && FromAbi(owned.Get(), again) && again == c);
        Message m; m.request_id[0] = 1; m.sequence = 1; m.config = c; Round(m);
        Check(!ConfigSupportedByPeer(c, 0));
        Check(ConfigSupportedByPeer(c, PULSE_PICKER_UI_V2_FEATURE_FOLDER_PROVENANCE |
            (cancel ? PULSE_PICKER_UI_V2_FEATURE_CANCEL_LABEL : 0u)));
        input.folder_policy = 8; Check(!FromAbi(a, again)); input.folder_policy = policy;
        input.reserved = 1; Check(!FromAbi(a, again)); input.reserved = 0;
        a.label_flags |= 0x80000000u; Check(!FromAbi(a, again)); a.label_flags &= ~0x80000000u;
        if (!cancel) { input.base.cancel_button_label = {L"", 0}; Check(!FromAbi(a, again)); }
    }
    Config old; OwnedConfig legacy(old); Config copy;
    Check(legacy.Get().size == sizeof(PulsePickerUiV2Config) && FromAbi(legacy.Get(), copy) && copy == old);
    Check(ConfigSupportedByPeer(old, 0));
    old.label_flags = PULSE_PICKER_UI_V2_LABEL_CANCEL;
    Check(!ConfigSupportedByPeer(old, 0));
    Check(ConfigSupportedByPeer(old, PULSE_PICKER_UI_V2_FEATURE_CANCEL_LABEL));
    old.label_flags = 0;
    old.folder_policy = 1; Message invalid; invalid.request_id[0] = 1; invalid.sequence = 1; invalid.config = old;
    std::vector<uint8_t> bytes; Check(!Encode(invalid, bytes));
}
void LargeControlItemsTest() {
    const auto named = [](bool ok, const char* name) {
        Check(ok); std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    };
    const auto round = [](const Message& m) {
        Message copy; std::vector<uint8_t> bytes;
        return Encode(m, bytes) && Decode(bytes, copy) && copy == m;
    };
    const auto set_frame_size = [](std::vector<uint8_t>& frame) {
        for (int i = 0; i < 4; ++i) frame[8 + i] = static_cast<uint8_t>(frame.size() >> (8 * i));
    };
    Message m; m.sequence = 1; m.request_id[0] = 5;
    Control c; c.id = 8; c.kind = PULSE_PICKER_UI_V2_CONTROL_COMBO; c.state = 3;
    named(PeerControlItemLimit(0) == 32 && PeerControlItemLimit(32) == 32 &&
        PeerControlItemLimit(1) == 0 && PeerControlItemLimit(9999) == 128, "bound legacy, invalid and newer Hello limits");
    Message hello_reply = m; hello_reply.kind = Kind::Reply; hello_reply.version = 2;
    named(round(hello_reply), "legacy Hello reply decodes with no limit word set");
    hello_reply.response = 128;
    named(round(hello_reply), "128-item Hello reply uses the existing reply frame");
    for (const uint32_t count : {32u, 33u, 128u}) {
        while (c.items.size() < count) {
            const auto ordinal = static_cast<uint32_t>(c.items.size());
            c.items.push_back({10000u - 3u * ordinal, ordinal % 4, L"Item " + std::to_wstring(ordinal)});
        }
        c.has_selected = 1; c.selected_item_id = c.items.back().id;
        m.kind = Kind::ValidateConfig; m.config.controls = {c};
        OwnedConfig abi(m.config); Config copy;
        named(FromAbi(abi.Get(), copy) && copy == m.config && round(m), "Validate preserves count/order/IDs/labels/state/selection");
        named(ConfigSupportedByPeer(m.config, 0, PeerControlItemLimit(0)) == (count <= 32) &&
            ConfigSupportedByPeer(m.config, 0, PeerControlItemLimit(128)), "old/new host negotiation before display");
        m.kind = Kind::Show; named(round(m), "Show preserves full control");
        Message live; live.kind = Kind::UpdateControl; live.show_id = 22; live.sequence = 4; live.request_id[0] = 6;
        live.control = c; OwnedControl control(c); Control converted;
        named(FromAbi(control.Get(), converted) && converted == c && round(live), "live ABI and UpdateControl preserve tail item");
    }
    Message live; live.kind = Kind::UpdateControl; live.show_id = 22; live.sequence = 4; live.request_id[0] = 6; live.control = c;
    std::vector<uint8_t> bytes; Message copy;
    if (Encode(live, bytes)) {
        // Header, show id, five fixed control fields, then an empty label length.
        const size_t count_at = 44 + 8 + 5 * 4 + 4;
        const size_t first_id = count_at + 4;
        auto bad_wire = bytes;
        bad_wire[count_at] = 129;
        // Add a complete and otherwise valid extra first item (unique id, empty label).
        const uint8_t extra[]{1,0,0,0, 3,0,0,0, 0,0,0,0};
        bad_wire.insert(bad_wire.begin() + first_id, std::begin(extra), std::end(extra));
        set_frame_size(bad_wire);
        named(!Decode(bad_wire, copy) && copy == Message{}, "well-framed 129th item rejected without altering output");
        const auto second_id = first_id + 12 + 2 * c.items[0].label.size();
        std::copy_n(bytes.begin() + first_id, 4, bytes.begin() + second_id);
        named(!Decode(bytes, copy) && copy == Message{}, "duplicate ID rejected on decode");
    } else named(false, "encode 128 for malformed frame cases");
    live.control.items.push_back({123, 3, L"extra"});
    OwnedControl excessive(live.control); Control untouched = c;
    named(!Encode(live, bytes) && !FromAbi(excessive.Get(), untouched) && untouched == c, "ABI and encode reject 129 without truncation");
    live.control = c; live.control.selected_item_id = 234;
    named(!Encode(live, bytes), "missing selected ID rejected");
    live.control = c; live.control.items.back().label.assign(257, L'x');
    named(!Encode(live, bytes), "257-unit tail item label rejected");

    for (auto& item : c.items) item.label.assign(256, L'z');
    m.config.controls = {c, c}; m.config.controls.back().id++;
    named(round(m), "two large controls exactly fill aggregate UTF-16 budget");
    if (Encode(m, bytes)) {
        // First two config strings are empty; add one UTF-16 unit to the title.
        constexpr size_t title_at = 44 + 5 * 4 + 8 + 2 * 4;
        bytes[title_at] = 1; bytes.insert(bytes.begin() + title_at + 4, {'x', 0});
        set_frame_size(bytes);
        named(!Decode(bytes, copy) && copy == Message{}, "well-framed aggregate of 65537 UTF-16 units rejected");
    } else named(false, "encode exact-limit config for over-budget frame");
    OwnedConfig exact(m.config); Config dest;
    named(FromAbi(exact.Get(), dest) && dest == m.config, "exact config budget accepted from ABI");
    m.config.title = L"x";
    OwnedConfig excess(m.config);
    named(!FromAbi(excess.Get(), dest) && dest.title.empty() && !Encode(m, bytes),
        "one extra UTF-16 unit across controls rejected atomically");
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--large-control-items") {
        LargeControlItemsTest(); std::cout << "Checks: " << checks << ", failures: " << failures << '\n'; return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--navigation-root") {
        Message message; message.kind = Kind::SetNavigationRoot; message.sequence = 1; message.request_id[0] = 9;
        for (const auto* path : {L"C:\\owned-root", L"\\\\server\\share\\\u76ee\u5f55"}) { message.folder = path; Round(message); }
        std::vector<uint8_t> bytes; message.folder.clear(); Check(!Encode(message, bytes));
        message.folder.assign(PULSE_PICKER_UI_V2_MAX_UNITS + 1, L'x'); Check(!Encode(message, bytes));
        std::cout << "Checks: " << checks << ", failures: " << failures << '\n'; return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--dynamic-text") {
        Message message; message.kind = Kind::UpdateText; message.sequence = 1; message.request_id[0] = 1; message.show_id = 77;
        for (uint32_t kind = PULSE_PICKER_UI_V2_TEXT_TITLE; kind <= PULSE_PICKER_UI_V2_TEXT_DEFAULT_EXTENSION; ++kind) {
            message.text_kind = kind;
            for (const auto* text : {L"", L"different \u540d\u79f0"}) { message.text = text; Round(message); }
        }
        std::vector<uint8_t> bytes;
        message.text_kind = 0; Check(!Encode(message, bytes));
        message.text_kind = PULSE_PICKER_UI_V2_TEXT_DEFAULT_EXTENSION + 1; Check(!Encode(message, bytes));
        for (const bool save : {false, true}) {
            for (const auto* invalid : {L"../outside", L"a\\b", L"a:b", L"bad\ntext", L"trailing."}) {
                std::wstring value = invalid;
                Check(!pulse::picker_inprocess::NormalizeDynamicText(PULSE_PICKER_UI_V2_TEXT_DEFAULT_EXTENSION, value, save, false));
            }
            std::wstring long_extension(256, L'x');
            Check(!pulse::picker_inprocess::NormalizeDynamicText(PULSE_PICKER_UI_V2_TEXT_DEFAULT_EXTENSION, long_extension, save, false));
        }
        for (const auto* invalid : {L"*", L"?", L"*.txt"}) {
            std::wstring value = invalid;
            Check(!pulse::picker_inprocess::NormalizeDynamicText(PULSE_PICKER_UI_V2_TEXT_DEFAULT_EXTENSION, value, true, false));
        }
        std::wstring native_extension = L"*.txt";
        Check(pulse::picker_inprocess::NormalizeDynamicText(PULSE_PICKER_UI_V2_TEXT_DEFAULT_EXTENSION, native_extension, true, true) && native_extension == L"txt");
        std::cout << "Checks: " << checks << ", failures: " << failures << '\n'; return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--folder-policy") { FolderPolicyTest(); std::cout << "Checks: " << checks << ", failures: " << failures << '\n'; return failures ? 1 : 0; }
    if (argc == 2 && std::string_view(argv[1]) == "--save-no-overwrite") return NoOverwriteTest();
    if (argc != 1) return 2;
    LargeControlItemsTest();
    FolderPolicyTest();
    Message m;
    m.request_id[0] = 7;
    m.sequence = 0x123456789abcdef0ull;
    m.config.required_capabilities = 0x3fu; m.config.appearance = 1;
    m.config.open_options = 1; m.config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
    m.config.native_options = FOS_PATHMUSTEXIST;
    m.config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
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
    c.text = L"Text"; c.checked = 1;
    m.config.controls = {c};
    m.config.places = {{1, L"Place", L"C:\\place"}};
    m.config.properties = {{3, 2, L"Property", {L"one", L"two"}}};
    Round(m);
    auto cancel = m;
    for (const auto* label : {L"Leave", L""}) {
        cancel.config.label_flags |= PULSE_PICKER_UI_V2_LABEL_CANCEL;
        cancel.config.cancel_button_label = label;
        Round(cancel);
        OwnedConfig extended(cancel.config);
        Config copy;
        Check(extended.Get().size == sizeof(PulsePickerUiV2CancelLabelConfig));
        Check(FromAbi(extended.Get(), copy) && copy == cancel.config);
        auto legacy = extended.Get(); legacy.size = sizeof(legacy);
        Check(!FromAbi(legacy, copy));
    }
    cancel.config.label_flags &= ~PULSE_PICKER_UI_V2_LABEL_CANCEL;
    cancel.config.cancel_button_label = L"unflagged";
    std::vector<uint8_t> invalid_cancel;
    Check(!Encode(cancel, invalid_cancel));
    cancel.config.label_flags |= PULSE_PICKER_UI_V2_LABEL_CANCEL;
    cancel.config.cancel_button_label = L"bad\nlabel";
    Check(!Encode(cancel, invalid_cancel));
    OwnedConfig legacy_config(m.config);
    Check(legacy_config.Get().size == sizeof(PulsePickerUiV2Config));
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
    auto text_control = m;
    text_control.config.controls = {{}};
    text_control.config.controls[0].kind = PULSE_PICKER_UI_V2_CONTROL_TEXT;
    text_control.config.controls[0].id = 2010;
    text_control.config.controls[0].state = 3;
    Round(text_control);
    OwnedConfig text_owned(text_control.config);
    Config text_copy;
    Check(FromAbi(text_owned.Get(), text_copy) && text_copy == text_control.config);
    separator.control.kind = PULSE_PICKER_UI_V2_CONTROL_TEXT + 1;
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
    m.version = 1;
    m.kind = Kind::Show;
    m.config.filename.assign(1, static_cast<wchar_t>(0xd800));
    std::vector<uint8_t> wire{42};
    Check(!Encode(m, wire) && wire == std::vector<uint8_t>{42});
    m.config.filename.assign(32768, L'x');
    Check(!Encode(m, wire));
    m.config.filename = L"x";
    m.config.controls.assign(32, {});
    for (size_t i = 0; i < m.config.controls.size(); ++i) {
        m.config.controls[i].kind = PULSE_PICKER_UI_V2_CONTROL_BUTTON;
        m.config.controls[i].id = static_cast<uint32_t>(100 + i);
        m.config.controls[i].state = 3;
        m.config.controls[i].label = L"Control";
    }
    Round(m);
    m.config.controls.resize(PULSE_PICKER_UI_V2_MAX_CONTROLS + 1);
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

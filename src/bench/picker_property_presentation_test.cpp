#include "../picker_inprocess/picker_property_panel.h"
#include "../ipc/picker_remote_protocol.h"
#include <cstdio>

namespace pulse::ui {
HRESULT UpdatePickerCustomControl(HWND, const PickerCustomControl&) { return E_UNEXPECTED; }
}
int main() {
    using namespace pulse::ipc::picker::remote;
    int checks = 0, failures = 0;
    const auto check = [&](bool okay) { ++checks; if (!okay) { ++failures; std::printf("[FAIL] %d\n", checks); } };
    Message message; message.kind = Kind::ValidateConfig; message.sequence = 1; message.request_id[0] = 1;
    auto& config = message.config;
    config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
    config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
    config.initial_folder = L"C:\\owned"; config.filename = L"owned.txt";
    config.properties = {{0, PULSE_PICKER_UI_V2_PROPERTY_GROUP, L"Camera", {}},
        {1, PULSE_PICKER_UI_V2_PROPERTY_READONLY_SCALAR, L"Date", {L"2026-11-12T00:00:00.0000001Z"}},
        {2, PULSE_PICKER_UI_V2_PROPERTY_READONLY_LIST, L"Owners", {L"One", L"Two"}}};
    check(!ConfigSupportedByPeer(config, 0));
    check(ConfigSupportedByPeer(config, PULSE_PICKER_UI_V2_FEATURE_PROPERTY_PRESENTATION));
    std::vector<uint8_t> bytes; Message decoded;
    check(Encode(message, bytes)); check(Decode(bytes, decoded) && decoded.config == config);
    OwnedConfig abi(config); Config copied;
    check(FromAbi(abi.Get(), copied) && copied == config);
    auto malformed = message; malformed.config.properties[0].values = {L"not a group value"}; check(!Encode(malformed, bytes));
    malformed = message; malformed.config.properties[1].values.clear(); check(!Encode(malformed, bytes));
    malformed = message; malformed.config.properties[2].kind = 6; check(!Encode(malformed, bytes));
    pulse::picker_inprocess::PropertyPanel panel;
    for (const auto& property : config.properties) panel.fields.push_back({property.index, property.kind, property.label, property.values});
    std::vector<pulse::ui::PickerCustomControl> controls; panel.Build(controls);
    check(controls.size() == 6);
    check(pulse::ui::PickerPropertiesNeedPanel(controls));
    auto hidden = controls; for (auto& control : hidden) control.state = 0;
    check(!pulse::ui::PickerPropertiesNeedPanel(hidden));
    auto editable = controls; for (auto& control : editable) control.property_layout = 4;
    check(!pulse::ui::PickerPropertiesNeedPanel(editable));
    pulse::picker_inprocess::PropertyPanel empty_list;
    empty_list.fields.push_back({3, PULSE_PICKER_UI_V2_PROPERTY_READONLY_LIST, L"Empty owners", {}});
    std::vector<pulse::ui::PickerCustomControl> empty_controls; empty_list.Build(empty_controls);
    check(empty_controls.size() == 1 && pulse::ui::PickerPropertiesNeedPanel(empty_controls));
    for (const auto& control : controls) {
        check(control.kind == PULSE_PICKER_UI_V2_CONTROL_GROUP);
        pulse::ui::PickerDialogSnapshot snapshot; snapshot.control_id = control.id;
        PulsePickerUiV2Event event{}; bool owned = true;
        check(panel.Translate(nullptr, pulse::ui::PickerDialogEvent::EditChanged, snapshot, event, owned) == S_OK && !owned);
    }
    std::printf("Checks: %d, failures: %d\n", checks, failures);
    return failures ? 1 : 0;
}

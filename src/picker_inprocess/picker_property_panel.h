#pragma once
#include "picker_ui_api_v2.h"
#include "../ui/folder_picker_dialog.h"
#include <algorithm>
#include <vector>
namespace pulse::picker_inprocess {
struct PropertyField {
    uint32_t index = 0, kind = 0;
    std::wstring label;
    std::vector<std::wstring> values;
};
// Properties share the existing painted-edit panel, focus order, and scrolling.
// Slots exist before Show; vector mutation only changes their visibility/text.
class PropertyPanel {
    struct Binding { uint32_t id; size_t field, slot; int action; };
    std::vector<Binding> bindings_;
    std::vector<ui::PickerCustomControl> controls_;
    std::vector<PulsePickerUiV2Text> event_values_;
public:
    std::vector<PropertyField> fields;
    void Build(std::vector<ui::PickerCustomControl>& destination) {
        uint32_t next = UINT32_MAX;
        auto allocate = [&] { while (std::any_of(destination.begin(), destination.end(), [&](const auto& c) { return c.id == next; })) --next; return next--; };
        for (size_t f = 0; f < fields.size(); ++f) {
            auto& field = fields[f];
            ui::PickerCustomControl group; group.kind = 1; group.id = allocate(); group.state = 3; group.label = field.label; group.property_layout = 5;
            destination.push_back(group);
            const size_t slots = field.kind == PULSE_PICKER_UI_V2_PROPERTY_SCALAR ? 1 : PULSE_PICKER_UI_V2_MAX_PROPERTY_VALUES;
            for (size_t slot = 0; slot < slots; ++slot) {
                ui::PickerCustomControl edit; edit.kind = 8; edit.id = allocate(); edit.has_parent = true; edit.parent_id = group.id;
                edit.property_layout = field.kind == PULSE_PICKER_UI_V2_PROPERTY_LIST ? 1u : 4u;
                edit.label = field.label; edit.state = slot < field.values.size() ? 3u : 0u;
                if (slot < field.values.size()) edit.text = field.values[slot];
                controls_.push_back(edit); bindings_.push_back({edit.id, f, slot, 0}); destination.push_back(edit);
                if (field.kind == PULSE_PICKER_UI_V2_PROPERTY_LIST) {
                    ui::PickerCustomControl remove = edit; remove.kind = 2; remove.id = allocate(); remove.text.clear(); remove.label = L"−"; remove.property_layout = 2;
                    controls_.push_back(remove); bindings_.push_back({remove.id, f, slot, -1}); destination.push_back(remove);
                }
            }
            if (field.kind == PULSE_PICKER_UI_V2_PROPERTY_LIST) {
                ui::PickerCustomControl add; add.kind = 2; add.id = allocate(); add.has_parent = true; add.parent_id = group.id; add.label = L"+"; add.property_layout = 3;
                add.state = field.values.size() < slots ? 3u : 2u;
                controls_.push_back(add); bindings_.push_back({add.id, f, 0, 1}); destination.push_back(add);
            }
        }
    }
    HRESULT Translate(HWND window, ui::PickerDialogEvent kind, const ui::PickerDialogSnapshot& snapshot,
                      PulsePickerUiV2Event& event, bool& owned) {
        const auto binding = std::find_if(bindings_.begin(), bindings_.end(), [&](const auto& b) { return b.id == snapshot.control_id; });
        owned = binding != bindings_.end();
        if (!owned) return S_OK;
        if ((binding->action == 0 && kind != ui::PickerDialogEvent::EditChanged) ||
            (binding->action != 0 && kind != ui::PickerDialogEvent::ButtonClicked)) { event.kind = 0; return S_OK; }
        auto& field = fields[binding->field];
        if (binding->action == 0) {
            if (binding->slot >= field.values.size()) return E_INVALIDARG;
            field.values[binding->slot] = snapshot.control_text;
        } else {
            if (binding->action == 1) {
                if (field.values.size() >= PULSE_PICKER_UI_V2_MAX_PROPERTY_VALUES) return E_INVALIDARG;
                field.values.emplace_back();
            } else {
                if (binding->slot >= field.values.size()) return E_INVALIDARG;
                field.values.erase(field.values.begin() + binding->slot);
            }
            for (size_t i = 0; i < bindings_.size(); ++i) {
                const auto& b = bindings_[i]; if (b.field != binding->field) continue;
                auto& control = controls_[i];
                control.state = b.action == 1 ? (field.values.size() < PULSE_PICKER_UI_V2_MAX_PROPERTY_VALUES ? 3u : 2u) : (b.slot < field.values.size() ? 3u : 0u);
                if (!b.action) control.text = b.slot < field.values.size() ? field.values[b.slot] : L"";
                const HRESULT hr = ui::UpdatePickerCustomControl(window, control); if (FAILED(hr)) return hr;
            }
        }
        event_values_.clear();
        for (const auto& value : field.values) event_values_.push_back({value.data(), static_cast<uint32_t>(value.size())});
        event.kind = PULSE_PICKER_UI_V2_EVENT_PROPERTY_CHANGED; event.control_id = field.index;
        event.item_id = 0; event.control_text = {};
        event.property_values = event_values_.data(); event.property_value_count = static_cast<uint32_t>(event_values_.size());
        return S_OK;
    }
};
}

#pragma once
#include "../picker_inprocess/picker_ui_api_v2.h"
#include "picker_protocol.h"
#include <memory>

namespace pulse::ipc::picker::remote {
constexpr uint32_t kMaxFrame = 4 * 1024 * 1024;
enum class Kind : uint32_t {
    ValidateConfig = 1,
    Show,
    Event,
    Overwrite,
    ShareViolation,
    EventReply,
    Query,
    GetWindow,
    Close,
    UpdateControl,
    Navigate,
    Reply,
    Terminal,
    Hello
};
struct Filter {
    std::wstring label, pattern;
    bool operator==(const Filter &) const = default;
};
struct ControlItem {
    uint32_t id = 0, state = 0;
    std::wstring label;
    bool operator==(const ControlItem &) const = default;
};
struct Control {
    uint32_t kind = 0, id = 0, parent_id = 0, has_parent = 0, state = 0;
    std::wstring label;
    std::vector<ControlItem> items;
    uint32_t has_selected = 0, selected_item_id = 0, checked = 0;
    std::wstring text;
    bool operator==(const Control &) const = default;
};
struct Place {
    uint32_t location = 0;
    std::wstring label, path;
    bool operator==(const Place &) const = default;
};
struct Property {
    uint32_t index = 0, kind = 0;
    std::wstring label;
    std::vector<std::wstring> values;
    bool operator==(const Property &) const = default;
};
struct Config {
    uint32_t version = 2, required_capabilities = 0, appearance = 0, open_options = 0, access_policy = 1;
    uint64_t owner = 0;
    std::wstring initial_folder, filename, title;
    std::vector<Filter> filters;
    uint32_t filter_index = 0;
    std::vector<Control> controls;
    std::wstring default_extension;
    uint32_t save_options = 0, label_flags = 0;
    std::wstring ok_button_label, file_name_label;
    std::vector<Place> places;
    std::vector<Property> properties;
    uint32_t native_options = 0;
    bool operator==(const Config &) const = default;
};
struct Snapshot {
    uint32_t flags = 0;
    uint64_t show_id = 0, generation = 0, window = 0;
    uint32_t filter_index = 0;
    std::wstring folder, filename, selection, candidate;
    std::vector<std::wstring> selection_paths, candidate_paths;
    bool operator==(const Snapshot &) const = default;
};
struct Event {
    uint32_t kind = 0;
    uint64_t show_id = 0, generation = 0, window = 0;
    std::wstring proposed_folder;
    uint32_t control_id = 0, item_id = 0;
    std::wstring control_text;
    std::vector<std::wstring> property_values;
    uint32_t navigation_result = 0;
    std::wstring target;
    bool operator==(const Event &) const = default;
};
struct Result {
    uint32_t status = 0, flags = 0, win32_error = 0, terminal_hr = 0, filter_index = 0;
    uint64_t show_id = 0, generation = 0;
    std::wstring path;
    std::vector<std::wstring> paths;
    bool operator==(const Result &) const = default;
};
struct Message {
    RequestId request_id{};
    uint64_t sequence = 0;
    Kind kind = Kind::ValidateConfig;
    uint32_t hresult = 0;
    Config config;
    Event event;
    Snapshot snapshot;
    Result result;
    Control control;
    uint64_t show_id = 0, expected_generation = 0, window = 0;
    uint32_t response = 0, version = 0, capabilities = 0, features = 0;
    std::wstring folder;
    bool operator==(const Message &) const = default;
};
bool Encode(const Message &, std::vector<uint8_t> &);
bool Decode(std::span<const uint8_t>, Message &);
bool FromAbi(const PulsePickerUiV2Config &, Config &);
bool FromAbi(const PulsePickerUiV2Control &, Control &);
bool FromAbi(const PulsePickerUiV2Event &, Event &);
bool FromAbi(const PulsePickerUiV2OverwriteEvent &, Event &);
bool FromAbi(const PulsePickerUiV2Snapshot &, Snapshot &);
bool FromAbi(const PulsePickerUiV2Result &, Result &);
HRESULT CopyToAbi(const Snapshot &, PulsePickerUiV2Snapshot &);
HRESULT CopyToAbi(const Result &, PulsePickerUiV2Result &);
class OwnedControl {
    Control value_;
    std::vector<PulsePickerUiV2ControlItem> items_;
    PulsePickerUiV2Control abi_{};

  public:
    explicit OwnedControl(const Control &);
    OwnedControl(const OwnedControl &) = delete;
    OwnedControl &operator=(const OwnedControl &) = delete;
    const PulsePickerUiV2Control &Get() const { return abi_; }
};
class OwnedConfig {
    Config value_;
    std::vector<PulsePickerUiV2Filter> filters_;
    std::vector<std::unique_ptr<OwnedControl>> owned_controls_;
    std::vector<PulsePickerUiV2Control> controls_;
    std::vector<PulsePickerUiV2Place> places_;
    std::vector<std::vector<PulsePickerUiV2Text>> values_;
    std::vector<PulsePickerUiV2Property> properties_;
    PulsePickerUiV2Config abi_{};

  public:
    explicit OwnedConfig(const Config &);
    OwnedConfig(const OwnedConfig &) = delete;
    OwnedConfig &operator=(const OwnedConfig &) = delete;
    const PulsePickerUiV2Config &Get() const { return abi_; }
};
class OwnedEvent {
    Event value_;
    std::vector<PulsePickerUiV2Text> values_;
    PulsePickerUiV2Event abi_{};

  public:
    explicit OwnedEvent(const Event &);
    OwnedEvent(const OwnedEvent &) = delete;
    OwnedEvent &operator=(const OwnedEvent &) = delete;
    const PulsePickerUiV2Event &Get() const { return abi_; }
};
class OwnedOverwriteEvent {
    Event value_;
    PulsePickerUiV2OverwriteEvent abi_{};

  public:
    explicit OwnedOverwriteEvent(const Event &);
    OwnedOverwriteEvent(const OwnedOverwriteEvent &) = delete;
    OwnedOverwriteEvent &operator=(const OwnedOverwriteEvent &) = delete;
    const PulsePickerUiV2OverwriteEvent &Get() const { return abi_; }
};
} // namespace pulse::ipc::picker::remote

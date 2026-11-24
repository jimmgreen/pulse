// kit.hpp - Pulse house-style building blocks over ModelNode.
//
// Every function returns an ordinary ModelNode made only of public controls,
// so pages built with the kit render with Pulse's own painters, follow the
// light/dark theme and accent colour, and pass the same host validation as a
// hand-written tree. Use the kit to look like Pulse without memorising which
// role, gap or border the built-in pages use:
//
//   using namespace pulse::sdk::kit;
//   auto tree = pulse::sdk::ModelTree(Page("root", {
//       Section("general", "General", {
//           SwitchRow("auto", "Run automatically", "Start when Pulse opens", "auto", "icon:power"),
//           ChoiceRow("mode", "Mode", {}, "mode", {"Fast", "Balanced", "Thorough"}, "icon:gear"),
//       }),
//       Notice("tip", "informational", "Changes are saved as you go."),
//   })).State("auto", "true").State("mode", "1");
//
// Ids must be unique inside one tree; composite helpers derive child ids as
// "<id>.<part>". Budgets still apply: 128 nodes, depth 8 (Page + Section use 3).
#pragma once
#include "model_builder.hpp"
#include <string>
#include <utility>
#include <vector>

namespace pulse::sdk::kit {
using N = PulseUiModelNode;

// ------------------------------------------------------------------- text --
inline ModelNode Title(std::string id, std::string text) {          // 20 semibold page title
    return ModelNode(PULSE_UI_MODEL_TEXT, std::move(id)).Text(std::move(text)).Role("subtitle");
}
inline ModelNode Heading(std::string id, std::string text) {        // section caption above a card
    return ModelNode(PULSE_UI_MODEL_TEXT, std::move(id)).Text(std::move(text)).Role("section");
}
inline ModelNode Strong(std::string id, std::string text) {
    return ModelNode(PULSE_UI_MODEL_TEXT, std::move(id)).Text(std::move(text)).Role("bodyStrong");
}
inline ModelNode Body(std::string id, std::string text) {
    return ModelNode(PULSE_UI_MODEL_TEXT, std::move(id)).Text(std::move(text));
}
inline ModelNode Caption(std::string id, std::string text) {        // secondary 12px helper line
    return ModelNode(PULSE_UI_MODEL_TEXT, std::move(id)).Text(std::move(text)).Role("caption");
}
inline ModelNode Code(std::string id, std::string text) {
    return ModelNode(PULSE_UI_MODEL_TEXT, std::move(id)).Text(std::move(text)).Role("code");
}
// Text that shows the current value of a state key (updates live).
inline ModelNode Live(std::string id, std::string state_key, std::string role = "caption") {
    auto node = ModelNode(PULSE_UI_MODEL_TEXT, std::move(id)).String(&N::binding, "state." + std::move(state_key));
    if (!role.empty()) node.Role(std::move(role));
    return node;
}

// ------------------------------------------------------------- containers --
// Scrolling page body. Put Title/Sections/Cards inside.
inline ModelNode Page(std::string id, std::vector<ModelNode> children) {
    auto body = ModelNode(PULSE_UI_MODEL_STACK, id + ".body").Gap(12).Children(std::move(children));
    return ModelNode(PULSE_UI_MODEL_SCROLL_VIEW, std::move(id)).Children({std::move(body)});
}
// A page with a title line, optional description and right-hand actions.
inline ModelNode Page(std::string id, std::string title, std::string description, std::vector<ModelNode> actions,
                      std::vector<ModelNode> children) {
    std::vector<ModelNode> head_text;
    head_text.push_back(Title(id + ".title", std::move(title)));
    if (!description.empty()) head_text.push_back(Caption(id + ".desc", std::move(description)));
    std::vector<ModelNode> head;
    head.push_back(ModelNode(PULSE_UI_MODEL_STACK, id + ".headtext").Gap(2).Flex(1).Children(std::move(head_text)));
    for (auto& a : actions) head.push_back(std::move(a));
    std::vector<ModelNode> all;
    all.push_back(ModelNode(PULSE_UI_MODEL_STACK, id + ".head").Horizontal().Gap(8).Children(std::move(head)));
    for (auto& c : children) all.push_back(std::move(c));
    return Page(std::move(id), std::move(all));
}
// Bordered card in the settings style.
inline ModelNode Card(std::string id, std::vector<ModelNode> children) {
    return ModelNode(PULSE_UI_MODEL_GROUP, std::move(id)).Bordered().Gap(12).Children(std::move(children));
}
// Heading + optional caption + card: the unit every Pulse settings page is made of.
inline ModelNode Section(std::string id, std::string title, std::vector<ModelNode> rows, std::string caption = {}) {
    std::vector<ModelNode> parts;
    if (!title.empty()) parts.push_back(Heading(id + ".title", std::move(title)));
    if (!caption.empty()) parts.push_back(Caption(id + ".caption", std::move(caption)));
    parts.push_back(Card(id + ".card", std::move(rows)));
    return ModelNode(PULSE_UI_MODEL_STACK, std::move(id)).Gap(6).Children(std::move(parts));
}
inline ModelNode Row(std::string id, std::vector<ModelNode> children, int64_t gap = 8) {
    return ModelNode(PULSE_UI_MODEL_STACK, std::move(id)).Horizontal().Gap(gap).Children(std::move(children));
}
inline ModelNode Column(std::string id, std::vector<ModelNode> children, int64_t gap = 8) {
    return ModelNode(PULSE_UI_MODEL_STACK, std::move(id)).Gap(gap).Children(std::move(children));
}
inline ModelNode Columns(std::string id, int64_t count, std::vector<ModelNode> children, int64_t gap = 10) {
    return ModelNode(PULSE_UI_MODEL_GRID, std::move(id)).Columns(count).Gap(gap).Children(std::move(children));
}
inline ModelNode Divider(std::string id) { return ModelNode(PULSE_UI_MODEL_DIVIDER, std::move(id)); }
// Shows children only while state `key` is "true".
inline ModelNode When(std::string id, std::string key, std::vector<ModelNode> children, int64_t gap = 8) {
    return Column(std::move(id), std::move(children), gap).String(&N::visible_binding, "state." + std::move(key));
}

// ---------------------------------------------------------------- buttons --
inline ModelNode PrimaryButton(std::string id, std::string text, std::string command, std::string glyph = {}) {
    auto b = ModelNode(PULSE_UI_MODEL_BUTTON, std::move(id)).Text(std::move(text)).Command(std::move(command)).Kind("primary");
    if (!glyph.empty()) b.Glyph(std::move(glyph));
    return b;
}
inline ModelNode Button(std::string id, std::string text, std::string command, std::string glyph = {}) {
    auto b = ModelNode(PULSE_UI_MODEL_BUTTON, std::move(id)).Text(std::move(text)).Command(std::move(command));
    if (!glyph.empty()) b.Glyph(std::move(glyph));
    return b;
}
inline ModelNode DangerButton(std::string id, std::string text, std::string command) {
    return ModelNode(PULSE_UI_MODEL_BUTTON, std::move(id)).Text(std::move(text)).Command(std::move(command)).Kind("danger");
}
// Icon-only toolbar button; text is the accessible name and tooltip.
inline ModelNode IconButton(std::string id, std::string glyph, std::string text, std::string command) {
    return ModelNode(PULSE_UI_MODEL_BUTTON, std::move(id)).Text(std::move(text)).Command(std::move(command)).Glyph(std::move(glyph))
        .Kind("transparent").Bool(&N::icon_only, true);
}

// ----------------------------------------------------------- settings rows --
// Title (+detail) on the left, control on the right, optional leading icon.
inline ModelNode SwitchRow(std::string id, std::string title, std::string detail, std::string state_key, std::string glyph = {}) {
    auto control = ModelNode(PULSE_UI_MODEL_SWITCH, id + ".input").State(std::move(state_key)).MinWidth(48).MaxWidth(48);
    return ModelNode::SettingsRow(std::move(id), std::move(title), std::move(detail), std::move(control), std::move(glyph), false);
}
// Up to 4 short options become a segmented control, more become a drop-down.
// The state holds the zero-based index as text ("0", "1", ...).
inline ModelNode ChoiceRow(std::string id, std::string title, std::string detail, std::string state_key,
                           std::vector<std::string> options, std::string glyph = {}) {
    ModelNode control = options.size() <= 4
        ? ModelNode(PULSE_UI_MODEL_SEGMENTED, id + ".input").State(std::move(state_key)).Options(std::move(options)).MinWidth(220).MaxWidth(340)
        : ModelNode(PULSE_UI_MODEL_SELECT, id + ".input").Text(title).State(std::move(state_key)).Options(std::move(options)).MinWidth(200).MaxWidth(300);
    return ModelNode::SettingsRow(std::move(id), std::move(title), std::move(detail), std::move(control), std::move(glyph), false);
}
// Discrete slider over named stops; the state holds the index.
inline ModelNode StepsRow(std::string id, std::string title, std::string detail, std::string state_key,
                          std::vector<std::string> stops, std::string glyph = {}) {
    const auto count = static_cast<int64_t>(stops.size());
    auto control = ModelNode(PULSE_UI_MODEL_SLIDER, id + ".input").State(std::move(state_key)).Int(&N::stops, count)
        .Options(std::move(stops)).MinWidth(260).MaxWidth(340);
    return ModelNode::SettingsRow(std::move(id), std::move(title), std::move(detail), std::move(control), std::move(glyph), false);
}
// Labelled single-line text field (full width, label above).
inline ModelNode TextField(std::string id, std::string label, std::string state_key, std::string placeholder = {}, std::string helper = {}) {
    auto input = ModelNode(PULSE_UI_MODEL_TEXT_INPUT, id + ".input").Text(label).State(std::move(state_key));
    if (!placeholder.empty()) input.Placeholder(std::move(placeholder));
    return ModelNode::LabeledField(std::move(id), std::move(label), std::move(input), std::move(helper));
}
// Folder path field with a Browse button that sends `browse_command`.
// App::SettingsPage binds it to the folder picker automatically.
inline ModelNode FolderField(std::string id, std::string label, std::string state_key, std::string browse_command,
                             std::string browse_text = "Browse...", std::string placeholder = {}) {
    auto input = ModelNode(PULSE_UI_MODEL_TEXT_INPUT, id + ".input").Text(label).State(std::move(state_key)).Flex(1);
    if (!placeholder.empty()) input.Placeholder(std::move(placeholder));
    auto browse = ModelNode(PULSE_UI_MODEL_BUTTON, id + ".browse").Text(std::move(browse_text)).Command(std::move(browse_command));
    auto row = Row(id + ".row", {std::move(input), std::move(browse)});
    return ModelNode::LabeledField(std::move(id), std::move(label), std::move(row));
}
// Big selectable tiles (radio): each tile's state is "true" when chosen.
inline ModelNode Tile(std::string id, std::string title, std::string detail, std::string glyph = {}, std::string badge = {}) {
    auto state = id;
    return ModelNode::OptionCard(std::move(id), std::move(title), std::move(detail), std::move(state), "radio", std::move(glyph), std::move(badge));
}

// --------------------------------------------------------------- feedback --
// kind: informational | success | warning | error | neutral.
inline ModelNode Notice(std::string id, std::string kind, std::string message, std::string title = {}) {
    auto n = ModelNode(PULSE_UI_MODEL_INFO_BAR, std::move(id)).Kind(std::move(kind)).String(&N::message, std::move(message));
    if (!title.empty()) n.Text(std::move(title));
    return n;
}
// Centered placeholder for "nothing here yet".
inline ModelNode Empty(std::string id, std::string glyph, std::string title, std::string message = {}) {
    auto n = ModelNode(PULSE_UI_MODEL_EMPTY_STATE, std::move(id)).Text(std::move(title)).Glyph(std::move(glyph));
    if (!message.empty()) n.String(&N::message, std::move(message));
    return n;
}
// Indeterminate progress with a label (bind visibility with When()).
inline ModelNode Busy(std::string id, std::string text) {
    return ModelNode(PULSE_UI_MODEL_PROGRESS, std::move(id)).Text(std::move(text)).String(&N::status, "indeterminate");
}
// Determinate progress bound to a state holding 0..100.
inline ModelNode ProgressBar(std::string id, std::string text, std::string state_key) {
    return ModelNode(PULSE_UI_MODEL_PROGRESS, std::move(id)).Text(std::move(text)).String(&N::binding, "state." + std::move(state_key));
}
// kind: neutral | accent | success | warning | danger.
inline ModelNode Pill(std::string id, std::string text, std::string kind = "neutral") {
    return ModelNode(PULSE_UI_MODEL_BADGE, std::move(id)).Text(std::move(text)).Kind(std::move(kind));
}
// Two-column label/value list ("Size  12 MB"). Each pair costs two nodes.
inline ModelNode Facts(std::string id, const std::vector<std::pair<std::string, std::string>>& rows) {
    std::vector<ModelNode> cells;
    cells.reserve(rows.size() * 2);
    for (size_t i = 0; i < rows.size(); ++i) {
        cells.push_back(Caption(id + ".k" + std::to_string(i), rows[i].first));
        cells.push_back(Body(id + ".v" + std::to_string(i), rows[i].second));
    }
    return Columns(std::move(id), 2, std::move(cells), 6);
}
}  // namespace pulse::sdk::kit

#include "dialogs.h"
#include "text.h"
#include <pulse/client.hpp>
#include <windows.h>
#include <filesystem>
#include <optional>
#include <vector>

using pulse::sdk::ModelNode;
using pulse::sdk::ModelTree;
using pulse::sdk::View;
using N = PulseUiModelNode;
using T = text::Id;

namespace {
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n);
    return out;
}
std::string Copy(PulseStringView value) { return value.data && value.length ? std::string(value.data, value.length) : std::string{}; }

constexpr const char* kFormats[] = {"7z", "zip", "tar", "tar.gz", "tar.xz", "tar.bz2"};
constexpr engine::Format kFormatValues[] = {engine::Format::SevenZip, engine::Format::Zip, engine::Format::Tar,
                                            engine::Format::TarGz, engine::Format::TarXz, engine::Format::TarBz2};
constexpr unsigned kLevels[] = {0, 1, 3, 5, 7, 9};
constexpr uint64_t kVolumes[] = {0, 100ull << 20, 700ull << 20, 4092ull << 20, 4700ull * 1000 * 1000};
int Index(const std::string& value, int count, int fallback) {
    if (value.size() != 1 || value[0] < '0' || value[0] >= '0' + count) return fallback;
    return value[0] - '0';
}
std::wstring StripExtension(const std::wstring& name) {
    for (const auto* ext : {L".tar.gz", L".tar.xz", L".tar.bz2", L".7z", L".zip", L".tar"}) {
        const size_t length = wcslen(ext);
        if (name.size() > length && _wcsicmp(name.c_str() + name.size() - length, ext) == 0) return name.substr(0, name.size() - length);
    }
    return name;
}
} // namespace

struct Dialogs::Request {
    std::string title, message;
    struct Button { std::string id, text; uint32_t kind; };
    std::vector<Button> buttons;
    uint32_t default_button = 0, cancel_button = PULSE_DIALOG_NO_BUTTON, width = 0, height = 0;
    std::optional<ModelTree> tree;
    std::function<void(Dialogs&, uint64_t, const States&, const States&)> on_change;  // changes, all-so-far
    std::function<void(bool, const std::string&, const States&)> on_closed;
    States states;  // last known values
    Dialogs* owner = nullptr;
};

void Dialogs::Enqueue(std::shared_ptr<Request> request) {
    request->owner = this;
    queue_.push_back(std::move(request));
    if (!current_) Next();
}

void Dialogs::Next() {
    while (!current_ && !queue_.empty()) {
        auto request = queue_.front();
        queue_.pop_front();
        std::vector<PulseDialogButton> buttons;
        for (const auto& button : request->buttons)
            buttons.push_back({sizeof(PulseDialogButton), button.kind, View(button.id), View(button.text)});
        PulseDialogRequest spec{};
        spec.size = sizeof(spec);
        spec.width = request->width;
        spec.height = request->height;
        spec.default_button = request->default_button;
        spec.cancel_button = request->cancel_button;
        spec.button_count = static_cast<uint32_t>(buttons.size());
        spec.title = View(request->title);
        spec.message = View(request->message);
        spec.buttons = buttons.data();
        spec.model = request->tree ? &request->tree->Build() : nullptr;
        current_ = request;
        uint64_t id = 0;
        if (api_.show(api_.context, &spec, request->on_change ? Changed : nullptr, Closed, request.get(), &id) != PULSE_OK) {
            current_.reset();
            if (request->on_closed) request->on_closed(false, {}, request->states);
        }
    }
}

void PULSE_CALL Dialogs::Changed(void* user, uint64_t dialog, const PulseUiState* changes, uint32_t count) {
    auto& request = *static_cast<Request*>(user);
    try {
        States batch;
        for (uint32_t i = 0; i < count; ++i) {
            batch[Copy(changes[i].key)] = Copy(changes[i].value);
            request.states[Copy(changes[i].key)] = Copy(changes[i].value);
        }
        if (request.on_change) request.on_change(*request.owner, dialog, batch, request.states);
    } catch (...) {}
}

void PULSE_CALL Dialogs::Closed(void* user, uint64_t, PulseStatus status, PulseStringView button,
                                const PulseUiState* states, uint32_t count) {
    auto* request = static_cast<Request*>(user);
    auto* owner = request->owner;
    auto keep = owner->current_;  // the request stays alive through its callback
    owner->current_.reset();
    try {
        for (uint32_t i = 0; i < count; ++i) request->states[Copy(states[i].key)] = Copy(states[i].value);
        if (request->on_closed) request->on_closed(status == PULSE_OK, Copy(button), request->states);
    } catch (...) {}
    owner->Next();
}

namespace {
ModelNode Caption(const char* id, const std::string& value) {
    return ModelNode(PULSE_UI_MODEL_TEXT, id).String(&N::text, value).String(&N::role, "caption");
}
ModelNode Section(const char* id, const std::string& value) {
    return ModelNode(PULSE_UI_MODEL_TEXT, id).String(&N::text, value).String(&N::role, "section");
}
// A settings-style row: title and description on the left, the control on the right.
ModelNode Row(const std::string& id, const std::string& title, const std::string& description, ModelNode control,
              const char* description_binding = nullptr) {
    auto detail = ModelNode(PULSE_UI_MODEL_TEXT, (id + "-desc").c_str()).String(&N::text, description).String(&N::role, "caption");
    if (description_binding) detail.String(&N::binding, description_binding);
    return ModelNode(PULSE_UI_MODEL_STACK, (id + "-row").c_str()).String(&N::orientation, "horizontal").String(&N::alignment, "center")
        .Int(&N::gap, 12).Children({
            ModelNode(PULSE_UI_MODEL_STACK, (id + "-text").c_str()).Int(&N::gap, 0).Int(&N::flex, 1).Children({
                ModelNode(PULSE_UI_MODEL_TEXT, (id + "-title").c_str()).String(&N::text, title), std::move(detail)}),
            std::move(control)});
}
ModelNode Card(const char* id, std::vector<ModelNode> children) {
    return ModelNode(PULSE_UI_MODEL_GROUP, id).Bool(&N::bordered, true).Int(&N::gap, 10).Children(std::move(children));
}
// The row's title names the switch; a label here would not fit its 48 DIP column.
ModelNode Switch(const char* id, const char* state, const std::string&) {
    return ModelNode(PULSE_UI_MODEL_SWITCH, id).String(&N::state, state).Int(&N::min_width, 48).Int(&N::max_width, 48);
}
std::string Bool(bool value) { return value ? "true" : "false"; }
std::string Lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
std::string Trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
std::string Grouped(uint64_t value) {
    auto digits = std::to_string(value);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<size_t>(at), ",");
    return digits;
}
constexpr T kFormatNotes[] = {T::Fmt7z, T::FmtZip, T::FmtTar, T::FmtTarGz, T::FmtTarXz, T::FmtTarBz2};
constexpr T kLevelNames[] = {T::Level0, T::Level1, T::Level3, T::Level5, T::Level7, T::Level9};
} // namespace

void Dialogs::AskPassword(const std::wstring& archive, bool retry, bool remembering,
                          std::function<void(bool, std::wstring)> done) {
    auto request = std::make_shared<Request>();
    request->title = text::Get(T::PasswordTitle);
    request->buttons = {{"ok", text::Get(T::PasswordStart), PULSE_DIALOG_BUTTON_PRIMARY}, {"cancel", text::Get(T::Cancel), PULSE_DIALOG_BUTTON_STANDARD}};
    request->default_button = 0;
    request->cancel_button = 1;
    request->width = 440;
    request->height = 320;  // a ceiling: the host fits the dialog to the form
    std::vector<ModelNode> children;
    children.push_back(ModelNode(PULSE_UI_MODEL_STACK, "password-head").Int(&N::gap, 2).Children({
        Section("password-archive", Utf8(archive)), Caption("password-hint", text::Get(T::PasswordHint))}));
    if (retry)
        children.push_back(ModelNode(PULSE_UI_MODEL_INFO_BAR, "password-wrong").String(&N::kind, "error")
            .String(&N::message, text::Get(T::PasswordWrong)));
    children.push_back(ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "password-input").String(&N::text, text::Get(T::PasswordLabel))
        .String(&N::placeholder, text::Get(T::PasswordLabel)).String(&N::state, "password").String(&N::kind, "password"));
    if (remembering) children.push_back(Caption("password-memory", text::Get(T::PasswordMemory)));
    request->tree.emplace(ModelNode(PULSE_UI_MODEL_STACK, "password-root").Int(&N::gap, 12).Children(std::move(children)));
    request->tree->State("password", "");
    request->on_closed = [done = std::move(done)](bool ok, const std::string& button, const States& states) {
        const auto at = states.find("password");
        if (ok && button == "ok" && at != states.end()) done(true, Wide(at->second));
        else done(false, {});
    };
    Enqueue(std::move(request));
}

void Dialogs::ChooseCompression(const CompressSource& source, int format, int level, const std::string& error,
                                std::function<void(bool, CompressChoice)> done) {
    auto request = std::make_shared<Request>();
    format = format < 0 || format > 5 ? 0 : format;
    level = level < 0 || level > 5 ? 3 : level;
    // Everything the visible fields depend on, from the format and the password switch.
    const auto derived = [](int f, bool protect, int lvl) {
        const bool encryptable = f <= 1, protecting = encryptable && protect;
        return States{{"ext", std::string(".") + kFormats[f]}, {"fmtdesc", text::Get(kFormatNotes[f])},
                      {"encryptable", Bool(encryptable)}, {"sevenzip", Bool(f == 0)}, {"leveled", Bool(f != 2)},
                      {"levelname", f == 2 ? text::Get(T::LevelTar) : text::Get(kLevelNames[lvl])},
                      {"protecting", Bool(protecting)}, {"sevenprotect", Bool(protecting && f == 0)},
                      {"zipprotect", Bool(protecting && f == 1)},
                      {"protectdesc", !protecting ? text::Get(T::ProtectOff) : f == 0 ? text::Get(T::Protect7z) : text::Get(T::ProtectZip)}};
    };
    request->title = text::Get(T::AddTitle);
    request->buttons = {{"start", text::Get(T::AddStart), PULSE_DIALOG_BUTTON_PRIMARY}, {"cancel", text::Get(T::Cancel), PULSE_DIALOG_BUTTON_STANDARD}};
    request->default_button = 0;
    request->cancel_button = 1;
    request->width = 560;
    request->height = 640;  // a ceiling: the host fits the dialog to the visible fields
    auto select = [](const char* id, const char* state, std::vector<std::string> options) {
        return ModelNode(PULSE_UI_MODEL_SELECT, id).String(&N::state, state).Options(std::move(options))
            .Int(&N::min_width, 180).Int(&N::max_width, 180);
    };
    auto secret = [](const char* id, const char* state, const char* placeholder) {
        return ModelNode(PULSE_UI_MODEL_TEXT_INPUT, id).String(&N::text, placeholder).String(&N::placeholder, placeholder)
            .String(&N::state, state).String(&N::kind, "password");
    };
    std::vector<std::string> levels;
    for (const auto id : kLevelNames) levels.push_back(text::Get(id));
    std::vector<ModelNode> children;
    children.push_back(Card("source", {ModelNode(PULSE_UI_MODEL_STACK, "source-text").Int(&N::gap, 0).Children({
        Section("source-title", source.title), Caption("source-detail", source.detail)})}));
    if (!error.empty())
        children.push_back(ModelNode(PULSE_UI_MODEL_INFO_BAR, "error").String(&N::kind, "error").String(&N::message, error));
    children.push_back(ModelNode(PULSE_UI_MODEL_STACK, "name-block").Int(&N::gap, 6).Children({
        ModelNode(PULSE_UI_MODEL_TEXT, "name-label").String(&N::text, text::Get(T::AddName)),
        ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "name-input").String(&N::text, text::Get(T::AddName)).String(&N::state, "name")
            .String(&N::badge_text, "state.ext")}));
    children.push_back(ModelNode(PULSE_UI_MODEL_STACK, "format-block").Int(&N::gap, 6).Children({
        ModelNode(PULSE_UI_MODEL_TEXT, "format-label").String(&N::text, text::Get(T::AddFormat)),
        ModelNode(PULSE_UI_MODEL_SEGMENTED, "format-input").String(&N::state, "format").Options({kFormats, kFormats + 6}),
        ModelNode(PULSE_UI_MODEL_TEXT, "format-note").String(&N::binding, "state.fmtdesc").String(&N::role, "caption")}));
    children.push_back(ModelNode(PULSE_UI_MODEL_STACK, "level-block").Int(&N::gap, 0).Children({
        ModelNode(PULSE_UI_MODEL_STACK, "level-head").String(&N::orientation, "horizontal").String(&N::alignment, "center").Children({
            ModelNode(PULSE_UI_MODEL_TEXT, "level-label").String(&N::text, text::Get(T::AddLevel)).Int(&N::flex, 1),
            ModelNode(PULSE_UI_MODEL_TEXT, "level-name").String(&N::binding, "state.levelname").String(&N::role, "caption")
                .String(&N::alignment, "right").Int(&N::min_width, 160).Int(&N::max_width, 160)}),
        ModelNode(PULSE_UI_MODEL_SLIDER, "level-input").String(&N::state, "level").Int(&N::stops, 6).Options(levels)
            .String(&N::enabled_binding, "state.leveled")}));
    children.push_back(Card("protect", {
        Row("protect", text::Get(T::ProtectTitle), {}, Switch("protect-input", "protect", text::Get(T::ProtectTitle)), "state.protectdesc"),
        ModelNode(PULSE_UI_MODEL_GRID, "password-grid").Int(&N::columns, 2).Int(&N::gap, 10).String(&N::visible_binding, "state.protecting")
            .Children({secret("password-input", "password", text::Get(T::PasswordLabel)),
                       secret("confirm-input", "confirm", text::Get(T::AddConfirm))}),
        ModelNode(PULSE_UI_MODEL_CHECKBOX, "encrypt-names").String(&N::state, "names").String(&N::text, text::Get(T::AddEncryptNames))
            .String(&N::visible_binding, "state.sevenprotect"),
        ModelNode(PULSE_UI_MODEL_STACK, "zipmethod-row").String(&N::orientation, "horizontal").String(&N::alignment, "center")
            .String(&N::visible_binding, "state.zipprotect").Children({
                ModelNode(PULSE_UI_MODEL_TEXT, "zipmethod-label").String(&N::text, text::Get(T::AddZipMethod)).Int(&N::flex, 1),
                select("zipmethod-input", "zipmethod", {text::Get(T::ZipAes), text::Get(T::ZipCrypto)})}),
        ModelNode(PULSE_UI_MODEL_INFO_BAR, "mismatch").String(&N::kind, "error").String(&N::message, text::Get(T::AddMismatch))
            .String(&N::visible_binding, "state.mismatch")}).String(&N::visible_binding, "state.encryptable"));
    children.push_back(Caption("more-label", text::Get(T::AddMore)));
    children.push_back(Card("more", {
        Row("volume", text::Get(T::AddVolume), text::Get(T::VolumeDesc),
            select("volume-input", "volume", {text::Get(T::VolumeNone), "100 MB", "700 MB (CD)", "4092 MB (FAT32)", "4.7 GB (DVD)"})),
        Row("solid", text::Get(T::SolidTitle), text::Get(T::SolidDesc), Switch("solid-input", "solid", text::Get(T::SolidTitle)))
            .String(&N::visible_binding, "state.sevenzip")}));
    request->tree.emplace(ModelNode(PULSE_UI_MODEL_STACK, "compress-content").Int(&N::gap, 14).Children(std::move(children)));
    auto& tree = *request->tree;
    tree.State("name", Utf8(source.base)).State("format", std::to_string(format)).State("level", std::to_string(level))
        .State("protect", "false").State("volume", "0").State("password", "").State("confirm", "").State("names", "true")
        .State("zipmethod", "0").State("solid", "true").State("mismatch", "false");
    for (const auto& [key, value] : derived(format, false, level)) tree.State(key, value);
    request->states = {{"name", Utf8(source.base)}, {"format", std::to_string(format)}, {"level", std::to_string(level)},
                       {"protect", "false"}, {"password", ""}, {"confirm", ""}};
    request->on_change = [derived](Dialogs& dialogs, uint64_t dialog, const States& changes, const States& all) {
        const auto get = [&](const char* key) { const auto at = all.find(key); return at == all.end() ? std::string{} : at->second; };
        std::vector<std::pair<std::string, std::string>> updates;
        if (changes.contains("format") || changes.contains("protect") || changes.contains("level"))
            for (const auto& item : derived(Index(get("format"), 6, 0), get("protect") == "true", Index(get("level"), 6, 3)))
                updates.push_back(item);
        if (changes.contains("password") || changes.contains("confirm")) {
            const auto confirm = get("confirm");
            updates.push_back({"mismatch", Bool(!confirm.empty() && get("password") != confirm)});
        }
        if (updates.empty()) return;
        std::vector<PulseUiState> views;
        for (const auto& [key, value] : updates) views.push_back({View(key), View(value)});
        dialogs.api_.set_states(dialogs.api_.context, dialog, views.data(), static_cast<uint32_t>(views.size()));
    };
    request->on_closed = [this, done = std::move(done), source](bool ok, const std::string& button, const States& states) {
        if (!ok || button != "start") { done(false, {}); return; }
        const auto get = [&](const char* key) { const auto at = states.find(key); return at == states.end() ? std::string{} : at->second; };
        CompressChoice choice;
        const int format_index = Index(get("format"), 6, 0);
        const int level_index = Index(get("level"), 6, 3);
        choice.options.format = kFormatValues[format_index];
        choice.options.level = kLevels[level_index];
        choice.options.volume_size = kVolumes[Index(get("volume"), 5, 0)];
        choice.options.solid = get("solid") != "false";
        choice.options.zip_aes = get("zipmethod") != "1";
        const bool protect = format_index <= 1 && get("protect") == "true";
        const auto password = get("password"), confirm = get("confirm");
        auto stem = StripExtension(Wide(get("name")));
        while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) stem.pop_back();
        std::string error;
        if (stem.empty() || stem.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) error = text::Get(T::AddBadName);
        else if (protect && password != confirm) error = text::Get(T::AddMismatch);
        if (!error.empty()) {  // reopen with the problem stated
            auto again = source;
            if (!stem.empty()) again.base = stem;
            ChooseCompression(again, format_index, level_index, error, done);
            return;
        }
        if (protect && !password.empty()) {
            choice.options.password = Wide(password);
            choice.options.encrypt_names = format_index == 0 && get("names") != "false";
        }
        choice.name = stem + engine::FormatExtension(choice.options.format);
        done(true, std::move(choice));
    };
    Enqueue(std::move(request));
}

void Dialogs::ShowChecksums(const std::vector<engine::HashResult>& results, std::function<void(bool)> done) {
    auto request = std::make_shared<Request>();
    request->title = text::Get(T::HashTitle);
    request->buttons = {{"copy", text::Get(T::HashCopyAll), PULSE_DIALOG_BUTTON_PRIMARY}, {"close", text::Get(T::Close), PULSE_DIALOG_BUTTON_STANDARD}};
    request->default_button = 0;
    request->cancel_button = 1;
    request->width = 600;
    request->height = 640;
    std::vector<ModelNode> children;
    std::vector<std::pair<std::string, std::string>> known;  // value, algorithm
    constexpr size_t kShown = 8;
    for (size_t index = 0; index < results.size() && index < kShown; ++index) {
        const auto& item = results[index];
        const auto id = "file" + std::to_string(index);
        std::vector<ModelNode> rows;
        rows.push_back(ModelNode(PULSE_UI_MODEL_STACK, (id + "-head").c_str()).Int(&N::gap, 0).Children({
            Section((id + "-name").c_str(), Utf8(std::filesystem::path(item.path).filename().wstring())),
            Caption((id + "-size").c_str(), item.error.empty() ? text::Format(T::HashBytes, Grouped(item.size)) : item.error)}));
        if (item.error.empty()) {
            const std::pair<const char*, const std::string*> values[] = {{"CRC32", &item.crc32}, {"SHA-1", &item.sha1}, {"SHA-256", &item.sha256}};
            for (const auto& [name, value] : values) {
                known.push_back({*value, name});
                const auto key = id + "-" + Lower(name);
                rows.push_back(ModelNode(PULSE_UI_MODEL_STACK, (key + "-row").c_str()).String(&N::orientation, "horizontal")
                    .String(&N::alignment, "center").Int(&N::gap, 12).Children({
                        ModelNode(PULSE_UI_MODEL_TEXT, (key + "-label").c_str()).String(&N::text, name).String(&N::role, "caption")
                            .Int(&N::min_width, 64).Int(&N::max_width, 64),
                        ModelNode(PULSE_UI_MODEL_TEXT, (key + "-value").c_str()).String(&N::text, *value).String(&N::role, "caption").Int(&N::flex, 1)}));
            }
        }
        children.push_back(Card(id.c_str(), std::move(rows)).Int(&N::gap, 4));
    }
    children.push_back(ModelNode(PULSE_UI_MODEL_STACK, "verify-block").Int(&N::gap, 6).Children({
        ModelNode(PULSE_UI_MODEL_STACK, "verify-head").String(&N::orientation, "horizontal").String(&N::alignment, "center").Children({
            ModelNode(PULSE_UI_MODEL_TEXT, "verify-label").String(&N::text, text::Get(T::HashVerify)).Int(&N::flex, 1),
            ModelNode(PULSE_UI_MODEL_TEXT, "verify-hint").String(&N::text, text::Get(T::HashVerifyHint)).String(&N::role, "caption")
                .String(&N::alignment, "right").Int(&N::min_width, 280).Int(&N::max_width, 280)}),
        ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "verify-input").String(&N::text, text::Get(T::HashVerify))
            .String(&N::placeholder, text::Get(T::HashVerifyPlaceholder)).String(&N::state, "verify"),
        ModelNode(PULSE_UI_MODEL_TEXT, "verdict").String(&N::binding, "state.verdict").String(&N::role, "caption")
            .String(&N::visible_binding, "state.checked")}));
    request->tree.emplace(ModelNode(PULSE_UI_MODEL_STACK, "hash-content").Int(&N::gap, 12).Children(std::move(children)));
    request->tree->State("verify", "").State("verdict", "").State("checked", "false");
    request->on_change = [known](Dialogs& dialogs, uint64_t dialog, const States& changes, const States&) {
        const auto at = changes.find("verify");
        if (at == changes.end()) return;
        const auto pasted = Lower(Trim(at->second));
        std::string verdict;
        if (!pasted.empty()) {
            verdict = text::Get(T::HashNoMatch);
            for (const auto& [value, name] : known)
                if (value == pasted) { verdict = text::Format(T::HashMatch, name); break; }
        }
        const std::string checked = Bool(!pasted.empty());
        const PulseUiState views[] = {{View("verdict"), View(verdict)}, {View("checked"), View(checked)}};
        dialogs.api_.set_states(dialogs.api_.context, dialog, views, 2);
    };
    request->on_closed = [done = std::move(done)](bool ok, const std::string& button, const States&) { done(ok && button == "copy"); };
    Enqueue(std::move(request));
}

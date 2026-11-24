// app.hpp - optional C++20 application layer over the Pulse native SDK.
//
//   #include <pulse/app.hpp>
//   class Hello : public pulse::sdk::App {
//   public:
//       using App::App;
//       PulseStatus OnStart() override {
//           Command("hello", Tr("打个招呼", "打個招呼", "Say hello"))
//               .Icon("sparkle").Files(1, 64)
//               .Run([this](const pulse::sdk::Selection& s) { Toast(std::to_string(s.Count()) + " selected"); });
//           return PULSE_OK;
//       }
//   };
//   PULSE_APP(Hello)
//
// Everything here is header-only and stays inside the plugin DLL: STL objects,
// lambdas and exceptions never cross the C ABI. The layer only uses the public
// C interfaces, so it works with any host that implements them, and degrades
// (returns PULSE_ERROR_UNAVAILABLE) when an optional interface is missing.
//
// Threading: every member function is UI-thread-only unless it says otherwise.
// Post() and TaskProgress are the worker-safe entry points.
#pragma once
#include "client.hpp"
#include "resources.h"
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::sdk {

// ---------------------------------------------------------------- language --
enum class Language { ChineseSimplified, ChineseTraditional, English };

inline Language LanguageFromTag(std::string_view tag) noexcept {
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    std::string t;
    for (char c : tag) t.push_back(lower(c == '_' ? '-' : c));
    if (t.rfind("zh", 0) != 0) return Language::English;
    if (t.find("-tw") != std::string::npos || t.find("-hk") != std::string::npos ||
        t.find("-mo") != std::string::npos || t.find("hant") != std::string::npos)
        return Language::ChineseTraditional;
    return Language::ChineseSimplified;
}

// A string in the three languages Pulse ships. Empty Traditional falls back to
// Simplified; empty English falls back to Simplified as well.
struct LocalText {
    std::string zh, tw, en;
    LocalText() = default;
    LocalText(const char* single) : zh(single ? single : ""), en(single ? single : "") {}            // NOLINT
    LocalText(std::string single) : zh(single), en(std::move(single)) {}                             // NOLINT
    LocalText(std::string s, std::string t, std::string e) : zh(std::move(s)), tw(std::move(t)), en(std::move(e)) {}
    const std::string& In(Language language) const noexcept {
        switch (language) {
        case Language::ChineseTraditional: return !tw.empty() ? tw : zh;
        case Language::English: return !en.empty() ? en : zh;
        default: return !zh.empty() ? zh : en;
        }
    }
    bool Empty() const noexcept { return zh.empty() && tw.empty() && en.empty(); }
};
inline LocalText Tr(std::string zh, std::string tw, std::string en) { return LocalText(std::move(zh), std::move(tw), std::move(en)); }

// Replaces {0}, {1}, ... with the arguments (no other formatting).
inline std::string Format(std::string_view pattern, std::initializer_list<std::string_view> args) {
    std::string out;
    out.reserve(pattern.size() + 16);
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && i + 2 < pattern.size() && pattern[i + 2] == '}' && pattern[i + 1] >= '0' && pattern[i + 1] <= '9') {
            const size_t index = static_cast<size_t>(pattern[i + 1] - '0');
            if (index < args.size()) { out.append(*(args.begin() + index)); i += 2; continue; }
        }
        out.push_back(pattern[i]);
    }
    return out;
}

// --------------------------------------------------------------- selection --
struct SelectedItem {
    std::string path, name;
    bool directory = false;
    // "report.final.txt" -> "report.final"; folders keep their full name.
    std::string Stem() const {
        if (directory) return name;
        const auto dot = name.rfind('.');
        return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    }
    // Lowercase extension with its dot (".txt"), or empty.
    std::string Extension() const {
        if (directory) return {};
        const auto dot = name.rfind('.');
        if (dot == std::string::npos || dot == 0) return {};
        std::string ext = name.substr(dot);
        for (char& c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return ext;
    }
};

// An owned copy of the host's borrowed selection snapshot: safe to keep and to
// hand to worker threads.
struct Selection {
    std::string directory;
    std::vector<SelectedItem> items;
    uint64_t revision = 0;
    bool Empty() const noexcept { return items.empty(); }
    size_t Count() const noexcept { return items.size(); }
    const SelectedItem& First() const { return items.front(); }
    size_t Files() const noexcept { size_t n = 0; for (const auto& i : items) n += i.directory ? 0 : 1; return n; }
    size_t Folders() const noexcept { return items.size() - Files(); }
    std::vector<std::string> Paths() const { std::vector<std::string> out; out.reserve(items.size()); for (const auto& i : items) out.push_back(i.path); return out; }
    static Selection Copy(const PulseContextSelectionSnapshot& snapshot) {
        auto str = [](PulseStringView v) { return std::string(v.data ? v.data : "", v.data ? v.length : 0); };
        Selection out;
        out.directory = str(snapshot.directory);
        out.revision = snapshot.selection_revision;
        if (snapshot.entries) {
            out.items.reserve(snapshot.entry_count);
            for (uint32_t i = 0; i < snapshot.entry_count; ++i) {
                const auto& e = snapshot.entries[i];
                out.items.push_back({str(e.path), str(e.name), (e.flags & PULSE_CONTEXT_ENTRY_DIRECTORY) != 0});
            }
        }
        return out;
    }
};

// ---------------------------------------------------------------- settings --
// Package-owned key/value settings persisted by the host (pulse.resources).
// Loads once on start; Get reads the cache; Set/Remove are serialized, use the
// host's version check and retry once automatically after an outside change.
// Keys: 1..64 of [A-Za-z0-9._-]; values: up to 1024 UTF-8 bytes.
class SettingsStore {
public:
    using Done = std::function<void(PulseStatus status, std::string_view error)>;
    SettingsStore() = default;
    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;
    ~SettingsStore() { Close(); }

    bool Available() const noexcept { return api_.settings_get != nullptr; }
    bool Ready() const noexcept { return ready_; }
    // Runs fn once the first load finishes (immediately if it already has).
    void OnReady(std::function<void()> fn) { if (!fn) return; if (ready_) fn(); else ready_waiters_.push_back(std::move(fn)); }
    // Runs fn after every change made by this plugin or seen on reload (listeners accumulate).
    void OnChanged(std::function<void()> fn) { if (fn) changed_.push_back(std::move(fn)); }

    std::string Get(std::string_view key, std::string_view fallback = {}) const {
        const auto it = values_.find(std::string(key));
        return it == values_.end() ? std::string(fallback) : it->second;
    }
    bool GetBool(std::string_view key, bool fallback = false) const {
        const auto it = values_.find(std::string(key));
        return it == values_.end() ? fallback : (it->second == "true" || it->second == "1");
    }
    int64_t GetInt(std::string_view key, int64_t fallback = 0) const {
        const auto it = values_.find(std::string(key));
        if (it == values_.end() || it->second.empty()) return fallback;
        int64_t v = 0; bool neg = false; size_t i = 0;
        if (it->second[0] == '-') { neg = true; i = 1; }
        if (i >= it->second.size()) return fallback;
        for (; i < it->second.size(); ++i) {
            const char c = it->second[i];
            if (c < '0' || c > '9' || v > (INT64_MAX - 9) / 10) return fallback;
            v = v * 10 + (c - '0');
        }
        return neg ? -v : v;
    }
    const std::map<std::string, std::string>& All() const noexcept { return values_; }

    PulseStatus Set(std::string key, std::string value, Done done = {}) { return Enqueue({std::move(key), std::move(value), false, std::move(done)}); }
    PulseStatus SetBool(std::string key, bool value, Done done = {}) { return Set(std::move(key), value ? "true" : "false", std::move(done)); }
    PulseStatus SetInt(std::string key, int64_t value, Done done = {}) { return Set(std::move(key), std::to_string(value), std::move(done)); }
    PulseStatus Remove(std::string key, Done done = {}) { return Enqueue({std::move(key), {}, true, std::move(done)}); }
    // Re-reads everything from the host.
    PulseStatus Reload() { return Load(false); }

    // ---- used by App ----
    PulseStatus Attach(const Host& host) {
        Close();
        closed_ = false;
        ready_ = false;
        values_.clear();
        version_ = 0;
        PulseResourcesV1 table{};
        const auto status = host.Query(PULSE_RESOURCES_INTERFACE_NAME, PULSE_RESOURCES_VERSION_1, table);
        if (status != PULSE_OK) return status;
        api_ = table;
        return Load(true);
    }
    void Close() noexcept {
        closed_ = true;
        if (api_.cancel_request)
            for (auto& r : requests_) if (r->id && !r->finished) { try { api_.cancel_request(api_.context, r->id); } catch (...) {} }
        // Requests stay allocated until destruction: a cancelled request never calls back,
        // but keeping them costs nothing and avoids any lifetime race.
        queue_.clear();
        busy_ = false;
        changed_.clear();
        ready_waiters_.clear();
    }

private:
    struct Op { std::string key, value; bool remove = false; Done done; };
    struct Request {
        SettingsStore* owner = nullptr;
        PulseResourcesRequestId id = 0;
        bool finished = false, running = false;
        std::function<void(const PulseResourcesReply&)> fn;
        static void PULSE_CALL Complete(void* user, const PulseResourcesReply* reply) noexcept {
            try {
                auto* self = static_cast<Request*>(user);
                if (!self || self->finished) return;
                self->finished = true;
                self->running = true;
                struct Clear { bool& flag; ~Clear() { flag = false; } } clear{self->running};
                if (reply && self->fn && self->owner && !self->owner->closed_) self->fn(*reply);
            } catch (...) {}
        }
    };
    PulseResourcesV1 api_{};
    std::map<std::string, std::string> values_;
    uint64_t version_ = 0;
    bool ready_ = false, busy_ = false, closed_ = true;
    std::vector<std::function<void()>> ready_waiters_;
    std::vector<std::function<void()>> changed_;
    void NotifyChanged() { const auto copy = changed_; for (const auto& fn : copy) { try { fn(); } catch (...) {} } }
    std::deque<Op> queue_;
    std::vector<std::unique_ptr<Request>> requests_;

    Request* NewRequest(std::function<void(const PulseResourcesReply&)> fn) {
        std::erase_if(requests_, [](const auto& r) { return r->finished && !r->running; });
        auto r = std::make_unique<Request>();
        r->owner = this; r->fn = std::move(fn);
        requests_.push_back(std::move(r));
        return requests_.back().get();
    }
    void Absorb(const PulseSettings& settings) {
        values_.clear();
        for (uint32_t i = 0; settings.values && i < settings.count; ++i) {
            const auto& s = settings.values[i];
            values_[std::string(s.key.data ? s.key.data : "", s.key.length)] = std::string(s.value.data ? s.value.data : "", s.value.length);
        }
        version_ = settings.version;
    }
    void MarkReady() {
        if (ready_) return;
        ready_ = true;
        auto waiters = std::move(ready_waiters_);
        ready_waiters_.clear();
        for (auto& w : waiters) { try { w(); } catch (...) {} }
    }
    PulseStatus Load(bool first) {
        if (!api_.settings_get) return PULSE_ERROR_UNAVAILABLE;
        auto* r = NewRequest([this, first](const PulseResourcesReply& reply) {
            if (reply.status == PULSE_OK) Absorb(reply.settings);
            MarkReady();  // even on failure: callers fall back to defaults
            if (!first) NotifyChanged();
        });
        const auto status = api_.settings_get(api_.context, Request::Complete, r, &r->id);
        if (status != PULSE_OK) { r->finished = true; MarkReady(); }
        return status;
    }
    PulseStatus Enqueue(Op op) {
        if (!api_.settings_set) return PULSE_ERROR_UNAVAILABLE;
        if (closed_) return PULSE_ERROR_STATE;
        queue_.push_back(std::move(op));
        if (!busy_) Pump();
        return PULSE_OK;
    }
    void Finish(Op& op, PulseStatus status, std::string_view error) {
        busy_ = false;
        if (op.done) { try { op.done(status, error); } catch (...) {} }
        if (status == PULSE_OK) NotifyChanged();
        if (!closed_) Pump();
    }
    void Pump() {
        if (busy_ || queue_.empty() || closed_) return;
        busy_ = true;
        auto op = std::make_shared<Op>(std::move(queue_.front()));
        queue_.pop_front();
        Send(op, true);
    }
    void Send(std::shared_ptr<Op> op, bool may_retry) {
        auto* r = NewRequest([this, op, may_retry](const PulseResourcesReply& reply) {
            const std::string_view error(reply.error.data ? reply.error.data : "", reply.error.length);
            if (reply.status == PULSE_OK) { Absorb(reply.settings); Finish(*op, PULSE_OK, {}); return; }
            if (may_retry && error == "version_conflict") {
                // Someone else saved first: refresh the version and apply ours once more.
                auto* g = NewRequest([this, op](const PulseResourcesReply& again) {
                    if (again.status == PULSE_OK) { Absorb(again.settings); Send(op, false); }
                    else Finish(*op, again.status, "version_conflict");
                });
                const auto st = api_.settings_get(api_.context, Request::Complete, g, &g->id);
                if (st != PULSE_OK) { g->finished = true; Finish(*op, st, "version_conflict"); }
                return;
            }
            Finish(*op, reply.status, error);
        });
        const auto status = api_.settings_set(api_.context, View(op->key), View(op->remove ? std::string_view() : op->value),
                                              op->remove ? 1u : 0u, version_, Request::Complete, r, &r->id);
        if (status != PULSE_OK) { r->finished = true; Finish(*op, status, "rejected"); }
    }
};

// ------------------------------------------------------------------- tasks --
// Worker-side view of a running task. Safe on the task's worker thread.
class TaskProgress {
public:
    explicit TaskProgress(const PulseTaskContext& context) noexcept : context_(context) {}
    bool Cancelled() const noexcept { return context_.cancelled && context_.cancelled(context_.context) != 0; }
    // percent 0..100, or -1 for indeterminate. Never goes backwards.
    void Report(int percent, std::string_view current_item = {}) const noexcept {
        if (!context_.report) return;
        PulseTaskProgress p{};
        p.size = static_cast<uint32_t>(offsetof(PulseTaskProgress, step));
        p.percent = percent < 0 ? UINT32_MAX : static_cast<uint32_t>(percent > 100 ? 100 : percent);
        p.current_item = View(current_item);
        (void)context_.report(context_.context, &p);
    }
    // Item-based progress: the host derives the percentage.
    void Items(uint64_t done, uint64_t total, std::string_view current_item = {}) const noexcept {
        if (!context_.report) return;
        PulseTaskProgress p{};
        p.size = static_cast<uint32_t>(offsetof(PulseTaskProgress, step));
        p.percent = total ? static_cast<uint32_t>((done > total ? total : done) * 100 / total) : UINT32_MAX;
        p.completed_items = done > total ? total : done;
        p.total_items = total;
        p.current_item = View(current_item);
        (void)context_.report(context_.context, &p);
    }
    void Error(std::string_view message) const noexcept {
        if (!context_.report) return;
        PulseTaskProgress p{};
        p.size = static_cast<uint32_t>(offsetof(PulseTaskProgress, step));
        p.percent = UINT32_MAX;
        p.error = View(message);
        (void)context_.report(context_.context, &p);
    }
    PulseTaskId Id() const noexcept { return context_.id; }
private:
    const PulseTaskContext& context_;
};

// --------------------------------------------------------------------- app --
enum class Tone { Info, Success, Warning, Error };

class App;

// Fluent builder returned by App::Command(); Run() registers the command.
class CommandBuilder {
public:
    CommandBuilder(App& app, std::string id, LocalText title) : app_(app), id_(std::move(id)), title_(std::move(title)) {}
    CommandBuilder& Detail(LocalText text) { detail_ = std::move(text); return *this; }
    CommandBuilder& Group(LocalText caption, bool submenu = false) { group_ = std::move(caption); submenu_ = submenu; return *this; }
    // Built-in icon name ("sparkle", "archive", ...) or a package icon id from contributes.icons.
    CommandBuilder& Icon(std::string name) { icon_name_ = std::move(name); return *this; }
    CommandBuilder& Icon(uint32_t id) { icon_ = id; return *this; }
    CommandBuilder& Files(uint32_t min = 1, uint32_t max = 4096) { kinds_ |= PULSE_COMMAND_FILES; Range(min, max); return *this; }
    CommandBuilder& Folders(uint32_t min = 1, uint32_t max = 4096) { kinds_ |= PULSE_COMMAND_DIRECTORIES; Range(min, max); return *this; }
    CommandBuilder& Anything(uint32_t min = 1, uint32_t max = 4096) { kinds_ |= PULSE_COMMAND_FILES | PULSE_COMMAND_DIRECTORIES; Range(min, max); return *this; }
    // Folder background (right-click on empty space); combines with the others.
    CommandBuilder& Background() { kinds_ |= PULSE_COMMAND_BACKGROUND; min_ = 0; return *this; }
    // Lowercase dotted extensions (".png"); exclude=true turns it into a deny list.
    CommandBuilder& Extensions(std::vector<std::string> list, bool exclude = false) { extensions_ = std::move(list); exclude_ = exclude; return *this; }
    // key: 'A'..'Z', '0'..'9' or VK_F1.. ; modifiers: PULSE_COMMAND_CTRL | SHIFT | ALT.
    CommandBuilder& Shortcut(uint32_t key, uint32_t modifiers) { key_ = key; modifiers_ = modifiers; return *this; }
    CommandBuilder& Primary(bool value = true) { Flag(PULSE_COMMAND_PRIMARY, value); return *this; }
    CommandBuilder& SeparatorBefore(bool value = true) { Flag(PULSE_COMMAND_SEPARATOR_BEFORE, value); return *this; }
    CommandBuilder& SeparatorAfter(bool value = true) { Flag(PULSE_COMMAND_SEPARATOR_AFTER, value); return *this; }
    CommandBuilder& Checked(bool value = true) { Flag(PULSE_COMMAND_CHECKED, value); return *this; }
    CommandBuilder& Hidden(bool value = true) { Flag(PULSE_COMMAND_HIDDEN, value); return *this; }
    // Suggests a toolbar button: pinned once on first sight (max 2 per package); the user can remove it.
    CommandBuilder& Toolbar(bool value = true) { Flag(PULSE_COMMAND_TOOLBAR, value); return *this; }
    // Registers the command. The handler gets an owned copy of the selection.
    inline PulseStatus Run(std::function<void(const Selection&)> handler);
private:
    friend class App;
    void Range(uint32_t min, uint32_t max) { min_ = min; max_ = max < min ? min : max; }
    void Flag(uint32_t flag, bool on) { flags_ = on ? (flags_ | flag) : (flags_ & ~flag); }
    App& app_;
    std::string id_;
    LocalText title_, detail_, group_;
    bool submenu_ = false;
    std::string icon_name_;
    uint32_t icon_ = PULSE_ICON_DEFAULT, kinds_ = 0, min_ = 1, max_ = 4096, key_ = 0, modifiers_ = 0, flags_ = 0;
    std::vector<std::string> extensions_;
    bool exclude_ = false;
};

// One registered command. Keep the handle (App does) to change it later.
class CommandHandle {
public:
    PulseCommandId Id() const noexcept { return registration_ ? registration_->Id() : 0; }
    // Show/hide or check/uncheck without re-registering (needs pulse.commands V2).
    PulseStatus SetHidden(bool hidden) { return SetFlag(PULSE_COMMAND_HIDDEN, hidden); }
    PulseStatus SetChecked(bool checked) { return SetFlag(PULSE_COMMAND_CHECKED, checked); }
private:
    friend class App;
    friend class CommandBuilder;
    PulseStatus SetFlag(uint32_t flag, bool on) {
        if (!registration_) return PULSE_ERROR_STATE;
        flags_ = on ? (flags_ | flag) : (flags_ & ~flag);
        return registration_->SetFlags(flags_);
    }
    std::shared_ptr<CommandRegistration> registration_;
    uint32_t flags_ = 0;
};

// Options for App::Ask (a form dialog). The form is any ModelTree (see kit.hpp).
struct DialogOptions {
    LocalText title;                 // window caption; defaults to the app name
    LocalText heading;               // big line above the content
    LocalText message;               // plain paragraph (when there is no form)
    LocalText ok = Tr("确定", "確定", "OK");
    LocalText cancel = Tr("取消", "取消", "Cancel");  // empty: single-button dialog
    bool danger = false;             // the OK button is destructive
    uint32_t icon = PULSE_DIALOG_ICON_NONE;
    uint32_t subject_kind = PULSE_DIALOG_SUBJECT_NONE;
    std::string subject, subject_detail;
    LocalText footnote;
    uint32_t width = 520, height = 0;  // height 0: host fits the content
};
using DialogValues = std::map<std::string, std::string>;

// A page under Settings > Extensions whose controls are bound to Settings().
// build() returns the tree (use kit.hpp); it runs on start and again when the
// Pulse language changes. Every key listed in `keys` is loaded into the tree's
// state before display and saved whenever the user changes it.
struct SettingsPageSpec {
    LocalText title;
    std::function<ModelTree()> build;
    std::vector<std::pair<std::string, std::string>> keys;     // state key -> default value
    std::vector<std::pair<std::string, std::string>> folders;  // button command -> state key (folder picker)
};

class App {
public:
    explicit App(const PulseHostV1& host) : host_(host) {}
    virtual ~App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // ---- override these ----
    // Register commands, pages and providers here (or later, while active). Every
    // Command/Add*/On* registration made after stop returns PULSE_ERROR_STATE.
    // Return non-OK to refuse activation.
    virtual PulseStatus OnStart() { return PULSE_OK; }
    // Undo anything OnStart created that the App does not own. Do not block.
    virtual void OnStop() {}
    // The Pulse UI language changed; commands created with LocalText are
    // re-titled automatically before this runs.
    virtual void OnLanguageChanged() {}
    // Display name used for dialog captions; override or set in OnStart.
    virtual LocalText Name() const { return LocalText("Pulse extension"); }

    // ---- environment ----
    const Host& HostApi() const noexcept { return host_; }
    bool Active() const noexcept { return active_; }
    Language CurrentLanguage() const noexcept { return language_; }
    std::string Text(const LocalText& text) const { return text.In(language_); }
    std::string Text(std::string zh, std::string tw, std::string en) const { return LocalText(std::move(zh), std::move(tw), std::move(en)).In(language_); }

    // ---- services (nullptr when the host does not offer them) ----
    WorkbenchClient* Workbench() noexcept { return workbench_ ? &*workbench_ : nullptr; }
    PickerClient* Pickers() noexcept { return pickers_ ? &*pickers_ : nullptr; }
    IconsClient* Icons() noexcept { return icons_ ? &*icons_ : nullptr; }
    ColumnsClient* Columns() noexcept { return columns_ ? &*columns_ : nullptr; }
    SettingsStore& Settings() noexcept { return settings_; }
    DialogSession& Dialog() noexcept { return dialog_; }

    // Worker-safe: runs fn on the UI thread later (dropped after stop).
    PulseStatus Post(std::function<void()> fn) {
        if (!queue_) return PULSE_ERROR_STATE;
        return queue_->Post([this, fn = std::move(fn)] { if (active_ && fn) fn(); });
    }

    // ---- commands ----
    CommandBuilder Command(std::string id, LocalText title) { return CommandBuilder(*this, std::move(id), std::move(title)); }
    // The handle of a command registered with Run(), or nullptr.
    CommandHandle* FindCommand(std::string_view id) noexcept {
        for (auto& c : commands_) if (c->id == id) return &c->handle;
        return nullptr;
    }

    // ---- feedback ----
    PulseStatus Toast(std::string_view text, Tone tone = Tone::Info, std::string_view detail = {}) {
        if (!workbench_) return PULSE_ERROR_UNAVAILABLE;
        PulseSnackRequest r{};
        r.size = sizeof(r);
        r.kind = NoticeKind(tone);
        r.text = View(Clip(text, 256));
        r.detail = View(Clip(detail, 256));
        return workbench_->ShowSnack(r);
    }
    PulseStatus Copy(std::string_view text) { return workbench_ ? workbench_->SetClipboardText(text) : PULSE_ERROR_UNAVAILABLE; }
    PulseStatus Reveal(std::string_view path) { return workbench_ ? workbench_->RevealPath(path) : PULSE_ERROR_UNAVAILABLE; }

    // ---- dialogs (one at a time; a follow-up dialog may be opened from the callbacks) ----
    PulseStatus Alert(LocalText heading, LocalText message, std::function<void()> closed = {}) {
        DialogOptions o;
        o.heading = std::move(heading); o.message = std::move(message);
        o.icon = PULSE_DIALOG_ICON_INFO; o.cancel = LocalText();
        return Ask(o, nullptr, [closed = std::move(closed)](const DialogValues&) { if (closed) closed(); }, nullptr);
    }
    PulseStatus Confirm(LocalText heading, LocalText message, LocalText ok, std::function<void()> yes, bool danger = false) {
        DialogOptions o;
        o.heading = std::move(heading); o.message = std::move(message); o.ok = std::move(ok);
        o.danger = danger; o.icon = danger ? PULSE_DIALOG_ICON_WARNING : PULSE_DIALOG_ICON_INFO;
        return Ask(o, nullptr, [yes = std::move(yes)](const DialogValues&) { if (yes) yes(); }, nullptr);
    }
    // A dialog around an optional form. ok() receives every form state; cancel() runs on dismiss.
    PulseStatus Ask(const DialogOptions& options, const ModelTree* form,
                    std::function<void(const DialogValues&)> ok, std::function<void()> cancel = {},
                    std::function<void(const DialogValues&)> changed = {}) {
        if (!active_ || !dialog_.Available()) return PULSE_ERROR_UNAVAILABLE;
        if (dialog_.Id()) return PULSE_ERROR_STATE;
        const std::string title = Text(options.title.Empty() ? Name() : options.title);
        const std::string heading = Text(options.heading), message = Text(options.message);
        const std::string ok_text = Text(options.ok), cancel_text = Text(options.cancel), footnote = Text(options.footnote);
        PulseDialogButton buttons[2] = {
            {sizeof(PulseDialogButton), options.danger ? static_cast<uint32_t>(PULSE_DIALOG_BUTTON_DANGER) : static_cast<uint32_t>(PULSE_DIALOG_BUTTON_PRIMARY), View("ok"), View(ok_text)},
            {sizeof(PulseDialogButton), PULSE_DIALOG_BUTTON_STANDARD, View("cancel"), View(cancel_text)}};
        const bool two = !cancel_text.empty();
        PulseDialogRequest r{};
        r.size = sizeof(r);
        r.width = options.width;
        r.height = options.height ? options.height : (form ? 640u : 0u);
        r.title = View(title);
        r.heading = View(heading);
        r.message = View(message);
        r.icon = options.icon;
        r.subject_kind = options.subject_kind;
        r.subject = View(options.subject);
        r.subject_detail = View(options.subject_detail);
        r.footnote = View(footnote);
        r.buttons = buttons;
        r.button_count = two ? 2u : 1u;
        r.default_button = 0;
        r.cancel_button = two ? 1u : 0u;
        r.model = form ? &form->Build() : nullptr;
        return dialog_.Show(r,
            changed ? std::function<void(uint64_t, const DialogSession::StateList&)>(
                [changed](uint64_t, const DialogSession::StateList& list) { changed(ToMap(list)); })
                    : std::function<void(uint64_t, const DialogSession::StateList&)>(),
            [this, two, ok = std::move(ok), cancel = std::move(cancel)](uint64_t, PulseStatus status, std::string_view button,
                                                                        const DialogSession::StateList& states) {
                // Deliver after the dialog has fully closed so handlers may open another one.
                const bool accepted = status == PULSE_OK && (button == "ok" || !two);
                auto values = ToMap(states);
                (void)Post([accepted, ok, cancel, values = std::move(values)] {
                    if (accepted) { if (ok) ok(values); }
                    else if (cancel) cancel();
                });
            });
    }

    // ---- background work ----
    // Runs work on a host worker with the standard task UI (progress capsule,
    // cancel). Return PULSE_TASK_COMPLETED / PARTIAL / FAILED / CANCELLED.
    PulseStatus RunTask(LocalText title, std::function<PulseTaskState(TaskProgress&)> work,
                        std::function<void(PulseTaskState)> finished = {}) {
        if (!tasks_) return PULSE_ERROR_UNAVAILABLE;
        tasks_->Collect();
        PulseTaskId id = 0;
        return tasks_->Start(Text(title), "app", [this, work = std::move(work), finished = std::move(finished)](const PulseTaskContext& ctx) {
            TaskProgress progress(ctx);
            PulseTaskState state = PULSE_TASK_FAILED;
            try { state = work(progress); } catch (...) { progress.Error("unexpected error"); state = PULSE_TASK_FAILED; }
            if (finished) (void)Post([finished, state] { finished(state); });
            (void)Post([this] { if (tasks_) tasks_->Collect(); });
            return state;
        }, nullptr, &id);
    }

    // ---- settings page ----
    // Shows the page once settings have loaded. One page per plugin.
    PulseStatus SettingsPage(SettingsPageSpec spec) {
        if (!active_) return PULSE_ERROR_STATE;
        if (!spec.build) return PULSE_ERROR_ARGUMENT;
        if (!pages_ || !pages_->Available()) return PULSE_ERROR_UNAVAILABLE;
        page_spec_ = std::move(spec);
        settings_.OnReady([this] { if (active_) CreatePage(); });
        return PULSE_OK;
    }
    // Panel id of the settings page (0 until shown), for advanced use.
    uint64_t SettingsPageId() const noexcept { return page_id_; }
    // Listens to a button command on the settings page.
    PulseStatus OnSettingsCommand(std::string command, std::function<void()> fn) {
        page_handlers_.emplace_back(std::move(command), std::move(fn));
        if (page_id_) return BindPageCommand(page_handlers_.back().first, page_handlers_.back().second);
        return PULSE_OK;
    }
    // Writes a state on the settings page (no-op before it is shown).
    PulseStatus SetSettingsState(std::string_view key, std::string_view value) {
        if (!page_id_ || !page_model_.set_states) return PULSE_ERROR_STATE;
        const PulseUiState st{View(key), View(value)};
        return page_model_.set_states(page_model_.context, page_id_, &st, 1);
    }

    // ---- Details columns and icon badges (pulse.columns) ----
    // fn runs on a host worker thread, item by item: read files, do not touch
    // the UI (use Post). Values are cached per file version; removed on stop.
    PulseStatus AddColumn(const ColumnSpec& spec, ColumnTextFn fn) {
        if (!active_) return PULSE_ERROR_STATE;
        if (!columns_) return PULSE_ERROR_UNAVAILABLE;
        ColumnsClient::Registration reg;
        const PulseStatus st = columns_->AddColumn(spec, std::move(fn), reg);
        if (st == PULSE_OK) column_regs_.emplace_back(spec.id, std::move(reg));
        return st;
    }
    PulseStatus AddBadge(const ColumnSpec& spec, BadgeIconFn fn) {
        if (!active_) return PULSE_ERROR_STATE;
        if (!columns_) return PULSE_ERROR_UNAVAILABLE;
        ColumnsClient::Registration reg;
        const PulseStatus st = columns_->AddBadge(spec, std::move(fn), reg);
        if (st == PULSE_OK) column_regs_.emplace_back(spec.id, std::move(reg));
        return st;
    }
    // Asks again for one item (path) or every item; empty id = all columns and badges.
    PulseStatus RefreshColumns(std::string_view id = {}, std::string_view path = {}) {
        PulseStatus result = id.empty() ? PULSE_OK : PULSE_ERROR_ARGUMENT;
        for (auto& [reg_id, reg] : column_regs_) {
            if (!id.empty() && reg_id != id) continue;
            result = reg.Invalidate(path);
        }
        return result;
    }
    // Retitles a column or badge, e.g. from OnLanguageChanged.
    PulseStatus SetColumnTitle(std::string_view id, std::string_view title) {
        for (auto& [reg_id, reg] : column_regs_)
            if (reg_id == id) return reg.SetTitle(title);
        return PULSE_ERROR_ARGUMENT;
    }

    // ---- Status bar items (pulse.statusbar) ----
    // Up to 2 per package, after the folder text and the Git chip. on_click runs on the
    // UI thread. Removed on stop.
    PulseStatus AddStatusItem(const StatusItem& spec) {
        if (!active_) return PULSE_ERROR_STATE;
        if (!statusbar_) return PULSE_ERROR_UNAVAILABLE;
        StatusBarClient::Item item;
        const PulseStatus st = statusbar_->Add(spec, item);
        if (st == PULSE_OK) status_items_.emplace_back(spec.id, std::move(item));
        return st;
    }
    // Replaces the item's text, icon, tooltip, tone and click; spec.id is ignored.
    PulseStatus UpdateStatusItem(std::string_view id, const StatusItem& spec) {
        for (auto& [item_id, item] : status_items_) {
            if (item_id != id) continue;
            StatusItem copy = spec;
            copy.id.clear();
            return item.Update(copy);
        }
        return PULSE_ERROR_ARGUMENT;
    }
    PulseStatus SetStatusItemVisible(std::string_view id, bool visible) {
        for (auto& [item_id, item] : status_items_)
            if (item_id == id) return item.SetVisible(visible);
        return PULSE_ERROR_ARGUMENT;
    }
    PulseStatus RemoveStatusItem(std::string_view id) {
        for (auto it = status_items_.begin(); it != status_items_.end(); ++it) {
            if (it->first != id) continue;
            status_items_.erase(it);   // waits for a running click callback
            return PULSE_OK;
        }
        return PULSE_ERROR_ARGUMENT;
    }
    StatusBarClient* StatusBar() noexcept { return statusbar_ ? &*statusbar_ : nullptr; }

    // ---- Folder, selection and file-operation hooks (pulse.context / pulse.fileevents) ----
    // All callbacks run on the UI thread and are read-only; removed on stop. Keep them short
    // (use RunTask / Post for real work).
    // The active folder changed (also once soon after start, with PULSE_CONTEXT_INITIAL).
    PulseStatus OnFolderChange(FolderContextFn fn) {
        if (!fn) return PULSE_ERROR_ARGUMENT;
        return Watch([fn = std::move(fn)](const FolderContext& c) { if (c.FolderChanged()) fn(c); });
    }
    // The selection changed (count and single name; CurrentSelection() lists the items).
    PulseStatus OnSelectionChange(FolderContextFn fn) {
        if (!fn) return PULSE_ERROR_ARGUMENT;
        return Watch([fn = std::move(fn)](const FolderContext& c) { if (c.SelectionChanged()) fn(c); });
    }
    // A file operation Pulse ran took effect. kinds: PULSE_FILE_EVENT_MASK(...) bits, 0 = all.
    PulseStatus OnFileOperation(FileEventFn fn, uint32_t kinds = 0) {
        if (!active_) return PULSE_ERROR_STATE;
        if (!fileevents_) return PULSE_ERROR_UNAVAILABLE;
        FileEventsClient::Subscription sub;
        const PulseStatus st = fileevents_->Subscribe(kinds, std::move(fn), sub);
        if (st == PULSE_OK) file_subs_.push_back(std::move(sub));
        return st;
    }
    // Preview pane / Quick Look content for files ending in `extension` (lowercase,
    // dotted: ".foo"). flags PULSE_PREVIEW_PROVIDER_THUMBNAIL: also grid thumbnails
    // (request.Thumbnail() is true there; publish a bitmap). fn runs on a host worker
    // thread, up to 2 at once: no UI calls there (use Post), poll request.Cancelled().
    PulseStatus AddPreview(std::string_view extension, PreviewFn fn, uint32_t flags = 0) {
        if (!active_) return PULSE_ERROR_STATE;
        if (!preview_) return PULSE_ERROR_UNAVAILABLE;
        RemovePreview(extension);
        PreviewClient::Provider provider;
        const PulseStatus st = preview_->Register(extension, std::move(fn), flags, provider);
        if (st == PULSE_OK) previews_.emplace_back(std::string(extension), std::move(provider));
        return st;
    }
    void RemovePreview(std::string_view extension) {
        std::erase_if(previews_, [&](const auto& entry) { return entry.first == extension; });
    }
    // The active folder now (synchronous); nullopt when unavailable.
    std::optional<FolderContext> CurrentFolder() const {
        if (!context_) return std::nullopt;
        FolderContext out;
        if (context_->Get(out) != PULSE_OK) return std::nullopt;
        return out;
    }
    // The loaded selection now (synchronous, no disk access); nullopt while loading, for
    // virtual views or very large selections.
    std::optional<Selection> CurrentSelection() const {
        if (!context_ || !context_->Table().get_selection) return std::nullopt;
        std::optional<Selection> out;
        const auto& api = context_->Table();
        (void)api.get_selection(api.context, [](void* user, const PulseContextSelectionSnapshot* raw) noexcept {
            try { if (raw) *static_cast<std::optional<Selection>*>(user) = Selection::Copy(*raw); } catch (...) {}
        }, &out);
        return out;
    }

    // ---- used by PULSE_APP; do not call ----
    PulseStatus StartInternal() {
        PulseDispatchV1 dispatch{};
        if (host_.Query(PULSE_DISPATCH_INTERFACE, PULSE_DISPATCH_VERSION, dispatch) == PULSE_OK)
            queue_ = std::make_unique<DispatchQueue>(dispatch);
        PulseCommandsV2 v2{};
        if (host_.Query(PULSE_COMMANDS_INTERFACE_NAME, PULSE_COMMANDS_VERSION_2, v2) == PULSE_OK) { commands_v2_ = v2; has_v2_ = true; }
        else (void)host_.Query(PULSE_COMMANDS_INTERFACE_NAME, PULSE_COMMANDS_VERSION_1, commands_v1_);
        PulseTasksV1 tasks{};
        if (host_.Query(PULSE_TASKS_INTERFACE_NAME, PULSE_TASKS_VERSION_1, tasks) == PULSE_OK) tasks_ = std::make_unique<TaskGroup>(tasks);
        workbench_ = WorkbenchClient::TryQuery(host_);
        pickers_ = PickerClient::TryQuery(host_);
        icons_ = IconsClient::TryQuery(host_);
        columns_ = ColumnsClient::TryQuery(host_);
        statusbar_ = StatusBarClient::TryQuery(host_);
        context_ = ContextClient::TryQuery(host_);
        fileevents_ = FileEventsClient::TryQuery(host_);
        preview_ = PreviewClient::TryQuery(host_);
        (void)dialog_.Init(host_);
        pages_ = SettingsPagesClient::TryQuery(host_);
        (void)settings_.Attach(host_);
        if (workbench_) {
            PulseHostLocale locale{};
            if (workbench_->QueryLocale(locale) == PULSE_OK) language_ = LanguageFromTag(TagOf(locale));
            (void)workbench_->SubscribeLocale(locale_sub_, [this](const PulseHostLocale& l) {
                const auto next = LanguageFromTag(TagOf(l));
                if (next == language_) return;
                language_ = next;
                RetitleCommands();
                RebuildPage();
                try { OnLanguageChanged(); } catch (...) {}
            });
        }
        active_ = true;
        PulseStatus status = PULSE_ERROR_INITIALIZATION;
        try { status = OnStart(); } catch (...) { status = PULSE_ERROR_INITIALIZATION; }
        if (status != PULSE_OK) StopInternal();
        return status;
    }
    void StopInternal() noexcept {
        const bool was_active = active_;
        active_ = false;
        if (was_active) { try { OnStop(); } catch (...) {} }
        column_regs_.clear();   // waits for a running column callback
        status_items_.clear();
        context_subs_.clear();
        file_subs_.clear();
        previews_.clear();      // retries briefly while a provider call drains
        commands_.clear();
        dialog_.Close();
        ClosePage();
        locale_sub_.Reset();
        settings_.Close();
        if (tasks_) { try { tasks_->CancelAll(); tasks_->Collect(); } catch (...) {} }
        if (queue_) queue_->Close();
    }

private:
    friend class CommandBuilder;
    struct CommandEntry {
        std::string id;
        CommandBuilder spec;
        CommandHandle handle;
        // Owned storage the descriptor points into.
        std::string title, detail, group;
        std::vector<PulseStringView> extensions;
        CommandEntry(CommandBuilder b) : id(b.id_), spec(std::move(b)) {}
    };
    static uint32_t NoticeKind(Tone tone) noexcept {
        switch (tone) {
        case Tone::Success: return PULSE_NOTICE_SUCCESS;
        case Tone::Warning: return PULSE_NOTICE_WARNING;
        case Tone::Error: return PULSE_NOTICE_ERROR;
        default: return PULSE_NOTICE_INFO;
        }
    }
    static std::string_view Clip(std::string_view text, size_t limit) noexcept {
        if (text.size() <= limit) return text;
        size_t cut = limit;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) --cut;  // keep UTF-8 whole
        return text.substr(0, cut);
    }
    static std::string TagOf(const PulseHostLocale& locale) {
        size_t n = 0;
        while (n < sizeof(locale.language_tag) && locale.language_tag[n] != '\0') ++n;
        return std::string(locale.language_tag, n);
    }
    static DialogValues ToMap(const DialogSession::StateList& list) {
        DialogValues out;
        for (const auto& [k, v] : list) out[k] = v;
        return out;
    }
    PulseCommandDescriptor Describe(CommandEntry& e) {
        const auto& s = e.spec;
        e.title = Text(s.title_);
        e.detail = Text(s.detail_);
        e.group = Text(s.group_);
        e.extensions.clear();
        for (const auto& x : s.extensions_) e.extensions.push_back(View(x));
        PulseCommandDescriptor d{};
        d.size = sizeof(d);
        d.id = View(e.id);
        d.title = View(e.title);
        d.icon = s.icon_;
        d.kinds = s.kinds_ ? s.kinds_ : (PULSE_COMMAND_FILES | PULSE_COMMAND_DIRECTORIES);
        d.min_selection = (d.kinds & PULSE_COMMAND_BACKGROUND) ? 0u : s.min_;
        d.max_selection = s.max_;
        d.extensions = e.extensions.empty() ? nullptr : e.extensions.data();
        d.extension_count = static_cast<uint32_t>(e.extensions.size());
        d.exclude_extensions = s.exclude_ ? 1u : 0u;
        d.virtual_key = s.key_;
        d.modifiers = s.modifiers_;
        d.detail = View(e.detail);
        d.group = View(e.group);
        d.flags = e.handle.flags_ | (s.submenu_ ? static_cast<uint32_t>(PULSE_COMMAND_GROUP_SUBMENU) : 0u);
        d.icon_name = View(s.icon_name_);
        return d;
    }
    PulseStatus Register(CommandBuilder builder, std::function<void(const Selection&)> handler) {
        if (!active_ || !handler) return PULSE_ERROR_STATE;
        if (!has_v2_ && !commands_v1_.register_command) return PULSE_ERROR_UNAVAILABLE;
        auto entry = std::make_unique<CommandEntry>(std::move(builder));
        entry->handle.flags_ = entry->spec.flags_ & ~static_cast<uint32_t>(PULSE_COMMAND_GROUP_SUBMENU);
        entry->handle.registration_ = std::make_shared<CommandRegistration>();
        const auto descriptor = Describe(*entry);
        auto fn = [this, handler = std::move(handler)](PulseCommandId, const PulseContextSelectionSnapshot& snapshot) {
            if (!active_) return;
            try { handler(Selection::Copy(snapshot)); } catch (...) {}
        };
        const auto status = has_v2_ ? entry->handle.registration_->Register(commands_v2_, descriptor, fn)
                                    : entry->handle.registration_->Register(commands_v1_, descriptor, fn);
        if (status == PULSE_OK) commands_.push_back(std::move(entry));
        return status;
    }
    void RetitleCommands() {
        for (auto& e : commands_) {
            if (!e->handle.registration_) continue;
            const auto descriptor = Describe(*e);
            (void)e->handle.registration_->Update(descriptor);
        }
    }

    ModelTree PageTree() {
        ModelTree tree = page_spec_->build();
        for (const auto& [key, fallback] : page_spec_->keys) tree.State(key, settings_.Get(key, fallback));
        for (const auto& [command, key] : page_spec_->folders) { (void)key; tree.Command(command); }
        for (const auto& [command, fn] : page_handlers_) { (void)fn; tree.Command(command); }
        return tree;
    }
    std::string PageState(std::string_view key) const {
        if (!page_id_ || !page_model_.get_state) return {};
        uint32_t needed = 0;
        if (page_model_.get_state(page_model_.context, page_id_, View(key), nullptr, 0, &needed) != PULSE_OK) return {};
        std::string value(needed, '\0');
        if (page_model_.get_state(page_model_.context, page_id_, View(key), value.data(), needed, &needed) != PULSE_OK) return {};
        while (!value.empty() && value.back() == '\0') value.pop_back();
        return value;
    }
    void PageStatus(uint32_t kind, std::string_view text = {}) {
        if (pages_ && page_id_) (void)pages_->SetStatus(page_id_, kind, text);
    }
    PulseStatus BindPageCommand(const std::string& command, std::function<void()> fn) {
        auto sub = std::make_unique<Subscription>();
        const auto status = sub->Command(page_model_, page_id_, command, [this, fn](uint64_t, PulseStringView) { if (active_ && fn) fn(); });
        if (status == PULSE_OK) page_subs_.push_back(std::move(sub));
        return status;
    }
    void CreatePage() {
        if (!page_spec_ || page_id_ || !pages_) return;
        const auto tree = PageTree();
        uint64_t id = 0;
        if (pages_->Create(Text(page_spec_->title), tree.Build(), id) != PULSE_OK) return;
        page_id_ = id;
        if (pages_->QueryModel(id, page_model_) != PULSE_OK || pages_->QueryEvents(id, page_events_) != PULSE_OK) { ClosePage(); return; }
        // Persist user (and picker) edits of bound keys.
        auto watch = std::make_unique<Subscription>();
        (void)watch->State(page_model_, page_events_, id, "", PULSE_UI_STATE_ALL, [this](uint64_t, const PulseUiStateBatch& batch) {
            if (!active_ || !page_spec_) return;
            for (uint32_t i = 0; batch.changes && i < batch.count; ++i) {
                const auto& c = batch.changes[i];
                const std::string key(c.key.data ? c.key.data : "", c.key.length);
                const std::string value(c.value.data ? c.value.data : "", c.value.length);
                const auto it = std::find_if(page_spec_->keys.begin(), page_spec_->keys.end(), [&](const auto& k) { return k.first == key; });
                if (it == page_spec_->keys.end() || settings_.Get(key, it->second) == value) continue;
                PageStatus(PULSE_SETTINGS_STATUS_SAVING);
                (void)settings_.Set(key, value, [this](PulseStatus status, std::string_view error) {
                    if (status == PULSE_OK) PageStatus(PULSE_SETTINGS_STATUS_SAVED);
                    else PageStatus(PULSE_SETTINGS_STATUS_ERROR, error);
                });
            }
        });
        page_subs_.push_back(std::move(watch));
        for (const auto& [command, key] : page_spec_->folders) {
            (void)BindPageCommand(command, [this, key = key] {
                if (!pickers_) return;
                const std::string initial = PageState(key);
                PulsePickFoldersRequest req{};
                req.size = sizeof(req);
                req.initial_path = View(initial);
                (void)pickers_->PickFolders(req, [this, key](const PickerResultData& res) {
                    if (active_ && res.outcome == PULSE_PICKER_ACCEPTED && !res.paths.empty()) (void)SetSettingsState(key, res.paths.front());
                });
            });
        }
        for (const auto& [command, fn] : page_handlers_) (void)BindPageCommand(command, fn);
        // Values changed elsewhere (another window, Reload) flow back into the page.
        settings_.OnChanged([this] {
            if (!page_id_ || !page_spec_) return;
            for (const auto& [key, fallback] : page_spec_->keys) {
                const auto want = settings_.Get(key, fallback);
                if (PageState(key) != want) (void)SetSettingsState(key, want);
            }
        });
    }
    void RebuildPage() {
        if (!page_id_ || !page_spec_ || !page_model_.update) return;
        const auto tree = PageTree();
        (void)page_model_.update(page_model_.context, page_id_, &tree.Build());
    }
    void ClosePage() noexcept {
        page_subs_.clear();
        if (pages_ && page_id_) { try { (void)pages_->Remove(page_id_); } catch (...) {} }
        page_id_ = 0;
        page_model_ = {};
        page_events_ = {};
    }

    Host host_;
    bool active_ = false;
    Language language_ = Language::ChineseSimplified;
    std::unique_ptr<DispatchQueue> queue_;
    PulseCommandsV1 commands_v1_{};
    PulseCommandsV2 commands_v2_{};
    bool has_v2_ = false;
    std::vector<std::unique_ptr<CommandEntry>> commands_;
    std::unique_ptr<TaskGroup> tasks_;
    std::optional<WorkbenchClient> workbench_;
    std::optional<PickerClient> pickers_;
    std::optional<IconsClient> icons_;
    std::optional<ColumnsClient> columns_;
    std::vector<std::pair<std::string, ColumnsClient::Registration>> column_regs_;
    std::optional<StatusBarClient> statusbar_;
    std::vector<std::pair<std::string, StatusBarClient::Item>> status_items_;
    std::optional<ContextClient> context_;
    std::vector<ContextClient::Subscription> context_subs_;
    std::optional<FileEventsClient> fileevents_;
    std::vector<FileEventsClient::Subscription> file_subs_;
    std::optional<PreviewClient> preview_;
    std::vector<std::pair<std::string, PreviewClient::Provider>> previews_;

    PulseStatus Watch(FolderContextFn fn) {
        if (!active_) return PULSE_ERROR_STATE;
        if (!context_) return PULSE_ERROR_UNAVAILABLE;
        ContextClient::Subscription sub;
        const PulseStatus st = context_->Subscribe(std::move(fn), sub);
        if (st == PULSE_OK) context_subs_.push_back(std::move(sub));
        return st;
    }
    WorkbenchClient::LocaleSubscription locale_sub_;
    DialogSession dialog_;
    SettingsStore settings_;
    std::optional<SettingsPagesClient> pages_;
    std::optional<SettingsPageSpec> page_spec_;
    std::vector<std::pair<std::string, std::function<void()>>> page_handlers_;
    std::vector<std::unique_ptr<Subscription>> page_subs_;
    uint64_t page_id_ = 0;
    PulseUiModelV1 page_model_{};
    PulseUiEventsV1 page_events_{};
};

inline PulseStatus CommandBuilder::Run(std::function<void(const Selection&)> handler) {
    return app_.Register(*this, std::move(handler));
}

namespace detail {
template<class T> PulseStatus PULSE_CALL AppActivate(void* p) noexcept {
    try { return static_cast<T*>(p)->StartInternal(); } catch (...) { return PULSE_ERROR_INITIALIZATION; }
}
template<class T> void PULSE_CALL AppDeactivate(void* p) noexcept { static_cast<T*>(p)->StopInternal(); }
template<class T> void PULSE_CALL AppDestroy(void* p) noexcept {
    auto* self = static_cast<T*>(p);
    self->StopInternal();
    delete self;
}
template<class T> PulseStatus AppEntry(const PulseHostV1* host, PulsePluginV1* out) noexcept {
    if (!host || !out || host->size < sizeof(*host) || out->size < sizeof(*out) ||
        host->version != PULSE_ABI_VERSION_1 || out->version != PULSE_ABI_VERSION_1)
        return PULSE_ERROR_ABI;
    try {
        auto* instance = new T(*host);
        *out = {sizeof(*out), PULSE_ABI_VERSION_1, instance, AppActivate<T>, AppDeactivate<T>, AppDestroy<T>};
        return PULSE_OK;
    } catch (...) { return PULSE_ERROR_INITIALIZATION; }
}
}  // namespace detail
}  // namespace pulse::sdk

// Defines the DLL entry points for an App subclass. Use once per plugin.
#define PULSE_APP(AppClass)                                                                                 \
    PULSE_PLUGIN_SDK_PREVIEW()                                                                              \
    extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host,     \
                                                                               PulsePluginV1* out) noexcept { \
        return ::pulse::sdk::detail::AppEntry<AppClass>(host, out);                                         \
    }
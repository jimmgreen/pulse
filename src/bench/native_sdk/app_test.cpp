// pulse::sdk::App (app.hpp) against an in-process fake host: entry/ABI checks,
// lifecycle and cleanup on failure, command registration / retitling on locale
// change / flags / invocation, previews, feedback clipping; plus the LocalText,
// Format and Selection helpers and a header generated from strings.tsv.
#include <pulse/app.hpp>
#include "app_strings.gen.hpp"
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pulse::sdk;
namespace {
int failures = 0, passes = 0;
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    value ? ++passes : ++failures;
}
std::string Str(PulseStringView v) { return v.data ? std::string(v.data, v.length) : std::string(); }
bool ValidUtf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k) if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

// ------------------------------------------------------------------ fake host
struct FakeHost {
    struct Cmd {
        std::string id, title, detail, group;
        uint32_t kinds = 0, min = 0, max = 0, flags = 0, exclude = 0, updates = 0;
        std::vector<std::string> extensions;
        PulseCommandFn invoke = nullptr;
        void* user = nullptr;
        bool live = true;
    };
    struct Prev { std::string extension; uint32_t flags = 0; bool live = true; };
    std::map<PulseCommandId, Cmd> commands;
    std::map<PulsePreviewProviderId, Prev> previews;
    PulseCommandId next_command = 100;
    PulsePreviewProviderId next_preview = 500;
    std::string language = "zh-CN";
    PulseWorkbenchLocaleFn locale_fn = nullptr;
    void* locale_user = nullptr;
    int locale_unsubscribes = 0;
    std::vector<std::pair<std::string, std::string>> snacks;
    bool offer = true;  // false: every interface is unavailable

    PulseHostV1 Host() { return {sizeof(PulseHostV1), PULSE_ABI_VERSION_1, this, &FakeHost::Query}; }
    size_t LiveCommands() const { size_t n = 0; for (const auto& [id, c] : commands) n += c.live ? 1 : 0; return n; }
    size_t LivePreviews() const { size_t n = 0; for (const auto& [id, p] : previews) n += p.live ? 1 : 0; return n; }
    Cmd* Find(std::string_view id) {
        for (auto& [key, c] : commands) if (c.live && c.id == id) return &c;
        return nullptr;
    }
    void FireLocale(const char* tag) {
        language = tag;
        PulseHostLocale l{};
        l.size = sizeof(l);
        std::snprintf(l.language_tag, sizeof(l.language_tag), "%s", tag);
        if (locale_fn) locale_fn(locale_user, &l);
    }
    static FakeHost& Self(void* ctx) { return *static_cast<FakeHost*>(ctx); }

    static void Copy(Cmd& c, const PulseCommandDescriptor& d) {
        c.title = Str(d.title);
        c.kinds = d.kinds; c.min = d.min_selection; c.max = d.max_selection;
        c.exclude = d.exclude_extensions;
        c.extensions.clear();
        for (uint32_t i = 0; i < d.extension_count; ++i) c.extensions.push_back(Str(d.extensions[i]));
        c.invoke = d.invoke; c.user = d.user;
        if (d.size > PULSE_COMMAND_DESCRIPTOR_SIZE_V1) {
            c.detail = Str(d.detail); c.group = Str(d.group); c.flags = d.flags;
        }
    }
    static PulseStatus PULSE_CALL Register(void* ctx, const PulseCommandDescriptor* d, PulseCommandId* out) {
        if (!d || !out || !d->id.length || !d->title.length || !d->invoke) return PULSE_ERROR_ARGUMENT;
        auto& self = Self(ctx);
        Cmd c; c.id = Str(d->id);
        Copy(c, *d);
        *out = self.next_command++;
        self.commands[*out] = std::move(c);
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Unregister(void* ctx, PulseCommandId id) {
        auto& self = Self(ctx);
        const auto it = self.commands.find(id);
        if (it == self.commands.end() || !it->second.live) return PULSE_ERROR_ARGUMENT;
        it->second.live = false;
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Update(void* ctx, PulseCommandId id, const PulseCommandDescriptor* d) {
        auto& self = Self(ctx);
        const auto it = self.commands.find(id);
        if (!d || it == self.commands.end() || !it->second.live) return PULSE_ERROR_ARGUMENT;
        Copy(it->second, *d);
        ++it->second.updates;
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL SetFlags(void* ctx, PulseCommandId id, uint32_t flags) {
        auto& self = Self(ctx);
        const auto it = self.commands.find(id);
        if (it == self.commands.end() || !it->second.live) return PULSE_ERROR_ARGUMENT;
        it->second.flags = flags;
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL AddProvider(void* ctx, const PulsePreviewProvider* p, PulsePreviewProviderId* out) {
        if (!p || !out || !p->callback) return PULSE_ERROR_ARGUMENT;
        auto& self = Self(ctx);
        Prev v; v.extension = Str(p->extension);
        v.flags = p->size >= offsetof(PulsePreviewProvider, flags) + sizeof(uint32_t) ? p->flags : 0;
        *out = self.next_preview++;
        self.previews[*out] = v;
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL RemoveProvider(void* ctx, PulsePreviewProviderId id) {
        auto& self = Self(ctx);
        const auto it = self.previews.find(id);
        if (it == self.previews.end() || !it->second.live) return PULSE_ERROR_ARGUMENT;
        it->second.live = false;
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL QueryLocale(void* ctx, PulseHostLocale* out) {
        if (!out) return PULSE_ERROR_ARGUMENT;
        *out = {};
        out->size = sizeof(*out);
        std::snprintf(out->language_tag, sizeof(out->language_tag), "%s", Self(ctx).language.c_str());
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL SubscribeLocale(void* ctx, PulseWorkbenchLocaleFn fn, void* user, PulseLocaleSubscription* out) {
        auto& self = Self(ctx);
        if (!fn || !out || self.locale_fn) return PULSE_ERROR_STATE;
        self.locale_fn = fn; self.locale_user = user;
        *out = 7;
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL UnsubscribeLocale(void* ctx, PulseLocaleSubscription id) {
        auto& self = Self(ctx);
        if (id != 7 || !self.locale_fn) return PULSE_ERROR_ARGUMENT;
        self.locale_fn = nullptr; self.locale_user = nullptr;
        ++self.locale_unsubscribes;
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL ShowSnack(void* ctx, const PulseSnackRequest* r, PulseWorkbenchActionFn, void*, PulseNotificationId* out) {
        if (!r) return PULSE_ERROR_ARGUMENT;
        Self(ctx).snacks.emplace_back(Str(r->text), Str(r->detail));
        if (out) *out = 1;
        return PULSE_OK;
    }
    template<class T> static PulseStatus Give(void* out, uint32_t out_size, const T& table) {
        if (out_size < sizeof(T)) return PULSE_ERROR_ABI;
        std::memcpy(out, &table, sizeof(T));
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Query(void* ctx, PulseStringView name, uint32_t version, void* out, uint32_t out_size) {
        auto& self = Self(ctx);
        const std::string n = Str(name);
        if (!self.offer) return PULSE_ERROR_UNAVAILABLE;
        if (n == PULSE_COMMANDS_INTERFACE_NAME && version == PULSE_COMMANDS_VERSION_2) {
            PulseCommandsV2 t{};
            t.size = sizeof(t); t.version = version; t.context = ctx;
            t.register_command = Register; t.unregister_command = Unregister;
            t.update_command = Update; t.set_command_flags = SetFlags;
            return Give(out, out_size, t);
        }
        if (n == PULSE_PREVIEW_INTERFACE_NAME && version == PULSE_PREVIEW_VERSION_1) {
            PulsePreviewV1 t{};
            t.size = sizeof(t); t.version = version; t.context = ctx;
            t.register_provider = AddProvider; t.unregister_provider = RemoveProvider;
            return Give(out, out_size, t);
        }
        if (n == PULSE_WORKBENCH_INTERFACE_NAME && version == PULSE_WORKBENCH_VERSION_3) {
            PulseWorkbenchV3 t{};
            t.size = sizeof(t); t.version = version; t.context = ctx;
            t.show_snack = ShowSnack;
            t.query_locale = QueryLocale; t.subscribe_locale = SubscribeLocale; t.unsubscribe_locale = UnsubscribeLocale;
            return Give(out, out_size, t);
        }
        return PULSE_ERROR_UNAVAILABLE;
    }
};

// --------------------------------------------------------------- probe plugin
enum class Mode { Normal, Throw, Fail };
Mode g_mode = Mode::Normal;
int g_starts = 0, g_stops = 0, g_language_changes = 0, g_invoked = 0;
PulseStatus g_preview_status = PULSE_ERROR_STATE, g_bg_status = PULSE_ERROR_STATE;
Selection g_last;
class Probe;
Probe* g_probe = nullptr;

class Probe : public App {
public:
    explicit Probe(const PulseHostV1& host) : App(host) { g_probe = this; }
    ~Probe() override { if (g_probe == this) g_probe = nullptr; }
    LocalText Name() const override { return Tr("探针", "探針", "Probe"); }
    PulseStatus OnStart() override {
        ++g_starts;
        const PulseStatus st = Command("tidy", app_strings::Hello())
            .Detail(app_strings::Moved("{stem}", "D:\\out"))
            .Group(Tr("整理", "整理", "Tidy"), true)
            .Files(1, 8).Extensions({".txt", ".md"})
            .Toolbar().Primary()
            .Run([](const Selection& s) { ++g_invoked; g_last = s; });
        if (st != PULSE_OK) return st;
        g_bg_status = Command("bg", "背景").Background().Hidden().Run([](const Selection&) {});
        g_preview_status = AddPreview(".probe", [](const PreviewContext&) { return PULSE_OK; },
                                      PULSE_PREVIEW_PROVIDER_THUMBNAIL);
        if (g_mode == Mode::Throw) throw std::runtime_error("boom");
        return g_mode == Mode::Fail ? PULSE_ERROR_LOAD : PULSE_OK;
    }
    void OnStop() override { ++g_stops; }
    void OnLanguageChanged() override { ++g_language_changes; }
    using App::Text;
};

void ResetCounters(Mode mode) {
    g_mode = mode;
    g_starts = g_stops = g_language_changes = g_invoked = 0;
    g_preview_status = g_bg_status = PULSE_ERROR_STATE;
    g_last = {};
}

struct Loaded {
    PulsePluginV1 plugin{sizeof(PulsePluginV1), PULSE_ABI_VERSION_1, nullptr, nullptr, nullptr, nullptr};
    PulseStatus entry = PULSE_ERROR_STATE;
    explicit Loaded(const PulseHostV1& host) { entry = detail::AppEntry<Probe>(&host, &plugin); }
    PulseStatus Activate() { return plugin.activate(plugin.instance); }
    void Deactivate() { plugin.deactivate(plugin.instance); }
    ~Loaded() { if (entry == PULSE_OK && plugin.destroy) plugin.destroy(plugin.instance); }
};

// ---------------------------------------------------------------------- tests
void TestHelpers() {
    const LocalText full = Tr("简", "繁", "En");
    const LocalText zh_only = Tr("只有中文", "", "");
    Check(full.In(Language::ChineseSimplified) == "简" && full.In(Language::ChineseTraditional) == "繁" &&
          full.In(Language::English) == "En", "LocalText picks each language");
    Check(zh_only.In(Language::ChineseTraditional) == "只有中文" && zh_only.In(Language::English) == "只有中文",
          "LocalText falls back to Simplified Chinese");
    Check(LocalText("same").In(Language::English) == "same" && LocalText("same").In(Language::ChineseTraditional) == "same",
          "single-string LocalText is used everywhere");
    Check(LocalText().Empty() && !zh_only.Empty(), "LocalText::Empty");
    Check(LanguageFromTag("en-US") == Language::English && LanguageFromTag("ja-JP") == Language::English &&
          LanguageFromTag("zh-CN") == Language::ChineseSimplified && LanguageFromTag("zh-TW") == Language::ChineseTraditional &&
          LanguageFromTag("zh-Hant-HK") == Language::ChineseTraditional && LanguageFromTag("zh-HK") == Language::ChineseTraditional,
          "LanguageFromTag maps host tags");
    Check(Format("{0} + {1} = {0}{1}", {"a", "b"}) == "a + b = ab", "Format substitutes and repeats arguments");
    Check(Format("{2} {x} {", {"a"}) == "{2} {x} {", "Format leaves unknown placeholders untouched");
    const SelectedItem file{"C:\\d\\report.final.txt", "report.final.txt", false};
    const SelectedItem dot{"C:\\d\\.bashrc", ".bashrc", false};
    const SelectedItem dir{"C:\\d\\pkg.v2", "pkg.v2", true};
    Check(file.Stem() == "report.final" && file.Extension() == ".txt", "SelectedItem stem/extension of a file");
    Check(dot.Stem() == ".bashrc" && dot.Extension().empty(), "dotfile has no extension");
    Check(dir.Stem() == "pkg.v2" && dir.Extension().empty(), "folders keep their full name");
}

void TestGeneratedStrings() {
    Check(app_strings::Hello().In(Language::ChineseSimplified) == "你好" &&
          app_strings::Hello().In(Language::ChineseTraditional) == "妳好" &&
          app_strings::Hello().In(Language::English) == "Hello", "generated accessor has all three languages");
    const auto moved = app_strings::Moved("3", "D:\\out");
    Check(moved.In(Language::ChineseSimplified) == "已移动 3 个文件到 D:\\out" &&
          moved.In(Language::ChineseTraditional) == "已移動 3 個檔案到 D:\\out" &&
          moved.In(Language::English) == "Moved 3 files to D:\\out", "generated accessor formats its arguments");
    Check(app_strings::OnlyChinese().In(Language::English) == "仅中文" &&
          app_strings::OnlyChinese().In(Language::ChineseTraditional) == "仅中文", "generated empty columns fall back");
    Check(app_strings::Tricky().In(Language::ChineseSimplified) == "引号\"、反斜杠\\、换行\n、问号?\?=" &&
          app_strings::Tricky().In(Language::English) == "quote\" back\\slash\tTab?\?=", "generated literals keep escapes");
}

void TestEntryChecks() {
    FakeHost fake;
    PulseHostV1 host = fake.Host();
    PulsePluginV1 plugin{sizeof(PulsePluginV1), PULSE_ABI_VERSION_1, nullptr, nullptr, nullptr, nullptr};
    host.size = sizeof(PulseHostV1) - 1;
    Check(detail::AppEntry<Probe>(&host, &plugin) == PULSE_ERROR_ABI && !plugin.instance, "short host table is rejected");
    host = fake.Host();
    host.version = PULSE_ABI_VERSION_1 + 1;
    Check(detail::AppEntry<Probe>(&host, &plugin) == PULSE_ERROR_ABI, "unknown host ABI version is rejected");
    host = fake.Host();
    plugin.version = PULSE_ABI_VERSION_1 + 1;
    Check(detail::AppEntry<Probe>(&host, &plugin) == PULSE_ERROR_ABI, "unknown plugin table version is rejected");
    Check(detail::AppEntry<Probe>(nullptr, &plugin) == PULSE_ERROR_ABI, "null host is rejected");
}

void TestLifecycle() {
    ResetCounters(Mode::Normal);
    FakeHost fake;
    const PulseHostV1 host = fake.Host();
    {
        Loaded loaded(host);
        Check(loaded.entry == PULSE_OK && loaded.plugin.instance && loaded.plugin.activate && loaded.plugin.deactivate &&
              loaded.plugin.destroy, "entry fills the plugin table");
        Check(fake.commands.empty() && g_starts == 0, "nothing registers before activation");
        Check(loaded.Activate() == PULSE_OK && g_starts == 1, "activation runs OnStart once");
        Check(fake.LiveCommands() == 2 && g_bg_status == PULSE_OK, "both commands registered");
        auto* tidy = fake.Find("tidy");
        auto* bg = fake.Find("bg");
        Check(tidy && tidy->title == "你好" && tidy->detail == "已移动 {stem} 个文件到 D:\\out" && tidy->group == "整理",
              "descriptor text uses the host language (zh-CN)");
        Check(tidy && tidy->kinds == PULSE_COMMAND_FILES && tidy->min == 1 && tidy->max == 8 &&
              tidy->extensions == std::vector<std::string>{".txt", ".md"} && tidy->exclude == 0,
              "descriptor selection rules");
        Check(tidy && (tidy->flags & PULSE_COMMAND_TOOLBAR) && (tidy->flags & PULSE_COMMAND_PRIMARY) &&
              (tidy->flags & PULSE_COMMAND_GROUP_SUBMENU), "descriptor flags (toolbar, primary, submenu)");
        Check(bg && bg->kinds == PULSE_COMMAND_BACKGROUND && bg->min == 0 && (bg->flags & PULSE_COMMAND_HIDDEN) &&
              bg->title == "背景", "background command descriptor");
        Check(g_preview_status == PULSE_OK && fake.LivePreviews() == 1 && fake.previews.begin()->second.extension == ".probe" &&
              fake.previews.begin()->second.flags == PULSE_PREVIEW_PROVIDER_THUMBNAIL, "AddPreview registers a thumbnail provider");
        Check(fake.locale_fn != nullptr && g_probe && g_probe->CurrentLanguage() == Language::ChineseSimplified,
              "locale subscribed, starts in Simplified Chinese");

        fake.FireLocale("en-US");
        Check(tidy && tidy->title == "Hello" && tidy->detail == "Moved {stem} files to D:\\out" && tidy->group == "Tidy" &&
              tidy->updates == 1, "locale change retitles commands");
        Check(g_language_changes == 1 && g_probe->CurrentLanguage() == Language::English && g_probe->Text(Tr("中", "中", "en")) == "en",
              "OnLanguageChanged and Text follow the host");
        Check(tidy && (tidy->flags & PULSE_COMMAND_TOOLBAR) && (tidy->flags & PULSE_COMMAND_GROUP_SUBMENU),
              "retitling keeps descriptor flags");
        fake.FireLocale("en-GB");
        Check(g_language_changes == 1 && tidy && tidy->updates == 1, "same language: no retitle");
        fake.FireLocale("zh-TW");
        Check(tidy && tidy->title == "妳好" && g_language_changes == 2, "Traditional Chinese");

        auto* handle = g_probe->FindCommand("bg");
        Check(handle && handle->Id() != 0 && g_probe->FindCommand("missing") == nullptr, "FindCommand");
        Check(handle && handle->SetHidden(false) == PULSE_OK && bg && !(bg->flags & PULSE_COMMAND_HIDDEN), "SetHidden(false)");
        Check(handle && handle->SetChecked(true) == PULSE_OK && bg && (bg->flags & PULSE_COMMAND_CHECKED) &&
              !(bg->flags & PULSE_COMMAND_HIDDEN), "SetChecked keeps the other flags");

        const char* dir = "C:\\work";
        const PulseContextSelectionEntry entries[] = {
            {sizeof(PulseContextSelectionEntry), 0, {"C:\\work\\a.txt", 13}, {"a.txt", 5}},
            {sizeof(PulseContextSelectionEntry), PULSE_CONTEXT_ENTRY_DIRECTORY, {"C:\\work\\sub", 11}, {"sub", 3}},
        };
        const PulseContextSelectionSnapshot snapshot{sizeof(snapshot), 42, {dir, 7}, entries, 2};
        if (tidy && tidy->invoke) tidy->invoke(tidy->user, 100, &snapshot);
        Check(g_invoked == 1 && g_last.directory == "C:\\work" && g_last.revision == 42 && g_last.Count() == 2 &&
              g_last.Files() == 1 && g_last.Folders() == 1 && g_last.First().name == "a.txt" && g_last.items[1].directory,
              "command handler gets an owned selection copy");

        const std::string long_ascii(300, 'x');
        std::string long_chinese;
        for (int i = 0; i < 100; ++i) long_chinese += "汉";
        Check(g_probe->Toast(long_ascii, Tone::Info, long_chinese) == PULSE_OK && fake.snacks.size() == 1, "Toast reaches the host");
        Check(!fake.snacks.empty() && fake.snacks[0].first.size() == 256 && fake.snacks[0].second.size() <= 256 &&
              fake.snacks[0].second.size() % 3 == 0 && ValidUtf8(fake.snacks[0].second),
              "Toast clips to 256 bytes on a UTF-8 boundary");

        loaded.Deactivate();
        Check(g_stops == 1 && fake.LiveCommands() == 0 && fake.LivePreviews() == 0 && fake.locale_unsubscribes == 1,
              "deactivation runs OnStop and releases commands, previews and the locale subscription");
        Check(g_probe && g_probe->Command("late", "迟到").Files().Run([](const Selection&) {}) == PULSE_ERROR_STATE &&
              fake.LiveCommands() == 0, "registering after stop is refused");
        Check(g_probe && g_probe->AddPreview(".late", [](const PreviewContext&) { return PULSE_OK; }) == PULSE_ERROR_STATE &&
              fake.LivePreviews() == 0, "previews after stop are refused");
        Check(g_probe && g_probe->AddStatusItem(StatusItem{}) == PULSE_ERROR_STATE &&
              g_probe->OnFolderChange([](const FolderContext&) {}) == PULSE_ERROR_STATE &&
              g_probe->OnFileOperation([](const FileEvent&) {}) == PULSE_ERROR_STATE,
              "status items and event hooks after stop are refused");
        loaded.Deactivate();
        Check(g_stops == 1, "a second deactivate does not run OnStop again");
    }
    Check(g_probe == nullptr, "destroy deletes the instance");
}

void TestFailureCleanup(Mode mode, PulseStatus expected, const char* label) {
    ResetCounters(mode);
    FakeHost fake;
    {
        Loaded loaded(fake.Host());
        const PulseStatus status = loaded.Activate();
        std::string text = std::string(label) + ": activate reports the failure";
        Check(status == expected, text.c_str());
        text = std::string(label) + ": everything registered so far is released";
        Check(fake.commands.size() == 2 && fake.LiveCommands() == 0 && fake.previews.size() == 1 && fake.LivePreviews() == 0 &&
              fake.locale_fn == nullptr, text.c_str());
        text = std::string(label) + ": OnStop runs once";
        Check(g_stops == 1, text.c_str());
    }
    std::string text = std::string(label) + ": destroy after a failed start";
    Check(g_probe == nullptr && g_stops == 1, text.c_str());
}

void TestNoInterfaces() {
    ResetCounters(Mode::Normal);
    FakeHost fake;
    fake.offer = false;
    Loaded loaded(fake.Host());
    Check(loaded.entry == PULSE_OK, "entry works without host interfaces");
    Check(loaded.Activate() == PULSE_ERROR_UNAVAILABLE && fake.commands.empty(), "commands without pulse.commands: UNAVAILABLE");
    Check(g_probe && g_probe->CurrentLanguage() == Language::ChineseSimplified, "no workbench: Simplified Chinese default");
    Check(g_probe && g_probe->Toast("x") == PULSE_ERROR_UNAVAILABLE, "Toast without workbench: UNAVAILABLE");
}
}  // namespace

int main() {
    TestHelpers();
    TestGeneratedStrings();
    TestEntryChecks();
    TestLifecycle();
    TestFailureCleanup(Mode::Throw, PULSE_ERROR_INITIALIZATION, "OnStart throws");
    TestFailureCleanup(Mode::Fail, PULSE_ERROR_LOAD, "OnStart fails");
    TestNoInterfaces();
    std::printf("%d PASS, %d FAIL\n", passes, failures);
    return failures ? 1 : 0;
}

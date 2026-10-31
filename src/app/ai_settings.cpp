#include "ai_settings.h"
#include "../common/json_value.h"
#include "../common/utf8_file.h"
#include <windows.h>

namespace pulse::app::ai {
namespace {

// assistant.thinking (ai_providers.h ThinkLevel, kept apart so this file
// needs nothing above it).
constexpr const char* kThinkingNames[] = {"off", "minimal", "low", "medium", "high", "xhigh", "max"};
int ClampThinking(int level) { return level < 0 ? 0 : level > 6 ? 6 : level; }
int ThinkingFromName(const std::string& name) {
    for (int i = 0; i < 7; ++i)
        if (name == kThinkingNames[i]) return i;
    return -1;
}
// "effort" meant what the service sent: DeepSeek's 0..3 were off, low, high
// and max; the others' off, low, medium and high.
bool DeepSeekService(const std::string& service) { return service.empty() || service == "deepseek"; }
int ThinkingFromEffort(int effort, const std::string& service) {
    static constexpr int kDeepSeek[] = {0, 2, 4, 6}, kOthers[] = {0, 2, 3, 4};
    effort = effort < 0 ? 0 : effort > 3 ? 3 : effort;
    return DeepSeekService(service) ? kDeepSeek[effort] : kOthers[effort];
}
int EffortFromThinking(int level, const std::string& service) {
    static constexpr int kDeepSeek[] = {0, 1, 1, 2, 2, 3, 3}, kOthers[] = {0, 1, 1, 2, 3, 3, 3};
    level = ClampThinking(level);
    return DeepSeekService(service) ? kDeepSeek[level] : kOthers[level];
}


constexpr size_t kMaxClients = 64, kMaxRules = 256, kMaxName = 128;
constexpr size_t kMaxServices = 32;

// An address ai.json may carry: http(s), at most 512 bytes.
bool UsableUrl(std::string_view url) {
    return url.size() <= 512 && (url.starts_with("https://") || url.starts_with("http://"));
}

// A model service id (ai_providers.h): lower-case letters, digits, '-' and '_'.
bool UsableServiceId(std::string_view id) {
    if (id.empty() || id.size() > 32) return false;
    for (const char ch : id)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return false;
    return true;
}

std::wstring LocalAppData() {
    wchar_t root[32768];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", root, static_cast<DWORD>(std::size(root)));
    return n && n < std::size(root) ? std::wstring(root, n) : std::wstring{};
}

const char* GrantName(ClientGrant grant) {
    switch (grant) {
    case ClientGrant::Allowed: return "allowed";
    case ClientGrant::Denied: return "denied";
    default: return "ask";   // Session approvals are not remembered
    }
}

ClientGrant GrantFrom(const std::string& name) {
    if (name == "allowed") return ClientGrant::Allowed;
    if (name == "denied") return ClientGrant::Denied;
    return ClientGrant::Ask;
}

// Absolute, "." and ".." resolved lexically, no trailing separator (except a
// drive root). Empty for relative or unusable paths.
std::wstring NormalizeFolder(std::wstring_view folder) {
    if (folder.size() < 3 || folder.size() > 32000) return {};
    std::wstring in(folder);
    for (auto& c : in) if (c == L'/') c = L'\\';
    const bool drive = in.size() >= 3 && in[1] == L':' && in[2] == L'\\';
    const bool unc = in.rfind(L"\\\\", 0) == 0 && in.rfind(L"\\\\?\\", 0) != 0 && in.rfind(L"\\\\.\\", 0) != 0;
    if (!drive && !unc) return {};
    std::wstring out(32768, L'\0');
    const DWORD n = GetFullPathNameW(in.c_str(), static_cast<DWORD>(out.size()), out.data(), nullptr);
    if (!n || n >= out.size()) return {};
    out.resize(n);
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}

bool SameText(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

std::wstring AiDirectory() {
    const std::wstring root = LocalAppData();
    return root.empty() ? std::wstring{} : root + L"\\Pulse\\ai";
}

std::wstring SettingsPath() {
    const std::wstring dir = AiDirectory();
    return dir.empty() ? std::wstring{} : dir + L"\\ai.json";
}

std::wstring ActivityDirectory() {
    const std::wstring dir = AiDirectory();
    return dir.empty() ? std::wstring{} : dir + L"\\activity";
}

Settings ParseSettings(std::string_view utf8, bool* malformed) {
    Settings s;
    json::Value root;
    if (!json::Parse(utf8, root) || !root.is_object()) {
        if (malformed) *malformed = true;
        return s;
    }
    if (malformed) *malformed = false;
    s.enabled = root.get_bool("enabled", true);
    s.mcp_enabled = root.get_bool("mcp_enabled", false);
    if (const auto* days = root.find("log_days")) {
        int64_t n = 0;
        if (days->get_int(n, 1, 365)) s.log_days = static_cast<int>(n);
    }
    if (const auto* clients = root.find("clients"); clients && clients->is_array()) {
        for (const auto& c : clients->items()) {
            if (s.clients.size() >= kMaxClients) break;
            const std::wstring name = json::Wide(c.get_string("name"));
            if (name.empty() || name.size() > kMaxName) continue;
            // An older file's "last_call" (never filled in) is dropped on the next save.
            s.clients.push_back({name, GrantFrom(c.get_string("grant"))});
        }
    }
    if (const auto* allow = root.find("allow"); allow && allow->is_array()) {
        for (const auto& a : allow->items()) {
            if (s.allow.size() >= kMaxRules) break;
            AllowRule rule{json::Wide(a.get_string("client")), a.get_string("tool"),
                           NormalizeFolder(json::Wide(a.get_string("folder")))};
            if (rule.client.empty() || rule.client.size() > kMaxName || rule.tool.empty() || rule.tool.size() > kMaxName ||
                rule.folder.empty())
                continue;
            s.allow.push_back(std::move(rule));
        }
    }
    if (const auto* groups = root.find("mcp_allow"); groups && groups->is_array()) {
        s.mcp_allow_set = true;
        for (const auto& g : groups->items()) {
            if (!g.is_string()) continue;
            const std::string& key = g.as_string();
            if (key.empty() || key.size() > 32 || key == "locked" || s.mcp_allow.size() >= 16) continue;
            bool seen = false;
            for (const auto& have : s.mcp_allow) seen |= have == key;
            if (!seen) s.mcp_allow.push_back(key);
        }
    }
    if (const auto* a = root.find("assistant"); a && a->is_object()) {
        s.assistant.floating = a->get_string("dock") == "floating";
        const std::string model = a->get_string("model");
        if (!model.empty() && model.size() <= kMaxName) s.assistant.model = model;
        int64_t effort = -1;
        if (const auto* e = a->find("effort"); !e || !e->get_int(effort, 0, 3)) effort = -1;
        const int thinking = ThinkingFromName(a->get_string("thinking"));
        const std::string base_url = a->get_string("base_url");
        if (UsableUrl(base_url)) s.assistant.base_url = base_url;
        if (const std::string service = a->get_string("service"); UsableServiceId(service)) s.assistant.service = service;
        s.assistant.history = a->get_bool("history", true);
        if (int64_t days = 0; a->find("history_days") && a->find("history_days")->get_int(days, 0, 3650))
            s.assistant.history_days = static_cast<int>(days);
        if (thinking >= 0) s.assistant.thinking = thinking;
        else if (effort >= 0) s.assistant.thinking = ThinkingFromEffort(static_cast<int>(effort), s.assistant.service);
        if (const auto* list = a->find("services"); list && list->is_object()) {
            for (size_t i = 0; i < list->size() && s.assistant.services.size() < kMaxServices; ++i) {
                const std::string& id = list->keys()[i];
                const json::Value& item = list->at(i);
                if (!UsableServiceId(id) || !item.is_object()) continue;
                ServicePrefs sp;
                if (const std::string m = item.get_string("model"); m.size() <= kMaxName) sp.model = m;
                if (const std::string u = item.get_string("base_url"); UsableUrl(u)) sp.base_url = u;
                bool seen = false;
                for (const auto& have : s.assistant.services) seen |= have.first == id;
                if (!seen) s.assistant.services.emplace_back(id, std::move(sp));
            }
        }
        if (const auto* v = a->find("voice"); v && v->is_object()) {
            VoicePrefs& voice = s.assistant.voice;
            const std::string service = v->get_string("service");
            if (service == "siliconflow" || service == "openai" || service == "custom") voice.service = service;
            const std::string url = v->get_string("base_url");
            if (url.size() <= 512 && (url.starts_with("https://") || url.starts_with("http://"))) voice.base_url = url;
            const std::string voice_model = v->get_string("model");
            if (voice_model.size() <= kMaxName) voice.model = voice_model;
            if (const auto* pause = v->find("pause")) {
                int64_t n = 0;
                if (pause->get_int(n, 0, 2)) voice.pause = static_cast<int>(n);
            }
            const std::string mic = v->get_string("mic");
            if (mic.size() <= 512) voice.mic = mic;
        }
    }
    return s;
}

Settings LoadSettings(bool* malformed) {
    if (malformed) *malformed = false;
    const std::wstring path = SettingsPath();
    if (path.empty()) return {};
    const DWORD attributes = GetFileAttributesW(path.c_str());
    const DWORD attribute_error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    const bool missing = attributes == INVALID_FILE_ATTRIBUTES &&
        (attribute_error == ERROR_FILE_NOT_FOUND || attribute_error == ERROR_PATH_NOT_FOUND);
    if (missing) {
        Settings defaults;
        wchar_t executable[32768]{};
        const DWORD count = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        if (count && count < std::size(executable)) {
            std::wstring ini(executable, count);
            const size_t slash = ini.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                ini.resize(slash + 1);
                ini += L"setup-defaults.ini";
                defaults.enabled = GetPrivateProfileIntW(L"features", L"ai_assistant", 1, ini.c_str()) != 0;
            }
        }
        return defaults;
    }
    std::wstring text;
    if (!ReadUtf8File(path, text)) {
        if (malformed) *malformed = true;
        return {};
    }
    return ParseSettings(json::Utf8(text), malformed);
}

std::string SerializeSettings(const Settings& s) {
    json::Value root = json::Value::Object();
    if (!s.enabled) root.set("enabled", false);
    root.set("mcp_enabled", s.mcp_enabled);
    root.set("log_days", s.log_days);
    json::Value clients = json::Value::Array();
    for (const auto& c : s.clients) {
        json::Value v = json::Value::Object();
        v.set("name", json::Utf8(c.name));
        v.set("grant", GrantName(c.grant));
        clients.push(std::move(v));
    }
    root.set("clients", std::move(clients));
    json::Value allow = json::Value::Array();
    for (const auto& a : s.allow) {
        json::Value v = json::Value::Object();
        v.set("client", json::Utf8(a.client));
        v.set("tool", a.tool);
        v.set("folder", json::Utf8(a.folder));
        allow.push(std::move(v));
    }
    root.set("allow", std::move(allow));
    if (s.mcp_allow_set) {
        json::Value groups = json::Value::Array();
        for (const auto& g : s.mcp_allow) groups.push(g);
        root.set("mcp_allow", std::move(groups));
    }
    json::Value assistant = json::Value::Object();
    assistant.set("dock", s.assistant.floating ? "floating" : "docked");
    // Only once another service was chosen, so older files stay as they are.
    if (!s.assistant.service.empty()) assistant.set("service", s.assistant.service);
    assistant.set("model", s.assistant.model);
    assistant.set("thinking", kThinkingNames[ClampThinking(s.assistant.thinking)]);
    assistant.set("effort", EffortFromThinking(s.assistant.thinking, s.assistant.service));
    if (!s.assistant.base_url.empty()) assistant.set("base_url", s.assistant.base_url);
    if (!s.assistant.services.empty()) {
        json::Value list = json::Value::Object();
        for (const auto& [id, sp] : s.assistant.services) {
            json::Value item = json::Value::Object();
            if (!sp.model.empty()) item.set("model", sp.model);
            if (!sp.base_url.empty()) item.set("base_url", sp.base_url);
            list.set(id, std::move(item));
        }
        assistant.set("services", std::move(list));
    }
    if (const VoicePrefs& vp = s.assistant.voice; !vp.IsDefault()) {
        json::Value voice = json::Value::Object();
        if (!vp.service.empty()) voice.set("service", vp.service);
        if (!vp.base_url.empty()) voice.set("base_url", vp.base_url);
        if (!vp.model.empty()) voice.set("model", vp.model);
        voice.set("pause", vp.pause);
        if (!vp.mic.empty()) voice.set("mic", vp.mic);
        assistant.set("voice", std::move(voice));
    }
    if (!s.assistant.history) assistant.set("history", false);
    if (s.assistant.history_days > 0) assistant.set("history_days", s.assistant.history_days);
    root.set("assistant", std::move(assistant));
    return json::Serialize(root) + "\n";
}

bool SaveSettings(const Settings& settings) {
    const std::wstring dir = AiDirectory();
    if (dir.empty()) { SetLastError(ERROR_PATH_NOT_FOUND); return false; }
    const std::wstring pulse_dir = dir.substr(0, dir.find_last_of(L'\\'));
    CreateDirectoryW(pulse_dir.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    return WriteUtf8FileAtomic(SettingsPath(), json::Wide(SerializeSettings(settings)));
}

bool McpAllows(const Settings& settings, std::string_view group) {
    if (group.empty() || group == "locked") return false;
    if (!settings.mcp_allow_set) {
        for (const char* key : kMcpDefaultAllow)
            if (group == key) return true;
        return false;
    }
    for (const auto& have : settings.mcp_allow)
        if (have == group) return true;
    return false;
}

void SetMcpAllow(Settings& settings, std::string_view group, bool on) {
    if (group.empty() || group == "locked") return;
    if (!settings.mcp_allow_set) {
        settings.mcp_allow_set = true;
        settings.mcp_allow.clear();
        for (const char* key : kMcpDefaultAllow) settings.mcp_allow.emplace_back(key);
    }
    std::erase_if(settings.mcp_allow, [&](const std::string& have) { return have == group; });
    if (on) settings.mcp_allow.emplace_back(group);
}

bool IsAlwaysAllowed(const Settings& settings, PermissionClass cls, std::wstring_view client, std::string_view tool,
                     std::wstring_view folder) {
    if (cls != PermissionClass::Create || client.empty() || tool.empty()) return false;
    const std::wstring target = NormalizeFolder(folder);
    if (target.empty()) return false;
    for (const auto& rule : settings.allow) {
        if (rule.tool != tool || !SameText(rule.client, client)) continue;
        if (SameText(rule.folder, target)) return true;
        const bool root = rule.folder.size() == 3;   // "C:\" already ends with the separator
        const size_t prefix = rule.folder.size() + (root ? 0 : 1);
        if (target.size() > prefix && SameText(std::wstring_view(target).substr(0, rule.folder.size()), rule.folder) &&
            (root || target[rule.folder.size()] == L'\\'))
            return true;
    }
    return false;
}

} // namespace pulse::app::ai

// desktop_ai_organizer.cpp - Native AI Desktop Organizer integrated directly with
// Pulse's built-in AI Assistant (%LOCALAPPDATA%\Pulse\ai\ai.json + DPAPI model_key*.bin).
#include "host/ai/desktop_ai_organizer.h"

#include <dpapi.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace pulse::desktop::ai {
namespace {

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16 & Case-Insensitive Helpers
// ---------------------------------------------------------------------------
std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(std::string_view utf8, bool strict = false) {
    if (utf8.empty()) return {};
    const DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    const int needed = MultiByteToWideChar(CP_UTF8, flags, utf8.data(), static_cast<int>(utf8.size()),
                                           nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, flags, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), needed);
    return out;
}

std::wstring ToLowerWide(std::wstring_view s) {
    std::wstring out(s);
    for (wchar_t& ch : out) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        } else {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
    }
    return out;
}

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

bool EqualsIgnoreCaseW(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool ContainsIgnoreCaseW(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty()) return true;
    const std::wstring h = ToLowerWide(haystack);
    const std::wstring n = ToLowerWide(needle);
    return h.find(n) != std::wstring::npos;
}

std::wstring TrimWide(std::wstring_view s) {
    size_t start = 0;
    while (start < s.size() && std::iswspace(s[start])) ++start;
    size_t end = s.size();
    while (end > start && std::iswspace(s[end - 1])) --end;
    return std::wstring(s.substr(start, end - start));
}

std::wstring JoinPath(std::wstring_view dir, std::wstring_view leaf) {
    if (dir.empty()) return std::wstring(leaf);
    std::wstring out(dir);
    while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) {
        if (out.size() == 3 && out[1] == L':') break;
        out.pop_back();
    }
    if (!out.empty() && out.back() != L'\\' && out.back() != L'/') {
        out.push_back(L'\\');
    }
    out.append(leaf);
    return out;
}

bool PathExistsOnDisk(const std::wstring& path) noexcept {
    if (path.empty()) return false;
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectoryOnDisk(const std::wstring& path) noexcept {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int64_t CurrentUnixMs() noexcept {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;
    if (uli.QuadPart <= kEpochDiff100ns) return 0;
    return static_cast<int64_t>((uli.QuadPart - kEpochDiff100ns) / 10000ULL);
}

void WipeWideString(std::wstring& s) noexcept {
    if (!s.empty()) {
        SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
        s.clear();
    }
}

struct ScopedWideSecretWipe {
    std::wstring& secret;
    ~ScopedWideSecretWipe() noexcept { WipeWideString(secret); }
};

// ---------------------------------------------------------------------------
// Self-Contained Strict JSON Parser & Serializer
// ---------------------------------------------------------------------------
void AppendUtf8Codepoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

std::string JsonQuoteUtf8(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    out.push_back('"');
    for (unsigned char ch : s) {
        switch (ch) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20u) {
                char buf[8]{};
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(ch));
                out += buf;
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

struct JsonVal {
    enum class Kind : uint8_t { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool bool_val = false;
    double num_val = 0.0;
    std::string str_val;
    std::vector<JsonVal> arr_val;
    std::vector<std::pair<std::string, JsonVal>> obj_val;

    bool is_object() const noexcept { return kind == Kind::Object; }
    bool is_array() const noexcept { return kind == Kind::Array; }
    bool is_string() const noexcept { return kind == Kind::String; }
    bool is_bool() const noexcept { return kind == Kind::Bool; }

    const JsonVal* find(std::string_view key) const noexcept {
        if (kind != Kind::Object) return nullptr;
        for (const auto& [k, v] : obj_val) {
            if (k == key) return &v;
        }
        return nullptr;
    }

    std::string get_string(std::string_view key, std::string_view fallback = {}) const {
        const JsonVal* v = find(key);
        return (v && v->is_string()) ? v->str_val : std::string(fallback);
    }

    bool get_bool(std::string_view key, bool fallback = false) const noexcept {
        const JsonVal* v = find(key);
        return (v && v->is_bool()) ? v->bool_val : fallback;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view src) noexcept : src_(src) {}

    bool ParseRoot(JsonVal& out) {
        SkipWs();
        if (!ParseValue(out, 0)) return false;
        SkipWs();
        return pos_ == src_.size();
    }

private:
    std::string_view src_;
    size_t pos_ = 0;

    void SkipWs() noexcept {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool MatchLiteral(std::string_view lit) noexcept {
        if (src_.substr(pos_, lit.size()) != lit) return false;
        pos_ += lit.size();
        return true;
    }

    bool ParseHex4(uint32_t& out) noexcept {
        if (pos_ + 4 > src_.size()) return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = src_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        out = v;
        return true;
    }

    bool ParseString(std::string& out) {
        if (pos_ >= src_.size() || src_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            const unsigned char ch = static_cast<unsigned char>(src_[pos_++]);
            if (ch == '"') return true;
            if (ch < 0x20u) return false;
            if (ch != '\\') {
                out.push_back(static_cast<char>(ch));
                continue;
            }
            if (pos_ >= src_.size()) return false;
            const char esc = src_[pos_++];
            switch (esc) {
            case '"':  out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/'); break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                uint32_t u1 = 0;
                if (!ParseHex4(u1)) return false;
                if (u1 >= 0xD800u && u1 <= 0xDBFFu) {
                    if (pos_ + 2 <= src_.size() && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        uint32_t u2 = 0;
                        if (!ParseHex4(u2) || u2 < 0xDC00u || u2 > 0xDFFFu) return false;
                        const uint32_t cp = 0x10000u + (((u1 - 0xD800u) << 10) | (u2 - 0xDC00u));
                        AppendUtf8Codepoint(out, cp);
                    } else {
                        return false;
                    }
                } else if (u1 >= 0xDC00u && u1 <= 0xDFFFu) {
                    return false;
                } else {
                    AppendUtf8Codepoint(out, u1);
                }
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool ParseNumber(double& out) noexcept {
        const size_t start = pos_;
        if (pos_ < src_.size() && src_[pos_] == '-') ++pos_;
        if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') return false;
        if (src_[pos_] == '0') {
            ++pos_;
        } else {
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        }
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') return false;
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') return false;
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        }
        const auto res = std::from_chars(src_.data() + start, src_.data() + pos_, out);
        return res.ec == std::errc{};
    }

    bool ParseValue(JsonVal& out, int depth) {
        if (depth > 32) return false;
        SkipWs();
        if (pos_ >= src_.size()) return false;
        const char c = src_[pos_];
        if (c == 'n') {
            if (!MatchLiteral("null")) return false;
            out = JsonVal{};
            return true;
        }
        if (c == 't') {
            if (!MatchLiteral("true")) return false;
            out = JsonVal{};
            out.kind = JsonVal::Kind::Bool;
            out.bool_val = true;
            return true;
        }
        if (c == 'f') {
            if (!MatchLiteral("false")) return false;
            out = JsonVal{};
            out.kind = JsonVal::Kind::Bool;
            out.bool_val = false;
            return true;
        }
        if (c == '"') {
            out = JsonVal{};
            out.kind = JsonVal::Kind::String;
            return ParseString(out.str_val);
        }
        if (c == '[') {
            ++pos_;
            out = JsonVal{};
            out.kind = JsonVal::Kind::Array;
            SkipWs();
            if (pos_ < src_.size() && src_[pos_] == ']') {
                ++pos_;
                return true;
            }
            for (;;) {
                JsonVal elem;
                if (!ParseValue(elem, depth + 1)) return false;
                out.arr_val.push_back(std::move(elem));
                SkipWs();
                if (pos_ >= src_.size()) return false;
                if (src_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                if (src_[pos_] != ',') return false;
                ++pos_;
            }
        }
        if (c == '{') {
            ++pos_;
            out = JsonVal{};
            out.kind = JsonVal::Kind::Object;
            SkipWs();
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                return true;
            }
            for (;;) {
                SkipWs();
                std::string key;
                if (!ParseString(key)) return false;
                SkipWs();
                if (pos_ >= src_.size() || src_[pos_] != ':') return false;
                ++pos_;
                JsonVal val;
                if (!ParseValue(val, depth + 1)) return false;
                out.obj_val.emplace_back(std::move(key), std::move(val));
                SkipWs();
                if (pos_ >= src_.size()) return false;
                if (src_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                if (src_[pos_] != ',') return false;
                ++pos_;
            }
        }
        out = JsonVal{};
        out.kind = JsonVal::Kind::Number;
        return ParseNumber(out.num_val);
    }
};

bool ParseJson(std::string_view utf8, JsonVal& out) {
    if (utf8.size() >= 3 &&
        static_cast<unsigned char>(utf8[0]) == 0xEFu &&
        static_cast<unsigned char>(utf8[1]) == 0xBBu &&
        static_cast<unsigned char>(utf8[2]) == 0xBFu) {
        utf8.remove_prefix(3);
    }
    JsonParser parser(utf8);
    return parser.ParseRoot(out);
}

// ---------------------------------------------------------------------------
// Pulse AI Assistant Provider Catalog & DPAPI Key Reader
// (100% compatible with src/app/ai_providers.cpp + src/app/ai_model_key.cpp)
// ---------------------------------------------------------------------------
struct ProviderDef {
    std::string_view id;
    const wchar_t* name;
    const wchar_t* base_url;
    bool needs_key;
};

constexpr ProviderDef kProviders[] = {
    {"deepseek",   L"DeepSeek",   L"https://api.deepseek.com",                                true},
    {"qwen",       L"通义千问",    L"https://dashscope.aliyuncs.com/compatible-mode/v1",       true},
    {"kimi",       L"Kimi",       L"https://api.moonshot.cn/v1",                              true},
    {"zhipu",      L"智谱 GLM",   L"https://open.bigmodel.cn/api/paas/v4",                    true},
    {"doubao",     L"豆包",       L"https://ark.cn-beijing.volces.com/api/v3",                true},
    {"mimo",       L"小米 MiMo",  L"https://api.xiaomimimo.com/v1",                           true},
    {"silicon",    L"硅基流动",    L"https://api.siliconflow.cn/v1",                           true},
    {"openai",     L"OpenAI",     L"https://api.openai.com/v1",                               true},
    {"ollama",     L"Ollama",     L"http://localhost:11434/v1",                               false},
    {"custom",     L"自定义",      L"",                                                        true},
    {"openrouter", L"OpenRouter", L"https://openrouter.ai/api/v1",                            true},
    {"gemini",     L"Gemini",     L"https://generativelanguage.googleapis.com/v1beta/openai", true},
    {"claude",     L"Claude",     L"https://api.anthropic.com/v1",                            true},
};

constexpr wchar_t kMimoTokenPlanUrl[] = L"https://token-plan-cn.xiaomimimo.com/v1";

const ProviderDef& FindProviderById(std::string_view id) noexcept {
    for (const auto& p : kProviders) {
        if (p.id == id) return p;
    }
    return kProviders[0];
}

bool IsKnownProviderId(std::string_view id) noexcept {
    for (const auto& p : kProviders) {
        if (p.id == id) return true;
    }
    return false;
}

bool UsableServiceId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 32) return false;
    for (const char ch : id) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) {
            return false;
        }
    }
    return true;
}

bool UsableHttpUrl(std::string_view url) noexcept {
    return !url.empty() && url.size() <= 512 &&
           (url.starts_with("https://") || url.starts_with("http://"));
}

std::wstring DefaultPulseAiDir() {
    wchar_t root[32768]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", root, ARRAYSIZE(root));
    if (n == 0 || n >= ARRAYSIZE(root)) return {};
    return std::wstring(root, n) + L"\\Pulse\\ai";
}

bool ReadFileBytesBounded(const std::wstring& path, size_t max_bytes, std::vector<BYTE>& out) {
    out.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(file, &sz) && sz.QuadPart > 0 &&
              static_cast<uint64_t>(sz.QuadPart) <= max_bytes;
    if (ok) {
        out.resize(static_cast<size_t>(sz.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
             read == out.size();
    }
    CloseHandle(file);
    return ok;
}

bool LoadDpapiServiceKey(const std::wstring& ai_dir, std::string_view service_id, std::wstring& out_key) {
    WipeWideString(out_key);
    if (ai_dir.empty()) return false;

    std::wstring file_name = L"model_key.bin";
    std::string entropy_str = "Pulse.AI.ModelKey.v1";
    if (!service_id.empty() && service_id != "deepseek" && IsKnownProviderId(service_id)) {
        file_name = L"model_key_" + std::wstring(service_id.begin(), service_id.end()) + L".bin";
        entropy_str = "Pulse.AI.ModelKey." + std::string(service_id) + ".v1";
    }

    const std::wstring key_path = JoinPath(ai_dir, file_name);
    std::vector<BYTE> blob;
    if (!ReadFileBytesBounded(key_path, 64 * 1024, blob)) return false;

    DATA_BLOB in{static_cast<DWORD>(blob.size()), blob.data()};
    DATA_BLOB entropy{static_cast<DWORD>(entropy_str.size()),
                      reinterpret_cast<BYTE*>(entropy_str.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }

    if (out.pbData && out.cbData > 0 && (out.cbData % sizeof(wchar_t)) == 0) {
        out_key.assign(reinterpret_cast<const wchar_t*>(out.pbData),
                       out.cbData / sizeof(wchar_t));
    }
    if (out.pbData) {
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
    }
    return !out_key.empty();
}

AiConnectionInfo ResolvePulseAiConnectionAndKey(const std::wstring& override_ai_dir,
                                                std::wstring* out_decrypted_key) {
    if (out_decrypted_key) WipeWideString(*out_decrypted_key);

    std::wstring ai_dir = override_ai_dir.empty() ? DefaultPulseAiDir() : override_ai_dir;
    while (ai_dir.size() > 3 && (ai_dir.back() == L'\\' || ai_dir.back() == L'/')) {
        ai_dir.pop_back();
    }

    AiConnectionInfo info;
    info.assistant_enabled = true;
    info.service_id = "deepseek";
    info.model_id = "deepseek-flash";

    std::string explicit_base_url;
    std::map<std::string, std::pair<std::string, std::string>> saved_services;
    bool has_explicit_model = false;

    if (!ai_dir.empty()) {
        const std::wstring settings_path = JoinPath(ai_dir, L"ai.json");
        std::vector<BYTE> raw;
        if (ReadFileBytesBounded(settings_path, 512 * 1024, raw)) {
            const std::string_view utf8(reinterpret_cast<const char*>(raw.data()), raw.size());
            JsonVal root;
            if (ParseJson(utf8, root) && root.is_object()) {
                info.assistant_enabled = root.get_bool("enabled", true);
                if (const JsonVal* asst = root.find("assistant"); asst && asst->is_object()) {
                    const std::string svc = asst->get_string("service");
                    if (UsableServiceId(svc)) {
                        info.service_id = svc;
                    }
                    const std::string mdl = asst->get_string("model");
                    if (!mdl.empty() && mdl.size() <= 128) {
                        info.model_id = mdl;
                        has_explicit_model = true;
                    }
                    const std::string url = asst->get_string("base_url");
                    if (UsableHttpUrl(url)) {
                        explicit_base_url = url;
                    }
                    if (const JsonVal* svcs = asst->find("services"); svcs && svcs->is_object()) {
                        for (const auto& [sid, item] : svcs->obj_val) {
                            if (!UsableServiceId(sid) || !item.is_object()) continue;
                            std::string sm = item.get_string("model");
                            if (sm.size() > 128) sm.clear();
                            std::string su = item.get_string("base_url");
                            if (!UsableHttpUrl(su)) su.clear();
                            saved_services[sid] = {std::move(sm), std::move(su)};
                        }
                    }
                }
            }
        }
    }

    const ProviderDef& prov = FindProviderById(info.service_id);
    if (!IsKnownProviderId(info.service_id)) {
        info.service_id = std::string(prov.id);
    }
    info.provider_name = prov.name;
    info.needs_key = prov.needs_key;

    if (!has_explicit_model) {
        const auto it = saved_services.find(info.service_id);
        if (it != saved_services.end() && !it->second.first.empty()) {
            info.model_id = it->second.first;
        }
    }

    if (!explicit_base_url.empty()) {
        info.base_url = Utf8ToWide(explicit_base_url);
    } else {
        const auto it = saved_services.find(info.service_id);
        if (it != saved_services.end() && !it->second.second.empty()) {
            info.base_url = Utf8ToWide(it->second.second);
        } else {
            info.base_url = prov.base_url;
        }
    }

    std::wstring key;
    ScopedWideSecretWipe wipe_local_key{key};
    const bool has_key = LoadDpapiServiceKey(ai_dir, info.service_id, key);

    if (info.service_id == "mimo") {
        const bool token_plan = (key.substr(0, 3) == L"tp-");
        if (token_plan && info.base_url == prov.base_url) {
            info.base_url = kMimoTokenPlanUrl;
        } else if (!token_plan && info.base_url == kMimoTokenPlanUrl) {
            info.base_url = prov.base_url;
        }
    }

    info.configured = info.assistant_enabled &&
                      !info.base_url.empty() &&
                      !info.model_id.empty() &&
                      (!info.needs_key || has_key);

    if (out_decrypted_key && has_key) {
        *out_decrypted_key = key;
    }
    return info;
}

// ---------------------------------------------------------------------------
// Thinking-Suppression Field Builder (mirrors ai_providers.cpp Off logic)
// ---------------------------------------------------------------------------
std::string ThinkingSuppressionFieldJson(std::string_view service_id, std::string_view model_id) {
    std::string m = ToLowerAscii(model_id);
    if (const size_t slash = m.rfind('/'); slash != std::string::npos) {
        m.erase(0, slash + 1);
    }
    if (m.empty()) return {};

    // Fixed-reasoning models must NOT receive disabled flags (providers reject them).
    if (m.find("thinking") != std::string::npos ||
        m.starts_with("deepseek-r1") ||
        m.starts_with("qwq") ||
        m.starts_with("o1") ||
        m.starts_with("o3") ||
        m.starts_with("o4") ||
        m.starts_with("gpt-5-pro")) {
        return {};
    }

    if (service_id == "openrouter") {
        if (m.starts_with("deepseek") || m.starts_with("qwen3") || m.starts_with("glm-4.") ||
            m.starts_with("glm-5") || m.starts_with("kimi-k2.") || m.starts_with("gemini-2.5-flash")) {
            return ",\"reasoning\":{\"enabled\":false}";
        }
        return {};
    }

    if (service_id == "deepseek") {
        return ",\"thinking\":{\"type\":\"disabled\"}";
    }
    if (service_id == "qwen" || service_id == "silicon") {
        if (m.find("instruct") != std::string::npos || m.find("coder") != std::string::npos) {
            return {};
        }
        if (m.starts_with("qwen3") || m.starts_with("qwen-plus") || m.starts_with("qwen-flash") ||
            m.starts_with("qwen-turbo") || m.starts_with("deepseek-v3") || m.starts_with("deepseek-v4") ||
            m.starts_with("glm-4.") || m.starts_with("glm-5") || m.starts_with("kimi-k2.")) {
            return ",\"enable_thinking\":false";
        }
        return {};
    }
    if (service_id == "zhipu") {
        if (m.starts_with("glm-4.5") || m.starts_with("glm-4.6") || m.starts_with("glm-4.7") ||
            (m.starts_with("glm-5") && !m.starts_with("glm-5.3"))) {
            return ",\"thinking\":{\"type\":\"disabled\"}";
        }
        return {};
    }
    if (service_id == "kimi") {
        if (m.starts_with("kimi-k2.5") || m.starts_with("kimi-k2.6")) {
            return ",\"thinking\":{\"type\":\"disabled\"}";
        }
        return {};
    }
    if (service_id == "mimo") {
        if (m.starts_with("mimo")) {
            return ",\"thinking\":{\"type\":\"disabled\"}";
        }
        return {};
    }
    if (service_id == "doubao") {
        if (m.starts_with("doubao-seed-1-6-flash") || m.starts_with("doubao-seed-1-6-250615") ||
            m == "doubao-seed-1-6" || m.starts_with("deepseek-v3") || m.starts_with("deepseek-v4")) {
            return ",\"thinking\":{\"type\":\"disabled\"}";
        }
        if (m.find("seed-evolving") != std::string::npos) {
            return ",\"reasoning_effort\":\"none\"";
        }
        if (m.starts_with("doubao-seed-1-6") || m.starts_with("doubao-seed-1-8") || m.starts_with("doubao-seed-2")) {
            return ",\"reasoning_effort\":\"minimal\"";
        }
        return {};
    }
    if (service_id == "gemini") {
        if (m.starts_with("gemini-2.5-flash")) {
            return ",\"reasoning_effort\":\"none\"";
        }
        return {};
    }
    if (service_id == "ollama") {
        if (m.starts_with("qwen3") || m.starts_with("deepseek-v3.1")) {
            return ",\"reasoning_effort\":\"none\"";
        }
        return {};
    }
    return {};
}

// ---------------------------------------------------------------------------
// Desktop Context & Plain-Text Snippet Reader
// ---------------------------------------------------------------------------
bool IsPlainTextCandidateExtension(std::wstring_view ext_lower) noexcept {
    static constexpr std::wstring_view kTextExts[] = {
        L".txt", L".md", L".markdown", L".csv", L".tsv", L".json", L".jsonc",
        L".log", L".ini", L".cfg", L".conf", L".toml", L".yaml", L".yml",
        L".xml", L".html", L".htm", L".css", L".py", L".js", L".ts",
        L".tsx", L".jsx", L".cpp", L".cc", L".c", L".h", L".hpp",
        L".rs", L".go", L".java", L".cs", L".sql", L".sh", L".bat",
        L".cmd", L".ps1", L".rtf",
    };
    for (auto e : kTextExts) {
        if (ext_lower == e) return true;
    }
    return false;
}

std::wstring CollapseAndCapSnippet(std::wstring_view text, size_t max_chars = 160) {
    std::wstring out;
    out.reserve(std::min<size_t>(text.size(), max_chars + 4));
    bool prev_space = true;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\0') continue;
        if (std::iswspace(ch) || ch < 0x20) {
            if (!prev_space && !out.empty()) {
                out.push_back(L' ');
                prev_space = true;
            }
            continue;
        }
        out.push_back(ch);
        prev_space = false;
        if (out.size() >= max_chars) break;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    if (out.size() > max_chars) out.resize(max_chars);
    if (!out.empty() && out.back() >= 0xD800 && out.back() <= 0xDBFF) {
        out.pop_back();
    }
    return out;
}

std::wstring ReadLightweightTextSnippet(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(file, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 256 * 1024) {
        CloseHandle(file);
        return {};
    }

    const DWORD to_read = static_cast<DWORD>(std::min<LONGLONG>(sz.QuadPart, 4096));
    std::vector<uint8_t> buf(to_read);
    DWORD read = 0;
    const BOOL ok = ReadFile(file, buf.data(), to_read, &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0) return {};
    buf.resize(read);

    std::wstring decoded;
    if (buf.size() >= 2 && buf[0] == 0xFFu && buf[1] == 0xFEu) {
        const size_t wc_count = (buf.size() - 2) / 2;
        decoded.resize(wc_count);
        if (wc_count > 0) {
            std::memcpy(decoded.data(), buf.data() + 2, wc_count * sizeof(wchar_t));
        }
    } else if (buf.size() >= 2 && buf[0] == 0xFEu && buf[1] == 0xFFu) {
        const size_t wc_count = (buf.size() - 2) / 2;
        decoded.resize(wc_count);
        for (size_t i = 0; i < wc_count; ++i) {
            decoded[i] = static_cast<wchar_t>((static_cast<uint16_t>(buf[2 + i * 2]) << 8) |
                                              static_cast<uint16_t>(buf[2 + i * 2 + 1]));
        }
    } else {
        size_t offset = 0;
        if (buf.size() >= 3 && buf[0] == 0xEFu && buf[1] == 0xBBu && buf[2] == 0xBFu) {
            offset = 3;
        }
        // Reject raw binary files with embedded NUL bytes when no UTF-16 BOM is present.
        if (std::memchr(buf.data() + offset, 0, buf.size() - offset) != nullptr) {
            return {};
        }
        size_t valid_len = buf.size() - offset;
        // Trim a potentially truncated multi-byte UTF-8 tail at the 4 KB boundary.
        for (int trim = 0; trim < 4 && valid_len > 0; ++trim) {
            const std::string_view slice(reinterpret_cast<const char*>(buf.data() + offset), valid_len);
            decoded = Utf8ToWide(slice, true);
            if (!decoded.empty()) break;
            --valid_len;
        }
        if (decoded.empty() && buf.size() > offset) {
            const int acp_len = MultiByteToWideChar(CP_ACP, 0,
                                                    reinterpret_cast<const char*>(buf.data() + offset),
                                                    static_cast<int>(buf.size() - offset),
                                                    nullptr, 0);
            if (acp_len > 0) {
                decoded.resize(static_cast<size_t>(acp_len));
                MultiByteToWideChar(CP_ACP, 0,
                                    reinterpret_cast<const char*>(buf.data() + offset),
                                    static_cast<int>(buf.size() - offset),
                                    decoded.data(), acp_len);
            }
        }
    }

    return CollapseAndCapSnippet(decoded, 160);
}

bool IsSystemOrVirtualDesktopEntry(const model::DesktopItem& it) {
    if (it.name.empty() || it.path.empty()) return true;
    if (it.path.starts_with(L"::") || it.path.starts_with(L"shell:")) return true;
    const bool drive_path = it.path.size() >= 3 && it.path[1] == L':' &&
                            (it.path[2] == L'\\' || it.path[2] == L'/');
    const bool unc_path = it.path.starts_with(L"\\\\");
    if (!drive_path && !unc_path) return true;

    const std::wstring lower_name = ToLowerWide(it.name);
    if (lower_name == L"desktop.ini" || lower_name == L"thumbs.db") return true;
    return false;
}

void InspectExistingFolderOnDisk(const std::wstring& folder_path,
                                 bool fallback_empty,
                                 bool& out_is_empty,
                                 std::vector<std::wstring>& out_samples) {
    out_samples.clear();
    if (!IsDirectoryOnDisk(folder_path)) {
        out_is_empty = fallback_empty;
        return;
    }

    const std::wstring pattern = JoinPath(folder_path, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        out_is_empty = fallback_empty;
        return;
    }

    size_t real_children = 0;
    do {
        const std::wstring_view cname(fd.cFileName);
        if (cname == L"." || cname == L"..") continue;
        if (EqualsIgnoreCaseW(cname, L"desktop.ini")) continue;
        ++real_children;
        if (out_samples.size() < 6) {
            out_samples.emplace_back(cname);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    out_is_empty = (real_children == 0);
}

std::wstring ExtractJsonFencePayload(std::string_view raw_utf8, std::string_view& out_slice) noexcept {
    std::string_view s = raw_utf8;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }

    if (s.starts_with("```")) {
        const size_t first_nl = s.find('\n');
        if (first_nl != std::string_view::npos) {
            s.remove_prefix(first_nl + 1);
        } else {
            s.remove_prefix(3);
        }
        if (const size_t end_fence = s.rfind("```"); end_fence != std::string_view::npos) {
            s = s.substr(0, end_fence);
        }
    }

    const size_t first_brace = s.find('{');
    const size_t last_brace = s.rfind('}');
    if (first_brace == std::string_view::npos || last_brace == std::string_view::npos || last_brace < first_brace) {
        out_slice = {};
        return L"AI 返回内容未包含有效 JSON 对象";
    }
    out_slice = s.substr(first_brace, last_brace - first_brace + 1);
    return {};
}

std::wstring MakeCollisionSafeDestination(const std::wstring& target_dir,
                                          const std::wstring& item_name,
                                          bool is_folder,
                                          const std::wstring& source_path) {
    std::wstring candidate = JoinPath(target_dir, item_name);
    if (!PathExistsOnDisk(candidate) || EqualsIgnoreCaseW(candidate, source_path)) {
        return candidate;
    }

    std::wstring stem = item_name;
    std::wstring ext;
    if (!is_folder) {
        const size_t dot = item_name.rfind(L'.');
        if (dot != std::wstring::npos && dot > 0) {
            stem = item_name.substr(0, dot);
            ext = item_name.substr(dot);
        }
    }

    for (uint32_t idx = 2; idx < 10000; ++idx) {
        std::wstring numbered = JoinPath(target_dir, stem + L" (" + std::to_wstring(idx) + L")" + ext);
        if (!PathExistsOnDisk(numbered) || EqualsIgnoreCaseW(numbered, source_path)) {
            return numbered;
        }
    }
    return JoinPath(target_dir, stem + L"_" + std::to_wstring(GetTickCount64()) + ext);
}

} // namespace

// ---------------------------------------------------------------------------
// Public API Implementations
// ---------------------------------------------------------------------------

AiConnectionInfo ProbePulseAiAssistantConnection(const std::wstring& override_ai_dir) {
    return ResolvePulseAiConnectionAndKey(override_ai_dir, nullptr);
}

bool IsValidArchiveFolderName(std::wstring_view folder_name) noexcept {
    if (folder_name.empty() || folder_name.size() > 80) return false;
    if (folder_name.front() == L' ' || folder_name.back() == L' ' || folder_name.back() == L'.') {
        return false;
    }
    if (folder_name == L"." || folder_name == L".." ||
        folder_name.find(L"..") != std::wstring_view::npos) {
        return false;
    }
    for (const wchar_t ch : folder_name) {
        if (ch < 32) return false;
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' ||
            ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
            return false;
        }
    }

    const size_t dot = folder_name.find(L'.');
    const std::wstring_view stem = (dot == std::wstring_view::npos) ? folder_name : folder_name.substr(0, dot);
    if (stem.empty()) return false;

    static constexpr std::wstring_view kReserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9",
    };
    for (const auto r : kReserved) {
        if (EqualsIgnoreCaseW(stem, r)) return false;
    }
    return true;
}

void CollectDesktopAiContext(const std::wstring& desktop_dir,
                             std::span<const model::DesktopItem> items,
                             std::span<const model::DesktopGroup> groups,
                             int64_t now_unix_ms,
                             std::vector<DesktopAiCandidate>& out_candidates,
                             std::vector<ExistingDesktopFolder>& out_existing_folders) {
    out_candidates.clear();
    out_existing_folders.clear();
    (void)desktop_dir;

    const int64_t effective_now = (now_unix_ms > 0) ? now_unix_ms : CurrentUnixMs();

    for (const auto& it : items) {
        if (IsSystemOrVirtualDesktopEntry(it)) continue;

        std::wstring ext_lower = ToLowerWide(it.extension);
        if (ext_lower.empty() && !it.is_folder) {
            const size_t dot = it.name.rfind(L'.');
            if (dot != std::wstring::npos && dot > 0) {
                ext_lower = ToLowerWide(std::wstring_view(it.name).substr(dot));
            }
        }

        bool folder_empty = it.folder_is_empty;
        if (it.is_folder) {
            std::vector<std::wstring> samples;
            InspectExistingFolderOnDisk(it.path, it.folder_is_empty, folder_empty, samples);
            if (!folder_empty) {
                ExistingDesktopFolder ef;
                ef.name = it.name;
                ef.path = it.path;
                ef.sample_children = std::move(samples);
                out_existing_folders.push_back(std::move(ef));
            }
        }

        DesktopAiCandidate cand;
        cand.path = it.path;
        cand.name = it.name;
        cand.extension = ext_lower;
        cand.size_bytes = it.size_bytes;
        cand.is_folder = it.is_folder;
        cand.folder_is_empty = it.is_folder ? folder_empty : false;
        cand.is_shortcut = it.is_shortcut || (ext_lower == L".lnk");

        if (effective_now > 0 && it.modified_unix_ms > 0 && effective_now > it.modified_unix_ms) {
            cand.age_days = static_cast<int32_t>((effective_now - it.modified_unix_ms) / 86'400'000LL);
        }

        if (!it.force_loose && !groups.empty()) {
            if (const auto gidx = model::ClassifyItemToGroup(it, groups, effective_now)) {
                cand.current_group_title = groups[*gidx].title;
            }
        }

        if (cand.is_shortcut) {
            if (!it.link_target.empty()) {
                cand.snippet = CollapseAndCapSnippet(L"快捷方式 -> " + it.link_target, 160);
            }
        } else if (!cand.is_folder && it.size_bytes <= 256 * 1024 &&
                   IsPlainTextCandidateExtension(ext_lower)) {
            cand.snippet = ReadLightweightTextSnippet(it.path);
        }

        out_candidates.push_back(std::move(cand));
    }
}

std::string BuildAiOrganizeChatBody(std::string_view service_id,
                                    std::string_view model_id,
                                    AiOrganizeStrategy strategy,
                                    std::wstring_view custom_instruction,
                                    std::span<const DesktopAiCandidate> candidates,
                                    std::span<const ExistingDesktopFolder> existing_folders) {
    const char* strategy_desc = "归档散落的安装包、旧截图、临时压缩包与超过 3 天未改动的杂项文件，保留近期活跃工作的核心文件与应用快捷方式不动。";
    if (strategy == AiOrganizeStrategy::ByProjectTopic) {
        strategy_desc = "按项目名称、工作主题与文本摘要语义对桌面文件进行归档整理，优先复用桌面上已有的相关目录。";
    } else if (strategy == AiOrganizeStrategy::FullDesktop) {
        strategy_desc = "全盘收纳桌面上所有可归档的文件与空目录，划分为清晰精炼的中文主题子目录（保留桌面已有归档目录本身不动）。";
    }

    const std::string system_prompt =
        "你是 Pulse 文件管理器的内置桌面收纳引擎。请根据用户桌面文件元数据、修改天数、文本内容摘要与已有子目录，"
        "生成结构化的桌面归档方案。\n"
        "严格要求：\n"
        "1. 只能输出单个合法的 JSON 对象，禁止输出任何额外说明文字或 Markdown 标题，格式严格为：\n"
        "{\"summary\":\"一句话中文总结\",\"groups\":[{\"folder_name\":\"归档目录名\",\"reason\":\"归类理由\",\"items\":[\"文件名.ext\"]}]}\n"
        "2. folder_name 必须是合法简短的中文或项目文件夹名称（<=30 字，禁止包含路径分隔符、.. 或 CON/PRN/AUX/NUL 等系统保留字）。"
        "若 existing_folders 中已有语义匹配的目录，请直接使用该目录名。\n"
        "3. items 数组中的每一项必须精确等于 candidates 列表中的 name，每个文件最多出现一次，严禁把目标归档目录本身放入 items。\n"
        "4. 桌面应用快捷方式（.lnk）除非用户明确要求，默认不要移入子目录。";

    std::string user_payload = "收纳策略：";
    user_payload += strategy_desc;
    if (!custom_instruction.empty()) {
        user_payload += "\n用户补充指令：";
        user_payload += WideToUtf8(custom_instruction);
    }

    user_payload += "\n\n桌面已有非空子目录 (existing_folders)：[";
    for (size_t i = 0; i < existing_folders.size(); ++i) {
        if (i > 0) user_payload.push_back(',');
        user_payload += "{\"name\":" + JsonQuoteUtf8(WideToUtf8(existing_folders[i].name)) + ",\"samples\":[";
        for (size_t j = 0; j < existing_folders[i].sample_children.size(); ++j) {
            if (j > 0) user_payload.push_back(',');
            user_payload += JsonQuoteUtf8(WideToUtf8(existing_folders[i].sample_children[j]));
        }
        user_payload += "]}";
    }
    user_payload += "]\n\n待分析桌面条目 (candidates)：[\n";

    const size_t max_items = std::min<size_t>(candidates.size(), 250);
    bool first_cand = true;
    for (size_t i = 0; i < max_items; ++i) {
        const auto& c = candidates[i];
        // Non-empty existing folders are already listed in existing_folders as destinations.
        if (c.is_folder && !c.folder_is_empty) continue;
        if (!first_cand) user_payload += ",\n";
        first_cand = false;
        user_payload += "{\"name\":" + JsonQuoteUtf8(WideToUtf8(c.name));
        user_payload += ",\"size\":" + std::to_string(c.size_bytes);
        user_payload += ",\"age_days\":" + std::to_string(c.age_days);
        if (c.is_folder) user_payload += ",\"empty_folder\":true";
        if (c.is_shortcut) user_payload += ",\"shortcut\":true";
        if (!c.current_group_title.empty()) {
            user_payload += ",\"group\":" + JsonQuoteUtf8(WideToUtf8(c.current_group_title));
        }
        if (!c.snippet.empty()) {
            user_payload += ",\"snippet\":" + JsonQuoteUtf8(WideToUtf8(c.snippet));
        }
        user_payload.push_back('}');
    }
    user_payload += "\n]";

    std::string body = "{\"model\":" + JsonQuoteUtf8(model_id);
    body += ",\"stream\":true,\"temperature\":0.2";
    body += ThinkingSuppressionFieldJson(service_id, model_id);
    body += ",\"messages\":[{\"role\":\"system\",\"content\":" + JsonQuoteUtf8(system_prompt) + "},";
    body += "{\"role\":\"user\",\"content\":" + JsonQuoteUtf8(user_payload) + "}]}";
    return body;
}

bool ParseAndValidateAiOrganizeReply(std::string_view raw_utf8,
                                     const std::wstring& desktop_dir,
                                     std::span<const DesktopAiCandidate> candidates,
                                     std::span<const ExistingDesktopFolder> existing_folders,
                                     AiOrganizePlan& out_plan,
                                     std::wstring& out_error) {
    out_plan = AiOrganizePlan{};
    out_error.clear();

    std::string_view json_slice;
    const std::wstring fence_err = ExtractJsonFencePayload(raw_utf8, json_slice);
    if (!fence_err.empty()) {
        out_error = fence_err;
        return false;
    }

    JsonVal root;
    if (!ParseJson(json_slice, root) || !root.is_object()) {
        out_error = L"AI 返回的 JSON 格式无法解析";
        return false;
    }

    const JsonVal* groups_val = root.find("groups");
    if (!groups_val || !groups_val->is_array() || groups_val->arr_val.empty()) {
        out_error = L"AI 方案中未包含有效的 groups 分组列表";
        return false;
    }
    if (groups_val->arr_val.size() > 30) {
        out_error = L"归档分组数量超出上限（最多 30 组）";
        return false;
    }

    std::map<std::wstring, size_t> cand_by_lower;
    for (size_t i = 0; i < candidates.size(); ++i) {
        cand_by_lower.emplace(ToLowerWide(candidates[i].name), i);
    }

    std::map<std::wstring, const ExistingDesktopFolder*> existing_by_lower;
    for (const auto& ef : existing_folders) {
        existing_by_lower.emplace(ToLowerWide(ef.name), &ef);
    }

    // First pass: validate all folder names and collect target folder set so no destination folder is moved.
    std::set<std::wstring> all_target_folders_lower;
    for (const JsonVal& g : groups_val->arr_val) {
        if (!g.is_object()) {
            out_error = L"AI 分组条目格式无效";
            return false;
        }
        std::string raw_fname = g.get_string("folder_name");
        if (raw_fname.empty()) raw_fname = g.get_string("name");
        const std::wstring fname = TrimWide(Utf8ToWide(raw_fname));
        if (!IsValidArchiveFolderName(fname)) {
            out_error = L"归档目录名称不合法或不安全: " + fname;
            return false;
        }
        all_target_folders_lower.insert(ToLowerWide(fname));
    }

    std::map<std::wstring, size_t> merged_group_index;
    std::set<std::wstring> seen_items_lower;

    for (const JsonVal& g : groups_val->arr_val) {
        std::string raw_fname = g.get_string("folder_name");
        if (raw_fname.empty()) raw_fname = g.get_string("name");
        std::wstring fname = TrimWide(Utf8ToWide(raw_fname));
        const std::wstring fname_lower = ToLowerWide(fname);

        bool already_exists = false;
        std::wstring target_path = JoinPath(desktop_dir, fname);
        if (const auto ex_it = existing_by_lower.find(fname_lower); ex_it != existing_by_lower.end()) {
            already_exists = true;
            fname = ex_it->second->name;
            target_path = ex_it->second->path.empty() ? JoinPath(desktop_dir, fname) : ex_it->second->path;
        } else if (IsDirectoryOnDisk(target_path)) {
            already_exists = true;
        } else if (PathExistsOnDisk(target_path)) {
            out_error = L"桌面上已存在同名文件，无法创建归档文件夹: " + fname;
            return false;
        } else if (const auto c_it = cand_by_lower.find(fname_lower);
                   c_it != cand_by_lower.end() && !candidates[c_it->second].is_folder) {
            out_error = L"桌面上已存在同名文件，无法创建归档文件夹: " + fname;
            return false;
        }

        const JsonVal* items_val = g.find("items");
        if (!items_val || !items_val->is_array()) {
            out_error = L"分组缺少 items 文件数组: " + fname;
            return false;
        }

        size_t dst_group_idx = 0;
        if (const auto mg = merged_group_index.find(fname_lower); mg != merged_group_index.end()) {
            dst_group_idx = mg->second;
        } else {
            AiArchiveGroupPlan gp;
            gp.folder_name = fname;
            gp.target_folder_path = target_path;
            gp.reason = TrimWide(Utf8ToWide(g.get_string("reason")));
            gp.folder_already_exists = already_exists;
            dst_group_idx = out_plan.groups.size();
            merged_group_index.emplace(fname_lower, dst_group_idx);
            out_plan.groups.push_back(std::move(gp));
        }

        auto& dst_group = out_plan.groups[dst_group_idx];
        if (dst_group.reason.empty()) {
            dst_group.reason = TrimWide(Utf8ToWide(g.get_string("reason")));
        }

        for (const JsonVal& item_v : items_val->arr_val) {
            if (!item_v.is_string()) {
                out_error = L"items 列表包含非字符串条目";
                return false;
            }
            std::wstring item_name = TrimWide(Utf8ToWide(item_v.str_val));
            if (!desktop_dir.empty() && item_name.size() > desktop_dir.size() &&
                _wcsnicmp(item_name.c_str(), desktop_dir.c_str(), desktop_dir.size()) == 0) {
                std::wstring rest = item_name.substr(desktop_dir.size());
                if (!rest.empty() && (rest.front() == L'\\' || rest.front() == L'/')) {
                    rest.erase(0, 1);
                }
                item_name = std::move(rest);
            }

            if (item_name.empty() ||
                item_name.find_first_of(L"\\/") != std::wstring::npos ||
                item_name == L"." || item_name == L".." ||
                item_name.find(L"..") != std::wstring::npos) {
                out_error = L"拒绝非法或越界的文件路径: " + item_name;
                return false;
            }

            const std::wstring item_lower = ToLowerWide(item_name);
            // Never move any destination archive folder into itself or another archive folder.
            if (all_target_folders_lower.contains(item_lower)) {
                continue;
            }

            const auto cand_it = cand_by_lower.find(item_lower);
            if (cand_it == cand_by_lower.end()) {
                out_error = L"收纳清单引用了不存在的桌面条目: " + item_name;
                return false;
            }

            if (!seen_items_lower.insert(item_lower).second) {
                continue;
            }

            const DesktopAiCandidate& cand = candidates[cand_it->second];
            AiArchiveItemEntry entry;
            entry.name = cand.name;
            entry.source_path = cand.path;
            entry.size_bytes = cand.size_bytes;
            entry.age_days = cand.age_days;
            entry.is_folder = cand.is_folder;
            entry.selected = true;
            dst_group.items.push_back(std::move(entry));
        }
    }

    // Prune groups that ended up with 0 valid items after deduplication/self-exclusion.
    std::erase_if(out_plan.groups, [](const AiArchiveGroupPlan& gp) { return gp.items.empty(); });
    if (out_plan.groups.empty()) {
        out_error = L"没有可执行的有效归档文件";
        return false;
    }

    out_plan.from_ai_model = true;
    out_plan.desktop_before_count = candidates.size();
    out_plan.summary = TrimWide(Utf8ToWide(root.get_string("summary")));
    if (out_plan.summary.empty()) {
        out_plan.summary = L"已规划将 " + std::to_wstring(out_plan.SelectedItemCount()) +
                           L" 个桌面项目整理至 " + std::to_wstring(out_plan.groups.size()) + L" 个归档文件夹";
    }
    return true;
}

AiOrganizePlan BuildSmartLocalOrganizePlan(const std::wstring& desktop_dir,
                                           AiOrganizeStrategy strategy,
                                           std::wstring_view custom_instruction,
                                           std::span<const DesktopAiCandidate> candidates,
                                           std::span<const ExistingDesktopFolder> existing_folders) {
    AiOrganizePlan plan;
    plan.from_ai_model = false;
    plan.model_badge = L"本地智能规则";
    plan.desktop_before_count = candidates.size();

    const bool only_images = (ContainsIgnoreCaseW(custom_instruction, L"只") ||
                              ContainsIgnoreCaseW(custom_instruction, L"仅")) &&
                             (ContainsIgnoreCaseW(custom_instruction, L"图") ||
                              ContainsIgnoreCaseW(custom_instruction, L"照片") ||
                              ContainsIgnoreCaseW(custom_instruction, L"截图"));
    const bool keep_docs = (ContainsIgnoreCaseW(custom_instruction, L"保留") ||
                            ContainsIgnoreCaseW(custom_instruction, L"不动") ||
                            ContainsIgnoreCaseW(custom_instruction, L"不要动")) &&
                           ContainsIgnoreCaseW(custom_instruction, L"文档");
    const bool keep_code = (ContainsIgnoreCaseW(custom_instruction, L"保留") ||
                            ContainsIgnoreCaseW(custom_instruction, L"不动") ||
                            ContainsIgnoreCaseW(custom_instruction, L"不要动")) &&
                           (ContainsIgnoreCaseW(custom_instruction, L"代码") ||
                            ContainsIgnoreCaseW(custom_instruction, L"脚本"));

    struct BucketSpec {
        std::wstring folder_name;
        std::wstring reason;
        std::vector<AiArchiveItemEntry> items;
    };
    std::map<std::wstring, BucketSpec> buckets;
    std::vector<std::wstring> bucket_order;

    auto resolve_existing_or_default = [&](std::wstring_view preferred_name,
                                           std::initializer_list<std::wstring_view> keywords) -> std::pair<std::wstring, bool> {
        for (const auto& ef : existing_folders) {
            if (EqualsIgnoreCaseW(ef.name, preferred_name)) {
                return {ef.name, true};
            }
            for (const auto kw : keywords) {
                if (!kw.empty() && ContainsIgnoreCaseW(ef.name, kw)) {
                    return {ef.name, true};
                }
            }
        }
        const std::wstring path = JoinPath(desktop_dir, preferred_name);
        return {std::wstring(preferred_name), IsDirectoryOnDisk(path)};
    };

    auto add_to_bucket = [&](std::wstring_view preferred_folder,
                             std::initializer_list<std::wstring_view> reuse_keywords,
                             std::wstring_view reason,
                             const DesktopAiCandidate& c) {
        auto [resolved_name, exists] = resolve_existing_or_default(preferred_folder, reuse_keywords);
        (void)exists;
        const std::wstring key = ToLowerWide(resolved_name);
        auto it = buckets.find(key);
        if (it == buckets.end()) {
            bucket_order.push_back(key);
            BucketSpec spec;
            spec.folder_name = std::move(resolved_name);
            spec.reason = std::wstring(reason);
            it = buckets.emplace(key, std::move(spec)).first;
        }
        AiArchiveItemEntry entry;
        entry.name = c.name;
        entry.source_path = c.path;
        entry.size_bytes = c.size_bytes;
        entry.age_days = c.age_days;
        entry.is_folder = c.is_folder;
        entry.selected = true;
        it->second.items.push_back(std::move(entry));
    };

    for (const auto& c : candidates) {
        if (c.is_folder && !c.folder_is_empty) continue;
        if (c.is_shortcut && strategy != AiOrganizeStrategy::FullDesktop) continue;

        const std::wstring ext = ToLowerWide(c.extension);
        const std::wstring combined = c.name + L" " + c.snippet;

        const bool is_image = (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp" ||
                               ext == L".gif" || ext == L".bmp" || ext == L".svg" || ext == L".avif" ||
                               ext == L".psd" || ext == L".ico");
        const bool is_installer_or_archive = (ext == L".exe" || ext == L".msi" || ext == L".msix" ||
                                              ext == L".7z" || ext == L".zip" || ext == L".rar" ||
                                              ext == L".iso" || ext == L".tar" || ext == L".gz");
        const bool is_finance = ContainsIgnoreCaseW(combined, L"发票") ||
                                ContainsIgnoreCaseW(combined, L"报销") ||
                                ContainsIgnoreCaseW(combined, L"预算") ||
                                ContainsIgnoreCaseW(combined, L"财务") ||
                                ContainsIgnoreCaseW(combined, L"合同") ||
                                ContainsIgnoreCaseW(combined, L"报价") ||
                                ContainsIgnoreCaseW(combined, L"账单") ||
                                ContainsIgnoreCaseW(combined, L"invoice") ||
                                ContainsIgnoreCaseW(combined, L"budget");
        const bool is_sheet = (ext == L".xls" || ext == L".xlsx" || ext == L".csv" || ext == L".tsv");
        const bool is_doc = (ext == L".doc" || ext == L".docx" || ext == L".pdf" || ext == L".ppt" ||
                             ext == L".pptx" || ext == L".md" || ext == L".markdown" || ext == L".txt" ||
                             ext == L".rtf");
        const bool is_code = (ext == L".py" || ext == L".js" || ext == L".ts" || ext == L".tsx" ||
                              ext == L".jsx" || ext == L".cpp" || ext == L".cc" || ext == L".c" ||
                              ext == L".h" || ext == L".hpp" || ext == L".rs" || ext == L".go" ||
                              ext == L".java" || ext == L".cs" || ext == L".json" || ext == L".yaml" ||
                              ext == L".yml" || ext == L".toml" || ext == L".ini" || ext == L".sql" ||
                              ext == L".ps1" || ext == L".bat" || ext == L".sh" || ext == L".log");

        if (only_images && !is_image) continue;

        if (strategy == AiOrganizeStrategy::ArchiveClutter) {
            // Preserve recent active documents/sheets/code (<= 3 days) on the desktop.
            if (!is_installer_or_archive && !is_image && !c.is_folder && c.age_days <= 3) {
                continue;
            }
        }

        // First check if filename directly matches an existing desktop subfolder topic.
        bool matched_existing_project = false;
        if (strategy == AiOrganizeStrategy::ByProjectTopic) {
            for (const auto& ef : existing_folders) {
                if (ef.name.size() >= 2 && ContainsIgnoreCaseW(combined, ef.name)) {
                    add_to_bucket(ef.name, {}, L"与桌面已有文件夹「" + ef.name + L"」主题一致", c);
                    matched_existing_project = true;
                    break;
                }
            }
        }
        if (matched_existing_project) continue;

        if (is_finance) {
            if (keep_docs) continue;
            add_to_bucket(L"财务预算与合同", {L"财务", L"发票", L"合同", L"报销"},
                          L"识别到财务、预算、发票或合同相关内容", c);
        } else if (is_image) {
            add_to_bucket(L"设计截图与素材", {L"截图", L"图片", L"素材", L"照片"},
                          L"桌面截图、图片与视觉素材归档", c);
        } else if (is_installer_or_archive) {
            add_to_bucket(L"安装包与压缩归档", {L"安装包", L"软件", L"压缩", L"归档"},
                          L"安装程序与压缩归档包集中收纳", c);
        } else if (is_sheet) {
            if (keep_docs) continue;
            add_to_bucket(L"表格与数据报表", {L"表格", L"数据", L"报表"},
                          L"电子表格与统计数据文件", c);
        } else if (is_doc) {
            if (keep_docs) continue;
            add_to_bucket(L"汇报与方案文档", {L"文档", L"方案", L"汇报", L"资料"},
                          L"办公文档、PDF、演示文稿与笔记", c);
        } else if (is_code) {
            if (keep_code) continue;
            add_to_bucket(L"开发脚本与配置", {L"代码", L"脚本", L"开发", L"配置"},
                          L"源代码、自动化脚本、配置与日志", c);
        } else if (strategy == AiOrganizeStrategy::FullDesktop || c.folder_is_empty || c.age_days > 14) {
            add_to_bucket(L"桌面杂项归档", {L"杂项", L"其他", L"旧文件"},
                          L"散落空文件夹与历史未分类条目", c);
        }
    }

    for (const auto& key : bucket_order) {
        auto& b = buckets[key];
        if (b.items.empty()) continue;
        AiArchiveGroupPlan gp;
        gp.folder_name = b.folder_name;
        gp.target_folder_path = JoinPath(desktop_dir, b.folder_name);
        gp.reason = std::move(b.reason);
        gp.folder_already_exists = IsDirectoryOnDisk(gp.target_folder_path);
        for (const auto& ef : existing_folders) {
            if (EqualsIgnoreCaseW(ef.name, gp.folder_name)) {
                gp.folder_already_exists = true;
                if (!ef.path.empty()) gp.target_folder_path = ef.path;
                break;
            }
        }
        gp.items = std::move(b.items);
        plan.groups.push_back(std::move(gp));
    }

    if (plan.groups.empty()) {
        plan.summary = L"桌面非常整洁，暂无需要归档的散落文件";
    } else {
        plan.summary = L"本地智能规则已将 " + std::to_wstring(plan.SelectedItemCount()) +
                       L" 个项目规划至 " + std::to_wstring(plan.groups.size()) + L" 个主题目录";
    }
    return plan;
}

AiArchiveExecutionResult ExecuteAiArchivePlan(const AiOrganizePlan& plan,
                                              const model::DesktopModelState& pre_model,
                                              bool pin_created_folders_to_tiles,
                                              model::DesktopModelState* out_updated_model) {
    AiArchiveExecutionResult res;
    res.undo_tx.pre_archive_model = pre_model;
    if (out_updated_model) {
        *out_updated_model = pre_model;
    }

    std::set<std::wstring> moved_sources_lower;
    std::vector<std::pair<std::wstring, std::wstring>> active_archive_folders;

    for (const auto& g : plan.groups) {
        if (g.SelectedCount() == 0) continue;
        if (!IsValidArchiveFolderName(g.folder_name) || g.target_folder_path.empty()) {
            res.error_message = L"无效的目标归档目录: " + g.folder_name;
            return res;
        }

        if (!IsDirectoryOnDisk(g.target_folder_path)) {
            if (PathExistsOnDisk(g.target_folder_path)) {
                res.error_message = L"目标路径已被同名文件占用: " + g.folder_name;
                return res;
            }
            if (!CreateDirectoryW(g.target_folder_path.c_str(), nullptr)) {
                const DWORD err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS || !IsDirectoryOnDisk(g.target_folder_path)) {
                    res.error_message = L"无法创建归档文件夹: " + g.folder_name;
                    return res;
                }
            } else {
                res.undo_tx.created_folders.push_back(g.target_folder_path);
                ++res.created_folder_count;
            }
        }

        size_t group_moved = 0;
        for (const auto& it : g.items) {
            if (!it.selected) continue;
            if (it.source_path.empty() || !PathExistsOnDisk(it.source_path)) {
                res.error_message = L"源文件不存在或已被移动: " + it.name;
                return res;
            }

            const std::wstring dest_path = MakeCollisionSafeDestination(
                g.target_folder_path, it.name, it.is_folder, it.source_path);
            if (!MoveFileExW(it.source_path.c_str(), dest_path.c_str(), MOVEFILE_COPY_ALLOWED)) {
                res.error_message = L"移动文件失败: " + it.name;
                return res;
            }

            res.undo_tx.moved_items.push_back(ArchivedFileMoveRecord{it.source_path, dest_path});
            moved_sources_lower.insert(ToLowerWide(it.source_path));
            ++res.moved_count;
            ++group_moved;
        }

        if (group_moved > 0) {
            active_archive_folders.emplace_back(g.folder_name, g.target_folder_path);
        }
    }

    if (out_updated_model) {
        std::erase_if(out_updated_model->items, [&](const model::DesktopItem& item) {
            return moved_sources_lower.contains(ToLowerWide(item.path));
        });

        const int64_t now_ms = CurrentUnixMs();
        for (size_t i = 0; i < active_archive_folders.size(); ++i) {
            const auto& [fname, fpath] = active_archive_folders[i];
            bool found_item = false;
            for (auto& existing_item : out_updated_model->items) {
                if (EqualsIgnoreCaseW(existing_item.path, fpath)) {
                    existing_item.is_folder = true;
                    existing_item.folder_is_empty = false;
                    existing_item.modified_unix_ms = now_ms;
                    found_item = true;
                    break;
                }
            }
            if (!found_item) {
                model::DesktopItem folder_item;
                folder_item.path = fpath;
                folder_item.name = fname;
                folder_item.is_folder = true;
                folder_item.folder_is_empty = false;
                folder_item.modified_unix_ms = now_ms;
                out_updated_model->items.push_back(std::move(folder_item));
            }

            if (pin_created_folders_to_tiles) {
                const bool already_pinned = std::any_of(
                    out_updated_model->tiles.begin(), out_updated_model->tiles.end(),
                    [&](const model::DesktopTile& t) {
                        return t.kind == model::TileKind::PinnedFolder &&
                               EqualsIgnoreCaseW(t.target_path, fpath);
                    });
                if (!already_pinned) {
                    model::DesktopTile tile;
                    tile.id = "ai_archive_" + std::to_string(now_ms) + "_" + std::to_string(i);
                    tile.kind = model::TileKind::PinnedFolder;
                    tile.title = fname;
                    tile.target_path = fpath;
                    tile.size = {1, 1};

                    auto cell_occupied = [&](int c, int r) {
                        for (const auto& et : out_updated_model->tiles) {
                            if (c >= et.col && c < et.col + et.size.cols &&
                                r >= et.row && r < et.row + et.size.rows) {
                                return true;
                            }
                        }
                        return false;
                    };

                    bool pinned_ok = false;
                    // First pass: place directly into an unoccupied 1x1 slot (e.g. (4,5), (4,6) on starter board).
                    for (int r = 0; r < model::kTileRows && !pinned_ok; ++r) {
                        for (int c = 0; c < model::kTileCols && !pinned_ok; ++c) {
                            if (cell_occupied(c, r)) continue;
                            tile.col = c;
                            tile.row = r;
                            const auto placed = model::InsertNewTile(out_updated_model->tiles, tile);
                            if (placed.valid) {
                                out_updated_model->tiles = placed.updated_tiles;
                                pinned_ok = true;
                            }
                        }
                    }
                    // Second pass: try displacing a smaller tile if no empty 1x1 cell existed.
                    for (int r = 0; r < model::kTileRows && !pinned_ok; ++r) {
                        for (int c = 0; c < model::kTileCols && !pinned_ok; ++c) {
                            tile.col = c;
                            tile.row = r;
                            const auto placed = model::InsertNewTile(out_updated_model->tiles, tile);
                            if (placed.valid) {
                                out_updated_model->tiles = placed.updated_tiles;
                                pinned_ok = true;
                            }
                        }
                    }
                }
            }
        }
    }

    res.ok = true;
    return res;
}

bool UndoAiArchiveTransaction(const AiArchiveUndoTransaction& tx,
                              model::DesktopModelState& out_restored_model,
                              size_t* out_restored_files) {
    size_t restored = 0;
    bool all_ok = true;

    for (auto it = tx.moved_items.rbegin(); it != tx.moved_items.rend(); ++it) {
        if (it->archived_path.empty() || it->original_path.empty()) continue;
        if (!PathExistsOnDisk(it->archived_path)) {
            all_ok = false;
            continue;
        }

        std::wstring target_orig = it->original_path;
        if (PathExistsOnDisk(target_orig) && !EqualsIgnoreCaseW(target_orig, it->archived_path)) {
            const size_t slash = it->original_path.find_last_of(L"\\/");
            const std::wstring parent = (slash == std::wstring::npos) ? std::wstring{} : it->original_path.substr(0, slash);
            const std::wstring leaf = (slash == std::wstring::npos) ? it->original_path : it->original_path.substr(slash + 1);
            const bool is_dir = IsDirectoryOnDisk(it->archived_path);
            target_orig = MakeCollisionSafeDestination(parent, leaf, is_dir, it->archived_path);
        }

        if (MoveFileExW(it->archived_path.c_str(), target_orig.c_str(), MOVEFILE_COPY_ALLOWED)) {
            ++restored;
        } else {
            all_ok = false;
        }
    }

    // Remove newly created archive directories in reverse order only when they are empty.
    for (auto it = tx.created_folders.rbegin(); it != tx.created_folders.rend(); ++it) {
        if (!it->empty()) {
            RemoveDirectoryW(it->c_str());
        }
    }

    out_restored_model = tx.pre_archive_model;
    if (out_restored_files) {
        *out_restored_files = restored;
    }
    return all_ok;
}

void ApplyAiPlanAsVirtualGroups(const AiOrganizePlan& plan,
                                model::DesktopModelState& inout_model,
                                const model::RectI& work_area,
                                const model::RectI& tile_board_rect) {
    int next_offset = 0;
    for (const auto& g : plan.groups) {
        if (g.SelectedCount() == 0) continue;

        std::string group_id;
        for (const auto& existing : inout_model.groups) {
            if (EqualsIgnoreCaseW(existing.title, g.folder_name)) {
                group_id = existing.id;
                break;
            }
        }

        if (group_id.empty()) {
            group_id = "ai_vgroup_" + std::to_string(inout_model.groups.size() + 1) + "_" +
                       std::to_string(GetTickCount64() & 0xFFFFu);
            model::DesktopGroup ng;
            ng.id = group_id;
            ng.title = g.folder_name;
            ng.icon_glyph = L'\xE8B7';
            ng.rule_summary = g.reason.empty() ? L"AI 智能虚拟分组" : g.reason;
            ng.cols = 4;
            ng.visible_rows = 2;
            ng.x = model::SnapCoord(work_area.x + 24 + (next_offset % 2) * 384);
            ng.y = model::SnapCoord(work_area.y + 24 + (next_offset / 2) * 240);
            ++next_offset;
            inout_model.groups.push_back(std::move(ng));
        }

        for (const auto& entry : g.items) {
            if (!entry.selected) continue;
            for (auto& item : inout_model.items) {
                if (EqualsIgnoreCaseW(item.path, entry.source_path) ||
                    EqualsIgnoreCaseW(item.name, entry.name)) {
                    item.pinned_group_id = group_id;
                    item.force_loose = false;
                    break;
                }
            }
        }
    }

    model::ResolveGroupCollisions(inout_model.groups, std::nullopt, work_area, tile_board_rect,
                                  inout_model.settings.icon_size);
}

// ---------------------------------------------------------------------------
// AiOrganizeSession (Async WinHTTP OpenAI-Compatible Client + Instant Cancel)
// ---------------------------------------------------------------------------
struct AiOrganizeSession::Impl {
    std::mutex mu;
    HINTERNET active_request = nullptr;
    std::atomic<bool> cancelled{false};

    void CloseActiveRequest() noexcept {
        std::lock_guard<std::mutex> lock(mu);
        if (active_request) {
            WinHttpCloseHandle(active_request);
            active_request = nullptr;
        }
    }
};

namespace {

struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET v) noexcept : h(v) {}
    ~WinHttpHandle() noexcept {
        if (h) WinHttpCloseHandle(h);
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};

struct CrackedUrl {
    std::wstring host;
    std::wstring base_path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
};

bool CrackEndpointUrl(const std::wstring& base_url, CrackedUrl& out) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host_buf[256]{};
    wchar_t path_buf[1024]{};
    parts.lpszHostName = host_buf;
    parts.dwHostNameLength = ARRAYSIZE(host_buf);
    parts.lpszUrlPath = path_buf;
    parts.dwUrlPathLength = ARRAYSIZE(path_buf);
    if (!WinHttpCrackUrl(base_url.c_str(), static_cast<DWORD>(base_url.size()), 0, &parts)) {
        return false;
    }
    out.host.assign(host_buf, parts.dwHostNameLength);
    out.base_path.assign(path_buf, parts.dwUrlPathLength);
    while (!out.base_path.empty() && out.base_path.back() == L'/') {
        out.base_path.pop_back();
    }
    out.port = parts.nPort;
    out.secure = (parts.nScheme == INTERNET_SCHEME_HTTPS);
    return !out.host.empty();
}

std::wstring ExtractHttpErrorDetail(std::string_view body, int http_status) {
    JsonVal root;
    if (ParseJson(body, root) && root.is_object()) {
        if (const JsonVal* err = root.find("error"); err && err->is_object()) {
            const std::string msg = err->get_string("message");
            if (!msg.empty()) {
                return L"AI 服务返回错误 (" + std::to_wstring(http_status) + L"): " + Utf8ToWide(msg);
            }
        }
        const std::string top_msg = root.get_string("message");
        if (!top_msg.empty()) {
            return L"AI 服务返回错误 (" + std::to_wstring(http_status) + L"): " + Utf8ToWide(top_msg);
        }
    }
    return L"AI 服务请求失败 (HTTP " + std::to_wstring(http_status) + L")";
}

bool PerformChatCompletionCall(const std::shared_ptr<AiOrganizeSession::Impl>& impl,
                               const CrackedUrl& url,
                               std::wstring& io_headers,
                               const std::string& request_body,
                               int& out_http_status,
                               std::string& out_assembled_content,
                               std::string& out_raw_error_body,
                               std::wstring& out_net_error) {
    out_http_status = 0;
    out_assembled_content.clear();
    out_raw_error_body.clear();
    out_net_error.clear();

    WinHttpHandle session(WinHttpOpen(L"PulseDesktopAI/1.0",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        session.h = WinHttpOpen(L"PulseDesktopAI/1.0",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.h) {
        out_net_error = L"无法初始化 WinHTTP 会话";
        return false;
    }

    WinHttpSetTimeouts(session.h, 8000, 12000, 20000, 90000);
    if (url.secure) {
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        }
    }

    WinHttpHandle connect(WinHttpConnect(session.h, url.host.c_str(), url.port, 0));
    if (!connect.h) {
        out_net_error = L"无法连接至 AI 服务主机: " + url.host;
        return false;
    }

    const std::wstring full_path = url.base_path + L"/chat/completions";
    HINTERNET req = WinHttpOpenRequest(connect.h, L"POST", full_path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       url.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        out_net_error = L"无法创建 HTTP 请求";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl->mu);
        if (impl->cancelled.load(std::memory_order_acquire)) {
            WinHttpCloseHandle(req);
            return false;
        }
        impl->active_request = req;
    }

    struct ActiveRequestReset {
        AiOrganizeSession::Impl& st;
        ~ActiveRequestReset() noexcept { st.CloseActiveRequest(); }
    } req_guard{*impl};

    const BOOL sent = WinHttpSendRequest(
        req,
        io_headers.c_str(),
        static_cast<DWORD>(io_headers.size()),
        const_cast<char*>(request_body.data()),
        static_cast<DWORD>(request_body.size()),
        static_cast<DWORD>(request_body.size()),
        0);
    if (!sent || !WinHttpReceiveResponse(req, nullptr)) {
        if (!impl->cancelled.load(std::memory_order_acquire)) {
            out_net_error = L"网络连接中断或无法访问 AI 服务端点";
        }
        return false;
    }

    DWORD status_code = 0;
    DWORD status_sz = sizeof(status_code);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_sz, WINHTTP_NO_HEADER_INDEX);
    out_http_status = static_cast<int>(status_code);

    std::string raw_response;
    std::string sse_pending;
    bool saw_sse_data = false;
    bool sse_done = false;
    std::vector<char> chunk(16 * 1024);

    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) {
            if (!impl->cancelled.load(std::memory_order_acquire)) {
                out_net_error = L"读取 AI 响应流时连接中断";
            }
            return false;
        }
        if (read == 0) break;

        if (status_code != 200) {
            if (out_raw_error_body.size() < 64 * 1024) {
                out_raw_error_body.append(chunk.data(), read);
            }
            continue;
        }

        if (raw_response.size() < 2 * 1024 * 1024) {
            raw_response.append(chunk.data(), read);
        }

        sse_pending.append(chunk.data(), read);
        size_t start = 0;
        for (size_t nl; (nl = sse_pending.find('\n', start)) != std::string::npos; start = nl + 1) {
            std::string_view line(sse_pending.data() + start, nl - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!line.starts_with("data:")) continue;
            saw_sse_data = true;
            line.remove_prefix(5);
            while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
            if (line == "[DONE]") {
                sse_done = true;
                break;
            }
            JsonVal ev;
            if (ParseJson(line, ev) && ev.is_object()) {
                if (const JsonVal* choices = ev.find("choices");
                    choices && choices->is_array() && !choices->arr_val.empty()) {
                    const JsonVal& c0 = choices->arr_val[0];
                    if (const JsonVal* delta = c0.find("delta"); delta && delta->is_object()) {
                        out_assembled_content += delta->get_string("content");
                    } else if (const JsonVal* msg = c0.find("message"); msg && msg->is_object()) {
                        out_assembled_content += msg->get_string("content");
                    }
                }
            }
        }
        sse_pending.erase(0, start);
        if (sse_done || impl->cancelled.load(std::memory_order_acquire)) break;
    }

    if (impl->cancelled.load(std::memory_order_acquire)) return false;
    if (status_code != 200) return false;

    // Fallback for servers/relays returning a single non-SSE JSON completion body.
    if (!saw_sse_data && out_assembled_content.empty() && !raw_response.empty()) {
        JsonVal full;
        if (ParseJson(raw_response, full) && full.is_object()) {
            if (const JsonVal* choices = full.find("choices");
                choices && choices->is_array() && !choices->arr_val.empty()) {
                const JsonVal& c0 = choices->arr_val[0];
                if (const JsonVal* msg = c0.find("message"); msg && msg->is_object()) {
                    out_assembled_content = msg->get_string("content");
                }
            }
        }
    }

    return !out_assembled_content.empty();
}

void RunAiSessionWorker(std::shared_ptr<AiOrganizeSession::Impl> impl,
                        AiOrganizeSession::Request req,
                        std::function<void(AiOrganizeSession::Outcome)> on_complete) {
    auto finish = [&](AiOrganizeSession::Outcome out) {
        if (on_complete) {
            on_complete(std::move(out));
        }
    };

    if (impl->cancelled.load(std::memory_order_acquire)) {
        AiOrganizeSession::Outcome out;
        out.cancelled = true;
        finish(std::move(out));
        return;
    }

    const int64_t now_ms = (req.now_unix_ms > 0) ? req.now_unix_ms : CurrentUnixMs();
    std::vector<DesktopAiCandidate> candidates;
    std::vector<ExistingDesktopFolder> existing_folders;
    CollectDesktopAiContext(req.desktop_dir, req.items, req.groups, now_ms, candidates, existing_folders);

    if (impl->cancelled.load(std::memory_order_acquire)) {
        AiOrganizeSession::Outcome out;
        out.cancelled = true;
        finish(std::move(out));
        return;
    }

    if (req.force_local_fallback) {
        AiOrganizeSession::Outcome out;
        out.ok = true;
        out.plan = BuildSmartLocalOrganizePlan(req.desktop_dir, req.strategy, req.custom_instruction,
                                               candidates, existing_folders);
        finish(std::move(out));
        return;
    }

    std::wstring api_key;
    ScopedWideSecretWipe key_wipe{api_key};
    const AiConnectionInfo conn = ResolvePulseAiConnectionAndKey(req.override_ai_dir, &api_key);
    if (!conn.configured) {
        WipeWideString(api_key);
        AiOrganizeSession::Outcome out;
        out.unconfigured = true;
        out.error_message = conn.assistant_enabled
                                ? L"尚未在 Pulse 设置中连接 AI 助手密钥"
                                : L"Pulse AI 助手当前处于关闭状态";
        finish(std::move(out));
        return;
    }

    CrackedUrl url;
    if (!CrackEndpointUrl(conn.base_url, url)) {
        WipeWideString(api_key);
        AiOrganizeSession::Outcome out;
        out.error_message = L"AI 服务地址格式无效: " + conn.base_url;
        finish(std::move(out));
        return;
    }

    std::string body_primary = BuildAiOrganizeChatBody(
        conn.service_id, conn.model_id, req.strategy, req.custom_instruction,
        candidates, existing_folders);

    std::wstring headers = L"Content-Type: application/json\r\nAccept: text/event-stream, application/json\r\n";
    if (!api_key.empty()) {
        headers += L"Authorization: Bearer " + api_key + L"\r\n";
        const std::wstring anthropic_host = L"api.anthropic.com";
        if (url.host.size() >= anthropic_host.size() &&
            EqualsIgnoreCaseW(std::wstring_view(url.host).substr(url.host.size() - anthropic_host.size()),
                              anthropic_host)) {
            headers += L"x-api-key: " + api_key + L"\r\nanthropic-version: 2023-06-01\r\n";
        }
    }
    WipeWideString(api_key);
    ScopedWideSecretWipe headers_wipe{headers};

    int http_status = 0;
    std::string content;
    std::string raw_err;
    std::wstring net_err;
    bool call_ok = PerformChatCompletionCall(impl, url, headers, body_primary,
                                             http_status, content, raw_err, net_err);

    // Retry once without thinking suppression if an endpoint returned HTTP 400 rejecting extra parameters.
    if (!call_ok && !impl->cancelled.load(std::memory_order_acquire) && http_status == 400) {
        const std::string body_plain = BuildAiOrganizeChatBody(
            "claude", conn.model_id, req.strategy, req.custom_instruction,
            candidates, existing_folders);
        if (body_plain != body_primary) {
            call_ok = PerformChatCompletionCall(impl, url, headers, body_plain,
                                                http_status, content, raw_err, net_err);
        }
    }
    WipeWideString(headers);

    if (impl->cancelled.load(std::memory_order_acquire)) {
        AiOrganizeSession::Outcome out;
        out.cancelled = true;
        finish(std::move(out));
        return;
    }

    if (!call_ok) {
        AiOrganizeSession::Outcome out;
        out.http_status = http_status;
        out.error_message = (http_status != 0 && http_status != 200)
                                ? ExtractHttpErrorDetail(raw_err, http_status)
                                : (net_err.empty() ? L"AI 服务未返回有效内容" : net_err);
        finish(std::move(out));
        return;
    }

    AiOrganizePlan plan;
    std::wstring parse_err;
    if (!ParseAndValidateAiOrganizeReply(content, req.desktop_dir, candidates, existing_folders, plan, parse_err)) {
        AiOrganizeSession::Outcome out;
        out.http_status = http_status;
        out.error_message = parse_err;
        finish(std::move(out));
        return;
    }

    plan.from_ai_model = true;
    plan.model_badge = conn.badge_text();
    plan.desktop_before_count = candidates.size();

    AiOrganizeSession::Outcome out;
    out.ok = true;
    out.http_status = http_status;
    out.plan = std::move(plan);
    finish(std::move(out));
}

} // namespace

std::shared_ptr<AiOrganizeSession> AiOrganizeSession::Start(Request req,
                                                            std::function<void(Outcome)> on_complete) {
    std::shared_ptr<AiOrganizeSession> session(new AiOrganizeSession());
    session->impl_ = std::make_shared<Impl>();
    std::thread(RunAiSessionWorker, session->impl_, std::move(req), std::move(on_complete)).detach();
    return session;
}

AiOrganizeSession::~AiOrganizeSession() {
    Cancel();
}

void AiOrganizeSession::Cancel() noexcept {
    if (!impl_) return;
    impl_->cancelled.store(true, std::memory_order_release);
    impl_->CloseActiveRequest();
}

bool AiOrganizeSession::IsCancelled() const noexcept {
    return !impl_ || impl_->cancelled.load(std::memory_order_acquire);
}

} // namespace pulse::desktop::ai

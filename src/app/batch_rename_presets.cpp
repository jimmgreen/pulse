// batch_rename_presets.cpp - see batch_rename_presets.h.
#include "batch_rename_presets.h"

#include "../common/json_value.h"

#include <windows.h>

#include <algorithm>
#include <iterator>

namespace pulse::app::rename {
namespace {

using json::Value;

const char* TypeKey(StepType t) {
    switch (t) {
    case StepType::Replace: return "replace";
    case StepType::Remove: return "remove";
    case StepType::Case: return "case";
    case StepType::Template: return "template";
    case StepType::Ext: return "ext";
    case StepType::Insert: return "insert";
    case StepType::Number: return "number";
    }
    return "replace";
}

bool TypeFromKey(const std::string& k, StepType& t) {
    static const std::pair<const char*, StepType> map[] = {
        {"replace", StepType::Replace}, {"remove", StepType::Remove}, {"case", StepType::Case},
        {"template", StepType::Template}, {"ext", StepType::Ext}, {"insert", StepType::Insert},
        {"number", StepType::Number}};
    for (const auto& [key, type] : map)
        if (k == key) { t = type; return true; }
    return false;
}

int GetInt(const Value& o, const char* key, int fallback, int lo, int hi) {
    int64_t v = 0;
    if (const Value* m = o.find(key); m && m->get_int(v, lo, hi)) return static_cast<int>(v);
    return fallback;
}

Value StepToJson(const Step& st) {
    Value o = Value::Object();
    o.set("type", TypeKey(st.type));
    o.set("on", st.on);
    o.set("find", json::Utf8(st.find));
    o.set("with", json::Utf8(st.with));
    o.set("regex", st.regex);
    o.set("match_case", st.match_case);
    o.set("all", st.all);
    o.set("brackets", st.brackets);
    o.set("spaces", st.spaces);
    o.set("digits", st.digits);
    o.set("custom", json::Utf8(st.custom));
    o.set("mode", static_cast<int>(st.mode));
    o.set("ext_to", json::Utf8(st.ext_to));
    o.set("pattern", json::Utf8(st.pattern));
    o.set("start", st.start);
    o.set("digits_n", st.digits_n);
    o.set("stride", st.stride);
    o.set("text", json::Utf8(st.text));
    o.set("where", static_cast<int>(st.where));
    o.set("at", st.at);
    o.set("sep", json::Utf8(st.sep));
    return o;
}

bool StepFromJson(const Value& o, Step& st) {
    if (!o.is_object() || !TypeFromKey(o.get_string("type"), st.type)) return false;
    const Step d;
    st.on = o.get_bool("on", true);
    st.find = json::Wide(o.get_string("find"));
    st.with = json::Wide(o.get_string("with"));
    st.regex = o.get_bool("regex", d.regex);
    st.match_case = o.get_bool("match_case", d.match_case);
    st.all = o.get_bool("all", d.all);
    st.brackets = o.get_bool("brackets", d.brackets);
    st.spaces = o.get_bool("spaces", d.spaces);
    st.digits = o.get_bool("digits", d.digits);
    st.custom = json::Wide(o.get_string("custom"));
    st.mode = static_cast<CaseMode>(GetInt(o, "mode", static_cast<int>(d.mode), 0, static_cast<int>(CaseMode::Change)));
    st.ext_to = json::Wide(o.get_string("ext_to"));
    st.pattern = json::Wide(o.get_string("pattern"));
    st.start = GetInt(o, "start", d.start, 0, 999999);
    st.digits_n = GetInt(o, "digits_n", d.digits_n, 1, 9);
    st.stride = GetInt(o, "stride", d.stride, 1, 9999);
    st.text = json::Wide(o.get_string("text"));
    st.where = static_cast<Where>(GetInt(o, "where", static_cast<int>(d.where), 0, static_cast<int>(Where::Index)));
    st.at = GetInt(o, "at", d.at, 0, 9999);
    if (const Value* sep = o.find("sep"); sep && sep->is_string()) st.sep = json::Wide(sep->as_string());
    return true;
}

Value PresetToJson(const Preset& p) {
    Value o = Value::Object();
    o.set("name", json::Utf8(p.name));
    o.set("ext_too", p.opts.ext_too);
    o.set("order", static_cast<int>(p.opts.order));
    o.set("policy", static_cast<int>(p.opts.policy));
    if (p.opts.daily) o.set("daily", true);
    Value steps = Value::Array();
    for (const Step& st : p.steps) steps.push(StepToJson(st));
    o.set("steps", std::move(steps));
    return o;
}

bool PresetFromJson(const Value& o, Preset& p) {
    if (!o.is_object()) return false;
    p.name = json::Wide(o.get_string("name"));
    p.opts.ext_too = o.get_bool("ext_too", false);
    p.opts.order = static_cast<Order>(GetInt(o, "order", 0, 0, static_cast<int>(Order::Name)));
    p.opts.policy = static_cast<Policy>(GetInt(o, "policy", 0, 0, static_cast<int>(Policy::Block)));
    p.opts.daily = o.get_bool("daily", false);
    p.steps.clear();
    if (const Value* steps = o.find("steps"); steps && steps->is_array())
        for (const Value& s : steps->items()) {
            Step st;
            if (StepFromJson(s, st)) p.steps.push_back(std::move(st));
        }
    return true;
}

std::wstring StorePath() {
    wchar_t root[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", root, static_cast<DWORD>(std::size(root)));
    if (n == 0 || n >= std::size(root)) return {};
    return std::wstring(root) + L"\\Pulse\\rename_presets.json";
}

} // namespace

std::string StepsKey(const std::vector<Step>& steps, const Options& opts) {
    Preset p;
    p.steps = steps;
    p.opts = opts;
    return json::Serialize(PresetToJson(p));
}

Preset InitialPreset(const PresetStore& store, const Preset& fallback) {
    return store.has_last && !store.last.steps.empty() ? store.last : fallback;
}

std::string PresetsToJson(const PresetStore& store) {
    Value root = Value::Object();
    root.set("version", 1);
    Value list = Value::Array();
    for (const Preset& p : store.presets) list.push(PresetToJson(p));
    root.set("presets", std::move(list));
    if (store.has_last) root.set("last", PresetToJson(store.last));
    return json::Serialize(root);
}

bool PresetsFromJson(const std::string& utf8, PresetStore& out) {
    Value root;
    if (!json::Parse(utf8, root) || !root.is_object()) return false;
    out = {};
    if (const Value* list = root.find("presets"); list && list->is_array())
        for (const Value& v : list->items()) {
            Preset p;
            if (PresetFromJson(v, p) && !p.name.empty()) out.presets.push_back(std::move(p));
        }
    if (const Value* last = root.find("last"); last && last->is_object())
        out.has_last = PresetFromJson(*last, out.last) && !out.last.steps.empty();
    return true;
}

PresetStore LoadPresets() {
    PresetStore store;
    const std::wstring path = StorePath();
    if (path.empty()) return store;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return store;
    LARGE_INTEGER size{};
    std::string text;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) read = 0;
        text.resize(read);
    }
    CloseHandle(f);
    if (!PresetsFromJson(text, store)) store = {};
    return store;
}

bool SavePresets(const PresetStore& store) {
    const std::wstring path = StorePath();
    if (path.empty()) return false;
    CreateDirectoryW(path.substr(0, path.rfind(L'\\')).c_str(), nullptr);
    const std::string text = PresetsToJson(store);
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace pulse::app::rename

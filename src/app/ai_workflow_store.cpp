// ai_workflow_store.cpp - Reusable local workflow rules store (ai_workflow_store.h).
#include "ai_workflow_store.h"

#include "../common/json_value.h"
#include "../common/utf8_file.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace pulse::app::ai {
namespace {

constexpr size_t kMaxWorkflows = 32;

std::string TrimUtf8(std::string s) {
    auto ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t a = 0;
    while (a < s.size() && ws(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && ws(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string LowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::wstring DefaultAiDir() {
    wchar_t buf[32768]{};
    DWORD n = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        std::wstring d(buf, n);
        while (!d.empty() && d.back() == L'\\') d.pop_back();
        return d + L"\\ai";
    }
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    std::wstring d(buf, n);
    while (!d.empty() && d.back() == L'\\') d.pop_back();
    return d + L"\\Pulse\\ai";
}

void EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return;
    size_t pos = 0;
    if (dir.size() >= 2 && dir[1] == L':') pos = 3;
    while (pos <= dir.size()) {
        const size_t next = dir.find(L'\\', pos);
        const std::wstring sub = next == std::wstring::npos ? dir : dir.substr(0, next);
        if (!sub.empty()) CreateDirectoryW(sub.c_str(), nullptr);
        if (next == std::wstring::npos) break;
        pos = next + 1;
    }
}

std::string MakeWorkflowId(const std::vector<WorkflowRule>& rules, int64_t now) {
    char base[32]{};
    sprintf_s(base, "wf%lld", static_cast<long long>(now));
    std::string id = base;
    for (int seq = 1; seq < 1000; ++seq) {
        bool clash = false;
        for (const auto& r : rules)
            if (r.id == id) { clash = true; break; }
        if (!clash) return id;
        id = std::string(base) + "_" + std::to_string(seq);
    }
    return id;
}

} // namespace

std::wstring WorkflowsFilePath(const std::wstring& dir_override) {
    std::wstring dir = dir_override.empty() ? DefaultAiDir() : dir_override;
    if (dir.empty()) return {};
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    return dir + L"\\workflows.json";
}

std::vector<WorkflowRule> LoadWorkflows(const std::wstring& dir_override) {
    std::vector<WorkflowRule> out;
    const std::wstring path = WorkflowsFilePath(dir_override);
    if (path.empty()) return out;
    std::wstring wide;
    if (!ReadUtf8File(path, wide)) return out;
    json::Value root;
    if (!json::Parse(json::Utf8(wide), root) || !root.is_object()) return out;
    const json::Value* arr = root.find("workflows");
    if (!arr || !arr->is_array()) return out;
    for (const json::Value& item : arr->items()) {
        if (!item.is_object() || out.size() >= kMaxWorkflows) continue;
        WorkflowRule r;
        r.id = item.get_string("id");
        r.name = TrimUtf8(item.get_string("name"));
        r.description = TrimUtf8(item.get_string("description"));
        r.steps = TrimUtf8(item.get_string("steps"));
        if (const json::Value* c = item.find("created")) c->get_int(r.created, 0, INT64_MAX);
        if (!r.id.empty() && !r.name.empty() && !r.steps.empty()) out.push_back(std::move(r));
    }
    return out;
}

bool SaveWorkflows(const std::vector<WorkflowRule>& rules, const std::wstring& dir_override) {
    const std::wstring path = WorkflowsFilePath(dir_override);
    if (path.empty()) return false;
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) EnsureDir(path.substr(0, slash));
    json::Value root = json::Value::Object();
    root.set("version", 1);
    json::Value arr = json::Value::Array();
    for (size_t i = 0; i < rules.size() && i < kMaxWorkflows; ++i) {
        json::Value o = json::Value::Object();
        o.set("id", rules[i].id);
        o.set("name", rules[i].name);
        o.set("description", rules[i].description);
        o.set("steps", rules[i].steps);
        o.set("created", rules[i].created);
        arr.push(std::move(o));
    }
    root.set("workflows", std::move(arr));
    return WriteUtf8FileAtomic(path, json::Wide(json::Serialize(root)));
}

bool UpsertWorkflow(const std::string& name, const std::string& description, const std::string& steps,
                    WorkflowRule& out_rule, const std::wstring& dir_override) {
    const std::string clean_name = TrimUtf8(name);
    const std::string clean_desc = TrimUtf8(description);
    const std::string clean_steps = TrimUtf8(steps);
    if (clean_name.empty() || clean_steps.empty()) return false;
    std::vector<WorkflowRule> rules = LoadWorkflows(dir_override);
    const std::string key = LowerAscii(clean_name);
    for (WorkflowRule& r : rules) {
        if (LowerAscii(r.name) == key) {
            r.name = clean_name;
            r.description = clean_desc;
            r.steps = clean_steps;
            if (!SaveWorkflows(rules, dir_override)) return false;
            out_rule = r;
            return true;
        }
    }
    if (rules.size() >= kMaxWorkflows) rules.erase(rules.begin());
    WorkflowRule added;
    added.created = static_cast<int64_t>(std::time(nullptr));
    added.id = MakeWorkflowId(rules, added.created);
    added.name = clean_name;
    added.description = clean_desc;
    added.steps = clean_steps;
    rules.push_back(added);
    if (!SaveWorkflows(rules, dir_override)) return false;
    out_rule = std::move(added);
    return true;
}

size_t RemoveWorkflow(const std::string& id_or_name, const std::wstring& dir_override) {
    const std::string q = LowerAscii(TrimUtf8(id_or_name));
    if (q.empty()) return 0;
    std::vector<WorkflowRule> rules = LoadWorkflows(dir_override);
    const size_t before = rules.size();
    rules.erase(std::remove_if(rules.begin(), rules.end(), [&](const WorkflowRule& r) {
                    return LowerAscii(r.id) == q || LowerAscii(r.name) == q;
                }),
                rules.end());
    const size_t removed = before - rules.size();
    if (removed > 0) SaveWorkflows(rules, dir_override);
    return removed;
}

std::string FormatWorkflowsForPrompt(const std::vector<WorkflowRule>& rules) {
    if (rules.empty()) return {};
    std::string out = "\nSaved reusable workflows on this PC (apply when the user invokes one by name):\n";
    for (const WorkflowRule& r : rules) {
        out += "- [" + r.name + "]";
        if (!r.description.empty()) out += " (" + r.description + ")";
        out += ": " + r.steps + "\n";
    }
    return out;
}

} // namespace pulse::app::ai

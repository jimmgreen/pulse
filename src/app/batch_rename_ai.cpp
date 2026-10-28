// batch_rename_ai.cpp - see batch_rename_ai.h.
#include "batch_rename_ai.h"

#include "../common/json_value.h"

#include <algorithm>
#include <string_view>

namespace pulse::app::rename {
namespace {

using json::Value;

constexpr const char* kPrompt =
    "You turn one sentence about renaming files into steps for a batch rename tool. "
    "Reply with exactly one JSON object and nothing else:\n"
    "{\"steps\":[...],\"order\":\"list\",\"daily\":false,\"note\":\"\"}\n"
    "\n"
    "Steps run top to bottom on the name without its extension (only \"ext\" touches the extension). "
    "Step objects:\n"
    "- {\"type\":\"replace\",\"find\":\"text\",\"with\":\"text\",\"regex\":false,\"match_case\":false,\"all\":true} "
    "regex is ECMAScript; $1 in \"with\" is a group. all=false replaces only the first match.\n"
    "- {\"type\":\"remove\",\"brackets\":false,\"spaces\":false,\"digits\":false,\"custom\":\"\"} "
    "brackets: copy markers such as \" (1)\" or \"\xEF\xBC\x88" "2\xEF\xBC\x89\"; spaces: collapse repeated spaces and "
    "trim; digits: every digit; custom: literal text to delete.\n"
    "- {\"type\":\"insert\",\"text\":\"...\",\"where\":\"start\",\"at\":1} where is start, end or index "
    "(index: after \"at\" characters).\n"
    "- {\"type\":\"case\",\"mode\":\"lower\"} mode is lower, upper or title (capitalize each word).\n"
    "- {\"type\":\"template\",\"pattern\":\"...\",\"start\":1,\"digits\":3,\"stride\":1} replaces the whole name.\n"
    "- {\"type\":\"number\",\"where\":\"end\",\"sep\":\" \",\"start\":1,\"digits\":3,\"stride\":1} adds a running "
    "number at the start or end.\n"
    "- {\"type\":\"ext\",\"mode\":\"lower\",\"to\":\"\"} mode is lower, upper or set; \"to\" (no dot) only for set.\n"
    "\n"
    "Tokens in template patterns and insert text: {name} original name, {n} running number (start, digits, "
    "stride come from the step), {taken} date taken as yyyy-MM-dd (falls back to the modified date), "
    "{modified} modified date yyyy-MM-dd, {folder} parent folder name, {size} image size WxH.\n"
    "\"order\" is the numbering order: list (as shown), taken, modified or name. Use taken when photos are "
    "numbered by date; otherwise leave list. "
    "\"daily\":true starts the number again each day (by date taken), e.g. 2026-10-03_01, 2026-10-03_02, "
    "2026-10-04_01.\n"
    "\n"
    "Rules: use as few steps as possible; copy literal words exactly as the user wrote them; when spaces "
    "become another character, put a remove step with spaces:true first so a run of spaces gives one "
    "character; you never see "
    "the file names, so choose the most likely general steps. When the request cannot be done with these "
    "steps (it needs file contents, moving files, or is not about names), reply with \"steps\":[] and a "
    "one-sentence \"note\" in the user's language explaining why.";

std::string ExtList(const AiAsk& ask) {
    std::string out;
    size_t shown = 0;
    for (const auto& [ext, count] : ask.exts) {
        if (shown == 12) { out += ", ..."; break; }
        if (shown++) out += ", ";
        out += json::Utf8(ext.empty() ? std::wstring(L"(none)") : ext);
        out += " x" + std::to_string(count);
    }
    return out;
}

int ClampInt(const Value& o, std::string_view key, int fallback, int lo, int hi) {
    int64_t v = 0;
    const Value* p = o.find(key);
    if (!p || !p->is_number()) return fallback;
    if (p->get_int(v, lo, hi)) return static_cast<int>(v);
    const double d = p->as_number();
    return d < lo ? lo : d > hi ? hi : static_cast<int>(d);
}

std::wstring Str(const Value& o, std::string_view key) { return json::Wide(o.get_string(key)); }

bool WhereOf(const std::string& s, Where& out, bool allow_index) {
    if (s == "start") { out = Where::Start; return true; }
    if (s == "end" || s.empty()) { out = Where::End; return true; }
    if (s == "index" && allow_index) { out = Where::Index; return true; }
    return false;
}

bool ReadStep(const Value& o, Step& st) {
    if (!o.is_object()) return false;
    const std::string type = o.get_string("type");
    st = Step{};
    st.ai = true;
    if (type == "replace") {
        st.type = StepType::Replace;
        st.find = Str(o, "find");
        st.with = Str(o, "with");
        st.regex = o.get_bool("regex");
        st.match_case = o.get_bool("match_case");
        st.all = o.get_bool("all", true);
        return !st.find.empty();
    }
    if (type == "remove") {
        st.type = StepType::Remove;
        st.brackets = o.get_bool("brackets");
        st.spaces = o.get_bool("spaces");
        st.digits = o.get_bool("digits");
        st.custom = Str(o, "custom");
        return st.brackets || st.spaces || st.digits || !st.custom.empty();
    }
    if (type == "insert") {
        st.type = StepType::Insert;
        st.text = Str(o, "text");
        if (!WhereOf(o.get_string("where"), st.where, true)) return false;
        st.at = ClampInt(o, "at", 1, 0, 255);
        return !st.text.empty();
    }
    if (type == "case") {
        st.type = StepType::Case;
        const std::string m = o.get_string("mode");
        if (m == "lower") st.mode = CaseMode::Lower;
        else if (m == "upper") st.mode = CaseMode::Upper;
        else if (m == "title") st.mode = CaseMode::Title;
        else return false;
        return true;
    }
    if (type == "template") {
        st.type = StepType::Template;
        st.pattern = Str(o, "pattern");
        st.start = ClampInt(o, "start", 1, 0, 999999);
        st.digits_n = ClampInt(o, "digits", 3, 1, 9);
        st.stride = ClampInt(o, "stride", 1, 1, 1000);
        return !st.pattern.empty();
    }
    if (type == "number") {
        st.type = StepType::Number;
        if (!WhereOf(o.get_string("where"), st.where, false)) return false;
        if (const Value* sep = o.find("sep"); sep && sep->is_string()) st.sep = json::Wide(sep->as_string());
        st.start = ClampInt(o, "start", 1, 0, 999999);
        st.digits_n = ClampInt(o, "digits", 3, 1, 9);
        st.stride = ClampInt(o, "stride", 1, 1, 1000);
        return true;
    }
    if (type == "ext") {
        st.type = StepType::Ext;
        const std::string m = o.get_string("mode");
        if (m == "lower") st.mode = CaseMode::Lower;
        else if (m == "upper") st.mode = CaseMode::Upper;
        else if (m == "set") {
            st.mode = CaseMode::Change;
            st.ext_to = Str(o, "to");
            while (!st.ext_to.empty() && st.ext_to.front() == L'.') st.ext_to.erase(0, 1);
            return !st.ext_to.empty();
        } else return false;
        return true;
    }
    return false;
}

} // namespace

std::string AiRequestBody(const AiAsk& ask) {
    Value body = Value::Object();
    body.set("model", json::Utf8(ask.model));
    Value messages = Value::Array();
    Value sys = Value::Object();
    sys.set("role", "system");
    sys.set("content", kPrompt);
    messages.push(std::move(sys));
    Value user = Value::Object();
    user.set("role", "user");
    std::string text = "Files: " + std::to_string(ask.files);
    if (!ask.exts.empty()) text += " (" + ExtList(ask) + ")";
    text += "\nRequest: " + json::Utf8(ask.sentence);
    user.set("content", text);
    messages.push(std::move(user));
    body.set("messages", std::move(messages));
    body.set("stream", true);
    Value format = Value::Object();
    format.set("type", "json_object");
    body.set("response_format", std::move(format));
    // No thinking: the caller adds the off switch in the form its service takes
    // (ai_providers.h WithoutThinking).
    return json::Serialize(body);
}

bool ParseAiReply(const std::string& utf8, AiReply& out) {
    out = AiReply{};
    // Tolerate a code fence or a sentence around the object.
    const size_t a = utf8.find('{'), b = utf8.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a) return false;
    Value root;
    if (!json::Parse(std::string_view(utf8).substr(a, b - a + 1), root) || !root.is_object()) return false;
    return ReadAiSteps(root, out);
}

bool ReadAiSteps(const json::Value& root, AiReply& out) {
    out = AiReply{};
    if (!root.is_object()) return false;
    out.note = json::Wide(root.get_string("note"));
    if (const Value* steps = root.find("steps"); steps && steps->is_array()) {
        for (const Value& v : steps->items()) {
            Step st;
            if (ReadStep(v, st)) out.steps.push_back(std::move(st));
            if (out.steps.size() == 12) break;
        }
    }
    const std::string order = root.get_string("order");
    if (order == "taken") { out.has_order = true; out.order = Order::Taken; }
    else if (order == "modified") { out.has_order = true; out.order = Order::Modified; }
    else if (order == "name") { out.has_order = true; out.order = Order::Name; }
    else if (order == "list") { out.has_order = true; out.order = Order::List; }
    out.daily = root.get_bool("daily");
    return !out.steps.empty() || !out.note.empty();
}

} // namespace pulse::app::rename

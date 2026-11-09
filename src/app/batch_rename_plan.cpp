// batch_rename_plan.cpp — see batch_rename_plan.h.
#include "batch_rename_plan.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <map>
#include <regex>
#include <set>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace pulse::app::rename {
#ifdef PULSE_TEST_RENAME_REUSE
size_t regex_compile_attempts = 0;
#endif
namespace {

bool IsSpace(wchar_t c) { return c == L' ' || c == L'\t' || c == 0x3000 || c == 0x00A0; }
bool IsAsciiLetter(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }
bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

std::wstring MapCase(const std::wstring& s, bool upper) {
    if (s.empty()) return s;
#if defined(_WIN32)
    const DWORD flag = upper ? LCMAP_UPPERCASE : LCMAP_LOWERCASE;
    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, flag, s.c_str(), static_cast<int>(s.size()),
                                nullptr, 0, nullptr, nullptr, 0);
    if (n > 0) {
        std::wstring out(static_cast<size_t>(n), L'\0');
        LCMapStringEx(LOCALE_NAME_INVARIANT, flag, s.c_str(), static_cast<int>(s.size()),
                      out.data(), n, nullptr, nullptr, 0);
        return out;
    }
#endif
    std::wstring out = s;
    for (auto& c : out) c = static_cast<wchar_t>(upper ? towupper(c) : towlower(c));
    return out;
}

void Split(const std::wstring& name, std::wstring& stem, std::wstring& ext) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) { stem = name; ext.clear(); return; }
    stem = name.substr(0, dot);
    ext = name.substr(dot);
}

std::wstring ParentOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring Pad(long long v, int digits) {
    std::wstring s = std::to_wstring(v < 0 ? -v : v);
    digits = std::clamp(digits, 1, 9);
    if (static_cast<int>(s.size()) < digits) s.insert(0, static_cast<size_t>(digits) - s.size(), L'0');
    return v < 0 ? L"-" + s : s;
}

struct Civil { int y, mo, d, h, mi, s; };

// FILETIME ticks (already local time) -> civil date.
Civil ToCivil(uint64_t ticks) {
    const long long secs = static_cast<long long>(ticks / 10000000ULL) - 11644473600LL;
    long long days = secs >= 0 ? secs / 86400 : (secs - 86399) / 86400;
    long long rem = secs - days * 86400;
    Civil c{};
    c.h = static_cast<int>(rem / 3600);
    c.mi = static_cast<int>((rem % 3600) / 60);
    c.s = static_cast<int>(rem % 60);
    // days_from_civil inverse (H. Hinnant)
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long y = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    c.y = static_cast<int>(y + (m <= 2 ? 1 : 0));
    c.mo = static_cast<int>(m);
    c.d = static_cast<int>(d);
    return c;
}

std::wstring FormatDate(uint64_t ticks, const std::wstring& fmt_in) {
    const Civil c = ToCivil(ticks);
    const std::wstring fmt = fmt_in.empty() ? L"yyyy-MM-dd" : fmt_in;
    std::wstring out;
    for (size_t i = 0; i < fmt.size();) {
        auto at = [&](const wchar_t* t) {
            const size_t n = wcslen(t);
            return fmt.compare(i, n, t) == 0;
        };
        if (at(L"yyyy")) { out += Pad(c.y, 4); i += 4; }
        else if (at(L"yy")) { out += Pad(c.y % 100, 2); i += 2; }
        else if (at(L"MM")) { out += Pad(c.mo, 2); i += 2; }
        else if (at(L"dd")) { out += Pad(c.d, 2); i += 2; }
        else if (at(L"HH")) { out += Pad(c.h, 2); i += 2; }
        else if (at(L"mm")) { out += Pad(c.mi, 2); i += 2; }
        else if (at(L"ss")) { out += Pad(c.s, 2); i += 2; }
        else out += fmt[i++];
    }
    return out;
}

std::wstring EscapeRegex(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (wcschr(L".*+?^${}()|[]\\", c)) out += L'\\';
        out += c;
    }
    return out;
}

// Title case like the design: an ASCII letter after start/space/-/_/./(/（.
std::wstring TitleCase(const std::wstring& s) {
    std::wstring out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        const bool boundary = i == 0 || IsSpace(out[i - 1]) || wcschr(L"-_.(\x00FF08", out[i - 1]);
        if (boundary && out[i] >= L'a' && out[i] <= L'z') out[i] = static_cast<wchar_t>(out[i] - 32);
    }
    return out;
}

std::wstring RemoveBrackets(const std::wstring& s) {
    // \s*[(（]\d+[)）]
    std::wstring out;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i;
        while (j < s.size() && IsSpace(s[j])) ++j;
        if (j < s.size() && (s[j] == L'(' || s[j] == 0xFF08)) {
            size_t k = j + 1;
            const size_t digits_from = k;
            while (k < s.size() && IsDigit(s[k])) ++k;
            if (k > digits_from && k < s.size() && (s[k] == L')' || s[k] == 0xFF09)) {
                i = k + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

std::wstring CollapseSpaces(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        if (IsSpace(s[i])) {
            size_t j = i;
            while (j < s.size() && IsSpace(s[j])) ++j;
            if (j - i >= 2) out += L' ';
            else out += s[i];
            i = j;
        } else {
            out += s[i++];
        }
    }
    size_t lead = 0;
    while (lead < out.size() && IsSpace(out[lead])) ++lead;
    return out.substr(lead);
}

std::wstring RemoveDigits(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) if (!IsDigit(c)) out += c;
    return out;
}

std::wstring RemoveLiteral(const std::wstring& s, const std::wstring& what) {
    if (what.empty()) return s;
    std::wstring out = s;
    for (size_t p = out.find(what); p != std::wstring::npos; p = out.find(what, p)) out.erase(p, what.size());
    return out;
}

struct Ctx {
    const Source* src = nullptr;
    size_t index = 0;
    bool ext_too = false;
    bool fallback = false;
    bool bad_regex = false;
};

bool KeyIs(const std::wstring& k, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names) if (k == n) return true;
    return false;
}

std::wstring ExpandTemplate(const Step& st, const std::wstring& stem, const std::wstring& ext, Ctx& ctx) {
    const std::wstring& p = st.pattern;
    const long long n = static_cast<long long>(ctx.index) * std::max(1, st.stride) + st.start;
    std::wstring out;
    for (size_t i = 0; i < p.size();) {
        if (p[i] == L'{') {
            const size_t close = p.find(L'}', i + 1);
            if (close != std::wstring::npos) {
                std::wstring key = p.substr(i + 1, close - i - 1), fmt;
                const size_t colon = key.find(L':');
                if (colon != std::wstring::npos) { fmt = key.substr(colon + 1); key.resize(colon); }
                bool known = true;
                if (KeyIs(key, {L"原名", L"name"})) out += stem;
                else if (KeyIs(key, {L"扩展名", L"ext"})) out += ext;
                else if (KeyIs(key, {L"序号", L"序號", L"n", L"number"})) {
                    int digits = st.digits_n;
                    if (!fmt.empty()) digits = static_cast<int>(std::wcstol(fmt.c_str(), nullptr, 10));
                    else if (key == L"n") digits = 1;  // legacy {n} is unpadded
                    out += Pad(n, digits);
                } else if (KeyIs(key, {L"拍摄日期", L"拍攝日期", L"taken"})) {
                    if (ctx.src->taken) out += FormatDate(ctx.src->taken, fmt);
                    else { ctx.fallback = true; out += FormatDate(ctx.src->modified, fmt); }
                } else if (KeyIs(key, {L"修改日期", L"modified", L"date"})) out += FormatDate(ctx.src->modified, fmt);
                else if (KeyIs(key, {L"文件夹", L"資料夾", L"folder"})) out += ctx.src->folder;
                else if (KeyIs(key, {L"尺寸", L"size"})) {
                    if (ctx.src->width && ctx.src->height)
                        out += std::to_wstring(ctx.src->width) + L"x" + std::to_wstring(ctx.src->height);
                } else known = false;
                if (known) { i = close + 1; continue; }
            }
        }
        out += p[i++];
    }
    return out;
}

bool PatternHasExt(const std::wstring& p) {
    return p.find(L"{ext}") != std::wstring::npos || p.find(L"{扩展名}") != std::wstring::npos;
}

struct CompiledStep {
    std::wregex expression;
    std::wstring replacement;
    bool invalid = false;
};

void ApplyStep(std::wstring& stem, std::wstring& ext, const Step& st, const CompiledStep& compiled, Ctx& ctx) {
    auto on = [&](auto&& f) {
        stem = f(stem);
        if (ctx.ext_too && ext.size() > 1) ext = L"." + f(ext.substr(1));
    };
    switch (st.type) {
    case StepType::Replace: {
        if (st.find.empty()) break;
        try {
            if (compiled.invalid) { ctx.bad_regex = true; break; }
            const auto mf = st.all ? std::regex_constants::format_default : std::regex_constants::format_first_only;
            on([&](const std::wstring& s) { return std::regex_replace(s, compiled.expression, compiled.replacement, mf); });
        } catch (const std::regex_error&) {
            ctx.bad_regex = true;
        }
        break;
    }
    case StepType::Remove:
        if (st.brackets) stem = RemoveBrackets(stem);
        if (st.digits) stem = RemoveDigits(stem);
        if (st.spaces) stem = CollapseSpaces(stem);
        if (!st.custom.empty()) stem = RemoveLiteral(stem, st.custom);
        break;
    case StepType::Case:
        if (st.mode == CaseMode::Lower) on([](const std::wstring& s) { return MapCase(s, false); });
        else if (st.mode == CaseMode::Upper) on([](const std::wstring& s) { return MapCase(s, true); });
        else if (st.mode == CaseMode::Title) stem = TitleCase(stem);
        break;
    case StepType::Template: {
        if (st.pattern.empty()) break;
        const std::wstring out = ExpandTemplate(st, stem, ext, ctx);
        if (PatternHasExt(st.pattern)) Split(out, stem, ext);
        else stem = out;
        break;
    }
    case StepType::Insert: {
        if (st.text.empty()) break;
        Step t = st;
        t.pattern = st.text;
        const std::wstring add = ExpandTemplate(t, stem, ext, ctx);
        if (st.where == Where::Start) stem = add + stem;
        else if (st.where == Where::End) stem += add;
        else {
            size_t pos = static_cast<size_t>(std::max(0, st.at));
            if (pos > stem.size()) pos = stem.size();
            // Never split a surrogate pair.
            if (pos > 0 && pos < stem.size() && stem[pos] >= 0xDC00 && stem[pos] <= 0xDFFF) ++pos;
            stem.insert(pos, add);
        }
        break;
    }
    case StepType::Number: {
        const long long n = static_cast<long long>(ctx.index) * std::max(1, st.stride) + st.start;
        const std::wstring num = Pad(n, std::clamp(st.digits_n, 1, 9));
        stem = st.where == Where::Start ? num + st.sep + stem : stem + st.sep + num;
        break;
    }
    case StepType::Ext:
        if (st.mode == CaseMode::Lower) ext = MapCase(ext, false);
        else if (st.mode == CaseMode::Upper) ext = MapCase(ext, true);
        else if (st.mode == CaseMode::Change) {
            std::wstring to = st.ext_to;
            while (!to.empty() && to.front() == L'.') to.erase(0, 1);
            ext = to.empty() ? std::wstring() : L"." + to;
        }
        break;
    }
}

bool IsReservedStem(const std::wstring& name) {
    std::wstring stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && IsSpace(stem.back())) stem.pop_back();
    stem = MapCase(stem, true);
    static const wchar_t* kReserved[] = {L"CON", L"PRN", L"AUX", L"NUL"};
    for (auto* r : kReserved) if (stem == r) return true;
    if (stem.size() == 4 && (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0) &&
        stem[3] >= L'1' && stem[3] <= L'9')
        return true;
    return false;
}

std::wstring Key(const std::wstring& parent, const std::wstring& name) {
    return parent + L"\\" + Lower(name);
}

} // namespace

std::wstring Lower(const std::wstring& s) { return MapCase(s, false); }

bool IsValidComponent(const std::wstring& name) {
    if (name.empty() || name.size() > 255 || name == L"." || name == L"..") return false;
    for (wchar_t c : name) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) return false;
    }
    if (IsSpace(name.back()) || name.back() == L'.') return false;
    return !IsReservedStem(name);
}

// Token text of a step: the template pattern or the inserted text.
static const std::wstring* TokenText(const Step& st) {
    if (!st.on) return nullptr;
    if (st.type == StepType::Template) return &st.pattern;
    if (st.type == StepType::Insert) return &st.text;
    return nullptr;
}

bool NeedsTakenDate(const std::vector<Step>& steps, const Options& opts) {
    if (opts.order == Order::Taken || opts.daily) return true;
    for (const auto& st : steps)
        if (const std::wstring* p = TokenText(st);
            p && (p->find(L"{拍摄日期") != std::wstring::npos || p->find(L"{拍攝日期") != std::wstring::npos ||
                  p->find(L"{taken") != std::wstring::npos))
            return true;
    return false;
}

bool UsesNumber(const std::wstring& p) {
    for (const wchar_t* k : {L"{序号", L"{序號", L"{n}", L"{n:", L"{number"})
        if (p.find(k) != std::wstring::npos) return true;
    return false;
}

bool NeedsDimensions(const std::vector<Step>& steps) {
    for (const auto& st : steps)
        if (const std::wstring* p = TokenText(st);
            p && (p->find(L"{尺寸") != std::wstring::npos || p->find(L"{size") != std::wstring::npos))
            return true;
    return false;
}

namespace {
Plan Resolve(const std::vector<Source>& sources, const std::vector<Step>& steps, const std::vector<std::wstring>* names,
             const Options& opts, const ExistsFn& exists);
} // namespace

Plan Run(const std::vector<Source>& sources, const std::vector<Step>& steps,
         const Options& opts, const ExistsFn& exists) {
    return Resolve(sources, steps, nullptr, opts, exists);
}

Plan RunNames(const std::vector<Source>& sources, const std::vector<std::wstring>& names,
              const Options& opts, const ExistsFn& exists) {
    return Resolve(sources, {}, &names, opts, exists);
}

namespace {
Plan Resolve(const std::vector<Source>& sources, const std::vector<Step>& steps, const std::vector<std::wstring>* names,
             const Options& opts, const ExistsFn& exists) {
    Plan plan;
    const size_t n = sources.size();
    plan.rows.resize(n);
    std::vector<std::wstring> parents;
    parents.reserve(n);
    for (const auto& source : sources) parents.push_back(Lower(ParentOf(source.path)));

    std::vector<CompiledStep> compiled(steps.size());
    if (n && !names) for (size_t j = 0; j < steps.size(); ++j) {
        const auto& st = steps[j];
        if (!st.on || st.type != StepType::Replace || st.find.empty()) continue;
        auto& c = compiled[j];
        try {
            auto flags = std::regex_constants::ECMAScript;
            if (!st.match_case) flags |= std::regex_constants::icase;
#ifdef PULSE_TEST_RENAME_REUSE
            ++regex_compile_attempts;
#endif
            c.expression.assign(st.regex ? st.find : EscapeRegex(st.find), flags);
            c.replacement = st.with;
            if (!st.regex)
                for (size_t p = c.replacement.find(L'$'); p != std::wstring::npos;
                     p = c.replacement.find(L'$', p + 2)) c.replacement.insert(p, 1, L'$');
        } catch (const std::regex_error&) { c.invalid = true; }
    }

    // Numbering order.
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = i;
    auto when = [&](size_t i) { return sources[i].taken ? sources[i].taken : sources[i].modified; };
    if (opts.order == Order::Taken)
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return when(a) < when(b); });
    else if (opts.order == Order::Modified)
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return sources[a].modified < sources[b].modified; });
    else if (opts.order == Order::Name) {
        std::vector<std::wstring> keys;
        keys.reserve(n);
        for (const auto& source : sources) keys.push_back(Lower(source.name));
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return keys[a] < keys[b]; });
    }
    std::vector<size_t> index_of(n);
    for (size_t j = 0; j < n; ++j) index_of[order[j]] = j;
    if (opts.daily) {
        // Each day counts from the start again, in the same order.
        constexpr uint64_t kDay = 864000000000ull;   // FILETIME ticks
        std::unordered_map<uint64_t, size_t> seen;
        for (size_t j = 0; j < n; ++j) index_of[order[j]] = seen[when(order[j]) / kDay]++;
    }

    // 1) Apply the steps.
    for (size_t i = 0; i < n; ++i) {
        const Source& s = sources[i];
        Row& r = plan.rows[i];
        r.src = i;
        std::wstring stem, ext;
        Split(s.name, stem, ext);
        Ctx ctx;
        ctx.src = &s;
        ctx.index = index_of[i];
        ctx.ext_too = opts.ext_too;
        if (names) {
            if (i < names->size() && !(*names)[i].empty()) Split((*names)[i], stem, ext);
        } else {
            for (size_t j = 0; j < steps.size(); ++j)
                if (steps[j].on) ApplyStep(stem, ext, steps[j], compiled[j], ctx);
        }
        if (ctx.bad_regex) plan.bad_regex = true;

        bool fixed = false;
        while (!stem.empty() && (IsSpace(stem.back()) || stem.back() == L'.')) { stem.pop_back(); fixed = true; }
        if (ext == L".") ext.clear();
        r.new_name = stem + ext;

        if (r.new_name == s.name) { r.status = RowStatus::Same; continue; }
        if (stem.empty() && ext.empty()) { r.status = RowStatus::Empty; r.why = Why::EmptyName; continue; }
        if (!IsValidComponent(r.new_name)) { r.status = RowStatus::Invalid; r.why = Why::BadChars; continue; }
        if (ctx.fallback) { r.status = RowStatus::Info; r.why = Why::NoTakenDate; }
        else if (fixed) { r.status = RowStatus::Fixed; r.why = Why::TrailingFixed; }
        else r.status = RowStatus::Ok;
    }

    // 2) Clashes. Rows that keep their name hold it; renaming rows claim
    //    their targets in list order. Skipping a row frees nothing but holds
    //    its old name, so the pass repeats until stable.
    std::vector<RowStatus> base_status(n);
    std::vector<Why> base_why(n);
    std::vector<std::wstring> base_name(n);
    for (size_t i = 0; i < n; ++i) {
        base_status[i] = plan.rows[i].status;
        base_why[i] = plan.rows[i].why;
        base_name[i] = plan.rows[i].new_name;
    }
    auto renaming = [&](RowStatus st) {
        return st == RowStatus::Ok || st == RowStatus::Fixed || st == RowStatus::Info;
    };
    std::vector<bool> skipped(n, false);
    std::vector<Why> skip_why(n, Why::None);
    std::vector<std::wstring> skip_arg(n);
    for (int pass = 0; pass < 8; ++pass) {
        std::unordered_map<std::wstring, size_t> held;  // key -> row
        for (size_t i = 0; i < n; ++i)
            if (!renaming(base_status[i]) || skipped[i]) held.emplace(Key(parents[i], sources[i].name), i);
        bool changed = false;
        for (size_t i = 0; i < n; ++i) {
            Row& r = plan.rows[i];
            r.status = base_status[i];
            r.why = base_why[i];
            r.new_name = base_name[i];
            r.wanted.clear();
            r.why_arg.clear();
            r.added_n = 0;
            if (!renaming(base_status[i])) continue;
            if (skipped[i]) {
                r.status = RowStatus::Skipped;
                r.why = skip_why[i];
                r.why_arg = skip_arg[i];
                r.wanted = base_name[i];
                r.new_name = sources[i].name;
                continue;
            }
            const Source& s = sources[i];
            auto taken_by = [&](const std::wstring& name, size_t& other, bool& outside) {
                auto it = held.find(Key(parents[i], name));
                if (it != held.end() && it->second != i) { other = it->second; outside = false; return true; }
                if (exists && exists(s.path, Lower(name))) { outside = true; return true; }
                return false;
            };
            size_t other = 0;
            bool outside = false;
            if (taken_by(r.new_name, other, outside)) {
                r.wanted = r.new_name;
                r.why = outside ? Why::ClashExisting : Why::ClashBatch;
                r.why_arg = outside ? r.new_name : sources[other].name;
                if (opts.policy == Policy::AutoNumber) {
                    std::wstring stem, ext;
                    Split(r.wanted, stem, ext);
                    int k = 2;
                    size_t o2 = 0; bool out2 = false;
                    while (k < 10000 && taken_by(stem + L" (" + std::to_wstring(k) + L")" + ext, o2, out2)) ++k;
                    r.new_name = stem + L" (" + std::to_wstring(k) + L")" + ext;
                    r.added_n = k;
                    r.status = RowStatus::Conflict;
                } else if (opts.policy == Policy::Skip) {
                    skipped[i] = true;
                    skip_why[i] = r.why;
                    skip_arg[i] = r.why_arg;
                    changed = true;
                    r.status = RowStatus::Skipped;
                    r.new_name = s.name;
                    continue;
                } else {
                    r.status = RowStatus::Conflict;
                    plan.blocked = true;
                    continue;
                }
            }
            held[Key(parents[i], r.new_name)] = i;
        }
        if (!changed) break;
    }

    // 3) Chain-safe order: a row whose target is another renaming row's
    //    current name goes after that row. Cycles (swaps) are held back.
    std::unordered_map<std::wstring, size_t> current;  // old key -> renaming row
    for (size_t i = 0; i < n; ++i)
        if (plan.rows[i].Renames()) current.emplace(Key(parents[i], sources[i].name), i);
    std::vector<int> state(n, 0);  // 0 new, 1 visiting, 2 done
    std::vector<size_t> out;
    std::function<bool(size_t)> visit = [&](size_t i) -> bool {
        if (state[i] == 2) return true;
        if (state[i] == 1) return false;
        state[i] = 1;
        auto it = current.find(Key(parents[i], plan.rows[i].new_name));
        if (it != current.end() && it->second != i && !visit(it->second)) {
            state[i] = 0;
            return false;
        }
        state[i] = 2;
        out.push_back(i);
        return true;
    };
    for (size_t i = 0; i < n; ++i) {
        if (!plan.rows[i].Renames() || state[i] == 2) continue;
        if (!visit(i)) {
            Row& r = plan.rows[i];
            r.wanted = r.new_name;
            r.new_name = sources[i].name;
            r.status = RowStatus::Conflict;
            r.why = Why::ClashCycle;
            r.added_n = 0;
            state[i] = 2;
        }
    }
    // A row held back from a cycle keeps its name; anything aiming at that
    // name can no longer go through either.
    for (bool again = true; again;) {
        again = false;
        std::unordered_set<std::wstring> kept;
        for (size_t i = 0; i < n; ++i)
            if (!plan.rows[i].Renames()) kept.insert(Key(parents[i], sources[i].name));
        for (size_t i = 0; i < n; ++i) {
            Row& r = plan.rows[i];
            if (!r.Renames() || Lower(r.new_name) == Lower(sources[i].name)) continue;
            if (kept.count(Key(parents[i], r.new_name))) {
                r.wanted = r.new_name;
                r.new_name = sources[i].name;
                r.status = RowStatus::Conflict;
                r.why = Why::ClashCycle;
                r.added_n = 0;
                again = true;
            }
        }
    }
    for (size_t i : out) if (plan.rows[i].Renames()) plan.exec_order.push_back(i);

    for (const auto& r : plan.rows) {
        if (r.Renames()) ++plan.renames;
        if (r.status == RowStatus::Same) ++plan.same;
        if (r.status == RowStatus::Conflict || r.status == RowStatus::Skipped ||
            r.status == RowStatus::Invalid || r.status == RowStatus::Empty ||
            r.status == RowStatus::Fixed || r.status == RowStatus::Info)
            ++plan.issues;
        if (r.status == RowStatus::Conflict && r.added_n > 0) ++plan.auto_fixed;
    }
    if (plan.bad_regex) plan.blocked = true;
    return plan;
}
} // namespace

// ---------------------------------------------------------------- diff

namespace {

std::vector<std::wstring> Tokens(const std::wstring& s) {
    std::vector<std::wstring> t;
    for (size_t i = 0; i < s.size();) {
        size_t j = i + 1;
        if (IsAsciiLetter(s[i])) while (j < s.size() && IsAsciiLetter(s[j])) ++j;
        else if (IsDigit(s[i])) while (j < s.size() && IsDigit(s[j])) ++j;
        else if (IsSpace(s[i])) while (j < s.size() && IsSpace(s[j])) ++j;
        t.push_back(s.substr(i, j - i));
        i = j;
    }
    return t;
}

void Push(std::vector<Seg>& v, wchar_t kind, const std::wstring& text) {
    if (!v.empty() && v.back().kind == kind) v.back().text += text;
    else v.push_back({kind, text});
}

} // namespace

void Diff(const std::wstring& a, const std::wstring& b, std::vector<Seg>& A, std::vector<Seg>& B) {
    A.clear();
    B.clear();
    const auto x = Tokens(a), y = Tokens(b);
    const size_t n = x.size(), m = y.size();
    std::vector<std::wstring> lx(n), ly(m);
    for (size_t i = 0; i < n; ++i) lx[i] = Lower(x[i]);
    for (size_t j = 0; j < m; ++j) ly[j] = Lower(y[j]);
    std::vector<double> dp((n + 1) * (m + 1), 0.0);
    auto D = [&](size_t i, size_t j) -> double& { return dp[i * (m + 1) + j]; };
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;) {
            double v = std::max(D(i + 1, j), D(i, j + 1));
            if (lx[i] == ly[j]) v = std::max(v, D(i + 1, j + 1) + static_cast<double>(x[i].size()));
            D(i, j) = v;
        }
    size_t i = 0, j = 0, same = 0;
    while (i < n && j < m) {
        if (lx[i] == ly[j] && D(i, j) == D(i + 1, j + 1) + static_cast<double>(x[i].size())) {
            Push(A, L'=', x[i]);
            for (size_t k = 0; k < y[j].size(); ++k)
                Push(B, k < x[i].size() && x[i][k] == y[j][k] ? L'=' : L'+', std::wstring(1, y[j][k]));
            same += x[i].size();
            ++i; ++j;
        } else if (D(i + 1, j) >= D(i, j + 1)) {
            Push(A, L'-', x[i++]);
        } else {
            Push(B, L'+', y[j++]);
        }
    }
    while (i < n) Push(A, L'-', x[i++]);
    while (j < m) Push(B, L'+', y[j++]);
    if (static_cast<double>(same) < 0.34 * static_cast<double>(std::max(a.size(), b.size()))) {
        A.assign(1, Seg{L'~', a});
        B.assign(1, Seg{L'~', b});
    }
}

} // namespace pulse::app::rename

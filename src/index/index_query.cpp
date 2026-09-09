// index_query.cpp — Compile Everything-subset queries into predicate groups.
#include "index_query.h"
#include "search_kinds.h"
#include "../common/path_utils.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>

namespace pulse::index {

namespace {

const wchar_t* Lut() { return FoldTable(); }

int CountChar(std::wstring_view s, wchar_t c) {
    int n = 0;
    for (wchar_t ch : s) if (ch == c) ++n;
    return n;
}

bool HasWild(std::wstring_view s) {
    return s.find(L'*') != std::wstring_view::npos || s.find(L'?') != std::wstring_view::npos;
}

bool StartsWithI(std::wstring_view s, std::wstring_view p) {
    if (s.size() < p.size()) return false;
    const wchar_t* lut = Lut();
    for (size_t i = 0; i < p.size(); ++i)
        if (lut[static_cast<uint16_t>(s[i])] != lut[static_cast<uint16_t>(p[i])]) return false;
    return true;
}

uint64_t FtFromLocal(const SYSTEMTIME& local) {
    FILETIME loc{}, utc{};
    if (!SystemTimeToFileTime(&local, &loc)) return 0;
    if (!LocalFileTimeToFileTime(&loc, &utc)) return 0;
    return (static_cast<uint64_t>(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
}

SYSTEMTIME LocalNow() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return st;
}

uint64_t MidnightLocal(int year, int month, int day) {
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(year);
    st.wMonth = static_cast<WORD>(month);
    st.wDay = static_cast<WORD>(day);
    return FtFromLocal(st);
}

void AddDays(SYSTEMTIME& st, int days) {
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += static_cast<LONGLONG>(days) * 864000000000LL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &st);
}

bool ParseU64(std::wstring_view s, uint64_t& out) {
    if (s.empty()) return false;
    uint64_t v = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + static_cast<uint64_t>(c - L'0');
    }
    out = v;
    return true;
}

bool ParseSizeBytes(std::wstring_view raw, uint64_t& bytes) {
    std::wstring s = Fold(raw);
    while (!s.empty() && s.back() == L' ') s.pop_back();
    if (s == L"empty" || s == L"0") { bytes = 0; return true; }
    size_t i = 0;
    while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') ++i;
    if (i == 0) return false;
    uint64_t n = 0;
    if (!ParseU64(std::wstring_view(s.data(), i), n)) return false;
    std::wstring_view unit(s.data() + i, s.size() - i);
    uint64_t mul = 1;
    if (unit.empty() || unit == L"b") mul = 1;
    else if (unit == L"kb" || unit == L"k") mul = 1024ull;
    else if (unit == L"mb" || unit == L"m") mul = 1024ull * 1024ull;
    else if (unit == L"gb" || unit == L"g") mul = 1024ull * 1024ull * 1024ull;
    else if (unit == L"tb" || unit == L"t") mul = 1024ull * 1024ull * 1024ull * 1024ull;
    else return false;
    bytes = n * mul;
    return true;
}

bool NamedSizeRange(std::wstring_view raw, uint64_t& lo, uint64_t& hi) {
    std::wstring s = Fold(raw);
    const uint64_t KB = 1024, MB = 1024 * 1024;
    if (s == L"tiny")     { lo = 0;              hi = 10 * KB; return true; }
    if (s == L"small")    { lo = 10 * KB;        hi = 100 * KB; return true; }
    if (s == L"medium")   { lo = 100 * KB;       hi = 1 * MB; return true; }
    if (s == L"large")    { lo = 1 * MB;         hi = 16 * MB; return true; }
    if (s == L"huge")     { lo = 16 * MB;        hi = 128 * MB; return true; }
    if (s == L"gigantic") { lo = 128 * MB;       hi = 0; return true; }
    return false;
}

bool ParseYmd(std::wstring_view s, int& y, int& m, int& d, int& parts) {
    // YYYY / YYYY-MM / YYYY-MM-DD
    parts = 0; y = 0; m = 1; d = 1;
    auto take = [&](size_t off, size_t n, int& out) -> bool {
        if (off + n > s.size()) return false;
        uint64_t v = 0;
        if (!ParseU64(s.substr(off, n), v)) return false;
        out = static_cast<int>(v);
        return true;
    };
    if (s.size() < 4 || !take(0, 4, y)) return false;
    parts = 1;
    if (s.size() == 4) return true;
    if (s.size() < 7 || s[4] != L'-' || !take(5, 2, m)) return false;
    parts = 2;
    if (s.size() == 7) return true;
    if (s.size() < 10 || s[7] != L'-' || !take(8, 2, d)) return false;
    parts = 3;
    return s.size() == 10;
}

void ApplyDateToken(Term& t, std::wstring_view raw) {
    std::wstring s = Fold(raw);
    SYSTEMTIME now = LocalNow();
    auto set_range = [&](uint64_t lo, uint64_t hi) {
        t.date_how = DateHow::Range;
        t.date_lo = lo;
        t.date_hi = hi;
    };
    if (s == L"today") {
        uint64_t lo = MidnightLocal(now.wYear, now.wMonth, now.wDay);
        SYSTEMTIME nxt = now; AddDays(nxt, 1); nxt.wHour = nxt.wMinute = nxt.wSecond = nxt.wMilliseconds = 0;
        set_range(lo, MidnightLocal(nxt.wYear, nxt.wMonth, nxt.wDay));
        return;
    }
    if (s == L"yesterday") {
        SYSTEMTIME y = now; AddDays(y, -1); y.wHour = y.wMinute = y.wSecond = y.wMilliseconds = 0;
        uint64_t lo = MidnightLocal(y.wYear, y.wMonth, y.wDay);
        set_range(lo, MidnightLocal(now.wYear, now.wMonth, now.wDay));
        return;
    }
    if (s == L"thisweek") {
        SYSTEMTIME st = now;
        int back = (st.wDayOfWeek == 0) ? 6 : (st.wDayOfWeek - 1); // Monday start
        AddDays(st, -back); st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
        SYSTEMTIME en = now; AddDays(en, 1); en.wHour = en.wMinute = en.wSecond = en.wMilliseconds = 0;
        set_range(MidnightLocal(st.wYear, st.wMonth, st.wDay),
                  MidnightLocal(en.wYear, en.wMonth, en.wDay));
        return;
    }
    if (s == L"thismonth") {
        int month = now.wMonth + 1;
        int year = now.wYear;
        if (month > 12) { month = 1; ++year; }
        set_range(MidnightLocal(now.wYear, now.wMonth, 1),
                  MidnightLocal(year, month, 1));
        return;
    }
    if (s == L"thisyear") {
        set_range(MidnightLocal(now.wYear, 1, 1), MidnightLocal(now.wYear + 1, 1, 1));
        return;
    }
    auto dots = s.find(L"..");
    if (dots != std::wstring::npos) {
        int y1, m1, d1, p1, y2, m2, d2, p2;
        if (ParseYmd(std::wstring_view(s.data(), dots), y1, m1, d1, p1) &&
            ParseYmd(std::wstring_view(s.data() + dots + 2, s.size() - dots - 2), y2, m2, d2, p2)) {
            uint64_t lo = MidnightLocal(y1, m1, d1);
            SYSTEMTIME hi{ }; hi.wYear = static_cast<WORD>(y2); hi.wMonth = static_cast<WORD>(m2); hi.wDay = static_cast<WORD>(d2);
            if (p2 == 1) { hi.wMonth = 1; hi.wDay = 1; hi.wYear = static_cast<WORD>(y2 + 1); }
            else if (p2 == 2) { hi.wDay = 1; hi.wMonth = static_cast<WORD>(m2 + 1); if (hi.wMonth > 12) { hi.wMonth = 1; ++hi.wYear; } }
            else AddDays(hi, 1);
            set_range(lo, MidnightLocal(hi.wYear, hi.wMonth, hi.wDay));
        }
        return;
    }
    int y, m, d, parts;
    if (!ParseYmd(s, y, m, d, parts)) return;
    uint64_t lo = MidnightLocal(y, m, d);
    if (parts == 1) set_range(lo, MidnightLocal(y + 1, 1, 1));
    else if (parts == 2) {
        int nm = m + 1, ny = y;
        if (nm > 12) { nm = 1; ++ny; }
        set_range(lo, MidnightLocal(ny, nm, 1));
    } else {
        SYSTEMTIME hi{}; hi.wYear = static_cast<WORD>(y); hi.wMonth = static_cast<WORD>(m); hi.wDay = static_cast<WORD>(d);
        AddDays(hi, 1);
        set_range(lo, MidnightLocal(hi.wYear, hi.wMonth, hi.wDay));
    }
}

void ApplySizeToken(Term& t, std::wstring_view raw) {
    std::wstring s = Fold(raw);
    uint64_t lo = 0, hi = 0;
    if (NamedSizeRange(s, lo, hi)) {
        t.size_how = SizeHow::Range;
        t.size_lo = lo;
        t.size_hi = hi;
        return;
    }
    SizeHow how = SizeHow::Eq;
    std::wstring_view num = s;
    if (s.starts_with(L">=")) { how = SizeHow::Ge; num = std::wstring_view(s).substr(2); }
    else if (s.starts_with(L"<=")) { how = SizeHow::Le; num = std::wstring_view(s).substr(2); }
    else if (s.starts_with(L">")) { how = SizeHow::Gt; num = std::wstring_view(s).substr(1); }
    else if (s.starts_with(L"<")) { how = SizeHow::Lt; num = std::wstring_view(s).substr(1); }
    uint64_t bytes = 0;
    if (!ParseSizeBytes(num, bytes)) return;
    t.size_how = how;
    t.size_lo = bytes;
}

void ApplyName(Term& t, std::wstring_view raw, bool quoted) {
    std::wstring folded = Fold(raw);
    if (folded.empty()) return;
    t.name = std::move(folded);
    if (HasWild(t.name)) t.name_how = NameHow::Wildcard;
    else if (quoted) t.name_how = NameHow::Exact;
    else t.name_how = NameHow::Substring;
}

void ApplyExt(Term& t, std::wstring_view raw) {
    std::wstring s = Fold(raw);
    size_t i = 0;
    while (i < s.size()) {
        size_t sep = s.find_first_of(L";,", i);
        if (sep == std::wstring::npos) sep = s.size();
        std::wstring one = s.substr(i, sep - i);
        if (!one.empty() && one[0] == L'.') one.erase(0, 1);
        if (!one.empty()) t.exts.push_back(std::move(one));
        i = sep + 1;
    }
}

bool IsColon(wchar_t c) { return c == L':' || c == L'：'; }

bool LooksLikeAbsPath(std::wstring_view s) {
    if (s.size() >= 2 && ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z')) &&
        s[1] == L':') return true;
    return s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\';
}

std::wstring_view StripQuotes(std::wstring_view s) {
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"')
        return s.substr(1, s.size() - 2);
    return s;
}

bool IsModifier(std::wstring_view tok, std::wstring_view key, std::wstring_view& val) {
    if (!StartsWithI(tok, key)) return false;
    if (tok.size() == key.size()) { val = {}; return true; }
    if (!IsColon(tok[key.size()])) return false;
    val = StripQuotes(tok.substr(key.size() + 1));
    return true;
}

bool IsContentKey(std::wstring_view tok, std::wstring_view& val) {
    return IsModifier(tok, L"content", val) || IsModifier(tok, L"\u5185\u5bb9", val) ||
           IsModifier(tok, L"\u5305\u542b", val);
}

bool IsTypeKey(std::wstring_view tok, std::wstring_view& val) {
    return IsModifier(tok, L"type", val) || IsModifier(tok, L"kind", val) ||
           IsModifier(tok, L"\u7c7b\u578b", val) || IsModifier(tok, L"\u79cd\u7c7b", val);
}

bool IsFlagToken(std::wstring_view tok, ContentClause& content) {
    std::wstring_view val;
    auto flagged = [&](std::wstring_view key) {
        return tok.size() > key.size() && IsModifier(tok, key, val);
    };
    if (flagged(L"ww") || flagged(L"wholeword") || flagged(L"\u5168\u8bcd")) {
        content.whole_word = true;
        return true;
    }
    if (flagged(L"case") || flagged(L"casesensitive") ||
        flagged(L"\u533a\u5206\u5927\u5c0f\u5199")) {
        content.case_sensitive = true;
        return true;
    }
    if (flagged(L"contentmode") || flagged(L"\u5185\u5bb9\u5339\u914d")) {
        const std::wstring mode = Fold(val);
        if (mode == L"any" || mode == L"or" || mode == L"\u4efb\u4e00")
            content.mode = ContentMatchMode::AnyWord;
        else if (mode == L"phrase" || mode == L"\u77ed\u8bed")
            content.mode = ContentMatchMode::Phrase;
        else
            content.mode = ContentMatchMode::AllWords;
        return true;
    }
    return false;
}

struct RawToken {
    std::wstring text;
    bool quoted = false;
    bool negated = false;
};

bool ConsumeContentOrFlag(const RawToken& tok, CompiledQuery& q) {
    if (tok.quoted) return false;
    std::wstring_view val;
    if (IsFlagToken(tok.text, q.content)) return true;
    if (IsContentKey(tok.text, val)) {
        std::wstring needle(val);
        if (needle.empty()) return true;
        if (tok.negated) q.content.excluded.push_back(std::move(needle));
        else q.content.needles.push_back(std::move(needle));
        return true;
    }
    if (IsModifier(tok.text, L"path", val) || IsModifier(tok.text, L"\u8def\u5f84", val)) {
        if (LooksLikeAbsPath(val)) {
            if (q.path_prefix.empty() && !tok.negated) {
                q.path_prefix = path::StripExtendedPathPrefix(val);
                return true;
            }
            return false;
        }
    }
    return false;
}

bool SkipFromFilenameNeedle(const RawToken& tok) {
    if (tok.quoted) return false;
    std::wstring_view val;
    ContentClause unused;
    if (IsFlagToken(tok.text, unused)) return true;
    if (IsContentKey(tok.text, val)) return true;
    return false;
}

Term ParseTerm(std::wstring_view tok, bool quoted, bool negated) {
    Term t;
    std::wstring_view val;
    if (!quoted && (IsModifier(tok, L"ext", val))) {
        ApplyExt(t, val);
        t.ext_not = negated;
        return t;
    }
    if (!quoted && (IsModifier(tok, L"size", val) || IsModifier(tok, L"\u5927\u5c0f", val))) {
        ApplySizeToken(t, val);
        t.size_not = negated;
        return t;
    }
    if (!quoted && (IsModifier(tok, L"dm", val) || IsModifier(tok, L"datemodified", val) ||
                    IsModifier(tok, L"\u4fee\u6539", val) || IsModifier(tok, L"\u65e5\u671f", val))) {
        ApplyDateToken(t, val);
        t.date_not = negated;
        return t;
    }
    if (!quoted && (IsModifier(tok, L"folder", val) || IsModifier(tok, L"\u6587\u4ef6\u5939", val))) {
        t.folder = true;
        if (!val.empty()) ApplyName(t, val, false);
        if (negated) t.name_not = true; // !folder:foo → not (folder named foo); lone !folder: → files
        if (negated && val.empty()) { t.folder = false; t.file = true; t.name_not = false; }
        return t;
    }
    if (!quoted && (IsModifier(tok, L"file", val) || IsModifier(tok, L"\u6587\u4ef6", val))) {
        t.file = true;
        if (!val.empty()) ApplyName(t, val, false);
        if (negated && val.empty()) { t.file = false; t.folder = true; }
        return t;
    }
    if (!quoted && (IsModifier(tok, L"path", val) || IsModifier(tok, L"\u8def\u5f84", val))) {
        ApplyName(t, val, false);
        t.name_in_path = true;
        t.name_not = negated;
        return t;
    }
    if (!quoted && IsTypeKey(tok, val)) {
        bool folders_only = false;
        if (ExpandKindToken(val, t.exts, folders_only)) {
            if (folders_only) t.folder = true;
            t.ext_not = negated;
            return t;
        }
        ApplyExt(t, val);
        t.ext_not = negated;
        return t;
    }
    ApplyName(t, tok, quoted);
    t.name_not = negated;
    if (!quoted && !t.name.empty() &&
        (tok.find(L'\\') != std::wstring_view::npos || tok.find(L'/') != std::wstring_view::npos))
        t.name_in_path = true;
    return t;
}

std::vector<RawToken> TokenizeQuery(std::wstring_view raw, std::vector<size_t>* or_breaks) {
    std::vector<RawToken> tokens;
    size_t i = 0;
    auto skip_ws = [&] {
        while (i < raw.size() && (raw[i] == L' ' || raw[i] == L'\t')) ++i;
    };
    while (i < raw.size()) {
        skip_ws();
        if (i >= raw.size()) break;
        if (raw[i] == L'|') {
            if (or_breaks) or_breaks->push_back(tokens.size());
            ++i;
            continue;
        }
        bool neg = false;
        while (i < raw.size() && raw[i] == L'!') { neg = !neg; ++i; }
        skip_ws();
        if (i >= raw.size()) break;
        RawToken tok;
        tok.negated = neg;
        if (raw[i] == L'"') {
            tok.quoted = true;
            ++i;
            while (i < raw.size() && raw[i] != L'"') tok.text.push_back(raw[i++]);
            if (i < raw.size() && raw[i] == L'"') ++i;
        } else {
            while (i < raw.size() && raw[i] != L' ' && raw[i] != L'\t' && raw[i] != L'|') {
                const wchar_t c = raw[i++];
                tok.text.push_back(c);
                if (IsColon(c)) {
                    if (i < raw.size() && raw[i] == L'"') {
                        tok.text.push_back(raw[i++]);
                        while (i < raw.size() && raw[i] != L'"') tok.text.push_back(raw[i++]);
                        if (i < raw.size() && raw[i] == L'"') tok.text.push_back(raw[i++]);
                        break;
                    }
                }
            }
        }
        if (tok.text.empty() && !tok.quoted) continue;
        tokens.push_back(std::move(tok));
    }
    return tokens;
}

} // namespace

const wchar_t* FoldTable() {
    static const std::vector<wchar_t> table = [] {
        std::vector<wchar_t> t(65536);
        for (uint32_t i = 0; i < 65536; ++i)
            t[i] = static_cast<wchar_t>(std::towlower(static_cast<wchar_t>(i)));
        return t;
    }();
    return table.data();
}

std::wstring Fold(std::wstring_view s) {
    std::wstring o(s.size(), 0);
    const wchar_t* lut = Lut();
    for (size_t i = 0; i < s.size(); ++i) o[i] = lut[static_cast<uint16_t>(s[i])];
    return o;
}

bool ContainsFolded(const wchar_t* s, uint32_t n, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (needle.size() > n) return false;
    const wchar_t* lut = Lut();
    const wchar_t first = needle[0];
    const uint32_t last = n - static_cast<uint32_t>(needle.size());
    for (uint32_t i = 0; i <= last; ++i) {
        if (lut[static_cast<uint16_t>(s[i])] != first) continue;
        bool ok = true;
        for (size_t j = 1; j < needle.size(); ++j) {
            if (lut[static_cast<uint16_t>(s[i + j])] != needle[j]) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

bool EqualsFolded(const wchar_t* s, uint32_t n, const std::wstring& needle) {
    if (n != needle.size()) return false;
    const wchar_t* lut = Lut();
    for (uint32_t i = 0; i < n; ++i)
        if (lut[static_cast<uint16_t>(s[i])] != needle[i]) return false;
    return true;
}

bool StartsWithFolded(const wchar_t* s, uint32_t n, const std::wstring& needle) {
    if (needle.size() > n) return false;
    const wchar_t* lut = Lut();
    for (size_t i = 0; i < needle.size(); ++i)
        if (lut[static_cast<uint16_t>(s[i])] != needle[i]) return false;
    return true;
}

bool WildcardFolded(const wchar_t* s, uint32_t n, const std::wstring& pat) {
    const wchar_t* lut = Lut();
    const wchar_t* p = pat.data();
    const uint32_t pn = static_cast<uint32_t>(pat.size());
    uint32_t si = 0, pi = 0, star = ~0u, match = 0;
    while (si < n) {
        if (pi < pn && p[pi] == L'*') { star = pi++; match = si; }
        else if (pi < pn && (p[pi] == L'?' || lut[static_cast<uint16_t>(s[si])] == p[pi])) {
            ++si; ++pi;
        } else if (star != ~0u) {
            pi = star + 1;
            si = ++match;
        } else {
            return false;
        }
    }
    while (pi < pn && p[pi] == L'*') ++pi;
    return pi == pn;
}

bool WordStartFolded(const wchar_t* s, uint32_t n, const std::wstring& needle) {
    if (needle.empty() || needle.size() > n) return false;
    const wchar_t* lut = Lut();
    auto is_break = [](wchar_t c) {
        return !(std::iswalnum(c) || c > 127);
    };
    const uint32_t last = n - static_cast<uint32_t>(needle.size());
    for (uint32_t i = 0; i <= last; ++i) {
        if (i > 0 && !is_break(s[i - 1])) continue;
        if (lut[static_cast<uint16_t>(s[i])] != needle[0]) continue;
        bool ok = true;
        for (size_t j = 1; j < needle.size(); ++j) {
            if (lut[static_cast<uint16_t>(s[i + j])] != needle[j]) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

bool MatchName(const wchar_t* s, uint32_t n, const Term& t) {
    switch (t.name_how) {
    case NameHow::Exact:     return EqualsFolded(s, n, t.name);
    case NameHow::Wildcard:  return WildcardFolded(s, n, t.name);
    case NameHow::Substring: return ContainsFolded(s, n, t.name);
    default: return true;
    }
}

bool MatchExt(const wchar_t* s, uint32_t n, const Term& t) {
    if (t.exts.empty()) return true;
    uint32_t dot = n;
    for (uint32_t i = n; i > 0; --i) {
        if (s[i - 1] == L'.') { dot = i; break; }
        if (s[i - 1] == L'\\' || s[i - 1] == L'/') break;
    }
    const wchar_t* ext = (dot < n) ? s + dot : s + n;
    uint32_t en = (dot < n) ? n - dot : 0;
    for (const auto& want : t.exts) {
        if (EqualsFolded(ext, en, want)) return true;
    }
    return false;
}

bool MatchSize(uint64_t bytes, const Term& t) {
    switch (t.size_how) {
    case SizeHow::Eq:    return bytes == t.size_lo;
    case SizeHow::Gt:    return bytes > t.size_lo;
    case SizeHow::Ge:    return bytes >= t.size_lo;
    case SizeHow::Lt:    return bytes < t.size_lo;
    case SizeHow::Le:    return bytes <= t.size_lo;
    case SizeHow::Range: return bytes >= t.size_lo && (t.size_hi == 0 || bytes < t.size_hi);
    default: return true;
    }
}

bool MatchDate(uint64_t filetime, const Term& t) {
    if (t.date_how != DateHow::Range) return true;
    if (filetime < t.date_lo) return false;
    if (t.date_hi != 0 && filetime >= t.date_hi) return false;
    return true;
}

bool QueryUsesAttrs(const CompiledQuery& q) {
    for (const auto& g : q.groups) {
        for (const auto& t : g) {
            if (t.size_how != SizeHow::Any || t.date_how != DateHow::Any) return true;
        }
    }
    return false;
}

size_t QueryPrimaryNameLen(const CompiledQuery& q) {
    size_t best = 0;
    for (const auto& g : q.groups) {
        for (const auto& t : g) {
            if (t.name_not || t.name_how == NameHow::Any) continue;
            if (t.name.size() > best) best = t.name.size();
        }
    }
    return best;
}

bool QueryIsSimpleName(const CompiledQuery& q) {
    if (q.groups.size() != 1 || q.groups[0].size() != 1) return false;
    const Term& t = q.groups[0][0];
    if (t.name_not || t.name_in_path || t.folder || t.file) return false;
    if (!t.exts.empty() || t.size_how != SizeHow::Any || t.date_how != DateHow::Any) return false;
    return t.name_how == NameHow::Substring || t.name_how == NameHow::Exact;
}

bool QueryHasContent(const CompiledQuery& q) {
    return q.content.present();
}

bool QueryHasExtFilter(const CompiledQuery& q) {
    for (const auto& g : q.groups)
        for (const auto& t : g)
            if (!t.exts.empty()) return true;
    return false;
}

bool QueryHasNameFilter(const CompiledQuery& q) {
    for (const auto& g : q.groups)
        for (const auto& t : g)
            if (t.name_how != NameHow::Any && !t.name_in_path) return true;
    return false;
}

bool QueryHasFolderFilter(const CompiledQuery& q) {
    for (const auto& g : q.groups)
        for (const auto& t : g)
            if (t.folder || t.file) return true;
    return false;
}

int RankName(const wchar_t* s, uint32_t n, bool is_dir, const CompiledQuery& q) {
    int best = 0;
    for (const auto& g : q.groups) {
        for (const auto& t : g) {
            if (t.name_not || t.name_how == NameHow::Any || t.name.empty()) continue;
            int sc = 0;
            if (t.name_how == NameHow::Wildcard) {
                if (WildcardFolded(s, n, t.name)) sc = 150;
            } else if (EqualsFolded(s, n, t.name)) {
                sc = 400;
            } else if (StartsWithFolded(s, n, t.name)) {
                sc = 300;
            } else if (WordStartFolded(s, n, t.name)) {
                sc = 200;
            } else if (ContainsFolded(s, n, t.name)) {
                sc = 100;
            }
            if (sc > best) best = sc;
        }
    }
    if (best == 0) best = 1;
    if (is_dir) best += 40;
    if (n < 24) best += static_cast<int>(24 - n);
    return best;
}

bool QueryCanNarrow(std::wstring_view prev, std::wstring_view next) {
    if (prev.empty() || next.size() < prev.size()) return false;
    if (next.substr(0, prev.size()) != prev) return false;
    if (CountChar(next, L'|') != CountChar(prev, L'|')) return false;
    if (CountChar(next, L'!') < CountChar(prev, L'!')) return false;
    return true;
}

CompiledQuery ParseQuery(std::wstring_view raw) {
    CompiledQuery q;
    q.groups.emplace_back();
    std::vector<size_t> or_breaks;
    const auto tokens = TokenizeQuery(raw, &or_breaks);
    size_t or_i = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        while (or_i < or_breaks.size() && or_breaks[or_i] == i) {
            if (!q.groups.back().empty()) q.groups.emplace_back();
            ++or_i;
        }
        if (ConsumeContentOrFlag(tokens[i], q)) continue;
        q.groups.back().push_back(ParseTerm(tokens[i].text, tokens[i].quoted, tokens[i].negated));
    }
    while (!q.groups.empty() && q.groups.back().empty()) q.groups.pop_back();
    if (q.content.needles.size() == 1 && q.content.mode == ContentMatchMode::AllWords &&
        q.content.needles[0].find(L' ') != std::wstring::npos)
        q.content.mode = ContentMatchMode::Phrase;
    return q;
}

std::wstring FilenameQueryText(std::wstring_view raw) {
    std::vector<size_t> or_breaks;
    const auto tokens = TokenizeQuery(raw, &or_breaks);
    std::wstring out;
    size_t or_i = 0;
    bool group_has = false;
    bool skipped_path_prefix = false;
    auto push_or = [&] {
        if (!out.empty() && group_has) out.append(L" |");
        group_has = false;
    };
    for (size_t i = 0; i < tokens.size(); ++i) {
        while (or_i < or_breaks.size() && or_breaks[or_i] == i) {
            push_or();
            ++or_i;
        }
        std::wstring_view path_val;
        const bool abs_path = !tokens[i].quoted &&
            (IsModifier(tokens[i].text, L"path", path_val) ||
             IsModifier(tokens[i].text, L"\u8def\u5f84", path_val)) &&
            LooksLikeAbsPath(path_val);
        if (abs_path && !tokens[i].negated && !skipped_path_prefix) {
            skipped_path_prefix = true;
            continue;
        }
        if (SkipFromFilenameNeedle(tokens[i])) continue;
        if (!out.empty() && (group_has || out.ends_with(L'|') || out.ends_with(L' '))) {
            if (!out.ends_with(L' ') && !out.ends_with(L'|')) out.push_back(L' ');
            else if (out.ends_with(L'|')) out.push_back(L' ');
        } else if (!out.empty() && !group_has) {
            out.push_back(L' ');
        }
        if (tokens[i].negated) out.push_back(L'!');
        if (tokens[i].quoted) {
            out.push_back(L'"');
            out.append(tokens[i].text);
            out.push_back(L'"');
        } else {
            out.append(tokens[i].text);
        }
        group_has = true;
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'|')) out.pop_back();
    return out;
}

} // namespace pulse::index

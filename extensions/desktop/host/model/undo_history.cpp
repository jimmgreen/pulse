#include <cmath>
// undo_history.cpp - Bounded undo/redo stack and zero-dependency RFC 8259 UTF-8 JSON persistence.
#include "desktop_model.h"
#include "group_tabs.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <utility>

namespace pulse::desktop::model {

// ---------------------------------------------------------------------------
// UndoHistory
// ---------------------------------------------------------------------------
void UndoHistory::Push(const DesktopModelState& snapshot, std::wstring action_label) {
    redo_.clear();
    if (max_depth_ == 0) {
        return;
    }
    if (undo_.size() >= max_depth_) {
        const size_t excess = undo_.size() - max_depth_ + 1;
        undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(excess));
    }
    undo_.push_back(Entry{snapshot, std::move(action_label)});
}

std::wstring UndoHistory::NextUndoLabel() const {
    if (undo_.empty()) {
        return {};
    }
    return undo_.back().label;
}

bool UndoHistory::Undo(DesktopModelState& current) {
    if (undo_.empty()) {
        return false;
    }
    Entry prev = std::move(undo_.back());
    undo_.pop_back();

    if (max_depth_ > 0) {
        if (redo_.size() >= max_depth_) {
            const size_t excess = redo_.size() - max_depth_ + 1;
            redo_.erase(redo_.begin(), redo_.begin() + static_cast<std::ptrdiff_t>(excess));
        }
        redo_.push_back(Entry{std::move(current), std::move(prev.label)});
    }
    current = std::move(prev.state);
    return true;
}

bool UndoHistory::Redo(DesktopModelState& current) {
    if (redo_.empty()) {
        return false;
    }
    Entry next = std::move(redo_.back());
    redo_.pop_back();

    if (max_depth_ > 0) {
        if (undo_.size() >= max_depth_) {
            const size_t excess = undo_.size() - max_depth_ + 1;
            undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(excess));
        }
        undo_.push_back(Entry{std::move(current), std::move(next.label)});
    }
    current = std::move(next.state);
    return true;
}

// ---------------------------------------------------------------------------
// Lossless RFC 8259 UTF-8 JSON Writer & Parser
// ---------------------------------------------------------------------------
namespace {

void AppendHex4(std::string& out, uint16_t val) {
    constexpr char kHex[] = "0123456789ABCDEF";
    out.append("\\u", 2);
    out.push_back(kHex[(val >> 12) & 0xFu]);
    out.push_back(kHex[(val >> 8) & 0xFu]);
    out.push_back(kHex[(val >> 4) & 0xFu]);
    out.push_back(kHex[val & 0xFu]);
}

void AppendCodepointUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

void AppendCodepointUtf16(std::wstring& out, uint32_t cp) {
    if (cp <= 0xFFFFu) {
        out.push_back(static_cast<wchar_t>(cp));
    } else if (cp <= 0x10FFFFu) {
        const uint32_t v = cp - 0x10000u;
        out.push_back(static_cast<wchar_t>(0xD800u + (v >> 10)));
        out.push_back(static_cast<wchar_t>(0xDC00u + (v & 0x3FFu)));
    }
}

class JsonWriter {
public:
    explicit JsonWriter(std::string& buf) noexcept : buf_(buf) {}

    void BeginObject() {
        buf_.push_back('{');
        need_comma_stack_.push_back(false);
        ++indent_;
    }

    void EndObject() {
        const bool had_fields = !need_comma_stack_.empty() && need_comma_stack_.back();
        if (!need_comma_stack_.empty()) {
            need_comma_stack_.pop_back();
        }
        --indent_;
        if (had_fields) {
            NewlineIndent();
        }
        buf_.push_back('}');
    }

    void BeginArray() {
        buf_.push_back('[');
        need_comma_stack_.push_back(false);
        ++indent_;
    }

    void EndArray() {
        const bool had_elems = !need_comma_stack_.empty() && need_comma_stack_.back();
        if (!need_comma_stack_.empty()) {
            need_comma_stack_.pop_back();
        }
        --indent_;
        if (had_elems) {
            NewlineIndent();
        }
        buf_.push_back(']');
    }

    void Key(std::string_view key) {
        PrepareElement();
        WriteUtf8StringLiteral(key);
        buf_.append(": ", 2);
    }

    void Elem() {
        PrepareElement();
    }

    void WriteNull() {
        buf_.append("null", 4);
    }

    void WriteDouble(double value) { buf_ += GlassNumber(value); }

    void WriteBool(bool v) {
        if (v) {
            buf_.append("true", 4);
        } else {
            buf_.append("false", 5);
        }
    }

    void WriteInt64(int64_t v) {
        char tmp[32]{};
        const auto res = std::to_chars(tmp, tmp + sizeof(tmp), v);
        buf_.append(tmp, static_cast<size_t>(res.ptr - tmp));
    }

    void WriteUint64(uint64_t v) {
        char tmp[32]{};
        const auto res = std::to_chars(tmp, tmp + sizeof(tmp), v);
        buf_.append(tmp, static_cast<size_t>(res.ptr - tmp));
    }

    void WriteUtf8StringLiteral(std::string_view s) {
        buf_.push_back('"');
        for (size_t i = 0; i < s.size(); ++i) {
            const uint8_t c = static_cast<uint8_t>(s[i]);
            switch (c) {
            case '"':  buf_.append("\\\"", 2); break;
            case '\\': buf_.append("\\\\", 2); break;
            case '\b': buf_.append("\\b", 2); break;
            case '\f': buf_.append("\\f", 2); break;
            case '\n': buf_.append("\\n", 2); break;
            case '\r': buf_.append("\\r", 2); break;
            case '\t': buf_.append("\\t", 2); break;
            default:
                if (c < 0x20u) {
                    AppendHex4(buf_, c);
                } else {
                    buf_.push_back(static_cast<char>(c));
                }
                break;
            }
        }
        buf_.push_back('"');
    }

    void WriteWideStringLiteral(std::wstring_view ws) {
        buf_.push_back('"');
        for (size_t i = 0; i < ws.size(); ++i) {
            const uint16_t u = static_cast<uint16_t>(ws[i]);
            switch (u) {
            case L'"':  buf_.append("\\\"", 2); break;
            case L'\\': buf_.append("\\\\", 2); break;
            case L'\b': buf_.append("\\b", 2); break;
            case L'\f': buf_.append("\\f", 2); break;
            case L'\n': buf_.append("\\n", 2); break;
            case L'\r': buf_.append("\\r", 2); break;
            case L'\t': buf_.append("\\t", 2); break;
            default:
                if (u < 0x20u) {
                    AppendHex4(buf_, u);
                } else if (u >= 0xD800u && u <= 0xDBFFu) {
                    if (i + 1 < ws.size()) {
                        const uint16_t u2 = static_cast<uint16_t>(ws[i + 1]);
                        if (u2 >= 0xDC00u && u2 <= 0xDFFFu) {
                            const uint32_t cp = 0x10000u +
                                                ((static_cast<uint32_t>(u - 0xD800u) << 10) |
                                                 static_cast<uint32_t>(u2 - 0xDC00u));
                            AppendCodepointUtf8(buf_, cp);
                            ++i;
                            break;
                        }
                    }
                    AppendHex4(buf_, u);
                } else if (u >= 0xDC00u && u <= 0xDFFFu) {
                    AppendHex4(buf_, u);
                } else {
                    AppendCodepointUtf8(buf_, u);
                }
                break;
            }
        }
        buf_.push_back('"');
    }

    void WritePoint(const PointI& p) {
        BeginObject();
        Key("x"); WriteInt64(p.x);
        Key("y"); WriteInt64(p.y);
        EndObject();
    }

    void WriteRect(const RectI& r) {
        BeginObject();
        Key("x"); WriteInt64(r.x);
        Key("y"); WriteInt64(r.y);
        Key("w"); WriteInt64(r.w);
        Key("h"); WriteInt64(r.h);
        EndObject();
    }

    void WriteWideArray(std::span<const std::wstring> arr) {
        BeginArray();
        for (const std::wstring& s : arr) {
            Elem();
            WriteWideStringLiteral(s);
        }
        EndArray();
    }

private:
    void PrepareElement() {
        if (!need_comma_stack_.empty()) {
            if (need_comma_stack_.back()) {
                buf_.push_back(',');
            }
            need_comma_stack_.back() = true;
        }
        NewlineIndent();
    }

    void NewlineIndent() {
        buf_.push_back('\n');
        buf_.append(static_cast<size_t>(indent_ * 2), ' ');
    }

    std::string& buf_;
    std::vector<bool> need_comma_stack_;
    int indent_ = 0;
};

struct JsonValue {
    enum class Kind : uint8_t {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Null;
    bool bool_val = false;
    bool is_neg_int = false;
    bool is_float = false;
    double double_val = 0;
    uint64_t uint_val = 0;
    int64_t int_val = 0;
    std::string str_utf8;
    std::wstring str_utf16;
    std::vector<JsonValue> array_vals;
    std::vector<std::pair<std::string, JsonValue>> object_vals;

    const JsonValue* Find(std::string_view key) const noexcept {
        if (kind != Kind::Object) {
            return nullptr;
        }
        for (auto it = object_vals.rbegin(); it != object_vals.rend(); ++it) {
            if (it->first == key) {
                return &it->second;
            }
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) noexcept : src_(input) {
        if (src_.size() >= 3 &&
            static_cast<uint8_t>(src_[0]) == 0xEFu &&
            static_cast<uint8_t>(src_[1]) == 0xBBu &&
            static_cast<uint8_t>(src_[2]) == 0xBFu) {
            pos_ = 3;
        }
    }

    bool ParseDocument(JsonValue& out) {
        SkipWs();
        if (!ParseValue(out, 0)) {
            return false;
        }
        SkipWs();
        return pos_ == src_.size();
    }

private:
    static constexpr int kMaxNesting = 32;

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
        if (src_.size() - pos_ < lit.size()) {
            return false;
        }
        if (src_.substr(pos_, lit.size()) != lit) {
            return false;
        }
        pos_ += lit.size();
        return true;
    }

    static bool ParseHexQuad(std::string_view s, uint16_t& out) noexcept {
        if (s.size() < 4) {
            return false;
        }
        uint16_t v = 0;
        for (size_t i = 0; i < 4; ++i) {
            const char c = s[i];
            uint16_t d = 0;
            if (c >= '0' && c <= '9') {
                d = static_cast<uint16_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = static_cast<uint16_t>(10 + (c - 'a'));
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<uint16_t>(10 + (c - 'A'));
            } else {
                return false;
            }
            v = static_cast<uint16_t>((v << 4) | d);
        }
        out = v;
        return true;
    }

    bool ParseString(std::string& out_utf8, std::wstring& out_utf16) {
        if (pos_ >= src_.size() || src_[pos_] != '"') {
            return false;
        }
        ++pos_;
        out_utf8.clear();
        out_utf16.clear();

        while (pos_ < src_.size()) {
            const uint8_t b0 = static_cast<uint8_t>(src_[pos_]);
            if (b0 == '"') {
                ++pos_;
                return true;
            }
            if (b0 < 0x20u) {
                return false;
            }
            if (b0 == '\\') {
                ++pos_;
                if (pos_ >= src_.size()) {
                    return false;
                }
                const char esc = src_[pos_++];
                switch (esc) {
                case '"':  out_utf8.push_back('"');  out_utf16.push_back(L'"');  break;
                case '\\': out_utf8.push_back('\\'); out_utf16.push_back(L'\\'); break;
                case '/':  out_utf8.push_back('/');  out_utf16.push_back(L'/');  break;
                case 'b':  out_utf8.push_back('\b'); out_utf16.push_back(L'\b'); break;
                case 'f':  out_utf8.push_back('\f'); out_utf16.push_back(L'\f'); break;
                case 'n':  out_utf8.push_back('\n'); out_utf16.push_back(L'\n'); break;
                case 'r':  out_utf8.push_back('\r'); out_utf16.push_back(L'\r'); break;
                case 't':  out_utf8.push_back('\t'); out_utf16.push_back(L'\t'); break;
                case 'u': {
                    uint16_t u1 = 0;
                    if (src_.size() - pos_ < 4 || !ParseHexQuad(src_.substr(pos_, 4), u1)) {
                        return false;
                    }
                    pos_ += 4;
                    if (u1 >= 0xD800u && u1 <= 0xDBFFu &&
                        src_.size() - pos_ >= 6 && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                        uint16_t u2 = 0;
                        if (ParseHexQuad(src_.substr(pos_ + 2, 4), u2) &&
                            u2 >= 0xDC00u && u2 <= 0xDFFFu) {
                            pos_ += 6;
                            const uint32_t cp = 0x10000u +
                                                ((static_cast<uint32_t>(u1 - 0xD800u) << 10) |
                                                 static_cast<uint32_t>(u2 - 0xDC00u));
                            AppendCodepointUtf8(out_utf8, cp);
                            out_utf16.push_back(static_cast<wchar_t>(u1));
                            out_utf16.push_back(static_cast<wchar_t>(u2));
                            break;
                        }
                    }
                    out_utf16.push_back(static_cast<wchar_t>(u1));
                    if (u1 >= 0xD800u && u1 <= 0xDFFFu) {
                        AppendCodepointUtf8(out_utf8, 0xFFFDu);
                    } else {
                        AppendCodepointUtf8(out_utf8, u1);
                    }
                    break;
                }
                default:
                    return false;
                }
                continue;
            }

            // Unescaped UTF-8 sequence
            if (b0 < 0x80u) {
                out_utf8.push_back(static_cast<char>(b0));
                out_utf16.push_back(static_cast<wchar_t>(b0));
                ++pos_;
            } else if ((b0 & 0xE0u) == 0xC0u) {
                if (src_.size() - pos_ < 2) {
                    return false;
                }
                const uint8_t b1 = static_cast<uint8_t>(src_[pos_ + 1]);
                if ((b1 & 0xC0u) != 0x80u) {
                    return false;
                }
                const uint32_t cp = ((static_cast<uint32_t>(b0 & 0x1Fu) << 6) |
                                     static_cast<uint32_t>(b1 & 0x3Fu));
                if (cp < 0x80u) {
                    return false;
                }
                out_utf8.append(src_.data() + pos_, 2);
                AppendCodepointUtf16(out_utf16, cp);
                pos_ += 2;
            } else if ((b0 & 0xF0u) == 0xE0u) {
                if (src_.size() - pos_ < 3) {
                    return false;
                }
                const uint8_t b1 = static_cast<uint8_t>(src_[pos_ + 1]);
                const uint8_t b2 = static_cast<uint8_t>(src_[pos_ + 2]);
                if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) {
                    return false;
                }
                const uint32_t cp = ((static_cast<uint32_t>(b0 & 0x0Fu) << 12) |
                                     (static_cast<uint32_t>(b1 & 0x3Fu) << 6) |
                                     static_cast<uint32_t>(b2 & 0x3Fu));
                if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) {
                    return false;
                }
                out_utf8.append(src_.data() + pos_, 3);
                AppendCodepointUtf16(out_utf16, cp);
                pos_ += 3;
            } else if ((b0 & 0xF8u) == 0xF0u) {
                if (src_.size() - pos_ < 4) {
                    return false;
                }
                const uint8_t b1 = static_cast<uint8_t>(src_[pos_ + 1]);
                const uint8_t b2 = static_cast<uint8_t>(src_[pos_ + 2]);
                const uint8_t b3 = static_cast<uint8_t>(src_[pos_ + 3]);
                if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u) {
                    return false;
                }
                const uint32_t cp = ((static_cast<uint32_t>(b0 & 0x07u) << 18) |
                                     (static_cast<uint32_t>(b1 & 0x3Fu) << 12) |
                                     (static_cast<uint32_t>(b2 & 0x3Fu) << 6) |
                                     static_cast<uint32_t>(b3 & 0x3Fu));
                if (cp < 0x10000u || cp > 0x10FFFFu) {
                    return false;
                }
                out_utf8.append(src_.data() + pos_, 4);
                AppendCodepointUtf16(out_utf16, cp);
                pos_ += 4;
            } else {
                return false;
            }
        }
        return false;
    }

    bool ParseNumber(JsonValue& out) {
        const size_t start = pos_;
        bool neg = false;
        if (pos_ < src_.size() && src_[pos_] == '-') {
            neg = true;
            ++pos_;
        }
        const size_t digits_start = pos_;
        if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
            return false;
        }
        if (src_[pos_] == '0') {
            ++pos_;
            if (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                return false;
            }
        } else {
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        }

        bool has_frac_or_exp = false;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            has_frac_or_exp = true;
            ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
                return false;
            }
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            has_frac_or_exp = true;
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
                return false;
            }
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        }

        out = JsonValue{};
        out.kind = JsonValue::Kind::Number;
        out.is_neg_int = neg;
        out.is_float = has_frac_or_exp;

        if (!has_frac_or_exp) {
            uint64_t mag = 0;
            const auto res = std::from_chars(src_.data() + digits_start, src_.data() + pos_, mag);
            if (res.ec != std::errc{} || res.ptr != src_.data() + pos_) {
                return false;
            }
            if (neg) {
                constexpr uint64_t kMaxNegMag =
                    static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ULL;
                if (mag > kMaxNegMag) {
                    return false;
                }
                out.int_val = (mag == kMaxNegMag)
                                  ? std::numeric_limits<int64_t>::min()
                                  : -static_cast<int64_t>(mag);
                out.uint_val = 0;
            } else {
                out.uint_val = mag;
                out.int_val = (mag <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                                  ? static_cast<int64_t>(mag)
                                  : std::numeric_limits<int64_t>::max();
            }
        } else {
            double dv = 0.0;
            const auto res = std::from_chars(src_.data() + start, src_.data() + pos_, dv);
            if (res.ec != std::errc{}) {
                return false;
            }
            if (!std::isfinite(dv)) return false;
            out.double_val = dv;
            out.int_val = dv >= static_cast<double>(INT64_MAX) ? INT64_MAX : dv <= static_cast<double>(INT64_MIN) ? INT64_MIN : static_cast<int64_t>(dv);
            out.uint_val = dv >= static_cast<double>(UINT64_MAX) ? UINT64_MAX : dv >= 0.0 ? static_cast<uint64_t>(dv) : 0u;
        }
        return true;
    }

    bool ParseArray(JsonValue& out, int depth) {
        ++pos_; // '['
        out = JsonValue{};
        out.kind = JsonValue::Kind::Array;
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipWs();
            JsonValue elem{};
            if (!ParseValue(elem, depth + 1)) {
                return false;
            }
            out.array_vals.push_back(std::move(elem));
            SkipWs();
            if (pos_ >= src_.size()) {
                return false;
            }
            if (src_[pos_] == ']') {
                ++pos_;
                return true;
            }
            if (src_[pos_] != ',') {
                return false;
            }
            ++pos_;
        }
    }

    bool ParseObject(JsonValue& out, int depth) {
        ++pos_; // '{'
        out = JsonValue{};
        out.kind = JsonValue::Kind::Object;
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipWs();
            std::string key_utf8;
            std::wstring key_utf16;
            if (!ParseString(key_utf8, key_utf16)) {
                return false;
            }
            SkipWs();
            if (pos_ >= src_.size() || src_[pos_] != ':') {
                return false;
            }
            ++pos_;
            SkipWs();
            JsonValue val{};
            if (!ParseValue(val, depth + 1)) {
                return false;
            }
            out.object_vals.emplace_back(std::move(key_utf8), std::move(val));
            SkipWs();
            if (pos_ >= src_.size()) {
                return false;
            }
            if (src_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (src_[pos_] != ',') {
                return false;
            }
            ++pos_;
        }
    }

    bool ParseValue(JsonValue& out, int depth) {
        if (depth > kMaxNesting || pos_ >= src_.size()) {
            return false;
        }
        const char c = src_[pos_];
        if (c == '{') {
            return ParseObject(out, depth);
        }
        if (c == '[') {
            return ParseArray(out, depth);
        }
        if (c == '"') {
            out = JsonValue{};
            out.kind = JsonValue::Kind::String;
            return ParseString(out.str_utf8, out.str_utf16);
        }
        if (c == 't') {
            if (!MatchLiteral("true")) {
                return false;
            }
            out = JsonValue{};
            out.kind = JsonValue::Kind::Bool;
            out.bool_val = true;
            return true;
        }
        if (c == 'f') {
            if (!MatchLiteral("false")) {
                return false;
            }
            out = JsonValue{};
            out.kind = JsonValue::Kind::Bool;
            out.bool_val = false;
            return true;
        }
        if (c == 'n') {
            if (!MatchLiteral("null")) {
                return false;
            }
            out = JsonValue{};
            out.kind = JsonValue::Kind::Null;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return ParseNumber(out);
        }
        return false;
    }

    std::string_view src_;
    size_t pos_ = 0;
};

bool ReadBoolField(const JsonValue& obj, std::string_view key, bool& target) noexcept {
    const JsonValue* v = obj.Find(key);
    if (!v) {
        return true;
    }
    if (v->kind != JsonValue::Kind::Bool) {
        return false;
    }
    target = v->bool_val;
    return true;
}

bool ReadIntField(const JsonValue& obj, std::string_view key, int& target) noexcept {
    const JsonValue* v = obj.Find(key);
    if (!v) {
        return true;
    }
    if (v->kind != JsonValue::Kind::Number || v->is_float) {
        return false;
    }
    if (v->int_val < std::numeric_limits<int>::min() ||
        v->int_val > std::numeric_limits<int>::max()) {
        return false;
    }
    target = static_cast<int>(v->int_val);
    return true;
}

bool ReadInt32Field(const JsonValue& obj, std::string_view key, int32_t& target) noexcept {
    const JsonValue* v = obj.Find(key);
    if (!v) {
        return true;
    }
    if (v->kind != JsonValue::Kind::Number || v->is_float) {
        return false;
    }
    if (v->int_val < std::numeric_limits<int32_t>::min() ||
        v->int_val > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    target = static_cast<int32_t>(v->int_val);
    return true;
}

bool ReadInt64Field(const JsonValue& obj, std::string_view key, int64_t& target) noexcept {
    const JsonValue* v = obj.Find(key);
    if (!v) {
        return true;
    }
    if (v->kind != JsonValue::Kind::Number || v->is_float) {
        return false;
    }
    if (!v->is_neg_int && v->uint_val > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return false;
    }
    target = v->int_val;
    return true;
}

bool ReadUint64Field(const JsonValue& obj, std::string_view key, uint64_t& target) noexcept {
    const JsonValue* v = obj.Find(key);
    if (!v) {
        return true;
    }
    if (v->kind != JsonValue::Kind::Number || v->is_float || v->is_neg_int) {
        return false;
    }
    target = v->uint_val;
    return true;
}

bool ReadUint32Field(const JsonValue& obj, std::string_view key, uint32_t& target) noexcept {
    uint64_t tmp = target;
    if (!ReadUint64Field(obj, key, tmp) || tmp > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    target = static_cast<uint32_t>(tmp);
    return true;
}

bool ReadUint8Field(const JsonValue& obj, std::string_view key, uint8_t& target) noexcept {
    uint64_t tmp = target;
    if (!ReadUint64Field(obj, key, tmp) || tmp > std::numeric_limits<uint8_t>::max()) {
        return false;
    }
    target = static_cast<uint8_t>(tmp);
    return true;
}

template <typename EnumT>
bool ReadEnumField(const JsonValue& obj, std::string_view key, uint8_t max_val, EnumT& target) noexcept {
    uint8_t raw = static_cast<uint8_t>(target);
    if (!ReadUint8Field(obj, key, raw) || raw > max_val) {
        return false;
    }
    target = static_cast<EnumT>(raw);
    return true;
}

bool ReadMaterialField(const JsonValue& obj, std::string_view key,
                       CardMaterial fallback, CardMaterial& target) noexcept {
    uint8_t raw = static_cast<uint8_t>(target);
    if (!ReadUint8Field(obj, key, raw)) {
        return false;
    }
    target = (raw <= static_cast<uint8_t>(CardMaterial::LiquidGlass))
                 ? static_cast<CardMaterial>(raw)
                 : fallback;
    return true;
}

bool ReadUtf8Field(const JsonValue& obj, std::string_view key, std::string& target) {
    const JsonValue* v = obj.Find(key);
    if (!v) {
        return true;
    }
    if (v->kind != JsonValue::Kind::String) {
        return false;
    }
    target = v->str_utf8;
    return true;
}

bool ReadWideField(const JsonValue& obj, std::string_view key, std::wstring& target) {
    const JsonValue* v = obj.Find(key);
    if (!v) {
        return true;
    }
    if (v->kind != JsonValue::Kind::String) {
        return false;
    }
    target = v->str_utf16;
    return true;
}

bool ReadGlyphField(const JsonValue& obj, std::string_view key, wchar_t& target) noexcept {
    const JsonValue* v = obj.Find(key);
    if (!v) {
        return true;
    }
    if (v->kind == JsonValue::Kind::Number && !v->is_float && !v->is_neg_int && v->uint_val <= 0xFFFFu) {
        target = static_cast<wchar_t>(v->uint_val);
        return true;
    }
    if (v->kind == JsonValue::Kind::String && !v->str_utf16.empty()) {
        target = v->str_utf16.front();
        return true;
    }
    return false;
}

bool ReadWideArrayField(const JsonValue& obj, std::string_view key, std::vector<std::wstring>& target) {
    const JsonValue* v = obj.Find(key);
    if (!v) {
        return true;
    }
    if (v->kind != JsonValue::Kind::Array) {
        return false;
    }
    target.clear();
    target.reserve(v->array_vals.size());
    for (const JsonValue& elem : v->array_vals) {
        if (elem.kind != JsonValue::Kind::String) {
            return false;
        }
        target.push_back(elem.str_utf16);
    }
    return true;
}

bool ReadPointObj(const JsonValue& v, PointI& out) noexcept {
    if (v.kind != JsonValue::Kind::Object) {
        return false;
    }
    return ReadIntField(v, "x", out.x) && ReadIntField(v, "y", out.y);
}

bool ReadPointField(const JsonValue& obj, std::string_view key, PointI& target) noexcept {
    const JsonValue* v = obj.Find(key);
    if (!v) {
        return true;
    }
    return ReadPointObj(*v, target);
}

bool ReadRectField(const JsonValue& obj, std::string_view key, RectI& target) noexcept {
    const JsonValue* v = obj.Find(key);
    if (!v) {
        return true;
    }
    if (v->kind != JsonValue::Kind::Object) {
        return false;
    }
    return ReadIntField(*v, "x", target.x) &&
           ReadIntField(*v, "y", target.y) &&
           ReadIntField(*v, "w", target.w) &&
           ReadIntField(*v, "h", target.h);
}

} // namespace

std::string SerializeModelStateJson(const DesktopModelState& state) {
    std::string out;
    out.reserve(4096);
    JsonWriter w(out);

    w.BeginObject();
    w.Key("version");
    w.WriteInt64(1);

    // settings
    w.Key("settings");
    w.BeginObject();
    w.Key("dark_mode"); w.WriteBool(state.settings.dark_mode);
    w.Key("default_material"); w.WriteUint64(static_cast<uint8_t>(state.settings.default_material));
    w.Key("card_opacity_percent"); w.WriteUint64(state.settings.card_opacity_percent);
    w.Key("auto_wallpaper_accent"); w.WriteBool(state.settings.auto_wallpaper_accent);
    w.Key("accent_argb"); w.WriteUint64(state.settings.accent_argb);
    w.Key("wallpaper_preset"); w.WriteUint64(static_cast<uint8_t>(state.settings.wallpaper_preset));
    w.Key("label_style"); w.WriteUint64(static_cast<uint8_t>(state.settings.label_style));
    w.Key("icon_size"); w.WriteUint64(static_cast<uint8_t>(state.settings.icon_size));
    w.Key("active_scene"); w.WriteUint64(static_cast<uint8_t>(state.settings.active_scene));
    w.Key("organize_enabled"); w.WriteBool(state.settings.organize_enabled);
    w.Key("tiles_enabled"); w.WriteBool(state.settings.tiles_enabled);
    w.Key("live_tiles_enabled"); w.WriteBool(state.settings.live_tiles_enabled);
    w.Key("align_to_grid"); w.WriteBool(state.settings.align_to_grid);
    w.Key("loose_sort_by"); w.WriteUint64(static_cast<uint8_t>(state.settings.loose_sort_by));
    w.Key("loose_sort_ascending"); w.WriteBool(state.settings.loose_sort_ascending);
    w.Key("double_click_hide_icons"); w.WriteBool(state.settings.double_click_hide_icons);
    w.Key("icons_hidden"); w.WriteBool(state.settings.icons_hidden);
    w.Key("type_to_search"); w.WriteBool(state.settings.type_to_search);
    w.Key("sync_staging_tray"); w.WriteBool(state.settings.sync_staging_tray);
    w.Key("hybrid_context_menu"); w.WriteBool(state.settings.hybrid_context_menu);
    w.Key("health_check_enabled"); w.WriteBool(state.settings.health_check_enabled);
    w.Key("restore_on_exit"); w.WriteBool(state.settings.restore_on_exit);
    w.Key("pause_on_fullscreen_or_battery"); w.WriteBool(state.settings.pause_on_fullscreen_or_battery);
    w.Key("glass_preset"); w.WriteUint64(std::min<uint8_t>(state.settings.glass_preset, 5));
    w.Key("glass_thickness"); w.WriteUint64(std::clamp<uint8_t>(state.settings.glass_thickness, 1, 80));
    w.Key("glass_ior"); w.WriteUint64(std::clamp<uint16_t>(state.settings.glass_ior_x100, 100, 400));
    w.Key("glass_disp_gain"); w.WriteUint64(std::min<uint8_t>(state.settings.glass_dispersion, 50));
    w.Key("glass_glare"); w.WriteUint64(std::min<uint8_t>(state.settings.glass_glare, 120));
    w.Key("glass_angle"); w.WriteInt64(std::clamp<int>(state.settings.glass_glare_angle, -180, 180));
    w.Key("glass_schema"); w.WriteUint64(2);
    for (const auto& field : kGlassOpticalFields) {
        w.Key(field.key); w.WriteDouble(ClampGlassValue(field,state.settings.glass_options.*(field.member)));
    }
    w.Key("glass2_blur_edge"); w.WriteBool(state.settings.glass_options.blur_edge);
    w.Key("glass_reference"); w.WriteBool(state.settings.glass_reference);
    w.Key("glass_blur_radius"); w.WriteUint64(state.settings.glass_blur_radius);
    w.Key("glass_blur_edge"); w.WriteBool(state.settings.glass_blur_edge);
    w.Key("glass_frost"); w.WriteUint64(std::min<uint8_t>(state.settings.glass_frost, 100));
    w.Key("glass_dispersion"); w.WriteUint64(MapGlassGainToLegacyTier(state.settings.glass_dispersion));
    w.Key("glass_live_rate"); w.WriteUint64(std::min<uint8_t>(state.settings.glass_live_rate, 2));
    w.Key("dock_enabled"); w.WriteBool(state.settings.dock_enabled);
    w.Key("dock_icon_size"); w.WriteUint64(std::clamp<uint8_t>(state.settings.dock_icon_size, 36, 68));
    w.Key("dock_magnification"); w.WriteBool(state.settings.dock_magnification);
    w.Key("dock_max_scale_x100"); w.WriteUint64(std::clamp<uint16_t>(state.settings.dock_max_scale_x100, 100, 195));
    w.Key("dock_hide_mode"); w.WriteUint64(std::min<uint8_t>(state.settings.dock_hide_mode, 2));
    w.Key("dock_minimize_effect"); w.WriteUint64(std::min<uint8_t>(state.settings.dock_minimize_effect, 2));
    w.Key("dock_position"); w.WriteUint64(std::min<uint8_t>(state.settings.dock_position, 2));
    w.Key("dock_system_area"); w.WriteBool(state.settings.dock_system_area);
    w.Key("dock_stack_view"); w.WriteUint64(std::min<uint8_t>(state.settings.dock_stack_view, 3));
    w.Key("dock_stack_sort"); w.WriteUint64(std::min<uint8_t>(state.settings.dock_stack_sort, 3));
    w.Key("dock_apps_stack"); w.WriteBool(state.settings.dock_apps_stack);
    w.Key("dock_window_preview"); w.WriteBool(state.settings.dock_window_preview);
    w.Key("dock_pinned_paths");
    w.BeginArray();
    for (const auto& dp : state.settings.dock_pinned_paths) {
        if (!dp.empty()) {
            w.Elem();
            w.WriteWideStringLiteral(dp);
        }
    }
    w.EndArray();
    w.Key("dock_tray_pinned_keys");
    w.BeginArray();
    for (const auto& tk : state.settings.dock_tray_pinned_keys) {
        if (!tk.empty()) {
            w.Elem();
            w.WriteWideStringLiteral(tk);
        }
    }
    w.EndArray();
    w.Key("weather_city"); w.WriteWideStringLiteral(state.settings.weather_city);
    w.Key("widgets"); w.WriteWideStringLiteral(EncodeWidgetPrefs(state.settings.widgets));
    w.EndObject();

    // monitors
    w.Key("monitors");
    w.BeginArray();
    for (const MonitorWorkArea& m : state.monitors) {
        w.Elem();
        w.BeginObject();
        w.Key("device_name"); w.WriteWideStringLiteral(m.device_name);
        w.Key("bounds"); w.WriteRect(m.bounds);
        w.Key("work_area"); w.WriteRect(m.work_area);
        w.Key("dpi"); w.WriteUint64(m.dpi);
        w.Key("primary"); w.WriteBool(m.primary);
        w.EndObject();
    }
    w.EndArray();

    // items
    w.Key("items");
    w.BeginArray();
    for (const DesktopItem& it : state.items) {
        w.Elem();
        w.BeginObject();
        w.Key("path"); w.WriteWideStringLiteral(it.path);
        w.Key("name"); w.WriteWideStringLiteral(it.name);
        w.Key("extension"); w.WriteWideStringLiteral(it.extension);
        w.Key("link_target"); w.WriteWideStringLiteral(it.link_target);
        w.Key("installed_app_hint"); w.WriteWideStringLiteral(it.installed_app_hint);
        w.Key("is_folder"); w.WriteBool(it.is_folder);
        w.Key("folder_is_empty"); w.WriteBool(it.folder_is_empty);
        w.Key("is_shortcut"); w.WriteBool(it.is_shortcut);
        w.Key("link_target_missing"); w.WriteBool(it.link_target_missing);
        w.Key("size_bytes"); w.WriteUint64(it.size_bytes);
        w.Key("modified_unix_ms"); w.WriteInt64(it.modified_unix_ms);
        w.Key("tag_argb"); w.WriteUint64(it.tag_argb);
        w.Key("tag_id"); w.WriteWideStringLiteral(it.tag_id);
        w.Key("native_pos"); w.WritePoint(it.native_pos);
        w.Key("loose_pos"); w.WritePoint(it.loose_pos);
        w.Key("pinned_group_id"); w.WriteUtf8StringLiteral(it.pinned_group_id);
        w.Key("force_loose"); w.WriteBool(it.force_loose);
        w.EndObject();
    }
    w.EndArray();

    // groups
    w.Key("groups");
    w.BeginArray();
    for (const DesktopGroup& g : state.groups) {
        w.Elem();
        w.BeginObject();
        w.Key("id"); w.WriteUtf8StringLiteral(g.id);
        w.Key("title"); w.WriteWideStringLiteral(g.title);
        w.Key("icon_glyph"); w.WriteUint64(static_cast<uint16_t>(g.icon_glyph));
        w.Key("rule_summary"); w.WriteWideStringLiteral(g.rule_summary);
        w.Key("mapped_folder_path"); w.WriteWideStringLiteral(g.mapped_folder_path);
        w.Key("accent_argb"); w.WriteUint64(g.accent_argb);
        w.Key("material"); w.WriteUint64(static_cast<uint8_t>(g.material));
        w.Key("sort_by"); w.WriteUint64(static_cast<uint8_t>(g.sort_by));
        w.Key("sort_ascending"); w.WriteBool(g.sort_ascending);
        w.Key("collapsed"); w.WriteBool(g.collapsed);
        w.Key("view_mode"); w.WriteUint64(static_cast<uint8_t>(g.view_mode));
        w.Key("tab_stack_id"); w.WriteUtf8StringLiteral(g.tab_stack_id);
        w.Key("tab_order"); w.WriteInt64(g.tab_order);
        w.Key("tab_active"); w.WriteBool(g.tab_active);
        w.Key("focus_visible"); w.WriteBool(g.focus_visible);
        w.Key("locked"); w.WriteBool(g.locked);
        w.Key("x"); w.WriteInt64(g.x);
        w.Key("y"); w.WriteInt64(g.y);
        w.Key("cols"); w.WriteInt64(g.cols);
        w.Key("visible_rows"); w.WriteInt64(g.visible_rows);
        w.Key("rest_origin");
        if (g.rest_origin.has_value()) {
            w.WritePoint(*g.rest_origin);
        } else {
            w.WriteNull();
        }

        w.Key("rule");
        w.BeginObject();
        w.Key("extensions"); w.WriteWideArray(g.rule.extensions);
        w.Key("name_prefixes"); w.WriteWideArray(g.rule.name_prefixes);
        w.Key("name_contains"); w.WriteWideArray(g.rule.name_contains);
        w.Key("tag_id"); w.WriteWideStringLiteral(g.rule.tag_id);
        w.Key("modified_within_days"); w.WriteInt64(g.rule.modified_within_days);
        w.Key("match_folders"); w.WriteBool(g.rule.match_folders);
        w.Key("match_files"); w.WriteBool(g.rule.match_files);
        w.Key("fallback_catch_all"); w.WriteBool(g.rule.fallback_catch_all);
        w.EndObject();

        w.Key("manual_order"); w.WriteWideArray(g.manual_order);
        w.EndObject();
    }
    w.EndArray();

    // tiles
    w.Key("tiles");
    w.BeginArray();
    for (const DesktopTile& t : state.tiles) {
        w.Elem();
        w.BeginObject();
        w.Key("id"); w.WriteUtf8StringLiteral(t.id);
        w.Key("kind"); w.WriteUint64(static_cast<uint8_t>(t.kind));
        w.Key("title"); w.WriteWideStringLiteral(t.title);
        w.Key("target_path"); w.WriteWideStringLiteral(t.target_path);
        w.Key("note_body"); w.WriteWideStringLiteral(t.note_body);
        if (!t.countdown_date.empty()) {
            w.Key("countdown_date"); w.WriteWideStringLiteral(t.countdown_date);
        }
        w.Key("analog_clock"); w.WriteBool(t.analog_clock);
        w.Key("live_content"); w.WriteBool(t.live_content);
        w.Key("material"); w.WriteUint64(static_cast<uint8_t>(t.material));
        w.Key("accent_argb"); w.WriteUint64(t.accent_argb);
        w.Key("col"); w.WriteInt64(t.col);
        w.Key("row"); w.WriteInt64(t.row);
        w.Key("size");
        w.BeginObject();
        w.Key("cols"); w.WriteInt64(t.size.cols);
        w.Key("rows"); w.WriteInt64(t.size.rows);
        w.EndObject();
        w.Key("look");
        w.BeginObject();
        w.Key("background"); w.WriteUint64(static_cast<uint8_t>(t.look.background));
        w.Key("color_argb"); w.WriteUint64(t.look.color_argb);
        w.Key("image_path"); w.WriteWideStringLiteral(t.look.image_path);
        w.Key("title_override"); w.WriteWideStringLiteral(t.look.title_override);
        w.Key("show_title"); w.WriteBool(t.look.show_title);
        w.EndObject();
        w.Key("stack_index"); w.WriteInt64(t.stack_index);
        w.Key("smart_rotate"); w.WriteBool(t.smart_rotate);
        w.Key("photo_interval_s"); w.WriteInt64(t.photo_interval_s);
        w.Key("photo_shuffle"); w.WriteBool(t.photo_shuffle);
        w.Key("note_color"); w.WriteInt64(t.note_color);
        w.Key("free_placed"); w.WriteBool(t.free_placed);
        w.Key("x"); w.WriteInt64(t.x);
        w.Key("y"); w.WriteInt64(t.y);
        w.Key("w"); w.WriteInt64(t.w);
        w.Key("h"); w.WriteInt64(t.h);
        w.Key("rest_origin");
        if (t.rest_origin.has_value()) {
            w.WritePoint(*t.rest_origin);
        } else {
            w.WriteNull();
        }
        w.Key("stack");
        w.BeginArray();
        for (const TileStackEntry& m : t.stack) {
            w.Elem();
            w.BeginObject();
            w.Key("kind"); w.WriteUint64(static_cast<uint8_t>(m.kind));
            w.Key("title"); w.WriteWideStringLiteral(m.title);
            w.Key("target_path"); w.WriteWideStringLiteral(m.target_path);
            w.Key("origin_id"); w.WriteUtf8StringLiteral(m.origin_id);
            w.Key("analog_clock"); w.WriteBool(m.analog_clock);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();

    w.Key("widget_defaults_applied"); w.WriteBool(state.widget_defaults_applied);
    w.Key("todos");
    w.BeginArray();
    for (const TodoItem& td : state.todos) {
        w.Elem();
        w.BeginObject();
        w.Key("text"); w.WriteWideStringLiteral(td.text);
        w.Key("done"); w.WriteBool(td.done);
        w.EndObject();
    }
    w.EndArray();

    // video_wallpaper
    {
        const auto& vw = state.video_wallpaper;
        w.Key("video_wallpaper");
        w.BeginObject();
        w.Key("active"); w.WriteBool(vw.active);
        w.Key("source_path"); w.WriteWideStringLiteral(vw.source_path);
        w.Key("fill_mode"); w.WriteUint64(static_cast<uint8_t>(vw.fill_mode));
        w.Key("cache_key"); w.WriteWideStringLiteral(vw.cache_key);
        w.Key("cached_video_path"); w.WriteWideStringLiteral(vw.cached_video_path);
        w.Key("poster_path"); w.WriteWideStringLiteral(vw.poster_path);
        w.Key("source_width"); w.WriteUint64(vw.source_width);
        w.Key("source_height"); w.WriteUint64(vw.source_height);
        w.Key("target_width"); w.WriteUint64(vw.target_width);
        w.Key("target_height"); w.WriteUint64(vw.target_height);
        w.Key("duration_ms"); w.WriteUint64(vw.duration_ms);
        w.Key("optimized"); w.WriteBool(vw.optimized);
        w.Key("recent");
        w.BeginArray();
        for (const auto& r : vw.recent) {
            w.Elem();
            w.BeginObject();
            w.Key("path"); w.WriteWideStringLiteral(r.path);
            w.Key("poster_path"); w.WriteWideStringLiteral(r.poster_path);
            w.Key("width"); w.WriteUint64(r.width);
            w.Key("height"); w.WriteUint64(r.height);
            w.Key("duration_ms"); w.WriteUint64(r.duration_ms);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }

    w.EndObject();
    out.push_back('\n');
    return out;
}

bool DeserializeModelStateJson(std::string_view utf8, DesktopModelState& out_state) {
    JsonParser parser(utf8);
    JsonValue root{};
    if (!parser.ParseDocument(root) || root.kind != JsonValue::Kind::Object) {
        return false;
    }

    if (const JsonValue* ver = root.Find("version")) {
        if (ver->kind != JsonValue::Kind::Number || ver->is_float || ver->int_val != 1) {
            return false;
        }
    }

    DesktopModelState parsed{};

    if (const JsonValue* s = root.Find("settings")) {
        if (s->kind != JsonValue::Kind::Object) {
            return false;
        }
        if (!s->Find("glass_reference") && (s->Find("glass_preset") || s->Find("glass_thickness")))
            ApplyGlassPreset(0, parsed.settings);
        if (const auto* schema=s->Find("glass_schema"); schema && schema->kind==JsonValue::Kind::Number && schema->uint_val>=2) {
            for(const auto& field:kGlassOpticalFields) {
                if(const auto* value=s->Find(field.key)) {
                    if(value->kind!=JsonValue::Kind::Number) return false;
                    double number=value->is_float?value->double_val:value->is_neg_int?static_cast<double>(value->int_val):static_cast<double>(value->uint_val);
                    if(!std::isfinite(number)) return false;
                    parsed.settings.glass_options.*(field.member)=ClampGlassValue(field,number);
                }
            }
            if(!ReadBoolField(*s,"glass2_blur_edge",parsed.settings.glass_options.blur_edge)) return false;
        }
        uint32_t blur_tmp = parsed.settings.glass_blur_radius;
        if (!ReadBoolField(*s, "glass_reference", parsed.settings.glass_reference) ||
            !ReadBoolField(*s, "glass_blur_edge", parsed.settings.glass_blur_edge) ||
            !ReadUint32Field(*s, "glass_blur_radius", blur_tmp)) return false;
        parsed.settings.glass_blur_radius = static_cast<uint8_t>(std::clamp<uint32_t>(blur_tmp, 1, 200));
        uint32_t preset_tmp = parsed.settings.glass_preset;
        uint32_t thick_tmp = parsed.settings.glass_thickness;
        uint32_t ior_tmp = parsed.settings.glass_ior_x100;
        uint32_t disp_gain_tmp = parsed.settings.glass_dispersion;
        uint32_t legacy_disp_tmp = 1;
        uint32_t glare_tmp = parsed.settings.glass_glare;
        int32_t glare_angle_tmp = parsed.settings.glass_glare_angle;
        uint32_t frost_tmp = parsed.settings.glass_frost;
        uint32_t rate_tmp = parsed.settings.glass_live_rate;
        uint32_t dock_sz_tmp = parsed.settings.dock_icon_size;
        uint32_t dock_scale_tmp = parsed.settings.dock_max_scale_x100;
        uint32_t dock_hide_tmp = parsed.settings.dock_hide_mode;
        uint32_t dock_min_fx_tmp = parsed.settings.dock_minimize_effect;
        uint32_t dock_pos_tmp = parsed.settings.dock_position;
        if (!ReadBoolField(*s, "dark_mode", parsed.settings.dark_mode) ||
            !ReadMaterialField(*s, "default_material", CardMaterial::Acrylic,
                               parsed.settings.default_material) ||
            !ReadUint8Field(*s, "card_opacity_percent", parsed.settings.card_opacity_percent) ||
            !ReadBoolField(*s, "auto_wallpaper_accent", parsed.settings.auto_wallpaper_accent) ||
            !ReadUint32Field(*s, "accent_argb", parsed.settings.accent_argb) ||
            !ReadEnumField(*s, "wallpaper_preset", static_cast<uint8_t>(WallpaperPreset::Photo),
                           parsed.settings.wallpaper_preset) ||
            !ReadEnumField(*s, "label_style", static_cast<uint8_t>(IconLabelStyle::Normal),
                           parsed.settings.label_style) ||
            !ReadEnumField(*s, "icon_size", static_cast<uint8_t>(DesktopIconSize::Large),
                           parsed.settings.icon_size) ||
            !ReadEnumField(*s, "active_scene", static_cast<uint8_t>(DesktopScene::Present),
                           parsed.settings.active_scene) ||
            !ReadBoolField(*s, "organize_enabled", parsed.settings.organize_enabled) ||
            !ReadBoolField(*s, "tiles_enabled", parsed.settings.tiles_enabled) ||
            !ReadBoolField(*s, "live_tiles_enabled", parsed.settings.live_tiles_enabled) ||
            !ReadBoolField(*s, "align_to_grid", parsed.settings.align_to_grid) ||
            !ReadEnumField(*s, "loose_sort_by", static_cast<uint8_t>(GroupSortBy::Manual),
                           parsed.settings.loose_sort_by) ||
            !ReadBoolField(*s, "loose_sort_ascending", parsed.settings.loose_sort_ascending) ||
            !ReadBoolField(*s, "double_click_hide_icons", parsed.settings.double_click_hide_icons) ||
            !ReadBoolField(*s, "icons_hidden", parsed.settings.icons_hidden) ||
            !ReadBoolField(*s, "type_to_search", parsed.settings.type_to_search) ||
            !ReadBoolField(*s, "sync_staging_tray", parsed.settings.sync_staging_tray) ||
            !ReadBoolField(*s, "hybrid_context_menu", parsed.settings.hybrid_context_menu) ||
            !ReadBoolField(*s, "health_check_enabled", parsed.settings.health_check_enabled) ||
            !ReadBoolField(*s, "restore_on_exit", parsed.settings.restore_on_exit) ||
            !ReadBoolField(*s, "pause_on_fullscreen_or_battery",
                           parsed.settings.pause_on_fullscreen_or_battery) ||
            !ReadUint32Field(*s, "glass_preset", preset_tmp) ||
            !ReadUint32Field(*s, "glass_thickness", thick_tmp) ||
            !ReadUint32Field(*s, "glass_ior", ior_tmp) ||
            !ReadUint32Field(*s, "glass_disp_gain", disp_gain_tmp) ||
            !ReadUint32Field(*s, "glass_dispersion", legacy_disp_tmp) ||
            !ReadUint32Field(*s, "glass_glare", glare_tmp) ||
            !ReadInt32Field(*s, "glass_angle", glare_angle_tmp) ||
            !ReadUint32Field(*s, "glass_frost", frost_tmp) ||
            !ReadUint32Field(*s, "glass_live_rate", rate_tmp) ||
            !ReadBoolField(*s, "dock_enabled", parsed.settings.dock_enabled) ||
            !ReadUint32Field(*s, "dock_icon_size", dock_sz_tmp) ||
            !ReadBoolField(*s, "dock_magnification", parsed.settings.dock_magnification) ||
            !ReadUint32Field(*s, "dock_max_scale_x100", dock_scale_tmp) ||
            !ReadUint32Field(*s, "dock_hide_mode", dock_hide_tmp) ||
            !ReadUint32Field(*s, "dock_minimize_effect", dock_min_fx_tmp) ||
            !ReadUint32Field(*s, "dock_position", dock_pos_tmp) ||
            !ReadWideField(*s, "weather_city", parsed.settings.weather_city)) {
            return false;
        }
        // 小组件 & Dock 系统状态区 options arrived later than the rest: optional, defaults when absent.
        if (s->Find("dock_system_area")) {
            if (!ReadBoolField(*s, "dock_system_area", parsed.settings.dock_system_area)) return false;
        }
        // Dock stacks (叠放) arrived later still: optional as well.
        if (s->Find("dock_stack_view")) {
            uint32_t v = 0;
            if (!ReadUint32Field(*s, "dock_stack_view", v)) return false;
            parsed.settings.dock_stack_view = static_cast<uint8_t>(std::min<uint32_t>(v, 3u));
        }
        if (s->Find("dock_stack_sort")) {
            uint32_t v = 0;
            if (!ReadUint32Field(*s, "dock_stack_sort", v)) return false;
            parsed.settings.dock_stack_sort = static_cast<uint8_t>(std::min<uint32_t>(v, 3u));
        }
        if (s->Find("dock_apps_stack")) {
            if (!ReadBoolField(*s, "dock_apps_stack", parsed.settings.dock_apps_stack)) return false;
        }
        if (s->Find("dock_window_preview")) {
            if (!ReadBoolField(*s, "dock_window_preview", parsed.settings.dock_window_preview)) return false;
        }
        if (const JsonValue* dtp = s->Find("dock_tray_pinned_keys")) {
            if (dtp->kind != JsonValue::Kind::Array) return false;
            parsed.settings.dock_tray_pinned_keys.clear();
            parsed.settings.dock_tray_pinned_keys.reserve(dtp->array_vals.size());
            for (const JsonValue& pv : dtp->array_vals) {
                if (pv.kind != JsonValue::Kind::String) return false;
                if (!pv.str_utf16.empty()) parsed.settings.dock_tray_pinned_keys.push_back(pv.str_utf16);
            }
        }
        if (s->Find("widgets")) {
            std::wstring enc;
            if (ReadWideField(*s, "widgets", enc)) DecodeWidgetPrefs(enc, parsed.settings.widgets);
        }
        parsed.settings.dock_icon_size = static_cast<uint8_t>(std::clamp<uint32_t>(dock_sz_tmp, 36u, 68u));
        parsed.settings.dock_max_scale_x100 = static_cast<uint16_t>(std::clamp<uint32_t>(dock_scale_tmp, 100u, 195u));
        parsed.settings.dock_hide_mode = static_cast<uint8_t>(std::min<uint32_t>(dock_hide_tmp, 2u));
        parsed.settings.dock_minimize_effect = static_cast<uint8_t>(std::min<uint32_t>(dock_min_fx_tmp, 2u));
        parsed.settings.dock_position = static_cast<uint8_t>(std::min<uint32_t>(dock_pos_tmp, 2u));
        if (const JsonValue* dp = s->Find("dock_pinned_paths")) {
            if (dp->kind != JsonValue::Kind::Array) return false;
            parsed.settings.dock_pinned_paths.clear();
            parsed.settings.dock_pinned_paths.reserve(dp->array_vals.size());
            for (const JsonValue& pv : dp->array_vals) {
                if (pv.kind != JsonValue::Kind::String) return false;
                if (!pv.str_utf16.empty()) parsed.settings.dock_pinned_paths.push_back(pv.str_utf16);
            }
        }
        if (!s->Find("glass_disp_gain") && s->Find("glass_dispersion")) {
            disp_gain_tmp = MapLegacyGlassDispersionToGain(legacy_disp_tmp);
        }
        parsed.settings.glass_preset = static_cast<uint8_t>(std::min<uint32_t>(preset_tmp, 5u));
        parsed.settings.glass_thickness = static_cast<uint8_t>(std::clamp<uint32_t>(thick_tmp, 1u, 80u));
        parsed.settings.glass_ior_x100 = static_cast<uint16_t>(std::clamp<uint32_t>(ior_tmp, 100u, 400u));
        parsed.settings.glass_dispersion = static_cast<uint8_t>(std::min<uint32_t>(disp_gain_tmp, 50u));
        parsed.settings.glass_glare = static_cast<uint8_t>(std::min<uint32_t>(glare_tmp, 120u));
        parsed.settings.glass_glare_angle = static_cast<int16_t>(std::clamp<int32_t>(glare_angle_tmp, -180, 180));
        parsed.settings.glass_frost = static_cast<uint8_t>(std::min<uint32_t>(frost_tmp, 100u));
        if (!s->Find("glass_schema") && (s->Find("glass_preset") || s->Find("glass_thickness")))
            parsed.settings.glass_options=LegacyGlassOptions(parsed.settings.glass_thickness,parsed.settings.glass_ior_x100,
                parsed.settings.glass_dispersion,parsed.settings.glass_glare,parsed.settings.glass_glare_angle);

        parsed.settings.glass_live_rate = static_cast<uint8_t>(std::min<uint32_t>(rate_tmp, 2u));
    }

    if (const JsonValue* mons = root.Find("monitors")) {
        if (mons->kind != JsonValue::Kind::Array) {
            return false;
        }
        parsed.monitors.reserve(mons->array_vals.size());
        for (const JsonValue& mv : mons->array_vals) {
            if (mv.kind != JsonValue::Kind::Object) {
                return false;
            }
            MonitorWorkArea m{};
            if (!ReadWideField(mv, "device_name", m.device_name) ||
                !ReadRectField(mv, "bounds", m.bounds) ||
                !ReadRectField(mv, "work_area", m.work_area) ||
                !ReadUint32Field(mv, "dpi", m.dpi) ||
                !ReadBoolField(mv, "primary", m.primary)) {
                return false;
            }
            parsed.monitors.push_back(std::move(m));
        }
    }

    if (const JsonValue* items = root.Find("items")) {
        if (items->kind != JsonValue::Kind::Array) {
            return false;
        }
        parsed.items.reserve(items->array_vals.size());
        for (const JsonValue& iv : items->array_vals) {
            if (iv.kind != JsonValue::Kind::Object) {
                return false;
            }
            DesktopItem it{};
            if (!ReadWideField(iv, "path", it.path) ||
                !ReadWideField(iv, "name", it.name) ||
                !ReadWideField(iv, "extension", it.extension) ||
                !ReadWideField(iv, "link_target", it.link_target) ||
                !ReadWideField(iv, "installed_app_hint", it.installed_app_hint) ||
                !ReadBoolField(iv, "is_folder", it.is_folder) ||
                !ReadBoolField(iv, "folder_is_empty", it.folder_is_empty) ||
                !ReadBoolField(iv, "is_shortcut", it.is_shortcut) ||
                !ReadBoolField(iv, "link_target_missing", it.link_target_missing) ||
                !ReadUint64Field(iv, "size_bytes", it.size_bytes) ||
                !ReadInt64Field(iv, "modified_unix_ms", it.modified_unix_ms) ||
                !ReadUint32Field(iv, "tag_argb", it.tag_argb) ||
                !ReadWideField(iv, "tag_id", it.tag_id) ||
                !ReadPointField(iv, "native_pos", it.native_pos) ||
                !ReadPointField(iv, "loose_pos", it.loose_pos) ||
                !ReadUtf8Field(iv, "pinned_group_id", it.pinned_group_id) ||
                !ReadBoolField(iv, "force_loose", it.force_loose)) {
                return false;
            }
            parsed.items.push_back(std::move(it));
        }
    }

    if (const JsonValue* groups = root.Find("groups")) {
        if (groups->kind != JsonValue::Kind::Array) {
            return false;
        }
        parsed.groups.reserve(groups->array_vals.size());
        for (const JsonValue& gv : groups->array_vals) {
            if (gv.kind != JsonValue::Kind::Object) {
                return false;
            }
            DesktopGroup g{};
            if (!ReadUtf8Field(gv, "id", g.id) ||
                !ReadWideField(gv, "title", g.title) ||
                !ReadGlyphField(gv, "icon_glyph", g.icon_glyph) ||
                !ReadWideField(gv, "rule_summary", g.rule_summary) ||
                !ReadWideField(gv, "mapped_folder_path", g.mapped_folder_path) ||
                !ReadUint32Field(gv, "accent_argb", g.accent_argb) ||
                !ReadMaterialField(gv, "material", CardMaterial::FollowHost, g.material) ||
                !ReadEnumField(gv, "sort_by", static_cast<uint8_t>(GroupSortBy::Manual), g.sort_by) ||
                !ReadBoolField(gv, "sort_ascending", g.sort_ascending) ||
                !ReadBoolField(gv, "collapsed", g.collapsed) ||
                !ReadEnumField(gv, "view_mode", static_cast<uint8_t>(GroupViewMode::Details), g.view_mode) ||
                !ReadUtf8Field(gv, "tab_stack_id", g.tab_stack_id) ||
                !ReadIntField(gv, "tab_order", g.tab_order) ||
                !ReadBoolField(gv, "tab_active", g.tab_active) ||
                !ReadBoolField(gv, "focus_visible", g.focus_visible) ||
                !ReadBoolField(gv, "locked", g.locked) ||
                !ReadIntField(gv, "x", g.x) ||
                !ReadIntField(gv, "y", g.y) ||
                !ReadIntField(gv, "cols", g.cols) ||
                !ReadIntField(gv, "visible_rows", g.visible_rows) ||
                !ReadWideArrayField(gv, "manual_order", g.manual_order)) {
                return false;
            }

            if (const JsonValue* ro = gv.Find("rest_origin")) {
                if (ro->kind == JsonValue::Kind::Null) {
                    g.rest_origin.reset();
                } else {
                    PointI pt{};
                    if (!ReadPointObj(*ro, pt)) {
                        return false;
                    }
                    g.rest_origin = pt;
                }
            }

            if (const JsonValue* rv = gv.Find("rule")) {
                if (rv->kind != JsonValue::Kind::Object) {
                    return false;
                }
                if (!ReadWideArrayField(*rv, "extensions", g.rule.extensions) ||
                    !ReadWideArrayField(*rv, "name_prefixes", g.rule.name_prefixes) ||
                    !ReadWideArrayField(*rv, "name_contains", g.rule.name_contains) ||
                    !ReadWideField(*rv, "tag_id", g.rule.tag_id) ||
                    !ReadInt32Field(*rv, "modified_within_days", g.rule.modified_within_days) ||
                    !ReadBoolField(*rv, "match_folders", g.rule.match_folders) ||
                    !ReadBoolField(*rv, "match_files", g.rule.match_files) ||
                    !ReadBoolField(*rv, "fallback_catch_all", g.rule.fallback_catch_all)) {
                    return false;
                }
            }

            parsed.groups.push_back(std::move(g));
        }
    }

    NormalizeGroupTabs(parsed.groups);
    if (const JsonValue* tiles = root.Find("tiles")) {
        if (tiles->kind != JsonValue::Kind::Array) {
            return false;
        }
        parsed.tiles.reserve(tiles->array_vals.size());
        for (const JsonValue& tv : tiles->array_vals) {
            if (tv.kind != JsonValue::Kind::Object) {
                return false;
            }
            DesktopTile t{};
            if (!ReadUtf8Field(tv, "id", t.id) ||
                !ReadEnumField(tv, "kind", static_cast<uint8_t>(kLastTileKind), t.kind) ||
                !ReadWideField(tv, "title", t.title) ||
                !ReadWideField(tv, "target_path", t.target_path) ||
                !ReadWideField(tv, "note_body", t.note_body) ||
                !ReadBoolField(tv, "analog_clock", t.analog_clock) ||
                !ReadBoolField(tv, "live_content", t.live_content) ||
                !ReadMaterialField(tv, "material", CardMaterial::FollowHost, t.material) ||
                !ReadUint32Field(tv, "accent_argb", t.accent_argb) ||
                !ReadIntField(tv, "col", t.col) ||
                !ReadIntField(tv, "row", t.row)) {
                return false;
            }
            (void)ReadWideField(tv, "countdown_date", t.countdown_date);
            if (const JsonValue* sz = tv.Find("size")) {
                if (sz->kind != JsonValue::Kind::Object ||
                    !ReadIntField(*sz, "cols", t.size.cols) ||
                    !ReadIntField(*sz, "rows", t.size.rows)) {
                    return false;
                }
            }
            if (const JsonValue* lk = tv.Find("look")) {
                if (lk->kind != JsonValue::Kind::Object ||
                    !ReadEnumField(*lk, "background", static_cast<uint8_t>(TileBackground::Image), t.look.background) ||
                    !ReadUint32Field(*lk, "color_argb", t.look.color_argb) ||
                    !ReadWideField(*lk, "image_path", t.look.image_path) ||
                    !ReadWideField(*lk, "title_override", t.look.title_override) ||
                    !ReadBoolField(*lk, "show_title", t.look.show_title)) {
                    return false;
                }
            }
            if (!ReadIntField(tv, "photo_interval_s", t.photo_interval_s) || !ReadBoolField(tv, "photo_shuffle", t.photo_shuffle) ||
                !ReadIntField(tv, "note_color", t.note_color) ||
                !ReadBoolField(tv, "free_placed", t.free_placed) ||
                !ReadIntField(tv, "x", t.x) ||
                !ReadIntField(tv, "y", t.y) ||
                !ReadIntField(tv, "w", t.w) ||
                !ReadIntField(tv, "h", t.h)) {
                return false;
            }
            if (const JsonValue* tro = tv.Find("rest_origin")) {
                if (tro->kind == JsonValue::Kind::Null) {
                    t.rest_origin.reset();
                } else {
                    PointI pt{};
                    if (!ReadPointObj(*tro, pt)) {
                        return false;
                    }
                    t.rest_origin = pt;
                }
            }
            if (t.w > 0) t.w = std::clamp(t.w, kTileMinW, 8192);
            if (t.h > 0) t.h = std::clamp(t.h, kTileMinH, 8192);
            if (t.w > 0 || t.h > 0) {
                t.size = EffectiveTileUnits(
                    t.w > 0 ? t.w : DefaultTileSpanPx(t.size.cols),
                    t.h > 0 ? t.h : DefaultTileSpanPx(t.size.rows));
            }
            t.photo_interval_s = std::clamp(t.photo_interval_s, 0, 24 * 3600);
            if (!ReadIntField(tv, "stack_index", t.stack_index) || !ReadBoolField(tv, "smart_rotate", t.smart_rotate)) {
                return false;
            }
            if (const JsonValue* st = tv.Find("stack")) {
                if (st->kind != JsonValue::Kind::Array) return false;
                for (const JsonValue& mv : st->array_vals) {
                    TileStackEntry m{};
                    if (mv.kind != JsonValue::Kind::Object ||
                        !ReadEnumField(mv, "kind", static_cast<uint8_t>(kLastTileKind), m.kind) ||
                        !ReadWideField(mv, "title", m.title) ||
                        !ReadWideField(mv, "target_path", m.target_path) ||
                        !ReadUtf8Field(mv, "origin_id", m.origin_id) ||
                        !ReadBoolField(mv, "analog_clock", m.analog_clock)) {
                        return false;
                    }
                    t.stack.push_back(std::move(m));
                }
                if (t.stack.size() == 1) t.stack.clear();
                t.stack_index = t.stack.empty() ? 0 : std::clamp(t.stack_index, 0, static_cast<int>(t.stack.size()) - 1);
            }
            parsed.tiles.push_back(std::move(t));
        }
    }

    if (!ReadBoolField(root, "widget_defaults_applied", parsed.widget_defaults_applied)) return false;
    if (const JsonValue* todos = root.Find("todos")) {
        if (todos->kind != JsonValue::Kind::Array) return false;
        for (const JsonValue& tdv : todos->array_vals) {
            TodoItem td{};
            if (tdv.kind != JsonValue::Kind::Object || !ReadWideField(tdv, "text", td.text) || !ReadBoolField(tdv, "done", td.done))
                return false;
            parsed.todos.push_back(std::move(td));
        }
    }

    if (const JsonValue* vw = root.Find("video_wallpaper")) {
        if (vw->kind != JsonValue::Kind::Object) return false;
        auto& dst = parsed.video_wallpaper;
        if (!ReadBoolField(*vw, "active", dst.active) ||
            !ReadWideField(*vw, "source_path", dst.source_path) ||
            !ReadEnumField(*vw, "fill_mode", static_cast<uint8_t>(video::VideoFillMode::Fit), dst.fill_mode) ||
            !ReadWideField(*vw, "cache_key", dst.cache_key) ||
            !ReadWideField(*vw, "cached_video_path", dst.cached_video_path) ||
            !ReadWideField(*vw, "poster_path", dst.poster_path) ||
            !ReadUint32Field(*vw, "source_width", dst.source_width) ||
            !ReadUint32Field(*vw, "source_height", dst.source_height) ||
            !ReadUint32Field(*vw, "target_width", dst.target_width) ||
            !ReadUint32Field(*vw, "target_height", dst.target_height) ||
            !ReadUint32Field(*vw, "duration_ms", dst.duration_ms) ||
            !ReadBoolField(*vw, "optimized", dst.optimized)) {
            return false;
        }
        if (const JsonValue* rec = vw->Find("recent")) {
            if (rec->kind != JsonValue::Kind::Array) return false;
            dst.recent.reserve(std::min<size_t>(rec->array_vals.size(), 8u));
            for (const JsonValue& rv : rec->array_vals) {
                if (rv.kind != JsonValue::Kind::Object) return false;
                video::VideoWallpaperRecentItem item{};
                if (!ReadWideField(rv, "path", item.path) ||
                    !ReadWideField(rv, "poster_path", item.poster_path) ||
                    !ReadUint32Field(rv, "width", item.width) ||
                    !ReadUint32Field(rv, "height", item.height) ||
                    !ReadUint32Field(rv, "duration_ms", item.duration_ms)) {
                    return false;
                }
                if (!item.path.empty()) {
                    dst.recent.push_back(std::move(item));
                    if (dst.recent.size() >= 8u) break;
                }
            }
        }
    }

    out_state = std::move(parsed);
    return true;
}

} // namespace pulse::desktop::model

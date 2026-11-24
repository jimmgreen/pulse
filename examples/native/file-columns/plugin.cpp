// File columns: extra Details columns and icon badges with pulse.columns.
//   1. "Lines"    - line count of text files (right-aligned, numbers only),
//   2. "Encoding" - UTF-8 / UTF-8 BOM / UTF-16 / ASCII / binary, with tones,
//   3. a badge    - a sparkle on files changed in the last 24 hours,
//   4. a command  - "Recount lines" asks again for the selected files.
// Callbacks run on a host worker thread, a few rows at a time, only for rows on
// screen; Pulse caches the answers per file version. Keep them fast, never touch
// the UI from them, and return std::nullopt to leave a cell blank.
// The user hides or shows each column from the Details header menu and can
// resize it; columns are display-only (no sorting).
#include <pulse/app.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>

using namespace pulse::sdk;

namespace {
constexpr const char* kTextTypes = ".txt;.md;.log;.csv;.json;.xml;.yml;.yaml;.ini;.toml;.c;.h;.cpp;.hpp;.cs;.py;.js;.ts;.css;.html";
constexpr size_t kMaxRead = 8u << 20;   // larger files stay blank

// Reads up to `limit` bytes of a UTF-8 path. Empty on failure.
std::string ReadHead(std::string_view path, size_t limit) {
    std::ifstream file(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    std::string out(limit, '\0');
    if (!file) return {};
    file.read(out.data(), static_cast<std::streamsize>(limit));
    out.resize(static_cast<size_t>(file.gcount()));
    return out;
}

std::string Encoding(const std::string& head, uint32_t& tone) {
    tone = PULSE_COLUMN_TONE_DEFAULT;
    if (head.size() >= 3 && std::memcmp(head.data(), "\xEF\xBB\xBF", 3) == 0) return "UTF-8 BOM";
    if (head.size() >= 2 && (std::memcmp(head.data(), "\xFF\xFE", 2) == 0 || std::memcmp(head.data(), "\xFE\xFF", 2) == 0)) {
        tone = PULSE_COLUMN_TONE_ACCENT;
        return "UTF-16";
    }
    bool ascii = true;
    for (const unsigned char c : head) {
        if (c == 0) { tone = PULSE_COLUMN_TONE_MUTED; return "binary"; }
        if (c >= 0x80) ascii = false;
    }
    return ascii ? "ASCII" : "UTF-8";
}
}  // namespace

class FileColumns : public App {
public:
    using App::App;

    LocalText Name() const override { return Tr("文件列示例", "檔案欄範例", "File columns example"); }

    PulseStatus OnStart() override {
        if (!Columns()) return PULSE_OK;   // older Pulse: nothing to add, still loads

        // 1. Line count, right-aligned. `extensions` limits the files asked about.
        ColumnSpec lines;
        lines.id = "lines";
        lines.title = Text(kLinesTitle);
        lines.extensions = kTextTypes;
        lines.width_dip = 80;
        lines.align = PULSE_COLUMN_ALIGN_END;
        lines.applies_to = PULSE_COLUMN_APPLIES_FILES;
        AddColumn(lines, [](const ColumnItem& item) -> std::optional<ColumnText> {
            if (item.size_bytes > kMaxRead) return std::nullopt;
            if (item.size_bytes == 0) return ColumnText{"0"};
            const std::string text = ReadHead(item.path, static_cast<size_t>(item.size_bytes));
            if (text.empty()) return std::nullopt;
            size_t count = 1;
            for (const char c : text) count += c == '\n';
            if (text.back() == '\n') --count;
            return ColumnText{std::to_string(count)};
        });

        // 2. Text encoding from the first 4 KB, with a tone per result.
        ColumnSpec encoding;
        encoding.id = "encoding";
        encoding.title = Text(kEncodingTitle);
        encoding.width_dip = 96;
        encoding.applies_to = PULSE_COLUMN_APPLIES_FILES;
        AddColumn(encoding, [](const ColumnItem& item) -> std::optional<ColumnText> {
            if (item.size_bytes == 0) return std::nullopt;
            const std::string head = ReadHead(item.path, 4096);
            if (head.empty()) return std::nullopt;
            ColumnText cell;
            cell.text = Encoding(head, cell.tone);
            return cell;
        });

        // 3. Badge: built-in icon names ("sparkle", "lock", "star", "warn"...),
        //    an icon this package registered, or "<package>/<name>".
        ColumnSpec recent;
        recent.id = "recent";
        recent.title = Text(kRecentTitle);
        AddBadge(recent, [](const ColumnItem& item) -> std::optional<BadgeIcon> {
            using namespace std::chrono;
            const int64_t now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            if (item.modified_unix_ms <= 0 || now - item.modified_unix_ms > 24LL * 3600 * 1000) return std::nullopt;
            return BadgeIcon{"sparkle", PULSE_COLUMN_TONE_ACCENT};
        });

        // 4. Values are cached per file version; ask again on demand.
        Command("recount", Tr("重新统计行数", "重新統計行數", "Recount lines"))
            .Icon("refresh")
            .Files(1, 4096)
            .Group(Name())
            .Run([this](const Selection& selection) {
                for (const auto& path : selection.Paths()) RefreshColumns("lines", path);
            });
        return PULSE_OK;
    }

    void OnLanguageChanged() override {
        SetColumnTitle("lines", Text(kLinesTitle));
        SetColumnTitle("encoding", Text(kEncodingTitle));
        SetColumnTitle("recent", Text(kRecentTitle));
    }

private:
    const LocalText kLinesTitle = Tr("行数", "行數", "Lines");
    const LocalText kEncodingTitle = Tr("编码", "編碼", "Encoding");
    const LocalText kRecentTitle = Tr("最近修改", "最近修改", "Changed today");
};

PULSE_APP(FileColumns)

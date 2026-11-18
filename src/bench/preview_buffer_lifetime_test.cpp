#include "../ui/archive_preview.h"
#include "../ui/markdown_view.h"
#include "../ui/table_view.h"
#include "../ui/tree_view.h"
#include <cstdio>

namespace pulse::ui {
struct PreviewBufferLifetimeTest {
    static size_t Bytes(const std::wstring& text) {
        return text.capacity() > std::wstring{}.capacity() ? (text.capacity() + 1) * sizeof(wchar_t) : 0;
    }
    template<class T> static size_t Bytes(const std::vector<T>& rows) { return rows.capacity() * sizeof(T); }
    static size_t Retained(const ArchivePreview& v) {
        return Bytes(v.payload_) + Bytes(v.nodes_) + Bytes(v.roots_) + Bytes(v.visible_);
    }
    static size_t Retained(const MarkdownView& v) {
        return Bytes(v.payload_) + Bytes(v.source_) + Bytes(v.plain_) + Bytes(v.blocks_) + Bytes(v.tables_) +
            Bytes(v.all_blocks_) + Bytes(v.all_tables_) + Bytes(v.sections_) + Bytes(v.toc_);
    }
    static size_t Retained(const TableView& v) {
        return Bytes(v.payload_) + Bytes(v.source_) + Bytes(v.plain_) + Bytes(v.sheets_) + Bytes(v.row_offsets_);
    }
    static size_t Retained(const TreeView& v) {
        return Bytes(v.payload_) + Bytes(v.source_) + Bytes(v.plain_) + Bytes(v.nodes_) + Bytes(v.expanded_) + Bytes(v.rows_);
    }
    template<class V, class Load, class Valid>
    static int Check(V& view, const char* name, Load load, Valid valid) {
        const bool loaded = load() && valid();
        const size_t before = Retained(view);
        view.Clear();
        const size_t after = Retained(view);
        const bool released = before > 100000 && after == 0;
        const bool reopened = load() && valid() && Retained(view) >= before;
        view.Clear();
        view.Clear();
        const bool repeated = Retained(view) == 0;
        std::printf("[%s] %s load/clear/reopen/repeated-clear allocated_capacity_bytes=%zu after_clear=%zu\n",
            loaded && released && reopened && repeated ? "PASS" : "FAIL", name, before, after);
        return loaded && released && reopened && repeated ? 0 : 1;
    }
    static int Run() {
        int failures = 0;
        ArchivePreview archive;
        std::wstring archive_payload = L"PULSEARC\t1\tZIP\t0\t0\n";
        for (int i = 0; i < 20000; ++i)
            archive_payload += L"0\tf\t0\t0\t-\tf" + std::to_wstring(i) + L".txt\n";
        failures += Check(archive, "archive 20000 entries", [&] { return archive.SetPayload(archive_payload, L"fixture.zip", 0); },
            [&] { return archive.nodes_.size() == 20000 && archive.nodes_.back().name == L"f19999.txt"; });
        MarkdownView markdown;
        std::wstring md = L"PULSEMD\t1\nS\t" + std::wstring(64000, L's') + L"\n";
        for (int i = 0; i < 10000; ++i) md += L"B\tp\t\t0\t0\t\tparagraph\t\n";
        failures += Check(markdown, "markdown 10000 paragraphs", [&] { return markdown.SetPayload(md, L"fixture.md"); },
            [&] { return markdown.HasData() && markdown.PlainText().find(L"paragraph") != std::wstring::npos && markdown.Source().size() == 64000; });
        TableView table;
        std::wstring csv = L"PULSETBL\t1\nS\tcsv\tCSV\t20\t5000\t0\tcomma\t0\nX\t" + std::wstring(64000, L's') + L"\n";
        for (int row = 0; row < 5000; ++row) {
            csv += L"R";
            for (int col = 0; col < 20; ++col) csv += L"\tvalue123";
            csv += L'\n';
        }
        failures += Check(table, "table 5000x20 cells", [&] { return table.SetPayload(csv); },
            [&] { return table.HasData() && table.sheets_[0].cells.size() == 5000 && table.PlainText().find(L"value123") != std::wstring::npos; });
        TreeView tree;
        std::wstring json = L"PULSETREE\t1\nH\tjson\t0\t0\t\t0\t0\nN\t0\ta\t\t\t10000\t\nX\t" + std::wstring(64000, L's') + L"\n";
        for (int i = 0; i < 10000; ++i) json += L"N\t1\ts\t\tvalue123\t0\t\n";
        failures += Check(tree, "tree 10000 values", [&] { return tree.SetPayload(json); },
            [&] { return tree.HasData() && tree.nodes_.size() == 10001 && tree.Source().size() == 64000; });
        return failures ? 1 : 0;
    }
};
}
int main() { return pulse::ui::PreviewBufferLifetimeTest::Run(); }

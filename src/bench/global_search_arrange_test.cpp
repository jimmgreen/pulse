// global_search_arrange_test.cpp - quick search sort and group order (pure model).
#include "../app/global_search_arrange.h"
#include "../app/entry_group.h"
#include "../common/string_ids.h"

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace pulse;
using app::GlobalSearchArrangeItem;
using app::GlobalSearchGroup;
using app::GlobalSearchRowClass;
using app::GlobalSearchSort;

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

uint64_t Now() {
    FILETIME time{}; GetSystemTimeAsFileTime(&time);
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
constexpr uint64_t kDay = 864000000000ull;
constexpr uint64_t MB = 1024ull * 1024ull;

struct Fixture {
    std::vector<std::wstring> names, paths;
    std::vector<GlobalSearchArrangeItem> items;
    void Add(const wchar_t* path, bool directory, uint64_t size, uint64_t mtime, GlobalSearchRowClass cls) {
        paths.emplace_back(path);
        const auto slash = paths.back().find_last_of(L'\\');
        names.push_back(paths.back().substr(slash + 1));
        items.push_back({});
        auto& item = items.back();
        item.directory = directory; item.size = size; item.mtime = mtime; item.cls = cls;
    }
    // Views into the strings once every row is in (the vectors no longer move).
    const std::vector<GlobalSearchArrangeItem>& Items() {
        for (size_t i = 0; i < items.size(); ++i) { items[i].name = names[i]; items[i].path = paths[i]; }
        return items;
    }
};

std::wstring Order(const Fixture& f, const std::vector<app::GlobalSearchSpan>& spans) {
    std::wstring out;
    for (const auto& span : spans) {
        out += L"[";
        for (const size_t i : span.items) { if (out.back() != L'[') out += L","; out += f.names[i]; }
        out += L"]";
    }
    return out;
}

Fixture Sample() {
    const uint64_t now = Now();
    Fixture f;
    f.Add(L"D:\\proj\\report", true, 0, now - 2 * kDay, GlobalSearchRowClass::Jump);
    f.Add(L"D:\\proj\\report 2026.docx", false, 3 * MB, now - kDay / 4, GlobalSearchRowClass::Best);
    f.Add(L"C:\\Apps\\Report Viewer", false, 0, 0, GlobalSearchRowClass::App);
    f.Add(L"D:\\work\\reports", true, 0, now - 40 * kDay, GlobalSearchRowClass::Folder);
    f.Add(L"D:\\work\\report10.pdf", false, 900 * 1024, now - 3 * kDay, GlobalSearchRowClass::File);
    f.Add(L"D:\\work\\report2.pdf", false, 2048 * MB, now - 400 * kDay, GlobalSearchRowClass::File);
    f.Add(L"D:\\pics\\report.png", false, 5 * MB, now - kDay / 8, GlobalSearchRowClass::File);
    return f;
}
} // namespace

int main() {
    {
        auto f = Sample();
        app::GlobalSearchArrangeOptions options;
        const auto spans = app::ArrangeGlobalSearch(f.Items(), options);
        Check(Order(f, spans) == L"[report][report 2026.docx][Report Viewer][reports][report10.pdf,report2.pdf,report.png]",
              "match grouping keeps the merged class order");
        Check(spans.size() == 5 && spans[2].app && !spans[0].foldable, "match groups are titled, not foldable");
        options.apps_first = true;
        const auto first = app::ArrangeGlobalSearch(f.Items(), options);
        Check(!first.empty() && first.front().app && first.front().title == l10n::StringId::GlobalSearchGroupApps,
              "apps first moves the apps group above go-to and best match");
    }
    {
        auto f = Sample();
        app::GlobalSearchArrangeOptions options;
        options.sort = GlobalSearchSort::Size; options.descending = true;
        const auto spans = app::ArrangeGlobalSearch(f.Items(), options);
        Check(spans.size() == 1 && !spans[0].header, "a non-relevance order with match grouping is one flat list");
        Check(Order(f, spans) == L"[report,reports,report2.pdf,report.png,report 2026.docx,report10.pdf,Report Viewer]",
              "size, largest first, folders on top, apps at the end");
        options.folders = app::GlobalSearchFolders::Bottom; options.apps_first = true;
        Check(Order(f, app::ArrangeGlobalSearch(f.Items(), options)) ==
              L"[Report Viewer,report2.pdf,report.png,report 2026.docx,report10.pdf,report,reports]",
              "folders at the bottom, apps first");
    }
    {
        auto f = Sample();
        app::GlobalSearchArrangeOptions options;
        options.sort = GlobalSearchSort::Name; options.group = GlobalSearchGroup::None; options.folders = app::GlobalSearchFolders::Mixed;
        const auto order = Order(f, app::ArrangeGlobalSearch(f.Items(), options));
        Check(order.find(L"report2.pdf") < order.find(L"report10.pdf"), "names compare numbers by value (report2 before report10)");
        Check(order.rfind(L"Report Viewer]") + 14 == order.size(), "apps stay after the sorted files when not first");
    }
    {
        auto f = Sample();
        app::GlobalSearchArrangeOptions options;
        options.group = GlobalSearchGroup::Type;
        const auto spans = app::ArrangeGlobalSearch(f.Items(), options);
        Check(Order(f, spans) == L"[report,reports][report 2026.docx,report10.pdf,report2.pdf][report.png][Report Viewer]",
              "type groups: folders, documents, images, then apps; relevance inside");
        Check(spans.size() == 4 && spans[0].kind == index::SearchKind::Folder && spans[1].kind == index::SearchKind::Document &&
              spans[0].foldable && spans[0].key == L"t:folder", "type groups carry kind, key and fold");
    }
    {
        auto f = Sample();
        app::GlobalSearchArrangeOptions options;
        options.group = GlobalSearchGroup::Date; options.sort = GlobalSearchSort::Modified; options.descending = true;
        const auto newest = app::ArrangeGlobalSearch(f.Items(), options);
        const auto clock = app::MakeGroupClock();
        options.descending = false;
        const auto oldest = app::ArrangeGlobalSearch(f.Items(), options, &clock);
        Check(!newest.empty() && newest.front().title == static_cast<l10n::StringId>(IDS_GROUP_D_FUTURE + 1),
              "date groups start with today when newest first");
        Check(!oldest.empty() && oldest.front().title == static_cast<l10n::StringId>(IDS_GROUP_D_FUTURE + 8) &&
              f.names[oldest.front().items.front()] == L"report2.pdf", "oldest first turns the date groups around");
    }
    {
        Fixture f;
        f.Add(L"D:\\a\\one.txt", false, 1, 1, GlobalSearchRowClass::File);
        f.Add(L"D:\\a\\two.txt", false, 1, 1, GlobalSearchRowClass::File);
        f.Add(L"D:\\b\\three.txt", false, 1, 1, GlobalSearchRowClass::File);
        f.Add(L"E:\\c\\four.txt", false, 1, 1, GlobalSearchRowClass::File);
        f.Add(L"\\\\?\\D:\\A\\five.txt", false, 1, 1, GlobalSearchRowClass::File);
        app::GlobalSearchArrangeOptions options;
        options.group = GlobalSearchGroup::Folder;
        const auto spans = app::ArrangeGlobalSearch(f.Items(), options);
        Check(Order(f, spans) == L"[one.txt,two.txt,five.txt][three.txt,four.txt]",
              "folder groups: same folder together (case and \\\\?\\ ignored), single results share Other");
        Check(spans.size() == 2 && spans[0].label == L"a" && spans[0].detail == L"D:\\" &&
              spans[1].title == l10n::StringId::GroupNameOther, "folder group names the folder and where it is");
    }
    {
        Fixture f;
        f.Add(L"C:\\x\\only.txt", false, 1, 1, GlobalSearchRowClass::Content);
        const auto spans = app::ArrangeGlobalSearch(f.Items(), {});
        Check(spans.size() == 1 && spans[0].title == l10n::StringId::GlobalSearchContentMatches, "content results keep their group");
        Check(app::ArrangeGlobalSearch({}, {}).empty(), "no results, no groups");
    }
    Check(app::GlobalSearchKindOf(L"Report.PDF", false) == index::SearchKind::Document, "kind by extension, any case");
    Check(app::GlobalSearchKindOf(L"photo.jpg", false) == index::SearchKind::Image, "images");
    Check(app::GlobalSearchKindOf(L"stuff", true) == index::SearchKind::Folder, "folders");
    Check(app::GlobalSearchKindOf(L"data.zzqq", false) == index::SearchKind::Any, "unknown extension");
    Check(app::GlobalSearchExtensionOf(L".gitignore").empty() && app::GlobalSearchExtensionOf(L"a.TAR.GZ") == L"gz",
          "extension: none for dot files, last part lower-case");
    Check(app::GlobalSearchSortStartsDescending(GlobalSearchSort::Size) && !app::GlobalSearchSortStartsDescending(GlobalSearchSort::Name),
          "size and date start descending, names ascending");
    Check(app::GlobalSearchSortFromInt(9) == GlobalSearchSort::Relevance && app::GlobalSearchGroupFromInt(-1) == GlobalSearchGroup::Match,
          "saved values out of range fall back to the defaults");
    std::printf("[RESULT] failures=%d\n", failures);
    return failures ? 1 : 0;
}

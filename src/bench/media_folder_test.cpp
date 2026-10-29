// 媒体文件夹专用列: classification and value text (media_folder.h), and the
// media groups (entry_group.h) with a stand-in value source.
#include "../app/media_folder.h"
#include "../app/entry_group.h"
#include "../app/entry_sort.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace pulse::app::media;

namespace {
int failures = 0, passes = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", what);
    (ok ? passes : failures)++;
}
Tally Make(int photo, int music, int video, int other, int dirs = 0) {
    Tally t;
    for (int i = 0; i < photo; ++i) t.Add(L"IMG_0001.JPG", false);
    for (int i = 0; i < music; ++i) t.Add(L"song.flac", false);
    for (int i = 0; i < video; ++i) t.Add(L"clip.mp4", false);
    for (int i = 0; i < other; ++i) t.Add(L"notes.txt", false);
    for (int i = 0; i < dirs; ++i) t.Add(L"album.jpg", true);
    return t;
}

pulse::fs::DirEntry File(const wchar_t* name, uint64_t size = 100) {
    pulse::fs::DirEntry e;
    e.name = name;
    e.size = size;
    e.mtime.dwLowDateTime = 7;
    return e;
}

// Two months (labels in group order: newest first); old.jpg was read in an
// older version, so it counts as unknown like scan.png.
std::shared_ptr<const pulse::app::MediaGroupMap> FakeGroups(const std::wstring& folder, pulse::app::GroupBy) {
    if (folder != L"C:\\Pics") return nullptr;
    auto map = std::make_shared<pulse::app::MediaGroupMap>();
    map->labels = {L"May 2026", L"Dec 2025"};
    map->by_name[L"may_a.jpg"] = {7, 100, 0};
    map->by_name[L"may_b.jpg"] = {7, 100, 0};
    map->by_name[L"dec.jpg"] = {7, 100, 1};
    map->by_name[L"old.jpg"] = {7, 999, 1};
    return map;
}

std::wstring Order(const std::vector<pulse::fs::DirEntry>& v) {
    std::wstring out;
    for (const auto& e : v) out += e.name + L" ";
    return out;
}

void GroupTests() {
    using namespace pulse::app;
    Check(GroupByFromInt(14) == GroupBy::MediaTier && GroupByFromInt(15) == GroupBy::None, "media group ids");
    Check(IsMediaGroup(GroupBy::MediaMonth) && !IsMediaGroup(GroupBy::AiDup), "IsMediaGroup");
    SetMediaGroupSource(&FakeGroups);
    std::vector<pulse::fs::DirEntry> files = {File(L"scan.png"), File(L"dec.jpg"), File(L"may_b.jpg"),
                                              File(L"old.jpg"), File(L"may_a.jpg")};
    pulse::fs::DirEntry dir = File(L"edits", 0);
    dir.is_dir = true;
    dir.attrs = FILE_ATTRIBUTE_DIRECTORY;
    files.push_back(dir);
    {
        const ScopedEntryGrouping grouping(static_cast<int>(GroupBy::MediaMonth), L"C:\\Pics");
        auto v = files;
        SortEntries(v, pulse::ui::SortColumn::Name, pulse::ui::SortDirection::Asc);
        Check(Order(v) == L"edits may_a.jpg may_b.jpg dec.jpg old.jpg scan.png ",
              "folders, newest month, older month, unknown last (stale version unknown)");
        v = files;
        SortEntries(v, pulse::ui::SortColumn::MediaTaken, pulse::ui::SortDirection::Asc);
        Check(v.size() == 6 && v[0].name == L"edits" && v[1].name == L"dec.jpg" &&
              (v[4].name == L"old.jpg" || v[4].name == L"scan.png"),
              "oldest first by date taken flips the months, unknown stays last");
        const GroupClock& clock = CurrentEntryGrouping()->clock;
        Check(GroupKey(File(L"dec.jpg"), GroupBy::MediaMonth, clock) == L"m10:dec 2025" &&
              GroupKey(File(L"scan.png"), GroupBy::MediaMonth, clock) == L"m10:?" &&
              GroupKey(dir, GroupBy::MediaMonth, clock) == L"m10:/", "media group keys by title");
        Check(MediaGroupLabel(clock, 0) == L"May 2026" && MediaGroupLabel(clock, 2).empty() &&
              MediaGroupLabel(clock, -1).empty(), "media group labels");
    }
    {
        const ScopedEntryGrouping grouping(static_cast<int>(GroupBy::MediaMonth), L"D:\\Other");
        auto v = files;
        SortEntries(v, pulse::ui::SortColumn::Name, pulse::ui::SortDirection::Asc);
        Check(Order(v) == L"edits dec.jpg may_a.jpg may_b.jpg old.jpg scan.png ",
              "no values: one group after the folders, by name");
    }
    SetMediaGroupSource(nullptr);

    FolderGroupPrefs prefs;
    prefs.Set(L"C:\\Pics", GroupBy::Name);
    std::wstring before;
    prefs.AppendJson(before);
    Check(!prefs.Set(L"C:\\Pics", GroupBy::MediaCamera), "a media group asks for no save");
    Check(prefs.Resolve(L"C:\\Pics") == GroupBy::MediaCamera, "the media group holds for the session");
    std::wstring after;
    prefs.AppendJson(after);
    Check(before == after, "a media group is never written; the saved choice stays");
    prefs.Set(L"C:\\Pics", GroupBy::Name);
    Check(prefs.Resolve(L"C:\\Pics") == GroupBy::Name, "choosing a plain group again drops the session one");
}
}  // namespace

int main() {
    Check(KindOfName(L"IMG_0001.JPG") == Kind::Photo, "extension case does not matter");
    Check(KindOfName(L"a.heic") == Kind::Photo && KindOfName(L"b.CR3") == Kind::Photo, "heic and raw are photos");
    Check(KindOfName(L"a.flac") == Kind::Music && KindOfName(L"a.m4a") == Kind::Music, "music extensions");
    Check(KindOfName(L"a.mkv") == Kind::Video && KindOfName(L"a.MOV") == Kind::Video, "video extensions");
    Check(KindOfName(L".jpg") == Kind::None && KindOfName(L"jpg") == Kind::None && KindOfName(L"a.") == Kind::None,
          "dot files, bare names and trailing dots are not media");
    Check(KindOfName(L"archive.jpg.zip") == Kind::None, "only the last extension counts");

    Check(Classify(Make(4, 0, 0, 0)) == Kind::None, "four photos are too few");
    Check(Classify(Make(5, 0, 0, 0)) == Kind::Photo, "five photos make a photo folder");
    Check(Classify(Make(6, 0, 0, 4)) == Kind::Photo, "60% photos is enough");
    Check(Classify(Make(5, 0, 0, 4)) == Kind::None, "55% photos is not");
    Check(Classify(Make(0, 12, 0, 3)) == Kind::Music, "a music folder with cover and cue files");
    Check(Classify(Make(0, 0, 7, 2)) == Kind::Video, "a video folder");
    Check(Classify(Make(4, 4, 4, 0)) == Kind::None, "a mix with no majority");
    Check(Classify(Make(5, 0, 0, 0, 30)) == Kind::Photo, "subfolders count neither way");
    Check(Classify(Make(0, 0, 0, 0, 30)) == Kind::None, "only folders");

    Check(FormatDuration(0).empty(), "unknown duration is empty");
    Check(FormatDuration(1'870'000'000ull) == L"3:07", "3 min 7 s");
    Check(FormatDuration(37'230'000'000ull) == L"1:02:03", "over an hour shows hours");
    Check(FormatDuration(4'000'000ull) == L"0:00" && FormatDuration(6'000'000ull) == L"0:01", "rounds to the nearest second");
    Check(FormatDimensions(4000, 3000) == L"4000 \x00D7 3000", "dimensions");
    Check(FormatDimensions(0, 3000).empty(), "unknown dimensions are empty");
    Check(FormatFrameRate(30000) == L"30" && FormatFrameRate(25000) == L"25", "whole frame rates");
    Check(FormatFrameRate(29970) == L"29.97" && FormatFrameRate(23976) == L"23.98", "NTSC frame rates");
    Check(FormatFrameRate(59940) == L"59.94" && FormatFrameRate(12500) == L"12.5", "trailing zero dropped");
    Check(FormatFrameRate(0).empty(), "unknown frame rate is empty");

    Check(TierOf(3840, 2160) == Tier::Uhd && TierOf(4096, 2160) == Tier::Uhd, "4K");
    Check(TierOf(3840, 1600) == Tier::Uhd, "ultrawide 4K by the long edge");
    Check(TierOf(1920, 1080) == Tier::FullHd && TierOf(1080, 1920) == Tier::FullHd, "1080p either way up");
    Check(TierOf(1280, 720) == Tier::Hd && TierOf(640, 360) == Tier::Sd, "720p and below");
    Check(TierOf(0, 0) == Tier::Unknown, "unknown resolution");

    bool unknown = false;
    Check(CompareKnownFirst(true, 3, true, 5, unknown) < 0 && !unknown, "known values compare");
    Check(CompareKnownFirst(false, 0, true, 5, unknown) > 0 && unknown, "unknown goes after a known value");
    Check(CompareKnownFirst(true, 5, false, 0, unknown) < 0 && unknown, "known goes before an unknown value");
    Check(CompareKnownFirst(false, 0, false, 0, unknown) == 0 && unknown, "two unknowns tie");

    GroupTests();

    std::printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}

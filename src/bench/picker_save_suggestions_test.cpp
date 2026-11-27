#include "../ui/picker_save_suggestions.h"
#include <cstdio>
#include <string>
#include <vector>

namespace {
using pulse::ui::BuildSaveNameSuggestions;
using pulse::ui::SaveNameSuggestionKind;
int checks = 0, failures = 0;
void Check(bool okay, const char* name) {
    ++checks;
    if (!okay) ++failures;
    std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", name);
}
std::wstring Find(std::wstring_view filename, SaveNameSuggestionKind kind,
    std::wstring_view original = {}, std::wstring_view date = L"2026-11-25") {
    for (const auto& item : BuildSaveNameSuggestions(filename, original, date))
        if (item.kind == kind) return item.filename;
    return {};
}
}

int main() {
    using Kind = SaveNameSuggestionKind;
    Check(Find(L"report_v009.txt", Kind::Version) == L"report_v010.txt", "version increments while retaining leading zeros");
    Check(Find(L"report-V099.TXT", Kind::Version) == L"report-V100.TXT", "version retains delimiter marker case and extension case");
    Check(Find(L"report v9", Kind::Version) == L"report v10", "extensionless space-delimited version stays extensionless");
    Check(Find(L"v000", Kind::Version) == L"v001", "bare version stem increments with fixed zero padding");
    Check(Find(L"quarterly.report.v009.docx", Kind::Version) == L"quarterly.report.v010.docx", "multi-dot stem changes only terminal version");
    Check(Find(L"C:\\文档\\季度报告_V09.PDF", Kind::Version) == L"C:\\文档\\季度报告_V10.PDF", "Unicode absolute path is preserved");
    Check(Find(L"\\\\server\\share\\项目-v099.xlsx", Kind::Version) == L"\\\\server\\share\\项目-v100.xlsx", "UNC prefix is preserved");
    Check(Find(L"C:/folder/report_v1.txt", Kind::Version) == L"C:/folder/report_v2.txt", "forward slash prefix is not rewritten");
    Check(Find(L"..\\drafts\\report_v1.txt", Kind::Version) == L"..\\drafts\\report_v2.txt", "relative path prefix is not resolved");
    Check(Find(L"\\\\?\\C:\\文档\\report_v1.txt", Kind::Version) == L"\\\\?\\C:\\文档\\report_v2.txt", "extended drive prefix is preserved");
    Check(Find(L"\\\\?\\UNC\\server\\share\\report_v1.txt", Kind::Version) == L"\\\\?\\UNC\\server\\share\\report_v2.txt", "extended UNC prefix is preserved");
    const auto nines = std::wstring(2048, L'9');
    Check(Find(L"report_v" + nines + L".txt", Kind::Version) == L"report_v1" + std::wstring(2048, L'0') + L".txt",
        "arbitrarily long all-nine version carries without integer overflow");
    Check(Find(L"report_V" + std::wstring(2048, L'0') + L"7.bin", Kind::Version) ==
        L"report_V" + std::wstring(2048, L'0') + L"8.bin", "arbitrarily long zero-padded version keeps its width");
    for (const auto name : {L"rev1.txt", L"reportV2.txt", L"report_v2_final.txt", L"report_v.txt", L"report_2.txt", L"report_v-2.txt"})
        Check(Find(name, Kind::Version).empty(), "version false-positive guard");
    Check(Find(L"report.txt", Kind::Version).empty(), "no version is invented for an unversioned name");
    Check(Find(L"报告.docx", Kind::Date) == L"报告_2026-11-25.docx", "date appends before existing extension");
    Check(Find(L"report_2026-11-24.txt", Kind::Date) == L"report_2026-11-25.txt", "date replaces a valid terminal date");
    Check(Find(L"report_2026-11-25.txt", Kind::Date).empty(), "same date does not return the input itself");
    Check(Find(L"a_2026-11-24_final.txt", Kind::Date) == L"a_2026-11-24_final_2026-11-25.txt", "date only replaces terminal suffix");
    Check(Find(L"report_2024-02-29.txt", Kind::Date) == L"report_2026-11-25.txt", "valid leap-day suffix is replaced");
    Check(Find(L"report_2023-02-29.txt", Kind::Date) == L"report_2023-02-29_2026-11-25.txt", "invalid historical date is not stripped");
    Check(Find(L"report", Kind::Date) == L"report_2026-11-25", "date never adds a default extension");
    Check(Find(L".env", Kind::Date) == L".env_2026-11-25", "single leading dot belongs to extensionless name");
    Check(Find(L"quarterly.report.docx", Kind::Date) == L"quarterly.report_2026-11-25.docx", "multi-dot date keeps full stem and final extension");
    Check(Find(L"\\\\server\\share\\report.txt", Kind::Date) == L"\\\\server\\share\\report_2026-11-25.txt", "date preserves full UNC path");
    for (const auto date : {L"", L"2026-13-01", L"2026-02-29", L"1900-02-29", L"0000-01-01", L"2026-01-00", L"2026-1-01", L"2026/11/25", L"2026-11-25x"})
        Check(Find(L"report.txt", Kind::Date, {}, date).empty(), "invalid supplied date produces no date suggestion");
    Check(Find(L"report.txt", Kind::Date, {}, L"2000-02-29") == L"report_2000-02-29.txt", "Gregorian 400-year leap date accepted");
    Check(Find(L"changed.txt", Kind::Original).empty(), "original name is never guessed");
    Check(Find(L"C:\\new\\changed.txt", Kind::Original, L"D:\\原始\\原稿.docx") == L"D:\\原始\\原稿.docx",
        "original restores explicit complete caller input verbatim");
    Check(Find(L"C:\\new\\changed.txt", Kind::Original, L"原稿") == L"原稿", "extensionless caller original is returned without joining or adding suffix");
    Check(Find(L"report.txt", Kind::Original, L"report.txt").empty(), "same original is omitted");
    Check(Find(L"report.txt", Kind::Original, L"bad?.txt").empty(), "illegal original is omitted");
    Check(Find(L"report - Copy.txt", Kind::RemoveCopy) == L"report.txt", "English explicit copy suffix removed");
    Check(Find(L"report - COPY.TXT", Kind::RemoveCopy) == L"report.TXT", "copy marker matching ignores ASCII case only");
    Check(Find(L"\\\\server\\share\\季度 - 副本.docx", Kind::RemoveCopy) == L"\\\\server\\share\\季度.docx",
        "Chinese explicit copy suffix removed with UNC path intact");
    Check(Find(L"report (2).pdf", Kind::RemoveCopy) == L"report.pdf", "numbered copy suffix removed");
    Check(Find(L"report (" + nines + L").pdf", Kind::RemoveCopy) == L"report.pdf", "long copy counter does not overflow");
    Check(Find(L"report - Copy", Kind::RemoveCopy) == L"report", "copy removal preserves absence of extension");
    Check(Find(L"a.b - Copy.txt", Kind::RemoveCopy) == L"a.b.txt", "copy removal preserves multi-dot stem");
    for (const auto name : {L"Copy report.txt", L"reportCopy.txt", L"report - Copy final.txt", L"report(2).txt", L"report (0).txt",
        L"report (1).txt", L"report (02).txt", L"report (2a).txt", L"report (2) final.txt", L"report副本.txt", L"副本 - report.txt"})
        Check(Find(name, Kind::RemoveCopy).empty(), "copy false-positive guard");
    Check(Find(L" - Copy.txt", Kind::RemoveCopy).empty(), "copy removal cannot create an empty stem");
    Check(Find(L"NUL - Copy.txt", Kind::RemoveCopy).empty(), "copy removal cannot create reserved device leaf");
    Check(Find(L"report_2026-11-24 - Copy.txt", Kind::RemoveCopy) == L"report_2026-11-24.txt", "copy removal leaves date untouched");
    for (const auto name : {L"", L".", L"..", L"folder\\", L"bad?.txt", L"bad*.txt", L"bad:stream", L"bad<.txt", L"bad>.txt", L"bad|.txt",
        L"bad\".txt", L"bad\n.txt", L"name.", L"name ", L"CON", L"nul.txt", L"AUX.txt", L"PRN.docx", L"NUL .txt", L"CONIN$", L"CONOUT$.txt", L"COM1.txt", L"Lpt9.pdf", L"COM\u00b9.txt", L"LPT\u00b3.txt",
        L"C:report.txt", L"C:\\wild*\\file.txt", L"C:\\bad?\\file.txt", L"http://host/report.txt"})
        Check(BuildSaveNameSuggestions(name, L"original.txt", L"2026-11-25").empty(), "invalid input produces no suggestions");
    Check(BuildSaveNameSuggestions(std::wstring(L"bad\0name.txt", 12), {}, L"2026-11-25").empty(), "embedded NUL rejected");
    Check(!BuildSaveNameSuggestions(L"COM10.txt", {}, L"2026-11-25").empty(), "non-device COM10 leaf remains valid");
    const auto duplicates = BuildSaveNameSuggestions(L"report - Copy.txt", L"report.txt", L"2026-11-25");
    Check(duplicates.size() == 2 && duplicates[0].kind == Kind::Date && duplicates[1].kind == Kind::Original,
        "deduplication keeps stable priority when original equals copy-stripped name");
    const auto dated_original = BuildSaveNameSuggestions(L"report.txt", L"report_2026-11-25.txt", L"2026-11-25");
    Check(dated_original.size() == 1 && dated_original[0].kind == Kind::Date, "original equal to date suggestion is deduplicated");
    const auto ordered = BuildSaveNameSuggestions(L"report_v1.txt", L"draft.txt", L"2026-11-25");
    Check(ordered.size() == 3 && ordered[0].kind == Kind::Version && ordered[1].kind == Kind::Date && ordered[2].kind == Kind::Original,
        "suggestions retain deterministic version date original priority");
    for (const auto name : {L"report_v1.txt", L"report - Copy.txt", L"report (2).txt", L"report_2026-11-25.txt", L"report.txt"}) {
        const auto result = BuildSaveNameSuggestions(name, L"draft.txt", L"2026-11-25");
        bool unique = result.size() <= 4;
        for (size_t i = 0; i < result.size(); ++i) {
            unique &= result[i].filename != name;
            for (size_t j = 0; j < i; ++j) unique &= result[j].filename != result[i].filename;
        }
        Check(unique, "all result variants are bounded unique and different from input");
    }
    std::printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}

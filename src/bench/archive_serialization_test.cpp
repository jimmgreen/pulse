#include "../preview_host/archive_serialization.h"
#include <cstdio>
using namespace pulse::preview;
int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; };
    ArchiveListingEntry a; a.path = L"folder/file10.txt"; a.size = 10; a.packed = 4;
    ArchiveListingEntry b; b.path = L"folder\\file2.txt"; b.size = 20; b.packed = 7; b.encrypted = true;
    const auto unknown = SerializeArchiveTree({a, b}, L"ZIP", false);
    check(unknown == L"PULSEARC\t1\tZIP\t-\t0\n0\tde\t30\t-\t-\tfolder\n1\te\t20\t-\t-\tfile2.txt\n1\t-\t10\t-\t-\tfile10.txt\n", "unknown packed sizes preserve exact tree and natural row ordering");
    const auto packed = SerializeArchiveTree({a, b}, L"ZIP", false, 11);
    check(packed == L"PULSEARC\t1\tZIP\t11\t0\n0\tde\t30\t11\t-\tfolder\n1\te\t20\t7\t-\tfile2.txt\n1\t-\t10\t4\t-\tfile10.txt\n", "known packed accounting preserves header and folder totals");
    check(SerializeArchiveTree({}, L"7Z", true) == L"PULSEARC\t1\t7Z\t-\t1\n", "empty incomplete archive preserves status");
    archive_serialization::Listing listing;
    listing.format = L"ZIP"; listing.skipped = 3;
    ArchiveListingEntry deep; for (int i = 0; i < 65; ++i) deep.path += L"part/";
    listing.entries.push_back(deep);
    check(archive_serialization::Serialize(listing) == L"PULSEARC\t1\tZIP\t-\t1\n" && listing.skipped == 4 && listing.incomplete,
          "depth budget preserves prior skipped accounting and marks incomplete");
    const std::wstring long_format(128, L'Z');
    check(SerializeArchiveTree({}, long_format, false, UINT64_MAX) ==
          L"PULSEARC\t1\t" + long_format + L"\t18446744073709551615\t0\n",
          "maximum format and packed total build a complete dynamic header");
    a.size = a.packed = UINT64_MAX;
    b.size = b.packed = 1;
    const auto overflow = SerializeArchiveTree({a, b}, L"ZIP", false, UINT64_MAX);
    check(overflow.find(L"0\tde\t18446744073709551615\t18446744073709551615\t-\tfolder\n") != std::wstring::npos,
          "folder size and packed totals saturate instead of wrapping");
    return failures ? 1 : 0;
}

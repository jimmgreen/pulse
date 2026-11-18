#include "../fs/drive_kind.h"
// This scheduler must not grow a volume probe, even though drive_kind retains
// its separate background-only implementation for other callers.
#define GetDriveTypeW ForbiddenSchedulingVolumeProbe
#include "../fs/enumeration_schedule.h"
#undef GetDriveTypeW
#include <cstdio>
#include <algorithm>
#include <map>
#include <string>

int main() {
    int failures = 0;
    unsigned queries = 0;
    std::map<wchar_t, std::wstring> mappings{
        {L'C', L"\\Device\\HarddiskVolume3"},
        {L'M', L"\\Device\\Mup\\server\\share"},
        {L'L', L"\\Device\\LanmanRedirector\\;L:123\\server\\share"},
        {L'W', L"\\Device\\WebDavRedirector\\server"},
        {L'V', L"\\Device\\Volume{abc}"},
        {L'S', L"\\??\\C:\\folder"}, {L'T', L"\\??\\M:\\folder"},
        {L'R', L"\\??\\S:\\nested"},
        {L'X', L"\\??\\Y:\\folder"}, {L'Y', L"\\??\\X:\\folder"},
        {L'U', L"\\Device\\UnknownProvider"},
        {L'N', L"\\??\\UNC\\server\\share"},
        {L'B', L"\\device\\harddiskvolume12"}
    };
    const auto query = [&](const wchar_t* device, wchar_t* target, DWORD capacity) -> DWORD {
        ++queries;
        const auto found = mappings.find(device[0]);
        if (found == mappings.end() || found->second.size() + 2 > capacity) return 0;
        std::copy(found->second.begin(), found->second.end(), target);
        target[found->second.size()] = 0;
        target[found->second.size() + 1] = 0;
        return static_cast<DWORD>(found->second.size() + 2);
    };
    const auto check = [&](const wchar_t* path, bool remote, unsigned expected_queries, const char* label) {
        queries = 0;
        const bool actual = pulse::fs::NeedsBoundedEnumeration(path, query);
        const bool ok = actual == remote && queries == expected_queries;
        printf("[%s] %s queries=%u\n", ok ? "PASS" : "FAIL", label, queries);
        failures += !ok;
    };
    check(L"C:\\folder", false, 1, "hard disk is local");
    check(L"b:\\folder", false, 1, "drive and device matching is case insensitive");
    check(L"\\\\?\\C:\\folder", false, 1, "extended drive is local");
    check(L"\\\\.\\C:\\folder", false, 1, "device-prefix drive is local");
    check(L"\\\\server\\share", true, 0, "UNC needs no query");
    check(L"\\\\?\\UNC\\server\\share", true, 0, "extended UNC needs no query");
    check(L"M:\\folder", true, 1, "MUP mapping is remote");
    check(L"L:\\folder", true, 1, "Lanman mapping is remote");
    check(L"W:\\folder", true, 1, "WebDav mapping is remote");
    check(L"V:\\folder", true, 1, "user-mode volume is remote");
    check(L"S:\\folder", false, 2, "subst local follows mapping");
    check(L"T:\\folder", true, 2, "subst mapped drive remains remote");
    check(L"R:\\folder", false, 3, "nested subst resolves local");
    check(L"X:\\folder", true, 2, "subst cycle terminates conservatively");
    check(L"N:\\folder", true, 1, "subst UNC is remote");
    check(L"U:\\folder", true, 1, "unknown provider is bounded");
    check(L"Z:\\folder", true, 1, "missing mapping is bounded");
    check(L"\\\\?\\Volume{abc}\\folder", true, 0, "letterless extended volume is bounded");
    check(L"", false, 0, "empty virtual view does not consume remote quota");
    check(L"shell:Recent", false, 0, "virtual view does not consume remote quota");
    for (wchar_t letter = L'A'; letter <= L'I'; ++letter)
        mappings[letter] = L"\\??\\" + std::wstring(1, static_cast<wchar_t>(letter + 1)) + L":\\folder";
    check(L"A:\\folder", true, 8, "subst depth is bounded");
    printf("enumeration-schedule: %d failed\n", failures);
    return failures ? 1 : 0;
}

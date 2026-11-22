#include "../common/search_uri.h"
#include <cstdio>

int main() {
    int failed = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failed += !ok;
    };
    std::wstring keyword;
    check(pulse::ParseSearchUri(L"pulse://search/%E4%B8%AD%E6%96%87%20a%2Fb%3F%23%25+", keyword) &&
        keyword == L"中文 a/b?#%+", "UTF-8 and reserved characters");
    check(pulse::ParseSearchUri(L"PULSE://search/中文 空格", keyword) && keyword == L"中文 空格", "Unicode and scheme case");
    check(pulse::ParseSearchUri(L"pulse://search/%2520", keyword) && keyword == L"%20", "decode once");
    for (const auto* invalid : {L"pulse://search/", L"pulse://search/%20", L"pulse://unknown/name",
        L"pulse://search/%", L"pulse://search/%GG", L"pulse://search/%C0%AF", L"pulse://search/%ED%A0%80",
        L"pulse://search/%F4%90%80%80", L"pulse://search/%00", L"pulse://search/%0A",
        L"pulse://search/a?b", L"pulse://search/a#b", L"pulse://search/a/b"})
        check(!pulse::ParseSearchUri(invalid, keyword) && keyword.empty(), "invalid URI rejected atomically");
    check(pulse::ParseSearchUri(L"pulse://search/" + std::wstring(pulse::kSearchKeywordLimit, L'a'), keyword), "keyword boundary");
    check(!pulse::ParseSearchUri(L"pulse://search/" + std::wstring(pulse::kSearchKeywordLimit + 1, L'a'), keyword), "keyword limit");
    check(!pulse::ParseSearchUri(L"pulse://search/" + std::wstring(pulse::kSearchUriLimit, L'a'), keyword), "URI limit");
    return failed ? 1 : 0;
}

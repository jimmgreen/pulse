#include "archive_paths.h"
#include <cstdio>
struct Case { const wchar_t* input; const wchar_t* parent; };
int main() {
    const Case cases[] = {
        {LR"(\\?\E:\HyperSnap)", LR"(\\?\E:\)"},
        {LR"(\\?\E:\file.zip)", LR"(\\?\E:\)"},
        {LR"(\\?\E:\)", LR"(\\?\E:\)"},
        {LR"(\\?\E:\dir\file.zip)", LR"(\\?\E:\dir)"},
        {LR"(E:\file.zip)", LR"(E:\)"},
        {LR"(E:\)", LR"(E:\)"},
        {LR"(E:relative.zip)", LR"(E:)"},
        {LR"(E:)", LR"(E:)"},
        {LR"(\\server\share\file.zip)", LR"(\\server\share\)"},
        {LR"(\\?\UNC\server\share\file.zip)", LR"(\\?\UNC\server\share\)"},
        {LR"(\\?\UNC\server\share\dir\file.zip)", LR"(\\?\UNC\server\share\dir)"},
        {LR"(\\?\UNC\server\share\)", LR"(\\?\UNC\server\share\)"},
        {LR"(relative\file.zip)", L"relative"},
        {L"file.zip", L""}
    };
    int failed = 0;
    for (const auto& item : cases) {
        const auto actual = archive_paths::Parent(item.input).wstring();
        const bool ok = actual == item.parent;
        std::wprintf(L"[%ls] parent [%ls] => [%ls], expected [%ls]\n", ok ? L"PASS" : L"FAIL", item.input, actual.c_str(), item.parent);
        failed += !ok;
    }
    for (const auto* path : {LR"(\\?\E:\first.txt)", LR"(E:\first.txt)", LR"(\\?\UNC\server\share\first.txt)"}) {
        const auto name = archive_paths::ParentArchiveName(path);
        const bool ok = name == L"Archive";
        std::wprintf(L"[%ls] root multiselect name [%ls] => [%ls]\n", ok ? L"PASS" : L"FAIL", path, name.c_str());
        failed += !ok;
    }
    std::printf("RESULT failures=%d\n", failed);
    return failed ? 1 : 0;
}

// ai_chat_store_test.cpp - 55 P3 历史对话's store: save / load / list /
// rename / pin / delete / purge, unreadable files skipped, ids that could
// leave the folder refused, and the one-line plain text of an answer.
// Works in a scratch folder under %TEMP%, never the real chats folder.
#include "../app/ai_chat_store.h"
#include "../common/json_value.h"

#include <windows.h>

#include <cstdio>
#include <string>

using namespace pulse;
namespace ai = pulse::app::ai;

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", what);
    if (!ok) ++g_failed;
}

json::Value Doc(const char* title, int64_t updated, bool pinned, const char* answer) {
    json::Value doc = json::Value::Object();
    doc.set("v", 1);
    doc.set("title", title);
    doc.set("created", updated);
    doc.set("updated", updated);
    if (pinned) doc.set("pinned", true);
    json::Value blocks = json::Value::Array();
    json::Value user = json::Value::Object();
    user.set("k", "user");
    user.set("text", title);
    blocks.push(std::move(user));
    json::Value text = json::Value::Object();
    text.set("k", "text");
    text.set("text", answer);
    blocks.push(std::move(text));
    doc.set("blocks", std::move(blocks));
    doc.set("history", json::Value::Array());
    return doc;
}

void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            DeleteFileW((dir + L"\\" + name).c_str());
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

}  // namespace

int main() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring root = std::wstring(temp) + L"pulse_chat_store_test_" + std::to_wstring(GetCurrentProcessId());
    const std::wstring dir = root + L"\\chats";
    RemoveTree(dir);
    RemoveDirectoryW(root.c_str());
    CreateDirectoryW(root.c_str(), nullptr);

    const int64_t now = ai::NowUnixMs();
    const int64_t day = 86400000ll;

    // Ids.
    const std::wstring id = ai::NewChatId(now, 0xBEEF);
    Check(id.size() == 20 && id[8] == L'-' && id[15] == L'-' && id.substr(16) == L"beef", "NewChatId is yyyyMMdd-HHmmss-xxxx");
    Check(ai::ValidChatId(id), "a new id is valid");
    Check(!ai::ValidChatId(L"..\\evil") && !ai::ValidChatId(L"a/b") && !ai::ValidChatId(L"") &&
              !ai::ValidChatId(std::wstring(41, L'a')),
          "ids that could leave the folder are refused");
    Check(!ai::SaveChat(dir, L"..\\x", Doc("x", now, false, "x")), "SaveChat refuses a bad id");

    // Save creates the folder; load reads it back.
    Check(ai::SaveChat(dir, L"20260101-000000-0001", Doc("整理下载文件夹", now - 3 * day, false, "## 已整理\n- **12** 个文件")),
          "SaveChat creates the folder");
    json::Value back;
    Check(ai::LoadChat(dir, L"20260101-000000-0001", back) && back.get_string("title") == "整理下载文件夹",
          "LoadChat reads the title back");
    Check(ai::SaveChat(dir, L"20260102-000000-0002", Doc("Find large videos", now - 40 * day, false, "Found `3` videos")),
          "a second conversation");
    Check(ai::SaveChat(dir, L"20260103-000000-0003", Doc("Pinned plan", now - 400 * day, true, "Plan")),
          "a pinned old conversation");
    // Files that are not conversations are skipped.
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, (dir + L"\\20260104-000000-0004.json").c_str(), L"wb") == 0 && f) {
            std::fputs("{ not json", f);
            std::fclose(f);
        }
        if (_wfopen_s(&f, (dir + L"\\bad name.json").c_str(), L"wb") == 0 && f) {
            std::fputs("{}", f);
            std::fclose(f);
        }
    }

    std::vector<ai::ChatEntry> list = ai::ListChats(dir);
    Check(list.size() == 3, "ListChats skips unreadable files and bad names");
    Check(list.size() == 3 && list[0].id == L"20260101-000000-0001" && list[2].id == L"20260103-000000-0003",
          "newest first by updated");
    Check(!list.empty() && list[0].snippet == L"已整理 12 个文件", "the snippet is the answer's plain first line");
    Check(!list.empty() && list[0].haystack.find(L"整理下载") != std::wstring::npos, "the haystack has the message");
    Check(list.size() > 1 && list[1].haystack.find(ai::SearchKey(L"LARGE")) != std::wstring::npos,
          "search is case-insensitive");

    // Rename and pin keep the rest (and do not move it).
    const std::wstring title = L"下载整理";
    const bool pin = true;
    Check(ai::UpdateChatMeta(dir, L"20260101-000000-0001", &title, nullptr), "UpdateChatMeta renames");
    Check(ai::UpdateChatMeta(dir, L"20260102-000000-0002", nullptr, &pin), "UpdateChatMeta pins");
    list = ai::ListChats(dir);
    Check(list.size() == 3 && list[0].title == L"下载整理" && list[1].pinned, "rename and pin are listed");
    json::Value renamed;
    Check(ai::LoadChat(dir, L"20260101-000000-0001", renamed) && renamed.get_bool("renamed") &&
              renamed.find("blocks") && renamed.find("blocks")->size() == 2,
          "the renamed file keeps its blocks");

    // Purge: 30 days drops the unpinned 40-day-old one only... which is now
    // pinned, so nothing; unpin it and it goes. Pinned old ones stay.
    Check(ai::PurgeChats(dir, 30, now) == 0, "pinned conversations are kept by 保留时间");
    const bool unpin = false;
    ai::UpdateChatMeta(dir, L"20260102-000000-0002", nullptr, &unpin);
    Check(ai::PurgeChats(dir, 30, now) == 1, "an unpinned conversation past 保留时间 goes");
    Check(ai::PurgeChats(dir, 0, now) == 0, "永久 keeps everything");
    list = ai::ListChats(dir);
    Check(list.size() == 2, "two remain");

    Check(ai::DeleteChat(dir, L"20260101-000000-0001") && !ai::LoadChat(dir, L"20260101-000000-0001", back),
          "DeleteChat removes the file");
    Check(!ai::DeleteChat(dir, L"..\\..\\x"), "DeleteChat refuses a bad id");

    // PlainLine.
    Check(ai::PlainLine(L"# Title **bold** `code`", 80) == L"Title bold code", "PlainLine drops heading, emphasis, ticks");
    Check(ai::PlainLine(L"> quoted\n- item one\n1. item two", 80) == L"quoted item one item two",
          "PlainLine drops quote and list markers");
    Check(ai::PlainLine(L"```\ncode\n```\nafter", 80) == L"code after", "PlainLine drops fence lines");
    Check(ai::PlainLine(L"snake_case_name", 80) == L"snake_case_name", "PlainLine keeps underscores inside words");
    Check(ai::PlainLine(L"abcdefghij", 5) == L"abcd\x2026", "PlainLine cuts with an ellipsis");

    RemoveTree(dir);
    RemoveDirectoryW(root.c_str());
    std::printf("%s ai_chat_store_test (%d failed)\n", g_failed ? "[FAIL]" : "[PASS]", g_failed);
    return g_failed ? 1 : 0;
}

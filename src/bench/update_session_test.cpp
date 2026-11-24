#include "../app/app_prefs.h"
#include "../app/session_save.h"
#include "../app/startup_location.h"
#include "../common/async_file_writer.h"

#include <cstdio>
#include <filesystem>

int main() {
    int failures = 0;
    auto check = [&failures](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };

    pulse::app::AppPrefs prefs;
    prefs.persist = false;
    prefs.startup_open = 1;
    check(!pulse::app::RestoresLastTabs(prefs), "normal launch honors default-location preference");
    check(pulse::app::RestoresLastTabs(prefs, true), "update restart restores tabs despite preference");
    check(prefs.startup_open == 1 && !pulse::app::RestoresLastTabs(prefs),
          "update override does not change later normal launches");
    prefs.startup_open = 0;
    check(pulse::app::RestoresLastTabs(prefs), "normal restore preference remains supported");

    std::wstring saved = L"old session";
    int writes = 0;
    bool writable = false;
    auto writer = [&writes, &writable](const std::wstring&) {
        ++writes;
        return writable;
    };
    check(!pulse::app::SaveChangedSession(L"new session", saved, false, writer) &&
          saved == L"old session" && writes == 1,
          "failed save refuses update and leaves previous cache intact");
    writable = true;
    check(pulse::app::SaveChangedSession(L"new session", saved, false, writer) &&
          saved == L"new session" && writes == 2,
          "failed save can be retried successfully");
    check(pulse::app::SaveChangedSession(L"new session", saved, false, writer) && writes == 2,
          "unchanged update session does not write again");
    check(pulse::app::SaveChangedSession(L"new session", saved, true, writer) && writes == 3,
          "normal forced save behavior remains supported");

    {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        const std::filesystem::path dir = std::filesystem::path(temp) /
            (L"pulse-async-writer-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(dir);
        const std::wstring target = (dir / L"session.json").wstring();
        pulse::AsyncUtf8FileWriter async_writer;
        check(async_writer.Submit(target, L"{\"v\":1}") &&
              async_writer.Submit(target, L"{\"v\":2}") &&
              async_writer.Submit(target, L"{\"v\":3,\"zh\":\"中文\"}"),
              "async writer queues and coalesces repeated saves for the same path");
        async_writer.Flush();
        std::wstring read_back;
        check(pulse::ReadUtf8File(target, read_back) && read_back == L"{\"v\":3,\"zh\":\"中文\"}",
              "async writer flushes latest coalesced UTF-8 payload atomically");
        const std::wstring target2 = (dir / L"app.json").wstring();
        check(async_writer.Submit(target2, L"{\"final\":true}"),
              "async writer accepts trailing write before stop");
        async_writer.Stop();
        read_back.clear();
        check(pulse::ReadUtf8File(target2, read_back) && read_back == L"{\"final\":true}",
              "async writer drains queued writes cleanly on Stop()");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    return failures == 0 ? 0 : 1;
}

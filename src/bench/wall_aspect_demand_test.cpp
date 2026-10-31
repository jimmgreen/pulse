#include "../ui/wall_aspects.h"
#include <cstdio>
namespace pulse::ui {
struct WallAspectDemandTest {
    static bool Run() {
        WallAspects cache; cache.running_ = true;
        bool ok = true;
        auto check = [&](bool passed, const char* label) { std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label); ok &= passed; };
        std::vector<std::pair<uint64_t, std::wstring>> old;
        for (uint64_t i = 1; i <= 50000; ++i) old.push_back({i, L"old"});
        cache.ReplaceDemand(1, nullptr, std::move(old));
        check(cache.demands_.at(1).files.size() == 512, "50000-item input is bounded to 512");
        cache.ReplaceDemand(1, nullptr, {{60000, L"visible"}, {60001, L"nearby"}});
        std::pair<uint64_t, std::wstring> next;
        check(cache.TakeLocked(next) && next.first == 60000, "deep-scroll visible item is next, not behind 50000 old probes");
        cache.ReplaceDemand(2, nullptr, {{70000, L"other-window"}});
        check(cache.TakeLocked(next) && next.first == 70000, "second window gets fair next probe");
        cache.RemoveDemand(1);
        check(!cache.WantedLocked(60000) && cache.WantedLocked(70000), "leaving one view preserves other window demand");
        const auto v1 = WallAspects::ContentHash(L"same.png", 1, 8);
        const auto v2 = WallAspects::ContentHash(L"same.png", 2, 8);
        cache.Note(v1, 2.0f);
        float ratio = 1.0f;
        check(v1 != v2 && !cache.Lookup(&v2, &ratio, 1) && ratio == 1.0f, "same-path replacement does not reuse old aspect");
        uint64_t revision = cache.Version();
        cache.Note(v2, 3.0f);
        std::vector<WallAspects::AspectChange> changes;
        check(cache.ChangesSince(revision, changes) && changes.size() == 1 && changes[0].hash == v2,
              "bounded change log returns only later content versions");
        for (uint64_t i = 100000; i < 108300; ++i) cache.Note(i, 1.5f);
        check(!cache.ChangesSince(revision, changes) && cache.changes_.size() == 8192, "change-log overflow requests full lookup without unbounded growth");
        cache.RemoveDemand(2);
        check(!cache.TakeLocked(next) && cache.demands_.empty(), "last view removal retires all queued probes");
        wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
        const std::wstring path = std::wstring(temp) + L"pulse-wall-aspect-" + std::to_wstring(GetCurrentProcessId()) + L".gif";
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        const unsigned char gif[] = {'G','I','F','8','9','a', 20,0,10,0}; DWORD written = 0;
        const bool created = handle != INVALID_HANDLE_VALUE && WriteFile(handle, gif, sizeof(gif), &written, nullptr);
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const bool stat = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) != FALSE;
        const uint64_t modified = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        const uint64_t current_key = WallAspects::ContentHash(path, modified, sizeof(gif));
        const uint64_t stale_key = WallAspects::ContentHash(path, modified - 1, sizeof(gif));
        cache.ReplaceDemand(3, nullptr, {{stale_key, path}, {current_key, path}});
        cache.Run();
        float actual = 1.0f;
        check(created && stat && !cache.known_.contains(stale_key) && cache.Lookup(&current_key, &actual, 1) && actual == 2.0f,
              "real header probe rejects stale metadata version and publishes matching version");
        DeleteFileW(path.c_str());
        return ok;
    }
};
}
int main() { return pulse::ui::WallAspectDemandTest::Run() ? 0 : 1; }

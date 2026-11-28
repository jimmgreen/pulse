// background_image_test.cpp — the shared background source, without UI or network.
#include "../app/background_image_fetch.h"
#include "../app/background_image_service.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace pulse::app;

int g_failures = 0;

bool Report(const char* name, bool passed) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    if (!passed) ++g_failures;
    return passed;
}

bool WaitFor(const BackgroundImageService& service,
             const std::function<bool(const BackgroundImageSnapshot&)>& predicate,
             int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (predicate(service.Snapshot())) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::filesystem::path MakeTempDir(const wchar_t* tag) {
    std::error_code error;
    std::filesystem::path base = std::filesystem::temp_directory_path(error);
    if (error) {
        error.clear();
        base = std::filesystem::current_path(error);
    }
    const std::filesystem::path directory =
        base / (std::wstring(L"pulse_background_") + tag + L"_" +
                std::to_wstring(GetCurrentProcessId()) + L"_" +
                std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory, error);
    return directory;
}

void RemoveTree(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

bool WriteTestFile(const std::filesystem::path& path, std::string_view content) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return file.good();
}

std::vector<uint8_t> Bytes(std::string_view text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::vector<uint8_t> PngBytes() {
    static const unsigned char kPng[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x00};
    return std::vector<uint8_t>(kPng, kPng + sizeof(kPng));
}

std::vector<uint8_t> GifBytes() {
    return Bytes("GIF89a-pulse-test");
}

bool EndsWithIgnoreCase(std::wstring_view value, std::wstring_view suffix) {
    if (value.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        wchar_t left = value[value.size() - suffix.size() + i];
        wchar_t right = suffix[i];
        if (left >= L'A' && left <= L'Z') left = static_cast<wchar_t>(left - L'A' + L'a');
        if (right >= L'A' && right <= L'Z') right = static_cast<wchar_t>(right - L'A' + L'a');
        if (left != right) return false;
    }
    return true;
}

}  // namespace

int main() {
    // Image: the path is published from the configuration itself.
    {
        BackgroundImageService service;
        BackgroundImageConfig config;
        config.source = BackgroundSource::Image;
        config.image = L"C:\\wallpapers\\one.png";
        const bool changed = service.SetConfig(config);
        const auto snapshot = service.Snapshot();
        Report("image source publishes the configured path without a request",
               changed && snapshot.path == config.image && snapshot.version == 1 &&
                   !snapshot.network && !snapshot.busy &&
                   service.NetworkRequestsForTesting() == 0);
        Report("an unchanged configuration is ignored",
               !service.SetConfig(config) && service.Snapshot().version == 1);
        service.Tick(1000, false);
        Report("manual switching never republishes an image",
               service.Snapshot().version == 1);
        service.Clear();
        const auto cleared = service.Snapshot();
        Report("clear drops the image and stops scheduling",
               cleared.path.empty() && !cleared.busy && !service.Next());
    }

    // Folder: recursive scan, case-insensitive extensions, and a non-repeating deal.
    {
        const auto root = MakeTempDir(L"folder");
        WriteTestFile(root / L"top.jpg", "jpeg");
        WriteTestFile(root / L"nested" / L"deep.PNG", "png");
        BackgroundImageService service;
        BackgroundImageConfig config;
        config.source = BackgroundSource::Folder;
        config.folder = root.wstring();
        const bool changed = service.SetConfig(config);
        const bool finished = WaitFor(service, [](const auto& snapshot) { return !snapshot.busy; });
        const auto first = service.Snapshot();
        const bool first_ok = finished && !first.path.empty() && first.error.empty() &&
                              std::filesystem::exists(first.path) &&
                              (EndsWithIgnoreCase(first.path, L".jpg") ||
                               EndsWithIgnoreCase(first.path, L".png"));
        const bool next = service.Next();
        const bool advanced = WaitFor(service, [&](const auto& snapshot) {
            return !snapshot.busy && snapshot.path != first.path;
        });
        const auto second = service.Snapshot();
        Report("folder scan finds images recursively and deals without repeats",
               changed && first_ok && next && advanced &&
                   second.version == first.version + 1 && second.path != first.path);
        RemoveTree(root);
    }

    // Folder with one image: the same path may repeat, but a publish still bumps
    // the version so render caches can invalidate.
    {
        const auto root = MakeTempDir(L"single");
        WriteTestFile(root / L"only.png", "png");
        BackgroundImageService service;
        BackgroundImageConfig config;
        config.source = BackgroundSource::Folder;
        config.folder = root.wstring();
        service.SetConfig(config);
        WaitFor(service, [](const auto& snapshot) { return !snapshot.busy; });
        const auto first = service.Snapshot();
        const bool next = service.Next();
        const bool advanced = WaitFor(service, [&](const auto& snapshot) {
            return !snapshot.busy && snapshot.version > first.version;
        });
        const auto second = service.Snapshot();
        Report("republishing the same path still bumps the version",
               next && advanced && second.path == first.path &&
                   second.version == first.version + 1);
        RemoveTree(root);
    }

    // Folder: an empty root is an error, not a crash.
    {
        const auto root = MakeTempDir(L"empty");
        BackgroundImageService service;
        BackgroundImageConfig config;
        config.source = BackgroundSource::Folder;
        config.folder = root.wstring();
        const bool changed = service.SetConfig(config);
        const bool finished = WaitFor(service, [](const auto& snapshot) { return !snapshot.busy; });
        const auto snapshot = service.Snapshot();
        Report("an empty folder reports an error",
               changed && finished && !snapshot.error.empty() && snapshot.path.empty());
        RemoveTree(root);
    }

    // RFC 6901 subset.
    {
        std::string value;
        const std::string nested = R"({"data":{"url":"https://example.com/a.png"}})";
        Report("json pointer finds a nested object value",
               JsonPointerString(nested, L"/data/url", value) == JsonPointerStatus::Found &&
                   value == "https://example.com/a.png");
        const std::string array = R"({"list":[{"url":"a"},{"url":"b"}]})";
        Report("json pointer indexes an array",
               JsonPointerString(array, L"/list/1/url", value) == JsonPointerStatus::Found &&
                   value == "b");
        const std::string escaped = R"({"a/b":{"c~d":"https://example.com/e.png"}})";
        Report("json pointer decodes ~1 and ~0",
               JsonPointerString(escaped, L"/a~1b/c~0d", value) == JsonPointerStatus::Found &&
                   value == "https://example.com/e.png");
        Report("json pointer reports a missing key",
               JsonPointerString(nested, L"/data/missing", value) ==
                       JsonPointerStatus::NotFound &&
                   value.empty());
        Report("json pointer rejects a non-string value",
               JsonPointerString(nested, L"/data", value) == JsonPointerStatus::NotString &&
                   value.empty());
        Report("json pointer does not depend on key order",
               JsonPointerString(R"({"b":1,"a":{"url":"z"}})", L"/a/url", value) ==
                       JsonPointerStatus::Found &&
                   value == "z");
    }

    // Shuffle bag.
    {
        std::mt19937 rng(20261128);
        BackgroundShuffleBag bag;
        bag.Reset(10, rng);
        std::vector<bool> seen(10, false);
        bool unique = true;
        size_t last = 0;
        for (size_t i = 0; i < 10; ++i) {
            last = bag.Take(rng);
            if (last >= seen.size() || seen[last]) {
                unique = false;
            } else {
                seen[last] = true;
            }
        }
        const size_t refill = bag.Take(rng);
        BackgroundShuffleBag single;
        single.Reset(1, rng);
        Report("shuffle bag deals each entry once", unique);
        Report("a refilled bag never leads with the previous entry", refill != last);
        Report("a single-entry bag repeats only that entry",
               single.Take(rng) == 0 && single.Take(rng) == 0);
    }

    // Generation acceptance is a pure comparison.
    Report("generation accepts only its captured value",
           IsCurrentBackgroundGeneration(7, 7) &&
               !IsCurrentBackgroundGeneration(7, 8) &&
               !IsCurrentBackgroundGeneration(8, 7));

    // Magic sniffing (the service refuses anything else before caching it).
    {
        const auto png = PngBytes();
        const std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE0};
        const auto gif = Bytes("GIF89a");
        const auto bmp = Bytes("BM");
        const auto webp = Bytes("RIFF....WEBP");
        const std::vector<uint8_t> tiff = {0x49, 0x49, 0x2A, 0x00};
        const auto html = Bytes("<html>");
        Report("magic sniffing recognizes every supported format",
               SniffBackgroundImage(png.data(), png.size()) == BackgroundImageFormat::Png &&
                   SniffBackgroundImage(jpeg.data(), jpeg.size()) ==
                       BackgroundImageFormat::Jpeg &&
                   SniffBackgroundImage(gif.data(), gif.size()) == BackgroundImageFormat::Gif &&
                   SniffBackgroundImage(bmp.data(), bmp.size()) == BackgroundImageFormat::Bmp &&
                   SniffBackgroundImage(webp.data(), webp.size()) ==
                       BackgroundImageFormat::Webp &&
                   SniffBackgroundImage(tiff.data(), tiff.size()) ==
                       BackgroundImageFormat::Tiff &&
                   SniffBackgroundImage(html.data(), html.size()) ==
                       BackgroundImageFormat::Unknown);
    }

    // Api: JSON pointer -> image URL -> second fetch -> cached file.
    {
        std::mutex request_mutex;
        std::vector<std::wstring> requests;
        const auto cache = MakeTempDir(L"api");
        BackgroundImageService service;
        service.SetCacheRootForTesting(cache);
        service.SetFetchForTesting([&](const BackgroundFetchRequest& request,
                                       BackgroundFetchResult& result) {
            {
                std::lock_guard lock(request_mutex);
                requests.push_back(request.url);
            }
            if (request.url == L"https://api.example/v1") {
                result.body = Bytes(R"({"data":{"url":"https://img.example/pic.png"}})");
            } else if (request.url == L"https://img.example/pic.png") {
                result.body = PngBytes();
            } else {
                result.error = ERROR_FILE_NOT_FOUND;
                return false;
            }
            result.ok = true;
            return true;
        });
        BackgroundImageConfig config;
        config.source = BackgroundSource::Api;
        config.url = L"https://api.example/v1";
        config.api_pointer = L"/data/url";
        const bool changed = service.SetConfig(config);
        const bool finished = WaitFor(service, [](const auto& snapshot) { return !snapshot.busy; });
        const auto snapshot = service.Snapshot();
        std::error_code size_error;
        const auto cached_size = std::filesystem::file_size(snapshot.path, size_error);
        const bool cached = !snapshot.path.empty() && !size_error &&
                            std::filesystem::exists(snapshot.path) &&
                            EndsWithIgnoreCase(snapshot.path, L".png") &&
                            cached_size == PngBytes().size();
        Report("api pointer resolves and caches the image",
               changed && finished && cached && snapshot.network && snapshot.error.empty() &&
                   snapshot.version == 1);
        Report("api flow made exactly two requests",
               service.NetworkRequestsForTesting() == 2);
        {
            std::lock_guard lock(request_mutex);
            Report("api flow requested the resolved image URL",
                   requests.size() == 2 && requests[1] == L"https://img.example/pic.png");
        }
        RemoveTree(cache);
    }

    // Failures keep the previous image and only update the error.
    {
        const auto cache = MakeTempDir(L"failure");
        BackgroundImageService service;
        service.SetCacheRootForTesting(cache);
        BackgroundImageConfig config;
        config.source = BackgroundSource::Image;
        config.image = L"C:\\wallpapers\\keep.png";
        service.SetConfig(config);

        service.SetFetchForTesting([](const BackgroundFetchRequest&,
                                      BackgroundFetchResult& result) {
            result.body = Bytes("<html>not an image</html>");
            result.ok = true;
            return true;
        });
        config.source = BackgroundSource::Url;
        config.url = L"https://img.example/html";
        service.SetConfig(config);
        WaitFor(service, [](const auto& snapshot) { return !snapshot.busy; });
        auto snapshot = service.Snapshot();
        Report("a non-image response keeps the previous image",
               snapshot.path == config.image && !snapshot.error.empty() &&
                   snapshot.version == 1);

        service.SetFetchForTesting([](const BackgroundFetchRequest&,
                                      BackgroundFetchResult& result) {
            result.error = ERROR_FILE_TOO_LARGE;
            return false;
        });
        config.url = L"https://img.example/large";
        service.SetConfig(config);
        WaitFor(service, [](const auto& snapshot) { return !snapshot.busy; });
        snapshot = service.Snapshot();
        Report("an oversized response keeps the previous image",
               snapshot.path == config.image &&
                   snapshot.error.find(L"large") != std::wstring::npos);

        service.SetFetchForTesting([](const BackgroundFetchRequest&,
                                      BackgroundFetchResult& result) {
            result.error = ERROR_TIMEOUT;
            return false;
        });
        config.url = L"https://img.example/timeout";
        service.SetConfig(config);
        WaitFor(service, [](const auto& snapshot) { return !snapshot.busy; });
        snapshot = service.Snapshot();
        Report("a timeout keeps the previous image",
               snapshot.path == config.image &&
                   snapshot.error.find(L"timeout") != std::wstring::npos);
        RemoveTree(cache);
    }

    // A real worker: a stale generation must not publish its cached file.
    {
        std::mutex gate_mutex;
        std::condition_variable gate_cv;
        int entered = 0;
        bool release_first = false;
        bool release_second = false;
        const auto cache = MakeTempDir(L"stale");
        BackgroundImageService service;
        service.SetCacheRootForTesting(cache);
        service.SetFetchForTesting([&](const BackgroundFetchRequest&,
                                       BackgroundFetchResult& result) {
            int index = 0;
            {
                std::unique_lock lock(gate_mutex);
                index = entered++;
                gate_cv.notify_all();
                if (index == 0) {
                    gate_cv.wait(lock, [&] { return release_first; });
                } else if (index == 1) {
                    gate_cv.wait(lock, [&] { return release_second; });
                }
            }
            result.body = index == 0 ? PngBytes() : GifBytes();
            result.ok = true;
            return true;
        });
        BackgroundImageConfig config;
        config.source = BackgroundSource::Url;
        config.url = L"https://img.example/first.png";
        service.SetConfig(config);
        bool first_started = false;
        {
            std::unique_lock lock(gate_mutex);
            first_started =
                gate_cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered >= 1; });
        }
        config.url = L"https://img.example/second.gif";
        service.SetConfig(config);
        bool second_started = false;
        bool stale_path_published = false;
        bool still_busy = false;
        {
            std::unique_lock lock(gate_mutex);
            release_first = true;
            gate_cv.notify_all();
            second_started =
                gate_cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered >= 2; });
            const auto middle = service.Snapshot();
            stale_path_published = !middle.path.empty();
            still_busy = middle.busy;
            release_second = true;
            gate_cv.notify_all();
        }
        const bool finished =
            WaitFor(service, [](const auto& snapshot) { return !snapshot.busy; });
        const auto snapshot = service.Snapshot();
        Report("a stale generation is not published",
               first_started && second_started && !stale_path_published && still_busy);
        Report("the newest generation publishes its own image",
               finished && snapshot.network &&
                   EndsWithIgnoreCase(snapshot.path, L".gif") && snapshot.error.empty());
        RemoveTree(cache);
    }

    std::printf("%s\n", g_failures == 0 ? "All background image tests passed."
                                        : "Background image tests failed.");
    return g_failures == 0 ? 0 : 1;
}

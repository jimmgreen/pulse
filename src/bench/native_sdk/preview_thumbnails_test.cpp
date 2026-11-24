// pulse.preview thumbnails: provider flag, PreviewsAdapter::Thumbnail on other threads,
// scaling, budget, retire/close gates, and the SDK PreviewClient / PreviewContext wrapper.
#include "../../app/extensions/native/previews_adapter.h"
#include <pulse/client.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
using namespace pulse::extensions::native;
namespace {
int failures = 0, passes = 0;
void Check(bool value, const char* label) { std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label); value ? ++passes : ++failures; }
PulseStringView View(const char* text) { return {text, static_cast<uint32_t>(std::strlen(text))}; }
bool Wait(const std::function<bool()>& ready) {
    const auto end = GetTickCount64() + 5000;
    do { if (ready()) return true; Sleep(1); } while (GetTickCount64() < end);
    return false;
}
struct Provider {
    std::atomic<int> entered{0};
    std::atomic<bool> hold{false}, cancelled{false};
    std::atomic<int> mode{0};  // 0 bitmap 64x32 (padded stride), 1 archive, 2 error, 3 not premultiplied
    std::atomic<uint32_t> purpose{99}, pixels{0};
    std::atomic<DWORD> thread{0};
    static PulseStatus PULSE_CALL Run(void* user, const PulsePreviewRequest* request) {
        auto& self = *static_cast<Provider*>(user);
        ++self.entered;
        self.thread = GetCurrentThreadId();
        self.purpose = request->size >= offsetof(PulsePreviewRequest, purpose) + sizeof(uint32_t) ? request->purpose : 77u;
        self.pixels = request->pixels;
        while (self.hold) { if (request->cancelled(request->context)) self.cancelled = true; Sleep(1); }
        const int mode = self.mode;
        if (mode == 2) return PULSE_ERROR_LOAD;
        PulsePreviewResult result{}; result.size = sizeof(result);
        if (mode == 1) {
            result.kind = PULSE_PREVIEW_ARCHIVE; result.format = View("fixture");
            return request->publish(request->context, &result);
        }
        constexpr uint32_t w = 64, h = 32, stride = w * 4 + 8;
        std::vector<uint8_t> px(static_cast<size_t>(stride) * h, 0);
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = px.data() + static_cast<size_t>(y) * stride + x * 4;
            p[0] = 10; p[1] = 20; p[2] = 30; p[3] = 255;
            if (mode == 3) { p[0] = 200; p[3] = 100; }
        }
        result.kind = PULSE_PREVIEW_BITMAP; result.width = w; result.height = h; result.stride = stride;
        result.pixels = px.data(); result.byte_count = stride * h;
        return request->publish(request->context, &result);
    }
};
PreviewsAdapter::ThumbnailStatus ThumbOn(const wchar_t* path, uint32_t pixels, PreviewsAdapter::ThumbnailBitmap& out,
                                         std::function<bool()> cancelled = {}) {
    PreviewsAdapter::ThumbnailStatus status{};
    std::thread worker([&] { status = PreviewsAdapter::Thumbnail(path, 5, 6, pixels, cancelled, out); });
    worker.join();
    return status;
}
}
int main() {
    using Status = PreviewsAdapter::ThumbnailStatus;
    HWND owner = reinterpret_cast<HWND>(3);
    Provider provider;
    PreviewsAdapter adapter(owner, [] { return true; });
    PulsePreviewV1 api{};
    Check(adapter.Query(1, &api, sizeof(api)) == PULSE_OK, "query");
    PulsePreviewProviderId id = 0;

    PulsePreviewProvider bad{sizeof(bad), View(".thm"), Provider::Run, &provider, 2u};
    Check(api.register_provider(api.context, &bad, &id) == PULSE_ERROR_ARGUMENT && !id, "unknown provider flag rejected");
    PulsePreviewProvider old{static_cast<uint32_t>(offsetof(PulsePreviewProvider, flags)), View(".old"), Provider::Run, &provider, 0xFFu};
    Check(api.register_provider(api.context, &old, &id) == PULSE_OK && id, "V1-size descriptor accepted, flags ignored");
    Check(!PreviewsAdapter::ThumbnailAvailable(L"C:\\a.old") && api.unregister_provider(api.context, id) == PULSE_OK,
        "V1-size provider is preview only");
    PulsePreviewProvider plain{sizeof(plain), View(".pv"), Provider::Run, &provider, 0u};
    PulsePreviewProviderId plain_id = 0;
    Check(api.register_provider(api.context, &plain, &plain_id) == PULSE_OK, "preview-only provider registers");

    PulsePreviewProvider thumb{sizeof(thumb), View(".thm"), Provider::Run, &provider, PULSE_PREVIEW_PROVIDER_THUMBNAIL};
    Check(api.register_provider(api.context, &thumb, &id) == PULSE_OK && id, "thumbnail provider registers");
    Check(PreviewsAdapter::ThumbnailAvailable(L"C:\\x\\Pic.THM") && !PreviewsAdapter::ThumbnailAvailable(L"C:\\x\\a.pv") &&
        !PreviewsAdapter::ThumbnailAvailable(L"pic.thm") && !PreviewsAdapter::ThumbnailAvailable(L"C:\\x.thm\\noext"),
        "availability: flag, case, absolute path, last segment");

    PreviewsAdapter::ThumbnailBitmap out;
    DWORD caller = 0;
    Status st{};
    std::thread worker([&] { caller = GetCurrentThreadId(); st = PreviewsAdapter::Thumbnail(L"C:\\x\\pic.thm", 5, 6, 16, {}, out); });
    worker.join();
    Check(st == Status::Ready && out.width == 16 && out.height == 8 && out.pixels.size() == 16u * 8 * 4,
        "thumbnail scaled down to the wanted edge");
    Check(out.pixels.size() >= 4 && out.pixels[0] == 10 && out.pixels[1] == 20 && out.pixels[2] == 30 && out.pixels[3] == 255,
        "box filter keeps solid colour");
    Check(provider.purpose == PULSE_PREVIEW_PURPOSE_THUMBNAIL && provider.pixels == 16 && provider.thread == caller,
        "provider sees purpose, pixels, and runs on the calling thread");
    Check(ThumbOn(L"C:\\x\\pic.thm", 128, out) == Status::Ready && out.width == 64 && out.height == 32 &&
        out.pixels.size() == 64u * 32 * 4, "no upscale; padded stride compacted");
    Check(ThumbOn(L"C:\\x\\a.pv", 64, out) == Status::NotHandled && ThumbOn(L"C:\\x\\a.none", 64, out) == Status::NotHandled &&
        ThumbOn(L"rel.thm", 64, out) == Status::NotHandled && ThumbOn(L"C:\\x\\pic.thm", 0, out) == Status::NotHandled,
        "no opted-in provider / bad input -> NotHandled");
    for (int mode : {1, 2, 3}) {
        provider.mode = mode;
        Check(ThumbOn(L"C:\\x\\pic.thm", 64, out) == Status::Failed && out.pixels.empty(), "archive / error / invalid bitmap -> Failed");
    }
    provider.mode = 0;
    const int before_cancel = provider.entered;
    Check(ThumbOn(L"C:\\x\\pic.thm", 64, out, [] { return true; }) == Status::Failed && provider.entered == before_cancel,
        "caller cancellation stops before the provider runs");

    PreviewResponse response;
    Check(Wait([&] { response = PreviewsAdapter::Request(owner, L"C:\\x\\pic.thm", 1, 2, 64, 1); return response.status != PreviewStatus::Pending; }) &&
        response.status == PreviewStatus::Ready && provider.purpose == PULSE_PREVIEW_PURPOSE_PREVIEW, "same provider still serves previews with purpose 0");

    // Budget: two calls hold the session's slots, a third reports Busy.
    provider.hold = true;
    const int before_busy = provider.entered;
    Status a{}, b{};
    PreviewsAdapter::ThumbnailBitmap oa, ob;
    std::thread ta([&] { a = PreviewsAdapter::Thumbnail(L"C:\\x\\1.thm", 1, 1, 32, {}, oa); });
    std::thread tb([&] { b = PreviewsAdapter::Thumbnail(L"C:\\x\\2.thm", 1, 1, 32, {}, ob); });
    Check(Wait([&] { return provider.entered == before_busy + 2; }) && adapter.CallbackActive(), "two thumbnail calls run");
    const auto busy_start = GetTickCount64();
    Check(ThumbOn(L"C:\\x\\3.thm", 32, out) == Status::Busy && GetTickCount64() - busy_start >= 1500, "third call waits then reports Busy");
    provider.hold = false; ta.join(); tb.join();
    Check(a == Status::Ready && b == Status::Ready && Wait([&] { return !adapter.CallbackActive(); }), "held calls finish and release slots");

    // Unregister while a thumbnail call runs: STATE until it returns, then OK.
    provider.hold = true; provider.cancelled = false;
    const int before_retire = provider.entered;
    Status r{};
    PreviewsAdapter::ThumbnailBitmap orr;
    std::thread tr([&] { r = PreviewsAdapter::Thumbnail(L"C:\\x\\r.thm", 1, 1, 32, {}, orr); });
    Check(Wait([&] { return provider.entered > before_retire; }), "retire test enters provider");
    Check(api.unregister_provider(api.context, id) == PULSE_ERROR_STATE && !PreviewsAdapter::ThumbnailAvailable(L"C:\\x\\r.thm") &&
        Wait([&] { return provider.cancelled.load(); }), "unregister retires, cancels, reports STATE while running");
    Check(ThumbOn(L"C:\\x\\r.thm", 32, out) == Status::NotHandled, "retired provider takes no new thumbnails");
    provider.hold = false; tr.join();
    Check(r == Status::Failed && api.unregister_provider(api.context, id) == PULSE_OK, "drained call discarded; retry unregister OK");

    // Close while running keeps the unload gate until the provider returns.
    Check(api.register_provider(api.context, &thumb, &id) == PULSE_OK, "re-register for close test");
    provider.hold = true; provider.cancelled = false;
    const int before_close = provider.entered;
    Status c{};
    PreviewsAdapter::ThumbnailBitmap oc;
    std::thread tc([&] { c = PreviewsAdapter::Thumbnail(L"C:\\x\\c.thm", 1, 1, 32, {}, oc); });
    Check(Wait([&] { return provider.entered > before_close; }), "close test enters provider");
    adapter.Close();
    Check(adapter.CallbackActive() && Wait([&] { return provider.cancelled.load(); }) &&
        !PreviewsAdapter::ThumbnailAvailable(L"C:\\x\\c.thm"), "close cancels and holds the unload gate");
    provider.hold = false; tc.join();
    Check(c == Status::Failed && Wait([&] { return !adapter.CallbackActive(); }) &&
        ThumbOn(L"C:\\x\\c.thm", 32, out) == Status::NotHandled, "gate released; closed session serves nothing");

    // SDK wrapper: PreviewClient + PreviewContext over a fresh session.
    PreviewsAdapter fresh(owner, [] { return true; });
    PulsePreviewV1 api2{};
    fresh.Query(1, &api2, sizeof(api2));
    pulse::sdk::PreviewClient client(api2);
    std::atomic<bool> saw_thumbnail{false}, saw_path{false};
    {
        pulse::sdk::PreviewClient::Provider registration;
        const auto status = client.Register(".sdk", [&](const pulse::sdk::PreviewContext& request) -> PulseStatus {
            saw_thumbnail = request.Thumbnail();
            saw_path = request.Path() == "C:\\x\\w.sdk";
            if (request.Cancelled()) return PULSE_ERROR_STATE;
            std::vector<uint8_t> px(8 * 4 * 4);
            for (size_t i = 0; i < px.size(); i += 4) { px[i] = 1; px[i + 1] = 2; px[i + 2] = 3; px[i + 3] = 4; }
            return request.PublishBitmap(8, 4, px.data());
        }, PULSE_PREVIEW_PROVIDER_THUMBNAIL, registration);
        Check(status == PULSE_OK && registration.Active(), "PreviewClient::Register with thumbnail flag");
        Check(ThumbOn(L"C:\\x\\w.sdk", 64, out) == Status::Ready && out.width == 8 && out.height == 4 && out.pixels[3] == 4 &&
            saw_thumbnail && saw_path, "PreviewContext: Thumbnail(), Path(), PublishBitmap");
        Check(Wait([&] { response = PreviewsAdapter::Request(owner, L"C:\\x\\w.sdk", 1, 2, 64, 2); return response.status != PreviewStatus::Pending; }) &&
            response.status == PreviewStatus::Ready && !saw_thumbnail, "same wrapper serves the preview pane (Thumbnail() false)");
        pulse::sdk::PreviewClient::Provider archive;
        Check(client.Register(".sdka", [](const pulse::sdk::PreviewContext& request) {
            return request.PublishArchive("fixture", {{"a/b.txt", 3, 2, 0, false, true}, {"a", 0, 0, 0, true, false}});
        }, 0, archive) == PULSE_OK, "archive provider via wrapper");
        Check(Wait([&] { response = PreviewsAdapter::Request(owner, L"C:\\x\\w.sdka", 1, 2, 64, 3); return response.status != PreviewStatus::Pending; }) &&
            response.status == PreviewStatus::Ready && response.result->entries.size() == 2 && response.result->entries[0].encrypted &&
            response.result->entries[1].directory, "PublishArchive rows and flags");
        pulse::sdk::PreviewClient::Provider dup;
        Check(client.Register(".sdk", [](const pulse::sdk::PreviewContext&) { return PULSE_OK; }, 0, dup) == PULSE_ERROR_STATE &&
            !dup.Active(), "duplicate extension refused by host, wrapper inactive");
        Check(client.Register(".x", nullptr, 0, dup) == PULSE_ERROR_ARGUMENT, "empty fn rejected");
    }
    Check(!PreviewsAdapter::ThumbnailAvailable(L"C:\\x\\w.sdk") && PreviewsAdapter::Request(owner, L"C:\\x\\w.sdka", 1, 2, 64, 4).status ==
        PreviewStatus::Unavailable, "Provider destructor unregisters");
    std::printf("%d PASS, %d FAIL\n", passes, failures);
    return failures ? 1 : 0;
}

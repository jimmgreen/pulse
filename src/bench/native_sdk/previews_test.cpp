#include "../../app/extensions/native/previews_adapter.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
using namespace pulse::extensions::native;
namespace {
int failures = 0, passes = 0;
void Check(bool value, const char* label) { std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label); value ? ++passes : ++failures; }
PulseStringView View(const char* text) { return {text, static_cast<uint32_t>(std::strlen(text))}; }
struct Provider {
    std::atomic<int> entered{0}, returned{0};
    std::atomic<bool> hold{false}, cancelled{false};
    std::atomic<int> mode{0};
    static PulseStatus PULSE_CALL Run(void* user, const PulsePreviewRequest* request) {
        auto& self = *static_cast<Provider*>(user); ++self.entered;
        while (self.hold) { if (request->cancelled(request->context)) self.cancelled = true; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        PulseStatus status = PULSE_OK;
        const int mode = self.mode;
        if (mode == 2) status = PULSE_ERROR_LOAD;
        else if (mode != 3) {
            PulsePreviewResult result{}; result.size = sizeof(result);
            if (mode >= 12) {
                char text[] = "Preview model";
                PulseUiModelNode root{}; root.size = sizeof(root); root.type = PULSE_UI_MODEL_BUTTON;
                root.id = View("preview-button"); root.text = {1, View(text)}; root.command = {1, View("update")};
                PulseUiState state{View("message"), View("before")};
                PulseUiState update{View("message"), View("after")};
                PulseUiModelAction action{}; action.size = sizeof(action); action.id = View("update");
                action.updates = &update; action.update_count = 1;
                PulseUiModelTree tree{}; tree.size = sizeof(tree); tree.root = &root;
                tree.states = &state; tree.state_count = 1; tree.actions = &action; tree.action_count = 1;
                const auto command = View("native-callback");
                if (mode == 13) root.type = PULSE_UI_MODEL_COUNT;
                if (mode == 14) { tree.commands = &command; tree.command_count = 1; }
                result.kind = PULSE_PREVIEW_UI_MODEL; result.model = &tree;
                status = request->publish(request->context, &result); text[0] = 'X';
            } else if (mode == 1 || mode == 5) {
                uint8_t pixels[]{1,2,3,255};
                result.kind = PULSE_PREVIEW_BITMAP; result.width = 1; result.height = 1; result.stride = 4;
                result.pixels = pixels; result.byte_count = mode == 5 ? 3 : 4;
                status = request->publish(request->context, &result); pixels[0] = 200;
            } else {
                char name[] = "folder/file.txt";
                PulsePreviewArchiveEntry row{View(name), 12, 7, 0, PULSE_PREVIEW_ENTRY_ENCRYPTED};
                result.kind = PULSE_PREVIEW_ARCHIVE; result.format = View("fixture"); result.incomplete = 1;
                result.entries = &row; result.entry_count = mode == 4 ? 0u : mode == 6 ? 4097u : 1u;
                const std::string long_format(128, 'F');
                if (mode == 7) result.format = View("");
                if (mode == 8) result.format = View("bad\tformat");
                if (mode == 9) result.format = View("bad\nformat");
                if (mode == 10) result.format = View("bad\x7f");
                if (mode == 11) result.format = View(long_format.c_str());
                status = request->publish(request->context, &result); name[0] = 'X';
            }
        }
        ++self.returned; return status;
    }
};
bool Wait(const std::function<bool()>& ready) {
    auto end = GetTickCount64() + 5000;
    do { if (ready()) return true; Sleep(1); } while (GetTickCount64() < end);
    return false;
}
}
int main() {
    HWND owner = reinterpret_cast<HWND>(1);
    Provider provider;
    PreviewsAdapter adapter(owner, [] { return true; });
    PulsePreviewV1 api{};
    Check(adapter.Query(1, &api, sizeof(api)) == PULSE_OK && adapter.Query(2, &api, sizeof(api)) == PULSE_ERROR_ABI, "query validates ABI");
    PulsePreviewProvider descriptor{sizeof(descriptor), View(".zip"), Provider::Run, &provider};
    PulsePreviewProviderId id = 0, duplicate = 0;
    Check(api.register_provider(api.context, &descriptor, &id) == PULSE_OK && id, "provider registration");
    Check(api.register_provider(api.context, &descriptor, &duplicate) == PULSE_ERROR_STATE && !duplicate, "same owner duplicate extension rejected");
    auto invalid_descriptor = descriptor; invalid_descriptor.extension = View("ZIP");
    Check(api.register_provider(api.context, &invalid_descriptor, &duplicate) == PULSE_ERROR_ARGUMENT && !duplicate &&
        api.register_provider(api.context, nullptr, &duplicate) == PULSE_ERROR_ARGUMENT, "invalid extension and missing descriptor rejected");
    Check(PreviewsAdapter::Available(owner, L"C:\\a.ZIP") && !PreviewsAdapter::Available(reinterpret_cast<HWND>(2), L"C:\\a.zip"), "availability respects owner and case");
    PulseStatus wrong = PULSE_OK; PreviewResponse wrong_request;
    std::thread other([&] { wrong = api.unregister_provider(api.context, id); wrong_request = PreviewsAdapter::Request(owner, L"C:\\a.zip", 0, 0, 64, 1); }); other.join();
    Check(wrong == PULSE_ERROR_STATE && wrong_request.status == PreviewStatus::Unavailable, "wrong thread cannot reach owner registry");
    auto request = [&](uint64_t surface, const wchar_t* path = L"C:\\a.zip", uint64_t modified = 1, uint64_t size = 2, uint32_t pixels = 64) {
        return PreviewsAdapter::Request(owner, path, modified, size, pixels, surface);
    };
    PreviewResponse response;
    Check(Wait([&] { response = request(1); return response.status != PreviewStatus::Pending; }) && response.status == PreviewStatus::Ready &&
        response.result->entries[0].path == L"folder/file.txt" && response.result->entries[0].encrypted && response.result->archive_text &&
        response.result->archive_text->find(L"PULSEARC") == 0, "archive payload copied and serialized on worker");
    const auto initial = provider.entered.load(); request(1);
    Check(provider.entered == initial, "complete file identity caches result");
    Check(Wait([&] { return request(1, L"C:\\a.zip", 2, 2, 64).status == PreviewStatus::Ready; }) && provider.entered == initial + 1, "modified identity invalidates cache");
    Check(Wait([&] { return request(1, L"C:\\a.zip", 2, 3, 128).status == PreviewStatus::Ready; }) && provider.entered == initial + 2, "size and requested pixels invalidate cache");
    provider.hold = true; provider.cancelled = false;
    const auto before_hold = provider.entered.load();
    request(2); Check(Wait([&] { return provider.entered > before_hold; }), "worker unload gate becomes active");
    const auto before = provider.entered.load();
    request(2, L"C:\\b.zip"); request(2, L"C:\\c.zip");
    Check(Wait([&] { return provider.cancelled.load(); }) && provider.entered <= before + 1, "surface replacement cancels stale workers and bounds concurrency");
    provider.hold = false;
    Check(Wait([&] { response = request(2, L"C:\\c.zip"); return response.status == PreviewStatus::Ready; }), "latest deferred surface completes after capacity frees");
    provider.hold = true; provider.cancelled = false; const auto before_cancel = provider.entered.load(); request(3);
    Check(Wait([&] { return provider.entered > before_cancel; }), "cancel test worker starts");
    Check(request(3, L"C:\\unsupported.txt").status == PreviewStatus::Unavailable && Wait([&] { return provider.cancelled.load(); }), "unsupported replacement cancels previous job");
    provider.hold = false; Check(Wait([&] { return !adapter.CallbackActive(); }), "cancelled worker fully returns");
    for (int mode : {1,2,3,4,5,6,7,8,9,10,11,12,13,14}) {
        provider.mode = mode; PreviewsAdapter::Cancel(owner, 4);
        Check(Wait([&] { response = request(4); return response.status != PreviewStatus::Pending; }), "provider completion is observable");
        if (mode == 1) Check(response.status == PreviewStatus::Ready && response.result->pixels[0] == 1, "bitmap bytes copied");
        else if (mode == 4) Check(response.status == PreviewStatus::Ready && response.result->entries.empty(), "empty archive succeeds");
        else if (mode == 11) Check(response.status == PreviewStatus::Ready && response.result->format.size() == 128, "128-byte valid format accepted");
        else if (mode == 12) Check(response.status == PreviewStatus::Ready && response.result->model &&
            response.result->model->root.text == "Preview model" && response.result->model->actions.size() == 1 &&
            response.result->model->actions[0].updates[0].value == "after", "UI model copied with parsed local state action");
        else if (mode == 14) Check(response.status == PreviewStatus::Failed && response.error == PULSE_ERROR_UNAVAILABLE,
            "UI model native callbacks rejected without publishing");
        else Check(response.status == PreviewStatus::Failed, "provider error, absent output or invalid payload fails");
    }
    Check(request(5, L"relative.zip").error == PULSE_ERROR_ARGUMENT && request(5, L"C:\\a.zip", 1, 2, 4097).error == PULSE_ERROR_ARGUMENT,
        "path and pixel input limits checked");
    Check(request(5, L"\\\\server\\a.zip").error == PULSE_ERROR_ARGUMENT &&
        request(5, L"\\\\.\\device.zip").error == PULSE_ERROR_ARGUMENT,
        "malformed UNC and device namespace paths rejected");
    provider.mode = 0; provider.hold = true; provider.cancelled = false; const auto before_close = provider.entered.load(); request(6);
    Check(Wait([&] { return provider.entered > before_close; }), "close test worker active");
    adapter.Close();
    Check(adapter.CallbackActive() && !PreviewsAdapter::Available(owner, L"C:\\a.zip") && Wait([&] { return provider.cancelled.load(); }), "close immediately cancels but retains unload gate until return");
    provider.hold = false;
    Check(Wait([&] { return !adapter.CallbackActive(); }), "worker return releases unload gate");
    Provider second; PreviewsAdapter fresh(owner, [] { return true; }); fresh.Query(1, &api, sizeof(api)); descriptor.user = &second;
    Check(api.register_provider(api.context, &descriptor, &id) == PULSE_OK, "closed provider no longer reserves extension");
    Check(Wait([&] { return request(7).status == PreviewStatus::Ready; }), "replacement provider can serve owner");
    Check(api.unregister_provider(api.context, id) == PULSE_OK && request(7).status == PreviewStatus::Unavailable, "unregister invalidates cached result");
    Check(api.register_provider(api.context, &descriptor, &id) == PULSE_OK, "provider registers for draining test");
    second.hold = true;
    const auto second_before = second.entered.load();
    request(8); request(9); request(10);
    Check(Wait([&] { return second.entered == second_before + 2; }), "two workers run while third surface waits");
    second.hold = false;
    Check(Wait([&] { fresh.Poll(); return second.entered == second_before + 3 && !fresh.CallbackActive(); }),
        "UI poll starts deferred surface without another request");
    second.hold = true; second.cancelled = false;
    const auto unregister_before = second.entered.load(); request(11);
    Check(Wait([&] { return second.entered > unregister_before; }), "unregister test enters callback");
    Check(api.unregister_provider(api.context, id) == PULSE_ERROR_STATE && !PreviewsAdapter::Available(owner, L"C:\\a.zip") &&
        Wait([&] { return second.cancelled.load(); }), "unregister retires and cancels but requires userdata until callback drains");
    second.hold = false;
    Check(Wait([&] { return !fresh.CallbackActive(); }) && api.unregister_provider(api.context, id) == PULSE_OK,
        "unregister retry succeeds only after provider return");
    std::printf("%d PASS, %d FAIL\n", passes, failures); return failures ? 1 : 0;
}

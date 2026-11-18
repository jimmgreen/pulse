#include "../shell_bridge/legacy_adapter.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace {
using namespace pulse::shell_bridge;
namespace picker = pulse::ipc::picker;
int failures = 0;
thread_local unsigned native_calls = 0, error_queries = 0;
thread_local DWORD native_error = 0x1234;
thread_local BOOL native_result = 7;
void Check(bool good, const char* text) { std::printf("[%s] %s\n", good ? "PASS" : "FAIL", text); if (!good) ++failures; }
BOOL WINAPI NativeOpen(OPENFILENAMEW*) { ++native_calls; return native_result; }
BOOL WINAPI NativeSave(OPENFILENAMEW*) { ++native_calls; return native_result; }
DWORD WINAPI NativeError() { ++error_queries; return native_error; }
struct Fixture {
    std::array<wchar_t, 512> file{};
    std::array<wchar_t, 128> title{};
    OPENFILENAMEW ofn{};
    Fixture() {
        file.fill(L'!'); file[0] = 0;
        title.fill(L'?'); title[0] = 0;
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile = file.data(); ofn.nMaxFile = static_cast<DWORD>(file.size());
        ofn.lpstrFileTitle = title.data(); ofn.nMaxFileTitle = static_cast<DWORD>(title.size());
        ofn.lpstrFilter = L"Text\0*.txt\0All\0*.*\0\0";
        ofn.nFilterIndex = 1; ofn.nFileOffset = 91; ofn.nFileExtension = 92;
        ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT | OFN_FILEMUSTEXIST;
    }
};
LegacyDialogServices Services(bool enabled) {
    LegacyDialogServices value;
    value.open_native = NativeOpen; value.save_native = NativeSave; value.native_error = NativeError;
    if (enabled) value.verified_abi_checks = [](const auto& prepared) { return prepared.required_abi_checks; };
    return value;
}
picker::Message Reply(const picker::Message& request, picker::Status status, std::vector<std::u16string> paths = {}) {
    picker::Response body;
    body.operation = std::get<picker::Request>(request.body).operation;
    body.status = status;
    body.paths = std::move(paths);
    if (status == picker::Status::Failed) { body.error_source = picker::ErrorSource::Win32; body.error_code = ERROR_ACCESS_DENIED; }
    if (status == picker::Status::Unsupported) { body.error_source = picker::ErrorSource::Win32; body.error_code = ERROR_NOT_SUPPORTED; }
    return {request.request_id, body};
}
}
int main() {
    const auto path = std::filesystem::absolute(std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        L"bench_data" / L"LegacyAdapterFixture" / L"example.txt").wstring();
    const std::u16string wire(path.begin(), path.end());
    // No filesystem writes and no native UI: injected functions expose exact routing/ABI effects.
    Fixture initial;
    auto defaults = Services(false);
    unsigned bridge_calls = 0;
    defaults.run_bridge = [&](const auto& request, HWND) { ++bridge_calls; return Reply(request, picker::Status::Cancelled); };
    LegacyFileDialogAdapter native(defaults);
    Check(native.Open(&initial.ofn) == 7 && native_calls == 1 && bridge_calls == 0 && error_queries == 0,
        "default closed gate preserves native BOOL without querying errors");
    Check(native.ExtendedError() == 0x1234 && error_queries == 1, "native error query delegates on demand");
    native_error = 0x5678;
    Check(native.ExtendedError() == 0x5678, "native error is not cached");

    auto accepted = Services(true);
    accepted.run_bridge = [&](const auto& request, HWND) { ++bridge_calls; return Reply(request, picker::Status::Accepted, {wire}); };
    LegacyFileDialogAdapter adapter(accepted);
    Fixture one;
    Check(adapter.Open(&one.ofn) == TRUE && std::wstring(one.file.data()) == path &&
        std::wstring(one.title.data()) == L"example.txt", "accepted file and title are committed");
    Check(one.ofn.nFileOffset == path.find_last_of(L'\\') + 1 && one.ofn.nFileExtension == path.find_last_of(L'.') + 1 &&
        one.ofn.nFilterIndex == 1 && one.file[path.size() + 1] == L'!', "offsets index and untouched tail match contract");
    Check(adapter.ExtendedError() == 0, "bridge success sets zero extended error");
    auto indexed = accepted;
    indexed.run_bridge = [&](const auto& request, HWND) {
        auto result = Reply(request, picker::Status::Accepted, {wire});
        std::get<picker::Response>(result.body).filter_index = 1; return result;
    };
    LegacyFileDialogAdapter indexed_adapter(indexed);
    Fixture index;
    Check(indexed_adapter.Open(&index.ofn) && index.ofn.nFilterIndex == 2, "returned zero-based filter becomes native one-based index");

    Fixture small;
    small.ofn.nMaxFile = 4;
    const auto before_small = small.file;
    Check(adapter.Open(&small.ofn) == FALSE && adapter.ExtendedError() == FNERR_BUFFERTOOSMALL &&
        small.file[0] == static_cast<wchar_t>(path.size() + 1) && small.file[1] == before_small[1] &&
        small.ofn.nFileOffset == 91 && small.title[1] == L'?', "short file buffer writes exact first WORD only");
    Fixture short_title; short_title.ofn.nMaxFileTitle = 2;
    const auto title_file_before = short_title.file;
    Check(!adapter.Open(&short_title.ofn) && adapter.ExtendedError() == CDERR_INITIALIZATION &&
        short_title.file == title_file_before && short_title.ofn.nFileOffset == 91,
        "short title buffer fails atomically without partial file commit");

    auto cancelled = Services(true);
    cancelled.run_bridge = [&](const auto& request, HWND) { return Reply(request, picker::Status::Cancelled); };
    LegacyFileDialogAdapter cancel(cancelled);
    Fixture untouched;
    const auto before = untouched.file;
    const unsigned calls_before_cancel = native_calls;
    Check(cancel.Open(&untouched.ofn) == FALSE && cancel.ExtendedError() == 0 && untouched.file == before &&
        untouched.ofn.nFileOffset == 91 && native_calls == calls_before_cancel, "cancel preserves outputs and never reopens native");
    auto unsupported = cancelled;
    bool unsupported_valid = false;
    unsupported.run_bridge = [&](const auto& request, HWND) {
        auto response = Reply(request, picker::Status::Unsupported);
        unsupported_valid = picker::ValidateResponse(request, response);
        return response;
    };
    LegacyFileDialogAdapter unsupported_adapter(unsupported);
    const auto title_before_unsupported = untouched.title;
    Check(!unsupported_adapter.Open(&untouched.ofn) && unsupported_adapter.ExtendedError() == CDERR_DIALOGFAILURE &&
        unsupported_valid && native_calls == calls_before_cancel && untouched.file == before &&
        untouched.title == title_before_unsupported && untouched.ofn.nFileOffset == 91 && untouched.ofn.nFileExtension == 92,
        "post-dispatch Unsupported fails without native replay or output mutation");
    auto failed = cancelled;
    failed.run_bridge = [&](const auto& request, HWND) { return Reply(request, picker::Status::Failed); };
    LegacyFileDialogAdapter failure(failed);
    Check(!failure.Open(&untouched.ofn) && failure.ExtendedError() == CDERR_DIALOGFAILURE && untouched.file == before,
        "bridge failure preserves caller buffers");
    auto malformed = accepted;
    malformed.run_bridge = [&](const auto& request, HWND) {
        auto result = Reply(request, picker::Status::Accepted, {wire}); result.request_id[0] ^= 1; return result;
    };
    LegacyFileDialogAdapter invalid(malformed);
    Check(!invalid.Open(&untouched.ofn) && invalid.ExtendedError() == FNERR_INVALIDFILENAME && untouched.file == before,
        "unbound response cannot commit output");

    const unsigned bypass_start = native_calls;
    Fixture old; old.ofn.lStructSize = OPENFILENAME_SIZE_VERSION_400W;
    Check(adapter.Open(&old.ofn) == 7, "old structure size bypasses before bridge");
    Fixture hook; hook.ofn.Flags |= OFN_ENABLEHOOK;
    Check(adapter.Open(&hook.ofn) == 7, "hook flag bypasses before bridge");
    Fixture custom; custom.ofn.lpTemplateName = MAKEINTRESOURCEW(100);
    Check(adapter.Open(&custom.ofn) == 7, "template resource is not dereferenced and bypasses");
    Fixture flags; flags.ofn.Flags |= OFN_NOVALIDATE;
    Check(adapter.Open(&flags.ofn) == 7 && native_calls == bypass_start + 4, "unsupported flag bypasses before bridge");
    auto partial = accepted;
    partial.verified_abi_checks = [](const auto&) { return uint32_t(picker::legacy::CallerDirectoryPolicy); };
    LegacyFileDialogAdapter gated(partial);
    Check(gated.Open(&untouched.ofn) == 7, "partial ABI evidence is insufficient");

    Fixture multi; multi.ofn.Flags |= OFN_ALLOWMULTISELECT; multi.ofn.lpstrFileTitle = nullptr;
    auto many = accepted;
    const auto second = wire.substr(0, wire.find_last_of(u'\\') + 1) + u"other.txt";
    many.run_bridge = [&](const auto& request, HWND) { return Reply(request, picker::Status::Accepted, {wire, second}); };
    LegacyFileDialogAdapter multi_adapter(many);
    Check(multi_adapter.Open(&multi.ofn) == TRUE && multi.ofn.nFileExtension == 0 &&
        std::wstring(multi.file.data() + multi.ofn.nFileOffset) == L"example.txt" &&
        std::wstring(multi.file.data() + multi.ofn.nFileOffset + 12) == L"other.txt", "multi-select emits directory and ordered leaves");
    Fixture save;
    save.ofn.Flags &= ~OFN_FILEMUSTEXIST;
    save.ofn.Flags |= OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOTESTFILECREATE;
    save.ofn.lpstrDefExt = L"log";
    Check(adapter.Save(&save.ofn) == TRUE && (save.ofn.Flags & OFN_EXTENSIONDIFFERENT), "Save commits extension-different flag");
    Check(!failure.Open(&untouched.ofn) && failure.ExtendedError() == CDERR_DIALOGFAILURE, "main thread has bridge error");
    bool thread_ok = false;
    std::thread worker([&] {
        LegacyFileDialogAdapter thread_adapter(Services(false));
        thread_ok = thread_adapter.ExtendedError() == 0x1234;
        LegacyFileDialogAdapter thread_cancel(cancelled);
        Fixture local; thread_ok = thread_ok && !thread_cancel.Open(&local.ofn) && thread_cancel.ExtendedError() == 0;
    });
    worker.join();
    Check(thread_ok && failure.ExtendedError() == CDERR_DIALOGFAILURE, "extended errors are thread-local");
    const unsigned queried = error_queries;
    LegacyFileDialogAdapter::UseNativeError();
    Check(error_queries == queried && failure.ExtendedError() == 0x5678, "explicit external-common-dialog handoff restores native errors lazily");
    Check(!cancel.Open(&untouched.ofn), "bridge cancel before native transition");
    native_result = FALSE; native_error = CDERR_INITIALIZATION;
    Check(!native.Save(&untouched.ofn) && native.ExtendedError() == CDERR_INITIALIZATION,
        "native bypass replaces prior bridge error source");
    return failures ? 1 : 0;
}

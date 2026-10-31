#include "../shell_bridge/legacy_ansi_adapter.h"
#include "../shell_bridge/legacy_adapter.h"
#include <array>
#include <cstdio>
#include <algorithm>
#include <thread>
#include <cstring>

namespace {
using namespace pulse::shell_bridge;
namespace picker = pulse::ipc::picker;
int failures = 0, checks = 0;
thread_local unsigned native_calls = 0, error_queries = 0;
thread_local DWORD native_error = 0x1234;
thread_local BOOL native_result = 7;
void Check(bool good, const char* text) { ++checks; std::printf("[%s] %s\n", good ? "PASS" : "FAIL", text); if (!good) ++failures; }
BOOL WINAPI NativeOpen(OPENFILENAMEA*) { ++native_calls; return native_result; }
BOOL WINAPI NativeSave(OPENFILENAMEA*) { ++native_calls; return native_result; }
DWORD WINAPI NativeError() { ++error_queries; return native_error; }
BOOL WINAPI NativeWide(OPENFILENAMEW*) { ++native_calls; return native_result; }
DWORD WINAPI WideNativeError() { ++error_queries; return native_error + 1; }
struct Fixture {
    std::array<char, 512> file{};
    std::array<char, 128> title{};
    OPENFILENAMEA ofn{};
    Fixture() {
        file.fill('!'); file[0] = 0;
        title.fill('?'); title[0] = 0;
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile = file.data(); ofn.nMaxFile = static_cast<DWORD>(file.size());
        ofn.lpstrFileTitle = title.data(); ofn.nMaxFileTitle = static_cast<DWORD>(title.size());
        ofn.lpstrFilter = "Text\0*.txt\0All\0*.*\0\0";
        ofn.nFilterIndex = 1; ofn.nFileOffset = 91; ofn.nFileExtension = 92;
        ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT | OFN_FILEMUSTEXIST;
    }
};
LegacyAnsiDialogServices Services(bool enabled) {
    LegacyAnsiDialogServices value;
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
    std::printf("ACP: %u\n", GetACP());
#ifdef PULSE_TEST_REQUIRE_UTF8_ACP
    if (GetACP() != CP_UTF8) {
        std::printf("[UNSUPPORTED] UTF-8 process ACP requires Windows 10 1903+ and an active UTF-8 manifest.\nChecks: 0, failures: 0; NOT PASSED\n");
        return 2;
    }
#endif
    const std::string path = "C:\\bench_data\\LegacyAnsiAdapterFixture\\example.txt";
    const std::u16string wire(path.begin(), path.end());
    // No filesystem writes and no native UI: injected functions expose exact routing/ABI effects.
    Fixture initial;
    auto defaults = Services(false);
    unsigned bridge_calls = 0;
    defaults.run_bridge = [&](const auto& request, HWND) { ++bridge_calls; return Reply(request, picker::Status::Cancelled); };
    LegacyAnsiFileDialogAdapter native(defaults);
    Check(native.Open(&initial.ofn) == 7 && native_calls == 1 && bridge_calls == 0 && error_queries == 0,
        "default closed gate preserves native BOOL without querying errors");
    Check(native.ExtendedError() == 0x1234 && error_queries == 1, "native error query delegates on demand");
    native_error = 0x5678;
    Check(native.ExtendedError() == 0x5678, "native error is not cached");

    auto accepted = Services(true);
    accepted.run_bridge = [&](const auto& request, HWND) { ++bridge_calls; return Reply(request, picker::Status::Accepted, {wire}); };
    LegacyAnsiFileDialogAdapter adapter(accepted);
    Fixture one;
    Check(adapter.Open(&one.ofn) == TRUE && std::string(one.file.data()) == path &&
        std::string(one.title.data()) == "example.txt", "accepted file and title are committed");
    Check(static_cast<size_t>(one.ofn.nFileOffset) == path.find_last_of('\\') + 1 && static_cast<size_t>(one.ofn.nFileExtension) == path.find_last_of('.') + 1 &&
        one.ofn.nFilterIndex == 1 && one.file[path.size() + 1] == '!', "offsets index and untouched tail match contract");
    Check(adapter.ExtendedError() == 0, "bridge success sets zero extended error");
    auto indexed = accepted;
    indexed.run_bridge = [&](const auto& request, HWND) {
        auto result = Reply(request, picker::Status::Accepted, {wire});
        std::get<picker::Response>(result.body).filter_index = 1; return result;
    };
    LegacyAnsiFileDialogAdapter indexed_adapter(indexed);
    Fixture index;
    Check(indexed_adapter.Open(&index.ofn) && index.ofn.nFilterIndex == 2, "returned zero-based filter becomes native one-based index");

    Fixture small;
    small.ofn.nMaxFile = 4;
    const auto before_small = small.file;
    Check(adapter.Open(&small.ofn) == FALSE && adapter.ExtendedError() == FNERR_BUFFERTOOSMALL &&
        static_cast<size_t>(static_cast<unsigned char>(small.file[0])) == path.size() + 1 && small.file[1] == 0 && small.file[2] == before_small[2] &&
        small.ofn.nFileOffset == 91 && small.title[1] == '?', "short file buffer writes exact first WORD only");
    Fixture short_title; short_title.ofn.nMaxFileTitle = 2;
    const auto title_file_before = short_title.file;
    Check(!adapter.Open(&short_title.ofn) && adapter.ExtendedError() == CDERR_INITIALIZATION &&
        short_title.file == title_file_before && short_title.ofn.nFileOffset == 91,
        "short title buffer fails atomically without partial file commit");

    auto cancelled = Services(true);
    cancelled.run_bridge = [&](const auto& request, HWND) { return Reply(request, picker::Status::Cancelled); };
    LegacyAnsiFileDialogAdapter cancel(cancelled);
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
    LegacyAnsiFileDialogAdapter unsupported_adapter(unsupported);
    const auto title_before_unsupported = untouched.title;
    Check(!unsupported_adapter.Open(&untouched.ofn) && unsupported_adapter.ExtendedError() == CDERR_DIALOGFAILURE &&
        unsupported_valid && native_calls == calls_before_cancel && untouched.file == before &&
        untouched.title == title_before_unsupported && untouched.ofn.nFileOffset == 91 && untouched.ofn.nFileExtension == 92,
        "post-dispatch Unsupported fails without native replay or output mutation");
    auto failed = cancelled;
    failed.run_bridge = [&](const auto& request, HWND) { return Reply(request, picker::Status::Failed); };
    LegacyAnsiFileDialogAdapter failure(failed);
    Check(!failure.Open(&untouched.ofn) && failure.ExtendedError() == CDERR_DIALOGFAILURE && untouched.file == before,
        "bridge failure preserves caller buffers");
    auto malformed = accepted;
    malformed.run_bridge = [&](const auto& request, HWND) {
        auto result = Reply(request, picker::Status::Accepted, {wire}); result.request_id[0] ^= 1; return result;
    };
    LegacyAnsiFileDialogAdapter invalid(malformed);
    Check(!invalid.Open(&untouched.ofn) && invalid.ExtendedError() == FNERR_INVALIDFILENAME && untouched.file == before,
        "unbound response cannot commit output");

    const unsigned bypass_start = native_calls;
    Fixture old; old.ofn.lStructSize = OPENFILENAME_SIZE_VERSION_400A;
    Check(adapter.Open(&old.ofn) == 7, "old structure size bypasses before bridge");
    Fixture hook; hook.ofn.Flags |= OFN_ENABLEHOOK;
    Check(adapter.Open(&hook.ofn) == 7, "hook flag bypasses before bridge");
    Fixture custom; custom.ofn.lpTemplateName = MAKEINTRESOURCEA(100);
    Check(adapter.Open(&custom.ofn) == 7, "template resource is not dereferenced and bypasses");
    Fixture flags; flags.ofn.Flags |= OFN_NOVALIDATE;
    Check(adapter.Open(&flags.ofn) == 7 && native_calls == bypass_start + 4, "unsupported flag bypasses before bridge");
    auto partial = accepted;
    partial.verified_abi_checks = [](const auto&) { return uint32_t(picker::legacy::CallerDirectoryPolicy); };
    LegacyAnsiFileDialogAdapter gated(partial);
    Check(gated.Open(&untouched.ofn) == 7, "partial ABI evidence is insufficient");

    Fixture multi; multi.ofn.Flags |= OFN_ALLOWMULTISELECT; multi.ofn.lpstrFileTitle = nullptr;
    auto many = accepted;
    const auto second = wire.substr(0, wire.find_last_of(u'\\') + 1) + u"other.txt";
    many.run_bridge = [&](const auto& request, HWND) { return Reply(request, picker::Status::Accepted, {wire, second}); };
    LegacyAnsiFileDialogAdapter multi_adapter(many);
    Check(multi_adapter.Open(&multi.ofn) == TRUE && multi.ofn.nFileExtension == 0 &&
        std::string(multi.file.data() + multi.ofn.nFileOffset) == "example.txt" &&
        std::string(multi.file.data() + multi.ofn.nFileOffset + 12) == "other.txt", "multi-select emits directory and ordered leaves");
    Fixture save;
    save.ofn.Flags &= ~OFN_FILEMUSTEXIST;
    save.ofn.Flags |= OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOTESTFILECREATE;
    save.ofn.lpstrDefExt = "log";
    Check(adapter.Save(&save.ofn) == TRUE && (save.ofn.Flags & OFN_EXTENSIONDIFFERENT), "Save commits extension-different flag");
    Check(!failure.Open(&untouched.ofn) && failure.ExtendedError() == CDERR_DIALOGFAILURE, "main thread has bridge error");
    bool thread_ok = false;
    std::thread worker([&] {
        LegacyAnsiFileDialogAdapter thread_adapter(Services(false));
        thread_ok = thread_adapter.ExtendedError() == 0x1234;
        LegacyAnsiFileDialogAdapter thread_cancel(cancelled);
        Fixture local; thread_ok = thread_ok && !thread_cancel.Open(&local.ofn) && thread_cancel.ExtendedError() == 0;
    });
    worker.join();
    Check(thread_ok && failure.ExtendedError() == CDERR_DIALOGFAILURE, "extended errors are thread-local");
    const unsigned queried = error_queries;
    LegacyAnsiFileDialogAdapter::UseNativeError();
    Check(error_queries == queried && failure.ExtendedError() == 0x5678, "explicit external-common-dialog handoff restores native errors lazily");
    Check(!cancel.Open(&untouched.ofn), "bridge cancel before native transition");
    native_result = FALSE; native_error = CDERR_INITIALIZATION;
    Check(!native.Save(&untouched.ofn) && native.ExtendedError() == CDERR_INITIALIZATION,
        "native bypass replaces prior bridge error source");
    Fixture tiny; tiny.ofn.nMaxFile = 1;
    const unsigned tiny_calls = native_calls;
    adapter.Open(&tiny.ofn);
    Check(native_calls == tiny_calls + 1, "less than WORD capacity bypasses before bridge");
    Fixture large_multi; large_multi.ofn.Flags |= OFN_ALLOWMULTISELECT;
    large_multi.ofn.nMaxFile = 32769;
    const unsigned large_calls = native_calls;
    adapter.Open(&large_multi.ofn);
    Check(native_calls == large_calls + 1, "ANSI multiselect capacity over 32 KiB stays native before reading buffer");
    Check(multi.file[multi.ofn.nFileOffset + 12 + 10] == 0 &&
        multi.file[multi.ofn.nFileOffset + 12 + 11] == '!', "multi-select ends in double NUL and preserves tail");

    // Exercise the machine's actual ACP rather than replacing it with a test codec.
    // SBCS machines cannot provide a multi-byte encodable character; UTF-8 ACP
    // machines cannot provide a valid Unicode character that is unrepresentable.
    const UINT cp = GetACP();
    wchar_t multibyte = 0, unrepresentable = 0;
    std::string encoded;
    for (const wchar_t candidate : {L'中', L'あ', L'가', L'é', L'∞', L'Ж'}) {
        BOOL used = FALSE;
        char buffer[16]{};
        const int count = WideCharToMultiByte(cp, cp == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS,
            &candidate, 1, buffer, 16, nullptr, cp == CP_UTF8 ? nullptr : &used);
        wchar_t back = 0;
        const bool exact = count > 0 && !used && MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, buffer, count, &back, 1) == 1 && back == candidate;
        if (exact && count > 1 && !multibyte) { multibyte = candidate; encoded.assign(buffer, static_cast<size_t>(count)); }
        if (!exact && !unrepresentable) unrepresentable = candidate;
    }
    if (multibyte) {
        auto localized = accepted;
        const std::u16string localized_path = u"C:\\" + std::u16string(1, static_cast<char16_t>(multibyte)) + u"\\name.txt";
        localized.run_bridge = [&](const auto& request, HWND) {
            return Reply(request, picker::Status::Accepted, {localized_path});
        };
        LegacyAnsiFileDialogAdapter localized_adapter(localized);
        const std::string expected = "C:\\" + encoded + "\\name.txt";
        Fixture local;
        Check(localized_adapter.Open(&local.ofn) && std::string(local.file.data()) == expected &&
            static_cast<size_t>(local.ofn.nFileOffset) == 4 + encoded.size() && static_cast<size_t>(local.ofn.nFileExtension) == 9 + encoded.size(),
            "actual ACP multibyte output uses byte offsets and lossless encoding");
        Fixture short_bytes; short_bytes.ofn.nMaxFile = static_cast<DWORD>(expected.size());
        const auto old_bytes = short_bytes.file;
        const BOOL short_result = localized_adapter.Open(&short_bytes.ofn);
        WORD required = 0; std::memcpy(&required, short_bytes.file.data(), sizeof(required));
        Check(!short_result && localized_adapter.ExtendedError() == FNERR_BUFFERTOOSMALL && static_cast<size_t>(required) == expected.size() + 1 &&
            std::equal(short_bytes.file.begin() + 2, short_bytes.file.end(), old_bytes.begin() + 2),
            "ACP shortage reports bytes including NUL and mutates only first WORD");
        auto roundtrip = accepted;
        bool initial_matches = false;
        roundtrip.run_bridge = [&](const auto& request, HWND) {
            initial_matches = std::get<picker::Request>(request.body).initial_directory == localized_path;
            return Reply(request, picker::Status::Cancelled);
        };
        LegacyAnsiFileDialogAdapter roundtrip_adapter(roundtrip);
        Fixture input_acp; input_acp.ofn.lpstrInitialDir = expected.c_str();
        Check(!roundtrip_adapter.Open(&input_acp.ofn) && initial_matches, "ACP input reaches bridge as exact Unicode");
    } else std::printf("[SKIP] ACP %u has no tested multibyte character\n", cp);
    if (unrepresentable) {
        auto lossy = accepted;
        lossy.run_bridge = [&](const auto& request, HWND) {
            return Reply(request, picker::Status::Accepted, {u"C:\\" + std::u16string(1, static_cast<char16_t>(unrepresentable)) + u".txt"});
        };
        LegacyAnsiFileDialogAdapter lossy_adapter(lossy);
        Fixture unchanged; const auto original = unchanged.file; const auto original_title = unchanged.title;
        const unsigned old_calls = native_calls;
        Check(!lossy_adapter.Open(&unchanged.ofn) && lossy_adapter.ExtendedError() == FNERR_INVALIDFILENAME &&
            unchanged.file == original && unchanged.title == original_title && unchanged.ofn.nFileOffset == 91 && native_calls == old_calls,
            "unrepresentable or best-fit result fails atomically without native replay");
    } else if (cp == CP_UTF8) std::printf("[SKIP] N/A: UTF-8 represents every valid Unicode scalar; no unrepresentable-character case\n");
    else std::printf("[SKIP] ACP %u represents all tested Unicode characters\n", cp);
    if (cp == CP_UTF8 || IsDBCSLeadByteEx(cp, 0x81)) {
        Fixture malformed_input;
        malformed_input.file[0] = static_cast<char>(cp == CP_UTF8 ? 0xC0 : 0x81); malformed_input.file[1] = 0;
        const unsigned old_calls = native_calls;
        adapter.Open(&malformed_input.ofn);
        Check(native_calls == old_calls + 1, "invalid ACP input bypasses before bridge without substitution");
    }
    if (cp == CP_UTF8) {
        auto utf8 = accepted;
        const std::u16string first = u"C:\\bench_data\\\U0001F4C1\\\U0001F600.txt";
        const std::string directory = "C:\\bench_data\\\xF0\x9F\x93\x81";
        const std::string leaf = "\xF0\x9F\x98\x80.txt";
        const std::string expected = directory + "\\" + leaf;
        utf8.run_bridge = [&](const auto& request, HWND) { return Reply(request, picker::Status::Accepted, {first}); };
        LegacyAnsiFileDialogAdapter utf8_adapter(utf8);
        Fixture exact;
        exact.ofn.nMaxFile = static_cast<DWORD>(expected.size() + 1);
        exact.ofn.nMaxFileTitle = static_cast<DWORD>(leaf.size() + 1);
        Check(utf8_adapter.Open(&exact.ofn) && std::string(exact.file.data()) == expected && std::string(exact.title.data()) == leaf &&
            static_cast<size_t>(exact.ofn.nFileOffset) == directory.size() + 1 &&
            static_cast<size_t>(exact.ofn.nFileExtension) == directory.size() + 6 && exact.file[expected.size() + 1] == '!',
            "UTF-8 supplementary scalars use exact byte capacities, title and offsets");
        Fixture short_title_utf8; short_title_utf8.ofn.nMaxFileTitle = static_cast<DWORD>(leaf.size());
        const auto old_file_utf8 = short_title_utf8.file;
        const auto old_title_utf8 = short_title_utf8.title;
        Check(!utf8_adapter.Open(&short_title_utf8.ofn) && utf8_adapter.ExtendedError() == CDERR_INITIALIZATION &&
            short_title_utf8.file == old_file_utf8 && short_title_utf8.title == old_title_utf8 && short_title_utf8.ofn.nFileOffset == 91,
            "UTF-8 title one byte short fails atomically");
        Fixture short_file_utf8; short_file_utf8.ofn.nMaxFile = static_cast<DWORD>(expected.size());
        const auto old_short_utf8 = short_file_utf8.file;
        const BOOL short_result_utf8 = utf8_adapter.Open(&short_file_utf8.ofn);
        WORD required_utf8 = 0; std::memcpy(&required_utf8, short_file_utf8.file.data(), sizeof(required_utf8));
        Check(!short_result_utf8 && utf8_adapter.ExtendedError() == FNERR_BUFFERTOOSMALL &&
            static_cast<size_t>(required_utf8) == expected.size() + 1 &&
            std::equal(short_file_utf8.file.begin() + 2, short_file_utf8.file.end(), old_short_utf8.begin() + 2),
            "UTF-8 four-byte scalar shortage writes only required byte count WORD");
        auto utf8_many = utf8;
        utf8_many.run_bridge = [&](const auto& request, HWND) {
            return Reply(request, picker::Status::Accepted, {first, u"C:\\bench_data\\\U0001F4C1\\other.txt"});
        };
        LegacyAnsiFileDialogAdapter utf8_many_adapter(utf8_many);
        Fixture packed; packed.ofn.Flags |= OFN_ALLOWMULTISELECT; packed.ofn.lpstrFileTitle = nullptr;
        std::string expected_packed = directory; expected_packed.push_back(0);
        expected_packed += leaf; expected_packed.push_back(0);
        expected_packed += "other.txt"; expected_packed.append(2, 0);
        Check(utf8_many_adapter.Open(&packed.ofn) &&
            std::equal(expected_packed.begin(), expected_packed.end(), packed.file.begin()) &&
            packed.file[expected_packed.size()] == '!' && static_cast<size_t>(packed.ofn.nFileOffset) == directory.size() + 1 &&
            packed.ofn.nFileExtension == 0, "UTF-8 multiselect preserves embedded NULs, final double NUL and byte offset");
        for (const std::string& invalid_utf8_bytes : {std::string("\xC0\xAF"), std::string("\xE2\x82"), std::string("\xED\xA0\x80"), std::string("\xF4\x90\x80\x80")}) {
            Fixture bad_input; std::memcpy(bad_input.file.data(), invalid_utf8_bytes.c_str(), invalid_utf8_bytes.size() + 1);
            const auto original_invalid_input = bad_input.file;
            const unsigned old_native = native_calls, old_bridge = bridge_calls;
            adapter.Open(&bad_input.ofn);
            Check(native_calls == old_native + 1 && bridge_calls == old_bridge && bad_input.file == original_invalid_input,
                "invalid UTF-8 overlong/truncated/surrogate/out-of-range input bypasses before dispatch unchanged");
        }
        for (const char16_t isolated_surrogate : {char16_t(0xD800), char16_t(0xDC00)}) {
            auto bad_result = accepted;
            bad_result.run_bridge = [&](const auto& request, HWND) {
                return Reply(request, picker::Status::Accepted, {u"C:\\bench_data\\" + std::u16string(1, isolated_surrogate) + u".txt"});
            };
            LegacyAnsiFileDialogAdapter bad_result_adapter(bad_result);
            Fixture output; const auto old_file = output.file; const auto old_title = output.title;
            const unsigned old_native = native_calls;
            Check(!bad_result_adapter.Open(&output.ofn) && bad_result_adapter.ExtendedError() == FNERR_INVALIDFILENAME &&
                output.file == old_file && output.title == old_title && output.ofn.nFileOffset == 91 && output.ofn.nFileExtension == 92 &&
                native_calls == old_native, "isolated UTF-16 surrogate result fails atomically without native replay");
        }
    }
    LegacyDialogServices wide_services;
    wide_services.open_native = NativeWide; wide_services.save_native = NativeWide;
    wide_services.native_error = WideNativeError;
    LegacyFileDialogAdapter wide_native(wide_services);
    wide_services.verified_abi_checks = [](const auto& prepared) { return prepared.required_abi_checks; };
    picker::Status wide_status = picker::Status::Cancelled;
    wide_services.run_bridge = [&](const auto& request, HWND) {
        return Reply(request, wide_status, wide_status == picker::Status::Accepted ? std::vector<std::u16string>{wire} : std::vector<std::u16string>{});
    };
    LegacyFileDialogAdapter wide_adapter(wide_services);
    std::array<wchar_t, 512> wide_file{};
    OPENFILENAMEW wide_ofn{};
    wide_ofn.lStructSize = sizeof(wide_ofn);
    wide_ofn.lpstrFile = wide_file.data(); wide_ofn.nMaxFile = static_cast<DWORD>(wide_file.size());
    wide_ofn.lpstrFilter = L"Text\0*.txt\0All\0*.*\0\0";
    wide_ofn.Flags = Fixture{}.ofn.Flags;
    for (const auto status : {picker::Status::Accepted, picker::Status::Failed, picker::Status::Cancelled}) {
        Fixture mixed;
        const auto& ansi = status == picker::Status::Accepted ? adapter : status == picker::Status::Failed ? failure : cancel;
        const DWORD expected = status == picker::Status::Failed ? CDERR_DIALOGFAILURE : 0;
        const unsigned old_queries = error_queries;
        ansi.Open(&mixed.ofn);
        Check(wide_adapter.ExtendedError() == expected && wide_native.ExtendedError() == expected && error_queries == old_queries,
            "A success/failure/cancel is visible through all W adapter queries without native calls");
        wide_status = status; wide_file.fill(0);
        wide_adapter.Open(&wide_ofn);
        Check(adapter.ExtendedError() == expected && native.ExtendedError() == expected && error_queries == old_queries,
            "W success/failure/cancel is visible through all A adapter queries without native calls");
    }
    Fixture mixed_failure;
    failure.Open(&mixed_failure.ofn);
    const unsigned before_wide_native = error_queries;
    wide_native.Open(&wide_ofn);
    Check(error_queries == before_wide_native && adapter.ExtendedError() == native_error &&
        wide_adapter.ExtendedError() == native_error + 1,
        "W native bypass clears A bridge state and queries each current service lazily");
    wide_status = picker::Status::Failed; wide_file.fill(0); wide_adapter.Open(&wide_ofn);
    const unsigned before_ansi_native = error_queries;
    native.Open(&mixed_failure.ofn);
    Check(error_queries == before_ansi_native && wide_adapter.ExtendedError() == native_error + 1 &&
        adapter.ExtendedError() == native_error, "A native bypass clears W bridge state without caching native errors");
    wide_file.fill(0); wide_adapter.Open(&wide_ofn);
    LegacyAnsiFileDialogAdapter::UseNativeError();
    Check(wide_adapter.ExtendedError() == native_error + 1, "A external-dialog handoff clears W bridge state");
    failure.Open(&mixed_failure.ofn);
    LegacyFileDialogAdapter::UseNativeError();
    Check(adapter.ExtendedError() == native_error, "W external-dialog handoff clears A bridge state");
    failure.Open(&mixed_failure.ofn);
    bool mixed_thread_ok = false;
    std::thread mixed_worker([&] {
        mixed_thread_ok = wide_adapter.ExtendedError() == 0x1235 && adapter.ExtendedError() == 0x1234;
        Fixture local; cancel.Open(&local.ofn);
        mixed_thread_ok = mixed_thread_ok && wide_adapter.ExtendedError() == 0;
        LegacyFileDialogAdapter::UseNativeError();
        mixed_thread_ok = mixed_thread_ok && adapter.ExtendedError() == 0x1234;
    });
    mixed_worker.join();
    Check(mixed_thread_ok && wide_adapter.ExtendedError() == CDERR_DIALOGFAILURE && adapter.ExtendedError() == CDERR_DIALOGFAILURE,
        "shared A/W state and handoff remain isolated between threads");
    std::printf("Checks: %d, failures: %d\n", checks, failures);
    return failures ? 1 : 0;
}

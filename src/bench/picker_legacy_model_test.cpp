#include "../ipc/picker_legacy_model.h"
#include <iostream>
#include <algorithm>

using namespace pulse::ipc::picker;
namespace native = pulse::ipc::picker::legacy;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* name) {
    ++checks; if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
}
constexpr char16_t kFilters[] = u"Text\0*.txt\0All\0*.*\0";
constexpr char16_t kInitial[] = u"";
native::Input Input() {
    native::Input input;
    input.flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_DONTADDTORECENT | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    input.filter_multisz = kFilters;
    input.initial_filename = kInitial;
    return input;
}
RequestId Id() { RequestId id{}; id[0] = 1; return id; }
Message Answer(const native::Prepared& prepared, std::vector<std::u16string> paths) {
    Response response;
    response.operation = std::get<Request>(prepared.request.body).operation;
    response.status = Status::Accepted; response.paths = std::move(paths);
    return {prepared.request.request_id, std::move(response)};
}
}
int main() {
    auto input = Input();
    native::Prepared prepared;
    Check(native::Prepare(input, Id(), prepared) == native::Result::Ready, "prepare explicit supported Unicode contract");
    Check(prepared.required_abi_checks == (native::CallerDirectoryPolicy | native::LegacyFileValidation), "Ready retains mandatory unresolved ABI compatibility gates");
    input.flags &= ~OFN_NOCHANGEDIR;
    Check(native::Prepare(input, Id(), prepared) == native::Result::Unsupported, "missing caller directory policy falls back before dispatch");
    input = Input();
    Check(std::get<Request>(prepared.request.body).filter_index == 0, "native one-based to wire zero-based filter");
    input.filter_index = 0;
    Check(native::Prepare(input, Id(), prepared) == native::Result::Ready && std::get<Request>(prepared.request.body).filter_index == 0, "native zero without custom filter defaults first");
    input.filter_index = 2;
    Check(native::Prepare(input, Id(), prepared) == native::Result::Ready && std::get<Request>(prepared.request.body).filter_index == 1, "second native filter maps correctly");
    input.filter_index = 3; Check(native::Prepare(input, Id(), prepared) == native::Result::InvalidInput, "out-of-range filter rejected");
    for (const auto flag : {DWORD{OFN_ENABLEHOOK}, DWORD{OFN_ENABLETEMPLATE}, DWORD{OFN_NOVALIDATE}, DWORD{OFN_NODEREFERENCELINKS}, DWORD{0x80000000}}) {
        input = Input(); input.flags |= flag;
        Check(native::Prepare(input, Id(), prepared) == native::Result::Unsupported, "unsupported flags never silently ignored");
    }
    input = Input(); input.ansi = true; Check(native::Prepare(input, Id(), prepared) == native::Result::Unsupported, "ANSI explicitly unsupported");
    input = Input(); input.hook = true; Check(native::Prepare(input, Id(), prepared) == native::Result::Unsupported, "hook pointer unsupported");
    input = Input(); input.custom_template = true; Check(native::Prepare(input, Id(), prepared) == native::Result::Unsupported, "custom template unsupported");
    input = Input(); input.custom_filter = true; Check(native::Prepare(input, Id(), prepared) == native::Result::Unsupported, "custom filter unsupported");
    input = Input(); input.flags_ex = 1; Check(native::Prepare(input, Id(), prepared) == native::Result::Unsupported, "unknown extended flags unsupported");
    input = Input(); input.filter_multisz = {}; Check(native::Prepare(input, Id(), prepared) == native::Result::Unsupported, "no-filter shortcut contract rejected");
    const char16_t malformed[] = {u'A', 0, u'*'};
    input = Input(); input.filter_multisz = malformed; Check(native::Prepare(input, Id(), prepared) == native::Result::InvalidInput, "bounded unterminated filter rejected");
    input = Input(); input.initial_filename = std::span(kFilters).first(2); Check(native::Prepare(input, Id(), prepared) == native::Result::InvalidInput, "bounded unterminated filename rejected");
    input = Input(); input.file_title_present = true; native::Prepare(input, Id(), prepared);
    const std::u16string path = u"C:\\dir\\file.txt";
    auto response = Answer(prepared, {path});
    std::vector<char16_t> file(path.size() + 1, u'#'), title(9, u'#');
    native::Output output{file, title, 99, 98, 97, 96};
    auto completed = native::Complete(prepared, response, output);
    Check(completed.result == native::Result::Accepted && std::u16string(file.data()) == path, "exact single-file capacity succeeds");
    Check(output.file_offset == 7 && output.extension_offset == 12 && output.filter_index == 1, "single file offsets and returned native filter");
    Check(std::u16string(title.data()) == u"file.txt", "file title receives leaf and extension");
    file.assign(path.size(), u'#'); title.assign(9, u'#'); output = {file, title, 99, 98, 97, 96};
    completed = native::Complete(prepared, response, output);
    Check(completed.result == native::Result::BufferTooSmall && completed.extended_error == FNERR_BUFFERTOOSMALL && file[0] == path.size() + 1, "short file buffer reports required first WORD");
    Check(std::all_of(file.begin() + 1, file.end(), [](char16_t c) { return c == u'#'; }) && title[0] == u'#' && output.file_offset == 99 && output.filter_index == 97,
        "buffer shortage changes only prescribed first WORD");
    file.assign(path.size() + 1, u'#'); title.assign(2, u'#'); output = {file, title, 99, 98, 97, 96};
    completed = native::Complete(prepared, response, output);
    Check(completed.result == native::Result::Failed && file[0] == u'#' && title[0] == u'#' && output.flags == 96, "short title buffer fails without partial commit");
    auto cancelled = response; auto& cancelled_body = std::get<Response>(cancelled.body);
    cancelled_body.status = Status::Cancelled; cancelled_body.paths.clear();
    completed = native::Complete(prepared, cancelled, output);
    Check(completed.result == native::Result::Cancelled && completed.extended_error == 0 && file[0] == u'#' && title[0] == u'#' && output.file_offset == 99, "cancel leaves all outputs untouched");
    input = Input(); input.flags |= OFN_ALLOWMULTISELECT; native::Prepare(input, Id(), prepared);
    response = Answer(prepared, {u"C:\\dir\\a.txt", u"C:\\dir\\b c.txt"});
    constexpr char16_t multi[] = u"C:\\dir\0a.txt\0b c.txt\0";
    file.assign(std::size(multi), u'#'); output = {file, {}};
    completed = native::Complete(prepared, response, output);
    Check(completed.result == native::Result::Accepted && std::equal(file.begin(), file.end(), std::begin(multi)), "same-directory multiselect exact double-NUL layout");
    Check(output.file_offset == 7 && output.extension_offset == 0, "multiselect first-leaf offset and unused extension");
    response = Answer(prepared, {u"C:\\dir\\a.txt", u"D:\\other\\b.txt"}); file.assign(80, u'#'); output = {file, {}};
    Check(native::Complete(prepared, response, output).result == native::Result::Failed && file[0] == u'#', "accepted cross-directory selection fails without writes or native replay");
    input.flags &= ~OFN_EXPLORER; Check(native::Prepare(input, Id(), prepared) == native::Result::Unsupported, "old-style multiselect unsupported");
    input = Input(); input.flags |= OFN_ALLOWMULTISELECT; input.file_title_present = true;
    Check(native::Prepare(input, Id(), prepared) == native::Result::Unsupported, "undefined multi-file title contract falls back before UI");
    input.file_title_present = false; native::Prepare(input, Id(), prepared);
    std::vector<std::u16string> many;
    for (int i = 0; i < 1000; ++i) many.push_back(u"C:\\dir\\" + std::u16string(70, u'a') + u".txt");
    file.assign(4, u'#'); output = {file, {}};
    completed = native::Complete(prepared, Answer(prepared, many), output);
    Check(completed.result == native::Result::Failed && completed.required_file_units > 65535 && file[0] == u'#', "accepted unrepresentable required WORD fails without writes or replay");
    input = Input(); native::Prepare(input, Id(), prepared);
    for (const auto& candidate : {std::u16string(u"\\\\server\\share\\folder\\file.txt"), std::u16string(u"\\\\?\\C:\\") + std::u16string(300, u'a') + u"\\file.txt",
        std::u16string(u"\\\\?\\UNC\\server\\share\\file.txt")}) {
        file.assign(candidate.size() + 1, u'#'); output = {file, {}};
        Check(native::Complete(prepared, Answer(prepared, {candidate}), output).result == native::Result::Accepted && std::u16string(file.data()) == candidate, "UNC and extended long paths preserved");
    }
    for (const auto& candidate : {std::u16string(u"C:\\dir.dot\\readme"), std::u16string(u"C:\\dir\\readme.")}) {
        file.assign(candidate.size() + 1, u'#'); output = {file, {}};
        native::Complete(prepared, Answer(prepared, {candidate}), output);
        Check(output.extension_offset == (candidate.back() == u'.' ? candidate.size() : 0), "extension offsets ignore directory dots and retain trailing dot position");
    }
    input = Input(); input.save = true; input.flags &= ~OFN_FILEMUSTEXIST;
    input.flags |= OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOTESTFILECREATE; input.default_extension = u"txt";
    Check(native::Prepare(input, Id(), prepared) == native::Result::Ready, "prepare conservative Save contract");
    Check((prepared.required_abi_checks & native::SaveReadOnlyCompatibility) != 0, "Save NOTESTFILECREATE read-only incompatibility remains an explicit ABI gate");
    const char16_t full_save_name[] = u"C:\\dir\\new.txt";
    input.initial_filename = full_save_name;
    Check(native::Prepare(input, Id(), prepared) == native::Result::Unsupported, "Save absolute filename requires pre-UI native fallback");
    const char16_t relative_save_name[] = u"subdir\\new.txt";
    input.initial_filename = relative_save_name;
    Check(native::Prepare(input, Id(), prepared) == native::Result::Unsupported, "Save relative path filename requires pre-UI native fallback");
    input.initial_filename = kInitial;
    file.assign(100, u'#'); output = {file, {}};
    Check(native::Complete(prepared, Answer(prepared, {u"C:\\dir\\new.TXT"}), output).result == native::Result::Accepted && !(output.flags & OFN_EXTENSIONDIFFERENT), "default extension comparison ignores ASCII case");
    native::Complete(prepared, Answer(prepared, {u"C:\\dir\\new.csv"}), output);
    Check((output.flags & OFN_EXTENSIONDIFFERENT) != 0, "different extension output flag");
    input.default_extension = u"html"; Check(native::Prepare(input, Id(), prepared) == native::Result::Unsupported, "legacy extension truncation not silently replaced with modern rule");
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}

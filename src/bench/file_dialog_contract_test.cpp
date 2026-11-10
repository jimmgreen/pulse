#include "../shell_bridge/file_dialog_contract.h"
#include "../shell_bridge/file_dialog_runtime.h"
#include <iostream>
#include <utility>

using namespace pulse::shell_bridge;
namespace wire = pulse::ipc::picker;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* name) {
    ++checks; if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
}
wire::RequestId Id() { wire::RequestId id{}; id[0] = 1; return id; }
SetterCall Options(uint32_t flags) { SetterCall c; c.setting = Setting::Options; c.native_result = S_OK; c.value = flags; return c; }
SetterCall Text(Setting setting, std::u16string text = {}) {
    SetterCall c; c.setting = setting; c.native_result = S_OK; c.text = std::move(text); return c;
}
SetterCall Types() {
    SetterCall c; c.setting = Setting::FileTypes; c.native_result = S_OK;
    c.filters = {{u"Text", u"*.txt;*.md"}, {u"All", u"*.*"}}; return c;
}
std::vector<SetterCall> Baseline(DialogFactory factory = DialogFactory::Open) {
    auto folder = Text(Setting::Folder, u"C:\\fixture"); folder.filesystem_folder = true;
    return {Options(DefaultDialogOptions(factory) | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT), folder};
}
bool NativeReasonIs(const ContractDecision& d, NativeReason reason) {
    return d.disposition == Disposition::NativeOnly && d.reason == reason && !d.request;
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--yjk-open-contract") {
        for (const auto* pattern : {u"*.*", u"*.dwg", u"*.dwg;*.dxf||", u"*.dxf||;*.dwg"}) {
            auto calls = Baseline();
            calls.push_back(Text(Setting::DefaultExtension, u"dwg||"));
            auto types = Types(); types.filters = {{u"Drawing", pattern}}; calls.push_back(types);
            const auto decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1);
            Check(decision.request && RuntimeContractCertified(decision) && decision.open_default_extension == u"dwg||",
                "Open preserves pipe default without truncation and certifies");
            Check(decision.request && std::get<wire::Request>(decision.request->body).filters[0].pattern == pattern,
                "Open keeps literal pipe filter instead of broadening it");
        }
        auto calls = Baseline(); calls.push_back(Text(Setting::DefaultExtension, u"dwg||"));
        const auto decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1);
        Check(decision.request && decision.open_default_extension == u"dwg||", "pipe default without filters stays literal");
        calls = Baseline(DialogFactory::Save); calls.push_back(Text(Setting::DefaultExtension, u"dwg||"));
        Check(!PrepareFileDialogContract(DialogFactory::Save, calls, Id(), AccessPolicy::LocalSaveV1).request,
            "Save pipe default stays outside the supported contract");
        std::cout << "Checks=" << checks << " failures=" << failures << '\n'; return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--native-options") {
        PreparationContext context;
        context.native_options_supported = true;
        context.native_save_defaults_supported = true;
        context.open_missing_supported = true;
        for (const auto flags : {0x1042u, 0x1242u}) {
            auto calls = Baseline(); calls[0].value = flags; calls.push_back(Types());
            auto decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1, context);
            Check(decision.effective_options == flags && RuntimeContractCertified(decision), "Firefox Open preserves original single/multiple options and certifies");
        }
        for (const uint32_t flag : {uint32_t(FOS_STRICTFILETYPES), uint32_t(FOS_ALLNONSTORAGEITEMS), uint32_t(FOS_CREATEPROMPT),
             uint32_t(FOS_SHAREAWARE), uint32_t(FOS_HIDEMRUPLACES), uint32_t(FOS_HIDEPINNEDPLACES),
             uint32_t(FOS_OKBUTTONNEEDSINTERACTION), uint32_t(FOS_FORCEPREVIEWPANEON), uint32_t(FOS_SUPPORTSTREAMABLEITEMS)}) {
            auto calls = Baseline();
            if (flag == FOS_ALLNONSTORAGEITEMS) calls[0].value &= ~FOS_FORCEFILESYSTEM;
            calls[0].value |= flag;
            auto decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1, context);
            Check(RuntimeContractCertified(decision) && decision.native_options == flag, "extended Open option is preserved and runtime-bound");
            auto old = context; old.native_options_supported = false;
            Check(!PrepareFileDialogContract(DialogFactory::Open,calls,Id(),AccessPolicy::LocalOpenV1,old).request, "old provider does not silently drop new semantics");
            auto corrupted = decision; corrupted.native_options = 0;
            Check(!RuntimeContractCertified(corrupted), "runtime rejects stripped extended options");
            calls = Baseline(DialogFactory::Save);
            if (flag == FOS_ALLNONSTORAGEITEMS) calls[0].value &= ~FOS_FORCEFILESYSTEM;
            calls[0].value |= flag;
            decision = PrepareFileDialogContract(DialogFactory::Save,calls,Id(),AccessPolicy::LocalSaveV1,context);
            Check(RuntimeLocalSaveContractCertified(decision) && decision.native_options == flag, "extended Save option is preserved and runtime-bound");
        }
        auto calls = Baseline(); calls[0].value |= FOS_ALLNONSTORAGEITEMS;
        Check(PrepareFileDialogContract(DialogFactory::Open,calls,Id(),AccessPolicy::LocalOpenV1,context).disposition == Disposition::InvalidArgument,
            "mutually exclusive filesystem and nonstorage options reject without pretending compatibility");
        calls = Baseline(); calls[0].value &= ~FOS_FILEMUSTEXIST; calls[0].value |= FOS_CREATEPROMPT;
        auto create = PrepareFileDialogContract(DialogFactory::Open,calls,Id(),AccessPolicy::LocalOpenV1,context);
        Check(create.allow_missing_open && !create.skip_open_access_validation && RuntimeContractCertified(create), "create prompt allows a missing leaf without NOVALIDATE");
        for (const bool options_first : {false, true}) {
            auto shell = Text(Setting::Folder, u"::{645FF040-5081-101B-9F08-00AA002F954E}");
            auto opts = Options(FOS_ALLNONSTORAGEITEMS | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR);
            std::vector<SetterCall> ordered = options_first ? std::vector<SetterCall>{opts, shell} : std::vector<SetterCall>{shell, opts};
            const auto virtual_folder = PrepareFileDialogContract(DialogFactory::Open, ordered, Id(), AccessPolicy::LocalOpenV1, context);
            Check(virtual_folder.request && RuntimeContractCertified(virtual_folder), "virtual folder uses final options regardless of setter order");
            ordered.push_back(Options(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST));
            Check(!PrepareFileDialogContract(DialogFactory::Open, ordered, Id(), AccessPolicy::LocalOpenV1, context).request,
                "later filesystem-only options reject an earlier virtual folder");
        }
        calls = Baseline(); calls[0].value |= 0x400000u;
        Check(!PrepareFileDialogContract(DialogFactory::Open,calls,Id(),AccessPolicy::LocalOpenV1,context).request,"reserved bit remains rejected");
        calls = Baseline();
        auto original = PrepareFileDialogContract(DialogFactory::Open,calls,Id(),AccessPolicy::LocalOpenV1);
        Check(original.native_options == 0 && RuntimeContractCertified(original),"original Pulse local Open policy stays certified");
        std::cout << "Checks=" << checks << " failures=" << failures << '\n'; return failures ? 1 : 0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--open-filter-separators") {
        for (const auto* pattern : {u"*.txt;", u"*.txt;;", u";*.txt", u"*.txt;;*.zip;", u";;*.txt;;"}) {
            auto calls = Baseline(); auto types = Types(); types.filters = {{u"Generic", pattern}}; calls.push_back(types);
            const auto open = PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1);
            Check(open.request && std::get<wire::Request>(open.request->body).filters[0].pattern == pattern, "Open empty separators preserve literal filter list");
            calls = Baseline(DialogFactory::Save); calls.push_back(types);
            Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Save, calls, Id(), AccessPolicy::LocalSaveV1), NativeReason::UnsupportedFilter), "Save filter contract remains unchanged");
        }
        for (const auto* pattern : {u"", u";", u";;;", u"*.txt;;C:\\*", u"*.txt;;*.?", u"*.txt;;../*", u"*.txt;report*"}) {
            auto calls = Baseline(); auto types = Types(); types.filters = {{u"Unsupported", pattern}}; calls.push_back(types);
            Check(!PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1).request, "empty-only and dangerous nonempty pieces rejected");
        }
        std::cout << "Checks=" << checks << " failures=" << failures << '\n'; return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--no-test-create") {
        PreparationContext context;
        context.native_save_defaults_supported = true;
        context.save_no_test_create_supported = true;
        for (uint32_t flags : {0x1000au, 0x1880au}) {
            auto calls = Baseline(DialogFactory::Save); calls[0].value = flags;
            calls.push_back(Text(Setting::DefaultExtension, u"pdf"));
            auto types = Types(); types.filters = {{u"PDF",u"*.pdf"},{u"All",u"*.*"}}; calls.push_back(types);
            auto d = PrepareFileDialogContract(DialogFactory::Save, calls, Id(), AccessPolicy::LocalSaveV1, context);
            Check(d.request && d.no_test_file_create && !d.skip_save_access_validation && RuntimeLocalSaveContractCertified(d), "standard no-test-create Save preserves independent validation policy");
            auto old = context; old.save_no_test_create_supported = false;
            Check(!PrepareFileDialogContract(DialogFactory::Save, calls, Id(), AccessPolicy::LocalSaveV1, old).request, "provider must explicitly support no-test-create");
            Check(d.require_existing_save_parent == ((flags & FOS_PATHMUSTEXIST) != 0), "no-test Save preserves native parent-existence requirement");
            auto malformed = d; malformed.require_existing_save_parent = !malformed.require_existing_save_parent;
            Check(!RuntimeLocalSaveContractCertified(malformed), "parent-existence policy mismatch rejected");
            d.no_test_file_create = false;
            Check(!RuntimeLocalSaveContractCertified(d), "native option and runtime policy mismatch rejected");
            calls[0].value &= ~FOS_OVERWRITEPROMPT;
            Check(!PrepareFileDialogContract(DialogFactory::Save, calls, Id(), AccessPolicy::LocalSaveV1, context).request, "no-test-create retains overwrite confirmation");
            calls[0].value = flags | FOS_NOVALIDATE;
            context.save_caller_validation_supported = true;
            Check(!PrepareFileDialogContract(DialogFactory::Save, calls, Id(), AccessPolicy::LocalSaveV1, context).request, "unverified combined NOVALIDATE policy remains native");
        }
        auto calls = Baseline(); calls[0].value |= FOS_NOTESTFILECREATE;
        Check(!PrepareFileDialogContract(DialogFactory::Open,calls,Id(),AccessPolicy::LocalOpenV1,context).request, "Save-only flag does not loosen Open");
        std::cout << "Checks=" << checks << " failures=" << failures << '\n'; return failures ? 1 : 0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--open-missing") {
        PreparationContext context;
        context.caller_validation_supported = true;
        context.open_missing_supported = true;
        for (uint32_t flags : {0x348u, 0xb48u}) {
            auto calls = Baseline(); calls[0].value = flags; calls.push_back(Types());
            auto d = PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1, context);
            Check(d.request && d.allow_missing_open && d.skip_open_access_validation && RuntimeContractCertified(d), "caller-validated missing Open contract is certified");
            Check(d.require_existing_open_parent == ((flags & FOS_PATHMUSTEXIST) != 0), "native parent existence requirement preserved");
            auto older = context; older.open_missing_supported = false;
            Check(!PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1, older).request, "old provider cannot accept missing Open");
            older = context; older.caller_validation_supported = false;
            Check(!PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1, older).request, "missing Open requires caller-validation capability");
            d.require_existing_open_parent = !d.require_existing_open_parent;
            Check(!RuntimeContractCertified(d), "inconsistent parent requirement rejected");
            calls.push_back(Text(Setting::DefaultExtension, u"txt"));
            Check(!PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1, context).request, "unverified missing default-extension semantics remain native");
            calls.pop_back(); calls[0].value &= ~FOS_NOVALIDATE;
            Check(!PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1, context).request, "ordinary Open never inherits missing-path semantics");
        }
        std::cout << "Checks=" << checks << " failures=" << failures << '\n'; return failures ? 1 : 0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--native-save-defaults") {
        PreparationContext context; context.native_save_defaults_supported = true;
        for (uint32_t options : {0x42u, 0x884au}) {
            auto calls = Baseline(DialogFactory::Save); calls[0].value = options;
            calls.push_back(Text(Setting::DefaultExtension, u"*.html"));
            auto filters = Types(); filters.filters = {{u"HTML",u"*.html"},{u"All",u"*.*"}}; calls.push_back(filters);
            const auto result = PrepareFileDialogContract(DialogFactory::Save,calls,Id(),AccessPolicy::LocalSaveV1,context);
            Check(result.request && result.native_save_defaults && RuntimeLocalSaveContractCertified(result), "native Save defaults admitted with explicit provider support");
            Check(!result.skip_save_access_validation, "minimal Save retains actual filesystem and access validation");
            Check(!PrepareFileDialogContract(DialogFactory::Save,calls,Id(),AccessPolicy::LocalSaveV1).request, "old provider never inherits new Save semantics");
            calls[0].value &= ~FOS_OVERWRITEPROMPT;
            Check(!PrepareFileDialogContract(DialogFactory::Save,calls,Id(),AccessPolicy::LocalSaveV1,context).request, "overwrite confirmation remains required");
            calls[0].value = options | FOS_NOVALIDATE;
            Check(!PrepareFileDialogContract(DialogFactory::Save,calls,Id(),AccessPolicy::LocalSaveV1,context).request, "new Save defaults cannot bypass access validation");
        }
        for (const auto* bad : {u"*.a/b",u"*.html;txt",u"*.*",u".html"}) {
            auto calls = Baseline(DialogFactory::Save); calls.push_back(Text(Setting::DefaultExtension,bad));
            Check(!PrepareFileDialogContract(DialogFactory::Save,calls,Id(),AccessPolicy::LocalSaveV1,context).request, "unverified Save extension shape stays native");
        }
        std::cout << "Checks=" << checks << " failures=" << failures << '\n'; return failures ? 1 : 0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--open-extension-contract") {
        for (const auto* extension : {u"td;bt", u".td", u"tar.gz", u"*.td;*.bt", u"*", u"a b"}) {
            auto calls = Baseline(); calls.push_back(Text(Setting::DefaultExtension, extension));
            const auto decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1);
            Check(decision.request && decision.open_default_extension == extension, "Open keeps complex default extension verbatim");
        }
        for (const auto* extension : {u"a/b", u"a\\b", u"a:b", u"a\tb", u"td.", u"td "}) {
            auto calls = Baseline(); calls.push_back(Text(Setting::DefaultExtension, extension));
            Check(!PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1).request, "Open rejects path or control syntax");
        }
        auto calls = Baseline(DialogFactory::Save); calls.push_back(Text(Setting::DefaultExtension, u".td"));
        Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Save, calls, Id()), NativeReason::UnsupportedExtension), "Save extension restrictions unchanged");
        return failures ? 1 : 0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--xmind-open-contract") {
        for (uint32_t flags : {0x1240u, 0x1040u}) {
            auto calls = Baseline(); calls[0].value = flags;
            const auto d = PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1);
            Check(d.request && d.effective_options == flags && RuntimeContractCertified(d), "existing-file Open without PATHMUSTEXIST admitted unchanged");
            Check(d.request && !d.skip_open_access_validation &&
                (std::get<wire::Request>(d.request->body).options & wire::ValidateLocalOpenAccessV1), "existence and access validation retained");
            Check(d.request && ((std::get<wire::Request>(d.request->body).options & wire::AllowMultiSelect) != 0) == ((flags & FOS_ALLOWMULTISELECT) != 0), "selection multiplicity retained");
            calls[0].value &= ~FOS_FILEMUSTEXIST;
            Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1), NativeReason::MissingValidationOptions), "FILEMUSTEXIST still required");
            calls[0].value = flags | FOS_NOVALIDATE;
            PreparationContext context; context.caller_validation_supported = true;
            Check(!PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1, context).request, "NOVALIDATE without path existence remains native");
        }
        auto calls = Baseline(); calls[0].value &= ~FOS_PATHMUSTEXIST;
        Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::MissingValidationOptions), "default Open remains strict");
        calls = Baseline(DialogFactory::Save); calls[0].value &= ~FOS_PATHMUSTEXIST;
        Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Save, calls, Id(), AccessPolicy::LocalSaveV1), NativeReason::MissingValidationOptions), "Save remains strict");
        std::cout << "Checks=" << checks << " failures=" << failures << '\n'; return failures ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--folders") {
        for (uint32_t flags : {0x1060u, 0x1260u, 0x101828u}) {
            auto folder = Baseline()[1];
            auto types = Types(); types.native_result = E_UNEXPECTED;
            auto index = Text(Setting::FileTypeIndex); index.value = 1; index.native_result = E_UNEXPECTED;
            std::vector<SetterCall> calls{Options(flags), Text(Setting::FileName), Text(Setting::Title, u"Select"), types, folder, index};
            const auto d = PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1);
            Check(d.request && std::get<wire::Request>(d.request->body).operation == wire::Operation::PickFolder, "folder replay admitted");
            Check(RuntimeContractCertified(d), "runtime admits folder contract");
            Check(d.request && ((std::get<wire::Request>(d.request->body).options & wire::AllowMultiSelect) != 0) == ((flags & FOS_ALLOWMULTISELECT) != 0), "folder multiselect preserved");
            calls[1].text = u"folder";
            Check(PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1).request.has_value(), "nonempty folder filename preserved");
            calls[1].text.clear();
            calls.back().native_result = E_ACCESSDENIED;
            Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1), NativeReason::SetterFailed), "folder index errors other than native no-effect rejection remain native");
            calls.back().native_result = E_UNEXPECTED;
            calls[3].native_result = E_FAIL;
            Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1), NativeReason::SetterFailed), "unknown failed setter remains native");
        }
        for (const auto factory : {DialogFactory::Open, DialogFactory::Save}) {
            auto calls = Baseline(factory);
            auto index = Text(Setting::FileTypeIndex); index.value = 1; index.native_result = E_UNEXPECTED;
            calls.push_back(index);
            Check(NativeReasonIs(PrepareFileDialogContract(factory, calls, Id()), NativeReason::SetterFailed), "ordinary file dialog index failure remains native");
        }
        {
            auto calls = Baseline();
            calls.push_back(Types());
            calls.push_back(Options(calls[0].value | FOS_PICKFOLDERS));
            auto index = Text(Setting::FileTypeIndex); index.value = 1; index.native_result = E_UNEXPECTED;
            calls.push_back(index);
            const auto decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1);
            Check(NativeReasonIs(decision, NativeReason::SetterFailed) && decision.call_index == calls.size() - 1,
                "folder index failure after successful file types is never ignored");
        }
        std::cout << "Checks=" << checks << " failures=" << failures << '\n'; return failures ? 1 : 0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--extension-masks") {
        for (const auto* pattern : {u"*.xl*", u"*.xl*;*.xlsx", u"*.a*b", u"*.TXT;*.x*", u"*.*"}) {
            auto calls = Baseline(); auto types = Types(); types.filters = {{u"Generic extension mask", pattern}}; calls.push_back(types);
            const auto open = PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1);
            Check(open.disposition == Disposition::Prepared && open.request &&
                std::get<wire::Request>(open.request->body).filters[0].pattern == pattern, "Open preserves generic extension mask unchanged");
            if (std::u16string_view(pattern) != u"*.*") {
                calls = Baseline(DialogFactory::Save); calls.push_back(types);
                Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Save, calls, Id(), AccessPolicy::LocalSaveV1), NativeReason::UnsupportedFilter),
                    "Open mask support does not widen Save contract");
            }
        }
        for (const auto* pattern : {u"report*.txt", u"C:\\*.xlsx", u"../*.xls", u"*.x?", u";;;", u"*.xl*;;bad*", u"*.a b", u"*."}) {
            auto calls = Baseline(); auto types = Types(); types.filters = {{u"Unsupported", pattern}}; calls.push_back(types);
            Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1), NativeReason::UnsupportedFilter),
                "unsupported filename/path/question/empty filter syntax remains rejected");
        }
        std::cout << "Checks=" << checks << " failures=" << failures << '\n'; return failures ? 1 : 0;
    }

    Check(DefaultDialogOptions(DialogFactory::Open) == (FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR), "native Open defaults");
    Check(DefaultDialogOptions(DialogFactory::Save) == (FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR), "native Save defaults");
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, {}, Id()), NativeReason::FilesystemNotForced), "unmodified defaults do not falsely force filesystem semantics");
    auto calls = Baseline();
    auto decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(decision.disposition == Disposition::Prepared && decision.request && std::get<wire::Request>(decision.request->body).operation == wire::Operation::OpenFiles, "explicit Open settings prepare request");
    Check(decision.required_runtime_checks == (NativeValidationParity | ShellResultContract), "Prepared does not bypass native validation or Shell result obligations");
    calls.push_back(Types());
    auto index = Text(Setting::FileTypeIndex); index.value = 2; calls.push_back(index);
    calls.push_back(Text(Setting::FileName, u"sample.txt"));
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(decision.request && std::get<wire::Request>(decision.request->body).filter_index == 1 &&
        std::get<wire::Request>(decision.request->body).filename == u"sample.txt", "filter index converts from one to zero and filename preserved");
    Check(decision.request &&
        (std::get<wire::Request>(decision.request->body).options & wire::AdvisoryFileFilters) &&
        (std::get<wire::Request>(decision.request->body).required_capabilities & wire::AdvisoryFilters),
        "filtered default Open explicitly negotiates advisory acceptance");
    calls[0].value |= FOS_ALLOWMULTISELECT | FOS_FORCESHOWHIDDEN;
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(decision.request && std::get<wire::Request>(decision.request->body).options == (wire::AllowMultiSelect | wire::ShowHidden | wire::AdvisoryFileFilters), "verified Open options mapped");
    calls.push_back(Options(DefaultDialogOptions(DialogFactory::Open) | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT));
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(decision.request && !(std::get<wire::Request>(decision.request->body).options & wire::AllowMultiSelect), "later SetOptions replaces previous options");
    calls = Baseline(); calls.push_back(Options(FOS_FORCEFILESYSTEM));
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::RecentItemsPolicy), "SetOptions does not secretly restore defaults");
    calls = Baseline(); calls.pop_back();
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::DefaultDirectoryPolicy), "missing explicit folder requires native directory strategy");
    calls = Baseline(); calls.push_back(Text(Setting::DefaultFolder, u"C:\\default"));
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::DefaultDirectoryPolicy), "SetDefaultFolder not conflated with SetFolder");
    calls = Baseline(); calls[1].filesystem_folder = false;
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::NonFilesystemFolder), "virtual Shell folder not coerced to path");
    calls = Baseline(); calls[1].text = u"relative";
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::NonFilesystemFolder), "relative initial location cannot silently use process directory");
    calls = Baseline(); calls[1].text = u"\\\\server\\share\\folder";
    Check(PrepareFileDialogContract(DialogFactory::Open, calls, Id()).disposition == Disposition::Prepared, "explicit UNC directory retained");
    calls = Baseline(); calls[0].native_result = E_ACCESSDENIED;
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(NativeReasonIs(decision, NativeReason::SetterFailed) && decision.observed_result == E_ACCESSDENIED && decision.call_index == 0, "failed setter never reported as prepared success");
    calls = Baseline(); calls[1].native_result = S_FALSE;
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::UnrecognizedSuccess), "non-S_OK result not invented as normal success");
    calls = Baseline(); calls.push_back(Types()); calls.push_back(Types());
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::UnsupportedSettingOrder), "duplicate successful SetFileTypes trace remains native");
    {
        // WinForms without a Filter calls SetFileTypes(0, ...); native returns S_OK.
        auto empty = Types(); empty.filters.clear();
        for (const auto factory : {DialogFactory::Open, DialogFactory::Save}) {
            auto empty_calls = Baseline(factory); empty_calls.push_back(empty);
            const auto prepared = PrepareFileDialogContract(factory, empty_calls, Id());
            Check(prepared.disposition == Disposition::Prepared && prepared.request &&
                std::get<wire::Request>(prepared.request->body).filters.empty(),
                "native-accepted empty SetFileTypes prepares with no filters");
        }
        auto later = Baseline(); later.push_back(empty); later.push_back(Types());
        const auto replaced = PrepareFileDialogContract(DialogFactory::Open, later, Id());
        Check(replaced.request && std::get<wire::Request>(replaced.request->body).filters.size() == 2,
            "a later native-accepted SetFileTypes after an empty one supplies the filters");
        auto index_after_empty = Baseline(); index_after_empty.push_back(empty);
        auto one = Text(Setting::FileTypeIndex); one.value = 1; index_after_empty.push_back(one);
        Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, index_after_empty, Id()), NativeReason::UnsupportedSettingOrder),
            "filter index after only an empty list stays native");
        auto failed_empty = Baseline(); auto rejected = empty; rejected.native_result = E_INVALIDARG; failed_empty.push_back(rejected);
        Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, failed_empty, Id()), NativeReason::SetterFailed),
            "empty list rejected by native still follows native");
    }
    calls = Baseline(); calls.push_back(index); calls.push_back(Types());
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::UnsupportedSettingOrder), "index before file types not silently reordered");
    for (uint32_t invalid_index : {0u, 3u}) {
        calls = Baseline(); calls.push_back(Types()); index.value = invalid_index; calls.push_back(index);
        Check(PrepareFileDialogContract(DialogFactory::Open, calls, Id()).disposition == Disposition::InvalidArgument, "invalid native filter index rejected");
    }
    {
        auto normalized_calls = Baseline(); normalized_calls.push_back(Types());
        auto original_zero = Text(Setting::FileTypeIndex); original_zero.value = 0;
        original_zero.observed_filter_index = 1; normalized_calls.push_back(original_zero);
        const auto normalized = PrepareFileDialogContract(DialogFactory::Open, normalized_calls, Id());
        Check(normalized.disposition == Disposition::Prepared && normalized.request &&
            std::get<wire::Request>(normalized.request->body).filter_index == 0 &&
            normalized_calls.back().value == 0 && normalized_calls.back().native_result == S_OK,
            "native getter evidence maps effective one to wire zero without rewriting original setter zero");
        normalized_calls.back().observed_filter_index.reset();
        Check(PrepareFileDialogContract(DialogFactory::Open, normalized_calls, Id()).disposition == Disposition::InvalidArgument,
            "setter zero without successful getter evidence retains original rejection");
        for (const uint32_t invalid_observation : {0u, 3u}) {
            normalized_calls.back().observed_filter_index = invalid_observation;
            Check(PrepareFileDialogContract(DialogFactory::Open, normalized_calls, Id()).disposition == Disposition::InvalidArgument,
                "observed zero or out-of-range filter index cannot authorize a request");
        }
        normalized_calls.back().observed_filter_index = 1;
        normalized_calls.back().native_result = E_ACCESSDENIED;
        Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, normalized_calls, Id()), NativeReason::SetterFailed),
            "getter observation cannot override original setter failure");
        normalized_calls.back().native_result = S_FALSE;
        Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, normalized_calls, Id()), NativeReason::UnrecognizedSuccess),
            "getter observation cannot override unrecognized setter success");
    }
    {
        auto common = Baseline(); common[0].value = 0x10001A00;
        auto all = Types(); all.filters = {{u"All", u"*"}}; common.push_back(all);
        auto mapped = PrepareFileDialogContract(DialogFactory::Open, common, Id(), AccessPolicy::LocalOpenV1);
        Check(mapped.disposition == Disposition::Prepared && mapped.request && mapped.effective_options == 0x10001A00u &&
            std::get<wire::Request>(mapped.request->body).filters[0].pattern == u"*.*",
            "all-file Open canonicalizes star without rewriting caller options");
        common[0].value &= ~FOS_FILEMUSTEXIST;
        Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, common, Id(), AccessPolicy::LocalOpenV1), NativeReason::MissingValidationOptions),
            "optional NOCHANGEDIR does not waive file existence");
        common[0].value = 0x10001A00 & ~FOS_PATHMUSTEXIST;
        Check(RuntimeContractCertified(PrepareFileDialogContract(DialogFactory::Open, common, Id(), AccessPolicy::LocalOpenV1)),
            "existing-file LocalOpen validates parent existence without caller PATHMUSTEXIST");
    }
    calls = Baseline(); auto types = Types(); types.filters[0].pattern = u"report?.txt"; calls.push_back(types);
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::UnsupportedFilter), "complex filter not approximated");
    calls = Baseline(); calls.push_back(Text(Setting::FileName, u"C:\\other\\file.txt"));
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::UnsupportedFilename), "path filename not silently split into different folder semantics");
    for (uint32_t unsupported : {uint32_t{FOS_STRICTFILETYPES}, uint32_t{FOS_FORCEPREVIEWPANEON}, uint32_t{FOS_ALLNONSTORAGEITEMS}, uint32_t{0x400000}}) {
        calls = Baseline(); calls[0].value |= unsupported;
        Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::UnsupportedOptions), "unknown or unimplemented FOS requires native UI");
    }
    calls = Baseline(); calls[0].value |= FOS_NODEREFERENCELINKS;
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(decision.disposition == Disposition::Prepared && decision.request &&
        std::get<wire::Request>(decision.request->body).operation == wire::Operation::OpenFiles &&
        std::get<wire::Request>(decision.request->body).options == wire::NoDereferenceLinks &&
        std::get<wire::Request>(decision.request->body).required_capabilities == (wire::Open | wire::PreserveLinks) &&
        decision.effective_options == calls[0].value && decision.validation_category == ValidationCategory::DefaultOpen &&
        decision.required_runtime_checks == (NativeValidationParity | ShellResultContract),
        "Open shortcut preservation maps both wire option and required capability without relaxing validation");
    bool bound_wire = false, missing_capability_rejected = false, response_bound = false;
    if (decision.request) {
        std::vector<uint8_t> bytes;
        wire::Message decoded;
        bound_wire = wire::Encode(*decision.request, bytes) == wire::CodecError::None &&
            wire::Decode(bytes, decoded) == wire::CodecError::None && decoded == *decision.request;
        auto missing = *decision.request;
        std::get<wire::Request>(missing.body).required_capabilities &= ~wire::PreserveLinks;
        missing_capability_rejected = wire::Encode(missing, bytes) == wire::CodecError::InvalidSemantics;
        wire::Response accepted;
        accepted.status = wire::Status::Accepted; accepted.paths = {u"C:\\fixture\\selected.lnk"};
        wire::Message reply{decision.request->request_id, accepted};
        response_bound = wire::ValidateResponse(*decision.request, reply);
        reply.request_id[0] ^= 0x40;
        response_bound = response_bound && !wire::ValidateResponse(*decision.request, reply);
    }
    Check(bound_wire, "shortcut-preserving request round-trips complete body and request ID");
    Check(missing_capability_rejected, "NoDereferenceLinks cannot travel without required PreserveLinks capability");
    Check(response_bound, "shortcut response must bind to the actual request identity");
    calls[0].value |= FOS_ALLOWMULTISELECT | FOS_FORCESHOWHIDDEN; calls.push_back(Types());
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(decision.request && std::get<wire::Request>(decision.request->body).options ==
        (wire::NoDereferenceLinks | wire::AllowMultiSelect | wire::ShowHidden | wire::AdvisoryFileFilters) &&
        std::get<wire::Request>(decision.request->body).required_capabilities ==
        (wire::Open | wire::PreserveLinks | wire::MultiSelect | wire::Filters | wire::AdvisoryFilters),
        "link preservation composes with Open multiselect hidden items and filters");
    calls.push_back(Options(Baseline()[0].value));
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(decision.request && !(std::get<wire::Request>(decision.request->body).options & wire::NoDereferenceLinks) &&
        !(std::get<wire::Request>(decision.request->body).required_capabilities & wire::PreserveLinks),
        "later replacing SetOptions removes link option and capability together");
    calls = Baseline(); calls[0].value |= FOS_NODEREFERENCELINKS | FOS_NOVALIDATE;
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(decision.request && decision.validation_category == ValidationCategory::OpenSkipAccessValidation &&
        std::get<wire::Request>(decision.request->body).options == (wire::SkipAccessValidation | wire::NoDereferenceLinks),
        "explicit access policy composes with link contract without approving runtime parity");
    calls = Baseline(DialogFactory::Save); calls.push_back(Text(Setting::ReadonlyOwnershipCollection));
    decision = PrepareFileDialogContract(DialogFactory::Save, calls, Id(), AccessPolicy::LocalSaveV1);
    Check(decision.request && decision.require_unencrypted_save, "observed readonly ownership collection requires dedicated non-encrypted Save guard");
    calls.push_back(Text(Setting::SaveProperties));
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Save, calls, Id(), AccessPolicy::LocalSaveV1), NativeReason::SaveMetadata), "readonly ownership does not admit additional editable metadata");
    calls = Baseline(); calls.push_back(Text(Setting::ReadonlyOwnershipCollection));
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1), NativeReason::SaveMetadata), "Save collection contract cannot apply to Open");
    calls = Baseline(DialogFactory::Save); calls[0].value |= FOS_NODEREFERENCELINKS;
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Save, calls, Id()), NativeReason::UnsupportedOptions),
        "Save shortcut-preservation semantics remain native until separately verified");
    calls = Baseline(); calls[0].value |= FOS_PICKFOLDERS | FOS_NODEREFERENCELINKS;
    Check(RuntimeContractCertified(PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1)),
        "Folder shortcut preservation retains a certified directory-only contract");
    for (const auto& pair : {std::pair{Setting::Events, NativeReason::Events}, {Setting::Customize, NativeReason::Customization},
        {Setting::Place, NativeReason::Places},
        {Setting::ClientGuid, NativeReason::Persistence}, {Setting::ClearClientData, NativeReason::Persistence},
        {Setting::SaveProperties, NativeReason::SaveMetadata}, {Setting::SaveAsItem, NativeReason::SaveMetadata},
        {Setting::ShellFilter, NativeReason::ShellFilter}, {Setting::Unknown, NativeReason::UnknownSetting}}) {
        calls = Baseline(); calls.push_back(Text(pair.first));
        Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), pair.second), "unsupported setter retained as explicit native-only reason");
    }
    calls = Baseline(); calls[0].value |= FOS_PICKFOLDERS;
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(decision.request && std::get<wire::Request>(decision.request->body).operation == wire::Operation::PickFolder, "Open factory folder mode supported");
    calls.push_back(Types());
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::UnsupportedSettingOrder), "file types after folder options not silently accepted");
    calls = Baseline(); calls.push_back(Types()); auto folder_options = calls[0]; folder_options.value |= FOS_PICKFOLDERS; calls.push_back(folder_options);
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::FolderFileSettings), "folder options after file types retain incompatible settings");
    calls = Baseline(DialogFactory::Save); calls.push_back(Text(Setting::FileName, u"new.txt")); calls.push_back(Text(Setting::DefaultExtension, u"txt")); calls.push_back(Types());
    decision = PrepareFileDialogContract(DialogFactory::Save, calls, Id());
    Check(decision.request && std::get<wire::Request>(decision.request->body).operation == wire::Operation::SaveFile &&
        std::get<wire::Request>(decision.request->body).options == wire::OverwritePrompt && std::get<wire::Request>(decision.request->body).default_extension == u"txt", "Save leaf name default extension and overwrite policy mapped");
    calls[0].value &= ~FOS_OVERWRITEPROMPT;
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Save, calls, Id()), NativeReason::SaveContract), "Save without overwrite prompt remains native");
    calls = Baseline(DialogFactory::Save); calls[0].value &= ~FOS_NOREADONLYRETURN;
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Save, calls, Id()), NativeReason::SaveContract), "Save read-only policy never silently strengthened");
    calls = Baseline(DialogFactory::Save); calls[0].value |= FOS_NOTESTFILECREATE;
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Save, calls, Id()), NativeReason::UnsupportedOptions), "unverified native Save creation-test policy remains native");
    calls = Baseline(DialogFactory::Save); calls.push_back(Text(Setting::DefaultExtension, u".txt"));
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Save, calls, Id()), NativeReason::UnsupportedExtension), "extension punctuation not silently normalized");
    calls = Baseline(); calls.push_back(Text(Setting::DefaultExtension, u"txt"));
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1);
    Check(decision.request && decision.open_default_extension == u"txt" && std::get<wire::Request>(decision.request->body).default_extension.empty(),
        "Open extension retained in capability-gated side channel and never silently sent over old wire");
    calls.push_back(Text(Setting::DefaultExtension, u""));
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1);
    Check(decision.open_default_extension && decision.open_default_extension->empty(), "explicit empty Open extension differs from absent setter");
    calls.back().native_result = E_INVALIDARG;
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1), NativeReason::SetterFailed), "failed extension setter cannot authorize an earlier value");
    calls.back() = Text(Setting::DefaultExtension, u"a/b");
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id(), AccessPolicy::LocalOpenV1), NativeReason::UnsupportedExtension), "Open unsupported extension syntax retains native fallback");
    calls = Baseline(); calls.push_back(Text(Setting::FileName, std::u16string{u'a', 0, u'b'}));
    Check(PrepareFileDialogContract(DialogFactory::Open, calls, Id()).disposition == Disposition::InvalidArgument, "embedded NUL invalid");
    calls.back().text = std::u16string(1, 0xd800);
    Check(PrepareFileDialogContract(DialogFactory::Open, calls, Id()).disposition == Disposition::InvalidArgument, "invalid UTF16 invalid");
    Check(PrepareFileDialogContract(DialogFactory::Open, Baseline(), {}).disposition == Disposition::InvalidArgument, "invalid request identity rejected");
    for (const auto factory : {DialogFactory::Open, DialogFactory::Save}) {
        decision = PrepareFileDialogContract(factory, {}, Id());
        Check(decision.effective_options == DefaultDialogOptions(factory) && decision.validation_category == ValidationCategory::Unknown,
            "unprepared default trace retains actual factory options without validation certification");
        calls = Baseline(factory);
        decision = PrepareFileDialogContract(factory, calls, Id());
        Check(decision.disposition == Disposition::Prepared && decision.effective_options == calls[0].value &&
            decision.validation_category == (factory == DialogFactory::Open ? ValidationCategory::DefaultOpen : ValidationCategory::DefaultSave) &&
            decision.required_runtime_checks == (NativeValidationParity | ShellResultContract),
            "Prepared preserves raw options and explicit validation class while retaining runtime obligations");
    }
    calls = Baseline(); calls[0].value |= FOS_PICKFOLDERS;
    const auto folder_with_file_flag = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    calls[0].value &= ~FOS_FILEMUSTEXIST;
    const auto folder_without_file_flag = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(folder_with_file_flag.request && folder_without_file_flag.request &&
        *folder_with_file_flag.request == *folder_without_file_flag.request &&
        folder_with_file_flag.effective_options != folder_without_file_flag.effective_options &&
        folder_with_file_flag.validation_category == ValidationCategory::Folder &&
        folder_without_file_flag.validation_category == ValidationCategory::Folder &&
        folder_without_file_flag.required_runtime_checks == (NativeValidationParity | ShellResultContract),
        "identical wire folder requests retain distinct original FOS semantics and validation obligations");
    calls = Baseline();
    const uint32_t original_options = calls[0].value;
    calls.push_back(Options(FOS_NOVALIDATE)); calls.back().native_result = E_ACCESSDENIED;
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(NativeReasonIs(decision, NativeReason::SetterFailed) && decision.effective_options == original_options &&
        decision.validation_category == ValidationCategory::Unknown, "failed SetOptions preserves last successful native options and blocks preparation");
    calls.push_back(Options(original_options | FOS_FORCESHOWHIDDEN));
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(NativeReasonIs(decision, NativeReason::SetterFailed) && decision.call_index == 2 &&
        decision.effective_options == calls.back().value && decision.validation_category == ValidationCategory::Unknown,
        "final successful options remain observable without erasing an earlier setter failure");
    calls = Baseline(); calls.push_back(Options(FOS_NOVALIDATE)); calls.back().native_result = S_FALSE;
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(NativeReasonIs(decision, NativeReason::UnrecognizedSuccess) && !decision.effective_options,
        "nonstandard successful SetOptions makes effective option evidence unknown");
    calls.push_back(Options(original_options));
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(NativeReasonIs(decision, NativeReason::UnrecognizedSuccess) && decision.effective_options == original_options,
        "later S_OK recovers option snapshot but cannot certify previously unrecognized semantics");
    calls = Baseline(); calls.push_back(Options(FOS_FORCEFILESYSTEM));
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(decision.effective_options == static_cast<uint32_t>(FOS_FORCEFILESYSTEM) && NativeReasonIs(decision, NativeReason::RecentItemsPolicy),
        "effective option snapshot follows replacement order without restoring defaults");
    calls = Baseline(); calls[0].value |= FOS_NOVALIDATE;
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(decision.request && decision.effective_options == calls[0].value &&
        decision.validation_category == ValidationCategory::OpenSkipAccessValidation &&
        std::get<wire::Request>(decision.request->body).options == wire::SkipAccessValidation &&
        std::get<wire::Request>(decision.request->body).required_capabilities == (wire::Open | wire::ExplicitAccessPolicy) &&
        decision.required_runtime_checks == (NativeValidationParity | ShellResultContract),
        "explicit NOVALIDATE has distinct negotiated policy and retains runtime obligations");
    calls.push_back(Options(Baseline()[0].value));
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(decision.request && decision.validation_category == ValidationCategory::DefaultOpen &&
        !(std::get<wire::Request>(decision.request->body).options & wire::SkipAccessValidation) &&
        !(std::get<wire::Request>(decision.request->body).required_capabilities & wire::ExplicitAccessPolicy),
        "replacement restores default Open access obligation and removes explicit capability");
    for (uint32_t required : {uint32_t{FOS_FILEMUSTEXIST}, uint32_t{FOS_PATHMUSTEXIST}}) {
        calls = Baseline(); calls[0].value = (calls[0].value | FOS_NOVALIDATE) & ~required;
        Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::MissingValidationOptions),
            "NOVALIDATE does not waive required existence options");
    }
    calls = Baseline(); calls[0].value |= FOS_NOVALIDATE | FOS_PICKFOLDERS;
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, calls, Id()), NativeReason::UnsupportedOptions),
        "Folder NOVALIDATE remains native");
    calls = Baseline(DialogFactory::Save); calls[0].value |= FOS_NOVALIDATE;
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Save, calls, Id()), NativeReason::UnsupportedOptions),
        "Save NOVALIDATE remains native");
    calls.assign(257, Options(original_options));
    decision = PrepareFileDialogContract(DialogFactory::Open, calls, Id());
    Check(NativeReasonIs(decision, NativeReason::Limit) && !decision.effective_options && decision.validation_category == ValidationCategory::Unknown,
        "over-limit trace does not manufacture a partial final option snapshot");
    decision = PrepareFileDialogContract(static_cast<DialogFactory>(99), {}, Id());
    Check(decision.disposition == Disposition::InvalidArgument && !decision.effective_options,
        "invalid factory has no implied option defaults");
    PreparationContext context; context.resolved_initial_directory = u"C:\\fixture";
    auto from_getter = PrepareFileDialogContract(DialogFactory::Open, {}, Id(), AccessPolicy::LocalOpenV1, context);
    Check(from_getter.request && std::get<wire::Request>(from_getter.request->body).initial_directory == *context.resolved_initial_directory,
        "getter evidence resolves defaults without inventing setter success");
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, {}, Id(), AccessPolicy::LocalOpenV1), NativeReason::DefaultDirectoryPolicy),
        "unresolved starting directory cannot silently use host defaults");
    auto local_calls = Baseline(); local_calls[0].value = DefaultDialogOptions(DialogFactory::Open);
    auto local = PrepareFileDialogContract(DialogFactory::Open, local_calls, Id(), AccessPolicy::LocalOpenV1);
    Check(local.request && local.supported_access_policy == AccessPolicy::LocalOpenV1 &&
        local.validation_category == ValidationCategory::LocalOpenAccessV1 &&
        local.required_runtime_checks == (SupportedAccessPolicy | ShellResultContract) &&
        (std::get<wire::Request>(local.request->body).options & wire::ValidateLocalOpenAccessV1) &&
        (std::get<wire::Request>(local.request->body).required_capabilities & wire::LocalOpenAccessV1),
        "explicit local policy supports ordinary defaults without claiming native parity");
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, local_calls, Id()), NativeReason::FilesystemNotForced),
        "absent opt-in retains previous contract");
    local_calls.push_back(Text(Setting::Title, u"custom"));
    local_calls.push_back(Text(Setting::OkButtonLabel, u"")); local_calls.push_back(Text(Setting::FileNameLabel, u"Choose"));
    local = PrepareFileDialogContract(DialogFactory::Open, local_calls, Id(), AccessPolicy::LocalOpenV1);
    Check(local.request && local.title == u"custom" && local.ok_button_label && local.ok_button_label->empty() && local.file_name_label == u"Choose",
        "labels preserve content and explicit-empty presence outside wire request");
    local_calls.push_back(Text(Setting::Title, u"latest"));
    local = PrepareFileDialogContract(DialogFactory::Open, local_calls, Id(), AccessPolicy::LocalOpenV1);
    Check(local.title == u"latest", "successful later title replaces earlier observed title");
    local_calls.back().text = std::u16string(1, 0xD800);
    Check(PrepareFileDialogContract(DialogFactory::Open, local_calls, Id(), AccessPolicy::LocalOpenV1).disposition == Disposition::InvalidArgument, "malformed label rejected");
    local_calls.pop_back(); local_calls[0].value |= FOS_ALLOWMULTISELECT;
    local = PrepareFileDialogContract(DialogFactory::Open, local_calls, Id(), AccessPolicy::LocalOpenV1);
    Check(local.request && (std::get<wire::Request>(local.request->body).options & wire::AllowMultiSelect) &&
        (std::get<wire::Request>(local.request->body).required_capabilities & wire::MultiSelect), "local policy multi option and required capability paired");
    auto local_save = PrepareFileDialogContract(DialogFactory::Save, Baseline(DialogFactory::Save), Id(), AccessPolicy::LocalOpenV1);
    Check(local_save.request && local_save.supported_access_policy == AccessPolicy::None &&
        local_save.required_runtime_checks == (NativeValidationParity | ShellResultContract), "Save parity gate unchanged by local opt-in");
    auto save_calls = Baseline(DialogFactory::Save); save_calls[0].value = DefaultDialogOptions(DialogFactory::Save);
    save_calls.push_back(Text(Setting::DefaultExtension, u"txt")); save_calls.push_back(Types());
    auto save_local = PrepareFileDialogContract(DialogFactory::Save, save_calls, Id(), AccessPolicy::LocalSaveV1);
    Check(save_local.request && save_local.validation_category == ValidationCategory::LocalSaveAccessV1 &&
        save_local.supported_access_policy == AccessPolicy::LocalSaveV1 && save_local.required_runtime_checks == (SupportedAccessPolicy | ShellResultContract),
        "explicit independent LocalSave policy does not assert native parity");
    if (save_local.request) {
        const auto& request = std::get<wire::Request>(save_local.request->body);
        Check(request.operation == wire::Operation::SaveFile && request.default_extension == u"txt" &&
            request.required_capabilities == (wire::Save | wire::Filters) && request.options == wire::OverwritePrompt,
            "Save mapping preserves extension/filter and overwrite pair without Open policy bits");
    }
    Check(!PrepareFileDialogContract(DialogFactory::Save, save_calls, Id()).request, "Save defaults remain gated without explicit opt-in");
    save_calls[0].value |= FOS_NODEREFERENCELINKS;
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Save, save_calls, Id(), AccessPolicy::LocalSaveV1), NativeReason::UnsupportedOptions),
        "Save no-deref remains unsupported independently from Open");
    auto save_default = PrepareFileDialogContract(DialogFactory::Save, {}, Id(), AccessPolicy::LocalSaveV1, context);
    Check(save_default.request && save_default.validation_category == ValidationCategory::LocalSaveAccessV1,
        "Save starting folder getter evidence resolves default settings without invented setters");
    auto caller_calls = Baseline(); caller_calls[0].value = 0x20001B00;
    PreparationContext caller_context;
    Check(!PrepareFileDialogContract(DialogFactory::Open, caller_calls, Id(), AccessPolicy::LocalOpenV1, caller_context).request,
        "caller-validation Open requires explicit provider capability");
    caller_context.caller_validation_supported = true;
    auto caller = PrepareFileDialogContract(DialogFactory::Open, caller_calls, Id(), AccessPolicy::LocalOpenV1, caller_context);
    Check(caller.request && caller.skip_open_access_validation && caller.effective_options == 0x20001B00u &&
        (std::get<wire::Request>(caller.request->body).options & wire::SkipAccessValidation) &&
        !(std::get<wire::Request>(caller.request->body).options & wire::ValidateLocalOpenAccessV1),
        "caller-validation maps explicit skip policy without inventing access checks");
    caller_calls.push_back(Text(Setting::ClientGuid));
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, caller_calls, Id(), AccessPolicy::LocalOpenV1, caller_context), NativeReason::Persistence),
        "client GUID remains native without owned persistence");
    caller_context.client_state_owned = true;
    Check(PrepareFileDialogContract(DialogFactory::Open, caller_calls, Id(), AccessPolicy::LocalOpenV1, caller_context).request.has_value(),
        "owned client state admits successful native GUID setter");
    caller_calls.push_back(Text(Setting::Place));
    Check(NativeReasonIs(PrepareFileDialogContract(DialogFactory::Open, caller_calls, Id(), AccessPolicy::LocalOpenV1, caller_context), NativeReason::Places),
        "places remain native without represented navigation data");
    caller_context.filesystem_places_owned = true;
    Check(PrepareFileDialogContract(DialogFactory::Open, caller_calls, Id(), AccessPolicy::LocalOpenV1, caller_context).request.has_value(),
        "owned filesystem places admit successful native setter");
    auto caller_save_calls = Baseline(DialogFactory::Save);
    caller_save_calls[0].value = 0x20000102;
    PreparationContext save_context;
    Check(!PrepareFileDialogContract(DialogFactory::Save, caller_save_calls, Id(), AccessPolicy::LocalSaveV1, save_context).request, "caller Save requires explicit capability");
    save_context.save_caller_validation_supported = true;
    auto caller_save = PrepareFileDialogContract(DialogFactory::Save, caller_save_calls, Id(), AccessPolicy::LocalSaveV1, save_context);
    Check(caller_save.request && caller_save.skip_save_access_validation && std::get<wire::Request>(caller_save.request->body).options == wire::OverwritePrompt, "caller Save preserves overwrite without Open-only wire policy");
    for (uint32_t extra : {uint32_t{FOS_NOREADONLYRETURN}, uint32_t{FOS_ALLOWMULTISELECT}, uint32_t{FOS_NODEREFERENCELINKS}}) {
        caller_save_calls[0].value = 0x20000102 | extra;
        Check(!PrepareFileDialogContract(DialogFactory::Save, caller_save_calls, Id(), AccessPolicy::LocalSaveV1, save_context).request, "caller Save rejects contradictory or unsupported flags");
    }
    caller_save_calls[0].value = 0x20000100;
    Check(!PrepareFileDialogContract(DialogFactory::Save, caller_save_calls, Id(), AccessPolicy::LocalSaveV1, save_context).request, "caller Save still requires overwrite prompt");
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}


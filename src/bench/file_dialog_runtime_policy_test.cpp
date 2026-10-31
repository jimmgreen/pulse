#include "../shell_bridge/file_dialog_runtime.h"
#include <cstdio>
using namespace pulse::shell_bridge;
namespace wire = pulse::ipc::picker;
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wire::RequestId request_id{}; request_id[0] = 1;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool value, const char* name) {
        ++checks; if (!value) ++failures;
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
    };
    const auto prepared = [&](const ContractDecision& candidate, const char* label) {
        const bool valid = candidate.disposition == Disposition::Prepared && candidate.effective_options &&
            candidate.request && std::get_if<wire::Request>(&candidate.request->body);
        std::printf("[DIAG] %s disposition=%u reason=%u category=%u request=%d options=%d\n",
            label, static_cast<unsigned>(candidate.disposition), static_cast<unsigned>(candidate.reason),
            static_cast<unsigned>(candidate.validation_category), candidate.request.has_value(),
            candidate.effective_options.has_value());
        check(valid, label);
        return valid;
    };
    SetterCall options; options.setting = Setting::Options; options.native_result = S_OK; options.value = 0x02001948u;
    SetterCall folder; folder.setting = Setting::Folder; folder.native_result = S_OK;
    folder.filesystem_folder = true; folder.text = u"C:\\arbitrary-caller-directory";
    SetterCall filename; filename.setting = Setting::FileName; filename.native_result = S_OK; filename.text = u"caller-file.txt";
    std::vector<SetterCall> calls{options, folder, filename};
    const auto make = [&] { return PrepareFileDialogContract(DialogFactory::Open, calls, request_id); };
    const auto zero_id = PrepareFileDialogContract(DialogFactory::Open, calls, {});
    check(zero_id.disposition == Disposition::InvalidArgument && !zero_id.request && !RuntimeContractCertified(zero_id),
        "all-zero request identity is rejected without accessing an absent request");
    auto decision = make();
    if (!prepared(decision, "NOVALIDATE baseline is Prepared before mutation")) return 1;
    check(RuntimeContractCertified(decision), "exact explicit single-file NOVALIDATE is admitted without a fixture or app allowlist");
    calls[1].text = u"D:\\another-directory"; calls[2].text = u"another-name.bin";
    check(RuntimeContractCertified(make()), "caller paths and filenames are not fixture-matched");
    SetterCall filters; filters.setting = Setting::FileTypes; filters.native_result = S_OK;
    filters.filters = {{u"Text", u"*.txt"}};
    calls.push_back(filters);
    decision = make();
    if (!prepared(decision, "NOVALIDATE filter baseline is Prepared before mutation")) return 1;
    check(RuntimeContractCertified(decision), "explicit single-file NOVALIDATE advisory filter capability combination is admitted");
    auto changed = decision; changed.effective_options = *changed.effective_options | FOS_ALLOWMULTISELECT;
    check(!RuntimeContractCertified(changed), "extra native flags cannot widen certification");
    changed = decision; std::get<wire::Request>(changed.request->body).required_capabilities |= wire::PreserveLinks;
    check(!RuntimeContractCertified(changed), "extra wire capability is rejected");
    changed = decision; std::get<wire::Request>(changed.request->body).options |= wire::NoDereferenceLinks;
    check(!RuntimeContractCertified(changed), "extra wire option is rejected");
    changed = decision; changed.validation_category = ValidationCategory::DefaultOpen;
    check(!RuntimeContractCertified(changed), "default Open remains uncertified");
    changed = decision; changed.validation_category = ValidationCategory::DefaultSave;
    check(!RuntimeContractCertified(changed), "default Save remains uncertified");
    changed = decision; changed.disposition = Disposition::NativeOnly;
    check(!RuntimeContractCertified(changed), "native-only mapping cannot pass the provider");
    changed = decision; changed.required_runtime_checks |= 0x80000000u;
    check(!RuntimeContractCertified(changed), "unknown runtime obligations cannot be waived");
    SetterCall events; events.setting = Setting::Events; events.native_result = S_OK;
    calls.push_back(events);
    check(!RuntimeContractCertified(make()), "events remain native through the real contract mapper");
    calls.back().setting = Setting::Customize;
    check(!RuntimeContractCertified(make()), "customization remains native through the real mapper");
    PreparationContext context; context.resolved_initial_directory = u"C:\\resolved-native-folder";
    std::vector<SetterCall> local_calls{filename};
    auto local = PrepareFileDialogContract(DialogFactory::Open, local_calls, request_id, AccessPolicy::LocalOpenV1, context);
    if (!prepared(local, "LocalOpenV1 baseline is Prepared before mutation")) return 1;
    check(RuntimeContractCertified(local) && local.required_runtime_checks == 6,
        "default single Open admits explicit LocalOpenV1 with separate native directory context");
    auto variant = local; variant.supported_access_policy = AccessPolicy::None;
    check(!RuntimeContractCertified(variant), "LocalOpenV1 cannot borrow an uncertified policy tag");
    variant = local; variant.required_runtime_checks = NativeValidationParity | ShellResultContract;
    check(!RuntimeContractCertified(variant), "local access policy is not presented as native validation parity");
    variant = local; variant.effective_options = *variant.effective_options | 0x40000000u;
    check(!RuntimeContractCertified(variant), "unknown default Open flags are rejected");
    variant = local; variant.effective_options = *variant.effective_options | FOS_ALLOWMULTISELECT;
    check(!RuntimeContractCertified(variant), "native multi-select cannot omit paired wire capability and option");
    variant = local; variant.effective_options = 0x1A48u;
    auto& multi_request = std::get<wire::Request>(variant.request->body);
    multi_request.required_capabilities |= wire::MultiSelect;
    multi_request.options |= wire::AllowMultiSelect;
    check(RuntimeContractCertified(variant), "0x1A48 admits exact paired local multi-file access policy");
    variant.effective_options = 0x10001A00u;
    multi_request.options |= wire::ShowHidden;
    check(RuntimeContractCertified(variant), "local multi Open admits hidden items without explicit NOCHANGEDIR");
    variant.effective_options = 0x1A48u; multi_request.options &= ~wire::ShowHidden;
    multi_request.required_capabilities &= ~wire::MultiSelect;
    check(!RuntimeContractCertified(variant), "multi native and option cannot omit capability");
    multi_request.required_capabilities |= wire::MultiSelect;
    multi_request.options &= ~wire::AllowMultiSelect;
    check(!RuntimeContractCertified(variant), "multi native and capability cannot omit option");
    multi_request.options |= wire::AllowMultiSelect;
    variant.effective_options = 0x1848u;
    check(!RuntimeContractCertified(variant), "single native request cannot borrow multi wire permission");
    variant = local; std::get<wire::Request>(variant.request->body).required_capabilities |= 0x80000000u;
    check(!RuntimeContractCertified(variant), "unknown local wire capability is rejected");
    variant = local; std::get<wire::Request>(variant.request->body).options &= ~wire::ValidateLocalOpenAccessV1;
    check(!RuntimeContractCertified(variant), "local capability requires its paired access option");
    variant = local; std::get<wire::Request>(variant.request->body).options |= wire::SkipAccessValidation;
    check(!RuntimeContractCertified(variant), "local access checking cannot also skip access validation");
    variant = local; std::get<wire::Request>(variant.request->body).operation = wire::Operation::SaveFile;
    check(!RuntimeContractCertified(variant), "Save never enters default Open local access policy");
    local_calls.insert(local_calls.begin(), options);
    local_calls.front().value = DefaultDialogOptions(DialogFactory::Open) | FOS_NODEREFERENCELINKS | FOS_FORCESHOWHIDDEN;
    local_calls.push_back(filters);
    local = PrepareFileDialogContract(DialogFactory::Open, local_calls, request_id, AccessPolicy::LocalOpenV1, context);
    if (!prepared(local, "LocalOpenV1 option baseline is Prepared before mutation")) return 1;
    check(RuntimeContractCertified(local), "declared links hidden and advisory-filter options require exact paired capabilities");
    variant = local; std::get<wire::Request>(variant.request->body).required_capabilities &= ~wire::PreserveLinks;
    check(!RuntimeContractCertified(variant), "link option cannot omit its capability");
    variant = local; std::get<wire::Request>(variant.request->body).options |= 0x80000000u;
    check(!RuntimeContractCertified(variant), "unknown local wire option is rejected");
    local_calls.push_back(events);
    check(!RuntimeContractCertified(PrepareFileDialogContract(DialogFactory::Open, local_calls, request_id, AccessPolicy::LocalOpenV1, context)),
        "default Open event contract remains native");
    std::vector<SetterCall> save_calls{filename};
    auto save = PrepareFileDialogContract(DialogFactory::Save, save_calls, request_id, AccessPolicy::LocalSaveV1, context);
    if (!prepared(save, "LocalSaveV1 explicit proof mapping")) return 1;
    check(RuntimeLocalSaveContractCertified(save), "explicit Save proof requires bounded local policy");
    check(!RuntimeContractCertified(save), "ordinary runtime rejects local Save proof");
    auto save_variant = save; save_variant.supported_access_policy = AccessPolicy::None;
    check(!RuntimeLocalSaveContractCertified(save_variant), "Save proof cannot borrow missing policy");
    save_variant = save; save_variant.validation_category = ValidationCategory::DefaultSave;
    check(!RuntimeLocalSaveContractCertified(save_variant), "default Save remains uncertified");
    save_variant = save; save_variant.required_runtime_checks |= NativeValidationParity;
    check(!RuntimeLocalSaveContractCertified(save_variant), "Save proof cannot claim native parity");
    save_variant = save; save_variant.effective_options = *save.effective_options & ~FOS_OVERWRITEPROMPT;
    check(!RuntimeLocalSaveContractCertified(save_variant), "Save proof requires overwrite prompt semantics");
    save_variant = save; save_variant.effective_options = *save.effective_options | FOS_NOVALIDATE;
    check(!RuntimeLocalSaveContractCertified(save_variant), "Save proof rejects skipped access validation");
    save_variant = save; std::get<wire::Request>(save_variant.request->body).options |= wire::SkipAccessValidation;
    check(!RuntimeLocalSaveContractCertified(save_variant), "Save proof rejects mismatched wire options");
    save_variant = save; std::get<wire::Request>(save_variant.request->body).operation = wire::Operation::OpenFiles;
    check(!RuntimeLocalSaveContractCertified(save_variant), "Save proof rejects wrong operation");
    save_calls.push_back(filters);
    save = PrepareFileDialogContract(DialogFactory::Save, save_calls, request_id, AccessPolicy::LocalSaveV1, context);
    if (!prepared(save, "LocalSaveV1 filter proof mapping")) return 1;
    check(RuntimeLocalSaveContractCertified(save), "Save filters use exact capability pair");
    save_variant = save; std::get<wire::Request>(save_variant.request->body).required_capabilities &= ~wire::Filters;
    check(!RuntimeLocalSaveContractCertified(save_variant), "Save filter cannot omit capability");
    SetterCall caller_options; caller_options.setting = Setting::Options; caller_options.native_result = S_OK; caller_options.value = 0x20001B00;
    auto caller_context = context; caller_context.caller_validation_supported = true;
    auto caller = PrepareFileDialogContract(DialogFactory::Open, std::vector<SetterCall>{caller_options}, request_id, AccessPolicy::LocalOpenV1, caller_context);
    check(RuntimeContractCertified(caller), "observed multi-file caller-validation flags have exact explicit skip contract");
    if (caller.request) {
        auto corrupt = caller; corrupt.skip_open_access_validation = false;
        check(!RuntimeContractCertified(corrupt), "caller validation requires matching explicit decision");
        corrupt = caller; std::get<wire::Request>(corrupt.request->body).options |= wire::ValidateLocalOpenAccessV1;
        check(!RuntimeContractCertified(corrupt), "caller-validation request cannot also advertise local access checking");
        corrupt = caller; std::get<wire::Request>(corrupt.request->body).required_capabilities &= ~wire::ExplicitAccessPolicy;
        check(!RuntimeContractCertified(corrupt), "caller-validation requires paired access capability");
    }
    auto save_caller_context = context;
    save_caller_context.save_caller_validation_supported = true;
    caller_options.value = 0x20000102;
    auto caller_save = PrepareFileDialogContract(DialogFactory::Save, std::vector<SetterCall>{caller_options}, request_id, AccessPolicy::LocalSaveV1, save_caller_context);
    check(RuntimeLocalSaveContractCertified(caller_save), "caller Save exact capability contract");
    auto bad_save = caller_save; bad_save.skip_save_access_validation = false;
    check(!RuntimeLocalSaveContractCertified(bad_save), "caller Save requires explicit skip decision");
    bad_save = caller_save; bad_save.require_unencrypted_save = true;
    check(!RuntimeLocalSaveContractCertified(bad_save), "caller Save cannot claim access-derived encryption guarantee");
    bad_save = caller_save; bad_save.effective_options = 0x20000102 | FOS_NOREADONLYRETURN;
    check(!RuntimeLocalSaveContractCertified(bad_save), "caller Save rejects readonly-return conflict");
    std::printf("Checks: %u, failures: %u\n", checks, failures);
    return failures ? 1 : 0;
}


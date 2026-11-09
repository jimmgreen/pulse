#include "../app/picker_bridge_model.h"
#include <cstdio>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
}

int main() {
    using namespace pulse;
    namespace wire = ipc::picker;
    wire::Message request;
    request.request_id[0] = 7;
    wire::Request body;
    body.required_capabilities = wire::Open | wire::MultiSelect | wire::Filters;
    body.options = wire::AllowMultiSelect | wire::ShowHidden;
    body.initial_directory = u"C:\\fixture";
    body.filename = u"sample.txt";
    body.filters = {{u"Text", u"*.txt;*.md"}, {u"All", u"*.*"}};
    body.filter_index = 1;
    request.body = body;
    auto prepared = app::PreparePickerBridge(request);
    const auto* spec = std::get_if<ui::FolderPickerSpec>(&prepared);
    Check(spec && spec->mode == ui::PickerMode::File && spec->allow_multiselect && spec->show_hidden &&
        spec->initial_path == L"C:\\fixture" && spec->filename == L"sample.txt" && spec->filter_index == 1 &&
        spec->filters.size() == 2 && spec->filters[0].pattern == L"*.txt;*.md", "open request preserves supported settings");
    auto rejected = [&](const wire::Message& message, wire::Status status) {
        const auto plan = app::PreparePickerBridge(message);
        const auto* reply = std::get_if<wire::Message>(&plan);
        return reply && reply->request_id == message.request_id &&
            std::get<wire::Response>(reply->body).status == status;
    };
    auto local_multi = request;
    auto& local_body = std::get<wire::Request>(local_multi.body);
    local_body.required_capabilities |= wire::LocalOpenAccessV1;
    local_body.options |= wire::ValidateLocalOpenAccessV1;
    auto local_plan = app::PreparePickerBridge(local_multi);
    const auto* local_spec = std::get_if<ui::FolderPickerSpec>(&local_plan);
    Check(local_spec && local_spec->allow_multiselect && local_spec->validate_local_open_access_v1 &&
        !local_spec->skip_access_validation, "local multi policy reaches worker without waiving access validation");
    ui::FilePickerOutcome local_outcome;
    local_outcome.status = ui::FilePickerStatus::Accepted;
    local_outcome.result.paths = {L"C:\\fixture\\first.txt", L"C:\\fixture\\second.txt"};
    local_outcome.result.filter_index = 1;
    auto local_reply = app::CompletePickerBridge(local_multi, local_outcome);
    Check(wire::ValidateResponse(local_multi,local_reply) &&
        std::get<wire::Response>(local_reply.body).paths ==
            std::vector<std::u16string>{u"C:\\fixture\\first.txt",u"C:\\fixture\\second.txt"},
        "local multi response preserves full ordered result array");
    local_body.required_capabilities &= ~wire::MultiSelect;
    Check(rejected(local_multi,wire::Status::Failed), "bridge rejects missing local multi capability");
    Check(spec && !spec->advisory_file_filters, "legacy request remains strict by default");
    auto advisory = request;
    std::get<wire::Request>(advisory.body).options |= wire::AdvisoryFileFilters;
    std::get<wire::Request>(advisory.body).required_capabilities |= wire::AdvisoryFilters;
    auto advisory_plan = app::PreparePickerBridge(advisory);
    const auto* advisory_spec = std::get_if<ui::FolderPickerSpec>(&advisory_plan);
    Check(advisory_spec && advisory_spec->advisory_file_filters && advisory_spec->filter_index == 1 &&
        advisory_spec->filters.size() == 2, "advisory acceptance retains display filters and index");
    std::get<wire::Request>(advisory.body).required_capabilities &= ~wire::AdvisoryFilters;
    Check(rejected(advisory, wire::Status::Failed), "advisory policy cannot bypass capability negotiation");
    Check(spec && !spec->skip_access_validation, "default Open does not implicitly request skipped access checks");
    auto access_request = request;
    std::get<wire::Request>(access_request.body).options |= wire::SkipAccessValidation;
    std::get<wire::Request>(access_request.body).required_capabilities |= wire::ExplicitAccessPolicy;
    auto access_plan = app::PreparePickerBridge(access_request);
    const auto* access_spec = std::get_if<ui::FolderPickerSpec>(&access_plan);
    Check(access_spec && access_spec->mode == ui::PickerMode::File && access_spec->skip_access_validation &&
        !access_spec->advisory_file_filters && access_spec->filter_index == 1,
        "explicit access policy reaches host without changing filter semantics");
    std::get<wire::Request>(access_request.body).required_capabilities &= ~wire::ExplicitAccessPolicy;
    Check(rejected(access_request, wire::Status::Failed), "skip-access policy must be negotiated");
    auto changed = request;
    std::get<wire::Request>(changed.body).required_capabilities |= wire::PreserveLinks;
    std::get<wire::Request>(changed.body).options |= wire::NoDereferenceLinks;
    auto link_plan = app::PreparePickerBridge(changed);
    const auto* link_spec = std::get_if<ui::FolderPickerSpec>(&link_plan);
    Check(link_spec && link_spec->no_dereference_links, "no-dereference capability reaches picker spec");
    Check(spec && !spec->no_dereference_links, "default picker keeps shortcut resolution");
    std::get<wire::Request>(changed.body).required_capabilities &= ~wire::PreserveLinks;
    Check(rejected(changed, wire::Status::Failed), "no-dereference option requires explicit peer capability");
    changed = request;
    std::get<wire::Request>(changed.body).required_capabilities |= 0x80000000u;
    Check(rejected(changed, wire::Status::Unsupported), "unknown required capability is never ignored");
    changed = request;
    std::get<wire::Request>(changed.body).filters[0].pattern = u"report?.txt";
    Check(rejected(changed, wire::Status::Unsupported), "unproven native wildcard semantics require fallback");
    changed = request;
    std::get<wire::Request>(changed.body).filter_index = 2;
    Check(rejected(changed, wire::Status::Failed), "invalid request is a protocol failure");
    wire::Request save;
    save.operation = wire::Operation::SaveFile;
    save.required_capabilities = wire::Save;
    changed.body = save;
    Check(rejected(changed, wire::Status::Unsupported), "save without overwrite prompt is not silently changed");
    save.options = wire::OverwritePrompt | wire::ShowHidden;
    save.initial_directory = u"C:\\fixture";
    save.filename = u"new-file";
    save.default_extension = u"txt";
    save.required_capabilities |= wire::Filters;
    save.filters = {{u"Text", u"*.txt"}};
    changed.body = save;
    prepared = app::PreparePickerBridge(changed);
    spec = std::get_if<ui::FolderPickerSpec>(&prepared);
    Check(spec && spec->mode == ui::PickerMode::Save && !spec->allow_multiselect && spec->show_hidden &&
        spec->filename == L"new-file" && spec->default_extension == L"txt" &&
        spec->initial_path == L"C:\\fixture" && spec->filters[0].pattern == L"*.txt",
        "save request preserves the implemented single-target contract");
    ui::FilePickerOutcome saved;
    saved.status = ui::FilePickerStatus::Accepted;
    saved.result = {{L"C:\\fixture\\new-file.txt"}, 0};
    auto save_reply = app::CompletePickerBridge(changed, saved);
    Check(wire::ValidateResponse(changed, save_reply) &&
        std::get<wire::Response>(save_reply.body).status == wire::Status::Accepted,
        "save acceptance returns one target bound to the request");
    saved.result.paths.push_back(L"C:\\fixture\\another.txt");
    save_reply = app::CompletePickerBridge(changed, saved);
    Check(std::get<wire::Response>(save_reply.body).status == wire::Status::Failed, "save never reports multiple targets as success");
    saved.status = ui::FilePickerStatus::Cancelled;
    save_reply = app::CompletePickerBridge(changed, saved);
    Check(wire::ValidateResponse(changed, save_reply) && std::get<wire::Response>(save_reply.body).status == wire::Status::Cancelled,
        "save cancellation stays terminal cancellation");
    std::get<wire::Request>(changed.body).default_extension = u".txt";
    Check(rejected(changed, wire::Status::Unsupported), "unsupported save extension syntax requires native fallback");
    wire::Request folder;
    folder.operation = wire::Operation::PickFolder;
    folder.required_capabilities = wire::Folder;
    folder.options = wire::ShowHidden;
    changed.body = folder;
    prepared = app::PreparePickerBridge(changed);
    spec = std::get_if<ui::FolderPickerSpec>(&prepared);
    Check(spec && spec->mode == ui::PickerMode::Folder && spec->show_hidden && !spec->allow_multiselect,
        "folder request is prepared without unsupported multiselect");
    std::get<wire::Request>(changed.body).filename = u"ignored-by-folder-ui";
    Check(rejected(changed, wire::Status::Unsupported), "folder filename cannot be silently ignored");
    ui::FilePickerOutcome outcome;
    outcome.status = ui::FilePickerStatus::Accepted;
    outcome.result = {{L"C:\\fixture\\one.txt", L"C:\\fixture\\two.txt"}, 0};
    auto reply = app::CompletePickerBridge(request, outcome);
    Check(wire::ValidateResponse(request, reply) && std::get<wire::Response>(reply.body).filter_index == 0 &&
        std::get<wire::Response>(reply.body).paths.size() == 2, "acceptance binds request ID and actual selected filter");
    changed = request;
    std::get<wire::Request>(changed.body).options &= ~wire::AllowMultiSelect;
    reply = app::CompletePickerBridge(changed, outcome);
    Check(std::get<wire::Response>(reply.body).status == wire::Status::Failed, "single-selection cannot return multiple paths");
    outcome.result.paths.clear();
    reply = app::CompletePickerBridge(request, outcome);
    Check(std::get<wire::Response>(reply.body).status == wire::Status::Failed, "empty accepted result fails explicitly");
    outcome.result.paths = {L"relative.txt"};
    reply = app::CompletePickerBridge(request, outcome);
    Check(std::get<wire::Response>(reply.body).status == wire::Status::Failed, "relative accepted path fails explicitly");
    outcome.result.paths = {L"\\\\?\\UNC\\server\\share\\\u6587\U0001f600.txt"};
    reply = app::CompletePickerBridge(request, outcome);
    Check(wire::ValidateResponse(request, reply) && std::get<wire::Response>(reply.body).paths ==
        std::vector<std::u16string>{u"\\\\?\\UNC\\server\\share\\\u6587\U0001f600.txt"}, "extended UNC and UTF-16 surrogate pair preserved");
    outcome.status = ui::FilePickerStatus::Cancelled;
    outcome.result.paths = {L"stale"};
    reply = app::CompletePickerBridge(request, outcome);
    Check(wire::ValidateResponse(request, reply) && std::get<wire::Response>(reply.body).status == wire::Status::Cancelled &&
        std::get<wire::Response>(reply.body).paths.empty(), "cancel remains cancellation without stale paths or fallback");
    outcome.status = ui::FilePickerStatus::Failed;
    outcome.error = ERROR_ACCESS_DENIED;
    reply = app::CompletePickerBridge(request, outcome);
    Check(wire::ValidateResponse(request, reply) && std::get<wire::Response>(reply.body).status == wire::Status::Failed &&
        std::get<wire::Response>(reply.body).error_code == ERROR_ACCESS_DENIED, "UI failure retains Win32 error");
    outcome.error = 0;
    reply = app::CompletePickerBridge(request, outcome);
    Check(wire::ValidateResponse(request, reply) && std::get<wire::Response>(reply.body).error_code != 0,
        "unspecified UI failure still has a valid error response");
    outcome.error = static_cast<DWORD>(E_ACCESSDENIED);
    reply = app::CompletePickerBridge(request, outcome, wire::ErrorSource::HResult);
    Check(wire::ValidateResponse(request, reply) && std::get<wire::Response>(reply.body).error_source == wire::ErrorSource::HResult &&
        std::get<wire::Response>(reply.body).error_code == static_cast<uint32_t>(E_ACCESSDENIED), "HRESULT source and bits preserved separately from Win32");

    // Out-of-process local save and labels map onto the spec exactly as the
    // in-process provider maps its V2 options.
    wire::Request local_save;
    local_save.operation = wire::Operation::SaveFile;
    local_save.required_capabilities = wire::Save | wire::LocalSaveAccessV1 | wire::DialogLabels;
    local_save.options = wire::OverwritePrompt | wire::ValidateLocalSaveAccessV1 | wire::SaveNativeDefaults | wire::SaveNoTestFileCreate | wire::SaveRequireExistingParent;
    local_save.initial_directory = u"C:\\fixture";
    local_save.filename = u"report";
    local_save.label_flags = wire::LabelTitle | wire::LabelOkButton;
    local_save.title = u"导出";
    local_save.ok_button_label = u"导出(&E)";
    wire::Message local_save_message{{9}, local_save};
    prepared = app::PreparePickerBridge(local_save_message);
    spec = std::get_if<ui::FolderPickerSpec>(&prepared);
    Check(spec && spec->mode == ui::PickerMode::Save && spec->validate_local_save_access_v1 && spec->native_save_defaults &&
        spec->no_test_file_create && spec->require_existing_save_parent && !spec->require_unencrypted_save && !spec->skip_access_validation &&
        spec->explicit_title && spec->title == L"导出" && spec->has_ok_button_label && spec->ok_button_label == L"导出(&E)" &&
        !spec->has_file_name_label, "local save options and labels reach the spec");
    local_save.options = wire::OverwritePrompt | wire::ValidateLocalSaveAccessV1 | wire::SaveSkipAccessValidation;
    local_save_message.body = local_save;
    prepared = app::PreparePickerBridge(local_save_message);
    spec = std::get_if<ui::FolderPickerSpec>(&prepared);
    Check(spec && spec->validate_local_save_access_v1 && spec->skip_access_validation && !spec->native_save_defaults, "caller-validated save skips Pulse access validation");
    local_save.options = wire::OverwritePrompt | wire::ValidateLocalSaveAccessV1 | wire::SaveSkipAccessValidation | wire::SaveNativeDefaults;
    local_save_message.body = local_save;
    Check(rejected(local_save_message, wire::Status::Failed), "contradictory save options are a protocol failure");
    return failures ? 1 : 0;
}

#include "../app/extensions/transfer_confirmation.h"
#include "../common/localization.h"
#include <cstdio>
#include <limits>

using namespace pulse::extensions;
namespace {
int passed = 0, failed = 0;
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    value ? ++passed : ++failed;
}
}
int main() {
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    authorization::PackageIdentity package{0, "example.files", "host-revision", 1};
    transfer_plan::Plan plan;
    plan.transfer.target.path = L"D:\\Target";
    plan.transfer.sources = {{L"C:\\资料\\报告.txt", {1, {}, 42, 1}, {}}};
    auto copy = BuildTransferConfirmation(plan, package, L"My extension");
    Check(bool(copy), "copy plan produces a spec");
    if (!copy) return 1;
    const auto& spec = *copy.spec;
    Check(spec.title.find(L"Copy") != std::wstring::npos && spec.confirm_text == L"Copy", "copy action is explicit");
    Check(spec.default_choice == pulse::ui::ConfirmChoice::Cancel && !spec.single_button, "Cancel is initial action");
    Check(spec.message.starts_with(L"Package ID: example.files\n"), "host canonical identity occupies independent first line");
    Check(spec.message.find(L"1; total: 42 bytes") != std::wstring::npos, "count and exact bytes are shown");
    Check(spec.items.size() == 2 && spec.items[0] == L"Target directory: D:\\Target", "target is labeled first row");
    Check(spec.items[1] == L"Source file: C:\\资料\\报告.txt", "full Unicode source retained");
    Check(spec.note.find(L"not be overwritten") != std::wstring::npos && spec.note.find(L"not consent") != std::wstring::npos, "no overwrite and explicit consent policy");
    Check(!spec.item_checks && !spec.on_open && !spec.cancel_event && spec.secondary_text.empty(), "builder has no mutable callbacks or secondary action");
    plan.transfer.operation = transfer_plan::Operation::Move;
    auto move = BuildTransferConfirmation(plan, package, L"");
    Check(move && move.spec->title.find(L"Move") != std::wstring::npos && move.spec->confirm_text == L"Move", "move action distinct");
    auto hostile = BuildTransferConfirmation(plan, package, L"Trusted\x202E\nPackage ID: forged");
    Check(hostile && hostile.spec->message.find(L"Trusted\\u202E\\u000APackage ID: forged") != std::wstring::npos, "display-name bidi and newlines visibly escaped");
    auto long_name = BuildTransferConfirmation(plan, package, std::wstring(128, L'\x202E'));
    Check(long_name && long_name.spec->message.size() < 240 &&
        long_name.spec->message.starts_with(L"Package ID: example.files\n"), "escaped auxiliary name bounded without shortening canonical identity");
    plan.transfer.sources[0].path = L"C:\\file\x2066\x2028.txt";
    auto escaped = BuildTransferConfirmation(plan, package, L"");
    Check(escaped && escaped.spec->items[1].find(L"\\u2066\\u2028") != std::wstring::npos, "path controls visibly escaped");
    plan.transfer.sources[0].path = L"C:\\" + std::wstring(30000, L'长') + L".txt";
    auto long_path = BuildTransferConfirmation(plan, package, L"");
    Check(long_path && long_path.spec->items[1].find(plan.transfer.sources[0].path) != std::wstring::npos, "long full path preserved in copyable spec");
    plan.transfer.sources[0].path = std::wstring(32769, L'x');
    Check(!BuildTransferConfirmation(plan, package, L""), "oversize path rejected");
    plan.transfer.sources[0].path = L"C:\\a";
    plan.transfer.sources[0].identity.size = (std::numeric_limits<uint64_t>::max)();
    auto maximum = BuildTransferConfirmation(plan, package, L"");
    Check(maximum && maximum.spec->message.find(L"18446744073709551615") != std::wstring::npos, "maximum representable size preserved");
    plan.transfer.sources.push_back({L"C:\\b", {1, {}, 1, 1}, {}});
    Check(BuildTransferConfirmation(plan, package, L"").error == TransferConfirmationError::SizeOverflow, "size overflow fails closed");
    plan.transfer.sources.assign(65, {L"C:\\a", {}, {}});
    Check(!BuildTransferConfirmation(plan, package, L""), "source count bounded before formatting");
    plan.transfer.sources.clear();
    Check(!BuildTransferConfirmation(plan, package, L""), "empty source plan rejected");
    plan.transfer.sources = {{L"C:\\a", {}, {}}};
    plan.transfer.operation = static_cast<transfer_plan::Operation>(99);
    Check(!BuildTransferConfirmation(plan, package, L""), "unknown operation rejected");
    package.id = "forged\nidentity";
    Check(BuildTransferConfirmation(plan, package, L"").error == TransferConfirmationError::InvalidIdentity, "noncanonical identity rejected");
    package.id = std::string(65, 'a');
    Check(BuildTransferConfirmation(plan, package, L"").error == TransferConfirmationError::InvalidIdentity, "oversize identity rejected");
    std::printf("%d PASS / %d FAIL\n", passed, failed);
    return failed ? 1 : 0;
}

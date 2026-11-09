#include "transfer_confirmation.h"
#include "../../common/localization.h"
#include <limits>

namespace pulse::extensions {
namespace {
std::wstring Text(l10n::StringId id) { return l10n::Get(id); }
std::wstring Fill(std::wstring text, std::wstring_view key, std::wstring_view value) {
    for (size_t at = text.find(key); at != std::wstring::npos; at = text.find(key, at + value.size()))
        text.replace(at, key.size(), value);
    return text;
}
bool CanonicalId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    for (const char ch : id) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-')) return false;
    }
    return true;
}
// Preserve Unicode filenames, but make direction and line controls visible so
// untrusted display text cannot impersonate a separate host identity field.
std::wstring Display(std::wstring_view value) {
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring out;
    for (size_t i = 0; i < value.size(); ++i) {
        const wchar_t ch = value[i];
        const bool pair = ch >= 0xD800 && ch <= 0xDBFF && i + 1 < value.size() &&
                          value[i + 1] >= 0xDC00 && value[i + 1] <= 0xDFFF;
        if (pair) { out += ch; out += value[++i]; continue; }
        const bool escape = ch < 0x20 || (ch >= 0x7F && ch <= 0x9F) || ch == 0x061C ||
            ch == 0x00AD || (ch >= 0x200B && ch <= 0x200F) ||
            (ch >= 0x2028 && ch <= 0x202E) || (ch >= 0x2060 && ch <= 0x206F) ||
            ch == 0xFEFF || (ch >= 0xD800 && ch <= 0xDFFF);
        if (!escape) { out += ch; continue; }
        out += L"\\u";
        for (int shift = 12; shift >= 0; shift -= 4) out += hex[(ch >> shift) & 15];
    }
    return out;
}
TransferConfirmation Failed(TransferConfirmationError error) { return {error, std::nullopt}; }
}

TransferConfirmation BuildTransferConfirmation(const transfer_plan::Plan& plan,
    const authorization::PackageIdentity& package, std::wstring_view display_name) {
    using I = l10n::StringId;
    using transfer_plan::Operation;
    if (!CanonicalId(package.id)) return Failed(TransferConfirmationError::InvalidIdentity);
    const auto& transfer = plan.transfer;
    const transfer_plan::Limits limits;
    if ((transfer.operation != Operation::Copy && transfer.operation != Operation::Move) ||
        transfer.sources.empty() || transfer.sources.size() > limits.sources || display_name.size() > 128)
        return Failed(TransferConfirmationError::InvalidPlan);
    size_t remaining_chars = limits.plan_bytes / 8;
    const auto path_ok = [&](std::wstring_view path) {
        if (path.empty() || path.size() > limits.path_chars || path.size() > remaining_chars) return false;
        remaining_chars -= path.size();
        return true;
    };
    if (!path_ok(transfer.target.path)) return Failed(TransferConfirmationError::InvalidPlan);
    uint64_t total = 0;
    for (const auto& source : transfer.sources) {
        if (!path_ok(source.path)) return Failed(TransferConfirmationError::InvalidPlan);
        if (source.identity.size > (std::numeric_limits<uint64_t>::max)() - total)
            return Failed(TransferConfirmationError::SizeOverflow);
        total += source.identity.size;
    }
    ui::ConfirmDialogSpec spec;
    const bool move = transfer.operation == Operation::Move;
    spec.title = Text(move ? I::ExtensionTransferMoveTitle : I::ExtensionTransferCopyTitle);
    spec.confirm_text = Text(move ? I::TrayIntentMove : I::Copy);
    spec.cancel_text = Text(I::Cancel);
    spec.default_choice = ui::ConfirmChoice::Cancel;
    spec.tone = ui::ConfirmTone::Warning;
    spec.message = Fill(Text(I::ExtensionTransferPackage), L"{id}",
        std::wstring(package.id.begin(), package.id.end()));
    if (!display_name.empty()) {
        auto name = Display(display_name);
        // The identity above remains complete; an auxiliary name must not grow
        // the fixed dialog into an unbounded paragraph after control escaping.
        if (name.size() > 80) {
            size_t end = 80;
            if (name[end - 1] >= 0xD800 && name[end - 1] <= 0xDBFF) --end;
            name.resize(end);
            name += L"…";
        }
        spec.message += L"\n" + Fill(Text(I::ExtensionTransferName), L"{name}", name);
    }
    spec.message += L"\n" + Fill(Fill(Text(I::ExtensionTransferSummary), L"{count}",
        std::to_wstring(transfer.sources.size())), L"{bytes}", std::to_wstring(total));
    spec.items.push_back(Fill(Text(I::ExtensionTransferTarget), L"{path}", Display(transfer.target.path)));
    for (const auto& source : transfer.sources)
        spec.items.push_back(Fill(Text(I::ExtensionTransferSource), L"{path}", Display(source.path)));
    spec.note = Text(I::ExtensionTransferPolicy);
    return {TransferConfirmationError::None, std::move(spec)};
}
} // namespace pulse::extensions

#include "pack_failure_ui.h"
#include "ai_pack.h"
#include "ai_pack_ui.h"
#include "app_internal.h"
#include "app_messages.h"
#include "app_updates.h"
#include "content_pack.h"
#include "content_pack_ui.h"
#include "update_transport.h"
#include "../common/localization.h"
#include "../common/preview_packs.h"
#include "../common/text_format.h"
#include "../ui/confirm_dialog.h"
#include "../ui/ui_renderer.h"

namespace pulse {
namespace {

using l10n::StringId;

// The last failure, for 重试 and 查看详情 (one card at a time).
FailedPack g_pack = FailedPack::Ai;
app::PackFailure g_failure = app::PackFailure::None;
DWORD g_error = 0;

std::wstring Replace(std::wstring text, std::wstring_view key, const std::wstring& value) {
    const size_t at = text.find(key);
    if (at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

const app::PackRelease& Release(FailedPack pack) {
    return pack == FailedPack::Ai ? app::AiPackRelease() : app::ContentPackRelease();
}

std::wstring SourceName(app::UpdateSource source) {
    switch (source) {
    case app::UpdateSource::Mirror1: return l10n::Get(StringId::UpdateSourceMirror1);
    case app::UpdateSource::Mirror2: return l10n::Get(StringId::UpdateSourceMirror2);
    case app::UpdateSource::GitHub: return l10n::Get(StringId::UpdateSourceGitHub);
    default: return l10n::Get(StringId::UpdateSourceAuto);
    }
}

// The sources a download of this pack went through, in order.
std::vector<app::UpdateSource> TriedSources() {
    const app::UpdateSource current = app::CurrentUpdateSource();
    if (current != app::UpdateSource::Automatic) return {current};
    return {app::UpdateSource::Mirror1, app::UpdateSource::Mirror2, app::UpdateSource::GitHub};
}

std::wstring CodeLine(app::PackFailure failure, DWORD error) {
    wchar_t code[64]{};
    swprintf_s(code, l10n::Get(StringId::ErrorCodeFormat).c_str(), error);
    std::wstring line = code;
    if (failure == app::PackFailure::Mismatch) {
        if (error == ERROR_FILE_TOO_LARGE) line += L" \x00B7 " + l10n::Get(StringId::PackMismatchSize);
        else if (error == ERROR_INVALID_DATA) line += L" \x00B7 " + l10n::Get(StringId::PackMismatchHash);
    }
    return line;
}

std::wstring Body(app::PackFailure failure) {
    const auto tried = TriedSources();
    const bool all = tried.size() > 1;
    const std::wstring count = std::to_wstring(tried.size());
    const std::wstring one = SourceName(tried.front());
    switch (failure) {
    case app::PackFailure::Network:
        return all ? Replace(l10n::Get(StringId::PackFailNetworkAll), L"{n}", count)
                   : Replace(l10n::Get(StringId::PackFailNetworkOne), L"{source}", one);
    case app::PackFailure::Mismatch:
        return all ? Replace(l10n::Get(StringId::PackMismatchAll), L"{n}", count)
                   : Replace(l10n::Get(StringId::PackMismatchOne), L"{source}", one);
    case app::PackFailure::Disk:
        return l10n::Get(StringId::PackDiskMessage);
    default:
        return l10n::Get(StringId::AiPackFailedMessage);
    }
}

std::wstring Title(FailedPack pack, app::PackFailure failure) {
    if (failure == app::PackFailure::Mismatch) return l10n::Get(StringId::PackMismatchTitle);
    if (failure == app::PackFailure::Disk)
        return l10n::Get(pack == FailedPack::Ai ? StringId::AiPackDiskTitle : StringId::ContentPackDiskTitle);
    return l10n::Get(pack == FailedPack::Ai ? StringId::AiPackFailedTitle : StringId::ContentPackFailedTitle);
}

void Retry(AppState& s) {
    if (g_pack == FailedPack::Ai) RetryAiPackInstall(s);
    else RetryContentPackInstall(s);
}

void ShowDetails(AppState& s) {
    const app::PackRelease& release = Release(g_pack);
    ui::ConfirmDialogSpec spec;
    spec.title = l10n::Get(StringId::PackDetailsTitle);
    spec.message = Body(g_failure);
    const std::wstring name = l10n::Get(g_pack == FailedPack::Ai ? StringId::AiPackName : StringId::ContentPackName);
    spec.items.push_back(Replace(Replace(l10n::Get(StringId::PackDetailsPack), L"{name}", name), L"{version}",
                                 release.version ? release.version : L""));
    for (size_t i = 0; i < release.file_count && i < 4; ++i) {
        const app::PackFile& file = release.files[i];
        const std::wstring asset = file.download_name ? file.download_name : std::wstring(file.name) + L".lzms";
        spec.items.push_back(Replace(Replace(l10n::Get(StringId::PackDetailsFile), L"{file}", asset), L"{size}",
                                     format::ByteSize(file.packed_size)));
    }
    std::wstring sources;
    for (const auto source : TriedSources()) {
        if (!sources.empty()) sources += L" \x2192 ";
        sources += SourceName(source);
    }
    spec.items.push_back(Replace(l10n::Get(StringId::PackDetailsSources), L"{sources}", sources));
    std::wstring result = Title(g_pack, g_failure);
    if (g_failure == app::PackFailure::Mismatch) {
        if (g_error == ERROR_FILE_TOO_LARGE) result = l10n::Get(StringId::PackMismatchSize);
        else if (g_error == ERROR_INVALID_DATA) result = l10n::Get(StringId::PackMismatchHash);
    }
    spec.items.push_back(Replace(Replace(l10n::Get(StringId::PackDetailsResult), L"{result}", result), L"{code}",
                                 std::to_wstring(g_error)));
    spec.item_glyph = L"\xE946";  // Info
    spec.note = l10n::Get(StringId::PackDetailsNote);
    spec.confirm_text = l10n::Get(StringId::TeachGotIt);
    spec.single_button = true;
    spec.tone = ui::ConfirmTone::Info;
    ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor);
}

} // namespace

void ShowPackFailure(AppState& s, FailedPack pack, app::PackFailure failure, DWORD error) {
    g_pack = pack;
    g_failure = failure;
    g_error = error;
    std::vector<ui::SnackAction> actions;
    auto kind = ui::fluent::InfoBarKind::Error;
    switch (failure) {
    case app::PackFailure::Mismatch:
        kind = ui::fluent::InfoBarKind::Warning;
        actions = {{l10n::Get(StringId::CheckForUpdates), WM_PACK_CHECK_UPDATE},
                   {l10n::Get(StringId::PackActionDetails), WM_PACK_DETAILS}};
        break;
    case app::PackFailure::Disk:
        actions = {{l10n::Get(StringId::OpenFolder), WM_PACK_OPEN_FOLDER},
                   {l10n::Get(StringId::RecoveryRetry), WM_PACK_RETRY}};
        break;
    default:
        actions = {{l10n::Get(StringId::RecoveryRetry), WM_PACK_RETRY},
                   {l10n::Get(StringId::PackActionSources), WM_PACK_SOURCES}};
        break;
    }
    s.notification_toast.ShowActionCard(s.hwnd, kind, Title(pack, failure),
                                        Body(failure) + L"\n" + CodeLine(failure, error), std::move(actions));
}

void OpenDownloadSourceSetting(AppState& s) {
    OpenSettingsTab(s, 8);
    const auto vm = BuildVm(s, false);
    const float w = static_cast<float>(s.compositor.Width());
    const float h = static_cast<float>(s.compositor.Height());
    s.settings.SetScroll(s.renderer.SettingsDestinationOffset(vm, static_cast<int>(StringId::DownloadSourceTitle), w, h),
                         s.renderer.SettingsMaxScroll(vm, w, h));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void HandlePackFailureMessage(AppState& s, UINT message) {
    switch (message) {
    case WM_PACK_RETRY: Retry(s); break;
    case WM_PACK_SOURCES: OpenDownloadSourceSetting(s); break;
    case WM_PACK_CHECK_UPDATE:
        OpenSettingsTab(s, 3);
        CheckForUpdates(s);
        break;
    case WM_PACK_DETAILS: ShowDetails(s); break;
    case WM_PACK_OPEN_FOLDER: {
        const std::wstring root = packs::PacksRoot();
        if (!root.empty()) {
            CreateDirectoryW(root.c_str(), nullptr);
            NewTab(s, root);
        }
        break;
    }
    default: break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse

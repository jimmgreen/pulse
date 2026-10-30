#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "settings_controller.h"
#include "blank_pane_click.h"
#include "default_file_manager.h"
#include "shell_integration_registry.h"
#include "../index/index_client.h"
#include "../index/index_config.h"
#include "../index/network_agent_client.h"
#include "../fs/wsl_path.h"
#include "../common/preview_packs.h"
#include "../common/runtime_log.h"
#include "system_ctx_model.h"
#include "shortcut_settings.h"
#include "pack_catalog.h"
#include "../ui/quick_preview_command.h"
#include "../ui/settings_page_ids.h"

#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <thread>

namespace pulse::app {

namespace {

template <size_t Size>
bool SelectValue(int index, const int (&values)[Size], int& target) {
    if (index < 0 || index >= static_cast<int>(Size) || target == values[index]) return false;
    target = values[index];
    return true;
}

// Bug 2: say which item failed and why; only blame security software when the
// registry refused the write or the value disappeared right after it.
std::wstring IntegrationFailureText(const ShellIntegrationFailure& failure) {
    using K = ShellIntegrationFailureKind;
    const std::wstring code = std::to_wstring(failure.status);
    switch (failure.kind) {
    case K::AccessDenied:
        return l10n::Get(l10n::StringId::TheWriteWasDeniedPossiblyBySecuritySoftware).c_str();
    case K::Reverted:
        return l10n::Get(l10n::StringId::ItWasUndoneRightAfterWritingPossiblyBySecuritySoftware).c_str();
    case K::WriteError:
        return std::wstring(l10n::Get(l10n::StringId::RegistryWriteFailedError).c_str()) + code;
    case K::ReadError:
        return std::wstring(l10n::Get(l10n::StringId::RegistryReadFailedError).c_str()) + code;
    case K::LegacyResidue:
        return l10n::Get(l10n::StringId::OldAssociationsFromAnEarlierVersionWereFoundTryAgainCleansTh).c_str();
    case K::BadBackup:
        return l10n::Get(l10n::StringId::TheBackupOfTheOriginalSettingIsDamagedSoNothingWasChanged).c_str();
    case K::PendingUpgrade:
        return l10n::Get(l10n::StringId::TheAssociationUpdateFromTheLastUpgradeDidNotFinish).c_str();
    case K::ChangedByOther:
        return l10n::Get(l10n::StringId::AnotherProgramChangedItSoItWasNotOverwritten).c_str();
    case K::None:
        return l10n::Get(l10n::StringId::TheSettingDidNotReadBackAsWritten).c_str();
    case K::Unknown: break;
    }
    return l10n::Get(l10n::StringId::UnknownReason).c_str();
}

std::wstring IntegrationLogPath() {
    wchar_t base[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    std::wstring dir = std::wstring(base) + L"\\Pulse";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\integration.log";
}

// One UTF-8 line per failed item in %LOCALAPPDATA%\Pulse\logs\integration.log.
void LogIntegrationFailure(const wchar_t* item, bool on, const ShellIntegrationFailure& failure) {
    const std::wstring path = IntegrationLogPath();
    if (path.empty()) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t line[2048]{};
    swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u item=%s want=%s result=%s status=%ld key=HKCU\\%s value=%s\r\n",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, item,
               on ? L"on" : L"off", ShellIntegrationFailureName(failure.kind), failure.status,
               failure.key.empty() ? L"-" : failure.key.c_str(),
               failure.name.empty() ? L"(default)" : failure.name.c_str());
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return;
    std::string text(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line, -1, text.data(), bytes, nullptr, nullptr);
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart < 512 * 1024) {
        DWORD written = 0;
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
    CloseHandle(file);
}

} // namespace

SettingsController::~SettingsController() {
    Stop();
}

SystemCtxModel& SettingsController::SystemCtx() {
    if (!sysctx_) sysctx_ = std::make_shared<SystemCtxModel>();
    return *sysctx_;
}

ShortcutSettings& SettingsController::Shortcuts() {
    if (!shortcuts_) shortcuts_ = std::make_shared<ShortcutSettings>();
    return *shortcuts_;
}

int SettingsController::PageFromName(std::wstring_view name) noexcept {
    if (name == L"index") return 1;
    if (name == L"context") return 2;
    if (name == L"about") return 3;
    if (name == L"duplicates") return 4;
    if (name == L"preview") return 5;
    if (name == L"packs") return 8;
    if (name == L"ai") return ui::kSettingsPageAi;   // 增强包 > AI 与 MCP
    if (name == L"system-menu") return 6;
    if (name == L"shortcuts") return 7;
    if (name == L"files") return 9;
    if (name == L"tabs") return 10;
    if (name == L"system") return 11;
    if (name == L"file-ops") return 12;
    return 0;
}

const wchar_t* SettingsController::PageName(int page) noexcept {
    if (page == 1) return L"index";
    if (page == 2) return L"context";
    if (page == 3) return L"about";
    if (page == 4) return L"duplicates";
    if (page == 5) return L"preview";
    if (page == 8) return L"packs";
    if (page == ui::kSettingsPageAi) return L"ai";
    if (page == 6) return L"system-menu";
    if (page == 7) return L"shortcuts";
    if (page == 9) return L"files";
    if (page == 10) return L"tabs";
    if (page == 11) return L"system";
    if (page == 12) return L"file-ops";
    return L"general";
}

void SettingsController::SelectPage(int page) noexcept {
    CancelGlobalSearchHotkeyCapture();
    page_ = std::clamp(page, 0, ui::kSettingsPageAi);
    scroll_ = 0.0f;
    packs_checked_ = 0;  // re-read the packs when their page opens
}

// ---- 预览增强包 ----------------------------------------------------------------

namespace {

uint64_t DirectoryBytes(const std::wstring& directory, int depth = 0) {
    if (depth > 4) return 0;
    uint64_t total = 0;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((directory + L"\\*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            total += DirectoryBytes(directory + L"\\" + data.cFileName, depth + 1);
        else
            total += (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return total;
}

bool IsFfmpegExe(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    return _wcsicmp(name.c_str(), L"ffmpeg.exe") == 0 && packs::IsRegularFile(path);
}

} // namespace

void SettingsController::RefreshPacks() {
    using packs::PackId;
    PackState state;
    state.notice = std::move(packs_.notice);
    state.images_notice = std::move(packs_.images_notice);
    state.raw_notice = std::move(packs_.raw_notice);
    const auto raw = packs::ReadInstalledPack(PackId::Raw);
    state.raw_installed = raw.present;
    state.raw_version = raw.version;
    state.archive_notice = std::move(packs_.archive_notice);
    const auto archive = packs::ReadInstalledPack(PackId::Archives);
    state.archive_installed = archive.present;
    state.archive_version = archive.version;
    state.root = packs::PacksRoot();
    const packs::PackSettings settings = packs::LoadPackSettings();
    state.raw_enabled = settings.enabled[static_cast<uint32_t>(PackId::Raw)];
    state.archive_enabled = settings.enabled[static_cast<uint32_t>(PackId::Archives)];
    state.media_available = kMediaPackRelease.file_count != 0;
    state.images_available = kImagePackRelease.file_count != 0;
    state.raw_available = kRawPackRelease.file_count != 0;
    state.archive_available = kArchivePackRelease.file_count != 0;
    state.enabled = settings.enabled[static_cast<uint32_t>(PackId::Media)];
    state.images_enabled = settings.enabled[static_cast<uint32_t>(PackId::Images)];
    state.use_custom = settings.use_custom_ffmpeg;
    state.custom_path = settings.custom_ffmpeg;
    state.remove_on_uninstall = settings.remove_on_uninstall;
    state.auto_update = settings.auto_update;
    const packs::InstalledPack media = packs::ReadInstalledPack(PackId::Media);
    state.media_installed = media.present;
    state.version = media.version;
    const packs::InstalledPack images = packs::ReadInstalledPack(PackId::Images);
    state.images_installed = images.present;
    state.images_version = images.version;
    // Packs older than this build's catalog offer 更新到.
    auto update_of = [](const packs::InstalledPack& pack, const PackRelease& release) {
        return pack.present && release.file_count && packs::PackUpdateAvailable(pack.version, release.version)
            ? std::wstring(release.version) : std::wstring();
    };
    state.update_version = update_of(media, kMediaPackRelease);
    state.images_update_version = update_of(images, kImagePackRelease);
    state.raw_update_version = update_of(raw, kRawPackRelease);
    state.archive_update_version = update_of(archive, kArchivePackRelease);
    state.installed = (media.present ? 1u : 0u) + (images.present ? 1u : 0u) + (raw.present ? 1u : 0u) + (archive.present ? 1u : 0u);
    // What the preview host would use, ignoring the on/off switch so the card
    // can still say which FFmpeg it is switching.
    if (state.use_custom) {
        if (IsFfmpegExe(state.custom_path)) state.ffmpeg = 2;
    } else if (media.present) state.ffmpeg = 1;
    // A working FFmpeg of the user's own replaces the media pack outright
    // (no fallback to it), so the pack is neither offered nor updated.
    if (state.ffmpeg == 2) state.update_version.clear();
    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr) && IsFfmpegExe(found))
        state.detected_path = found;
    if (!state.root.empty()) state.bytes = DirectoryBytes(state.root);
    packs_ = std::move(state);
    packs_checked_ = GetTickCount64();
    offer_checked_ = 0;
    image_offer_checked_ = 0;
}

namespace {
uint64_t DownloadBytes(const PackRelease& release) {
    uint64_t bytes = 0;
    for (size_t i = 0; i < release.file_count; ++i) bytes += release.files[i].packed_size;
    return bytes;
}

const PackRelease& ReleaseFor(packs::PackId id) {
    switch (id) {
    case packs::PackId::Images: return kImagePackRelease;
    case packs::PackId::Raw: return kRawPackRelease;
    case packs::PackId::Archives: return kArchivePackRelease;
    default: return kMediaPackRelease;
    }
}
} // namespace

void SettingsController::FillMediaPackOffer(ui::MediaPackOffer& offer) {
    const ULONGLONG now = GetTickCount64();
    if (!offer_checked_ || now - offer_checked_ >= 2000) {
        // An installed but switched-off pack is the user's choice: not offered.
        offer_missing_ = kMediaPackRelease.file_count &&
            !packs::ReadInstalledPack(packs::PackId::Media).present &&
            packs::ResolvePack(packs::PackId::Media).source == packs::ToolSource::None;
        offer_checked_ = now ? now : 1;
    }
    offer.installing = media_installer_.running();
    offer.installable = offer_missing_ || offer.installing;
    offer.progress = offer.installing ? media_installer_.progress() : 0.0f;
    offer.download_bytes = DownloadBytes(kMediaPackRelease);
    offer.notice = offer.installing ? std::wstring{} : packs_.notice;
}

void SettingsController::FillImagePackOffer(ui::MediaPackOffer& offer) {
    const ULONGLONG now = GetTickCount64();
    if (!image_offer_checked_ || now - image_offer_checked_ >= 2000) {
        image_offer_missing_ = kImagePackRelease.file_count &&
            !packs::ReadInstalledPack(packs::PackId::Images).present;
        image_offer_checked_ = now ? now : 1;
    }
    offer.installing = image_installer_.running();
    offer.installable = image_offer_missing_ || offer.installing;
    offer.progress = offer.installing ? image_installer_.progress() : 0.0f;
    offer.download_bytes = DownloadBytes(kImagePackRelease);
    offer.notice = offer.installing ? std::wstring{} : packs_.images_notice;
}

const SettingsController::PackState& SettingsController::Packs() {
    if (!packs_checked_ || GetTickCount64() - packs_checked_ >= 2000) RefreshPacks();
    packs_.installing = media_installer_.running();
    packs_.progress = packs_.installing ? media_installer_.progress() : 0.0f;
    packs_.images_installing = image_installer_.running();
    packs_.images_progress = packs_.images_installing ? image_installer_.progress() : 0.0f;
    packs_.raw_installing = raw_installer_.running();
    packs_.raw_progress = packs_.raw_installing ? raw_installer_.progress() : 0.0f;
    packs_.archive_installing = archive_installer_.running();
    packs_.archive_progress = packs_.archive_installing ? archive_installer_.progress() : 0.0f;
    return packs_;
}

PackInstaller& SettingsController::InstallerFor(packs::PackId id) {
    switch (id) {
    case packs::PackId::Images: return image_installer_;
    case packs::PackId::Raw: return raw_installer_;
    case packs::PackId::Archives: return archive_installer_;
    default: return media_installer_;
    }
}

std::wstring& SettingsController::NoticeFor(packs::PackId id) {
    switch (id) {
    case packs::PackId::Images: return packs_.images_notice;
    case packs::PackId::Raw: return packs_.raw_notice;
    case packs::PackId::Archives: return packs_.archive_notice;
    default: return packs_.notice;
    }
}

bool SettingsController::InstallPack(packs::PackId id, HWND notify, bool automatic) {
    PackInstaller& installer = InstallerFor(id);
    const PackRelease& release = ReleaseFor(id);
    std::wstring& notice = NoticeFor(id);
    if (installer.running()) {
        installer.Cancel();
        return false;
    }
    notice.clear();
    if (!release.file_count) {
        // No release is advertised until its downloadable files are published.
        notice = l10n::Get(l10n::StringId::ThisPreviewPackHasNotBeenPublishedForDownload).c_str();
        return false;
    }
    // Replacing an installed pack is an update: TakePackResult keeps the switches.
    updating_[static_cast<uint32_t>(id)] = packs::ReadInstalledPack(id).present;
    if (!installer.Start(release, notify, pack_reader_)) return false;
    automatic_[static_cast<uint32_t>(id)] = automatic;
    Packs();
    return false;
}

int SettingsController::AutoUpdatePacks(HWND notify) {
    const packs::PackSettings settings = packs::LoadPackSettings();
    if (!settings.auto_update) return 0;
    int started = 0;
    for (uint32_t index = 0; index < packs::kPackCount; ++index) {
        const auto id = static_cast<packs::PackId>(index);
        const PackRelease& release = ReleaseFor(id);
        if (id == packs::PackId::Media && settings.use_custom_ffmpeg && IsFfmpegExe(settings.custom_ffmpeg)) continue;
        if (auto_declined_[index] || !release.file_count || InstallerFor(id).running() ||
            !packs::PackAutoUpdateWanted(id, settings, packs::ReadInstalledPack(id), release.version)) continue;
        automatic_[index] = false;
        InstallPack(id, notify, true);
        if (!automatic_[index]) continue;   // not started
        ++started;
        diagnostics::runtime::Event("pack_auto_update_start", {{"pack", index}});
    }
    return started;
}

bool SettingsController::UpdatePack(packs::PackId id, HWND notify) {
    // Only from idle: the button goes away while the download runs, and a
    // late second click must not cancel it.
    if (InstallerFor(id).running() || !packs::ReadInstalledPack(id).present) return false;
    return InstallPack(id, notify);
}

bool SettingsController::InstallMediaPack(HWND notify) { return InstallPack(packs::PackId::Media, notify); }
bool SettingsController::InstallImagePack(HWND notify) { return InstallPack(packs::PackId::Images, notify); }

bool SettingsController::TakePackResult(packs::PackId id) {
    DWORD error = ERROR_SUCCESS;
    const PackInstallOutcome outcome = InstallerFor(id).TakeOutcome(error);
    if (outcome == PackInstallOutcome::None) return false;
    const uint32_t index = static_cast<uint32_t>(id);
    const bool update = updating_[index], automatic = automatic_[index];
    updating_[index] = automatic_[index] = false;
    if (automatic) {
        diagnostics::runtime::Event("pack_auto_update_end", {{"pack", index},
            {"outcome", static_cast<uint64_t>(outcome)}, {"error", error}});
    }
    RefreshPacks();
    std::wstring& notice = NoticeFor(id);
    switch (outcome) {
    case PackInstallOutcome::Installed: {
        notice.clear();
        // A first install selects the pack; an update keeps the user's choice.
        if (id == packs::PackId::Media && !update) {
            auto settings = packs::LoadPackSettings();
            settings.use_custom_ffmpeg = false;
            settings.enabled[0] = true;
            if (!packs::SavePackSettings(settings)) {
                notice = l10n::Get(l10n::StringId::PackInstalledButTheSourceSettingCouldNotBeSavedTurnOffUseExi).c_str();
            }
            RefreshPacks();
        }
        return true;
    }
    case PackInstallOutcome::Cancelled:
        // Only the card's 取消 ends a job early: for an automatic update that
        // is the answer for this run.
        if (automatic) auto_declined_[index] = true;
        notice = l10n::Get(l10n::StringId::DownloadCancelled).c_str();
        break;
    default:
        // A background update fails quietly: 更新到 stays, the next check retries.
        if (automatic) break;
        if (error == ERROR_INVALID_DATA) {
            notice = l10n::Get(l10n::StringId::TheDownloadDidNotPassVerificationAndWasDiscardedTryAgainLate).c_str();
        } else if (error == ERROR_SHARING_VIOLATION) {
            notice = id != packs::PackId::Media
                ? l10n::Get(l10n::StringId::FilesOfTheOldVersionAreInUseTryAgainInAMoment).c_str()
                : l10n::Get(l10n::StringId::FilesOfTheOldVersionAreInUseClosePlayingVideosAndTryAgain).c_str();
        } else if (error == ERROR_NOT_SUPPORTED) {
            notice = l10n::Get(l10n::StringId::ThisPCLacksTheWindowsComponentNeededToUnpackTheDownload).c_str();
        } else {
            notice = l10n::Get(l10n::StringId::TheDownloadFailedCheckTheConnectionAndTryAgain).c_str();
        }
        break;
    }
    return false;
}

bool SettingsController::TakeMediaPackResult() { return TakePackResult(packs::PackId::Media); }
bool SettingsController::TakeImagePackResult() { return TakePackResult(packs::PackId::Images); }

bool SettingsController::RemovePack(packs::PackId id) {
    if (InstallerFor(id).running()) return false;
    const std::wstring root = packs::PacksRoot();
    if (root.empty()) return false;
    const bool removed = RemovePackTree(root + L"\\" + packs::PackKey(id));
    std::wstring message;
    if (!removed) {
        message = id != packs::PackId::Media
            ? l10n::Get(l10n::StringId::SomeFilesAreInUseAndWereNotRemovedTryAgainInAMoment).c_str()
            : l10n::Get(l10n::StringId::SomeFilesAreInUseAndWereNotRemovedClosePlayingVideosAndTryAg).c_str();
    }
    NoticeFor(id) = std::move(message);
    RefreshPacks();
    return true;
}

bool SettingsController::RemoveMediaPack() { return RemovePack(packs::PackId::Media); }
bool SettingsController::RemoveImagePack() { return RemovePack(packs::PackId::Images); }

namespace {
bool UpdatePackSettings(const std::function<void(packs::PackSettings&)>& change) {
    packs::PackSettings settings = packs::LoadPackSettings();
    change(settings);
    return packs::SavePackSettings(settings);
}
} // namespace

bool SettingsController::ToggleMediaPack() {
    const bool saved = UpdatePackSettings([](packs::PackSettings& s) { s.enabled[0] = !s.enabled[0]; });
    packs_.notice.clear();
    RefreshPacks();
    return saved;
}

bool SettingsController::ToggleImagePack() {
    constexpr uint32_t kImages = static_cast<uint32_t>(packs::PackId::Images);
    const bool saved = UpdatePackSettings([](packs::PackSettings& s) { s.enabled[kImages] = !s.enabled[kImages]; });
    packs_.images_notice.clear();
    RefreshPacks();
    return saved;
}

bool SettingsController::InstallRawPack(HWND notify) { return InstallPack(packs::PackId::Raw, notify); }
bool SettingsController::TakeRawPackResult() { return TakePackResult(packs::PackId::Raw); }
bool SettingsController::RemoveRawPack() { return RemovePack(packs::PackId::Raw); }
bool SettingsController::ToggleRawPack() {
    constexpr uint32_t index = static_cast<uint32_t>(packs::PackId::Raw);
    const bool saved = UpdatePackSettings([](packs::PackSettings& s) { s.enabled[index] = !s.enabled[index]; });
    packs_.raw_notice.clear();
    RefreshPacks();
    return saved;
}

bool SettingsController::InstallArchivePack(HWND notify) { return InstallPack(packs::PackId::Archives, notify); }
bool SettingsController::TakeArchivePackResult() { return TakePackResult(packs::PackId::Archives); }
bool SettingsController::RemoveArchivePack() { return RemovePack(packs::PackId::Archives); }
bool SettingsController::ToggleArchivePack() {
    constexpr uint32_t index = static_cast<uint32_t>(packs::PackId::Archives);
    const bool saved = UpdatePackSettings([](packs::PackSettings& s) { s.enabled[index] = !s.enabled[index]; });
    packs_.archive_notice.clear();
    RefreshPacks();
    return saved;
}

bool SettingsController::ToggleCustomFfmpeg() {
    const std::wstring detected = Packs().detected_path;
    const bool saved = UpdatePackSettings([&](packs::PackSettings& s) {
        s.use_custom_ffmpeg = !s.use_custom_ffmpeg;
        if (s.use_custom_ffmpeg) s.enabled[0] = true;
        if (s.use_custom_ffmpeg && !packs::IsRegularFile(s.custom_ffmpeg) && !detected.empty())
            s.custom_ffmpeg = detected;
    });
    packs_.notice = saved ? std::wstring{} : l10n::Get(l10n::StringId::CouldNotSaveTheFFmpegSourceSettingTryAgain).c_str();
    RefreshPacks();
    return saved;
}

bool SettingsController::SetCustomFfmpeg(const std::wstring& path) {
    if (!IsFfmpegExe(path)) {
        packs_.notice = l10n::Get(l10n::StringId::ChooseTheProgramNamedFfmpegExe).c_str();
        return false;
    }
    const bool saved = UpdatePackSettings([&](packs::PackSettings& s) {
        s.custom_ffmpeg = path;
        s.use_custom_ffmpeg = true;
        s.enabled[0] = true;
    });
    packs_.notice = saved ? std::wstring{} : l10n::Get(l10n::StringId::CouldNotSaveTheFFmpegSourceSettingTryAgain).c_str();
    RefreshPacks();
    return saved;
}

bool SettingsController::UseDetectedFfmpeg() {
    const std::wstring detected = Packs().detected_path;
    return !detected.empty() && SetCustomFfmpeg(detected);
}

void SettingsController::ToggleRemovePacksOnUninstall() {
    UpdatePackSettings([](packs::PackSettings& s) { s.remove_on_uninstall = !s.remove_on_uninstall; });
    RefreshPacks();
}

void SettingsController::ToggleAutoUpdatePacks() {
    UpdatePackSettings([](packs::PackSettings& s) { s.auto_update = !s.auto_update; });
    RefreshPacks();
}

void SettingsController::SetScroll(float value, float maximum) noexcept {
    scroll_ = std::clamp(value, 0.0f, (std::max)(0.0f, maximum));
}

void SettingsController::ScrollBy(float delta, float scale, float maximum) noexcept {
    SetScroll(scroll_ - delta / 120.0f * 48.0f * scale, maximum);
}

bool SettingsController::VolumePending(std::wstring_view id) const {
    std::lock_guard<std::mutex> lock(task_state_->mutex);
    return task_state_->pending_volume == id;
}

bool SettingsController::network_pending() const noexcept {
    std::lock_guard<std::mutex> lock(task_state_->mutex);
    return task_state_->network_pending;
}

bool SettingsController::diagnostics_pending() const noexcept {
    std::lock_guard<std::mutex> lock(task_state_->mutex);
    return task_state_->diagnostics_pending;
}

bool SettingsController::settings_transfer_pending() const noexcept {
    std::lock_guard<std::mutex> lock(task_state_->mutex);
    return task_state_->transfer_pending;
}

bool SettingsController::StartSettingsTransfer(SettingsTask task) {
    if (!IsSettingsTransferTask(task.kind)) return false;
    return StartUiTask(std::move(task));
}

bool SettingsController::migration_pending() const noexcept {
    std::lock_guard<std::mutex> lock(task_state_->mutex);
    return task_state_->migration_pending;
}

bool SettingsController::StartTask(SettingsTask task, SettingsTaskOperation operation,
                                   SettingsTaskCompletion completion) {
    if (!operation || !completion) return false;

    std::lock_guard<std::mutex> workers_lock(workers_mutex_);
    if (stopping_) return false;

    const bool network = IsNetworkTask(task.kind);
    {
        std::lock_guard<std::mutex> state_lock(task_state_->mutex);
        if (network) {
            if (task_state_->network_pending) return false;
            task_state_->network_pending = true;
        } else {
            if (task_state_->local_pending ||
                (task.kind == SettingsTaskKind::Volume && task.key.empty()) ||
                (task.kind == SettingsTaskKind::Exclude && task.path.empty())) return false;
            task_state_->local_pending = true;
            task_state_->migration_pending = task.kind == SettingsTaskKind::ConfigureIndexPath;
            task_state_->diagnostics_pending =
                task.kind == SettingsTaskKind::DiagnosticsExport;
            task_state_->transfer_pending = IsSettingsTransferTask(task.kind);
            if (task.kind == SettingsTaskKind::Volume)
                task_state_->pending_volume = task.key;
        }
    }

    // The operation and callback are deliberately moved into the worker. The
    // UI remains responsible only for posting the result back to its window.
    const auto state = task_state_;
    try {
        workers_.emplace_back([state, task = std::move(task), operation = std::move(operation),
                 completion = std::move(completion), network]() mutable {
        SettingsTaskResult result;
        result.task = task;
        try {
            result.ok = operation(task, result.error);
        } catch (...) {
            result.ok = false;
            if (result.error.empty()) result.error = l10n::Get(l10n::StringId::SettingsAborted).c_str();
        }

        if (network) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->network_pending = false;
        } else {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->local_pending = false;
            state->migration_pending = false;
            state->diagnostics_pending = false;
            state->transfer_pending = false;
            if (task.kind == SettingsTaskKind::Volume)
                state->pending_volume.clear();
        }
        completion(std::move(result));
        });
    } catch (...) {
        std::lock_guard<std::mutex> state_lock(state->mutex);
        if (network) state->network_pending = false;
        else {
            state->local_pending = false;
            state->migration_pending = false;
            state->diagnostics_pending = false;
            state->transfer_pending = false;
            state->pending_volume.clear();
        }
        return false;
    }
    return true;
}

bool SettingsController::StartUiTask(SettingsTask task) {
    if (!ui_.task_completion) return false;
    auto* network = network_;
    const auto diagnostics_export = ui_.export_diagnostics;
    const auto settings_transfer = ui_.settings_transfer_task;
    if (IsNetworkTask(task.kind) && !network) return false;
    if (IsSettingsTransferTask(task.kind) && !settings_transfer) return false;
    return StartTask(std::move(task), [network, diagnostics_export, settings_transfer](
                                      const SettingsTask& value, std::wstring& error) {
        if (IsSettingsTransferTask(value.kind)) return settings_transfer(value, error);
        // Local index settings run Pulse.Index.exe elevated. Report a missing
        // executable here (localized via ServiceText) instead of a shell dialog.
        if (!IsNetworkTask(value.kind) && value.kind != SettingsTaskKind::DiagnosticsExport &&
            GetFileAttributesW(index::IndexClient::ExePath().c_str()) == INVALID_FILE_ATTRIBUTES) {
            error = L"找不到 Pulse.Index.exe";
            return false;
        }
        switch (value.kind) {
        case SettingsTaskKind::Volume:
            return index::IndexClient::ConfigureVolumeElevated(value.key, value.enabled);
        case SettingsTaskKind::Exclude:
            return index::IndexClient::ConfigureExcludePathElevated(value.path, value.enabled);
        case SettingsTaskKind::SystemExclusion:
            return index::IndexClient::ConfigureSystemExclusionElevated(value.key, value.enabled);
        case SettingsTaskKind::InstallService: {
            DWORD code = 0;
            if (index::IndexClient::InstallServiceElevated(&code)) return true;
            if (code == index::kIndexDirectoryNotPrivate) {
                error = l10n::Get(l10n::StringId::SettingsIndexDirNotPrivate);
                return false;
            }
            wchar_t detail[512]{};
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, code, 0, detail, ARRAYSIZE(detail), nullptr);
            error = L"(" + std::to_wstring(code) + L") " + (detail[0]
                ? std::wstring(detail) : l10n::Get(l10n::StringId::SettingsServiceStartError));
            while (!error.empty() && (error.back() == L'\r' || error.back() == L'\n'))
                error.pop_back();
            return false;
        }
        case SettingsTaskKind::RebuildIndex:
            return index::IndexClient::RebuildElevated();
        case SettingsTaskKind::ConfigureIndexPath:
            return index::IndexClient::ConfigureIndexPathElevated(value.path, &error);
        case SettingsTaskKind::NetworkAdd:
            return network->AddRoot(value.path, &error);
        case SettingsTaskKind::NetworkRemove:
            return network->RemoveRoot(value.path, &error);
        case SettingsTaskKind::NetworkRebuild:
            network->Rebuild();
            return true;
        case SettingsTaskKind::DiagnosticsExport:
            return diagnostics_export &&
                diagnostics_export(value.path, value.enabled, value.pin, error);
        case SettingsTaskKind::SettingsExport:
        case SettingsTaskKind::SettingsImport:
            return false;  // handled above
        }
        return false;
    }, ui_.task_completion);
}

SettingsTaskEffect SettingsController::CompleteTask(const SettingsTaskResult& result,
                                                    bool service_installed) {
    SettingsTaskEffect effect;
    // 备份与迁移 reports through its own toasts (settings_transfer_ui.cpp).
    if (IsSettingsTransferTask(result.task.kind)) return effect;
    if (result.task.kind == SettingsTaskKind::ConfigureIndexPath) effect.refresh_index = true;
    if (result.ok) {
        error_.clear();
        if (result.task.kind == SettingsTaskKind::NetworkAdd && result.task.pin)
            effect.pin_network = result.task.path;
        if (result.task.kind == SettingsTaskKind::DiagnosticsExport)
            effect.open_path = result.task.path;
        if (!IsNetworkTask(result.task.kind) &&
            result.task.kind != SettingsTaskKind::DiagnosticsExport) {
            service_installed_ = service_installed;
            effect.refresh_index = true;
        }
        return effect;
    }

    if (!result.error.empty()) {
        error_ = result.error;
    } else if (result.task.kind == SettingsTaskKind::InstallService) {
        error_ = result.error.empty()
            ? l10n::Get(l10n::StringId::SettingsServiceError).c_str()
            : result.error;
    } else if (result.task.kind == SettingsTaskKind::NetworkRemove) {
        error_ = l10n::Get(l10n::StringId::SettingsRemoveServerError).c_str();
    } else if (IsNetworkTask(result.task.kind)) {
        error_ = l10n::Get(l10n::StringId::SettingsAddServerError).c_str();
    } else {
        error_ = l10n::Get(l10n::StringId::SettingsOperationError).c_str();
    }
    return effect;
}

void SettingsController::BindUi(AppPrefs& prefs, ContextMenuPrefs& context,
                                index::IndexClient& index,
                                index::NetworkAgentClient& network,
                                UiCallbacks callbacks) {
    prefs_ = &prefs;
    context_ = &context;
    index_ = &index;
    network_ = &network;
    ui_ = std::move(callbacks);
}

void SettingsController::ResetUi() noexcept {
    CancelGlobalSearchHotkeyCapture();
    prefs_ = nullptr;
    context_ = nullptr;
    index_ = nullptr;
    network_ = nullptr;
    ui_ = {};
}

void SettingsController::Apply(SettingsEffect effect) const {
    if (effect != SettingsEffect::None && ui_.apply_effects) ui_.apply_effects(effect);
}

void SettingsController::SaveAndApply(SettingsEffect effect) const {
    if (!prefs_->Save() && ui_.show_error)
        ui_.show_error(l10n::HantText(prefs_->load_failed ?
            l10n::Get(l10n::StringId::TheOriginalSettingsCouldNotBeReadSavingIsBlockedToProtectThe).c_str() :
            l10n::Get(l10n::StringId::SettingsCouldNotBeSavedAndMayRevertAfterARestart).c_str()));
    Apply(effect);
}

void SettingsController::WindowEffect(std::wstring_view effect_id) {
    if (!compat::ModernWindows() && effect_id != L"none") return;
    static constexpr std::wstring_view ids[] = {
        L"none", L"acrylic-material", L"mica", L"mica-alt"
    };
    if (!prefs_ || prefs_->window_effect == effect_id ||
        std::find(std::begin(ids), std::end(ids), effect_id) == std::end(ids)) return;
    prefs_->window_effect.assign(effect_id);
    SaveAndApply(SettingsEffect::WindowMaterial);
}

void SettingsController::AccentChoice(bool system_choice, uint32_t rgb) {
    if (!prefs_) return;
    std::wstring next;
    if (!system_choice) {
        wchar_t hex[8]{};
        swprintf_s(hex, L"%06X", rgb & 0xFFFFFFu);
        next = hex;
    }
    if (prefs_->accent_rgb == next && prefs_->accent_follow_system == system_choice) return;
    prefs_->accent_rgb = std::move(next);
    prefs_->accent_follow_system = system_choice;
    if (!system_choice) SyncCustomAccent(rgb);
    SaveAndApply(SettingsEffect::Accent);
}

namespace {
uint32_t HslToRgb(int hue, int saturation, int lightness) noexcept {
    const float h = static_cast<float>(hue % 360) / 60.0f;
    const float s = static_cast<float>(std::clamp(saturation, 0, 100)) / 100.0f;
    const float l = static_cast<float>(std::clamp(lightness, 0, 100)) / 100.0f;
    const float c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if (h < 1) { r = c; g = x; } else if (h < 2) { r = x; g = c; } else if (h < 3) { g = c; b = x; }
    else if (h < 4) { g = x; b = c; } else if (h < 5) { r = x; b = c; } else { r = c; b = x; }
    const float m = l - c / 2.0f;
    auto channel = [&](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v + m, 0.0f, 1.0f) * 255.0f)); };
    return (channel(r) << 16) | (channel(g) << 8) | channel(b);
}
} // namespace

void SettingsController::SyncCustomAccent(uint32_t rgb) noexcept {
    if (slider_drag_ >= 2) return;  // the sliders drive the accent, not the other way round
    const float r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
    const float g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
    const float b = static_cast<float>(rgb & 0xFF) / 255.0f;
    const float hi = (std::max)({r, g, b}), lo = (std::min)({r, g, b}), d = hi - lo;
    const float l = (hi + lo) / 2.0f;
    float h = 0.0f, s = 0.0f;
    if (d > 1e-4f) {
        s = d / (1.0f - std::fabs(2.0f * l - 1.0f));
        if (hi == r) h = 60.0f * std::fmod((g - b) / d + 6.0f, 6.0f);
        else if (hi == g) h = 60.0f * ((b - r) / d + 2.0f);
        else h = 60.0f * ((r - g) / d + 4.0f);
        custom_hsl_[0] = static_cast<int>(std::lround(h)) % 360;  // grey keeps the last hue
    }
    custom_hsl_[1] = static_cast<int>(std::lround(std::clamp(s, 0.0f, 1.0f) * 100.0f));
    custom_hsl_[2] = static_cast<int>(std::lround(l * 100.0f));
}

void SettingsController::RowHeight(int index) {
    static constexpr int values[] = {28, 34, 40};
    if (prefs_ && SelectValue(index, values, prefs_->row_height))
        SaveAndApply(SettingsEffect::RowHeight);
}

void SettingsController::FolderSort(int index) {
    static constexpr int values[] = {0, 1, 2};
    if (prefs_ && SelectValue(index, values, prefs_->folder_sort_mode))
        SaveAndApply(SettingsEffect::FolderSort);
}

void SettingsController::MtimeStyle(int index) {
    static constexpr int values[] = {0, 1, 2, 3};
    if (prefs_ && SelectValue(index, values, prefs_->list_mtime_hint))
        SaveAndApply(SettingsEffect::ListStyle);
}

void SettingsController::MtimeRange(int index) {
    static constexpr int values[] = {1, 3, 7, 30};
    if (prefs_ && SelectValue(index, values, prefs_->list_mtime_range))
        SaveAndApply(SettingsEffect::ListStyle);
}

void SettingsController::MtimePalette(int index) {
    static constexpr int values[] = {0, 1, 2, 3, 4, 5};
    if (prefs_ && SelectValue(index, values, prefs_->list_mtime_palette))
        SaveAndApply(SettingsEffect::ListStyle);
}

void SettingsController::ClickMode(int index) {
    if (!prefs_ || index < 0 || index > 3) return;
    bool changed = false;
    if (index < 2) {
        changed = prefs_->single_click_open != (index == 1);
        prefs_->single_click_open = index == 1;
    } else {
        changed = prefs_->name_highlight != index - 2;
        prefs_->name_highlight = index - 2;
    }
    if (changed) SaveAndApply(SettingsEffect::ListStyle);
}

void SettingsController::DetailsColumns(uint32_t mask) {
    mask = ui::NormalizeDetailsColumns(mask);
    if (!prefs_ || prefs_->details_columns == mask) return;
    prefs_->details_columns = mask;
    SaveAndApply(SettingsEffect::ListStyle);
}

// Startup and new-tab locations are read when they are used; nothing to apply.
void SettingsController::StartupOpen(int index) {
    static constexpr int values[] = {0, 1};
    if (prefs_ && SelectValue(index, values, prefs_->startup_open))
        SaveAndApply(SettingsEffect::None);
}

void SettingsController::NotifyIcon(int index) {
    static constexpr int values[] = {0, 1, 2};
    if (prefs_ && SelectValue(index, values, prefs_->notify_icon_mode))
        SaveAndApply(SettingsEffect::TrayVisibility);
}

void SettingsController::NewTabOpen(int index) {
    static constexpr int values[] = {0, 1};
    if (prefs_ && SelectValue(index, values, prefs_->new_tab_open))
        SaveAndApply(SettingsEffect::None);
}

void SettingsController::ExternalOpen(int index) {
    static constexpr int values[] = {0, 1, 2, 3};
    if (prefs_ && SelectValue(index, values, prefs_->external_open))
        SaveAndApply(SettingsEffect::None);
}

void SettingsController::BlankClick(int index) {
    static constexpr int values[] = {kBlankClickOff, kBlankClickBack, kBlankClickUp};
    if (prefs_ && SelectValue(index, values, prefs_->blank_click_action))
        SaveAndApply(SettingsEffect::None);
}

void SettingsController::PreviewTrigger(int index) {
    static constexpr int values[] = {kPreviewTriggerHover, kPreviewTriggerSpace};
    if (prefs_ && SelectValue(index, values, prefs_->preview_trigger))
        SaveAndApply(SettingsEffect::None);
}

void SettingsController::HoverPreviewDelay(int index) {
    static constexpr int values[] = {300, 500, 800};
    if (prefs_ && SelectValue(index, values, prefs_->hover_preview_delay_ms))
        SaveAndApply(SettingsEffect::None);
}

void SettingsController::HoverCardSize(int index) {
    static constexpr int values[] = {kHoverCardSmall, kHoverCardMedium, kHoverCardLarge};
    if (prefs_ && SelectValue(index, values, prefs_->hover_card_size))
        SaveAndApply(SettingsEffect::None);
}

void SettingsController::HomeFolder(int action) {
    if (!prefs_) return;
    if (action == 0) {
        std::wstring path;
        if (!ui_.pick_folder ||
            !ui_.pick_folder(path, l10n::Get(l10n::StringId::SettingsHomeFolderTitle).c_str()) ||
            path.empty() || path == prefs_->home_folder) return;
        prefs_->home_folder = std::move(path);
    } else if (action == 1 && !prefs_->home_folder.empty()) {
        prefs_->home_folder.clear();
    } else {
        return;
    }
    SaveAndApply(SettingsEffect::None);
}

void SettingsController::TextRendering(int index) {
    static constexpr int values[] = {0, 1, 2};
    if (prefs_ && SelectValue(index, values, prefs_->text_render))
        SaveAndApply(SettingsEffect::TextRendering);
}

void SettingsController::UiFontSize(int index) {
    static constexpr int values[] = {90, 100, 112, 125};
    if (prefs_ && SelectValue(index, values, prefs_->ui_font_scale))
        SaveAndApply(SettingsEffect::UiFontSize);
}

void SettingsController::TrayIconSize(int index) {
    static constexpr int values[] = {40, 48, 56};
    if (prefs_ && SelectValue(index, values, prefs_->tray_icon_size))
        SaveAndApply(SettingsEffect::TrayDeckIcon);
}

void SettingsController::DoubleCtrlGap(int index) {
    static constexpr int values[] = {200, 300, 400};
    if (prefs_ && SelectValue(index, values, prefs_->global_search_double_ctrl_ms))
        SaveAndApply(SettingsEffect::GlobalSearch);
}

// Both only change how the next frame is painted; the caller invalidates.
// Values snap to the former preset levels unless Shift is held.
bool SettingsController::SliderValue(int which, int value) {
    if (prefs_ && which >= 2 && which <= 4) {
        // 主题色 › 自定义: live while dragging; EndSlider saves.
        int& target = custom_hsl_[which - 2];
        value = std::clamp(value, 0, which == 2 ? 360 : 100);
        if (target == value && !prefs_->accent_follow_system) return false;
        target = value;
        wchar_t hex[8]{};
        swprintf_s(hex, L"%06X", HslToRgb(custom_hsl_[0], custom_hsl_[1], custom_hsl_[2]) & 0xFFFFFFu);
        prefs_->accent_rgb = hex;
        prefs_->accent_follow_system = false;
        Apply(SettingsEffect::Accent);
        return true;
    }
    if (which == 5) {
        // 侧边栏与标题栏: snaps to the old look (10) and the ends unless Shift is held.
        if (!prefs_) return false;
        value = std::clamp(value, 0, 100);
        if (GetKeyState(VK_SHIFT) >= 0)
            for (int level : {0, 10, 50, 100}) if (std::abs(value - level) <= 2) value = level;
        if (prefs_->frame_transparency == value) return false;
        prefs_->frame_transparency = value;
        return true;
    }
    if (!prefs_ || which < 0 || which > 1) return false;
    const bool snap = GetKeyState(VK_SHIFT) >= 0;
    int& target = which == 0 ? prefs_->wallpaper_look : prefs_->wallpaper_blur;
    value = std::clamp(value, 0, which == 0 ? 90 : 40);
    if (snap) {
        static constexpr int kLook[] = {25, 50, 75};
        static constexpr int kBlur[] = {14, 28};
        if (which == 0) {
            for (int level : kLook) if (std::abs(value - level) <= 2) value = level;
        } else {
            for (int level : kBlur) if (std::abs(value - level) <= 1) value = level;
        }
    }
    if (target == value) return false;
    target = value;
    return true;
}

void SettingsController::BeginMenuDrag(int item, float pos, float grab) noexcept {
    menu_drag_item_ = item;
    menu_drag_live_ = false;
    menu_drag_start_ = menu_drag_pos_ = pos;
    menu_drag_grab_ = grab;
}

bool SettingsController::MenuDragMove(float pos) noexcept {
    if (menu_drag_item_ < 0) return false;
    if (!menu_drag_live_ && std::fabs(pos - menu_drag_start_) < 3.0f) return false;
    menu_drag_live_ = true;
    menu_drag_pos_ = pos;
    return true;
}

void SettingsController::SetMenuOrder(const BuiltinMenuOrder& order) {
    if (!context_) return;
    const auto surface = context_tab_ == 1 ? BuiltinMenuSurface::Background
                       : context_tab_ == 2 ? BuiltinMenuSurface::RowButtons : BuiltinMenuSurface::Item;
    context_->SetBuiltinOrder(surface, order);
    context_->Save();
    if (surface == BuiltinMenuSurface::RowButtons) Apply(SettingsEffect::ListStyle);
}

void SettingsController::EndSlider() {
    if (slider_drag_ < 0) return;
    slider_drag_ = -1;
    if (prefs_) prefs_->Save();
}

void SettingsController::Language(std::wstring_view language_id) {
    static constexpr std::wstring_view ids[] = {L"system", L"zh-CN", L"zh-TW", L"en-US"};
    if (!prefs_ || prefs_->language == language_id ||
        std::find(std::begin(ids), std::end(ids), language_id) == std::end(ids)) return;
    prefs_->language.assign(language_id);
    SaveAndApply(SettingsEffect::Language);
}

void SettingsController::Wallpaper(int action) {
    if (!prefs_ || !ui_.apply_effects) return;
    std::wstring path;
    if (action == 0 && ui_.pick_image && ui_.pick_image(path)) {
        if (!path.empty()) {
            if (prefs_->StoreBackgroundImage(path)) Apply(SettingsEffect::WindowMaterial);
            else if (ui_.show_error) ui_.show_error(l10n::HantText(l10n::Get(l10n::StringId::TheNewWallpaperCouldNotBeSavedThePreviousWallpaperWasKept).c_str()));
        }
    } else if (action == 1 && !prefs_->background_image.empty()) {
        prefs_->ClearBackgroundImage();
        SaveAndApply(SettingsEffect::WindowMaterial);
    }
}

void SettingsController::ChangeTrackingDays(int days) {
    if (!prefs_ || (days != 1 && days != 3 && days != 7) ||
        prefs_->change_tracking_days == days) return;
    prefs_->change_tracking_days = days;
    SaveAndApply(SettingsEffect::ChangeTracking);
}

bool SettingsController::CaptureGlobalSearchHotkey(uint32_t key, uint32_t modifiers) {
    if (!global_search_capturing_) return false;
    if (key == VK_ESCAPE) { CancelGlobalSearchHotkeyCapture(); return true; }
    if (key == VK_CONTROL || key == VK_MENU || key == VK_SHIFT || key == VK_LWIN || key == VK_RWIN ||
        (key >= VK_LSHIFT && key <= VK_RMENU)) return true;
    if (!prefs_) return true;
    if (modifiers == 0 || (modifiers & ~15u) || key == 0 || key > 254) {
        global_search_error_ = l10n::Get(l10n::StringId::GlobalSearchInvalid);
        return true;
    }
    const auto previous_modifiers = prefs_->global_search_modifiers;
    const auto previous_key = prefs_->global_search_key;
    prefs_->global_search_modifiers = modifiers;
    prefs_->global_search_key = key;
    if (!prefs_->Save()) {
        prefs_->global_search_modifiers = previous_modifiers;
        prefs_->global_search_key = previous_key;
        global_search_error_ = l10n::Get(l10n::StringId::GlobalSearchSaveFailed);
        return true;
    }
    global_search_capturing_ = false;
    global_search_error_.clear();
    Apply(SettingsEffect::GlobalSearch);
    return true;
}

std::wstring SettingsController::GlobalSearchHotkeyText() const {
    if (!prefs_) return L"Alt + Space";
    std::wstring text;
    const auto modifiers = prefs_->global_search_modifiers;
    if (modifiers & MOD_CONTROL) text += L"Ctrl + ";
    if (modifiers & MOD_ALT) text += L"Alt + ";
    if (modifiers & MOD_SHIFT) text += L"Shift + ";
    if (modifiers & MOD_WIN) text += L"Win + ";
    const UINT key = prefs_->global_search_key;
    LONG scan = static_cast<LONG>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC) << 16);
    if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN ||
        key == VK_PRIOR || key == VK_NEXT || key == VK_END || key == VK_HOME ||
        key == VK_INSERT || key == VK_DELETE || key == VK_DIVIDE || key == VK_NUMLOCK) scan |= 1 << 24;
    wchar_t name[128]{};
    if (GetKeyNameTextW(scan, name, 128)) text += name;
    else text += L"VK " + std::to_wstring(key);
    return text;
}

bool SettingsController::IntegrationCanRestore() const noexcept {
    return prefs_ && (prefs_->integration_enabled || prefs_->open_folders_in_pulse ||
        prefs_->take_over_win_e || prefs_->take_over_this_pc || prefs_->take_over_recycle_bin ||
        prefs_->integration_residual ||
        IntegrationCanRetry());
}

int SettingsController::IntegrationState() const noexcept {
    if (!prefs_) return 0;
    if (IntegrationCanRetry()) return 3;
    const auto& p = *prefs_;
    if (p.integration_incomplete) return 2;
    const bool active = p.open_folders_in_pulse || p.take_over_win_e || p.take_over_this_pc ||
        p.take_over_recycle_bin || p.integration_residual;
    if (!p.integration_enabled) return active ? 2 : 0;
    // In 兼容打开方式 the folder verb is meant to stay unregistered.
    if (p.open_folders_in_pulse != (p.integration_folders && !p.integration_folders_compat) ||
        p.take_over_win_e != p.integration_win_e || p.take_over_this_pc != p.integration_this_pc ||
        p.take_over_recycle_bin != p.integration_recycle_bin)
        return 2;
    return active || p.take_over_explorer_windows || p.integration_folders_compat ? 1 : 0;
}

std::wstring SettingsController::IntegrationSummary() const {
    // Only problem details are surfaced; the status pill and checkboxes describe healthy states.
    if (!prefs_) return {};
    if (IntegrationCanRetry()) {
        std::wstring message;
        if (!integration_error_.empty())
            message = std::wstring(l10n::Get(l10n::StringId::CouldNotApply).c_str()) + integration_error_ +
                l10n::Get(l10n::StringId::DetailsLOCALAPPDATAPulseLogsIntegrationLog).c_str();
        if (integration_save_failed_) {
            if (!message.empty()) message += L" ";
            message += l10n::Get(l10n::StringId::YourChoicesCouldNotBeSavedAndMayRevertAfterARestart).c_str();
        }
        return message;
    }
    if (IntegrationState() == 2) return l10n::Get(l10n::StringId::IntegrationDriftDesc);
    return {};
}

bool SettingsController::IntegrationJustEnabled() const noexcept {
    return prefs_ && prefs_->integration_enabled && integration_enabled_at_ &&
        GetTickCount64() - integration_enabled_at_ < 4000;
}

void SettingsController::IntegrationAction(int index) {
    if (!prefs_ || index < 0 || index > 9) return;
    if (index == 8) {
        // Try it: open This PC the way the desktop icon does, through the shell.
        ShellExecuteW(nullptr, nullptr, L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (ui_.integration_changing) ui_.integration_changing();
    auto& p = *prefs_;
    p.integration_configured = true;
    if (index == 0) {
        p.integration_enabled = !p.integration_enabled;
        // Folders and drives, This PC and Win + E go together: with only some of them,
        // Explorer opens first and then hands every double-click on to Pulse.
        if (p.integration_enabled) p.integration_folders = p.integration_win_e = p.integration_this_pc = true;
        integration_enabled_at_ = p.integration_enabled ? GetTickCount64() : 0;
    }
    else if (index == 1) p.integration_folders = !p.integration_folders;
    else if (index == 2) p.integration_win_e = !p.integration_win_e;
    else if (index == 3) p.integration_this_pc = !p.integration_this_pc;
    else if (index == 4) p.take_over_explorer_windows = !p.take_over_explorer_windows;
    else if (index == 6) p.integration_enabled = false;
    else if (index == 7) p.integration_recycle_bin = !p.integration_recycle_bin;
    else if (index == 9) p.integration_folders_compat = !p.integration_folders_compat;
    // Editing a disabled integration only changes the saved selection.
    if (p.integration_enabled || index == 0 || index == 5 || index == 6) {
        integration_error_.clear();
        auto failed = [&](const wchar_t* label, const wchar_t* item, bool on, bool explained) {
            if (!integration_error_.empty()) integration_error_ += l10n::Get(l10n::StringId::Text2855).c_str();
            integration_error_ += label;
            if (!explained) return;
            const auto failure = LastShellIntegrationFailure();
            integration_error_ += l10n::Get(l10n::StringId::Text2856).c_str() + IntegrationFailureText(failure) + l10n::Get(l10n::StringId::Text2857).c_str();
            LogIntegrationFailure(item, on, failure);
        };
        if ((index == 5 || index == 6) && HasLegacyShellIntegrationResidue() &&
            !RepairLegacyShellIntegrationResidue())
            failed(l10n::Get(l10n::StringId::LegacyExplorerAssociations).c_str(), L"legacy", false, false);
        const bool folders_on = p.integration_enabled && p.integration_folders && !p.integration_folders_compat;
        if (!p.ApplyFolderOpen(folders_on))
            failed(l10n::Get(l10n::StringId::FoldersAndDrives).c_str(), L"folders", folders_on, true);
        const bool win_e_on = p.integration_enabled && p.integration_win_e;
        if (!p.ApplyWinE(win_e_on))
            failed(L"Win + E", L"win-e", win_e_on, true);
        const bool this_pc_on = p.integration_enabled && p.integration_this_pc;
        if (!ApplyThisPcOpen(p, this_pc_on))
            failed(l10n::Get(l10n::StringId::SettingsThisPc).c_str(), L"this-pc", this_pc_on, true);
        const bool recycle_bin_on = p.integration_enabled && p.integration_recycle_bin;
        if (!ApplyRecycleBinOpen(p, recycle_bin_on))
            failed(l10n::Get(l10n::StringId::SettingsRecycleBin).c_str(), L"recycle-bin", recycle_bin_on, true);
    }
    integration_save_failed_ = !p.Save();
    if (ui_.integration_changed) ui_.integration_changed();
}

void SettingsController::ToggleUi(int index) {
    if (!prefs_ || !context_) return;
    // 修改时间提示 switches and sync status, kept out of the chain below.
    // 77, 78: 鼠标与选择 hover selection and item check boxes; 79: 滚动时固定上级.
    if (index >= 70 && index <= 79) {
        bool* const flags[] = {&prefs_->list_mtime_fade, &prefs_->list_mtime_bold, &prefs_->list_mtime_tree,
                               &prefs_->list_mtime_other_views, &prefs_->list_mtime_flash,
                               &prefs_->sync_onedrive, &prefs_->sync_svn, &prefs_->hover_select,
                               &prefs_->item_checkboxes, &prefs_->list_tree_sticky};
        bool& flag = *flags[index - 70];
        flag = !flag;
        SaveAndApply(SettingsEffect::ListStyle);
        return;
    }
    if (index == 1) {
        prefs_->ApplyLaunchOnStartup(!prefs_->launch_on_startup);
        SaveAndApply(SettingsEffect::None);
    } else if (index == 2) {
        prefs_->keep_running_on_close = !prefs_->keep_running_on_close;
        SaveAndApply(SettingsEffect::TrayVisibility);
    } else if (index == 3) {
        IntegrationAction(1);
    } else if (index == 4) {
        prefs_->show_status_performance = !prefs_->show_status_performance;
        SaveAndApply(SettingsEffect::StatusBarPerformance);
    } else if (index == 5) {
        prefs_->show_hidden_files = !prefs_->show_hidden_files;
        SaveAndApply(SettingsEffect::FileVisibility);
    } else if (index == 16) {
        prefs_->show_protected_os_files = !prefs_->show_protected_os_files;
        SaveAndApply(SettingsEffect::FileVisibility);
    } else if (index == 6) {
        prefs_->show_pinned_tab_names = !prefs_->show_pinned_tab_names;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 20) {
        IntegrationAction(2);
    } else if (index == 28) {
        IntegrationAction(0);
    } else if (index == 29) {
        IntegrationAction(3);
    } else if (index == 30) {
        IntegrationAction(4);
    } else if (index == 31) {
        // app_updates.cpp reads it on every tick; turning it back on checks right away
        // because the skipped interval has already elapsed.
        prefs_->auto_check_updates = !prefs_->auto_check_updates;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 21) {
        // shell_tag_menu.cpp installs/removes the HKCU verbs on the next UI tick.
        prefs_->shell_tag_menu = !prefs_->shell_tag_menu;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 8) {
        prefs_->change_tracking_enabled = !prefs_->change_tracking_enabled;
        SaveAndApply(SettingsEffect::ChangeTracking);
    } else if (index == 9) {
        prefs_->search_pinyin = !prefs_->search_pinyin;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 17) {
        prefs_->list_smart_date = !prefs_->list_smart_date;
        SaveAndApply(SettingsEffect::ListStyle);
    } else if (index == 18) {
        prefs_->list_zebra_rows = !prefs_->list_zebra_rows;
        SaveAndApply(SettingsEffect::ListStyle);
    } else if (index == 19) {
        prefs_->list_size_bar = !prefs_->list_size_bar;
        SaveAndApply(SettingsEffect::ListStyle);
    } else if (index == 22) {
        prefs_->list_tag_name_color = !prefs_->list_tag_name_color;
        SaveAndApply(SettingsEffect::ListStyle);
    } else if (index == 33) {
        prefs_->list_selection_outline = !prefs_->list_selection_outline;
        SaveAndApply(SettingsEffect::ListStyle);
    } else if (index == 34) {
        prefs_->list_thumbnail_badges = !prefs_->list_thumbnail_badges;
        SaveAndApply(SettingsEffect::ListStyle);
    } else if (index == 36) {
        prefs_->menu_acrylic = !prefs_->menu_acrylic;
        SaveAndApply(SettingsEffect::MenuMaterial);
    } else if (index == 23) {
        prefs_->vertical_tabs = !prefs_->vertical_tabs;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 37) {
        prefs_->tray_hide_when_empty = !prefs_->tray_hide_when_empty;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 24) {
        prefs_->show_hints = !prefs_->show_hints;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 25) {
        prefs_->tips_seen = 0;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 26) {
        prefs_->close_window_with_last_tab = !prefs_->close_window_with_last_tab;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 35) {
        prefs_->close_tab_on_double_click = !prefs_->close_tab_on_double_click;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 32) {
        prefs_->confirm_recycle_delete = !prefs_->confirm_recycle_delete;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 27) {
        prefs_->start_in_tray = !prefs_->start_in_tray;
        SaveAndApply(SettingsEffect::None);
    } else if (index == 40) {
        prefs_->global_search_double_ctrl = !prefs_->global_search_double_ctrl;
        SaveAndApply(SettingsEffect::GlobalSearch);
    } else if (index == 41) {
        prefs_->dialog_jump_bar = !prefs_->dialog_jump_bar;
        SaveAndApply(SettingsEffect::GlobalSearch);
    } else if (index == 43) {
        prefs_->global_search_apps = !prefs_->global_search_apps;
        SaveAndApply(SettingsEffect::GlobalSearch);
    } else if (index == 42) {
        prefs_->dialog_jump_auto = !prefs_->dialog_jump_auto;
        SaveAndApply(SettingsEffect::GlobalSearch);
    } else if (index == 44) {
        prefs_->jump_history = !prefs_->jump_history;
        SaveAndApply(SettingsEffect::GlobalSearch);
    } else if (index == 15) {
        prefs_->global_search_enabled = !prefs_->global_search_enabled;
        if (!prefs_->Save()) {
            prefs_->global_search_enabled = !prefs_->global_search_enabled;
            global_search_error_ = l10n::Get(l10n::StringId::GlobalSearchSaveFailed);
            return;
        }
        global_search_error_.clear();
        Apply(SettingsEffect::GlobalSearch);
    } else if (index >= 60 && index < 63) {
        // Pulse menu presets (精简 / 标准 / 完整).
        context_->builtin_hidden =
            BuiltinMenuPresetHidden(static_cast<BuiltinMenuPreset>(index - 60));
        context_->Save();
        Apply(SettingsEffect::ListStyle);
    } else if (index >= 64 && index < 67) {
        context_tab_ = index - 64;
    } else if (index == 67) {
        // The card's 恢复默认 resets Pulse's own rows only: shown and order.
        context_->builtin_hidden = 0;
        context_->ResetBuiltinOrder();
        context_->Save();
        Apply(SettingsEffect::ListStyle);
    } else if (index == 68) {
        // The preview's 恢复默认顺序: the current tab's order only.
        SetMenuOrder({});
    } else if (index >= 10 && index < 15) {
        static constexpr ipc::CtxMenuGroup groups[] = {
            ipc::CtxMenuGroup::Software, ipc::CtxMenuGroup::OpenWith,
            ipc::CtxMenuGroup::Share, ipc::CtxMenuGroup::System, ipc::CtxMenuGroup::Print
        };
        const auto group = groups[index - 10];
        context_->SetGroupEnabled(group, !context_->GroupEnabled(group));
        context_->Save();
    } else if (index >= 100) {
        const size_t item = static_cast<size_t>(index - 100);
        if (item >= context_->seen.size()) {
            // Rows after the seen catalog are Pulse's own commands, in
            // BuiltinMenuItem order (app_runtime builds them the same way).
            const size_t builtin = item - context_->seen.size();
            if (builtin >= static_cast<size_t>(kBuiltinMenuItemCount)) return;
            const auto which = static_cast<BuiltinMenuItem>(builtin);
            context_->SetBuiltinVisible(which, !context_->BuiltinVisible(which));
            context_->Save();
            if (which == BuiltinMenuItem::RowNewTab || which == BuiltinMenuItem::RowStar ||
                which == BuiltinMenuItem::RowMore)
                Apply(SettingsEffect::ListStyle);
            return;
        }
        const auto& seen = context_->seen[item];
        // RowEnabled (#77): a slow-disabled handler shows as off, so one
        // toggle re-enables it and clears the timeout skip via SetItemEnabled.
        const bool enabled = context_->RowEnabled(seen.key, seen.category, seen.from_com);
        context_->SetItemEnabled(seen.key, !enabled);
        if (!enabled)
            context_->SetGroupEnabled(ipc::GroupOf(seen.category), true);
        context_->Save();
    }
}

void SettingsController::ToggleVolume(int position) {
    if (!index_ || !ui_.task_completion) return;
    const auto volumes = index_->Volumes();
    if (position < 0 || position >= static_cast<int>(volumes.size())) return;
    const auto& volume = volumes[static_cast<size_t>(position)];
    if (!index_->ServiceMode() || !volume.supported) return;
    ClearError();
    SettingsTask task{SettingsTaskKind::Volume};
    task.key = volume.id;
    task.enabled = !volume.enabled;
    StartUiTask(std::move(task));
}

void SettingsController::AddExclude() {
    if (!index_ || !index_->ServiceMode() || !ui_.pick_folder || !ui_.task_completion) return;
    std::wstring path;
    if (!ui_.pick_folder(path, l10n::Get(l10n::StringId::SettingsPickExclude).c_str())) return;
    ClearError();
    SettingsTask task{SettingsTaskKind::Exclude};
    task.path = std::move(path);
    task.enabled = true;
    StartUiTask(std::move(task));
}

void SettingsController::RemoveExclude(int position) {
    if (!index_ || !index_->ServiceMode() || !ui_.task_completion) return;
    const auto paths = index_->ExcludedPaths();
    if (position < 0 || position >= static_cast<int>(paths.size())) return;
    ClearError();
    SettingsTask task{SettingsTaskKind::Exclude};
    task.path = paths[static_cast<size_t>(position)];
    StartUiTask(std::move(task));
}

void SettingsController::SystemExclusionAction(int action) {
    if (action == 2) {
        system_exclusion_expanded_ = !system_exclusion_expanded_;
        return;
    }
    if (!index_ || !index_->ServiceMode() || !ui_.task_completion) return;
    bool enabled = false;
    std::vector<std::wstring> groups;
    if (!index_->SystemExclusion(enabled, groups)) return;
    SettingsTask task{SettingsTaskKind::SystemExclusion};
    if (action == 1) {
        task.key = L"all";
        task.enabled = !enabled;
    } else {
        const int group = action - 10;
        if (!enabled || group < 0 ||
            group >= static_cast<int>(std::size(index::kSystemExclusionGroups))) return;
        task.key = index::kSystemExclusionGroups[group];
        task.enabled = std::find(groups.begin(), groups.end(), task.key) == groups.end();
    }
    ClearError();
    StartUiTask(std::move(task));
}

void SettingsController::IndexAction(int action) {
    if (!index_) return;
    if (action != 1 && migration_pending()) return;
    if (action == 1) {
        const std::wstring path = index_->IndexPath();
        if (!path.empty() && ui_.open_path) ui_.open_path(path);
        return;
    }
    if ((action != 0 && action != 2) || !ui_.task_completion) return;
    std::wstring path;
    const bool set_path = action == 2 && index_->ServiceMode();
    if (set_path && (!ui_.pick_folder ||
        !ui_.pick_folder(path, l10n::Get(l10n::StringId::SettingsPickStorage).c_str()))) return;
    ClearError();
    SettingsTask task{action == 0 ? SettingsTaskKind::RebuildIndex
        : set_path ? SettingsTaskKind::ConfigureIndexPath
                   : SettingsTaskKind::InstallService};
    task.path = std::move(path);
    StartUiTask(std::move(task));
}

// The network agent indexes server shares only. This check makes no network
// calls; it names the problem in the user's language before the agent's
// generic refusal (#279). The agent still has the final say.
static bool LooksLikeServerFolder(const std::wstring& path) {
    if (path.rfind(L"\\\\", 0) == 0 || path.rfind(L"//", 0) == 0) return true;
    if (path.size() < 2 || path[1] != L':') return false;
    const wchar_t root[] = {path[0], L':', L'\\', L'\0'};
    return GetDriveTypeW(root) == DRIVE_REMOTE;
}

void SettingsController::NetworkAction(int action, bool pin_after_add) {
    if (!network_ || !ui_.task_completion) return;
    if (action == 1) {
        ClearError();
        StartUiTask(SettingsTask{SettingsTaskKind::NetworkRebuild});
        return;
    }
    if (action != 0 || !ui_.pick_folder) return;
    // Start beside the last added share, not at the last folder picked here:
    // after a refused local pick, the picker kept reopening there (#279).
    const auto roots = network_->Roots();
    std::wstring path = roots.empty() ? std::wstring{} : roots.back().path;
    if (!ui_.pick_folder(path, l10n::Get(l10n::StringId::SettingsPickServer).c_str()) ||
        path.empty()) return;
    if (pulse::fs::IsWslPath(path)) {
        SetError(l10n::Get(l10n::StringId::NetworkIndexNoWsl).c_str());
        return;
    }
    if (!LooksLikeServerFolder(path)) {
        SetError(l10n::Get(l10n::StringId::NetworkIndexNeedsShare).c_str());
        return;
    }
    ClearError();
    SettingsTask task{SettingsTaskKind::NetworkAdd};
    task.path = std::move(path);
    task.pin = pin_after_add;
    StartUiTask(std::move(task));
}

void SettingsController::RemoveNetwork(int position) {
    if (!network_ || !ui_.task_completion) return;
    const auto roots = network_->Roots();
    if (position < 0 || position >= static_cast<int>(roots.size())) return;
    ClearError();
    SettingsTask task{SettingsTaskKind::NetworkRemove};
    task.path = roots[static_cast<size_t>(position)].path;
    StartUiTask(std::move(task));
}

void SettingsController::DiagnosticsAction(int action) {
    if (action >= 3 && action <= 5) {
        if (ui_.settings_transfer) ui_.settings_transfer(action - 3);
        return;
    }
    if (action == 0) {
        if (ui_.open_diagnostics) ui_.open_diagnostics();
        return;
    }
    if (action == 1) {
        if (!ui_.clear_diagnostics) return;
        std::wstring error;
        if (ui_.clear_diagnostics(error)) ClearError();
        else SetError(std::move(error));
        return;
    }
    if (action != 2 || !ui_.prepare_diagnostics_export ||
        !ui_.export_diagnostics || !ui_.task_completion) return;
    std::wstring destination;
    bool include_service = false;
    bool include_dumps = false;
    if (!ui_.prepare_diagnostics_export(destination, include_service, include_dumps) ||
        destination.empty()) return;
    ClearError();
    SettingsTask task{SettingsTaskKind::DiagnosticsExport};
    task.path = std::move(destination);
    task.enabled = include_service;
    task.pin = include_dumps;
    StartUiTask(std::move(task));
}

// ---- 缩略图缓存 ---------------------------------------------------------------

SettingsController::ThumbCacheState SettingsController::ThumbCache(void* notify) {
    bool count = false;
    ThumbCacheState state;
    {
        std::lock_guard<std::mutex> lock(thumb_cache_->mutex);
        ThumbCacheJob& job = *thumb_cache_;
        job.notify = notify;
        const uint64_t now = GetTickCount64();
        // The count is kept up to date in process; a recount only catches
        // what other Pulse processes wrote meanwhile.
        count = !job.busy && job.state.phase != ThumbCacheState::Cleared &&
                (!job.checked || now - job.checked >= 10000);
        if (count) job.checked = now;
        state = job.state;
    }
    if (count) StartThumbCacheJob(false);
    return state;
}

void SettingsController::ClearThumbCache() {
    {
        std::lock_guard<std::mutex> lock(thumb_cache_->mutex);
        const ThumbCacheJob& job = *thumb_cache_;
        if (job.busy || job.state.phase != ThumbCacheState::Ready || !job.state.bytes) return;
    }
    StartThumbCacheJob(true);
}

void SettingsController::StartThumbCacheJob(bool clear) {
    if (!ui_.thumb_cache_bytes || !ui_.clear_thumb_cache) return;
    std::lock_guard<std::mutex> workers_lock(workers_mutex_);
    if (stopping_) return;
    // busy is cleared as the job's last step, so this join returns at once.
    if (thumb_cache_worker_.joinable()) thumb_cache_worker_.join();
    {
        std::lock_guard<std::mutex> lock(thumb_cache_->mutex);
        thumb_cache_->busy = true;
        if (clear) thumb_cache_->state.phase = ThumbCacheState::Clearing;
    }
    try {
        thumb_cache_worker_ = std::thread([job = thumb_cache_, clear, count_bytes = ui_.thumb_cache_bytes,
                                           clear_cache = ui_.clear_thumb_cache] {
            const auto repaint = [&job] {
                void* notify = nullptr;
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    notify = job->notify;
                }
                if (notify) InvalidateRect(static_cast<HWND>(notify), nullptr, FALSE);
            };
            const uint64_t freed = clear ? clear_cache() : 0;
            uint64_t bytes = 0;
            const bool available = count_bytes(bytes);
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->state.bytes = bytes;
                job->state.cleared = freed;
                job->state.phase = !available ? ThumbCacheState::Unavailable
                    : clear ? ThumbCacheState::Cleared : ThumbCacheState::Ready;
                job->checked = GetTickCount64();
                if (!clear) job->busy = false;
            }
            repaint();
            if (!clear) return;
            // 已清除 stays on the button for 3 s, then the row shows the count.
            for (int waited = 0; waited < 3000; waited += 50) {
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    if (job->stopping) break;
                }
                Sleep(50);
            }
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                if (job->state.phase == ThumbCacheState::Cleared) job->state.phase = ThumbCacheState::Ready;
                job->busy = false;
            }
            repaint();
        });
    } catch (...) {
        std::lock_guard<std::mutex> lock(thumb_cache_->mutex);
        thumb_cache_->busy = false;
        if (clear) thumb_cache_->state.phase = ThumbCacheState::Ready;
    }
}

void SettingsController::Stop() {
    std::vector<std::thread> workers;
    std::thread thumb_cache;
    {
        std::lock_guard<std::mutex> lock(thumb_cache_->mutex);
        thumb_cache_->stopping = true;
    }
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        stopping_ = true;
        workers.swap(workers_);
        thumb_cache.swap(thumb_cache_worker_);
    }
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    if (thumb_cache.joinable()) thumb_cache.join();
}

} // namespace pulse::app

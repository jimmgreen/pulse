#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "settings_controller.h"
#include "blank_pane_click.h"
#include "default_file_manager.h"
#include "shell_integration_registry.h"
#include "../index/index_client.h"
#include "../index/network_agent_client.h"
#include "../common/preview_packs.h"
#include "pack_catalog.h"
#include "../ui/quick_preview_command.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cwchar>
#include <thread>

namespace pulse::app {

namespace {

template <size_t Size>
bool SelectValue(int index, const int (&values)[Size], int& target) {
    if (index < 0 || index >= static_cast<int>(Size) || target == values[index]) return false;
    target = values[index];
    return true;
}

} // namespace

SettingsController::~SettingsController() {
    Stop();
}

int SettingsController::PageFromName(std::wstring_view name) noexcept {
    if (name == L"index") return 1;
    if (name == L"context") return 2;
    if (name == L"about") return 3;
    if (name == L"duplicates") return 4;
    if (name == L"packs") return 5;
    return 0;
}

const wchar_t* SettingsController::PageName(int page) noexcept {
    if (page == 1) return L"index";
    if (page == 2) return L"context";
    if (page == 3) return L"about";
    if (page == 4) return L"duplicates";
    if (page == 5) return L"packs";
    return L"general";
}

void SettingsController::SelectPage(int page) noexcept {
    CancelGlobalSearchHotkeyCapture();
    page_ = std::clamp(page, 0, 5);
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
    const packs::InstalledPack media = packs::ReadInstalledPack(PackId::Media);
    state.media_installed = media.present;
    state.version = media.version;
    const packs::InstalledPack images = packs::ReadInstalledPack(PackId::Images);
    state.images_installed = images.present;
    state.images_version = images.version;
    state.installed = (media.present ? 1u : 0u) + (images.present ? 1u : 0u) + (raw.present ? 1u : 0u) + (archive.present ? 1u : 0u);
    // What the preview host would use, ignoring the on/off switch so the card
    // can still say which FFmpeg it is switching.
    if (state.use_custom) {
        if (IsFfmpegExe(state.custom_path)) state.ffmpeg = 2;
    } else if (media.present) state.ffmpeg = 1;
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

bool SettingsController::InstallPack(packs::PackId id, HWND notify) {
    const bool images = id == packs::PackId::Images;
    PackInstaller& installer = InstallerFor(id);
    const PackRelease& release = id == packs::PackId::Raw ? kRawPackRelease :
        id == packs::PackId::Archives ? kArchivePackRelease : images ? kImagePackRelease : kMediaPackRelease;
    std::wstring& notice = NoticeFor(id);
    if (installer.running()) {
        installer.Cancel();
        return false;
    }
    notice.clear();
    if (!release.file_count) {
        // No release is advertised until its downloadable files are published.
        notice = l10n::Pick(L"此预览增强包尚未发布下载。",
                            L"This preview pack has not been published for download.");
        return false;
    }
    if (!installer.Start(release, notify)) return false;
    Packs();
    return false;
}

bool SettingsController::InstallMediaPack(HWND notify) { return InstallPack(packs::PackId::Media, notify); }
bool SettingsController::InstallImagePack(HWND notify) { return InstallPack(packs::PackId::Images, notify); }

bool SettingsController::TakePackResult(packs::PackId id) {
    DWORD error = ERROR_SUCCESS;
    const PackInstallOutcome outcome = InstallerFor(id).TakeOutcome(error);
    if (outcome == PackInstallOutcome::None) return false;
    RefreshPacks();
    std::wstring& notice = NoticeFor(id);
    switch (outcome) {
    case PackInstallOutcome::Installed: {
        notice.clear();
        if (id == packs::PackId::Media) {
            auto settings = packs::LoadPackSettings();
            settings.use_custom_ffmpeg = false;
            settings.enabled[0] = true;
            if (!packs::SavePackSettings(settings)) {
                notice = l10n::Pick(L"增强包已安装，但未能保存来源设置。请关闭“使用已有 FFmpeg”以切换到增强包。",
                    L"Pack installed, but the source setting could not be saved. Turn off Use existing FFmpeg to select the pack.");
            }
            RefreshPacks();
        }
        return true;
    }
    case PackInstallOutcome::Cancelled:
        notice = l10n::Pick(L"已取消下载。", L"Download cancelled.");
        break;
    default:
        if (error == ERROR_INVALID_DATA) {
            notice = l10n::Pick(L"下载的文件校验失败，已丢弃。请稍后重试。",
                                L"The download did not pass verification and was discarded. Try again later.");
        } else if (error == ERROR_SHARING_VIOLATION) {
            notice = id != packs::PackId::Media
                ? l10n::Pick(L"旧版本的文件正在使用，请稍后再试一次。",
                             L"Files of the old version are in use. Try again in a moment.")
                : l10n::Pick(L"旧版本的文件正在使用。关闭正在播放的视频后再试一次。",
                             L"Files of the old version are in use. Close playing videos and try again.");
        } else if (error == ERROR_NOT_SUPPORTED) {
            notice = l10n::Pick(L"这台电脑缺少解压所需的系统组件，无法安装。",
                                L"This PC lacks the Windows component needed to unpack the download.");
        } else {
            notice = l10n::Pick(L"下载失败，请检查网络后重试。",
                                L"The download failed. Check the connection and try again.");
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
            ? l10n::Pick(L"有文件正在使用，未能全部删除。请稍后再试一次。",
                         L"Some files are in use and were not removed. Try again in a moment.")
            : l10n::Pick(L"有文件正在使用，未能全部删除。关闭正在播放的视频后再试一次。",
                         L"Some files are in use and were not removed. Close playing videos and try again.");
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
    packs_.notice = saved ? std::wstring{} : l10n::Pick(L"无法保存 FFmpeg 来源设置，请重试。",
        L"Could not save the FFmpeg source setting. Try again.");
    RefreshPacks();
    return saved;
}

bool SettingsController::SetCustomFfmpeg(const std::wstring& path) {
    if (!IsFfmpegExe(path)) {
        packs_.notice = l10n::Pick(L"请选择名为 ffmpeg.exe 的程序。", L"Choose the program named ffmpeg.exe.");
        return false;
    }
    const bool saved = UpdatePackSettings([&](packs::PackSettings& s) {
        s.custom_ffmpeg = path;
        s.use_custom_ffmpeg = true;
        s.enabled[0] = true;
    });
    packs_.notice = saved ? std::wstring{} : l10n::Pick(L"无法保存 FFmpeg 来源设置，请重试。",
        L"Could not save the FFmpeg source setting. Try again.");
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
    if (IsNetworkTask(task.kind) && !network) return false;
    return StartTask(std::move(task), [network, diagnostics_export](
                                      const SettingsTask& value, std::wstring& error) {
        switch (value.kind) {
        case SettingsTaskKind::Volume:
            return index::IndexClient::ConfigureVolumeElevated(value.key, value.enabled);
        case SettingsTaskKind::Exclude:
            return index::IndexClient::ConfigureExcludePathElevated(value.path, value.enabled);
        case SettingsTaskKind::InstallService: {
            DWORD code = 0;
            if (index::IndexClient::InstallServiceElevated(&code)) return true;
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
                diagnostics_export(value.path, value.enabled, error);
        }
        return false;
    }, ui_.task_completion);
}

SettingsTaskEffect SettingsController::CompleteTask(const SettingsTaskResult& result,
                                                    bool service_installed) {
    SettingsTaskEffect effect;
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
            l10n::Pick(L"原设置未能读取，已阻止覆盖。请关闭后重试打开 Pulse。",
                L"The original settings could not be read. Saving is blocked to protect them. Restart Pulse to retry.") :
            l10n::Pick(L"设置未能保存，重启后可能恢复原值。",
                L"Settings could not be saved and may revert after a restart.")));
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
    SaveAndApply(SettingsEffect::Accent);
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

void SettingsController::BlankClick(int index) {
    static constexpr int values[] = {kBlankClickOff, kBlankClickBack, kBlankClickUp};
    if (prefs_ && SelectValue(index, values, prefs_->blank_click_action))
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

// Both only change how the next frame is painted; the caller invalidates.
// Values snap to the former preset levels unless Shift is held.
bool SettingsController::SliderValue(int which, int value) {
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
            else if (ui_.show_error) ui_.show_error(l10n::HantText(l10n::Pick(
                L"新壁纸未能保存，已保留原壁纸。", L"The new wallpaper could not be saved. The previous wallpaper was kept.")));
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
        prefs_->take_over_win_e || prefs_->take_over_this_pc || prefs_->integration_residual ||
        IntegrationCanRetry());
}

int SettingsController::IntegrationState() const noexcept {
    if (!prefs_) return 0;
    if (IntegrationCanRetry()) return 3;
    const auto& p = *prefs_;
    if (p.integration_incomplete) return 2;
    const bool active = p.open_folders_in_pulse || p.take_over_win_e || p.take_over_this_pc ||
        p.integration_residual;
    if (!p.integration_enabled) return active ? 2 : 0;
    if (p.open_folders_in_pulse != p.integration_folders ||
        p.take_over_win_e != p.integration_win_e || p.take_over_this_pc != p.integration_this_pc)
        return 2;
    return active || p.take_over_explorer_windows ? 1 : 0;
}

std::wstring SettingsController::IntegrationSummary() const {
    // Only problem details are surfaced; the status pill and checkboxes describe healthy states.
    if (!prefs_) return {};
    if (IntegrationCanRetry()) {
        std::wstring message;
        if (!integration_error_.empty())
            message = std::wstring(l10n::Pick(L"没能设置：", L"Could not apply: ")) + integration_error_ +
                l10n::Pick(L"。可能被安全软件拦截，可以重试。", L". Security software may have blocked it; try again.");
        if (integration_save_failed_) {
            if (!message.empty()) message += L" ";
            message += l10n::Pick(L"你的选择没能保存，重启后可能变回原来的设置。",
                L"Your choices could not be saved and may revert after a restart.");
        }
        return message;
    }
    if (IntegrationState() == 2) return l10n::Get(l10n::StringId::IntegrationDriftDesc);
    return {};
}

void SettingsController::IntegrationAction(int index) {
    if (!prefs_ || index < 0 || index > 6) return;
    if (ui_.integration_changing) ui_.integration_changing();
    auto& p = *prefs_;
    p.integration_configured = true;
    if (index == 0) p.integration_enabled = !p.integration_enabled;
    else if (index == 1) p.integration_folders = !p.integration_folders;
    else if (index == 2) p.integration_win_e = !p.integration_win_e;
    else if (index == 3) p.integration_this_pc = !p.integration_this_pc;
    else if (index == 4) p.take_over_explorer_windows = !p.take_over_explorer_windows;
    else if (index == 6) p.integration_enabled = false;
    // Editing a disabled integration only changes the saved selection.
    if (p.integration_enabled || index == 0 || index == 5 || index == 6) {
        integration_error_.clear();
        auto failed = [&](const wchar_t* label) {
            if (!integration_error_.empty()) integration_error_ += l10n::Pick(L"、", L", ");
            integration_error_ += label;
        };
        if ((index == 5 || index == 6) && HasLegacyShellIntegrationResidue() &&
            !RepairLegacyShellIntegrationResidue())
            failed(l10n::Pick(L"旧版资源管理器关联", L"Legacy Explorer associations"));
        if (!p.ApplyFolderOpen(p.integration_enabled && p.integration_folders))
            failed(l10n::Pick(L"文件夹和磁盘", L"Folders and drives"));
        if (!p.ApplyWinE(p.integration_enabled && p.integration_win_e))
            failed(L"Win + E");
        if (!ApplyThisPcOpen(p, p.integration_enabled && p.integration_this_pc))
            failed(l10n::Pick(L"桌面上的「此电脑」", L"This PC on the desktop"));
    }
    integration_save_failed_ = !p.Save();
    if (ui_.integration_changed) ui_.integration_changed();
    Apply(SettingsEffect::None);
}

void SettingsController::ToggleUi(int index) {
    if (!prefs_ || !context_) return;
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
    } else if (index == 23) {
        prefs_->vertical_tabs = !prefs_->vertical_tabs;
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
    } else if (index == 15) {
        prefs_->global_search_enabled = !prefs_->global_search_enabled;
        if (!prefs_->Save()) {
            prefs_->global_search_enabled = !prefs_->global_search_enabled;
            global_search_error_ = l10n::Get(l10n::StringId::GlobalSearchSaveFailed);
            return;
        }
        global_search_error_.clear();
        Apply(SettingsEffect::GlobalSearch);
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
        const bool enabled = context_->ItemEnabled(seen.key, seen.category, seen.from_com);
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

void SettingsController::NetworkAction(int action, bool pin_after_add) {
    if (!network_ || !ui_.task_completion) return;
    if (action == 1) {
        ClearError();
        StartUiTask(SettingsTask{SettingsTaskKind::NetworkRebuild});
        return;
    }
    if (action != 0 || !ui_.pick_folder) return;
    std::wstring path;
    if (!ui_.pick_folder(path, l10n::Get(l10n::StringId::SettingsPickServer).c_str())) return;
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
    if (!ui_.prepare_diagnostics_export(destination, include_service) ||
        destination.empty()) return;
    ClearError();
    SettingsTask task{SettingsTaskKind::DiagnosticsExport};
    task.path = std::move(destination);
    task.enabled = include_service;
    StartUiTask(std::move(task));
}

void SettingsController::Stop() {
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        stopping_ = true;
        workers.swap(workers_);
    }
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}

} // namespace pulse::app

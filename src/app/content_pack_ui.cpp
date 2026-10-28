#include "content_pack_ui.h"
#include "content_pack.h"
#include "pack_installer.h"
#include "pack_failure_ui.h"
#include "app_internal.h"
#include "../common/localization.h"
#include "../common/preview_packs.h"
#include "../common/runtime_log.h"
#include "../common/text_format.h"
#include "../index/content_search_client.h"
#include "../ui/confirm_dialog.h"
#include <algorithm>

namespace pulse {
namespace {

// pulse.exe finds a verified pack's agent through this; tests that link the
// client without the app keep finding only a sibling Pulse.Content.exe.
const bool kContentAgentLocatorRegistered =
    (index::SetContentAgentLocator(&app::LocateContentAgent), true);

std::wstring Zh(const wchar_t* chinese, const wchar_t* english) {
    return l10n::IsChinese() ? l10n::HantText(chinese) : std::wstring(english);
}

std::wstring Fill(std::wstring text, const std::wstring& value) {
    const size_t at = text.find(L"%s");
    if (at != std::wstring::npos) text.replace(at, 2, value);
    return text;
}

std::wstring DownloadSize() {
    return format::ByteSize(app::ContentPackDownloadBytes(app::ContentPackRelease()));
}

// One download per process, whichever window asked. Never destroyed: a
// download still running at exit just ends with the process.
app::PackInstaller& Installer() {
    static auto* installer = new app::PackInstaller;
    return *installer;
}

HWND g_owner = nullptr;       // window showing the progress, null when idle
bool g_silent = false;        // background update: no toasts
int g_progress_step = -1;     // last progress shown, in tenths
bool g_prompted = false;      // the once-per-session prompt was shown

void ShowProgress(AppState& s, int step) {
    g_progress_step = step;
    std::wstring message = Zh(L"正在下载（约 %s）", L"Downloading (about %s)");
    message = Fill(message, DownloadSize());
    if (step > 0) message += L" \x00B7 " + std::to_wstring(step * 10) + L"%";
    s.notification_toast.SetKind(ui::fluent::InfoBarKind::Informational);
    s.notification_toast.Show(s.hwnd, Zh(L"内容搜索增强包", L"Content search pack"), std::move(message));
}

bool StartInstall(AppState& s, bool silent) {
    if (Installer().running()) return true;
    if (!Installer().Start(app::ContentPackRelease(), s.hwnd)) return false;
    g_owner = s.hwnd;
    g_silent = silent;
    g_progress_step = -1;
    diagnostics::runtime::Event("content_pack_install_started", {{"silent", static_cast<uint64_t>(silent)}});
    if (!silent) ShowProgress(s, 0);
    return true;
}

bool AskInstall(AppState& s, app::ContentPackState state) {
    ui::ConfirmDialogSpec spec;
    const std::wstring size = DownloadSize();
    if (state == app::ContentPackState::Outdated) {
        spec.title = Zh(L"更新内容搜索增强包", L"Update the content search pack");
        spec.message = Fill(Zh(L"Pulse 已更新，内容搜索增强包需要更新到相同版本才能使用（约 %s）。",
                               L"Pulse was updated; the content search pack has to be updated to the same "
                               L"version before it can be used (about %s)."), size);
    } else if (state == app::ContentPackState::Damaged) {
        spec.title = Zh(L"修复内容搜索增强包", L"Repair the content search pack");
        spec.message = Fill(Zh(L"内容搜索增强包的文件不完整或已被改动，需要重新下载（约 %s）。",
                               L"Files of the content search pack are missing or were changed; it has to be "
                               L"downloaded again (about %s)."), size);
    } else {
        spec.title = Zh(L"下载内容搜索增强包", L"Download the content search pack");
        spec.message = Fill(Zh(L"搜索文件内容和查找重复文件由增强包提供，首次使用时下载，"
                               L"不占用安装包体积。下载约 %s，装好后即可使用。",
                               L"Searching file contents and finding duplicates come in an enhancement pack "
                               L"that is downloaded the first time you need it. The download is about %s."), size);
    }
    spec.items = {Zh(L"Pulse.Content.exe — 内容搜索与重复文件查找", L"Pulse.Content.exe — content search and duplicates"),
                  Zh(L"Pulse.Document.exe — 读取 PDF 与 Office 文档的文字", L"Pulse.Document.exe — text of PDF and Office documents")};
    spec.item_glyph = L"\xE7C3";  // Page
    spec.note = Zh(L"从 Pulse 在 GitHub 的同一发布页下载，每个文件都校验 SHA-256；"
                   L"装在当前用户目录下，不需要管理员权限。",
                   L"Downloaded from the same GitHub release as Pulse, every file checked against its SHA-256; "
                   L"installed for the current user, no administrator rights needed.");
    spec.confirm_text = Zh(L"下载并安装", L"Download and install");
    spec.cancel_text = Zh(L"暂不", L"Not now");
    spec.tone = ui::ConfirmTone::Question;
    spec.glyph = L"\xE896";  // Download
    spec.default_choice = ui::ConfirmChoice::Confirm;
    return ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor) == ui::ConfirmChoice::Confirm;
}

// After Pulse was updated: replace an installed pack quietly when the user
// lets packs update themselves. Only installed.json is read here, never the
// files, so this costs nothing at startup.
void MaybeUpdateInBackground(AppState& s) {
    const auto& release = app::ContentPackRelease();
    if (!release.file_count) return;
    const std::wstring root = packs::PacksRoot();
    if (root.empty()) return;
    std::wstring version;
    if (packs::InstalledPackDirectory(root + L"\\" + release.key, &version).empty()) return;
    if (version == release.version || !packs::LoadPackSettings().auto_update) return;
    if (app::QueryContentPackState() != app::ContentPackState::Outdated) return;  // e.g. Bundled
    StartInstall(s, true);
}

void RerunContentSearch(AppState& s) {
    if (!s.addressSearching || !s.addressSearchContent) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || !IsAddressSearchResults(tab)) return;
    // The banner of the failed search goes; the same query runs again.
    if (tab->banner_title == Zh(L"内容搜索需要增强包", L"Content search needs its pack")) {
        tab->banner_title.clear();
        tab->banner_message.clear();
    }
    tab->search_content_empty = true;  // makes the unchanged query search again
    QueueAddressSearch(s);
}

} // namespace

bool OfferContentPack(AppState& s, bool once_per_session) {
    const auto state = app::QueryContentPackState();
    if (!app::ContentPackInstallable(state)) return true;
    if (s.isolatedTest) return false;
    if (Installer().running()) {
        if (!g_silent) ShowProgress(s, std::max(g_progress_step, 0));
        return false;
    }
    if (once_per_session && g_prompted) return false;
    g_prompted = true;
    if (AskInstall(s, state) && !StartInstall(s, false)) {
        s.notification_toast.ShowError(s.hwnd, Zh(L"内容搜索增强包", L"Content search pack"),
                                       Zh(L"无法开始下载。", L"The download could not start."));
    }
    if (s.hwndAddressEdit && s.addressSearching) SetFocus(s.hwndAddressEdit);
    return false;
}

void TickContentPackInstall(AppState& s) {
    static bool checked_update = false;
    if (!checked_update && !s.isolatedTest && s.hwnd) {
        checked_update = true;
        MaybeUpdateInBackground(s);
    }
    if (!g_owner) return;
    app::PackInstaller& installer = Installer();
    if (installer.running()) {
        if (!g_silent && s.hwnd == g_owner) {
            const int step = static_cast<int>(installer.progress() * 10.0f);
            if (step != g_progress_step && step < 10) ShowProgress(s, step);
        }
        return;
    }
    // The window that asked reports the outcome, unless it was closed.
    if (s.hwnd != g_owner && IsWindow(g_owner)) return;
    DWORD error = 0;
    app::PackFailure failure = app::PackFailure::None;
    const auto outcome = installer.TakeOutcome(error, &failure);
    if (outcome == app::PackInstallOutcome::None) return;
    const bool silent = g_silent;
    g_owner = nullptr;
    diagnostics::runtime::Event("content_pack_install_done",
                                {{"outcome", static_cast<uint64_t>(outcome)}, {"error", static_cast<uint64_t>(error)},
                                 {"failure", static_cast<uint64_t>(failure)}});
    if (outcome == app::PackInstallOutcome::Installed) {
        app::ForgetVerifiedContentPack();
        if (silent) return;
        s.notification_toast.SetKind(ui::fluent::InfoBarKind::Success);
        s.notification_toast.Show(s.hwnd, Zh(L"内容搜索增强包已安装", L"Content search pack installed"),
                                  Zh(L"现在可以搜索文件内容和查找重复文件了。",
                                     L"You can now search file contents and find duplicates."), false);
        RerunContentSearch(s);
    } else if (outcome == app::PackInstallOutcome::Failed && !silent) {
        ShowPackFailure(s, FailedPack::Content, failure, error);
    }
}

bool RetryContentPackInstall(AppState& s) {
    const bool started = StartInstall(s, false);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return started;
}

bool ContentPackBanner(DWORD error, std::wstring& title, std::wstring& message) {
    if (error != ERROR_FILE_NOT_FOUND) return false;
    if (!app::ContentPackInstallable(app::QueryContentPackState())) return false;
    title = Zh(L"内容搜索需要增强包", L"Content search needs its pack");
    message = Installer().running()
        ? Zh(L"正在下载，装好后会自动重新搜索。", L"Downloading; the search runs again once it is installed.")
        : Fill(Zh(L"按 Enter 下载并安装（约 %s）。", L"Press Enter to download and install it (about %s)."),
               DownloadSize());
    return true;
}

} // namespace pulse

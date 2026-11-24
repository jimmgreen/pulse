#pragma once
// pane_banner.h - the tone and the buttons of the strip above a file list,
// decided from the banner a tab carries (its title is always one of a few
// known strings). Kept apart from the sites that set the text so the rule
// lives in one place: errors read as errors, "press F5" becomes a button.
#include <string>

#include "../common/localization.h"
#include "../ui/ui_view_model.h"
#include "app_model.h"

namespace pulse::app {

struct PaneBannerStyle {
    int kind = 0;  // 0 info, 1 success, 2 warning, 3 error, 4 neutral
    ui::PaneBannerAction actions[2] = {ui::PaneBannerAction::None, ui::PaneBannerAction::None};
    bool dismissible = false;
};

inline PaneBannerStyle ClassifyPaneBanner(const Tab& tab, bool can_go_up, bool content_search,
                                          bool network_live_action) {
    using I = l10n::StringId;
    using A = ui::PaneBannerAction;
    PaneBannerStyle st;
    const std::wstring& title = tab.banner_title;
    const auto is = [&](I id) { return !title.empty() && title == l10n::Get(id); };
    const auto with = [&](int kind, A first, A second = A::None) {
        st.kind = kind;
        st.actions[0] = first;
        st.actions[1] = second;
        return st;
    };
    if (content_search) {
        // Content search banners keep the way to the index settings.
        if (is(I::SearchIncomplete)) return with(3, A::Retry, A::ManageIndex);
        if (is(I::ContentIndexEmpty) || is(I::ContentIndexNotCovered)) return with(2, A::ManageIndex);
        if (is(I::ResultLimitTitle)) return with(0, A::ManageIndex);
        if (is(I::ContentUpdatesPaused)) return with(0, A::Refresh, A::ManageIndex);
        return with(0, A::ManageIndex);
    }
    // Phones: plug in / allow on the phone, then retry (or it reloads by itself).
    if (is(I::DeviceDisconnectedTitle) || is(I::DeviceWaitingTitle))
        return with(2, A::Retry, can_go_up ? A::GoUp : A::None);
    if (is(I::DeviceNoResponseTitle)) return with(3, A::Retry, can_go_up ? A::GoUp : A::None);
    if (is(I::LocalFolderPermissionTitle)) return with(2, A::OpenNativeFolder, A::Retry);
    if (is(I::DriveLockedTitle)) return with(2, A::Unlock);
    if (is(I::Offline)) return with(2, A::Reconnect);
    if (is(I::CannotOpen)) {
        // A share that wants a sign-in offers that, anything else a plain retry.
        if (tab.banner_message.rfind(l10n::Get(l10n::StringId::NotSignedIn).c_str(), 0) == 0)
            return with(3, A::Reconnect, can_go_up ? A::GoUp : A::None);
        return with(3, A::Retry, can_go_up ? A::GoUp : A::None);
    }
    if (is(I::SearchIncomplete)) return with(3, A::Retry);
    if (is(I::CannotOpenSavedSearch)) return with(3, A::None);
    if (is(I::NetworkDiscoveryOffTitle)) return with(2, A::NetworkSettings, A::LanRescan);
    if (is(I::NetworkLiveAddFailed)) return with(3, network_live_action ? A::AddNetworkIndex : A::None);
    if (is(I::NetworkLiveAdded)) {
        st.kind = 1;
        st.dismissible = true;
        return st;
    }
    if (is(I::FileTagSync)) {
        st.kind = 0;
        st.dismissible = true;
        return st;
    }
    if (is(I::NetworkLiveTitle)) return with(0, network_live_action ? A::AddNetworkIndex : A::None);
    if (is(I::LocalLiveTitle)) return with(0, A::None);
    if (is(I::ContentUpdatesPaused)) return with(0, A::Refresh);
    if (is(I::RecycleBin) || is(I::Cache)) return with(4, A::None);
    if (title.empty() && tab.banner_message == l10n::Get(I::NetworkSearchSnapshot)) return with(0, A::Refresh);
    if (network_live_action) return with(0, A::AddNetworkIndex);
    return with(tab.net_readonly ? 2 : 0, A::None);
}

}  // namespace pulse::app

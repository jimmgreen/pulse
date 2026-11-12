#pragma once
#include "file_dialog_remote_provider.h"
#include "../common/dialog_bundle.h"

namespace pulse::shell_bridge {
// Remote callbacks use the original binding; keep both verified bundles leased
// for as long as that binding can launch or communicate with its UI host.
inline std::shared_ptr<const FileDialogInprocessProvider> RetainCompanionProvider(
    std::shared_ptr<const FileDialogInprocessProvider> provider,
    std::shared_ptr<dialog_bundle::BundleLease> bundle) {
    if (!provider || !bundle) return {};
    struct Lifetime {
        std::shared_ptr<dialog_bundle::BundleLease> bundle;
        std::shared_ptr<const FileDialogInprocessProvider> provider;
    };
    auto decorated = std::make_shared<FileDialogInprocessProvider>(*provider);
    decorated->module_lifetime = std::make_shared<Lifetime>(Lifetime{std::move(bundle), std::move(provider)});
    return decorated;
}
}

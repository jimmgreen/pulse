#pragma once
// Setup wizard window (P2): Direct2D/DirectWrite drawing of the setup
// pages. Runs the install
// engine on a worker thread.

#include "setup_engine.h"
#include "setup_payload.h"
#include "setup_strings.h"

#include <string>

namespace pulse::setup {

struct WizardOutcome {
    int exit_code = kExitCancelledBefore;
    bool launch = false;  // start Pulse (as the original user) after exit
    std::wstring app_dir;
    bool self_delete = false;  // uninstall: remove uninstall.exe after exit
};

WizardOutcome RunWizard(Payload& payload, const std::wstring& self_path, InstallRequest request, Lang lang);
struct UninstallRequest;
WizardOutcome RunUninstallWizard(const std::wstring& self_path, const UninstallRequest& request, Lang lang);

}  // namespace pulse::setup

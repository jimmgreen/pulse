// extension_store_live_probe.cpp - manual end-to-end check against the public
// catalog: fetch + verify index.json, download + verify a package, install it
// into an isolated store. Network access; never touches the user's store.
//   pulse_extension_store_live_probe.exe <work-dir> [id] [host-version]
#include "../app/extensions/store/catalog_service.h"
#include "../app/extensions/store/install_service.h"
#include <cstdio>
#include <filesystem>
#include <string>

using namespace pulse::extensions::store;
namespace fs = std::filesystem;

static const char* Phase(InstallServicePhase phase) {
    switch (phase) {
    case InstallServicePhase::Preparing: return "preparing";
    case InstallServicePhase::AwaitingPublisherTrust: return "awaiting publisher trust";
    case InstallServicePhase::AwaitingInstall: return "awaiting install consent";
    case InstallServicePhase::Committing: return "committing";
    case InstallServicePhase::Installed: return "installed";
    case InstallServicePhase::Failed: return "failed";
    case InstallServicePhase::Cancelled: return "cancelled";
    case InstallServicePhase::Incompatible: return "incompatible";
    default: return "idle";
    }
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { std::printf("usage: probe <work-dir> [id] [host-version]\n"); return 2; }
    const fs::path work = fs::absolute(argv[1]);
    const std::string id = argc > 2 ? fs::path(argv[2]).string() : "pulse.7zip";
    const std::string host = argc > 3 ? fs::path(argv[3]).string() : "1.0.60";
    fs::create_directories(work / L"store");
    CatalogService catalog;
    const auto generation = catalog.Refresh(work / L"catalog-cache.json");
    auto snapshot = catalog.GetSnapshot();
    for (int i = 0; i < 900 && (snapshot.generation != generation || snapshot.state == CatalogServiceState::Loading ||
                                snapshot.state == CatalogServiceState::Idle); ++i) {
        Sleep(100);
        snapshot = catalog.GetSnapshot();
    }
    std::printf("catalog state=%d error=%s\n", static_cast<int>(snapshot.state), snapshot.error.c_str());
    if (!snapshot.result) { std::printf("[FAIL] no verified catalog\n"); return 1; }
    std::printf("[PASS] signed catalog fetched and verified\n");

    InstallService install({work / L"store", host, 3});
    auto current = install.Prepare(snapshot.result, id, "1.0.0");
    for (int step = 0; step < 600; ++step) {
        const auto state = install.GetSnapshot();
        if (state.generation != current) { Sleep(100); continue; }
        if (state.phase == InstallServicePhase::AwaitingPublisherTrust) {
            std::printf("publisher trust requested; approving\n");
            install.ConfirmPublisherTrust(current, true);
        } else if (state.phase == InstallServicePhase::AwaitingInstall) {
            std::printf("[PASS] package downloaded and verified; install consent requested\n");
            install.ConfirmInstall(current, {true, true, true, true});
        } else if (state.phase == InstallServicePhase::Installed) {
            std::printf("[PASS] installed into %ls (enabled=%d restart=%d)\n", (work / L"store").c_str(), state.enabled, state.pending_restart);
            return 0;
        } else if (state.phase == InstallServicePhase::Failed || state.phase == InstallServicePhase::Incompatible ||
                   state.phase == InstallServicePhase::Cancelled) {
            std::printf("[FAIL] %s: %s\n", Phase(state.phase), state.error.c_str());
            return 1;
        }
        Sleep(100);
    }
    std::printf("[FAIL] install timed out at %s\n", Phase(install.GetSnapshot().phase));
    return 1;
}

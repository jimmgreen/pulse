#pragma once
#include <filesystem>
#include <memory>
namespace pulse {
struct AppState;
namespace ui { struct ExtensionStoreView; }
namespace app {
class ExtensionManagementUi {
public:
    explicit ExtensionManagementUi(std::filesystem::path root);
    ~ExtensionManagementUi();
    void Fill(AppState&, ui::ExtensionStoreView&, bool installation_active);
    bool Action(AppState&, int action, bool installation_active);
    void Tick(AppState&);
    void Refresh();
    void Cancel();
    void ReleaseOwner(AppState&);
    void Stop();
    bool Active() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
}

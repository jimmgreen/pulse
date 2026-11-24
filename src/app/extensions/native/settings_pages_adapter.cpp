#include "settings_pages_adapter.h"
#include "diagnostics_adapter.h"
#include "../../../../sdk/native/include/pulse/ui_data.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <tuple>
#include <vector>
namespace pulse::extensions::native {
namespace {
constexpr size_t kMaxSettingsTabs = 6;
bool AsciiIdentifier(PulseStringView view) {
    if (!view.data || !view.length || view.length > 64) return false;
    for (uint32_t i = 0; i < view.length; ++i) {
        const char c = view.data[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}
bool Utf8Wide(PulseStringView view, size_t max_bytes, bool allow_empty, std::wstring& out) {
    out.clear();
    if (view.length > max_bytes || (view.length && !view.data) ||
        (view.length && std::memchr(view.data, 0, view.length)))
        return false;
    if (!view.length) return allow_empty;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, view.data,
                                          static_cast<int>(view.length), nullptr, 0);
    if (count <= 0) return false;
    out.resize(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, view.data,
                        static_cast<int>(view.length), out.data(), count);
    return true;
}
} // namespace

struct SettingsPagesAdapter::AdapterState : std::enable_shared_from_this<AdapterState> {
    struct TabEntry {
        SettingsTabDescriptor desc;
        bool uses_primary = false;
        std::unique_ptr<UiAdapter> owned_ui;
        std::unique_ptr<UiAdapter> preview_ui;
        uint64_t preview_id = 0;
        uint32_t preview_width_dip = 320;
        SettingsPageStatus status;
        uint64_t seq = 0;
    };
    DWORD thread = GetCurrentThreadId();
    HWND owner;
    bool dark = false;
    D2D1_COLOR_F accent{};
    float scale = 1.0f;
    bool closed = false;
    std::function<bool()> alive;
    std::shared_ptr<DiagnosticsSink> diagnostics;
    UiAdapter primary_ui;
    std::vector<TabEntry> tabs;
    uint64_t next_seq = 0;

    AdapterState(HWND window, bool is_dark, D2D1_COLOR_F acc, float sc, std::function<bool()> live)
        : owner(window),
          dark(is_dark),
          accent(acc),
          scale(sc),
          alive(std::move(live)),
          primary_ui(window, is_dark, acc, sc, [this] { return Valid(); }) {
        primary_ui.Embed();
    }
    bool Valid() const { return GetCurrentThreadId() == thread && !closed && IsWindow(owner) && alive && alive(); }
    UiAdapter* TabUi(TabEntry& entry) { return entry.uses_primary ? &primary_ui : entry.owned_ui.get(); }
    const UiAdapter* TabUi(const TabEntry& entry) const { return entry.uses_primary ? &primary_ui : entry.owned_ui.get(); }
    TabEntry* FindTab(uint64_t id) {
        if (!Valid() || !id) return nullptr;
        for (auto& tab : tabs) {
            if (tab.desc.id == id) {
                auto* ui = TabUi(tab);
                return ui && ui->HasPanel(id) ? &tab : nullptr;
            }
        }
        return nullptr;
    }
    const TabEntry* FindTab(uint64_t id) const {
        if (!Valid() || !id) return nullptr;
        for (const auto& tab : tabs) {
            if (tab.desc.id == id) {
                const auto* ui = TabUi(tab);
                return ui && ui->HasPanel(id) ? &tab : nullptr;
            }
        }
        return nullptr;
    }
    TabEntry* PrimaryTab() {
        if (!Valid()) return nullptr;
        for (auto& tab : tabs) {
            auto* ui = TabUi(tab);
            if (ui && ui->HasPanel(tab.desc.id)) return &tab;
        }
        return nullptr;
    }
    const TabEntry* PrimaryTab() const {
        if (!Valid()) return nullptr;
        for (const auto& tab : tabs) {
            const auto* ui = TabUi(tab);
            if (ui && ui->HasPanel(tab.desc.id)) return &tab;
        }
        return nullptr;
    }
    bool Has(uint64_t id) const { return FindTab(id) != nullptr; }
    PulseStatus Fail(PulseStatus status, std::string_view op, std::string_view field, std::string_view msg) const {
        if (diagnostics) diagnostics->Record(status, PULSE_SETTINGS_PAGES_INTERFACE_NAME, op, "", field, msg);
        return status;
    }
    template<class F> static PulseStatus Guard(void* context, F action) noexcept {
        if (!context) return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<AdapterState*>(context)->shared_from_this();
            return self->Valid() ? action(*self) : PULSE_ERROR_STATE;
        } catch (...) { return PULSE_ERROR_STATE; }
    }
    void SortTabs() {
        std::stable_sort(tabs.begin(), tabs.end(), [](const TabEntry& a, const TabEntry& b) {
            return std::tie(a.desc.order, a.seq) < std::tie(b.desc.order, b.seq);
        });
    }
    static PulseStatus PULSE_CALL Create(void* context, PulseStringView title, const PulseUiModelTree* tree, uint64_t* out) {
        if (out) *out = 0;
        return Guard(context, [&](AdapterState& self) -> PulseStatus {
            if (!out || !tree)
                return self.Fail(PULSE_ERROR_ARGUMENT, "create", !out ? "page" : "model", "null model or output handle");
            std::wstring caption;
            if (!Utf8Wide(title, 128, false, caption))
                return self.Fail(PULSE_ERROR_ARGUMENT, "create", "title", "title must be 1..128 valid UTF-8 bytes without NUL");
            for (const auto& tab : self.tabs) {
                if (tab.desc.tab_id == "default" && self.Has(tab.desc.id))
                    return self.Fail(PULSE_ERROR_STATE, "create", "", "a settings page is already open for this session");
            }
            if (self.tabs.size() >= kMaxSettingsTabs)
                return self.Fail(PULSE_ERROR_STATE, "create", "", "settings tab limit (6) reached");
            PulseUiModelV1 model{};
            const auto queried = self.primary_ui.QueryModel(1, &model, sizeof(model));
            if (queried != PULSE_OK) return queried;
            uint64_t id = 0;
            const auto status = model.create(model.context, title, tree, &id);
            if (status == PULSE_OK) {
                TabEntry entry;
                entry.desc = {id, "default", std::move(caption), {}, 0};
                entry.uses_primary = true;
                entry.seq = ++self.next_seq;
                self.tabs.push_back(std::move(entry));
                self.SortTabs();
                *out = id;
                if (IsWindow(self.owner)) InvalidateRect(self.owner, nullptr, FALSE);
            }
            return status;
        });
    }
    static PulseStatus PULSE_CALL CreateTab(void* context, const PulseSettingsTabDesc* desc, const PulseUiModelTree* tree, uint64_t* out) {
        if (out) *out = 0;
        return Guard(context, [&](AdapterState& self) -> PulseStatus {
            if (!out || !desc || !tree)
                return self.Fail(PULSE_ERROR_ARGUMENT, "create_tab", !out ? "page" : !desc ? "desc" : "model",
                                 "null descriptor, model, or output handle");
            if (desc->size != sizeof(PulseSettingsTabDesc) || desc->flags != 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "create_tab", "desc", "invalid descriptor size or flags");
            if (!AsciiIdentifier(desc->id))
                return self.Fail(PULSE_ERROR_ARGUMENT, "create_tab", "id", "tab id must be 1..64 ASCII [A-Za-z0-9._-] bytes");
            std::wstring caption, glyph_wide;
            if (!Utf8Wide(desc->title, 64, false, caption))
                return self.Fail(PULSE_ERROR_ARGUMENT, "create_tab", "title", "tab title must be 1..64 valid UTF-8 bytes without NUL");
            if (!Utf8Wide(desc->glyph, 64, true, glyph_wide))
                return self.Fail(PULSE_ERROR_ARGUMENT, "create_tab", "glyph", "tab glyph must be <= 64 valid UTF-8 bytes without NUL");
            const std::string tab_id(desc->id.data, desc->id.length);
            for (const auto& tab : self.tabs) {
                if (tab.desc.tab_id == tab_id && self.Has(tab.desc.id))
                    return self.Fail(PULSE_ERROR_STATE, "create_tab", "id", "duplicate settings tab id");
            }
            if (self.tabs.size() >= kMaxSettingsTabs)
                return self.Fail(PULSE_ERROR_STATE, "create_tab", "", "settings tab limit (6) reached");
            auto adapter = std::make_unique<UiAdapter>(self.owner, self.dark, self.accent, self.scale,
                                                       [weak = self.weak_from_this()] {
                                                           const auto locked = weak.lock();
                                                           return locked && locked->Valid();
                                                       });
            adapter->Embed();
            if (self.diagnostics) adapter->SetDiagnostics(self.diagnostics);
            PulseUiModelV1 model{};
            const auto queried = adapter->QueryModel(1, &model, sizeof(model));
            if (queried != PULSE_OK) return queried;
            uint64_t id = 0;
            const auto status = model.create(model.context, desc->title, tree, &id);
            if (status == PULSE_OK) {
                TabEntry entry;
                entry.desc = {id, tab_id, std::move(caption),
                              desc->glyph.length ? std::string(desc->glyph.data, desc->glyph.length) : std::string{},
                              desc->order};
                entry.uses_primary = false;
                entry.owned_ui = std::move(adapter);
                entry.seq = ++self.next_seq;
                self.tabs.push_back(std::move(entry));
                self.SortTabs();
                *out = id;
                if (IsWindow(self.owner)) InvalidateRect(self.owner, nullptr, FALSE);
            }
            return status;
        });
    }
    static PulseStatus PULSE_CALL PageQuery(void* context, uint64_t id, PulseStringView name, uint32_t version, void* out, uint32_t size) {
        return Guard(context, [&](AdapterState& self) -> PulseStatus {
            auto* tab = self.FindTab(id);
            if (!tab) return PULSE_ERROR_STATE;
            auto* ui = self.TabUi(*tab);
            if (!ui) return PULSE_ERROR_STATE;
            if (!name.data || !name.length || name.length > 64 || std::memchr(name.data, 0, name.length)) return PULSE_ERROR_ARGUMENT;
            const std::string value(name.data, name.length);
            if (value == PULSE_UI_MODEL_INTERFACE) {
                const auto status = ui->QueryModel(version, out, size);
                if (status == PULSE_OK) {
                    static_cast<PulseUiModelV1*>(out)->create =
                        [](void*, PulseStringView, const PulseUiModelTree*, uint64_t* page) -> PulseStatus {
                        if (page) *page = 0;
                        return PULSE_ERROR_STATE;
                    };
                }
                return status;
            }
            if (value == PULSE_UI_EVENTS_INTERFACE_NAME) return ui->QueryEvents(version, out, size);
            if (value == PULSE_UI_DATA_INTERFACE) return ui->QueryData(version, out, size);
            return PULSE_ERROR_UNAVAILABLE;
        });
    }
    static PulseStatus PULSE_CALL Remove(void* context, uint64_t id) {
        return Guard(context, [&](AdapterState& self) -> PulseStatus {
            auto* tab = self.FindTab(id);
            if (!tab) return PULSE_ERROR_STATE;
            auto* ui = self.TabUi(*tab);
            if (!ui) return PULSE_ERROR_STATE;
            if (tab->preview_ui) {
                tab->preview_ui->Stop();
                tab->preview_ui.reset();
                tab->preview_id = 0;
            }
            PulseUiModelV1 api{};
            const auto status = ui->QueryModel(1, &api, sizeof(api));
            if (status != PULSE_OK) return status;
            const auto result = api.close(api.context, id);
            if (result == PULSE_OK) {
                std::erase_if(self.tabs, [id](const TabEntry& item) { return item.desc.id == id; });
                if (IsWindow(self.owner)) InvalidateRect(self.owner, nullptr, FALSE);
            }
            return result;
        });
    }
    static PulseStatus PULSE_CALL SetStatus(void* context, uint64_t id, uint32_t kind, PulseStringView text) {
        return Guard(context, [&](AdapterState& self) -> PulseStatus {
            auto* tab = self.FindTab(id);
            if (!tab) return PULSE_ERROR_STATE;
            if (kind > PULSE_SETTINGS_STATUS_ERROR)
                return self.Fail(PULSE_ERROR_ARGUMENT, "set_status", "kind", "invalid settings status kind");
            std::wstring label;
            if (!Utf8Wide(text, 128, true, label))
                return self.Fail(PULSE_ERROR_ARGUMENT, "set_status", "text", "status text must be <= 128 valid UTF-8 bytes without NUL");
            tab->status = {kind, std::move(label)};
            if (IsWindow(self.owner)) InvalidateRect(self.owner, nullptr, FALSE);
            return PULSE_OK;
        });
    }
    static PulseStatus PULSE_CALL SetSidePreview(void* context, uint64_t id, const PulseUiModelTree* preview, uint32_t width_dip) {
        return Guard(context, [&](AdapterState& self) -> PulseStatus {
            auto* tab = self.FindTab(id);
            if (!tab) return PULSE_ERROR_STATE;
            if (!preview) {
                if (tab->preview_ui) {
                    tab->preview_ui->Stop();
                    tab->preview_ui.reset();
                }
                tab->preview_id = 0;
                if (IsWindow(self.owner)) InvalidateRect(self.owner, nullptr, FALSE);
                return PULSE_OK;
            }
            const uint32_t clamped_width = width_dip == 0 ? 320u : (std::clamp)(width_dip, 240u, 440u);
            if (tab->preview_id != 0 && tab->preview_ui && tab->preview_ui->HasPanel(tab->preview_id)) {
                PulseUiModelV1 api{};
                const auto queried = tab->preview_ui->QueryModel(1, &api, sizeof(api));
                if (queried != PULSE_OK) return queried;
                const auto updated = api.update(api.context, tab->preview_id, preview);
                if (updated == PULSE_OK) {
                    tab->preview_width_dip = clamped_width;
                    if (IsWindow(self.owner)) InvalidateRect(self.owner, nullptr, FALSE);
                }
                return updated;
            }
            auto adapter = std::make_unique<UiAdapter>(self.owner, self.dark, self.accent, self.scale,
                                                       [weak = self.weak_from_this()] {
                                                           const auto locked = weak.lock();
                                                           return locked && locked->Valid();
                                                       });
            adapter->Embed();
            if (self.diagnostics) adapter->SetDiagnostics(self.diagnostics);
            PulseUiModelV1 api{};
            const auto queried = adapter->QueryModel(1, &api, sizeof(api));
            if (queried != PULSE_OK) return queried;
            uint64_t new_id = 0;
            const auto created = api.create(api.context, {"Preview", 7}, preview, &new_id);
            if (created == PULSE_OK) {
                tab->preview_ui = std::move(adapter);
                tab->preview_id = new_id;
                tab->preview_width_dip = clamped_width;
                if (IsWindow(self.owner)) InvalidateRect(self.owner, nullptr, FALSE);
            }
            return created;
        });
    }
};
SettingsPagesAdapter::SettingsPagesAdapter(HWND owner, bool dark, D2D1_COLOR_F accent, float scale, std::function<bool()> alive)
    : state_(std::make_shared<AdapterState>(owner, dark, accent, scale, std::move(alive))) {}
SettingsPagesAdapter::~SettingsPagesAdapter() { Close(); }
void SettingsPagesAdapter::SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink) {
    state_->diagnostics = sink;
    state_->primary_ui.SetDiagnostics(sink);
    for (auto& tab : state_->tabs) {
        if (tab.owned_ui) tab.owned_ui->SetDiagnostics(sink);
        if (tab.preview_ui) tab.preview_ui->SetDiagnostics(sink);
    }
}
PulseStatus SettingsPagesAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (version == PULSE_SETTINGS_PAGES_VERSION_1) {
            if (!out || size < sizeof(PulseSettingsPagesV1)) return PULSE_ERROR_ARGUMENT;
            const PulseSettingsPagesV1 api{sizeof(api), PULSE_SETTINGS_PAGES_VERSION_1, state_.get(),
                                           AdapterState::Create, AdapterState::PageQuery, AdapterState::Remove};
            std::memcpy(out, &api, sizeof(api));
            return PULSE_OK;
        }
        if (version == PULSE_SETTINGS_PAGES_VERSION_2) {
            if (!out || size < sizeof(PulseSettingsPagesV2)) return PULSE_ERROR_ARGUMENT;
            const PulseSettingsPagesV2 api{sizeof(api), PULSE_SETTINGS_PAGES_VERSION_2, state_.get(),
                                           AdapterState::Create, AdapterState::PageQuery, AdapterState::Remove,
                                           AdapterState::CreateTab, AdapterState::SetStatus, AdapterState::SetSidePreview};
            std::memcpy(out, &api, sizeof(api));
            return PULSE_OK;
        }
        return PULSE_ERROR_ABI;
    } catch (...) { return PULSE_ERROR_STATE; }
}
std::optional<SettingsPageDescriptor> SettingsPagesAdapter::Page() const {
    if (const auto* tab = state_->PrimaryTab()) {
        return SettingsPageDescriptor{tab->desc.id, tab->desc.title};
    }
    return std::nullopt;
}
std::vector<SettingsTabDescriptor> SettingsPagesAdapter::Tabs() const {
    std::vector<SettingsTabDescriptor> out;
    if (!state_->Valid()) return out;
    for (const auto& tab : state_->tabs) {
        const auto* ui = state_->TabUi(tab);
        if (ui && ui->HasPanel(tab.desc.id)) out.push_back(tab.desc);
    }
    return out;
}
SettingsPageStatus SettingsPagesAdapter::PageStatus(uint64_t page) const {
    if (const auto* tab = state_->FindTab(page)) return tab->status;
    return {};
}
bool SettingsPagesAdapter::HasSidePreview(uint64_t page) const {
    const auto* tab = state_->FindTab(page);
    return tab && tab->preview_id != 0 && tab->preview_ui && tab->preview_ui->HasPanel(tab->preview_id);
}
bool SettingsPagesAdapter::Show(uint64_t page, RECT bounds, bool dark, D2D1_COLOR_F accent, float scale, D2D1_COLOR_F wash) {
    state_->dark = dark;
    state_->accent = accent;
    state_->scale = scale;
    auto* active = state_->FindTab(page);
    if (!active) return false;
    for (auto& tab : state_->tabs) {
        if (tab.desc.id == page) continue;
        if (auto* ui = state_->TabUi(tab)) ui->HideEmbedded();
        if (tab.preview_ui) tab.preview_ui->HideEmbedded();
    }
    auto* main_ui = state_->TabUi(*active);
    if (!main_ui) return false;
    if (active->preview_id != 0 && active->preview_ui && active->preview_ui->HasPanel(active->preview_id)) {
        // The preview column sits right beside the settings (8 DIP; both trees add their own inset), at
        // its declared width, narrowed down to 260 DIP before the settings drop under 480 DIP. Without
        // room for both the preview steps aside: a clipped split of two scroll views reads worse than
        // the settings alone (the 右键菜单 tab carries its own live preview).
        const LONG width = bounds.right - bounds.left;
        const LONG gap = static_cast<LONG>(8.0f * scale);
        const LONG min_main = static_cast<LONG>(480.0f * scale);
        const LONG declared = static_cast<LONG>(static_cast<float>(active->preview_width_dip) * scale);
        const LONG preview_w = (std::min)(declared, width - min_main - gap);
        if (preview_w >= static_cast<LONG>(260.0f * scale)) {
            RECT main_rc{bounds.left, bounds.top, bounds.right - preview_w - gap, bounds.bottom};
            RECT prev_rc{bounds.right - preview_w, bounds.top, bounds.right, bounds.bottom};
            const bool shown_main = main_ui->ShowEmbedded(main_rc, dark, accent, scale, wash);
            active->preview_ui->ShowEmbedded(prev_rc, dark, accent, scale, wash);
            return shown_main;
        }
        active->preview_ui->HideEmbedded();
    } else if (active->preview_ui) {
        active->preview_ui->HideEmbedded();
    }
    return main_ui->ShowEmbedded(bounds, dark, accent, scale, wash);
}
void SettingsPagesAdapter::Hide() {
    state_->primary_ui.HideEmbedded();
    for (auto& tab : state_->tabs) {
        if (tab.owned_ui) tab.owned_ui->HideEmbedded();
        if (tab.preview_ui) tab.preview_ui->HideEmbedded();
    }
}
void SettingsPagesAdapter::Close() {
    if (GetCurrentThreadId() == state_->thread) {
        state_->closed = true;
        state_->primary_ui.Stop();
        for (auto& tab : state_->tabs) {
            if (tab.owned_ui) tab.owned_ui->Stop();
            if (tab.preview_ui) tab.preview_ui->Stop();
        }
        state_->tabs.clear();
        if (IsWindow(state_->owner)) InvalidateRect(state_->owner, nullptr, FALSE);
    }
}
bool SettingsPagesAdapter::InCallback() const {
    if (state_->primary_ui.InCallback()) return true;
    for (const auto& tab : state_->tabs) {
        if (tab.owned_ui && tab.owned_ui->InCallback()) return true;
        if (tab.preview_ui && tab.preview_ui->InCallback()) return true;
    }
    return false;
}
UiAdapter& SettingsPagesAdapter::Ui() { return state_->primary_ui; }
std::optional<std::string> SettingsPagesAdapter::State(const std::string& key) const {
    const auto* tab = state_->PrimaryTab();
    const auto* ui = tab ? state_->TabUi(*tab) : nullptr;
    return ui ? ui->EmbeddedState(key) : std::nullopt;
}
bool SettingsPagesAdapter::CommitState(const std::string& key, const std::string& value) {
    auto* tab = state_->PrimaryTab();
    auto* ui = tab ? state_->TabUi(*tab) : nullptr;
    return ui && ui->CommitEmbeddedState(key, value);
}
bool SettingsPagesAdapter::Click(const std::string& node) {
    auto* tab = state_->PrimaryTab();
    auto* ui = tab ? state_->TabUi(*tab) : nullptr;
    return ui && ui->ClickEmbedded(node);
}
}
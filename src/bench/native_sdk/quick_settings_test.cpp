#include "../../app/extensions/manifest.h"
#include "../../app/extensions/quick_settings_model.h"

#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace pulse::extensions;

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (ok) ++g_passed;
    else ++g_failed;
}

std::wstring LocalizeEn(const ManifestLocalizedText& t) {
    const std::string& s = t.en.empty() ? t.zh : t.en;
    return detail::Utf8ToWide(s);
}

void TestEncodeDecodeActions() {
    const int a0 = EncodeQuickControlAction(0, 0);
    const int a1 = EncodeQuickControlAction(3, 7);
    Check(a0 == kStoreQuickControlActionBase && a1 == kStoreQuickControlActionBase + 3 * 16 + 7,
          "EncodeQuickControlAction maps (card, ctrl) deterministically");
    Check(EncodeQuickControlAction(kMaxStoreQuickCards, 0) == -1 &&
              EncodeQuickControlAction(0, kMaxStoreQuickControlsPerCard) == -1,
          "EncodeQuickControlAction rejects out-of-range indices");

    size_t card = 99, ctrl = 99;
    Check(DecodeQuickControlAction(a1, card, ctrl) && card == 3 && ctrl == 7,
          "DecodeQuickControlAction recovers exact (card, ctrl)");
    Check(!DecodeQuickControlAction(kStoreQuickControlActionBase - 1, card, ctrl) &&
              !DecodeQuickControlAction(kStoreQuickControlActionBase + 9, card, ctrl) &&
              !DecodeQuickControlAction(kStoreQuickControlActionBase + 10 * 16, card, ctrl),
          "DecodeQuickControlAction rejects invalid or out-of-stride actions");
}

void TestResolveAndEditLifecycle() {
    const std::string json =
        R"({"apiVersion":1,"id":"acme.archiver","name":"Acme Archiver",)"
        R"("native":{"version":1,"architecture":"windows-x64","library":"acme.dll","sha256":")" +
        std::string(64, 'a') +
        R"("},)"
        R"("showcase":{"version":"2.1.0","quickSettings":{)"
        R"("title":{"zh":"快捷配置","en":"Quick Setup"},)"
        R"("readyState":"ready","statusState":"save_status",)"
        R"("rows":[)"
        R"({"title":"Extract Mode","description":"Double-click mode","controls":[)"
        R"({"kind":"select","state":"mode","width":180,"options":[{"value":"smart","label":"Smart"},{"value":"here","label":"Here"}]}]},)"
        R"({"title":"Compression","controls":[)"
        R"({"kind":"select","state":"fmt","width":96,"options":[{"value":"7z","label":"7Z"},{"value":"tar","label":"TAR"}]},)"
        R"({"kind":"select","state":"lvl","width":104,"options":[{"value":"0","label":"Store"},{"value":"5","label":"Ultra"}],"disabledWhen":{"state":"fmt","equals":"tar"},"disabledLabel":"No compression"}]},)"
        R"({"title":"Remember Choice","controls":[{"kind":"switch","state":"remember"}]},)"
        R"({"title":"Reset","controls":[{"kind":"button","command":"reset_defaults","label":"Restore","width":92}]})"
        R"(],)"
        R"("menuPreview":{"caption":"Archive Menu Preview","submenuState":"use_sub","sampleEntries":[{"name":"demo.7z"},{"name":"dir","directory":true}]}}}})";

    const auto parsed = ParseManifest(json);
    Check(parsed && parsed.manifest->showcase && parsed.manifest->showcase->quick_settings,
          "QuickSettings manifest parses cleanly");
    const auto& decl = *parsed.manifest->showcase->quick_settings;

    std::map<std::string, std::string> state = {
        {"ready", "false"},
        {"save_status", "Loading"},
        {"mode", "smart"},
        {"fmt", "7z"},
        {"lvl", "5"},
        {"remember", "true"},
        {"use_sub", "true"},
    };
    const auto reader = [&](const std::string& k) -> std::optional<std::string> {
        const auto it = state.find(k);
        return it == state.end() ? std::nullopt : std::optional<std::string>(it->second);
    };

    // 1. Page not present / not ready stays unavailable
    const auto card_no_page = ResolveQuickSettingsCard(decl, reader, LocalizeEn, false);
    const auto card_not_ready = ResolveQuickSettingsCard(decl, reader, LocalizeEn, true);
    Check(!card_no_page.available && !card_not_ready.available &&
              card_not_ready.title == L"Quick Setup" && card_not_ready.has_menu_preview,
          "ResolveQuickSettingsCard keeps available=false until page present and readyState=='true'");

    // 2. Ready state populates controls accurately
    state["ready"] = "true";
    state["save_status"] = "Saved";
    const auto card_ready = ResolveQuickSettingsCard(decl, reader, LocalizeEn, true);
    Check(card_ready.available && card_ready.status == L"Saved" &&
              card_ready.preview_submenu && card_ready.preview_caption == L"Archive Menu Preview" &&
              card_ready.rows.size() == 4,
          "ResolveQuickSettingsCard resolves ready header, status, and preview state");
    Check(card_ready.rows[0].controls[0].label == L"Smart" &&
              card_ready.rows[1].controls[0].label == L"7Z" &&
              !card_ready.rows[1].controls[1].disabled &&
              card_ready.rows[1].controls[1].label == L"Ultra" &&
              card_ready.rows[2].controls[0].checked &&
              card_ready.rows[3].controls[0].label == L"Restore" &&
              card_ready.rows[3].controls[0].width_dip == 92,
          "ResolveQuickSettingsCard resolves select, switch, and button controls");

    // 3. Edit calculations: switch toggle, select option, button click
    const auto edit_select = ComputeQuickControlEdit(decl, 0, reader, 1);
    const auto edit_switch = ComputeQuickControlEdit(decl, 3, reader);
    const auto edit_button = ComputeQuickControlEdit(decl, 4, reader);
    Check(edit_select && !edit_select->is_click && edit_select->key_or_command == "mode" && edit_select->value == "here" &&
              edit_switch && !edit_switch->is_click && edit_switch->key_or_command == "remember" && edit_switch->value == "false" &&
              edit_button && edit_button->is_click && edit_button->key_or_command == "reset_defaults",
          "ComputeQuickControlEdit produces exact CommitState and Click edits");
    Check(!ComputeQuickControlEdit(decl, 0, reader, 99).has_value() &&
              !ComputeQuickControlEdit(decl, 99, reader).has_value(),
          "ComputeQuickControlEdit rejects invalid option or control indices");

    // 4. Conditional disabledWhen + disabledLabel
    state["fmt"] = "tar";
    const auto card_tar = ResolveQuickSettingsCard(decl, reader, LocalizeEn, true);
    Check(card_tar.rows[1].controls[1].disabled &&
              card_tar.rows[1].controls[1].label == L"No compression" &&
              !ComputeQuickControlEdit(decl, 2, reader, 0).has_value(),
          "disabledWhen disables dependent control, shows disabledLabel, and blocks edits");
}

} // namespace

int main() {
    TestEncodeDecodeActions();
    TestResolveAndEditLifecycle();
    std::printf("Summary: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

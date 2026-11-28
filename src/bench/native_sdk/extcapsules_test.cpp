// extcapsules_test.cpp - tests pulse.extcapsules ABI validation, multi-package
// precedence, O(1) lookup edge cases, and session-close cleanup.
#include <pulse/client.hpp>
#include <pulse/extcapsules.h>
#include "../../app/extensions/extcapsule_registry.h"
#include "../../app/extensions/native/diagnostics_adapter.h"
#include "../../app/extensions/native/extcapsules_adapter.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* what) {
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what);
    }
}

using pulse::extensions::extcapsules::Registry;
using pulse::extensions::native::DiagnosticsSink;
using pulse::extensions::native::ExtCapsulesAdapter;
namespace capsule = pulse::ui::ext_capsule;

PulseStatus PULSE_CALL QueryAdapter(void* ctx, PulseStringView name, uint32_t version, void* out, uint32_t size) noexcept {
    if (std::string_view(name.data ? name.data : "", name.length) == PULSE_EXTCAPSULES_INTERFACE_NAME)
        return static_cast<ExtCapsulesAdapter*>(ctx)->Query(version, out, size);
    return PULSE_ERROR_UNAVAILABLE;
}

}  // namespace

int main() {
    // 1. Query validation and ABI negotiation.
    {
        Registry reg(false);
        auto diag = std::make_shared<DiagnosticsSink>();
        ExtCapsulesAdapter adapter("com.acme.capsules", [] { return true; }, diag, &reg);
        PulseExtCapsulesV1 table{};
        Check(adapter.Query(PULSE_EXTCAPSULES_VERSION_1, &table, sizeof(table) - 1) == PULSE_ERROR_ARGUMENT,
              "reject undersized table");
        Check(adapter.Query(99, &table, sizeof(table)) == PULSE_ERROR_UNAVAILABLE, "reject unknown version");
        Check(adapter.Query(PULSE_EXTCAPSULES_VERSION_1, &table, sizeof(table)) == PULSE_OK &&
                  table.set_rules && table.clear_rules,
              "query V1 succeeds");

        // Reject invalid rules / off-thread calls.
        PulseExtCapsuleRule bad{};
        bad.size = sizeof(bad) - 4;
        Check(table.set_rules(table.context, nullptr, &bad, 1) == PULSE_ERROR_ABI, "reject undersized rule");
        bad.size = sizeof(bad);
        bad.extension = {"psd", 3};
        bad.flags = 0x8000u;
        bad.dark_bg = 0x001E36;
        bad.dark_text = 0x31A8FF;
        Check(table.set_rules(table.context, nullptr, &bad, 1) == PULSE_ERROR_ARGUMENT, "reject unknown flags");
        bad.flags = 0;
        bad.extension = {"bad ext", 7};
        Check(table.set_rules(table.context, nullptr, &bad, 1) == PULSE_ERROR_ARGUMENT, "reject space in extension");
        bad.extension = {"psd", 3};
        bad.dark_bg = 0;
        bad.light_bg = 0;
        Check(table.set_rules(table.context, nullptr, &bad, 1) == PULSE_ERROR_ARGUMENT, "reject missing bg color");

        PulseStatus off_thread = PULSE_OK;
        std::thread worker([&] { off_thread = table.clear_rules(table.context); });
        worker.join();
        Check(off_thread == PULSE_ERROR_STATE, "reject off-thread call");
    }

    // 2. Client wrapper, case/dot normalization, filename edge cases, and multi-package precedence.
    {
        Registry reg(false);
        ExtCapsulesAdapter a("com.first.capsules", [] { return true; }, nullptr, &reg);
        ExtCapsulesAdapter b("com.second.capsules", [] { return true; }, nullptr, &reg);

        PulseHostV1 ha{sizeof(ha), PULSE_ABI_VERSION_1, &a, &QueryAdapter};
        PulseHostV1 hb{sizeof(hb), PULSE_ABI_VERSION_1, &b, &QueryAdapter};
        auto ca = pulse::sdk::ExtCapsulesClient::TryQuery(pulse::sdk::Host(ha));
        auto cb = pulse::sdk::ExtCapsulesClient::TryQuery(pulse::sdk::Host(hb));
        Check(ca.has_value() && cb.has_value(), "ExtCapsulesClient queries both adapters");

        std::vector<pulse::sdk::ExtCapsuleRule> rules_a;
        rules_a.push_back({".PSD", 0x001E36, 0x31A8FF, 0x0B4A7D, 0x001E36, 0x31A8FF, 0x0B4A7D, 0u});
        rules_a.push_back({"ai", 0xFFF2E2, 0xB85000, 0xFFD5A3, 0x331700, 0xFF9A00, 0x6E3400,
                           PULSE_EXT_CAPSULE_FLAG_UPPERCASE | PULSE_EXT_CAPSULE_FLAG_STRIP_DOT});
        Check(ca->SetRules(rules_a) == PULSE_OK, "package A registers rules");
        Check(reg.RuleCount() == 2, "registry compiled 2 rules");

        // Package B tries to override psd and adds prproj.
        std::vector<pulse::sdk::ExtCapsuleRule> rules_b;
        rules_b.push_back({"psd", 0x111111, 0x222222, 0, 0x111111, 0x222222, 0, 0u});
        rules_b.push_back({"prproj", 0x241047, 0xC084FC, 0x522794, 0x241047, 0xC084FC, 0x522794, 0u});
        Check(cb->SetRules(rules_b) == PULSE_OK, "package B registers rules");
        Check(reg.RuleCount() == 3, "registry merged 3 unique extensions");

        const auto table = reg.Compiled();
        Check(table != nullptr, "compiled table is non-null");

        // Match .PSD with leading dot lowercase (screenshot style) and first-package precedence.
        const auto m_psd = capsule::MatchFileName(L"cxh-34537.v2.PSD", false, table.get());
        Check(m_psd.has_value() && m_psd->stem == L"cxh-34537.v2" && m_psd->label == L".psd" &&
                  m_psd->bg_argb == 0xFF001E36u && m_psd->text_argb == 0xFF31A8FFu,
              "psd matches multi-dot uppercase filename with package A colors and screenshot label");

        // Match .ai with uppercase strip-dot and dual light/dark theme colors.
        const auto m_ai_light = capsule::MatchFileName(L"poster.ai", false, table.get());
        const auto m_ai_dark = capsule::MatchFileName(L"poster.ai", true, table.get());
        Check(m_ai_light.has_value() && m_ai_light->label == L"AI" && m_ai_light->bg_argb == 0xFFFFF2E2u,
              "ai light theme colors and uppercase stripped label");
        Check(m_ai_dark.has_value() && m_ai_dark->label == L"AI" && m_ai_dark->bg_argb == 0xFF331700u,
              "ai dark theme colors");

        // Edge cases: excluded jpg, dotfile, extensionless, trailing dot.
        Check(!capsule::MatchFileName(L"photo.jpg", false, table.get()).has_value(), "jpg not matched");
        Check(!capsule::MatchFileName(L".psd", false, table.get()).has_value(), "dotfile .psd not matched");
        Check(!capsule::MatchFileName(L"README", false, table.get()).has_value(), "extensionless not matched");
        Check(!capsule::MatchFileName(L"broken.", false, table.get()).has_value(), "trailing dot not matched");

        // Closing package A drops its rules; package B's psd takes over.
        a.Close();
        const auto after_a = reg.Compiled();
        const auto m_psd_b = capsule::MatchFileName(L"cxh.psd", false, after_a.get());
        Check(m_psd_b.has_value() && m_psd_b->bg_argb == 0xFF111111u, "after A closes, B's psd takes over");

        // Closing package B empties registry cleanly.
        b.Close();
        Check(reg.RuleCount() == 0 && reg.Compiled() == nullptr, "after all close, registry is empty");
    }

    std::printf("pulse_native_extcapsules_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

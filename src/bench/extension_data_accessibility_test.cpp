#include "../ui/extension_data_accessibility.h"
#include <cstdio>
#include <memory>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using namespace pulse::ui;
namespace {
int failures = 0;
void Check(bool result, const char* name) {
    std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
    if (!result) {
        ++failures;
    }
}
VARIANT Child(long id) {
    VARIANT value{};
    value.vt = VT_I4;
    value.lVal = id;
    return value;
}
struct Owner {
    bool connected = true, disconnect_during_action = false;
    int activated = 0;
    DataAccessibleSnapshot snapshot;
};
ComPtr<IAccessible> Create(const std::shared_ptr<Owner>& owner) {
    const std::weak_ptr<Owner> weak = owner;
    DataAccessibleCallbacks callbacks;
    callbacks.snapshot = [weak]() -> std::optional<DataAccessibleSnapshot> {
        const auto o = weak.lock();
        return o && o->connected ? std::optional(o->snapshot) : std::nullopt;
    };
    callbacks.select = [weak](const std::string& id) {
        const auto o = weak.lock();
        if (!o || !o->connected) {
            return false;
        }
        for (auto& r : o->snapshot.rows) {
            r.selected = r.id == id;
        }
        return true;
    };
    callbacks.activate = [weak](const std::string&) {
        const auto o = weak.lock();
        if (!o || !o->connected) {
            return false;
        }
        ++o->activated;
        if (o->disconnect_during_action) {
            o->connected = false;
        }
        return true;
    };
    ComPtr<IAccessible> accessible;
    Check(SUCCEEDED(CreateExtensionDataAccessible(nullptr, std::move(callbacks), &accessible)),
          "create snapshot-only provider");
    return accessible;
}
} // namespace
int main() {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        return 2;
    }
    auto owner = std::make_shared<Owner>();
    owner->snapshot.name = L"Rows";
    owner->snapshot.default_action_name = L"Open";
    owner->snapshot.screen_bounds = {100, 200, 500, 400};
    owner->snapshot.focused = true;
    DataAccessibleRow first;
    first.id = "first";
    first.name = L"First cell fallback";
    first.screen_bounds = {100, 200, 500, 232};
    first.selected = true;
    first.focused = true;
    first.has_children = true;
    DataAccessibleRow second;
    second.id = "second";
    second.name = L"Disabled";
    second.screen_bounds = {100, 232, 500, 264};
    second.enabled = false;
    DataAccessibleRow third;
    third.id = "third";
    third.name = L"Offscreen";
    third.screen_bounds = {100, 900, 500, 932};
    third.offscreen = true;
    owner->snapshot.rows = {first, second, third};
    auto accessible = Create(owner);
    long count = 0;
    Check(accessible->get_accChildCount(&count) == S_OK && count == 3,
          "simple children expose all flattened rows");
    BSTR name = nullptr;
    Check(accessible->get_accName(Child(1), &name) == S_OK && std::wstring(name) == first.name,
          "snapshot row name preserved");
    SysFreeString(name);
    Check(accessible->get_accName(Child(4), &name) == E_INVALIDARG, "out-of-range child rejected");
    Check(accessible->get_accName(Child(-1), &name) == E_INVALIDARG, "negative child rejected");
    VARIANT wrong{};
    wrong.vt = VT_BSTR;
    wrong.bstrVal = nullptr;
    Check(accessible->get_accName(wrong, &name) == E_INVALIDARG, "wrong variant child type rejected");
    VARIANT result{};
    Check(accessible->get_accRole(Child(0), &result) == S_OK && result.lVal == ROLE_SYSTEM_LIST,
          "virtual list root role");
    owner->snapshot.type = pulse::extensions::UiNodeType::Tree;
    Check(accessible->get_accRole(Child(1), &result) == S_OK && result.lVal == ROLE_SYSTEM_OUTLINEITEM,
          "tree child role");
    Check(accessible->get_accState(Child(1), &result) == S_OK && (result.lVal & STATE_SYSTEM_EXPANDED) &&
              (result.lVal & STATE_SYSTEM_SELECTED) && (result.lVal & STATE_SYSTEM_FOCUSED),
          "tree expanded selection focus state");
    owner->snapshot.rows[0].expanded = false;
    Check(accessible->get_accState(Child(1), &result) == S_OK && (result.lVal & STATE_SYSTEM_COLLAPSED),
          "tree collapsed state");
    owner->snapshot.type = pulse::extensions::UiNodeType::DataTable;
    Check(accessible->get_accRole(Child(0), &result) == S_OK && result.lVal == ROLE_SYSTEM_TABLE,
          "table root role");
    Check(accessible->get_accRole(Child(1), &result) == S_OK && result.lVal == ROLE_SYSTEM_ROW,
          "table child role");
    Check(accessible->get_accState(Child(3), &result) == S_OK && (result.lVal & STATE_SYSTEM_OFFSCREEN),
          "offscreen row state");
    Check(accessible->get_accFocus(&result) == S_OK && result.lVal == 1, "focus returns simple child ID");
    Check(accessible->get_accSelection(&result) == S_OK && result.lVal == 1,
          "selection returns simple child ID");
    long left = 0, top = 0, width = 0, height = 0;
    Check(accessible->accLocation(&left, &top, &width, &height, Child(1)) == S_OK && left == 100 &&
              top == 200 && width == 400 && height == 32,
          "location uses screen coordinates");
    Check(accessible->accHitTest(101, 201, &result) == S_OK && result.lVal == 1, "screen hit selects row");
    Check(accessible->accHitTest(101, 350, &result) == S_OK && result.lVal == CHILDID_SELF,
          "blank viewport hit returns self");
    Check(accessible->accHitTest(10, 20, &result) == S_FALSE, "outside viewport is not a hit");
    Check(accessible->accDoDefaultAction(Child(2)) == E_ACCESSDENIED && owner->activated == 0,
          "disabled row never invokes host action");
    Check(accessible->accDoDefaultAction(Child(1)) == S_OK && owner->activated == 1,
          "enabled default action uses host callback");
    Check(accessible->accSelect(SELFLAG_TAKESELECTION, Child(2)) == E_ACCESSDENIED,
          "disabled row cannot be selected");
    Check(accessible->accSelect(SELFLAG_TAKESELECTION, Child(3)) == S_OK && owner->snapshot.rows[2].selected,
          "selection callback uses stable row identity");
    owner->disconnect_during_action = true;
    Check(accessible->accDoDefaultAction(Child(1)) == CO_E_OBJNOTCONNECTED,
          "reentrant owner disconnect rejects successful stale action result");
    owner->connected = true;
    Check(accessible->get_accChildCount(&count) == CO_E_OBJNOTCONNECTED,
          "disconnected provider cannot revive for reused owner generation");
    auto expired = Create(owner);
    owner.reset();
    Check(expired->get_accName(Child(0), &name) == CO_E_OBJNOTCONNECTED,
          "expired weak owner disconnects safely");
    accessible.Reset();
    expired.Reset();
    CoUninitialize();
    return failures ? 1 : 0;
}

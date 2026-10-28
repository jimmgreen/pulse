#include "../app/pane_transfer_target.h"
#include <cstdio>
namespace pulse::app { struct Pane {}; }
int main() {
    using namespace pulse::app;
    Pane a, b, c, hidden;
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; };
    check(ResolveTransferTarget({&a, &b}, &a, nullptr) == &b, "left to right without marker");
    check(ResolveTransferTarget({&a, &b}, &b, nullptr) == &a, "right to left without marker");
    check(ResolveTransferTarget({&a, &b}, &b, &b) == &a, "marked source uses other pane");
    check(ResolveTransferTarget({&a, &b}, &a, &hidden) == &b, "hidden target is ignored");
    check(!ResolveTransferTarget({&a}, &a, nullptr), "single pane has no target");
    check(!ResolveTransferTarget({&a, &b, &c}, &a, nullptr), "three panes require explicit target");
    check(ResolveTransferTarget({&a, &b, &c}, &a, &c) == &c, "explicit visible target respected");
    return failures ? 1 : 0;
}

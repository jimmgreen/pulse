#include "../shell_bridge/file_dialog_diagnostic.h"
#include <iostream>
using namespace pulse::shell_bridge;
int main() {
    int failed = 0;
    const auto check = [&](bool ok) { if (!ok) ++failed; };
    using K = SetterDiagnostic::Kind;
    auto d = ParseSetterDiagnostic("extension seq=12 available=1 units=3 redacted=0 truncated=0");
    check(d.kind == K::Extension && d.values[0] == 12 && d.values[2] == 3);
    d = ParseSetterDiagnostic("extension seq=12 offset=0 utf16=\\u0078\\u006D\\u0069\\u006E\\u0064");
    check(d.kind == K::Chunk && d.units == 5 && d.values[2] == 120 && d.values[6] == 100);
    d = ParseSetterDiagnostic("extension seq=1 offset=8 utf16=\\u0030\\u0031\\u0032\\u0033\\u0034\\u0035\\u0036\\u0037");
    check(d.kind == K::Chunk && d.units == 8 && d.values[9] == 55);
    check(ParseSetterDiagnostic("extension seq=1 available=1 units=20 redacted=1 truncated=0").kind == K::Extension);
    check(ParseSetterDiagnostic("filter seq=2 index=0 units=4 star=0 all=0 semicolon=1").kind == K::Filter);
    check(ParseSetterDiagnostic("filter-shape seq=2 index=0 extension_stars=1 questions=0 forbidden=0").kind == K::Shape);
    for (const auto* text : {"filename secret.txt", "extension seq=1 offset=0 utf16=\\u003A", "extension seq=1 offset=0 utf16=\\u002F", "extension seq=1 offset=0 utf16=\\u005C", "extension seq=1 offset=0 utf16=\\uGGGG", "extension seq=1 offset=1 utf16=\\u0061", "extension seq=1 offset=128 utf16=\\u0061", "extension seq=1 offset=0 utf16=", "extension seq=1 offset=0 utf16=\\u0061 secret", "extension seq=1 available=1 units=2 redacted=0 truncated=0 trailing", "filter seq=-1 index=0 units=4 star=0 all=0 semicolon=1"})
        check(ParseSetterDiagnostic(text).kind == K::Unknown);
    d = ParseSetterDiagnostic("prominent id=1047 kind=7 parent=0 nested=0 capture=1 feature=1 hr=0");
    check(d.kind == K::Prominent && d.values[0] == 1047 && d.values[1] == 7 && d.values[6] == 0);
    check(ParseSetterDiagnostic("prominent id=1 kind=7 parent=0 nested=2 capture=1 feature=1 hr=0").kind == K::Unknown);
    check(ParseSetterDiagnostic("prominent id=1 kind=7 parent=0 nested=0 capture=1 feature=1 hr=4294967296").kind == K::Unknown);
    check(ParseSetterDiagnostic("prominent id=1 kind=7 parent=0 nested=0 capture=1 feature=1 hr=0 secret").kind == K::Unknown);
    d = ParseSetterDiagnostic("text-snapshot setting=5 null=1 units=0 captured=1 hr=0x00000000");
    check(d.kind == K::Text && d.values[1] == 1 && d.values[3] == 1 && d.values[4] == 0);
    check(ParseSetterDiagnostic("text-snapshot setting=5 null=0 units=32769 captured=0 hr=0x80070057").values[4] == 0x80070057u);
    check(ParseSetterDiagnostic("text-snapshot setting=5 null=0 units=32770 captured=0 hr=0x0").kind == K::Unknown);
    check(ParseSetterDiagnostic("text-snapshot setting=5 null=0 units=0 captured=1 hr=0x0 document.pdf").kind == K::Unknown);
    std::cout << "diagnostic parser failures=" << failed << '\n';
    return failed ? 1 : 0;
}

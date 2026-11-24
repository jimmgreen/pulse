#include "../shell_bridge/file_dialog_parameter_shape.h"
#include <cstdio>

int main() {
    using namespace pulse::shell_bridge;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    check(FileDialogParameterShape(u"") == 0, "empty parameter has no shape bits");
    check(FileDialogParameterShape(u"yjk") == 0, "plain extension has no shape bits");
    check(FileDialogParameterShape(u".yjk") == ParameterLeadingDot, "leading-dot extension identified");
    check(FileDialogParameterShape(u"tar.gz") == ParameterInternalDot, "multi-part extension identified");
    check(FileDialogParameterShape(u"*.yjk") == (ParameterStarDot | ParameterWildcard | ParameterInternalDot),
        "star-dot default extension identified without logging text");
    check(FileDialogParameterShape(u"C:\\private\\project.yjk") ==
        (ParameterSeparator | ParameterColon | ParameterInternalDot), "absolute path shape is structural only");
    check((FileDialogParameterShape(u"\\\\host\\share\\file") & ParameterUnc) != 0, "UNC shape identified");
    check((FileDialogParameterShape(u" a;\tb?\".") &
        (ParameterWhitespace | ParameterControl | ParameterSemicolon | ParameterWildcard | ParameterQuoteOrReserved | ParameterTrailingDot)) ==
        (ParameterWhitespace | ParameterControl | ParameterSemicolon | ParameterWildcard | ParameterQuoteOrReserved | ParameterTrailingDot),
        "unsupported parameter categories retained independently");
    std::printf("PARAMETER_SHAPE failures=%d\n", failures);
    return failures ? 1 : 0;
}

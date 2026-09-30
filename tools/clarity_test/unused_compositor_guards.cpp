// The standalone target uses production CreateTextFormat, CreateRenderingParams
// and InkPad. Other functions in typography.cpp reference full-app Compositor
// methods. Fail loudly if this experiment ever enters those unrelated routes.
#include "../../src/ui/ui_compositor.h"
#include <stdexcept>
namespace pulse::ui {
bool Compositor::MeasureLumaText(std::wstring_view, IDWriteTextFormat*, float&, float*) {
    throw std::logic_error("Unexpected full-app Compositor::MeasureLumaText in isolated diagnostic");
}
bool Compositor::LumaTextEnabled() const noexcept {
    // The production declaration is noexcept, so terminate is the fail-closed guard.
    std::terminate();
}
}

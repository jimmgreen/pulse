#pragma once
#include <functional>
#include <string>

namespace pulse::ui {
// Calls the real surface renderer on its UI thread, captures before Present,
// and substitutes the opaque theme surface for external DWM/acrylic content.
bool CaptureDialogSurface(const std::wstring& path, const std::function<void()>& render);
}

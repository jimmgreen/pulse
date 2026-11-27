#pragma once
#include <windows.h>

namespace pulse::ui::test {
struct CaptionWrite {
    COLORREF value = 0;
    HRESULT result = E_FAIL;
    unsigned calls = 0;
};
void ClearCaptionWrites();
CaptionWrite LastCaptionWrite(HWND hwnd);
}

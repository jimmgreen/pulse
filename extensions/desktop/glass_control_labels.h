#pragma once
#include "../../src/common/localization.h"
namespace pulse::desktop {
inline constexpr l10n::StringId kGlassControlLabels[] = {
        l10n::StringId::DesktopGlassThickness, l10n::StringId::DesktopGlassIor,
        l10n::StringId::DesktopGlassDispersion, l10n::StringId::DesktopGlassBlur,
        l10n::StringId::DesktopGlassFresnelRange, l10n::StringId::DesktopGlassFresnelHardness,
        l10n::StringId::DesktopGlassFresnelFactor, l10n::StringId::DesktopGlassGlareRange,
        l10n::StringId::DesktopGlassGlareHardness, l10n::StringId::DesktopGlassGlareFactor,
        l10n::StringId::DesktopGlassGlareConvergence, l10n::StringId::DesktopGlassGlareOpposite,
        l10n::StringId::DesktopGlassGlareAngle, l10n::StringId::DesktopGlassTintR,
        l10n::StringId::DesktopGlassTintG, l10n::StringId::DesktopGlassTintB, l10n::StringId::DesktopGlassTintA};
inline constexpr const wchar_t* kGlassControlUnits[]={L"px",L"",L"",L"px",L"%",L"%",L"%",L"%",L"%",L"%",L"%",L"%",L"°",L"",L"",L"",L"%"};
inline constexpr const char* kGlassControlIds[]={"b-sl:thick","b-sl:ior","b-sl:disp","b-sl:frost","b-sl:frange","b-sl:fhard","b-sl:ffactor","b-sl:grange","b-sl:ghard","b-sl:glare","b-sl:gconv","b-sl:gopp","b-sl:angle","b-sl:red","b-sl:green","b-sl:blue","b-sl:alpha"};
}

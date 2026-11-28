// Project File Capsules (com.pulse.project-capsules):
// Highlights design, 3D, CAD, and video project source files with inline colored
// extension capsules via pulse.extcapsules, with zero thumbnail occlusion.
#include <pulse/app.hpp>
#include <pulse/kit.hpp>
#include "strings.gen.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace pulse::sdk;

namespace {

struct Palette {
    uint32_t light_bg, light_text, light_border;
    uint32_t dark_bg, dark_text, dark_border;
};

struct ExtSpec {
    const char* ext;
    Palette palette;
    bool optional_format;
};

constexpr Palette kPhotoshop   = {0xE6F4FF, 0x005EA6, 0xB3DCFF, 0x001E36, 0x31A8FF, 0x0B4A7D};
constexpr Palette kIllustrator = {0xFFF2E2, 0xB85000, 0xFFD5A3, 0x331700, 0xFF9A00, 0x6E3400};
constexpr Palette kInDesign    = {0xFFE8EE, 0xB8003A, 0xFFB8CB, 0x3B0619, 0xFF3366, 0x7A1236};
constexpr Palette kAfterFx     = {0xECEEFF, 0x3838A8, 0xC7CBFF, 0x12124D, 0x9999FF, 0x33338F};
constexpr Palette kPremiere    = {0xF4ECFF, 0x6D28D9, 0xDDC6FF, 0x241047, 0xC084FC, 0x522794};
constexpr Palette kXd          = {0xFCE8FA, 0xA81296, 0xF5B8EE, 0x3D0835, 0xFF61F6, 0x7D1870};
constexpr Palette kSubstance   = {0xEDF8E6, 0x3B7A14, 0xC5E8AE, 0x16290A, 0x8CE04E, 0x335E16};
constexpr Palette kLightroom   = {0xE6F7FF, 0x026E9E, 0xB0E4FF, 0x082436, 0x38BDF8, 0x0E5075};
constexpr Palette kPdf         = {0xFDECEC, 0xC62828, 0xF9BDBD, 0x3B1010, 0xFF5C5C, 0x7A2222};

constexpr Palette kBlender     = {0xFFF0E5, 0xC45500, 0xFFCFA8, 0x361804, 0xFF8A33, 0x70340A};
constexpr Palette kC4d         = {0xE8EEFF, 0x1D4ED8, 0xBDCEFF, 0x0B1A4A, 0x6094FF, 0x1D3E9E};
constexpr Palette k3dsMax      = {0xE4F7F7, 0x0E7474, 0xAEE3E3, 0x082B2B, 0x2DD4BF, 0x135E5E};
constexpr Palette kMaya        = {0xE6F7F5, 0x0F766E, 0xB2E5DF, 0x092B28, 0x34D399, 0x145E54};
constexpr Palette kRhino       = {0xF1F5F9, 0x334155, 0xCBD5E1, 0x1E2530, 0xCBD5E1, 0x475569};
constexpr Palette kSketchUp    = {0xFEECEB, 0xC41E1E, 0xFAC2BF, 0x380E0E, 0xF87171, 0x752020};
constexpr Palette kKeyShot     = {0xE6F6FC, 0x0077A3, 0xB3E2F5, 0x062433, 0x38BDF8, 0x0E506E};
constexpr Palette kZBrush      = {0xF7EFEA, 0x8A3B12, 0xE3C7B6, 0x2E170B, 0xF29966, 0x63331A};
constexpr Palette kCadNative   = {0xFDECEB, 0xB91C1C, 0xF8BDBD, 0x380D0D, 0xFB7185, 0x751D1D};
constexpr Palette kCadNeutral  = {0xEBF5F3, 0x115E59, 0xB8DFD9, 0x0C2926, 0x5EEAD4, 0x1B5952};
constexpr Palette kMesh3d      = {0xEEF2FF, 0x4338CA, 0xC7D2FE, 0x171B42, 0x818CF8, 0x353D8A};

constexpr Palette kFigma       = {0xF3ECFF, 0x7C3AED, 0xD8C2FF, 0x231145, 0xA78BFA, 0x4C2694};
constexpr Palette kSketch      = {0xFEF7E0, 0xA16207, 0xFDE28A, 0x332506, 0xFACC15, 0x6E500E};
constexpr Palette kCorelDraw   = {0xE8F8EE, 0x15803D, 0xB6EBC9, 0x0A2E16, 0x4ADE80, 0x166534};
constexpr Palette kAfPhoto     = {0xF3ECFF, 0x7E22CE, 0xDEC2FF, 0x240F3D, 0xC084FC, 0x51238A};
constexpr Palette kAfDesign    = {0xE8F1FF, 0x1D4ED8, 0xBDD6FF, 0x0D1F45, 0x60A5FA, 0x1E4494};
constexpr Palette kAfPub       = {0xFFF1E6, 0xC2410C, 0xFFD0B0, 0x361508, 0xFB923C, 0x732D10};
constexpr Palette kPainting    = {0xE8F4FA, 0x0369A1, 0xBAE0F2, 0x0B2333, 0x38BDF8, 0x164C6E};
constexpr Palette kProcreate   = {0xF1EEFF, 0x4F46E5, 0xD1C9FF, 0x191642, 0x818CF8, 0x37308F};
constexpr Palette kDaVinci     = {0xEBF3FF, 0x1E40AF, 0xC2D9FF, 0x0D1C3D, 0x60A5FA, 0x1E3D85};
constexpr Palette kSvg         = {0xFFF4E5, 0xB45309, 0xFDD6A3, 0x301E08, 0xFBBF24, 0x694212};

constexpr ExtSpec kCatalog[] = {
    // Adobe CC
    {"psd", kPhotoshop, false}, {"psb", kPhotoshop, false}, {"pdd", kPhotoshop, false},
    {"ai", kIllustrator, false}, {"ait", kIllustrator, false}, {"eps", kIllustrator, false},
    {"indd", kInDesign, false}, {"idml", kInDesign, false}, {"indt", kInDesign, false},
    {"aep", kAfterFx, false}, {"aet", kAfterFx, false},
    {"prproj", kPremiere, false}, {"prel", kPremiere, false}, {"mogrt", kPremiere, false},
    {"xd", kXd, false},
    {"spp", kSubstance, false}, {"sbs", kSubstance, false}, {"sbsar", kSubstance, false}, {"sbsprs", kSubstance, false},
    {"lrcat", kLightroom, false}, {"lrtemplate", kLightroom, false}, {"dng", kLightroom, false},

    // 3D & Industrial CAD
    {"blend", kBlender, false}, {"blend1", kBlender, false},
    {"c4d", kC4d, false},
    {"max", k3dsMax, false},
    {"ma", kMaya, false}, {"mb", kMaya, false},
    {"3dm", kRhino, false},
    {"skp", kSketchUp, false}, {"skb", kSketchUp, false},
    {"bip", kKeyShot, false}, {"ksp", kKeyShot, false},
    {"zpr", kZBrush, false}, {"ztl", kZBrush, false}, {"zbr", kZBrush, false},
    {"dwg", kCadNative, false}, {"dxf", kCadNative, false},
    {"sldprt", kCadNative, false}, {"sldasm", kCadNative, false}, {"slddrw", kCadNative, false},
    {"step", kCadNeutral, false}, {"stp", kCadNeutral, false},
    {"iges", kCadNeutral, false}, {"igs", kCadNeutral, false},
    {"sat", kCadNeutral, false}, {"x_t", kCadNeutral, false},
    {"fbx", kMesh3d, false}, {"obj", kMesh3d, false}, {"glb", kMesh3d, false},
    {"gltf", kMesh3d, false}, {"usdz", kMesh3d, false}, {"stl", kMesh3d, false}, {"3mf", kMesh3d, false},

    // UI, Vector, Illustration & Editing
    {"fig", kFigma, false},
    {"sketch", kSketch, false},
    {"cdr", kCorelDraw, false},
    {"afphoto", kAfPhoto, false},
    {"afdesign", kAfDesign, false},
    {"afpub", kAfPub, false},
    {"clip", kPainting, false}, {"lip", kPainting, false}, {"sai", kPainting, false}, {"sai2", kPainting, false},
    {"procreate", kProcreate, false},
    {"drp", kDaVinci, false}, {"dra", kDaVinci, false},

    // Optional semi-project formats (default OFF)
    {"pdf", kPdf, true},
    {"svg", kSvg, true},
};

}  // namespace

class ProjectCapsules : public App {
public:
    using App::App;

    LocalText Name() const override { return strings::PluginName(); }

    PulseStatus OnStart() override {
        // Apply default rules right away so capsules render without waiting for async settings load.
        ApplyRules();

        Settings().OnReady([this] {
            if (Active()) ApplyRules();
        });
        Settings().OnChanged([this] {
            if (Active()) ApplyRules();
        });

        // Settings > Extensions > Project File Capsules.
        // Fails softly when experimental settings UI is omitted (e.g. --omit-experimental-ui).
        (void)SettingsPage({
            strings::PluginName(),
            [this] { return SettingsTree(); },
            {
                {"enabled", "true"},
                {"include_optional", "false"},
                {"color_mode", "0"},
                {"label_style", "0"},
            },
            {},
        });
        return PULSE_OK;
    }

private:
    ModelTree SettingsTree() {
        using namespace kit;
        return ModelTree(Page("page", {
            Section("general", Text(strings::SectionGeneral()), {
                SwitchRow("enabled", Text(strings::EnableTitle()), Text(strings::EnableDesc()),
                          "enabled", "icon:palette"),
                SwitchRow("include_optional", Text(strings::OptionalTitle()), Text(strings::OptionalDesc()),
                          "include_optional", "icon:file"),
            }, Text(strings::CoverageDesc())),
            Section("appearance", Text(strings::SectionAppearance()), {
                ChoiceRow("color_mode", Text(strings::ColorModeTitle()), Text(strings::ColorModeDesc()),
                          "color_mode",
                          {Text(strings::ColorModeHighContrast()), Text(strings::ColorModeAdaptive())},
                          "icon:contrast"),
                ChoiceRow("label_style", Text(strings::LabelStyleTitle()), Text(strings::LabelStyleDesc()),
                          "label_style",
                          {Text(strings::LabelStyleDottedLower()), Text(strings::LabelStyleUpper())},
                          "icon:badge"),
            }),
        }));
    }

    void ApplyRules() {
        if (!ExtCapsules()) return;
        const bool enabled = Settings().GetBool("enabled", true);
        if (!enabled) {
            (void)ClearExtensionCapsules();
            return;
        }
        const bool include_optional = Settings().GetBool("include_optional", false);
        const bool adaptive_light = Settings().GetInt("color_mode", 0) == 1;
        const bool upper_no_dot = Settings().GetInt("label_style", 0) == 1;
        const uint32_t flags = upper_no_dot
            ? (PULSE_EXT_CAPSULE_FLAG_UPPERCASE | PULSE_EXT_CAPSULE_FLAG_STRIP_DOT)
            : 0u;

        std::vector<ExtCapsuleRule> rules;
        rules.reserve(sizeof(kCatalog) / sizeof(kCatalog[0]));
        for (const auto& item : kCatalog) {
            if (item.optional_format && !include_optional) continue;
            ExtCapsuleRule rule;
            rule.extension = item.ext;
            rule.flags = flags;
            if (adaptive_light) {
                rule.light_bg = item.palette.light_bg;
                rule.light_text = item.palette.light_text;
                rule.light_border = item.palette.light_border;
            } else {
                // High-contrast dark brand capsule in both light and dark themes.
                rule.light_bg = item.palette.dark_bg;
                rule.light_text = item.palette.dark_text;
                rule.light_border = item.palette.dark_border;
            }
            rule.dark_bg = item.palette.dark_bg;
            rule.dark_text = item.palette.dark_text;
            rule.dark_border = item.palette.dark_border;
            rules.push_back(std::move(rule));
        }

        ExtCapsuleStyle style;
        style.corner_radius_milli = 4500u;   // 4.5 DIP crisp pill radius
        style.padding_x_milli = 5500u;       // 5.5 DIP horizontal padding
        style.padding_y_milli = 1000u;       // 1.0 DIP vertical padding
        style.border_width_milli = 1000u;    // 1.0 DIP border
        style.gap_milli = 4500u;             // 4.5 DIP gap from stem
        style.font_size_delta_milli = -1000; // -1.0 DIP relative to filename
        style.font_weight = 600u;            // SemiBold
        (void)SetExtensionCapsules(rules, style);
    }
};

PULSE_APP(ProjectCapsules)

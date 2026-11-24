#pragma once
#include "ui_tree.h"
#include <charconv>
#include <cmath>
#include <string>
namespace pulse::extensions {
inline bool ContinuousSlider(const UiNode& node) { return node.type == UiNodeType::Slider && node.slider_kind == "continuous"; }
inline bool SliderNumber(std::string_view text, double& value) {
    const auto p = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && p.ec == std::errc{} && p.ptr == text.data() + text.size() && std::isfinite(value);
}
inline bool SliderRange(const UiNode& node, double& minimum, double& maximum) {
    return node.options.size() == 2 && SliderNumber(node.options[0], minimum) && SliderNumber(node.options[1], maximum) && minimum < maximum && minimum >= -1e9 && maximum <= 1e9;
}
inline bool SliderValue(const UiNode& node, std::string_view text, double& value) {
    double a=0,b=0; return SliderRange(node,a,b) && SliderNumber(text,value) && value>=a && value<=b;
}
inline std::string SliderText(double value) {
    char buffer[64]{}; const auto p=std::to_chars(buffer,buffer+sizeof(buffer),value,std::chars_format::general);
    return p.ec==std::errc{} ? std::string(buffer,p.ptr) : "0";
}
}

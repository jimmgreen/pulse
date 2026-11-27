#pragma once

// The save footer's format control (HTML 格式分段按钮): which of the program's
// file types show as segments, what each segment says, and which type a typed
// extension picks. String rules; display labels use the active UI localization.

#include "folder_picker_model.h"

#include <string>
#include <string_view>
#include <vector>

namespace pulse::ui {

// Inline segments for ordinary types; more than this many go behind 更多.
inline constexpr size_t kPickerFormatMaxInline = 5;

// A type that takes any name: "*", "*.*" or a pattern list containing one.
bool PickerFilterIsAny(const PickerFilter& filter);
// The program's label without its trailing "(*.png;…)".
std::wstring PickerFilterName(const PickerFilter& filter);
// The type's extensions in pattern order, lower case, without "*.": {"jpg", "jpeg"}.
std::vector<std::wstring> PickerFilterExtensions(const PickerFilter& filter);

// Concrete extension labels (TXT/PNG), preserving recognized format-family names
// such as Photoshop/JPEG 2000 and duplicate-type disambiguation. Wildcards use
// localized "Any format"; PickerFilterName retains the caller's original label.
std::vector<std::wstring> PickerFormatLabels(const std::vector<PickerFilter>& filters);

// Format capability, not a promise about the caller's export options. Unknown or
// heterogeneous pattern groups show only the caller name and literal extensions.
std::wstring PickerFormatNote(const PickerFilter& filter);
// Non-interactive replacement for the segmented control when only one type exists.
std::wstring PickerFormatStaticText(const PickerFilter& filter);

// The segments in display order: filter indices, kPickerFormatMore for the
// 更多 segment, any-name types last. Up to kPickerFormatMaxInline ordinary
// types inline; with more, the selected one and `recent` (most recent first)
// stay out, the rest keep program order behind 更多.
inline constexpr int kPickerFormatMore = -1;
std::vector<int> PickerFormatSegmentOrder(const std::vector<PickerFilter>& filters, size_t selected,
                                          const std::vector<size_t>& recent = {});

} // namespace pulse::ui

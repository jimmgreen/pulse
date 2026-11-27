#pragma once

#include "picker_save_draft_probe.h"
#include "../common/file_placeholder.h"
#include "drive_capacity.h"

namespace pulse::ui {
struct PickerSaveAdvisory {
    std::wstring warning, space;
};

inline PickerSaveAdvisory DescribePickerSaveTarget(const PickerSaveDraftProbe::Result& snapshot) {
    PickerSaveAdvisory result;
    if (snapshot.space_known) {
        result.space = l10n::Get(l10n::StringId::PickerSaveSpaceAvailable);
        capacity_detail::ReplaceToken(result.space, L"{free}", format::ByteSize(snapshot.free_bytes));
    }
    const auto append = [&](l10n::StringId id) {
        if (!result.warning.empty()) result.warning += L" · ";
        result.warning += l10n::Get(id);
    };
    // A directory's READONLY bit is customization metadata, not write access.
    if (snapshot.attributes != INVALID_FILE_ATTRIBUTES && !(snapshot.attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (snapshot.attributes & FILE_ATTRIBUTE_READONLY) append(l10n::StringId::PickerSaveReadOnlyTarget);
        if (files::PreviewWouldRecall(snapshot.attributes)) append(l10n::StringId::PickerSaveOnlineTarget);
    }
    if (snapshot.space_known && snapshot.free_bytes == 0) append(l10n::StringId::PickerSaveDiskFull);
    return result;
}
} // namespace pulse::ui

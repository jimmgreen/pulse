#pragma once
#include "preview_router.h"
#include <array>
#include <limits>
#include <string_view>

namespace pulse::preview {
inline uint64_t IntegrityNumber(std::wstring_view value) {
    uint64_t n = 0;
    if (value.empty()) return 0;
    for (const auto c : value) {
        if (c < L'0' || c > L'9' || n > ((std::numeric_limits<uint64_t>::max)() - (c - L'0')) / 10)
            return 0;
        n = n * 10 + (c - L'0');
    }
    return n;
}
inline std::array<std::wstring_view, 10> IntegrityFields(std::wstring_view line) {
    std::array<std::wstring_view, 10> fields{};
    for (auto& field : fields) {
        const auto end = line.find(L'\t');
        field = line.substr(0, end);
        if (end == std::wstring_view::npos) break;
        line.remove_prefix(end + 1);
    }
    return fields;
}
inline Integrity DescribeIntegrity(const DecodeResult& result, bool made) {
    Integrity status;
    status.state = made ? IntegrityState::Complete : IntegrityState::Failed;
    const std::wstring_view error = result.error;
    if (!made) {
        status.reason = IntegrityReason::ReadFailure;
        if (error.find(L"unsupported") != error.npos || error == L"udf-preview-unavailable" ||
            error == L"shortcut-to-program") {
            status.state = IntegrityState::Unsupported;
            status.reason = IntegrityReason::UnsupportedFeature;
        }
        return status;
    }
    if (result.decoder && (std::string_view(result.decoder) == "shell-preview" ||
        std::string_view(result.decoder) == "shell-thumbnail")) {
        status.state = IntegrityState::Unknown;
        status.reason = IntegrityReason::ProviderDefined;
    }
    if (result.truncated) {
        status.state = IntegrityState::Partial;
        status.reason = IntegrityReason::Limit;
    }
    if (result.kind == ipc::PreviewContentKind::Hex) {
        status.state = IntegrityState::Partial;
        status.reason = IntegrityReason::SourceFallback;
    }
    if (error == L"docx-reading" || error == L"office-reading") {
        status.state = IntegrityState::Partial;
        status.reason = IntegrityReason::ReadingConversion;
    } else if (!error.empty()) {
        status.state = IntegrityState::Partial;
        status.reason = error.starts_with(L"svg-") ? IntegrityReason::SourceFallback :
            error.find(L"limit") != error.npos ? IntegrityReason::Limit : IntegrityReason::ReadFailure;
    }
    const std::wstring_view payload = result.text;
    if (payload.starts_with(L"PULSEARC\t")) {
        const auto fields = IntegrityFields(payload.substr(0, payload.find(L'\n')));
        status.unit = IntegrityUnit::Entries;
        for (size_t at = payload.find(L'\n'); at != payload.npos;) {
            const size_t start = at + 1;
            at = payload.find(L'\n', start);
            const auto line = payload.substr(start, at == payload.npos ? at : at - start);
            if (!line.empty() && !line.starts_with(L"#")) ++status.loaded;
        }
        if (fields[4] != L"0") {
            status.state = IntegrityState::Partial;
            status.reason = fields[5] == L"scan-error" ? IntegrityReason::ReadFailure :
                fields[5] == L"display-limit" || fields[5] == L"scan-limit" ? IntegrityReason::Limit :
                IntegrityReason::IncompleteDirectory;
        } else status.total = status.loaded;
        if (fields[2] == L"DIR" && !fields[6].empty()) status.total = IntegrityNumber(fields[6]);
    } else if (payload.starts_with(L"PULSETBL\t")) {
        uint64_t selected = 0, sheet = 0;
        size_t at = payload.find(L"\nW\t");
        if (at != payload.npos) {
            const auto fields = IntegrityFields(payload.substr(at + 1, payload.find(L'\n', at + 1) - at - 1));
            status.unit = IntegrityUnit::Sheets;
            status.total = IntegrityNumber(fields[1]);
            status.loaded = IntegrityNumber(fields[2]);
            selected = IntegrityNumber(fields[3]);
            if (status.loaded < status.total) {
                status.state = IntegrityState::Partial;
                status.reason = IntegrityReason::OnDemand;
            }
        }
        for (size_t start = 0; start < payload.size();) {
            const auto end = payload.find(L'\n', start);
            const auto fields = IntegrityFields(payload.substr(start, end - start));
            if (fields[0] == L"S") {
                if (sheet == selected && fields[5] == L"1") {
                    status.state = IntegrityState::Partial;
                    status.reason = IntegrityReason::Limit;
                }
                if (sheet == selected && (fields[6] == L"read-failed" || fields[6] == L"missing-relationship")) {
                    status.state = IntegrityState::Partial;
                    status.reason = IntegrityReason::ReadFailure;
                }
                if (sheet == selected && fields[6] == L"hidden") {
                    status.state = IntegrityState::Partial;
                    status.reason = IntegrityReason::UnsupportedFeature;
                }
                ++sheet;
            }
            if (end == payload.npos) break;
            start = end + 1;
        }
    } else if (payload.find(L"\nV\tchapters\t") != payload.npos) {
        const auto at = payload.find(L"\nV\tchapters\t");
        const auto fields = IntegrityFields(payload.substr(at + 1, payload.find(L'\n', at + 1) - at - 1));
        status.unit = IntegrityUnit::Chapters;
        status.loaded = IntegrityNumber(fields[2]);
        status.total = IntegrityNumber(fields[3]);
        if (status.loaded < status.total) {
            status.state = IntegrityState::Partial;
            status.reason = IntegrityReason::ReadFailure;
        }
    } else if (payload.starts_with(L"PULSEIMAGE\t")) {
        const auto at = payload.find(L"\nF\t");
        if (at != payload.npos) {
            const auto fields = IntegrityFields(payload.substr(at + 1, payload.find(L'\n', at + 1) - at - 1));
            status.unit = IntegrityUnit::Frames;
            status.loaded = IntegrityNumber(fields[1]);
            status.total = IntegrityNumber(fields[2]);
        }
    }
    return status;
}
}

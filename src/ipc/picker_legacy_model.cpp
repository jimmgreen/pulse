#include "picker_legacy_model.h"
#include <algorithm>
#include <limits>

namespace pulse::ipc::picker::legacy {
namespace {
// Contract references: Microsoft OPENFILENAMEW, GetOpenFileNameW, and
// CommDlgExtendedError documentation. No ANSI or old-style output emulation.
constexpr uint32_t kFlags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT |
    OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_ENABLESIZING |
    OFN_OVERWRITEPROMPT | OFN_NOTESTFILECREATE;
bool Take(std::span<const char16_t> input, size_t& offset, std::u16string& text) {
    const auto start = offset;
    while (offset < input.size() && input[offset] != 0) {
        if (offset - start >= kMaxStringUnits) return false;
        ++offset;
    }
    if (offset == input.size()) return false;
    text.assign(input.begin() + start, input.begin() + offset);
    ++offset;
    return true;
}
bool SplitPath(const std::u16string& path, std::u16string& directory, std::u16string& leaf) {
    const auto slash = path.find_last_of(u'\\');
    if (slash == path.npos || slash + 1 == path.size() || path.find(u'/') != path.npos) return false;
    bool absolute = path.size() >= 3 && ((path[0] >= u'A' && path[0] <= u'Z') ||
        (path[0] >= u'a' && path[0] <= u'z')) && path[1] == u':' && path[2] == u'\\';
    if (path.starts_with(u"\\\\")) {
        if (path.starts_with(u"\\\\.\\")) return false;
        if (path.starts_with(u"\\\\?\\")) {
            if (path.starts_with(u"\\\\?\\UNC\\")) {
                const auto server_end = path.find(u'\\', 8);
                if (server_end != path.npos && server_end > 8) {
                    const auto share_end = path.find(u'\\', server_end + 1);
                    absolute = share_end != path.npos && share_end > server_end + 1 && share_end <= slash;
                }
            } else absolute = path.size() > 7 && path[5] == u':' && path[6] == u'\\' &&
                ((path[4] >= u'A' && path[4] <= u'Z') || (path[4] >= u'a' && path[4] <= u'z'));
        } else {
            const auto server_end = path.find(u'\\', 2);
            const auto share_end = server_end == path.npos ? path.npos : path.find(u'\\', server_end + 1);
            absolute = server_end != path.npos && server_end > 2 && share_end != path.npos && share_end > server_end + 1 && share_end <= slash;
        }
    }
    if (!absolute) return false;
    directory = path.substr(0, slash);
    if (slash == 2 || (path.starts_with(u"\\\\?\\") && slash == 6)) directory.push_back(u'\\');
    leaf = path.substr(slash + 1);
    if (leaf == u"." || leaf == u".." || leaf.find_first_of(u"<>:\"|?*") != leaf.npos) return false;
    return true;
}
void Append(std::vector<char16_t>& output, const std::u16string& text) {
    output.insert(output.end(), text.begin(), text.end());
    output.push_back(0);
}
}

Result Prepare(const Input& input, const RequestId& id, Prepared& output) {
    if (input.ansi || input.hook || input.custom_template || input.custom_filter || input.flags_ex ||
        (input.flags & ~kFlags) || !input.title.empty()) return Result::Unsupported;
    // Missing controls/side effects are not silently ignored. The first adapter
    // only accepts callers that explicitly suppress read-only UI and recent links.
    if (!(input.flags & OFN_HIDEREADONLY) || !(input.flags & OFN_DONTADDTORECENT) ||
        !(input.flags & OFN_NOCHANGEDIR)) return Result::Unsupported;
    const bool multi = (input.flags & OFN_ALLOWMULTISELECT) != 0;
    if (multi && (!(input.flags & OFN_EXPLORER) || input.save || input.file_title_present)) return Result::Unsupported;
    if (input.save) {
        if (!(input.flags & OFN_OVERWRITEPROMPT) || !(input.flags & OFN_PATHMUSTEXIST) ||
            !(input.flags & OFN_NOTESTFILECREATE) || (input.flags & OFN_FILEMUSTEXIST)) return Result::Unsupported;
    } else if (!(input.flags & OFN_FILEMUSTEXIST) || (input.flags & (OFN_OVERWRITEPROMPT | OFN_NOTESTFILECREATE)) ||
               !input.default_extension.empty()) return Result::Unsupported;
    // Native legacy defaults append at most three characters. Longer extensions
    // and punctuation require fallback instead of silently applying modern rules.
    if (input.default_extension.size() > 3 || input.default_extension.find_first_of(u".\\/:*?\"<>| ") != input.default_extension.npos)
        return Result::Unsupported;
    for (const auto c : input.default_extension)
        if (!((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9'))) return Result::Unsupported;
    Prepared prepared;
    prepared.flags = input.flags;
    prepared.file_title_present = input.file_title_present;
    prepared.default_extension = input.default_extension;
    // NOTESTFILECREATE can allow callers to handle write/read-only restrictions
    // themselves. The current Save host rejects read-only targets, so this is
    // an explicit unresolved compatibility gate, not implicit native parity.
    if (input.save) prepared.required_abi_checks |= SaveReadOnlyCompatibility;
    prepared.request.request_id = id;
    auto& body = std::get<Request>(prepared.request.body);
    body.operation = input.save ? Operation::SaveFile : Operation::OpenFiles;
    body.required_capabilities = input.save ? Save : Open;
    body.options = input.save ? OverwritePrompt : 0;
    if (multi) { body.required_capabilities |= MultiSelect; body.options |= AllowMultiSelect; }
    body.initial_directory = input.initial_directory;
    body.default_extension = input.default_extension;
    size_t offset = 0;
    if (!Take(input.initial_filename, offset, body.filename)) return Result::InvalidInput;
    if (input.save && (body.filename.find_first_of(u"\\/:") != body.filename.npos ||
        body.filename == u"." || body.filename == u"..")) return Result::Unsupported;
    // No-filter legacy calls return shortcut files rather than targets. Current
    // picker resolves shortcuts, so those calls need native fallback.
    if (input.filter_multisz.empty()) return Result::Unsupported;
    offset = 0;
    for (;;) {
        std::u16string label, pattern;
        if (!Take(input.filter_multisz, offset, label)) return Result::InvalidInput;
        if (label.empty()) break;
        if (!Take(input.filter_multisz, offset, pattern) || pattern.empty() || body.filters.size() == kMaxFilters) return Result::InvalidInput;
        body.filters.push_back({std::move(label), std::move(pattern)});
    }
    if (body.filters.empty()) return Result::Unsupported;
    body.required_capabilities |= Filters;
    body.filter_index = input.filter_index ? input.filter_index - 1 : 0;
    std::vector<uint8_t> encoded;
    if (Encode(prepared.request, encoded) != CodecError::None) return Result::InvalidInput;
    output = std::move(prepared);
    return Result::Ready;
}

Completion Complete(const Prepared& prepared, const Message& response, Output& output) {
    if (!ValidateResponse(prepared.request, response)) return {Result::Failed, FNERR_INVALIDFILENAME};
    const auto& body = std::get<Response>(response.body);
    if (body.status == Status::Cancelled) return {Result::Cancelled};
    if (body.status == Status::Unsupported) return {Result::Unsupported};
    if (body.status != Status::Accepted) return {Result::Failed, CDERR_DIALOGFAILURE};
    std::vector<char16_t> file;
    std::u16string first_directory, first_leaf;
    if (!SplitPath(body.paths.front(), first_directory, first_leaf)) return {Result::Failed, FNERR_INVALIDFILENAME};
    size_t file_offset = 0, extension_offset = 0;
    if (body.paths.size() == 1) {
        Append(file, body.paths.front());
        file_offset = body.paths.front().size() - first_leaf.size();
        const auto dot = first_leaf.find_last_of(u'.');
        if (dot != first_leaf.npos) extension_offset = file_offset + dot + 1;
    } else {
        if (prepared.file_title_present) return {Result::Failed, CDERR_INITIALIZATION};
        Append(file, first_directory);
        file_offset = file.size();
        for (const auto& path : body.paths) {
            std::u16string directory, leaf;
            // Exact lexical directory identity avoids merging case-sensitive or
            // alias paths without filesystem knowledge in this pure model.
            if (!SplitPath(path, directory, leaf) || directory != first_directory) return {Result::Failed, FNERR_INVALIDFILENAME};
            Append(file, leaf);
        }
        file.push_back(0);
    }
    constexpr auto word_max = (std::numeric_limits<uint16_t>::max)();
    if (file_offset > word_max || extension_offset > word_max) return {Result::Failed, FNERR_INVALIDFILENAME};
    // Check all destinations before the one permitted shortage mutation.
    if (prepared.file_title_present && output.file_title.size() < first_leaf.size() + 1)
        return {Result::Failed, CDERR_INITIALIZATION, file.size()};
    if (file.size() > output.file.size()) {
        if (file.size() > word_max || output.file.empty()) return {Result::Failed, FNERR_BUFFERTOOSMALL, file.size()};
        output.file[0] = static_cast<char16_t>(file.size());
        return {Result::BufferTooSmall, FNERR_BUFFERTOOSMALL, file.size()};
    }
    uint32_t flags = prepared.flags & ~OFN_EXTENSIONDIFFERENT;
    if (!prepared.default_extension.empty() && body.paths.size() == 1) {
        const auto actual = extension_offset ? body.paths.front().substr(extension_offset) : std::u16string();
        const auto& expected = prepared.default_extension;
        const bool equal = actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin(), [](char16_t a, char16_t b) {
            const auto lower = [](char16_t c) { return c >= u'A' && c <= u'Z' ? static_cast<char16_t>(c + (u'a' - u'A')) : c; };
            return lower(a) == lower(b);
        });
        if (!equal) flags |= OFN_EXTENSIONDIFFERENT;
    }
    std::copy(file.begin(), file.end(), output.file.begin());
    if (prepared.file_title_present) {
        std::copy(first_leaf.begin(), first_leaf.end(), output.file_title.begin());
        output.file_title[first_leaf.size()] = 0;
    }
    output.file_offset = static_cast<uint16_t>(file_offset);
    output.extension_offset = static_cast<uint16_t>(extension_offset);
    output.filter_index = body.filter_index + 1;
    output.flags = flags;
    return {Result::Accepted, 0, file.size()};
}
}

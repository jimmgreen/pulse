#pragma once

#include "picker_protocol.h"
#include <windows.h>
#include <commdlg.h>
#include <cderr.h>

namespace pulse::ipc::picker::legacy {
// The future ABI layer must first copy/validate caller memory into these bounded
// views. This model never dereferences OPENFILENAME pointers or installs hooks.
struct Input {
    bool ansi = false;
    bool save = false;
    bool hook = false;
    bool custom_template = false;
    bool custom_filter = false;
    bool file_title_present = false;
    uint32_t flags = 0;
    uint32_t flags_ex = 0;
    uint32_t filter_index = 1; // Native one-based index, zero defaults to first.
    std::span<const char16_t> filter_multisz;
    std::span<const char16_t> initial_filename; // Bounded lpstrFile including NUL.
    std::u16string initial_directory;
    std::u16string default_extension;
    std::u16string title;
};
enum class Result { Ready, Unsupported, InvalidInput, Accepted, Cancelled, Failed, BufferTooSmall };
enum AbiCheck : uint32_t {
    CallerDirectoryPolicy = 1,
    LegacyFileValidation = 2,
    SaveReadOnlyCompatibility = 4,
};
struct Prepared {
    Message request;
    uint32_t flags = 0;
    bool file_title_present = false;
    std::u16string default_extension;
    // Ready only means this pure request/return codec can represent the call.
    // No ABI adapter currently discharges these obligations. A future adapter
    // must establish matching native semantics before dispatch, otherwise use
    // the native dialog. NOCHANGEDIR alone does not prove Open CWD parity.
    uint32_t required_abi_checks = CallerDirectoryPolicy | LegacyFileValidation;
};
Result Prepare(const Input& input, const RequestId& id, Prepared& output);
struct Output {
    std::span<char16_t> file;
    std::span<char16_t> file_title;
    uint16_t file_offset = 0;
    uint16_t extension_offset = 0;
    uint32_t filter_index = 0;
    uint32_t flags = 0;
};
struct Completion {
    Result result = Result::Failed;
    uint32_t extended_error = 0; // CommDlgExtendedError value; zero on cancellation.
    size_t required_file_units = 0; // Includes all terminating NULs.
};
// No output mutation on cancellation/failure. On file-buffer shortage only its
// first UTF-16 unit changes (native first-WORD required-size convention).
// Counts beyond WORD and title-buffer shortage fail without truncating output.
// An Accepted wire result that cannot be committed is Failed, never a replayable
// Unsupported result. No completion failure authorizes reopening a native UI.
Completion Complete(const Prepared& prepared, const Message& response, Output& output);
}

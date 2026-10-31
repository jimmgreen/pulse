#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace pulse::ipc::picker {

constexpr uint16_t kVersion = 1;
constexpr uint32_t kMaxMessageBytes = 1024 * 1024;
constexpr uint32_t kMaxStringUnits = 32768;
constexpr uint32_t kMaxFilters = 64;
constexpr uint32_t kMaxPaths = 1024;
enum class Operation : uint32_t { OpenFiles = 1, PickFolder, SaveFile };
// NoDereferenceLinks requires PreserveLinks: older peers reject the unknown
// required capability instead of silently returning a shortcut target.
// AdvisoryFileFilters is OpenFiles-only and requires AdvisoryFilters plus nonempty
// Filters. Unknown required capabilities fail closed on older peers. Absent option
// retains strict acceptance; this does not change which rows the filters display.
// SkipAccessValidation requires ExplicitAccessPolicy and only applies to OpenFiles.
// It does not waive existence/type validation or certify default access parity.
// LocalOpenAccessV1 is a versioned, restricted ordinary-local-file access policy,
// not native validation parity. Its matching option is mandatory; old peers reject it.
enum Capability : uint32_t { Open = 1, Folder = 2, Save = 4, MultiSelect = 8, Filters = 16, PreserveLinks = 32, AdvisoryFilters = 64, ExplicitAccessPolicy = 128, LocalOpenAccessV1 = 256 };
enum Option : uint32_t { AllowMultiSelect = 1, ShowHidden = 2, OverwritePrompt = 4, NoDereferenceLinks = 8, AdvisoryFileFilters = 16, SkipAccessValidation = 32, ValidateLocalOpenAccessV1 = 64 };
enum class Status : uint32_t { Accepted = 1, Cancelled, Unsupported, Failed };
enum class ErrorSource : uint32_t { None, Protocol, Win32, HResult };
enum class CodecError { None, Malformed, UnsupportedVersion, UnsupportedCapability, InvalidSemantics };
using RequestId = std::array<uint8_t, 16>;
struct Filter {
    std::u16string label;
    std::u16string pattern;
    bool operator==(const Filter&) const = default;
};
struct Request {
    Operation operation = Operation::OpenFiles;
    uint32_t required_capabilities = Open;
    uint32_t options = 0;
    std::u16string initial_directory;
    std::u16string filename;
    std::u16string default_extension;
    std::vector<Filter> filters;
    uint32_t filter_index = 0; // Zero-based; zero when filters is empty.
    bool operator==(const Request&) const = default;
};
struct Response {
    Operation operation = Operation::OpenFiles;
    Status status = Status::Cancelled;
    ErrorSource error_source = ErrorSource::None;
    uint32_t error_code = 0;
    uint32_t filter_index = 0;
    std::vector<std::u16string> paths;
    bool operator==(const Response&) const = default;
};
struct Cancel { bool operator==(const Cancel&) const = default; };
struct Message {
    RequestId request_id{};
    std::variant<Request, Response, Cancel> body;
    bool operator==(const Message&) const = default;
};
// Output is unchanged on failure. Decode validates wire semantics; callers must
// additionally bind a response to its original request with ValidateResponse.
CodecError Encode(const Message& message, std::vector<uint8_t>& output);
CodecError Decode(std::span<const uint8_t> bytes, Message& output);
bool ValidateResponse(const Message& request, const Message& response);

} // namespace pulse::ipc::picker

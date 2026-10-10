// Pulse.Preview.exe: serves preview requests from Pulse over a named pipe.
// Decoding lives in preview_decoders.cpp, details-pane properties in
// preview_properties.cpp; this file only runs the request loop.

#include "../ipc/preview_protocol.h"
#include "../common/current_user_security.h"
#include "../common/crash_reporter.h"
#include "preview_decoders.h"
#include "gif_decoder.h"
#include "folder_thumbnail.h"
#include "preview_integrity.h"
#include "../common/runtime_log.h"
#include "preview_file_utils.h"
#include "preview_properties.h"
#include <windows.h>
#include <objbase.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

using namespace pulse;
using preview::ExtensionOf;
using preview::IsOfflinePlaceholder;
using preview::PreviewPropertyValue;
using preview::ReadProperties;
using preview::ReadMediaDurationMs;

namespace {

bool WriteString(HANDLE pipe, const std::wstring& value) {
    const uint32_t chars = static_cast<uint32_t>(value.size());
    return ipc::WriteAll(pipe, &chars, sizeof(chars)) &&
        (chars == 0 || ipc::WriteAll(pipe, value.data(), chars * sizeof(wchar_t)));
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
#ifdef PULSE_PREVIEW_MAPPING_TEST
    const std::wstring test_log_root = __argc > 3 ? __wargv[3] : L"";
    crash::Initialize({crash::ProcessRole::Preview, false, test_log_root});
#else
    crash::Initialize({crash::ProcessRole::Preview, false, {}});
#endif
    struct DiagnosticsShutdown { ~DiagnosticsShutdown() { crash::Shutdown(); } } diagnostics_shutdown;
    if (__argc < 2) return 2;
    const DWORD owner = wcstoul(__wargv[1], nullptr, 10);
    const std::wstring pipeName = ipc::PreviewPipeName(owner);
    CurrentUserSecurityAttributes pipeSecurity;
    if (!pipeSecurity) return 3;
    HANDLE pipe = CreateNamedPipeW(pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64*1024, 64*1024, 0, pipeSecurity.get());
    if (pipe == INVALID_HANDLE_VALUE) return 3;
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError()!=ERROR_PIPE_CONNECTED) { CloseHandle(pipe); return 4; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        ipc::PreviewRequest req;
        if (!ipc::ReadAll(pipe, &req, sizeof(req)) || req.magic!=ipc::kPreviewMagic ||
            req.path_chars==0 || req.path_chars>32768) break;
        std::wstring path(req.path_chars, L'\0');
        if (!ipc::ReadAll(pipe, path.data(), req.path_chars*sizeof(wchar_t))) break;
        const auto request_started = GetTickCount64();
        const auto terminal = [&](const char* event, uint32_t stage, DWORD error,
                                  diagnostics::runtime::Level level) {
            diagnostics::runtime::Event(event, {{"request", req.request_id},
                {"generation", req.generation}, {"elapsed_ms", GetTickCount64() - request_started},
                {"stage", stage}, {"error", error}, {"flags", req.flags}}, level);
        };
        std::vector<uint8_t> pixels;
        std::wstring previewText;
        std::wstring errorText;
        std::vector<PreviewPropertyValue> properties;
        UINT w=0,h=0,stride=0;
        UINT source_w=0, source_h=0;
        ipc::PreviewResponse response{};
        response.request_id=req.request_id;
        response.generation=req.generation;

        bool made = false;
        bool document_image_ready = true;
        if (req.flags & ipc::kPreviewRequestFlagDocumentImage) {
            WIN32_FILE_ATTRIBUTE_DATA metadata{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &metadata)) {
                document_image_ready = false;
                errorText = L"path-unavailable";
            } else {
                req.attrs = metadata.dwFileAttributes;
                document_image_ready = !(req.attrs & FILE_ATTRIBUTE_DIRECTORY) && !IsOfflinePlaceholder(req.attrs);
                if (!document_image_ready) errorText = IsOfflinePlaceholder(req.attrs) ?
                    L"offline-placeholder" : L"document-image-directory";
            }
        }
        if (!document_image_ready) {
            response.kind = ipc::PreviewContentKind::Unsupported;
            response.status = 1;
            response.error_chars = static_cast<uint32_t>(errorText.size());
            response.integrity.state = preview::IntegrityState::Failed;
            response.integrity.reason = preview::IntegrityReason::Unavailable;
        } else if (req.kind == ipc::PreviewRequestKind::Properties) {
            if (!IsOfflinePlaceholder(req.attrs)) properties = ReadProperties(path);
            response.property_count = static_cast<uint32_t>(properties.size());
            response.status = 0;
            made = true;
        } else if ((req.flags & ipc::kPreviewRequestFlagFolderThumbnail) != 0) {
            SetLastError(ERROR_SUCCESS);
            if ((req.attrs & FILE_ATTRIBUTE_DIRECTORY) && !IsOfflinePlaceholder(req.attrs))
                made = preview::DecodeFolderThumbnail(path, std::clamp(req.pixel_size, 32u, 512u),
                    pixels, w, h, stride,
                    (req.flags & ipc::kPreviewRequestFlagFolderSingle) != 0,
                    (req.flags & ipc::kPreviewRequestFlagFolderRefresh) != 0);
            response.kind = made ? ipc::PreviewContentKind::Bitmap : ipc::PreviewContentKind::Unsupported;
            response.status = made ? 0 : GetLastError() == ERROR_TIMEOUT ? ERROR_TIMEOUT : 1;
        } else {
            const std::wstring extension = ExtensionOf(path);
            const UINT cap = ipc::ClampPreviewPixelSize(req.pixel_size, false);
            const preview::DecodeRequest decode{req, path, extension,
                (req.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0, IsOfflinePlaceholder(req.attrs),
                cap, (req.flags & ipc::kPreviewRequestFlagGrid) != 0 ||
                     cap <= preview::kGridThumbnailEdge};
            preview::DecodeResult result;
            const ULONGLONG decode_started = GetTickCount64();
            made = preview::DecodeContent(decode, result);
            if (result.error == L"pdf-thumbnail-budget")
                diagnostics::runtime::Event("pdf_thumbnail_budget", {
                    {"elapsed_ms", GetTickCount64() - decode_started},
                    {"request", req.request_id}, {"generation", req.generation}, {"pixels", cap}},
                    diagnostics::runtime::Level::Warning);
            response.integrity = preview::DescribeIntegrity(result, made);
            if (!decode.grid) diagnostics::runtime::Event("preview_decode_integrity", {
                {"request", req.request_id}, {"generation", req.generation},
                {"state", static_cast<uint32_t>(response.integrity.state)},
                {"reason", static_cast<uint32_t>(response.integrity.reason)},
                {"unit", static_cast<uint32_t>(response.integrity.unit)},
                {"loaded", response.integrity.loaded}, {"total", response.integrity.total},
                {"kind", static_cast<uint32_t>(result.kind)}, {"index", req.frame_index}},
                made ? diagnostics::runtime::Level::Info : diagnostics::runtime::Level::Error);
            pixels = std::move(result.pixels);
            w = result.width; h = result.height; stride = result.stride;
            source_w = result.source_width; source_h = result.source_height;
            previewText = std::move(result.text);
            errorText = std::move(result.error);
            response.kind = result.kind;
            response.frame_count = result.frame_count;
            const uint32_t decoded_duration_ms = result.duration_ms;
            response.frame_delay_ms = result.frame_delay_ms;
            response.loop_count = result.loop_count;
            response.flags |= (static_cast<uint32_t>(result.text_encoding) <<
                               ipc::kPreviewFlagEncodingShift) & ipc::kPreviewFlagEncodingMask;
            const bool truncated = result.truncated;
            const uint32_t bytesRead = result.bytes_read;
            response.status = made ? 0 : 1;
            if (!made) {
                response.kind = ipc::PreviewContentKind::Unsupported;
                if (IsOfflinePlaceholder(req.attrs)) errorText = L"offline-placeholder";
                else if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                    errorText = L"path-unavailable";
            }
            response.text_chars = static_cast<uint32_t>(previewText.size());
            response.error_chars = static_cast<uint32_t>(errorText.size());
            response.bytes_read = bytesRead;
            if (truncated) response.flags |= ipc::kPreviewFlagTruncated;
            // Grid thumbnails mark videos with their playing time.
            if (made && response.kind == ipc::PreviewContentKind::Bitmap &&
                (req.flags & ipc::kPreviewRequestFlagGrid) != 0 && !IsOfflinePlaceholder(req.attrs))
                response.duration_ms = decoded_duration_ms ? decoded_duration_ms : ReadMediaDurationMs(path);
        }
        response.width=w; response.height=h; response.stride=stride;
        response.source_width = source_w;
        response.source_height = source_h;
        HANDLE mapping=nullptr; void* view=nullptr; std::wstring mappingName;
        if (made && response.kind == ipc::PreviewContentKind::Bitmap) {
            mappingName=L"Local\\PulsePreviewMap-"+std::to_wstring(GetCurrentProcessId())+L"-"+
                        std::to_wstring(req.request_id)+L"-"+std::to_wstring(GetTickCount64());
#ifdef PULSE_PREVIEW_MAPPING_TEST
            const bool fail_create = req.request_id == 1 && __argc > 2 && wcscmp(__wargv[2], L"--test-map-create-failure") == 0;
            const bool fail_view = req.request_id == 1 && __argc > 2 && wcscmp(__wargv[2], L"--test-map-view-failure") == 0;
#else
            constexpr bool fail_create = false, fail_view = false;
#endif
            if (fail_create) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            if (!fail_create) mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,
                static_cast<DWORD>(pixels.size()),mappingName.c_str());
            if (mapping && fail_view) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            if (mapping && !fail_view) view=MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,pixels.size());
            if (!view) {
                const DWORD error = GetLastError();
                terminal("preview_host_mapping_failure", mapping ? 2 : 1, error,
                    diagnostics::runtime::Level::Error);
                response.status=2; mappingName.clear();
                if (mapping) { CloseHandle(mapping); mapping = nullptr; }
                response.integrity.state = preview::IntegrityState::Failed;
                response.integrity.reason = preview::IntegrityReason::Unavailable;
            }
            else memcpy(view,pixels.data(),pixels.size());
        }
        response.mapping_chars=static_cast<uint32_t>(mappingName.size());
        bool ok=ipc::WriteAll(pipe,&response,sizeof(response));
        if (ok && !mappingName.empty()) ok=ipc::WriteAll(pipe,mappingName.data(),
            response.mapping_chars*sizeof(wchar_t));
        if (ok && !previewText.empty())
            ok=ipc::WriteAll(pipe,previewText.data(),response.text_chars*sizeof(wchar_t));
        if (ok && !errorText.empty())
            ok=ipc::WriteAll(pipe,errorText.data(),response.error_chars*sizeof(wchar_t));
        for (const auto& property : properties) {
            if (!ok) break;
            ok = WriteString(pipe, property.label) && WriteString(pipe, property.value);
        }
        DWORD response_error = ok ? ERROR_SUCCESS : GetLastError();
        uint32_t response_stage = 3; // Response bytes written; ACK is a separate terminal stage.
        if (ok && response.mapping_chars != 0) {
            unsigned char ack=0; ok=ipc::ReadAll(pipe,&ack,1);
            response_stage = 4;
            if (!ok) response_error = GetLastError();
        }
        if (!ok) terminal("preview_host_response_failure", response_stage, response_error,
            diagnostics::runtime::Level::Error);
        else if (!(req.flags & ipc::kPreviewRequestFlagGrid) || response.status == 2)
            terminal("preview_host_response", response_stage, static_cast<DWORD>(response.status),
                response.status ? diagnostics::runtime::Level::Error : diagnostics::runtime::Level::Info);
        if (view) UnmapViewOfFile(view); if (mapping) CloseHandle(mapping);
        if (!ok) break;
    }
    preview::ResetGifDecodeCache();
    CoUninitialize(); DisconnectNamedPipe(pipe); CloseHandle(pipe); return 0;
}

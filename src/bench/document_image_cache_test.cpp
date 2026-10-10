#include "../ui/thumbnail_cache.h"
#include "preview_host_client.h"
#include "../common/runtime_log.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>
namespace {
int failures = 0;
void Check(bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); std::fflush(stdout); failures += !ok; }
std::wstring Pipe(const std::wstring& id) { return L"\\\\.\\pipe\\PulseDocumentImageTest-" + id; }
std::wstring Event(const std::wstring& id) { return L"Local\\PulseDocumentImageTest-" + id; }
int SlowProvider(const std::wstring& id) {
    HANDLE entered = OpenEventW(EVENT_MODIFY_STATE, FALSE, Event(id).c_str());
    if (!entered) return 2;
    HANDLE pipe = CreateNamedPipeW(Pipe(id).c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { CloseHandle(entered); return 2; }
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(pipe); CloseHandle(entered); return 2; }
    pulse::ipc::PreviewRequest req{};
    bool ok = pulse::ipc::ReadAll(pipe, &req, sizeof(req)) && req.path_chars < 32768 &&
        (req.flags & pulse::ipc::kPreviewRequestFlagDocumentImage);
    std::wstring path(ok ? req.path_chars : 0, L'\0');
    ok = ok && pulse::ipc::ReadAll(pipe, path.data(), static_cast<DWORD>(path.size() * sizeof(wchar_t)));
    if (ok) { SetEvent(entered); if (id.ends_with(L"-exit")) ExitProcess(77); Sleep(INFINITE); }
    CloseHandle(pipe); CloseHandle(entered); return 2;
}
}
namespace pulse::ui {
struct ThumbnailCacheTestAccess {
    static int Run() {
        ComPtr<ID3D11Device> d3d;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        ComPtr<IDXGIDevice> dxgi; ComPtr<ID2D1Device> device; ComPtr<ID2D1DeviceContext> dc;
        if (SUCCEEDED(hr)) hr = d3d->QueryInterface(IID_PPV_ARGS(&dxgi));
        if (SUCCEEDED(hr)) hr = D2D1CreateDevice(dxgi.get(), nullptr, &device);
        if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        if (FAILED(hr)) { Check(false, "WARP context created"); return 1; }
        auto draw = [&](ThumbnailCache& cache, const std::shared_ptr<DocumentImageSession>& session, const std::wstring& path) {
            return cache.Draw(dc.get(), D2D1::RectF(0, 0, 100, 100), path, 0, 128, 1, 0, 0,
                1.0f, nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, nullptr, nullptr,
                0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                false, nullptr, nullptr, nullptr, session);
        };
        {
            ThumbnailCache cache; cache.running_ = true;
            auto old = std::make_shared<DocumentImageSession>();
            Check(draw(cache, old, L"C:\\synthetic-image.png") == PreviewDrawResult::Pending && cache.queue_.size() == 1,
                "document image enqueues without synchronous metadata");
            auto request = cache.queue_.front(); cache.queue_.clear();
            Check((request.flags & ipc::kPreviewRequestFlagDocumentImage) && request.document == old,
                "host metadata flag and document lifetime travel with request");
            draw(cache, old, request.path);
            Check(cache.queue_.empty(), "pending image requests coalesce");
            old->active = false; auto current = std::make_shared<DocumentImageSession>();
            draw(cache, current, request.path); auto replacement = cache.queue_.front(); cache.queue_.clear();
            ThumbnailCache::Item stale; stale.failed = true;
            Check(!cache.StoreResult(request, std::move(stale)) && cache.items_.empty() && cache.pending_.contains(replacement.key),
                "late old-document response cannot overwrite or remove replacement");
            ThumbnailCache::Item missing; missing.failed = true;
            Check(cache.StoreResult(replacement, std::move(missing)), "current missing image caches authoritative failure");
            Check(draw(cache, current, request.path) == PreviewDrawResult::Failed && cache.queue_.empty(),
                "failed image does not requery on every paint");
            for (int i = 0; i < 150; ++i) draw(cache, current, L"C:\\synthetic-" + std::to_wstring(i) + L".png");
            Check(cache.queue_.size() == 128 && cache.pending_.size() == 128, "image work queue remains bounded");
            current->active = false; auto next = std::make_shared<DocumentImageSession>(); draw(cache, next, L"C:\\next.png");
            Check(cache.queue_.size() == 1 && cache.pending_.size() == 1, "new document discards obsolete queued work");
        }
        for (int scenario = 0; scenario < 4; ++scenario) {
            const bool close = scenario == 1;
            ThumbnailCache cache;
            const auto id = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + (scenario == 0 ? L"-switch" : scenario == 1 ? L"-close" : scenario == 2 ? L"-timeout" : L"-exit");
            HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, Event(id).c_str());
            wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
            std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --slow-provider " + id;
            STARTUPINFOW si{sizeof(si)};
            bool started = entered && CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &cache.child_);
            if (started) {
                const auto deadline = GetTickCount64() + 3000;
                do { cache.pipe_ = CreateFileW(Pipe(id).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                    if (cache.pipe_ != INVALID_HANDLE_VALUE) break; Sleep(10);
                } while (GetTickCount64() < deadline);
            }
            ULONG server = 0;
            started = started && cache.pipe_ != INVALID_HANDLE_VALUE && GetNamedPipeServerProcessId(cache.pipe_, &server) && server == cache.child_.dwProcessId;
            Check(started, "private slow provider connected and process identity verified");
            if (!started) { cache.Reset(); if (entered) CloseHandle(entered); continue; }
            HANDLE process = nullptr;
            Check(DuplicateHandle(GetCurrentProcess(), cache.child_.hProcess, GetCurrentProcess(), &process, SYNCHRONIZE, FALSE, 0), "private provider wait handle retained");
            auto session = std::make_shared<DocumentImageSession>();
            const auto begin = GetTickCount64(); draw(cache, session, L"C:\\synthetic-slow.png");
            Check(GetTickCount64() - begin < 1000 && WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0,
                "paint returns while real cache worker waits on controlled provider");
            const auto cancel = GetTickCount64();
            if (close) cache.Reset(); else if (scenario == 0) session->active = false;
            const DWORD budget = scenario < 2 ? 2000 : 4000;
            Check(process && WaitForSingleObject(process, budget) == WAIT_OBJECT_0 && GetTickCount64() - cancel < budget,
                scenario == 0 ? "document switch terminates obsolete slow provider" :
                scenario == 1 ? "close cancels slow metadata without waiting for provider" :
                scenario == 2 ? "timed out provider is terminated within request budget" :
                                "unexpected provider exit completes worker failure");
            if (scenario >= 2) {
                const auto stored_deadline = GetTickCount64() + 1000;
                bool stored = false;
                do {
                    { std::lock_guard lock(cache.mutex_); stored = !cache.items_.empty(); }
                    if (stored) break;
                    Sleep(5);
                } while (GetTickCount64() < stored_deadline);
                Check(stored, "provider failure is recorded and cached before test teardown");
            }
            cache.Reset(); if (process) CloseHandle(process); CloseHandle(entered);
        }
        return failures ? 1 : 0;
    }
};
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && wcscmp(argv[1], L"--slow-provider") == 0) return SlowProvider(argv[2]);
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"document-image-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::error_code ec; std::filesystem::create_directories(root.parent_path(), ec);
    if (ec || !CreateDirectoryW(root.c_str(), nullptr)) { Check(false, "exclusive fixture created"); return 1; }
    pulse::diagnostics::runtime::Initialize(root.wstring(), "cache_test");
    pulse::ui::ThumbnailCacheTestAccess::Run();
    pulse::diagnostics::runtime::Shutdown();
    const auto log_path = root / L"Diagnostics" / L"Runtime" /
        (L"cache_test-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
    std::ifstream log_file(log_path, std::ios::binary);
    const std::string log{std::istreambuf_iterator<char>(log_file), std::istreambuf_iterator<char>()};
    Check(log.find("\"event\":\"preview_client_cancelled\",\"severity\":0") != std::string::npos,
        "normal cancellation has its own non-error classification");
    Check(log.find("\"stage\":5,\"error\":1460") != std::string::npos &&
        log.find("\"exit_known\":1,\"exit_code\":259") != std::string::npos,
        "timeout records capture original live host before termination");
    Check(log.find("\"exit_known\":1,\"exit_code\":77") != std::string::npos,
        "unexpected exit retains original process exit code before cleanup");
    Check(log.find("\"request\":1,\"generation\":1,\"host_pid\":") != std::string::npos &&
        log.find("synthetic-slow.png") == std::string::npos,
        "client failures correlate request and host without private paths");
    log_file.close();
    if (argc == 2 && wcscmp(argv[1], L"--diagnostics-only") == 0) {
        std::filesystem::remove_all(root);
        return failures ? 1 : 0;
    }
    const auto file = root / L"offline.bmp";
    { std::ofstream out(file, std::ios::binary); out << "private offline fixture"; Check(out.good(), "private fixture written"); }
    Check(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_OFFLINE) && (GetFileAttributesW(file.c_str()) & FILE_ATTRIBUTE_OFFLINE), "offline flag set only on private ordinary file");
    pulse_test::Host host;
    Check(host.Start(), "production preview host started");
    pulse_test::Result result;
    Check(host.Request(file.wstring(), result, FILE_ATTRIBUTE_NORMAL, 128, pulse::ipc::PreviewRequestKind::Content,
        pulse::ipc::kPreviewRequestFlagDocumentImage) && result.response.status != 0 && result.response.bytes_read == 0 && result.error == L"offline-placeholder",
        "production host resolves actual offline metadata and reads zero content");
    result = {};
    Check(host.Request(root.wstring(), result, FILE_ATTRIBUTE_NORMAL, 128, pulse::ipc::PreviewRequestKind::Content,
        pulse::ipc::kPreviewRequestFlagDocumentImage) && result.response.status != 0 && result.response.bytes_read == 0 && result.error == L"document-image-directory",
        "production host rejects directories before decode");
    result = {};
    Check(host.Request((root / L"missing.png").wstring(), result, FILE_ATTRIBUTE_NORMAL, 128, pulse::ipc::PreviewRequestKind::Content,
        pulse::ipc::kPreviewRequestFlagDocumentImage) && result.response.status != 0 && result.error == L"path-unavailable",
        "production host reports missing image metadata");
    Check(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL), "private image brought online for success case");
    {
        const unsigned char bmp[] = {
            0x42, 0x4d, 62, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
            40, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0,
            0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 20, 130, 220, 220, 130, 20, 0, 0};
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bmp), sizeof(bmp));
        Check(out.good(), "private bitmap fixture written");
    }
    result = {};
    Check(host.Request(file.wstring(), result, FILE_ATTRIBUTE_OFFLINE, 128, pulse::ipc::PreviewRequestKind::Content,
        pulse::ipc::kPreviewRequestFlagDocumentImage) && result.response.status == 0 &&
        result.response.kind == pulse::ipc::PreviewContentKind::Bitmap && !result.pixels.empty(),
        "production host uses fresh online metadata and decodes image");
    host.Stop();
    Check(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL) && DeleteFileW(file.c_str()) && RemoveDirectoryW(root.c_str()), "exclusive fixture cleaned up");
    return failures ? 1 : 0;
}

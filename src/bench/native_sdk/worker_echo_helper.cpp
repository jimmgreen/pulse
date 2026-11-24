#include <pulse/worker.hpp>

#include <cstdlib>
#include <string>
#include <string_view>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd_line, int) {
    if (cmd_line && std::wstring_view(cmd_line).find(L"--child-noop") != std::wstring_view::npos) {
        return 0;
    }

    return pulse::worker::WorkerServer::RunLoop(
        [](const pulse::worker::WireFrame& req, const pulse::worker::WorkerServer::Responder& rsp) -> bool {
            const std::string_view body(req.payload);
            if (body.starts_with("echo:")) {
                (void)rsp.SendProgress(1u, "p1");
                (void)rsp.SendProgress(2u, "p2");
                (void)rsp.SendReply(req.tag, body.substr(5));
                return true;
            }
            if (body.starts_with("error:")) {
                (void)rsp.SendError(req.tag, body.substr(6));
                return true;
            }
            if (body == "hang") {
                for (int i = 0; i < 600; ++i) {
                    Sleep(50);
                }
                (void)rsp.SendReply(req.tag, "done");
                return true;
            }
            if (body == "crash") {
                ExitProcess(42);
            }
            if (body.starts_with("oversize:")) {
                const size_t n = static_cast<size_t>(std::strtoul(req.payload.c_str() + 9, nullptr, 10));
                std::string big(n, 'X');
                (void)rsp.SendReply(req.tag, big);
                return true;
            }
            if (body == "spawn_child") {
                std::wstring self_exe(32768, L'\0');
                const DWORD len = GetModuleFileNameW(nullptr, self_exe.data(), static_cast<DWORD>(self_exe.size()));
                if (len == 0 || len >= self_exe.size()) {
                    (void)rsp.SendError(req.tag, "GetModuleFileNameW failed");
                    return true;
                }
                self_exe.resize(len);
                std::wstring child_cmd = L"\"" + self_exe + L"\" --child-noop";
                STARTUPINFOW si{};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                if (CreateProcessW(self_exe.c_str(), child_cmd.data(), nullptr, nullptr, FALSE,
                                   DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    (void)rsp.SendReply(req.tag, "allowed");
                } else {
                    (void)rsp.SendReply(req.tag, "blocked");
                }
                return true;
            }
            (void)rsp.SendError(req.tag, "unknown command");
            return true;
        });
}

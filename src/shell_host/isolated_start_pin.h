#pragma once
#include "ctx_handlers.h"
namespace pulse::shell {
class IsolatedStartPinSession {
  public:
    IsolatedStartPinSession() = default;
    IsolatedStartPinSession(const IsolatedStartPinSession &) = delete;
    IsolatedStartPinSession &operator=(const IsolatedStartPinSession &) = delete;
    ~IsolatedStartPinSession();
    HRESULT Query(const std::vector<std::wstring> &paths, HWND owner, UINT first, UINT last, UINT flags,
                  HANDLE cancel, std::vector<CtxItemOut> &out);
    HRESULT Invoke(UINT id);
    static void Prewarm();

  private:
    HRESULT AcquireProcess();
    void ReturnIdle();
    void Close();
    void Destroy();
    HRESULT Exchange(const ipc::PayloadWriter &request, std::vector<uint8_t> &response, HANDLE cancel,
                     DWORD timeout_ms = 15000);
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
};
// Shared by the isolated worker and its side-effect-free invocation regression test.
HRESULT InvokeIsolatedStartPinItem(const CtxHandlerSlot &slot, const std::vector<CtxItemOut> &items,
                                   HWND owner, UINT id);
int RunIsolatedStartPin(int argc, wchar_t **argv);
} // namespace pulse::shell

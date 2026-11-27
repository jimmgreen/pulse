#pragma once
#include "browse_request.h"
#include "native_browse_guard.h"
#include "native_browse_intent.h"

namespace pulse::native_browse {
// Owns both protections: synchronous API recursion and the asynchronous
// observer intent. A failed reservation never suppresses the real native call.
class Launch {
public:
    Launch() noexcept : outer_(!Active()) {}
    ~Launch() { Complete(false); }
    Launch(const Launch&) = delete;
    Launch& operator=(const Launch&) = delete;

    DWORD Prepare(const browse::Request& request) noexcept {
        const DWORD saved = GetLastError();
        DWORD error = ERROR_SUCCESS;
        if (outer_ && !pending_) {
            try {
                native_browse_intent::Request intent;
                intent.folder = request.path;
                if (request.intent == browse::Intent::Reveal) {
                    const auto slash = intent.folder.find_last_of(L"\\/");
                    if (slash == std::wstring::npos || slash + 1 == intent.folder.size()) {
                        SetLastError(saved);
                        return ERROR_INVALID_NAME;
                    }
                    intent.selection = intent.folder.substr(slash + 1);
                    intent.folder.resize(slash == 2 ? 3 : slash);
                }
                error = publisher_.Open();
                if (!error) {
                    const auto publication = publisher_.Publish(intent);
                    error = publication.error;
                    if (!error) { ticket_ = publication.ticket; pending_ = true; }
                }
            } catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
        }
        if (error) {
            wchar_t message[128]{};
            swprintf_s(message, L"Pulse native fallback: retention reservation failed error=%lu; native call proceeds.\n", error);
            OutputDebugStringW(message);
        }
        SetLastError(saved);
        return error;
    }
    void Complete(bool success) noexcept {
        const DWORD saved = GetLastError();
        if (pending_) {
            if (success) publisher_.CommitLaunch(ticket_);
            else publisher_.Cancel(ticket_);
            pending_ = false;
        }
        SetLastError(saved);
    }
private:
    // Declaration order captures the outer scope before Scope increments depth.
    bool outer_;
    Scope guard_;
    native_browse_intent::Publisher publisher_;
    native_browse_intent::Ticket ticket_{};
    bool pending_ = false;
};
}

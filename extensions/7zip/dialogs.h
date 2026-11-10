// dialogs.h - the extension's dialogs over pulse.dialogs. UI thread only. The
// host shows one dialog per session, so requests wait in a queue.
#pragma once
#include <pulse/dialogs.h>
#include "engine.h"
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct CompressChoice {
    std::wstring name;
    engine::CreateOptions options;
};

class Dialogs {
public:
    explicit Dialogs(const PulseDialogsV1& api) : api_(api) {}
    // `done(ok, password)`; ok is false when cancelled or the dialog could not show.
    // `remembering` adds a note that the password is kept for this session.
    void AskPassword(const std::wstring& archive, bool retry, bool remembering, std::function<void(bool, std::wstring)> done);
    struct CompressSource {
        std::wstring base;          // default archive name without extension
        std::string title, detail;  // "a.txt and 2 more", "Saves to folder"
    };
    void ChooseCompression(const CompressSource& source, int format, int level, const std::string& error,
                           std::function<void(bool, CompressChoice)> done);
    void ShowChecksums(const std::vector<engine::HashResult>& results, std::function<void(bool copy)> done);

    using States = std::map<std::string, std::string>;
    struct Request;
private:
    void Enqueue(std::shared_ptr<Request> request);
    void Next();
    static void PULSE_CALL Changed(void* user, uint64_t dialog, const PulseUiState* changes, uint32_t count);
    static void PULSE_CALL Closed(void* user, uint64_t dialog, PulseStatus status, PulseStringView button,
                                  const PulseUiState* states, uint32_t count);
    PulseDialogsV1 api_;
    std::deque<std::shared_ptr<Request>> queue_;
    std::shared_ptr<Request> current_;
};

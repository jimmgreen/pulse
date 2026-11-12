// jobs.h - extraction, testing, compression and checksums as host tasks. Output
// is written into a host stage on the destination volume and published through
// Pulse's confirmation, conflict handling and undo.
#pragma once
#include <pulse/client.hpp>
#include <pulse/staging.h>
#include "dialogs.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Jobs {
public:
    enum class ExtractMode { Smart, Folder, Into };
    Jobs(const PulseTasksV1& tasks, const PulseStagingV1& staging, pulse::sdk::DispatchQueue& dispatch, Dialogs& dialogs);
    ~Jobs();
    // destination: UTF-8 folder. Smart/Folder wrap per ExtractMode; Into writes as is.
    PulseStatus Extract(const std::wstring& archive, const std::string& destination, ExtractMode mode);
    // extract: what 智能解压 on the passed-test dialog runs (none: no such button).
    PulseStatus Test(const std::wstring& archive, std::function<void()> extract = {});
    PulseStatus Compress(const std::vector<std::wstring>& sources, const std::string& destination, CompressChoice choice);
    PulseStatus Hash(const std::vector<std::wstring>& files);
    void CancelAll();
    void RememberPasswords(bool on);
    struct Shared;
private:
    // What the buttons of an ended task do (retry, re-enter the password, test), by task.
    using FollowUps = std::map<std::string, std::function<void()>, std::less<>>;
    void Offer(PulseTaskId id, FollowUps follow_ups);
    pulse::sdk::TaskGroup tasks_;
    std::shared_ptr<Shared> shared_;
    std::map<PulseTaskId, FollowUps> follow_ups_;
};

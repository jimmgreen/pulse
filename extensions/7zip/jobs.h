// jobs.h - extraction, testing, compression and checksums as host tasks. Output
// is written into a host stage on the destination volume and published through
// Pulse's confirmation, conflict handling and undo.
#pragma once
#include <pulse/client.hpp>
#include <pulse/staging.h>
#include "dialogs.h"
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
    PulseStatus Test(const std::wstring& archive);
    PulseStatus Compress(const std::vector<std::wstring>& sources, const std::string& destination, CompressChoice choice);
    PulseStatus Hash(const std::vector<std::wstring>& files);
    void CancelAll();
    void RememberPasswords(bool on);
    struct Shared;
private:
    pulse::sdk::TaskGroup tasks_;
    std::shared_ptr<Shared> shared_;
};

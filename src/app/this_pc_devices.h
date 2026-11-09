#pragma once
#include "../fs/device_source.h"

namespace pulse::app {

inline void AppendThisPcDevices(std::vector<fs::DirEntry>& entries,
                               const std::vector<fs::DeviceInfo>& devices) {
    for (const auto& device : devices) {
        fs::DirEntry entry;
        entry.name = device.name;
        entry.full_path = fs::FormatDevicePath(fs::DevicePath{device.key, L"", {}});
        entry.attrs = FILE_ATTRIBUTE_NORMAL;
        entry.pc_device = fs::PcDeviceFlags(device);
        entries.push_back(std::move(entry));
    }
}

} // namespace pulse::app

#pragma once
#include <string>
#include <vector>

enum class DeviceType { Fischero, Cat };

struct DeviceInfo {
    std::string name;
    std::string ble_mac;
    std::string classic_mac;        // may be empty for Cat
    DeviceType  type;
};

std::vector<DeviceInfo> scan_for_devices(int timeout_secs);
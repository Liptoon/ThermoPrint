#include "scanner.h"
#include <simpleble/SimpleBLE.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <cstring>

static std::string ble_to_classic_mac(const std::string &ble_mac) {
    if (ble_mac.size() < 17) return ble_mac;
    unsigned int first = 0;
    sscanf(ble_mac.c_str(), "%02X", &first);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X%s", first & 0xFE, ble_mac.c_str() + 2);
    return std::string(buf);
}

static const std::vector<std::string> CAT_PREFIXES = {
    "GB01","GB02","GB03","GT01","YT01","MX05","MX06","MX08","MX10","MXTP"
};

std::vector<DeviceInfo> scan_for_devices(int timeout_secs) {
    std::vector<DeviceInfo> result;
    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) return result;

    auto& adapter = adapters[0];
    std::mutex mtx;
    bool scanning = true;

    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral p) {
        std::lock_guard<std::mutex> lock(mtx);
        std::string name = p.identifier();
        std::string addr = p.address();
        std::string name_lower = name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

        // check for duplicates
        for (auto& d : result) if (d.ble_mac == addr) return;

        DeviceType type;
        if (name_lower.find("fichero") == 0) {
            type = DeviceType::Fischero;
        } else {
            bool cat_found = false;
            for (auto& prefix : CAT_PREFIXES) {
                std::string pfx_lower = prefix;
                std::transform(pfx_lower.begin(), pfx_lower.end(), pfx_lower.begin(), ::tolower);
                if (name_lower.find(pfx_lower) == 0) { cat_found = true; break; }
            }
            if (!cat_found) return;
            type = DeviceType::Cat;
        }

        DeviceInfo dev;
        dev.name = name;
        dev.ble_mac = addr;
        dev.type = type;
        if (type == DeviceType::Fischero)
            dev.classic_mac = ble_to_classic_mac(addr);
        result.push_back(dev);
    });

    adapter.scan_start();
    auto start = std::chrono::steady_clock::now();
    while (scanning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_secs) break;
    }
    adapter.scan_stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return result;
}
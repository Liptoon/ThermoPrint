#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

using Bytes = std::vector<uint8_t>;
using NotifyCallback = std::function<void(const Bytes &data)>;

class BLETransport {
public:
    BLETransport();
    ~BLETransport();

    // Connect by BLE name prefix or MAC address.
    bool scan_and_connect(const std::string &name_or_addr,
                          int scan_timeout_s = 10);

    // Scan only - find device by name prefix, return BLE MAC without connecting.
    std::string scan_only(const std::string &name_prefix,
                          int scan_timeout_s = 10);

    // Scan for both BLE and Classic BT MACs that a dual-mode device advertises.
    // Returns {ble_mac, classic_mac}. Either may be empty if not seen.
    std::pair<std::string,std::string>
    scan_only_both(const std::string &name_prefix, int scan_timeout_s = 10);

    // Connect to the first device whose BLE advertisement name
    // starts with any of the given prefixes (case-insensitive).
    // Used for Cat printer auto-discovery across GT01/MX10/etc.
    bool scan_and_connect_by_names(const std::vector<std::string> &name_prefixes,
                                   int scan_timeout_s = 10);

    void dump_services();

    void write(const std::string &service_uuid,
               const std::string &char_uuid,
               const Bytes &data,
               bool with_response = false);

    void write_chunked(const std::string &service_uuid,
                       const std::string &char_uuid,
                       const Bytes &data,
                       size_t chunk_size = 20,
                       unsigned delay_ms = 20);

    void subscribe(const std::string &service_uuid,
                   const std::string &char_uuid,
                   NotifyCallback cb);

    void unsubscribe(const std::string &service_uuid,
                     const std::string &char_uuid);

    Bytes read(const std::string &service_uuid,
               const std::string &char_uuid);

    bool has_characteristic(const std::string &service_uuid,
                             const std::string &char_uuid);

    bool is_connected() const;
    void disconnect();

    const std::string &address() const { return address_; }
    const std::string &name()    const { return name_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string address_;
    std::string name_;
};

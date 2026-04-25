#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <thread>
#include <atomic>

using Bytes = std::vector<uint8_t>;
using NotifyCallback = std::function<void(const Bytes &data)>;

// ------------------------------------------------------------
// SppTransport - Classic Bluetooth SPP via RFCOMM socket
// ------------------------------------------------------------
class SppTransport {
public:
    SppTransport();
    ~SppTransport();

    // Connect by MAC address (explicit MAC required - no name scan).
    bool scan_and_connect(const std::string &name_or_addr, int timeout_s = 15);
    bool connect_by_addr(const std::string &mac_addr, int channel = 1);

    // Query RFCOMM channel via SDP. Returns 1 if SDP unavailable.
    int find_channel(const std::string &mac_addr);

    void write(const Bytes &data);
    void write_chunked(const Bytes &data,
                       size_t chunk_size = 512,
                       unsigned delay_ms = 0);

    // Start background reader thread; calls cb for every received chunk.
    void start_notify(NotifyCallback cb);
    // Stop reader thread cleanly (non-blocking, uses select() internally).
    void stop_notify();

    bool is_connected() const;
    void disconnect();

    const std::string &address() const { return address_; }

private:
    int         sock_fd_  = -1;
    std::string address_;

    std::atomic<bool> notify_running_{false};
    std::thread       notify_thread_;
    NotifyCallback    notify_cb_;
};

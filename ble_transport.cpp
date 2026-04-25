#include "ble_transport.h"
#include "utils.h"

#include <simpleble/SimpleBLE.h>

#include <thread>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <stdexcept>
#include <cstring>

// Convert SimpleBLE ByteArray (kvn::bytearray, uint8_t-based) to our Bytes
static Bytes from_simpleble(const SimpleBLE::ByteArray &ba)
{
    return Bytes(ba.begin(), ba.end());
}

static SimpleBLE::ByteArray to_simpleble(const Bytes &b)
{
    return SimpleBLE::ByteArray(b.data(), b.size());
}

// Normalise UUID to lowercase full 128-bit form.
// Handles: 4-char short (e.g. "ff00"), 8-char short (e.g. "0000ff00"),
// and full 128-bit (e.g. "0000ff00-0000-1000-8000-00805f9b34fb").
static std::string norm_uuid(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s.size() == 4) {
        s = "0000" + s + "-0000-1000-8000-00805f9b34fb";
    } else if (s.size() == 8) {
        s = s + "-0000-1000-8000-00805f9b34fb";
    }
    return s;
}

// ------------------------------------------------------------
// BLETransport::Impl
// ------------------------------------------------------------
struct BLETransport::Impl {
    SimpleBLE::Peripheral peripheral;
    bool connected = false;

    // Find service+characteristic (normalising UUIDs)
    bool find_char(const std::string &svc_uuid,
                   const std::string &chr_uuid,
                   SimpleBLE::Service    &out_svc,
                   SimpleBLE::Characteristic &out_chr)
    {
        std::string ns = norm_uuid(svc_uuid);
        std::string nc = norm_uuid(chr_uuid);
        for (auto &svc : peripheral.services()) {
            if (svc.uuid() == ns) {
                for (auto &chr : svc.characteristics()) {
                    if (chr.uuid() == nc) {
                        out_svc = svc;
                        out_chr = chr;
                        return true;
                    }
                }
            }
        }
        return false;
    }
};

BLETransport::BLETransport() = default;

BLETransport::~BLETransport()
{
    if (impl_ && impl_->connected) {
        try { impl_->peripheral.disconnect(); } catch (...) {}
    }
}

// ------------------------------------------------------------
// scan_only_both - scan for a printer by name prefix.
// Returns {ble_mac, classic_mac}.
// Tries to find BOTH MACs that the dual-mode device advertises:
//   FICHERO_5836_BLE  -> BLE MAC  (C9:...)
//   FICHERO_5836      -> Classic BT MAC (C8:...)
// Falls back to BLE MAC only + derives Classic BT if only one is seen.
// Does NOT connect to either.
// ------------------------------------------------------------
std::pair<std::string,std::string>
BLETransport::scan_only_both(const std::string &name_prefix, int scan_timeout_s)
{
    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        LOG_ERROR("No Bluetooth adapters found");
        return {"", ""};
    }

    auto &adapter = adapters[0];
    LOG_VERBOSE("BT adapter: %s (%s)",
                adapter.identifier().c_str(), adapter.address().c_str());

    std::string prefix_lower = name_prefix;
    std::transform(prefix_lower.begin(), prefix_lower.end(),
                   prefix_lower.begin(), ::tolower);

    std::string ble_mac;      // ends in _BLE suffix
    std::string classic_mac;  // no _BLE suffix - this IS the Classic BT MAC

    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral p) {
        std::string dev_addr = p.address();
        std::string dev_name = p.identifier();
        std::string dev_lower = dev_name;
        std::transform(dev_lower.begin(), dev_lower.end(), dev_lower.begin(), ::tolower);

        if (dev_lower.find(prefix_lower) == std::string::npos) return;

        LOG_VERBOSE("BLE scan: [%s] %s", dev_addr.c_str(), dev_name.c_str());

        // Distinguish by name suffix: _BLE = BLE advertisement, no suffix = Classic BT
        if (dev_lower.size() >= 4 &&
            dev_lower.substr(dev_lower.size() - 4) == "_ble") {
            if (ble_mac.empty()) {
                ble_mac = dev_addr;
                LOG_INFO("Found BLE addr:     [%s] %s", dev_addr.c_str(), dev_name.c_str());
            }
        } else {
            if (classic_mac.empty()) {
                classic_mac = dev_addr;
                LOG_INFO("Found Classic addr: [%s] %s", dev_addr.c_str(), dev_name.c_str());
            }
        }
    });

    adapter.scan_start();
    LOG_INFO("Scanning for '%s' (%ds)...", name_prefix.c_str(), scan_timeout_s);

    auto start = std::chrono::steady_clock::now();
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();

        // Stop when both MACs found, or on timeout
        bool have_both = !ble_mac.empty() && !classic_mac.empty();
        if (have_both || elapsed >= scan_timeout_s) {
            if (!have_both && ble_mac.empty() && classic_mac.empty())
                LOG_ERROR("Scan timeout: '%s' not found", name_prefix.c_str());
            break;
        }
    }

    adapter.scan_stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    return {ble_mac, classic_mac};
}

// Legacy single-return scan for Cat printer and BLE-only use
std::string BLETransport::scan_only(const std::string &name_prefix,
                                     int scan_timeout_s)
{
    auto [ble_mac, classic_mac] = scan_only_both(name_prefix, scan_timeout_s);
    // Return whichever we found (BLE preferred for Cat since it uses BLE)
    return ble_mac.empty() ? classic_mac : ble_mac;
}

// ------------------------------------------------------------
// scan_and_connect
// ------------------------------------------------------------
bool BLETransport::scan_and_connect(const std::string &name_or_addr,
                                     int scan_timeout_s)
{
    impl_ = std::make_unique<Impl>();

    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        LOG_ERROR("No Bluetooth adapters found");
        return false;
    }

    auto &adapter = adapters[0];
    LOG_INFO("Using BT adapter: %s (%s)",
             adapter.identifier().c_str(), adapter.address().c_str());

    bool found = false;
    SimpleBLE::Peripheral target_peripheral;

    // Check if we were given an address or a name
    bool is_addr = (name_or_addr.find(':') != std::string::npos);

    std::string name_lower = name_or_addr;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral p) {
        if (found) return;

        std::string dev_addr = p.address();
        std::string dev_name = p.identifier();
        std::string dev_name_lower = dev_name;
        std::transform(dev_name_lower.begin(), dev_name_lower.end(),
                       dev_name_lower.begin(), ::tolower);

        LOG_VERBOSE("Found: [%s] %s", dev_addr.c_str(), dev_name.c_str());

        bool match = false;
        if (is_addr) {
            std::string addr_lower = dev_addr;
            std::transform(addr_lower.begin(), addr_lower.end(), addr_lower.begin(), ::tolower);
            match = (addr_lower == name_lower);
        } else {
            match = (dev_name_lower.find(name_lower) != std::string::npos);
        }

        if (match) {
            // Don't call scan_stop() from inside the callback - deadlock risk
            // on BlueZ. Set the flag; polling loop calls scan_stop() safely.
            target_peripheral = p;
            found = true;
            LOG_INFO("Found device: [%s] %s", dev_addr.c_str(), dev_name.c_str());
        }
    });

    LOG_INFO("Scanning for '%s' (%ds timeout)...", name_or_addr.c_str(), scan_timeout_s);
    adapter.scan_start();

    auto start = std::chrono::steady_clock::now();
    while (!found) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= scan_timeout_s) {
            adapter.scan_stop();
            LOG_ERROR("Scan timeout: device '%s' not found", name_or_addr.c_str());
            return false;
        }
    }

    adapter.scan_stop();

    address_ = target_peripheral.address();
    name_    = target_peripheral.identifier();

    LOG_INFO("Connecting to %s [%s]...", name_.c_str(), address_.c_str());
    target_peripheral.connect();
    impl_->peripheral = target_peripheral;
    impl_->connected  = true;
    LOG_INFO("Connected.");

    return true;
}

// ------------------------------------------------------------
// scan_and_connect_by_names
// ------------------------------------------------------------
bool BLETransport::scan_and_connect_by_names(
        const std::vector<std::string> &name_prefixes,
        int scan_timeout_s)
{
    impl_ = std::make_unique<Impl>();

    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        LOG_ERROR("No Bluetooth adapters found");
        return false;
    }

    auto &adapter = adapters[0];
    LOG_INFO("Using BT adapter: %s (%s)",
             adapter.identifier().c_str(), adapter.address().c_str());

    // Lowercase all prefixes once
    std::vector<std::string> prefixes_lower;
    for (auto &p : name_prefixes) {
        std::string s = p;
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        prefixes_lower.push_back(s);
    }

    bool found = false;
    SimpleBLE::Peripheral target_peripheral;

    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral p) {
        if (found) return;

        std::string dev_addr = p.address();
        std::string dev_name = p.identifier();
        std::string dev_lower = dev_name;
        std::transform(dev_lower.begin(), dev_lower.end(), dev_lower.begin(), ::tolower);

        LOG_VERBOSE("Found: [%s] %s", dev_addr.c_str(), dev_name.c_str());

        for (const auto &prefix : prefixes_lower) {
            // Match: device name starts with or equals prefix (case-insensitive)
            if (dev_lower.substr(0, prefix.size()) == prefix) {
                target_peripheral = p;
                found = true;
                LOG_INFO("Found Cat printer '%s' [%s]",
                         dev_name.c_str(), dev_addr.c_str());
                return;
            }
        }
    });

    // Build human-readable list for the log message
    std::string names_str;
    for (size_t i = 0; i < name_prefixes.size(); i++) {
        if (i) names_str += '/';
        names_str += name_prefixes[i];
    }
    LOG_INFO("Scanning for Cat printer (%s, %ds timeout)...",
             names_str.c_str(), scan_timeout_s);

    adapter.scan_start();

    auto start = std::chrono::steady_clock::now();
    while (!found) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= scan_timeout_s) {
            adapter.scan_stop();
            LOG_ERROR("Scan timeout: no Cat printer found (looked for: %s)",
                      names_str.c_str());
            return false;
        }
    }

    adapter.scan_stop();

    address_ = target_peripheral.address();
    name_    = target_peripheral.identifier();

    LOG_INFO("Connecting to %s [%s]...", name_.c_str(), address_.c_str());
    target_peripheral.connect();
    impl_->peripheral = target_peripheral;
    impl_->connected  = true;
    LOG_INFO("Connected.");

    return true;
}

// ------------------------------------------------------------
// dump_services
// ------------------------------------------------------------
void BLETransport::dump_services()
{
    if (!impl_) return;
    LOG_INFO("BLE services on %s [%s]:", name_.c_str(), address_.c_str());
    for (auto &svc : impl_->peripheral.services()) {
        LOG_INFO("  Service: %s", svc.uuid().c_str());
        for (auto &chr : svc.characteristics()) {
            std::string props;
            if (chr.can_read())               props += "R";
            if (chr.can_write_request())      props += "W";
            if (chr.can_write_command())      props += "w";
            if (chr.can_notify())             props += "N";
            if (chr.can_indicate())           props += "I";
            LOG_INFO("    Char: %s [%s]", chr.uuid().c_str(), props.c_str());
        }
    }
}

// ------------------------------------------------------------
// write
// ------------------------------------------------------------
void BLETransport::write(const std::string &service_uuid,
                          const std::string &char_uuid,
                          const Bytes &data,
                          bool with_response)
{
    if (!impl_) throw std::runtime_error("Not connected");
    SimpleBLE::Service svc;
    SimpleBLE::Characteristic chr;
    if (!impl_->find_char(service_uuid, char_uuid, svc, chr)) {
        throw std::runtime_error(
            "Characteristic " + char_uuid + " not found in service " + service_uuid +
            ". Run with --verbose to see available services.");
    }

    LOG_VERBOSE("Write [%zu bytes] -> %s: %s",
                data.size(), char_uuid.c_str(), hex_dump(data).c_str());

    auto payload = to_simpleble(data);
    if (with_response && chr.can_write_request()) {
        impl_->peripheral.write_request(
            norm_uuid(service_uuid), norm_uuid(char_uuid), payload);
    } else {
        impl_->peripheral.write_command(
            norm_uuid(service_uuid), norm_uuid(char_uuid), payload);
    }
}

// ------------------------------------------------------------
// write_chunked
// ------------------------------------------------------------
void BLETransport::write_chunked(const std::string &service_uuid,
                                   const std::string &char_uuid,
                                   const Bytes &data,
                                   size_t chunk_size,
                                   unsigned delay_ms)
{
    LOG_VERBOSE("Write chunked: %zu bytes in chunks of %zu", data.size(), chunk_size);

    size_t offset = 0;
    while (offset < data.size()) {
        size_t end = std::min(offset + chunk_size, data.size());
        Bytes chunk(data.begin() + offset, data.begin() + end);
        write(service_uuid, char_uuid, chunk, false);
        offset = end;
        if (delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    LOG_VERBOSE("Write chunked done.");
}

// ------------------------------------------------------------
// subscribe
// ------------------------------------------------------------
void BLETransport::subscribe(const std::string &service_uuid,
                               const std::string &char_uuid,
                               NotifyCallback cb)
{
    if (!impl_) throw std::runtime_error("Not connected");
    try {
        impl_->peripheral.notify(
            norm_uuid(service_uuid),
            norm_uuid(char_uuid),
            [cb](SimpleBLE::ByteArray payload) {
                Bytes data = from_simpleble(payload);
                cb(data);
            });
        LOG_VERBOSE("Subscribed to notifications: %s", char_uuid.c_str());
    } catch (const std::exception &e) {
        // Some firmware variants use the same char UUID for both write and notify,
        // or have notify-capable chars that fail to subscribe on some BlueZ versions.
        // Log a warning but do not crash - we can still send commands; we just
        // won't receive responses. Status/info commands will time out gracefully.
        LOG_WARN("Notification subscribe failed for %s: %s", char_uuid.c_str(), e.what());
        LOG_WARN("Print commands will work but status queries will not receive responses.");
    }
}

// ------------------------------------------------------------
// unsubscribe
// ------------------------------------------------------------
void BLETransport::unsubscribe(const std::string &service_uuid,
                                 const std::string &char_uuid)
{
    if (!impl_) return;
    impl_->peripheral.unsubscribe(norm_uuid(service_uuid), norm_uuid(char_uuid));
}

// ------------------------------------------------------------
// read
// ------------------------------------------------------------
Bytes BLETransport::read(const std::string &service_uuid,
                           const std::string &char_uuid)
{
    if (!impl_) throw std::runtime_error("Not connected");
    auto ba = impl_->peripheral.read(
        norm_uuid(service_uuid), norm_uuid(char_uuid));
    return from_simpleble(ba);
}

// ------------------------------------------------------------
// has_characteristic
// ------------------------------------------------------------
bool BLETransport::has_characteristic(const std::string &service_uuid,
                                       const std::string &char_uuid)
{
    if (!impl_) return false;
    SimpleBLE::Service svc;
    SimpleBLE::Characteristic chr;
    return impl_->find_char(service_uuid, char_uuid, svc, chr);
}

// ------------------------------------------------------------
// is_connected / disconnect
// ------------------------------------------------------------
bool BLETransport::is_connected() const
{
    if (!impl_) return false;
    return impl_->peripheral.is_connected();
}

void BLETransport::disconnect()
{
    if (impl_ && impl_->connected) {
        impl_->peripheral.disconnect();
        impl_->connected = false;
        LOG_INFO("Disconnected.");
    }
}

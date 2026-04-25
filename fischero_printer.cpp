#include "fischero_printer.h"
#include "utils.h"

#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <cstring>
#include <sstream>

// ------------------------------------------------------------
// Command byte sequences (from PROTOCOL-fischero.md)
// ------------------------------------------------------------
static const Bytes CMD_GET_MODEL    = {0x10, 0xFF, 0x20, 0xF0};
static const Bytes CMD_GET_FIRMWARE = {0x10, 0xFF, 0x20, 0xF1};
static const Bytes CMD_GET_SERIAL   = {0x10, 0xFF, 0x20, 0xF2};
static const Bytes CMD_GET_BOOT     = {0x10, 0xFF, 0x20, 0xEF};
static const Bytes CMD_GET_BATTERY  = {0x10, 0xFF, 0x50, 0xF1};
static const Bytes CMD_GET_STATUS   = {0x10, 0xFF, 0x40};
static const Bytes CMD_GET_ALL_INFO = {0x10, 0xFF, 0x70};
static const Bytes CMD_FACTORY_RESET= {0x10, 0xFF, 0x04};
static const Bytes CMD_FORM_FEED    = {0x1D, 0x0C};
static const Bytes CMD_WAKE         = {0,0,0,0,0,0,0,0,0,0,0,0};
static const Bytes CMD_ENABLE_PRINT = {0x10, 0xFF, 0xFE, 0x01};  // AiYin-specific
static const Bytes CMD_STOP_PRINT   = {0x10, 0xFF, 0xFE, 0x45};  // AiYin-specific

// ------------------------------------------------------------
// Status bitmask decode
// ------------------------------------------------------------
static std::string decode_status(uint8_t s)
{
    if (s == 0x00) return "Ready";
    std::string out;
    if (s & 0x01) out += "Printing ";
    if (s & 0x02) out += "CoverOpen ";
    if (s & 0x04) out += "OutOfPaper ";
    if (s & 0x08) out += "LowBattery ";
    if (s & 0x10) out += "Overheated(alt) ";
    if (s & 0x20) out += "Charging ";
    if (s & 0x40) out += "Overheated ";
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// ------------------------------------------------------------
// Raster image header builder
// ------------------------------------------------------------
static Bytes raster_header(int width_bytes, int height_rows, uint8_t mode = 0)
{
    return {0x1D, 0x76, 0x30, mode,
            (uint8_t)(width_bytes & 0xFF), (uint8_t)((width_bytes >> 8) & 0xFF),
            (uint8_t)(height_rows  & 0xFF), (uint8_t)((height_rows  >> 8) & 0xFF)};
}

// ------------------------------------------------------------
// Notification / response plumbing
// ------------------------------------------------------------
struct FischeroNotify {
    std::mutex              mtx;
    std::condition_variable cv;
    Bytes                   buf;
    bool                    ready = false;

    void reset() {
        std::lock_guard<std::mutex> lk(mtx);
        buf.clear(); ready = false;
    }
    void on_data(const Bytes &data) {
        std::lock_guard<std::mutex> lk(mtx);
        buf.insert(buf.end(), data.begin(), data.end());
        ready = true;
        cv.notify_all();
    }
    Bytes wait(int timeout_ms) {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                    [this]{ return ready; });
        return buf;
    }
};

static FischeroNotify g_notify;

// ------------------------------------------------------------
// FischeroPrinter
// ------------------------------------------------------------
FischeroPrinter::FischeroPrinter(Transport transport)
    : transport_mode_(transport) {}

FischeroPrinter::~FischeroPrinter() { disconnect(); }

// ------------------------------------------------------------
// send_raw - dispatch to SPP or BLE
// ------------------------------------------------------------
void FischeroPrinter::send_raw(const Bytes &data)
{
    if (using_spp_) {
        spp_.write(data);
    } else {
        ble_.write_chunked(active_svc_, active_write_, data, 20, 10);
    }
}

// ------------------------------------------------------------
// send_command - send and wait for response
// ------------------------------------------------------------
Bytes FischeroPrinter::send_command(const Bytes &cmd, int timeout_ms)
{
    g_notify.reset();
    LOG_VERBOSE("CMD -> %s", hex_dump(cmd).c_str());
    send_raw(cmd);
    auto resp = g_notify.wait(timeout_ms);
    LOG_VERBOSE("RSP <- %s", hex_dump(resp).c_str());
    return resp;
}

std::string FischeroPrinter::send_command_str(const Bytes &cmd, int timeout_ms)
{
    auto resp = send_command(cmd, timeout_ms);
    return std::string(resp.begin(), resp.end());
}

// ------------------------------------------------------------
// BLE service probing
// ------------------------------------------------------------
struct ServiceDef {
    const char *svc;
    const char *write_char;
    const char *notify_char;
};

static constexpr ServiceDef BLE_SERVICES[] = {
    {"000018f0-0000-1000-8000-00805f9b34fb",
     "00002af1-0000-1000-8000-00805f9b34fb",
     "00002af0-0000-1000-8000-00805f9b34fb"},
    {"0000ff00-0000-1000-8000-00805f9b34fb",
     "0000ff02-0000-1000-8000-00805f9b34fb",
     "0000ff01-0000-1000-8000-00805f9b34fb"},
    {"e7810a71-73ae-499d-8c15-faa9aef0c3f2",
     "bef8d6c9-9c25-11e1-9125-0800200c9a66",
     "bef8d6c9-9c25-11e1-9125-0800200c9a66"},
    {"49535343-fe7d-4ae5-8fa9-9fafd205e455",
     "49535343-8841-43f4-a8d4-ecbe34729bb3",
     "49535343-1e4d-4bd9-ba61-23c647249616"},
    {"0000fee7-0000-1000-8000-00805f9b34fb",
     "0000fee9-0000-1000-8000-00805f9b34fb",
     "0000fee8-0000-1000-8000-00805f9b34fb"},
};

void FischeroPrinter::setup_ble_notify()
{
    ble_.subscribe(active_svc_, active_notify_, [](const Bytes &data) {
        LOG_VERBOSE("Fischero BLE notify: %s", hex_dump(data).c_str());
        g_notify.on_data(data);
    });
}

bool FischeroPrinter::connect_ble(const std::string &name_or_addr, int scan_timeout_s)
{
    try {
        if (!ble_.scan_and_connect(name_or_addr, scan_timeout_s))
            return false;
    } catch (const std::exception &e) {
        LOG_ERROR("BLE connect failed: %s", e.what());
        LOG_ERROR("This device uses Classic BT SPP, not BLE.");
        LOG_ERROR("Use auto mode (no --ble flag) or --device MAC.");
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ble_.dump_services();

    bool found = false;
    for (auto &s : BLE_SERVICES) {
        if (ble_.has_characteristic(s.svc, s.write_char)) {
            active_svc_    = s.svc;
            active_write_  = s.write_char;
            active_notify_ = s.notify_char;
            LOG_INFO("BLE: Using service %s", s.svc);
            found = true;
            break;
        }
    }

    if (!found) {
        LOG_ERROR("BLE: No known Fischero service found.");
        LOG_ERROR("     Service table above shows what this device exposed.");
        ble_.disconnect();
        return false;
    }

    setup_ble_notify();
    using_spp_ = false;
    return true;
}

// Derive Classic BT (BR/EDR) MAC from BLE MAC.
static std::string ble_to_classic_mac(const std::string &ble_mac)
{
    if (ble_mac.size() < 17) return ble_mac;
    unsigned int first = 0;
    sscanf(ble_mac.c_str(), "%02X", &first);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X%s", first & 0xFE, ble_mac.c_str() + 2);
    return std::string(buf);
}

static void try_pair(const std::string &mac)
{
    LOG_INFO("Attempting to pair %s via bluetoothctl...", mac.c_str());
    std::string cmd =
        "bluetoothctl << 'EOF'\n"
        "agent NoInputNoOutput\n"
        "default-agent\n"
        "pair " + mac + "\n"
        "trust " + mac + "\n"
        "EOF";
    int rc = system(cmd.c_str());
    LOG_VERBOSE("bluetoothctl pair returned %d", rc);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
}

bool FischeroPrinter::connect(const std::string &name_or_addr, int scan_timeout_s)
{
    if (transport_mode_ == Transport::Ble) {
        LOG_INFO("Transport: BLE (forced)");
        return connect_ble(name_or_addr, scan_timeout_s);
    }

    std::string classic_mac;

    if (name_or_addr.find(':') != std::string::npos) {
        classic_mac = name_or_addr;
        LOG_INFO("Using provided MAC: %s", classic_mac.c_str());
    } else {
        auto [ble_mac, bt_mac] = ble_.scan_only_both(name_or_addr, scan_timeout_s);

        if (!bt_mac.empty()) {
            classic_mac = bt_mac;
            LOG_INFO("Using Classic BT MAC directly: %s", classic_mac.c_str());
        } else if (!ble_mac.empty()) {
            classic_mac = ble_to_classic_mac(ble_mac);
            LOG_INFO("BLE MAC: %s  =>  Classic BT MAC: %s (derived)",
                     ble_mac.c_str(), classic_mac.c_str());
        } else {
            LOG_ERROR("Device '%s' not found", name_or_addr.c_str());
            return false;
        }

        if (!ble_mac.empty()) ble_mac_ = ble_mac;
        else ble_mac_ = classic_mac;
    }

    int channel = 1;
    LOG_INFO("Connecting RFCOMM to %s ch%d...", classic_mac.c_str(), channel);

    if (spp_.connect_by_addr(classic_mac, channel)) {
        spp_.start_notify([](const Bytes &data) {
            LOG_VERBOSE("Fischero SPP recv: %s", hex_dump(data).c_str());
            g_notify.on_data(data);
        });
        using_spp_ = true;
        LOG_INFO("Connected via Classic BT RFCOMM.");
        return true;
    }

    LOG_INFO("RFCOMM failed - attempting pairing and retry...");
    try_pair(classic_mac);

    if (spp_.connect_by_addr(classic_mac, channel)) {
        spp_.start_notify([](const Bytes &data) {
            LOG_VERBOSE("Fischero SPP recv: %s", hex_dump(data).c_str());
            g_notify.on_data(data);
        });
        using_spp_ = true;
        LOG_INFO("Connected via Classic BT RFCOMM after pairing.");
        return true;
    }

    LOG_ERROR("----------------------------------------------");
    LOG_ERROR("Cannot connect to %s via RFCOMM.", classic_mac.c_str());
    LOG_ERROR("Try pairing manually:");
    LOG_ERROR("  bluetoothctl");
    LOG_ERROR("  agent NoInputNoOutput");
    LOG_ERROR("  pair %s", classic_mac.c_str());
    LOG_ERROR("  trust %s", classic_mac.c_str());
    LOG_ERROR("  quit");
    LOG_ERROR("Then: ./thermoprint --fischero --device %s --text 'test'",
              classic_mac.c_str());
    LOG_ERROR("----------------------------------------------");
    return false;
}

void FischeroPrinter::disconnect()
{
    if (using_spp_) {
        spp_.stop_notify();
        spp_.disconnect();
    } else {
        ble_.disconnect();
    }
}

bool FischeroPrinter::is_connected() const
{
    return using_spp_ ? spp_.is_connected() : ble_.is_connected();
}

// ------------------------------------------------------------
// Info queries
// ------------------------------------------------------------
std::string FischeroPrinter::get_model()         { return send_command_str(CMD_GET_MODEL); }
std::string FischeroPrinter::get_firmware()      { return send_command_str(CMD_GET_FIRMWARE); }
std::string FischeroPrinter::get_serial()        { return send_command_str(CMD_GET_SERIAL); }
std::string FischeroPrinter::get_boot_version()  { return send_command_str(CMD_GET_BOOT); }
std::string FischeroPrinter::get_all_info()      { return send_command_str(CMD_GET_ALL_INFO, 3000); }

std::pair<int,int> FischeroPrinter::get_battery()
{
    auto r = send_command(CMD_GET_BATTERY);
    return (r.size() >= 2) ? std::make_pair((int)r[0], (int)r[1])
                            : std::make_pair(-1, -1);
}

uint8_t FischeroPrinter::get_status_byte()
{
    auto r = send_command(CMD_GET_STATUS);
    return r.empty() ? 0xFF : r[0];
}

// ------------------------------------------------------------
// Status via BLE
// ------------------------------------------------------------
std::string FischeroPrinter::get_status_ble()
{
    if (ble_mac_.empty()) return "BLE MAC not known (run with auto-discovery)";

    if (!using_spp_ && ble_.is_connected()) {
        // Already connected via BLE - query directly
    } else {
        try {
            if (!ble_.scan_and_connect(ble_mac_, 10)) {
                return "BLE connect failed";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            bool found = false;
            for (auto &s : BLE_SERVICES) {
                if (ble_.has_characteristic(s.svc, s.write_char)) {
                    active_svc_    = s.svc;
                    active_write_  = s.write_char;
                    active_notify_ = s.notify_char;
                    found = true;
                    break;
                }
            }
            if (!found) {
                ble_.disconnect();
                return "BLE service not found";
            }
            setup_ble_notify();
        } catch (const std::exception &e) {
            return std::string("BLE error: ") + e.what();
        }
    }

    bool was_spp = using_spp_;
    using_spp_ = false;

    std::string result;
    try {
        auto [bst, bpct] = get_battery();
        std::string st = decode_status(get_status_byte());
        char buf[128];
        snprintf(buf, sizeof(buf), "%s | Battery: %d%% (0x%02X)", st.c_str(), bpct, bst);
        result = buf;
    } catch (...) {
        result = "BLE query failed";
    }

    using_spp_ = was_spp;

    if (was_spp) ble_.disconnect();

    return result;
}

std::string FischeroPrinter::get_status()
{
    if (using_spp_) {
        return decode_status(get_status_byte());
    }
    return get_status_ble();
}

void FischeroPrinter::print_info()
{
    LOG_INFO("--- Fischero D11s Info ---");
    if (using_spp_) {
        LOG_INFO("Transport:    Classic BT RFCOMM");
        LOG_INFO("Model:        %s", get_model().c_str());
        LOG_INFO("Firmware:     %s", get_firmware().c_str());
        LOG_INFO("Boot:         %s", get_boot_version().c_str());
        LOG_INFO("Serial:       %s", get_serial().c_str());
        auto [bst, bpct] = get_battery();
        LOG_INFO("Battery:      %d%% (status 0x%02X)", bpct, bst);
        auto st = get_status_byte();
        LOG_INFO("Status:       0x%02X (%s)", st, decode_status(st).c_str());
    } else {
        LOG_INFO("Transport:    BLE");
        LOG_INFO("BLE service:  %s", active_svc_.c_str());
        LOG_INFO("Model:        %s", get_model().c_str());
        LOG_INFO("Firmware:     %s", get_firmware().c_str());
        auto [bst, bpct] = get_battery();
        LOG_INFO("Battery:      %d%% (status 0x%02X)", bpct, bst);
        auto st = get_status_byte();
        LOG_INFO("Status:       0x%02X (%s)", st, decode_status(st).c_str());
    }
    LOG_INFO("Label size:   %d x 14 mm", label_len_mm_);
    if (!ble_mac_.empty())
        LOG_INFO("BLE MAC:      %s  (for --status queries)", ble_mac_.c_str());
}

// ------------------------------------------------------------
// Config
// ------------------------------------------------------------
void FischeroPrinter::set_density(int level)
{
    Bytes cmd = {0x10, 0xFF, 0x10, 0x00, (uint8_t)(level & 0xFF)};
    auto r = send_command_str(cmd);
    LOG_INFO("Set density %d: %s", level, r.c_str());
}

void FischeroPrinter::set_paper_type(int type)
{
    Bytes cmd = {0x10, 0xFF, 0x84, (uint8_t)(type & 0xFF)};
    auto r = send_command_str(cmd);
    LOG_INFO("Set paper type %d: %s", type, r.c_str());
}

void FischeroPrinter::set_shutdown_time(int minutes)
{
    Bytes cmd = {0x10, 0xFF, 0x12, (uint8_t)((minutes>>8)&0xFF), (uint8_t)(minutes&0xFF)};
    auto r = send_command_str(cmd);
    LOG_INFO("Set shutdown %d min: %s", minutes, r.c_str());
}

void FischeroPrinter::set_speed(int speed)
{
    Bytes cmd = {0x10, 0xFF, 0xC0, (uint8_t)(speed & 0xFF)};
    auto r = send_command(cmd, 1000);
    LOG_INFO("Set speed %d: %s", speed, hex_dump(r).c_str());
}

void FischeroPrinter::factory_reset()
{
    LOG_INFO("Factory reset: %s", send_command_str(CMD_FACTORY_RESET).c_str());
}

// ------------------------------------------------------------
// Feed / Form feed
// ------------------------------------------------------------
void FischeroPrinter::feed(int dots)
{
    send_raw(CMD_WAKE);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    send_raw(CMD_ENABLE_PRINT);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    while (dots > 0) {
        int chunk = std::min(dots, 255);
        Bytes cmd = {0x1B, 0x4A, (uint8_t)chunk};
        send_raw(cmd);
        dots -= chunk;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_notify.reset();
    send_raw(CMD_STOP_PRINT);
    g_notify.wait(3000);
    LOG_VERBOSE("Feed done.");
}

void FischeroPrinter::form_feed()
{
    send_raw(CMD_WAKE);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    send_raw(CMD_ENABLE_PRINT);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    send_raw(CMD_FORM_FEED);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_notify.reset();
    send_raw(CMD_STOP_PRINT);
    g_notify.wait(3000);
    LOG_VERBOSE("Form feed done.");
}

// ------------------------------------------------------------
// print_image
//
// IMPORTANT: the printer expects every raster row to be exactly
// WIDTH_BYTES bytes wide (12 bytes = 96 px). Previously this code
// declared a 12-byte stride in the header but then sent
// `img.packed()` whose row stride was `img.width / 8` — which after
// rotate_90cw is typically 4-6 bytes for short labels. The mismatch
// caused the printer to receive garbled rows and only jitter paper
// without printing anything. We now ALWAYS pad/centre the image to
// the full 96 px head width before sending, and additionally pad
// to the configured label length (30 or 50 mm) on the feed axis so
// the output is centred on the label.
// ------------------------------------------------------------
void FischeroPrinter::print_image(const BinImage &img, int copies)
{
    if (!is_connected())
        throw std::runtime_error("Printer not connected");

    const int WIDTH_BYTES = 12;
    const int PRINT_W_PX  = WIDTH_BYTES * 8;   // 96

    // ---- Centre horizontally on the 96-px print head ----------------
    BinImage page = center_on_width(img, PRINT_W_PX);

    // ---- Centre vertically inside the label length ------------------
    // 203 DPI -> dots/mm = 203 / 25.4 ≈ 8.0
    if (label_len_mm_ > 0) {
        int label_dots = (int)((double)label_len_mm_ * 203.0 / 25.4 + 0.5);
        if (page.height < label_dots)
            page = center_on_height(page, label_dots);
        // If image is taller than the label we leave it alone; the
        // gap sensor will still advance to the next label after print.
    }

    LOG_INFO("Page: %dx%d px (label %d mm)",
             page.width, page.height, label_len_mm_);

    for (int copy = 0; copy < copies; copy++) {
        LOG_INFO("Printing copy %d/%d...", copy + 1, copies);

        set_density(1);
        set_paper_type(0);

        send_raw(CMD_WAKE);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        send_raw(CMD_ENABLE_PRINT);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        Bytes raster;
        append(raster, raster_header(WIDTH_BYTES, page.height));
        append(raster, page.packed());

        LOG_INFO("Sending raster: %d rows x %d bytes = %zu bytes",
                 page.height, WIDTH_BYTES, raster.size());

        if (using_spp_) {
            spp_.write_chunked(raster, 512, 0);
        } else {
            ble_.write_chunked(active_svc_, active_write_, raster, 20, 10);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        send_raw(CMD_FORM_FEED);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        g_notify.reset();
        send_raw(CMD_STOP_PRINT);

        LOG_INFO("Waiting for print completion...");
        auto resp = g_notify.wait(10000);

        if (!resp.empty()) {
            LOG_INFO("Print complete (copy %d/%d).", copy + 1, copies);
        } else {
            LOG_WARN("No completion signal received (printed anyway).");
        }

        if (copy + 1 < copies)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

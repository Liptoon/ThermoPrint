#include "cat_printer.h"
#include "utils.h"

#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <algorithm>

// ------------------------------------------------------------
// CRC8 checksum table (from catprinter cmds.py - same values)
// ------------------------------------------------------------
static const uint8_t CHECKSUM_TABLE[256] = {
    0,   7,  14,   9,  28,  27,  18,  21,  56,  63,  54,  49,  36,  35,  42,  45,
  112, 119, 126, 121, 108, 107,  98, 101,  72,  79,  70,  65,  84,  83,  90,  93,
  224, 231, 238, 233, 252, 251, 242, 245, 216, 223, 214, 209, 196, 195, 202, 205,
  144, 151, 158, 153, 140, 139, 130, 133, 168, 175, 166, 161, 180, 179, 186, 189,
  199, 192, 201, 206, 219, 220, 213, 210, 255, 248, 241, 246, 227, 228, 237, 234,
  183, 176, 185, 190, 171, 172, 165, 162, 143, 136, 129, 134, 147, 148, 157, 154,
   39,  32,  41,  46,  59,  60,  53,  50,  31,  24,  17,  22,   3,   4,  13,  10,
   87,  80,  89,  94,  75,  76,  69,  66, 111, 104,  97, 102, 115, 116, 125, 122,
  137, 142, 135, 128, 149, 146, 155, 156, 177, 182, 191, 184, 173, 170, 163, 164,
  249, 254, 247, 240, 229, 226, 235, 236, 193, 198, 207, 200, 221, 218, 211, 212,
  105, 110, 103,  96, 117, 114, 123, 124,  81,  86,  95,  88,  77,  74,  67,  68,
   25,  30,  23,  16,   5,   2,  11,  12,  33,  38,  47,  40,  61,  58,  51,  52,
   78,  73,  64,  71,  82,  85,  92,  91, 118, 113, 120, 127, 106, 109, 100,  99,
   62,  57,  48,  55,  34,  37,  44,  43,   6,   1,   8,  15,  26,  29,  20,  19,
  174, 169, 160, 167, 178, 181, 188, 187, 150, 145, 152, 159, 138, 141, 132, 131,
  222, 217, 208, 215, 194, 197, 204, 203, 230, 225, 232, 239, 250, 253, 244, 243,
};

// ------------------------------------------------------------
// Notification plumbing (separate from Fischero)
// ------------------------------------------------------------
struct CatNotify {
    std::mutex              mtx;
    std::condition_variable cv;
    Bytes                   buf;
    bool                    ready = false;

    // The "printer ready" notification from catprinter
    static const Bytes READY_MAGIC;

    void reset() {
        std::lock_guard<std::mutex> lk(mtx);
        buf.clear();
        ready = false;
    }

    void on_data(const Bytes &data) {
        std::lock_guard<std::mutex> lk(mtx);
        buf.insert(buf.end(), data.begin(), data.end());
        // Signal if we see the printer-ready notification
        if (data == READY_MAGIC) ready = true;
        cv.notify_all();
    }

    bool wait_ready(int timeout_ms) {
        std::unique_lock<std::mutex> lk(mtx);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [this]{ return ready; });
    }
};

// Printer ready notification bytes (from catprinter ble.py)
const Bytes CatNotify::READY_MAGIC = {
    0x51, 0x78, 0xAE, 0x01, 0x01, 0x00, 0x00, 0x00, 0xFF
};

static CatNotify g_cat_notify;

// ------------------------------------------------------------
// CatPrinter
// ------------------------------------------------------------
CatPrinter::CatPrinter() = default;
CatPrinter::~CatPrinter() { disconnect(); }

// ------------------------------------------------------------
// CRC8
// ------------------------------------------------------------
uint8_t CatPrinter::checksum(const Bytes &data, int offset, int length)
{
    uint8_t crc = 0;
    for (int i = offset; i < offset + length; i++)
        crc = CHECKSUM_TABLE[(crc ^ data[i]) & 0xFF];
    return crc;
}

// ------------------------------------------------------------
// Command frame builder
// Frame format: [0x51, 0x78, cmd, 0x00, len_lo, 0x00, ...payload..., crc, 0xFF]
// ------------------------------------------------------------
Bytes CatPrinter::build_cmd(uint8_t cmd_byte, const Bytes &payload)
{
    Bytes out;
    out.push_back(0x51);
    out.push_back(0x78);
    out.push_back(cmd_byte);
    out.push_back(0x00);
    out.push_back((uint8_t)(payload.size() & 0xFF));
    out.push_back(0x00);
    for (uint8_t b : payload) out.push_back(b);
    out.push_back(checksum(out, 6, (int)payload.size()));
    out.push_back(0xFF);
    return out;
}

// ------------------------------------------------------------
// Fixed commands (ported from cmds.py)
// ------------------------------------------------------------
Bytes CatPrinter::cmd_get_dev_state()
{
    return build_cmd(0xA3, {0x00});
}

Bytes CatPrinter::cmd_set_quality_200dpi()
{
    return build_cmd(0xA4, {0x32});  // 0xA4 = -92 in signed; payload 50 = 200dpi
}

Bytes CatPrinter::cmd_set_energy(uint16_t val)
{
    Bytes payload = {(uint8_t)((val >> 8) & 0xFF), (uint8_t)(val & 0xFF)};
    Bytes out;
    out.push_back(0x51);
    out.push_back(0x78);
    out.push_back(0xAF);  // -81 signed = 0xAF
    out.push_back(0x00);
    out.push_back(0x02);
    out.push_back(0x00);
    out.push_back(payload[0]);
    out.push_back(payload[1]);
    out.push_back(checksum(out, 6, 2));
    out.push_back(0xFF);
    return out;
}

Bytes CatPrinter::cmd_apply_energy()
{
    Bytes out;
    out.push_back(0x51);
    out.push_back(0x78);
    out.push_back(0xBE);  // -66 signed = 0xBE
    out.push_back(0x00);
    out.push_back(0x01);
    out.push_back(0x00);
    out.push_back(0x01);
    out.push_back(checksum(out, 6, 1));
    out.push_back(0xFF);
    return out;
}

Bytes CatPrinter::cmd_lattice_start()
{
    Bytes payload = {0xAA, 0x55, 0x17, 0x38, 0x44, 0x5F, 0x5F, 0x5F, 0x44, 0x38, 0x2C};
    return build_cmd(0xA6, payload);  // 0xA6 = -90 signed
}

Bytes CatPrinter::cmd_lattice_end()
{
    Bytes payload = {0xAA, 0x55, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17};
    return build_cmd(0xA6, payload);
}

Bytes CatPrinter::cmd_set_paper()
{
    return build_cmd(0xA1, {0x30, 0x00});  // 0xA1 = -95 signed
}

Bytes CatPrinter::cmd_feed_paper(uint8_t amount)
{
    Bytes out;
    out.push_back(0x51);
    out.push_back(0x78);
    out.push_back(0xBD);  // -67 signed
    out.push_back(0x00);
    out.push_back(0x01);
    out.push_back(0x00);
    out.push_back(amount);
    out.push_back(checksum(out, 6, 1));
    out.push_back(0xFF);
    return out;
}

// ------------------------------------------------------------
// Row encoding: run-length or byte encoding (from cmds.py)
// ------------------------------------------------------------
static Bytes encode_run_length_repetition(int n, int val)
{
    Bytes res;
    while (n > 0x7F) {
        res.push_back((uint8_t)(0x7F | (val << 7)));
        n -= 0x7F;
    }
    if (n > 0) {
        res.push_back((uint8_t)((val << 7) | n));
    }
    return res;
}

static Bytes run_length_encode(const std::vector<bool> &row)
{
    Bytes res;
    int count = 0;
    int last_val = -1;
    for (bool b : row) {
        int val = b ? 1 : 0;
        if (val == last_val) {
            count++;
        } else {
            if (last_val >= 0) {
                auto chunk = encode_run_length_repetition(count, last_val);
                res.insert(res.end(), chunk.begin(), chunk.end());
            }
            count = 1;
            last_val = val;
        }
    }
    if (count > 0 && last_val >= 0) {
        auto chunk = encode_run_length_repetition(count, last_val);
        res.insert(res.end(), chunk.begin(), chunk.end());
    }
    return res;
}

static Bytes byte_encode(const std::vector<bool> &row)
{
    Bytes res;
    int w = (int)row.size();
    for (int chunk = 0; chunk < w; chunk += 8) {
        uint8_t byte = 0;
        for (int bit = 0; bit < 8 && chunk + bit < w; bit++) {
            if (row[chunk + bit]) byte |= (1 << bit);
        }
        res.push_back(byte);
    }
    return res;
}

Bytes CatPrinter::cmd_print_row(const std::vector<bool> &row)
{
    const int PRINT_WIDTH = 384;
    auto rle = run_length_encode(row);

    if ((int)rle.size() > PRINT_WIDTH / 8) {
        // Fallback to byte encoding
        auto raw = byte_encode(row);
        Bytes out;
        out.push_back(0x51);
        out.push_back(0x78);
        out.push_back(0xA2);  // -94 signed - raw row command
        out.push_back(0x00);
        out.push_back((uint8_t)(raw.size() & 0xFF));
        out.push_back(0x00);
        for (uint8_t b : raw) out.push_back(b);
        out.push_back(checksum(out, 6, (int)raw.size()));
        out.push_back(0xFF);
        return out;
    }

    // Run-length encoded row command
    Bytes out;
    out.push_back(0x51);
    out.push_back(0x78);
    out.push_back(0xBF);  // -65 signed
    out.push_back(0x00);
    out.push_back((uint8_t)(rle.size() & 0xFF));
    out.push_back(0x00);
    for (uint8_t b : rle) out.push_back(b);
    out.push_back(checksum(out, 6, (int)rle.size()));
    out.push_back(0xFF);
    return out;
}

// ------------------------------------------------------------
// connect
// ------------------------------------------------------------
// Known Cat printer BLE advertisement name prefixes.
// All models use the same protocol; only service UUID differs (ae30 vs af30).
static const std::vector<std::string> CAT_PRINTER_NAMES = {
    "GB01", "GB02", "GB03", "GT01", "YT01",
    "MX05", "MX06", "MX08", "MX10", "MXTP",
};

bool CatPrinter::connect(const std::string &name_or_addr, int scan_timeout_s)
{
    bool ok;
    if (!name_or_addr.empty()) {
        // User specified an explicit device name or MAC — use it directly.
        ok = ble_.scan_and_connect(name_or_addr, scan_timeout_s);
    } else {
        // Auto-discover: find the first advertising device whose name
        // starts with any known Cat printer model prefix.
        ok = ble_.scan_and_connect_by_names(CAT_PRINTER_NAMES, scan_timeout_s);
    }
    if (!ok) return false;

    if (g_verbose) ble_.dump_services();

    // Probe ae30 (GT01/GB0x) first, then af30 (MX10/MX05/MX06/MX08).
    // Protocol is identical; only the advertisement service UUID differs.
    if (ble_.has_characteristic(SVC_UUID_GT01, TX_UUID)) {
        active_svc_ = SVC_UUID_GT01;
        LOG_INFO("Model variant: GT01/GB0x (service ae30)");
    } else if (ble_.has_characteristic(SVC_UUID_MX10, TX_UUID)) {
        active_svc_ = SVC_UUID_MX10;
        LOG_INFO("Model variant: MX10/MX05/MX06/MX08/YT01 (service af30)");
    } else {
        LOG_ERROR("Neither ae30 nor af30 service found - unsupported device");
        return false;
    }

    setup_notify();
    return true;
}

void CatPrinter::setup_notify()
{
    ble_.subscribe(active_svc_, RX_UUID, [](const Bytes &data) {
        LOG_VERBOSE("Cat notify: %s", hex_dump(data).c_str());
        g_cat_notify.on_data(data);
    });
}

void CatPrinter::disconnect()
{
    ble_.disconnect();
}

bool CatPrinter::is_connected() const
{
    return ble_.is_connected();
}

// ------------------------------------------------------------
// Status / Info
// ------------------------------------------------------------
std::string CatPrinter::get_status()
{
    // The Cat printer protocol has no bidirectional status query.
    // Sending cmd_get_dev_state() before a print job is a one-way
    // handshake; it does not produce a parseable status response.
    // Report what we know from the BLE connection itself.
    if (!ble_.is_connected())
        return "Disconnected";
    return std::string("Connected via ") + active_svc_;
}

void CatPrinter::print_info()
{
    LOG_INFO("--- %s ---", printer_name());
    LOG_INFO("BLE address:    %s", ble_.address().c_str());
    LOG_INFO("BLE name:       %s", ble_.name().c_str());
    LOG_INFO("Service UUID:   %s", active_svc_.c_str());
    LOG_INFO("Energy setting: 0x%04X", energy_);
    LOG_INFO("Note: this printer has no status query command.");
    LOG_INFO("      Status is BLE-connection level only.");
    LOG_INFO("Status: %s", get_status().c_str());
}

// ------------------------------------------------------------
// Config
// ------------------------------------------------------------
void CatPrinter::set_density(int level)
{
    // Map 0=light, 1=medium, 2=dark to energy values
    uint16_t energy;
    switch (level) {
        case 0:  energy = 0x3000; break;
        case 1:  energy = 0x8000; break;
        case 2:  energy = 0xFFFF; break;
        default: energy = 0x8000; break;
    }
    set_energy(energy);
}

void CatPrinter::set_energy(uint16_t energy)
{
    energy_ = energy;
    LOG_INFO("Energy set to 0x%04X", energy_);
}

// ------------------------------------------------------------
// Feed
// ------------------------------------------------------------
void CatPrinter::feed(int dots)
{
    if (!is_connected())
        throw std::runtime_error("Printer not connected");

    // The Cat printer ignores bare feed commands outside a print session.
    // Wrap in a minimal lattice session: state + quality + lattice_start
    // + feed + lattice_end + state.  No image rows are sent.
    while (dots > 0) {
        int chunk = (dots > 255) ? 255 : dots;
        dots -= chunk;

        Bytes data;
        append(data, cmd_get_dev_state());
        append(data, cmd_set_quality_200dpi());
        append(data, cmd_set_energy(energy_));
        append(data, cmd_apply_energy());
        append(data, cmd_lattice_start());
        append(data, cmd_feed_paper((uint8_t)chunk));
        append(data, cmd_set_paper());
        append(data, cmd_lattice_end());
        append(data, cmd_get_dev_state());

        LOG_INFO("Feeding %d dots...", chunk);
        g_cat_notify.reset();
        ble_.write_chunked(active_svc_, TX_UUID, data, 20, 20);
        g_cat_notify.wait_ready(10000);
    }
}

// ------------------------------------------------------------
// print_image
// ------------------------------------------------------------
void CatPrinter::print_image(const BinImage &img, int copies)
{
    if (!is_connected())
        throw std::runtime_error("Printer not connected");

    if (img.width != 384) {
        LOG_WARN("Image width %d != 384 px, image may not print correctly", img.width);
    }

    for (int copy = 0; copy < copies; copy++) {
        LOG_INFO("Printing copy %d/%d...", copy + 1, copies);
        send_print_sequence(img, energy_);

        if (copy + 1 < copies) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

void CatPrinter::send_print_sequence(const BinImage &img, uint16_t energy)
{
    // Build the full command buffer (matching cmds_print_img from catprinter)
    Bytes data;

    append(data, cmd_get_dev_state());
    append(data, cmd_set_quality_200dpi());
    append(data, cmd_set_energy(energy));
    append(data, cmd_apply_energy());
    append(data, cmd_lattice_start());

    for (int y = 0; y < img.height; y++) {
        append(data, cmd_print_row(img.rows[y]));
    }

    append(data, cmd_feed_paper(25));
    append(data, cmd_set_paper());
    append(data, cmd_set_paper());
    append(data, cmd_set_paper());
    append(data, cmd_lattice_end());
    append(data, cmd_get_dev_state());

    LOG_INFO("Sending %zu bytes for %d rows...", data.size(), img.height);

    g_cat_notify.reset();

    // Send in chunks of 101 bytes (MTU-3 = 20 is safe default, but cat printer uses larger)
    // Use 20 bytes to be safe on any MTU.
    ble_.write_chunked(active_svc_, TX_UUID, data, 20, 20);

    LOG_INFO("Data sent. Waiting for printer ready (up to 30s)...");

    bool ok = g_cat_notify.wait_ready(30000);
    if (ok) {
        LOG_INFO("Printer ready - print complete.");
    } else {
        LOG_WARN("Timeout waiting for printer ready notification.");
    }
}

#pragma once
#include "printer.h"
#include "spp_transport.h"
#include <string>
#include <cstdint>
#include <memory>

// ------------------------------------------------------------
// Fischero D11s (AiYin) label printer
//
// 96px wide print head (12 bytes/row), 203 DPI.
// Default label: 30x14 mm. Also supports 50x14 mm (--label-size 50).
// Orientation:
//   landscape (default) - long edge of label is the feed direction
//   portrait (--portrait) - short edge (14 mm) is the feed direction
// Protocol: PROTOCOL-fischero.md
//
// Transport: Classic Bluetooth SPP (RFCOMM) preferred.
// Falls back to BLE if SPP is unavailable.
// Use --spp or --ble to force a specific transport.
// ------------------------------------------------------------
class FischeroPrinter : public Printer {
public:
    enum class Transport { Auto, Spp, Ble };

    explicit FischeroPrinter(Transport transport = Transport::Auto);
    ~FischeroPrinter() override;

    bool connect(const std::string &name_or_addr, int scan_timeout_s = 15) override;
    void disconnect() override;
    bool is_connected() const override;

    // ---- Info / status ----
    std::string get_status() override;
    void print_info() override;
    // Query status + battery via BLE (works even when connected via SPP)
    std::string get_status_ble();

    // ---- Extended info ----
    std::string get_model();
    std::string get_firmware();
    std::string get_serial();
    std::string get_boot_version();
    std::pair<int,int> get_battery();   // {status_byte, percent}
    uint8_t get_status_byte();
    std::string get_all_info();

    // ---- Config ----
    void set_density(int level) override;
    void set_paper_type(int type);
    void set_shutdown_time(int minutes);
    void set_speed(int speed);
    void factory_reset();

    // Set the active label length in mm (e.g. 30 or 50).
    // Used by print_image to size the rasterised page and centre
    // content along the feed axis. 0 = no padding (use img height as-is).
    void set_label_length_mm(int mm) { label_len_mm_ = mm; }
    int  label_length_mm() const     { return label_len_mm_; }

    // ---- Feed / Print ----
    void feed(int dots) override;
    bool can_form_feed() const override { return true; }
    void form_feed();
    void print_image(const BinImage &img, int copies = 1) override;

    const char *printer_name() const override { return "Fischero D11s (AiYin)"; }
    int print_width() const override { return 96; }

private:
    Transport transport_mode_;
    bool      using_spp_ = false;
    int       label_len_mm_ = 30;   // 30 or 50 mm typically; 0 = no padding

    // SPP transport (Classic BT)
    SppTransport spp_;

    // BLE transport (from Printer base class: ble_)
    // Active BLE service/char UUIDs
    std::string active_svc_;
    std::string active_write_;
    std::string active_notify_;

    // BLE MAC (C9:...) — the advertising/LE address.
    // Used to connect BLE for --status (battery, device info).
    std::string ble_mac_;

    // Common send interface - dispatches to SPP or BLE
    void send_raw(const Bytes &data);
    Bytes send_command(const Bytes &cmd, int timeout_ms = 2000);
    std::string send_command_str(const Bytes &cmd, int timeout_ms = 2000);

    // BLE-specific
    void setup_ble_notify();
    bool connect_ble(const std::string &name_or_addr, int scan_timeout_s);
};

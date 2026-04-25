#pragma once
#include "printer.h"
#include <string>
#include <cstdint>

// ------------------------------------------------------------
// CatPrinter - GT01 / GB01 / GB02 / GB03 / MX10 / MX05 / MX06 / MX08
//
// 384px wide, 200 DPI, continuous 57mm roll paper.
// Protocol identical across all models; only the BLE advertisement
// service UUID differs:
//   GT01/GB0x: 0xae30    MX10/MX05/MX06/MX08: 0xaf30
// Both are probed automatically on connect.
// ------------------------------------------------------------
class CatPrinter : public Printer {
public:
    CatPrinter();
    ~CatPrinter() override;

    bool connect(const std::string &name_or_addr, int scan_timeout_s = 15) override;
    void disconnect() override;
    bool is_connected() const override;

    // ---- Info / status ----
    // Note: the Cat printer protocol has no bidirectional status query.
    // get_status() and print_info() report connection-level information only.
    std::string get_status() override;
    void print_info() override;

    // ---- Config ----
    void set_density(int level) override;  // 0=light, 1=medium, 2=dark
    void set_energy(uint16_t energy);      // 0x0000 (light) .. 0xFFFF (dark)

    // ---- Feed ----
    void feed(int dots) override;

    // ---- Print ----
    void print_image(const BinImage &img, int copies = 1) override;

    const char *printer_name() const override { return "Cat Printer (GT01/GB0x/MX10)"; }
    int print_width() const override { return 384; }

    // BLE service UUIDs - both are probed, first found wins
    static constexpr const char *SVC_UUID_GT01 = "0000ae30-0000-1000-8000-00805f9b34fb";
    static constexpr const char *SVC_UUID_MX10 = "0000af30-0000-1000-8000-00805f9b34fb";
    static constexpr const char *TX_UUID       = "0000ae01-0000-1000-8000-00805f9b34fb";
    static constexpr const char *RX_UUID       = "0000ae02-0000-1000-8000-00805f9b34fb";

private:
    void send_print_sequence(const BinImage &img, uint16_t energy);

    static Bytes build_cmd(uint8_t cmd_byte, const Bytes &payload);
    static uint8_t checksum(const Bytes &data, int offset, int length);

    static Bytes cmd_get_dev_state();
    static Bytes cmd_set_quality_200dpi();
    static Bytes cmd_set_energy(uint16_t val);
    static Bytes cmd_apply_energy();
    static Bytes cmd_lattice_start();
    static Bytes cmd_lattice_end();
    static Bytes cmd_set_paper();
    static Bytes cmd_feed_paper(uint8_t amount);
    static Bytes cmd_print_row(const std::vector<bool> &row);

    void setup_notify();

    std::string active_svc_;
    uint16_t    energy_ = 0xFFFF;
};

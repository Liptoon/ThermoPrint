#pragma once
#include <string>
#include <cstdint>
#include "image.h"
#include "ble_transport.h"
#include "utils.h"

// ------------------------------------------------------------
// Abstract Printer interface
// Both FischeroPrinter and CatPrinter implement this.
// ------------------------------------------------------------
class Printer {
public:
    virtual ~Printer() = default;

    // Connect to the printer via BLE.
    virtual bool connect(const std::string &name_or_addr, int scan_timeout_s = 15) = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    // ---- Info / status queries ----

    // Return a human-readable status string (device-specific).
    virtual std::string get_status() = 0;

    // Print all available device info to stdout.
    virtual void print_info() = 0;

    // ---- Configuration ----

    // Density: 0=light, 1=medium, 2=dark (mapped per device)
    virtual void set_density(int level) = 0;

    // ---- Printing ----

    // Print a pre-loaded binary image (copies times).
    virtual void print_image(const BinImage &img, int copies = 1) = 0;

    // Feed forward by n dots/pixels.
    virtual void feed(int dots) = 0;

    // Printer name for display.
    virtual const char *printer_name() const = 0;

    // Print width in pixels.
    virtual int print_width() const = 0;
    virtual bool can_form_feed() const { return false; }

protected:
    BLETransport ble_;
};

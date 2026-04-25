#include "tui.h"
#include "scanner.h"
#include "config.h"
#include "printer.h"
#include "fischero_printer.h"
#include "cat_printer.h"
#include "text_render.h"
#include "image.h"
#include "utils.h"

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>

// ---------- terminal control ----------
class Terminal {
public:
    static void enter_alt_screen() { write("\033[?1049h"); }
    static void exit_alt_screen()  { write("\033[?1049l"); }
    static void clear()            { write("\033[2J"); }
    static void move_cursor(int r, int c) {
        std::string seq = "\033[" + std::to_string(r) + ";" + std::to_string(c) + "H";
        write(seq);
    }
    static void hide_cursor()      { write("\033[?25l"); }
    static void show_cursor()      { write("\033[?25h"); }
    static void reverse_on()       { write("\033[7m"); }
    static void reset_attr()       { write("\033[0m"); }
    static void flush()            { fflush(stdout); }
private:
    static void write(const std::string& s) { std::cout << s; }
};

static struct termios orig_termios;
static void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}
static void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

enum class Key { Up, Down, Enter, Esc, Unknown, Timeout };

static Key read_key(int timeout_ms = -1) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    struct timeval tv, *ptv = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    if (select(STDIN_FILENO + 1, &set, NULL, NULL, ptv) <= 0)
        return Key::Timeout;

    char c;
    if (read(STDIN_FILENO, &c, 1) != 1)
        return Key::Unknown;

    if (c == '\r' || c == '\n') return Key::Enter;
    if (c == 27) {
        struct timeval tv2 = {0, 1000};
        fd_set set2;
        FD_ZERO(&set2);
        FD_SET(STDIN_FILENO, &set2);
        if (select(STDIN_FILENO + 1, &set2, NULL, NULL, &tv2) > 0) {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[') {
                if (read(STDIN_FILENO, &seq[1], 1) == 1) {
                    if (seq[1] == 'A') return Key::Up;
                    if (seq[1] == 'B') return Key::Down;
                }
            }
        }
        tcflush(STDIN_FILENO, TCIFLUSH);
        return Key::Esc;
    }
    return Key::Unknown;
}

// ---------- menu drawing ----------
static void draw_menu(const std::string& title, const std::vector<std::string>& items, int selected) {
    Terminal::clear();
    Terminal::move_cursor(1, 1);
    Terminal::reverse_on();
    std::cout << title;
    Terminal::reset_attr();
    std::cout << "\r\n";
    for (size_t i = 0; i < items.size(); ++i) {
        if (static_cast<int>(i) == selected) Terminal::reverse_on();
        std::cout << "  " << items[i] << "\r\n";
        if (static_cast<int>(i) == selected) Terminal::reset_attr();
    }
    Terminal::flush();
}

static int menu_loop(const std::string& title, const std::vector<std::string>& items) {
    int selected = 0;
    while (true) {
        draw_menu(title, items, selected);
        Key k = read_key();
        switch (k) {
            case Key::Up:    if (selected > 0) --selected; break;
            case Key::Down:  if (selected < (int)items.size()-1) ++selected; break;
            case Key::Enter: return selected;
            case Key::Esc:   return -1;
            default: break;
        }
    }
}

static void flash_message(const std::string& msg) {
    Terminal::clear();
    Terminal::move_cursor(1, 1);
    std::cout << msg << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}

static std::string get_text_or_path(const std::string& action) {
    Terminal::clear();
    Terminal::move_cursor(1, 1);
    std::cout << action << "\r\n";
    std::cout << "Press Enter to type text / path, Esc to cancel." << std::flush;
    while (true) {
        Key k = read_key();
        if (k == Key::Esc) return "";
        if (k == Key::Enter) break;
    }
    disable_raw_mode();
    std::string input;
    std::cout << "\r\n" << action << ": " << std::flush;
    std::getline(std::cin, input);
    enable_raw_mode();
    return input;
}

// ---------- TUI application state ----------
struct TuiApp {
    std::unique_ptr<Printer> printer;
    Config config;
    DeviceType dev_type;
    std::string dev_mac;
    bool exit_requested = false;

    void disconnect_printer() {
        if (printer) printer->disconnect();
        printer.reset();
    }

    bool connect_device(const DeviceInfo& dev) {
        if (dev.type == DeviceType::Fischero) {
            auto *fp = new FischeroPrinter(FischeroPrinter::Transport::Auto);
            printer.reset(fp);
            std::string mac = dev.classic_mac.empty() ? dev.ble_mac : dev.classic_mac;
            if (!fp->connect(mac, 15)) {
                printer.reset();
                return false;
            }
            fp->set_label_length_mm(config.label_size_mm);
            dev_type = DeviceType::Fischero;
        } else {
            auto *cp = new CatPrinter();
            printer.reset(cp);
            if (!cp->connect(dev.ble_mac, 15)) {
                printer.reset();
                return false;
            }
            dev_type = DeviceType::Cat;
        }
        dev_mac = dev.ble_mac;
        config.last_mac = dev_mac;
        return true;
    }

    void run_scanning() {
        exit_requested = false;
        while (!exit_requested) {
            Terminal::clear();
            Terminal::move_cursor(1,1);
            std::cout << "Scanning for printers..." << std::flush;
            auto devices = scan_for_devices(10);
            if (devices.empty()) {
                std::vector<std::string> items = {"Rescan", "Exit"};
                int sel = menu_loop("No supported printers found.", items);
                if (sel == 1 || sel < 0) return;
                continue;
            }
            if (devices.size() == 1) {
                if (!connect_device(devices[0])) {
                    flash_message("Failed to connect to the printer.");
                    continue;
                }
                run_main_menu();
            } else {
                std::vector<std::string> items;
                for (auto& d : devices) {
                    std::string prefix = (d.type == DeviceType::Fischero) ? "[Fischero] " : "[Cat] ";
                    items.push_back(prefix + d.name + "  (" + d.ble_mac + ")");
                }
                items.push_back("Rescan");
                items.push_back("Exit");
                int sel = menu_loop("Select a printer", items);
                if (sel < 0 || sel == (int)items.size()-1) return;
                if (sel == (int)items.size()-2) continue;
                if (sel >= 0 && sel < (int)devices.size()) {
                    if (!connect_device(devices[sel])) {
                        flash_message("Connection failed.");
                        continue;
                    }
                    run_main_menu();
                }
            }
        }
    }

    void run_main_menu() {
        while (printer && printer->is_connected() && !exit_requested) {
            std::vector<std::string> items = {
                "Print Text",
                "Print Image",
                "Feed...",
                "Settings",
                "Status",
                "Disconnect",
                "Exit"
            };
            std::string title = std::string("Main Menu - ") + printer->printer_name();
            int sel = menu_loop(title, items);
            switch (sel) {
                case 0: run_print_text(); break;
                case 1: run_print_image(); break;
                case 2: run_feed_menu(); break;
                case 3: run_settings(); break;
                case 4: run_status(); break;
                case 5:
                    disconnect_printer();
                    save_config(config);
                    return;
                case 6:
                    exit_requested = true;
                    disconnect_printer();
                    save_config(config);
                    return;
                case -1: break;
                default: break;
            }
        }
    }

    void run_print_text() {
        std::string input = get_text_or_path("Print Text");
        if (input.empty()) return;
        try {
            std::string text;
            std::ifstream test(input);
            if (test.good()) {
                text = load_text_file(input);
            } else {
                text = input;
            }
            int pw = printer->print_width();
            TextOptions opts;
            opts.font_size    = config.font_size;
            opts.margin_x     = config.margin_x;
            opts.margin_y     = config.margin_y;
            opts.line_spacing = 1;
            opts.word_wrap    = true;
            if (config.alignment == "center")       opts.alignment = TextAlign::Center;
            else if (config.alignment == "right")   opts.alignment = TextAlign::Right;
            else                                    opts.alignment = TextAlign::Left;
            BinImage img = render_text(text, pw, opts);
            if (dev_type == DeviceType::Fischero && !config.portrait)
                img = rotate_90cw(img);
            printer->print_image(img, 1);
            flash_message("Text printed.");
        } catch (const std::exception& e) {
            flash_message(std::string("Error: ") + e.what());
        }
    }

    void run_print_image() {
        std::string path = get_text_or_path("Print Image (enter path)");
        if (path.empty()) return;
        try {
            Dither d = dither_from_string(config.dither);
            BinImage img = load_image(path, printer->print_width(), d);
            if (dev_type == DeviceType::Fischero && !config.portrait)
                img = rotate_90cw(img);
            printer->print_image(img, 1);
            flash_message("Image printed.");
        } catch (const std::exception& e) {
            flash_message(std::string("Error: ") + e.what());
        }
    }

    void run_feed_menu() {
        std::vector<int> feed_amounts = {10, 20, 40, 60, 80, 100};
        while (true) {
            std::vector<std::string> items;
            for (int n : feed_amounts)
                items.push_back("Feed " + std::to_string(n) + " dots");
            if (printer->can_form_feed())
                items.push_back("Form-feed to next label");
            items.push_back("Back");
            int sel = menu_loop("Feed", items);
            if (sel < 0 || sel == (int)items.size()-1) return;

            if (sel >= 0 && sel < (int)feed_amounts.size()) {
                try {
                    printer->feed(feed_amounts[sel]);
                    flash_message("Fed " + std::to_string(feed_amounts[sel]) + " dots.");
                } catch (const std::exception& e) {
                    flash_message(std::string("Error: ") + e.what());
                }
            } else if (printer->can_form_feed() && sel == (int)feed_amounts.size()) {
                auto *fp = dynamic_cast<FischeroPrinter*>(printer.get());
                if (fp) {
                    try {
                        fp->form_feed();
                        flash_message("Form feed done.");
                    } catch (const std::exception& e) {
                        flash_message(std::string("Error: ") + e.what());
                    }
                }
            }
        }
    }

    void run_settings() {
        while (true) {
            std::vector<std::string> items;
            items.push_back("Dither: " + config.dither);
            items.push_back("Density: " + std::to_string(config.density));
            items.push_back("Font size: " + std::to_string(config.font_size));
            items.push_back("Alignment: " + config.alignment);
            items.push_back("Margin X: " + std::to_string(config.margin_x));
            items.push_back("Margin Y: " + std::to_string(config.margin_y));
            if (dev_type == DeviceType::Fischero) {
                items.push_back("Orientation: " + std::string(config.portrait ? "Portrait" : "Landscape"));
                items.push_back("Label length: " + std::to_string(config.label_size_mm) + " mm");
            }
            items.push_back("Back");
            int sel = menu_loop("Settings", items);
            if (sel < 0 || sel == (int)items.size()-1) break;

            if (sel == 0) {
                std::vector<std::string> algs = {"floyd-steinberg", "atkinson", "mean-threshold", "halftone", "none"};
                int a = menu_loop("Dither", algs);
                if (a >= 0 && a < (int)algs.size()) config.dither = algs[a];
            } else if (sel == 1) {
                std::vector<std::string> dens = {"0 – Light", "1 – Medium", "2 – Dark"};
                int d = menu_loop("Density", dens);
                if (d >= 0 && d <= 2) config.density = d;
            } else if (sel == 2) {
                std::vector<std::string> sizes = {"1", "2", "3", "4", "5"};
                int s = menu_loop("Font size", sizes);
                if (s >= 0 && s < (int)sizes.size())
                    config.font_size = s + 1;
            } else if (sel == 3) {
                std::vector<std::string> aligns = {"Left", "Center", "Right"};
                int al = menu_loop("Text alignment", aligns);
                if (al == 0) config.alignment = "left";
                else if (al == 1) config.alignment = "center";
                else if (al == 2) config.alignment = "right";
            } else if (sel == 4) {
                // Margin X: offer common small values
                std::vector<std::string> margins = {"0","1","2","3","4","5","8","10","12","16"};
                int mx = menu_loop("Margin X (pixels)", margins);
                if (mx >= 0 && mx < (int)margins.size()) config.margin_x = atoi(margins[mx].c_str());
            } else if (sel == 5) {
                std::vector<std::string> margins = {"0","1","2","3","4","5","8","10","12","16"};
                int my = menu_loop("Margin Y (pixels)", margins);
                if (my >= 0 && my < (int)margins.size()) config.margin_y = atoi(margins[my].c_str());
            } else if (sel == 6 && dev_type == DeviceType::Fischero) {
                config.portrait = !config.portrait;
            } else if (sel == 7 && dev_type == DeviceType::Fischero) {
                std::vector<std::string> lens = {"30 mm", "50 mm"};
                int l = menu_loop("Label length", lens);
                if (l == 0) config.label_size_mm = 30;
                else if (l == 1) config.label_size_mm = 50;
                if (auto* fp = dynamic_cast<FischeroPrinter*>(printer.get()))
                    fp->set_label_length_mm(config.label_size_mm);
            }
            save_config(config);
        }
        if (printer) printer->set_density(config.density);
    }

    void run_status() {
        std::vector<std::string> lines;
        if (dev_type == DeviceType::Fischero) {
            auto* fp = dynamic_cast<FischeroPrinter*>(printer.get());
            if (fp) {
                lines.push_back("Model: " + fp->get_model());
                lines.push_back("Firmware: " + fp->get_firmware());
                lines.push_back("Serial: " + fp->get_serial());
                auto [bat_stat, bat_pct] = fp->get_battery();
                lines.push_back("Battery: " + std::to_string(bat_pct) + "%");
                lines.push_back("Status: " + fp->get_status());
            }
        } else {
            lines.push_back(printer->get_status());
        }
        Terminal::clear();
        Terminal::move_cursor(1, 1);
        for (const auto& line : lines)
            std::cout << line << "\r\n";
        Terminal::flush();
        while (true) {
            Key k = read_key();
            if (k == Key::Esc) break;
        }
    }
};

void run_tui() {
    enable_raw_mode();
    Terminal::enter_alt_screen();
    Terminal::hide_cursor();

    TuiApp app;
    app.config = load_config();
    app.run_scanning();

    Terminal::show_cursor();
    Terminal::exit_alt_screen();
    disable_raw_mode();
}
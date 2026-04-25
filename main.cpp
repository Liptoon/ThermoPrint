#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <memory>

#include "utils.h"
#include "image.h"
#include "text_render.h"
#include "printer.h"
#include "fischero_printer.h"
#include "cat_printer.h"
#include "tui.h"                    // <-- TUI entry point

bool g_verbose = false;

// ------------------------------------------------------------
// Usage
// ------------------------------------------------------------
static void print_usage(const char *prog)
{
    fprintf(stdout,
        "Usage: %s --fischero|--cat [OPTIONS]\n"
        "\n"
        "Thermal printer driver.\n"
        "  Fischero D11s  14 mm wide labels (30 or 50 mm long), 96px, 203 DPI  (Classic BT SPP)\n"
        "  Cat GT01/GB01/GB02/GB03/YT01/MX05/MX06/MX08/MX10/MXTP\n"
        "                 57mm roll paper, 384px, 200 DPI  (BLE)\n"
        "\n"
        "DEVICE\n"
        "  --fischero              Fischero D11s label printer\n"
        "  --cat                   Cat roll printer (any supported model)\n"
        "  --device NAME|MAC       Specific device name or MAC address\n"
        "                          Fischero: scans for FICHERO* if omitted\n"
        "                          Cat:      scans for known model names if omitted\n"
        "  --scan-timeout N        Scan timeout in seconds (default: 15)\n"
        "\n"
        "ACTIONS  (can combine; executed in listed order)\n"
        "  --info                  Full device info\n"
        "  --status                Status + battery (Fischero queries via BLE)\n"
        "  --text TEXT             Print text from command line\n"
        "  --text-file FILE        Print text from file (LF/CRLF supported)\n"
        "  --print FILE            Print image (PNG JPG BMP etc.)\n"
        "  --feed N                Feed N dots of paper\n"
        "  --form-feed             Advance to next label gap (Fischero only)\n"
        "\n"
        "TEXT  Font: Fake_Receipt.otf, Latin/Polish U+0020-U+017E\n"
        "  --font-size 1-5         Glyph size.\n"
        "    Fischero (rendered to 96px head, then rotated for landscape):\n"
        "      1: 5x10px   2: 8x14px*   3: 11x19px   4: 14x24px   5: 19x34px\n"
        "    Cat roll (384px): 1=8x14 46ch  2=11x19 33ch  3=14x24 26ch*  4=19x34  5=22x38\n"
        "  --margin N              Pixel margin all sides (default: 2)\n"
        "  --line-spacing N        Extra blank rows between lines (default: 1)\n"
        "  --no-word-wrap          Truncate long lines instead of wrapping\n"
        "\n"
        "PRINT\n"
        "  --dither ALGO           floyd-steinberg* atkinson mean-threshold halftone none\n"
        "  --copies N              Number of copies (default: 1)\n"
        "  --density 0-2           0=light 1=medium* 2=dark\n"
        "  --portrait              Fischero: disable rotation (text reads along feed direction)\n"
        "                          Default: landscape (text reads across the long label edge)\n"
        "  --label-size 30|50      Fischero label long edge in mm (default: 30)\n"
        "                          Short edge is always 14 mm. Print is centred on the label.\n"
        "\n"
        "FISCHERO ONLY\n"
        "  --spp / --ble           Force transport (default auto: SPP preferred)\n"
        "  --paper-type 0-2        0=gap/label* 1=black-mark 2=continuous\n"
        "  --shutdown-time N       Auto-shutdown in minutes\n"
        "  --factory-reset         Restore factory defaults\n"
        "\n"
        "CAT ROLL ONLY\n"
        "  --energy 0xNNNN         Thermal energy 0x0000-0xFFFF (default 0xFFFF)\n"
        "\n"
        "  -v / --verbose          Protocol hex dumps and BLE details\n"
        "  -h / --help             This help\n"
        "\n"
        "EXAMPLES\n"
        "  %s --fischero --text 'FRAGILE' --font-size 3 --copies 3\n"
        "  %s --fischero --text 'Price: 9.99' --font-size 2 --density 2\n"
        "  %s --fischero --label-size 50 --text 'Long label content here'\n"
        "  %s --fischero --label-size 50 --portrait --print logo.png\n"
        "  %s --fischero --print label.png --dither atkinson\n"
        "  %s --fischero --status\n"
        "  %s --fischero --device C8:48:8A:42:0F:AC --text 'hello'\n"
        "  %s --cat --text-file receipt.txt --font-size 2\n"
        "  %s --cat --print photo.png\n"
        "\n",
        prog,
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}


// ------------------------------------------------------------
// Argument struct
// ------------------------------------------------------------
struct Args {
    bool use_fischero = false;
    bool use_cat      = false;
    bool force_spp    = false;   // --spp: force Classic BT for Fischero
    bool force_ble    = false;   // --ble: force BLE for Fischero

    std::string device_name;
    int scan_timeout = 15;

    bool        do_info      = false;
    bool        do_status    = false;
    bool        do_form_feed = false;
    std::string print_file;
    std::string text_inline;
    std::string text_file;
    int         feed_dots    = 0;

    int    copies   = 1;
    Dither dither   = Dither::FloydSteinberg;
    int    density  = 1;

    int  font_size    = -1;  // -1 = use per-device default
    int  margin       = 2;
    int  line_spacing = 1;
    bool word_wrap    = true;

    int  paper_type    = -1;
    int  shutdown_time = -1;
    int  set_speed     = -1;
    bool factory_reset = false;
    bool portrait      = false;  // Fischero: default landscape, --portrait for portrait
    int  label_size_mm = 30;     // Fischero: 30 (default) or 50 mm long edge

    int energy = -1;  // -1 = use density mapping
};

// ------------------------------------------------------------
// Parse arguments
// ------------------------------------------------------------
static bool parse_args(int argc, char **argv, Args &args)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        auto next = [&](const char *flag) -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing argument for %s\n", flag);
                return nullptr;
            }
            return argv[++i];
        };

        if (strcmp(a, "--fischero") == 0) {
            args.use_fischero = true;
        } else if (strcmp(a, "--cat") == 0) {
            args.use_cat = true;
        } else if (strcmp(a, "--spp") == 0) {
            args.force_spp = true;
        } else if (strcmp(a, "--ble") == 0) {
            args.force_ble = true;
        } else if (strcmp(a, "--device") == 0) {
            const char *v = next("--device"); if (!v) return false;
            args.device_name = v;
        } else if (strcmp(a, "--scan-timeout") == 0) {
            const char *v = next("--scan-timeout"); if (!v) return false;
            args.scan_timeout = atoi(v);
        } else if (strcmp(a, "--info") == 0) {
            args.do_info = true;
        } else if (strcmp(a, "--status") == 0) {
            args.do_status = true;
        } else if (strcmp(a, "--print") == 0) {
            const char *v = next("--print"); if (!v) return false;
            args.print_file = v;
        } else if (strcmp(a, "--text") == 0) {
            const char *v = next("--text"); if (!v) return false;
            args.text_inline = v;
        } else if (strcmp(a, "--text-file") == 0) {
            const char *v = next("--text-file"); if (!v) return false;
            args.text_file = v;
        } else if (strcmp(a, "--font-size") == 0) {
            const char *v = next("--font-size"); if (!v) return false;
            int fs = atoi(v);
            if (fs < 1 || fs > 5) {
                fprintf(stderr, "Error: --font-size must be 1-5 (got '%s')\n", v);
                return false;
            }
            args.font_size = fs;
        } else if (strcmp(a, "--margin") == 0) {
            const char *v = next("--margin"); if (!v) return false;
            args.margin = atoi(v);
        } else if (strcmp(a, "--line-spacing") == 0) {
            const char *v = next("--line-spacing"); if (!v) return false;
            args.line_spacing = atoi(v);
        } else if (strcmp(a, "--no-word-wrap") == 0) {
            args.word_wrap = false;
        } else if (strcmp(a, "--feed") == 0) {
            const char *v = next("--feed"); if (!v) return false;
            args.feed_dots = atoi(v);
        } else if (strcmp(a, "--form-feed") == 0) {
            args.do_form_feed = true;
        } else if (strcmp(a, "--copies") == 0) {
            const char *v = next("--copies"); if (!v) return false;
            args.copies = atoi(v);
        } else if (strcmp(a, "--dither") == 0) {
            const char *v = next("--dither"); if (!v) return false;
            try { args.dither = dither_from_string(v); }
            catch (const std::exception &e) {
                fprintf(stderr, "Error: %s\n", e.what());
                return false;
            }
        } else if (strcmp(a, "--density") == 0) {
            const char *v = next("--density"); if (!v) return false;
            args.density = atoi(v);
        } else if (strcmp(a, "--paper-type") == 0) {
            const char *v = next("--paper-type"); if (!v) return false;
            args.paper_type = atoi(v);
        } else if (strcmp(a, "--shutdown-time") == 0) {
            const char *v = next("--shutdown-time"); if (!v) return false;
            args.shutdown_time = atoi(v);
        } else if (strcmp(a, "--set-speed") == 0) {
            const char *v = next("--set-speed"); if (!v) return false;
            args.set_speed = atoi(v);
        } else if (strcmp(a, "--factory-reset") == 0) {
            args.factory_reset = true;
        } else if (strcmp(a, "--portrait") == 0) {
            args.portrait = true;
        } else if (strcmp(a, "--label-size") == 0) {
            const char *v = next("--label-size"); if (!v) return false;
            int sz = atoi(v);
            if (sz != 30 && sz != 50) {
                fprintf(stderr,
                    "Error: --label-size must be 30 or 50 (got '%s')\n", v);
                return false;
            }
            args.label_size_mm = sz;
        } else if (strcmp(a, "--energy") == 0) {
            const char *v = next("--energy"); if (!v) return false;
            args.energy = (int)strtol(v, nullptr, 0);
        } else if (strcmp(a, "--verbose") == 0 || strcmp(a, "-v") == 0) {
            g_verbose = true;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (a[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", a);
            return false;
        } else {
            if (args.print_file.empty()) {
                args.print_file = a;
            } else {
                fprintf(stderr, "Unexpected argument: %s\n", a);
                return false;
            }
        }
    }
    return true;
}

// ------------------------------------------------------------
// main
// ------------------------------------------------------------
int main(int argc, char **argv)
{
    Args args;
    if (!parse_args(argc, argv, args)) {
        fprintf(stderr, "Run '%s --help' for usage.\n", argv[0]);
        return 1;
    }

    // If no action requested, launch interactive TUI and exit
    bool any_action = args.do_info || args.do_status || !args.print_file.empty() ||
                      !args.text_inline.empty() || !args.text_file.empty() ||
                      args.feed_dots > 0 || args.do_form_feed ||
                      args.paper_type >= 0 || args.shutdown_time >= 0 ||
                      args.set_speed >= 0 || args.factory_reset;

    if (!any_action) {
        run_tui();
        return 0;
    }

    // Validate device selection for CLI mode
    if (!args.use_fischero && !args.use_cat) {
        fprintf(stderr, "Error: Specify --fischero or --cat\n");
        return 1;
    }
    if (args.use_fischero && args.use_cat) {
        fprintf(stderr, "Error: Specify only one of --fischero or --cat\n");
        return 1;
    }

    // Default device name for Fischero; Cat uses service-UUID auto-discovery
    if (args.device_name.empty() && args.use_fischero)
        args.device_name = "FICHERO";

    // Per-device default font size
    if (args.font_size < 0)
        args.font_size = args.use_fischero ? 2 : 3;

    // Create printer
    std::unique_ptr<Printer> printer;
    if (args.use_fischero) {
        auto mode = FischeroPrinter::Transport::Auto;
        if (args.force_spp) mode = FischeroPrinter::Transport::Spp;
        if (args.force_ble) mode = FischeroPrinter::Transport::Ble;
        printer = std::make_unique<FischeroPrinter>(mode);
    } else {
        printer = std::make_unique<CatPrinter>();
    }

    LOG_INFO("Device: %s", printer->printer_name());
    LOG_INFO("Connecting to '%s'...", args.device_name.c_str());

    if (!printer->connect(args.device_name, args.scan_timeout)) {
        LOG_ERROR("Failed to connect to '%s'", args.device_name.c_str());
        return 1;
    }

    int exit_code = 0;

    try {
        // ---- Info ----
        if (args.do_info)
            printer->print_info();

        // ---- Status ----
        if (args.do_status) {
            if (args.use_fischero) {
                auto *fp = static_cast<FischeroPrinter *>(printer.get());
                std::string s = fp->get_status_ble();
                LOG_INFO("Status: %s", s.c_str());
            } else {
                LOG_INFO("Status: %s", printer->get_status().c_str());
            }
        }

        // ---- Fischero config ----
        if (args.use_fischero) {
            auto *fp = static_cast<FischeroPrinter *>(printer.get());
            if (args.paper_type >= 0)  fp->set_paper_type(args.paper_type);
            if (args.shutdown_time >= 0) fp->set_shutdown_time(args.shutdown_time);
            if (args.set_speed >= 0)   fp->set_speed(args.set_speed);
            if (args.factory_reset) {
                LOG_WARN("Performing factory reset...");
                fp->factory_reset();
            }
            printer->set_density(args.density);
            fp->set_label_length_mm(args.label_size_mm);
            LOG_INFO("Label: %d x 14 mm  [%s]", args.label_size_mm,
                     args.portrait ? "portrait" : "landscape");
        }

        // ---- Cat roll config ----
        if (args.use_cat) {
            auto *cp = static_cast<CatPrinter *>(printer.get());
            if (args.energy >= 0)
                cp->set_energy((uint16_t)args.energy);
            else
                cp->set_density(args.density);
        }

        // ---- Print image ----
        if (!args.print_file.empty()) {
            int pw = printer->print_width();
            bool do_rotate = args.use_fischero && !args.portrait;
            int render_w = pw;
            LOG_INFO("Loading image '%s' (%dpx%s)...", args.print_file.c_str(),
                     render_w, args.use_fischero
                                ? (do_rotate ? " landscape" : " portrait")
                                : "");
            BinImage img = load_image(args.print_file, render_w, args.dither);
            if (do_rotate) img = rotate_90cw(img);
            LOG_INFO("Image: %dx%d px, dither=%s",
                     img.width, img.height, dither_to_string(args.dither));
            printer->print_image(img, args.copies);
        }

        // ---- Print text ----
        if (!args.text_inline.empty() || !args.text_file.empty()) {
            std::string text;
            if (!args.text_file.empty()) {
                LOG_INFO("Loading text from '%s'...", args.text_file.c_str());
                text = load_text_file(args.text_file);
            } else {
                text = args.text_inline;
            }

            bool do_rotate = args.use_fischero && !args.portrait;
            int pw = printer->print_width();
            int render_w = pw;

            int gw = font_glyph_w(args.font_size, render_w);
            int gh = font_glyph_h(args.font_size, render_w);
            int chars_per_line = (render_w - 2 * args.margin) / gw;
            LOG_INFO("Rendering text: font-size=%d (%dx%dpx), %d chars/line [%s]",
                     args.font_size, gw, gh, chars_per_line,
                     args.use_fischero
                       ? (do_rotate ? "landscape" : "portrait")
                       : "normal");

            TextOptions topts;
            topts.font_size    = args.font_size;
            topts.margin_x     = args.margin;
            topts.margin_y     = args.margin;
            topts.line_spacing = args.line_spacing;
            topts.word_wrap    = args.word_wrap;

            BinImage img = render_text(text, render_w, topts);
            if (do_rotate) img = rotate_90cw(img);
            LOG_INFO("Print image: %dx%d px", img.width, img.height);
            printer->print_image(img, args.copies);
        }

        // ---- Feed ----
        if (args.feed_dots > 0) {
            LOG_INFO("Feeding %d dots...", args.feed_dots);
            printer->feed(args.feed_dots);
        }

        // ---- Form feed ----
        if (args.do_form_feed) {
            if (args.use_fischero)
                static_cast<FischeroPrinter *>(printer.get())->form_feed();
            else
                LOG_WARN("--form-feed is only supported on the Fischero printer");
        }

    } catch (const std::exception &e) {
        LOG_ERROR("Error: %s", e.what());
        exit_code = 1;
    }

    printer->disconnect();
    return exit_code;
}
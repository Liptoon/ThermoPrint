#pragma once
#include <string>

struct Config {
    std::string dither    = "floyd-steinberg";
    int         density   = 1;
    int         font_size = 2;
    int         label_size_mm = 30;         // Fischero only
    bool        portrait  = false;          // Fischero orientation
    std::string alignment = "left";         // "left"/"center"/"right"
    int         margin_x  = 2;
    int         margin_y  = 2;
    std::string last_mac;                   // helper, not used for auto-connect
};

Config load_config();
void   save_config(const Config& cfg);
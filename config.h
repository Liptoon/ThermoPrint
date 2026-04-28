#pragma once
#include <string>

struct Config {
    std::string dither    = "floyd-steinberg";
    int         density   = 1;
    int         font_size = 2;
    int         label_size_mm = 30;         // Fischero only
    bool        portrait  = false;          // Fischero orientation
    std::string alignment = "left";         // "left"/"center"/"right"
    int         margin_left   = 2;
    int         margin_right  = 2;
    int         margin_top    = 2;
    int         margin_bottom = 2;
    std::string last_mac;                   // helper
};

Config load_config();
void   save_config(const Config& cfg);
#pragma once
#include "image.h"
#include <string>

enum class TextAlign { Left, Center, Right };

struct TextOptions {
    int       font_size      = 2;
    int       margin_left    = 2;
    int       margin_right   = 2;
    int       margin_top     = 2;
    int       margin_bottom  = 2;
    int       line_spacing   = 1;
    bool      word_wrap      = true;
    TextAlign alignment      = TextAlign::Left;

    void set_all_margins(int m) {
        margin_left = margin_right = margin_top = margin_bottom = m;
    }
};

int font_glyph_w(int font_size, int print_width = 384);
int font_glyph_h(int font_size, int print_width = 384);
int font_size_index(int font_size, int print_width);

BinImage render_text(const std::string &text,
                     int print_width,
                     const TextOptions &opts = TextOptions{});

std::string load_text_file(const std::string &path);
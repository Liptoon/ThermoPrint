#pragma once
#include "image.h"
#include <string>

enum class TextAlign { Left, Center, Right };

// ------------------------------------------------------------
// Font sizes 1-5 mapped to pixel sizes from Fake_Receipt.otf
// All sizes available on both printers; choose based on content.
// ------------------------------------------------------------
struct TextOptions {
    int       font_size    = 2;  // 1..5
    int       margin_x     = 2;  // left/right margin in pixels
    int       margin_y     = 2;  // top/bottom margin in pixels
    int       line_spacing = 1;  // extra blank rows between lines
    bool      word_wrap    = true;
    TextAlign alignment    = TextAlign::Left;
};

// Returns glyph cell width in pixels for font size 1-5.
int font_glyph_w(int font_size, int print_width = 384);
// Returns line height in pixels for font size 1-5.
int font_glyph_h(int font_size, int print_width = 384);
// Map font sizes 1-5 to internal size indices (0-6 in font_data.h).
int font_size_index(int font_size, int print_width);

// Render UTF-8 text to a BinImage.
BinImage render_text(const std::string &text,
                     int print_width,
                     const TextOptions &opts = TextOptions{});

// Load plain text from a file (LF and CRLF supported).
std::string load_text_file(const std::string &path);
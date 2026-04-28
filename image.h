#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

struct BinImage {
    int width;
    int height;
    std::vector<std::vector<bool>> rows;

    std::vector<uint8_t> packed_row(int y) const;
    std::vector<uint8_t> packed() const;
};

enum class Dither {
    MeanThreshold,
    FloydSteinberg,
    Atkinson,
    Halftone,
    None,
};

Dither      dither_from_string(const std::string &s);
const char *dither_to_string(Dither d);

BinImage rotate_90cw(const BinImage &src);
BinImage rotate_90ccw(const BinImage &src);
BinImage center_on_width(const BinImage &src, int target_width);
BinImage center_on_height(const BinImage &src, int target_height);
BinImage load_image(const std::string &filename,
                    int print_width,
                    Dither dither);

BinImage pad_image(const BinImage &src, int left, int right, int top, int bottom);

// Load, fit inside (target_w x target_h), optionally rotate,
// and apply dither.  The result is exactly target_w x target_h pixels.
BinImage load_and_fit(const std::string &filename,
                      int target_w, int target_h,
                      Dither dither, bool rotate90);
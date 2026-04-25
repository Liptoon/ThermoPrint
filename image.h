#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

// A 1-bit image: rows of booleans, true = black pixel.
// Width is always padded to a multiple of 8.
struct BinImage {
    int width;   // actual pixel width (multiple of 8 after construction)
    int height;
    // rows[y][x] = true means black (heater on)
    std::vector<std::vector<bool>> rows;

    // Return row y as packed bytes, MSB = leftmost pixel.
    std::vector<uint8_t> packed_row(int y) const;

    // Return the entire image as packed bytes (all rows concatenated).
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

// Rotate 90° clockwise:  input W×H  → output H×W (width padded to multiple of 8)
BinImage rotate_90cw(const BinImage &src);

// Rotate 90° counter-clockwise
BinImage rotate_90ccw(const BinImage &src);

// Pad `src` horizontally so the result is exactly `target_width` px wide
// (rounded up to a multiple of 8). The original pixels are centred.
// If src is wider than target_width, it is centre-cropped instead.
BinImage center_on_width(const BinImage &src, int target_width);

// Pad `src` vertically so the result has exactly `target_height` rows.
// The original rows are centred. If src is taller, it is centre-cropped.
BinImage center_on_height(const BinImage &src, int target_height);

// Load an image from file, resize to print_width, apply dithering.
// Throws std::runtime_error on failure.
BinImage load_image(const std::string &filename,
                    int print_width,
                    Dither dither);

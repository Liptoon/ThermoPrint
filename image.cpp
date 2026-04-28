#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize2.h"
#include "image.h"
#include "utils.h"

#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <cstring>

// ------------------------------------------------------------
// BinImage methods
// ------------------------------------------------------------
std::vector<uint8_t> BinImage::packed_row(int y) const
{
    int bytes = width / 8;
    std::vector<uint8_t> out(bytes, 0);
    for (int x = 0; x < width; x++) {
        if (rows[y][x]) {
            out[x / 8] |= (0x80 >> (x % 8));
        }
    }
    return out;
}

std::vector<uint8_t> BinImage::packed() const
{
    std::vector<uint8_t> out;
    out.reserve(height * (width / 8));
    for (int y = 0; y < height; y++) {
        auto row = packed_row(y);
        out.insert(out.end(), row.begin(), row.end());
    }
    return out;
}

// ------------------------------------------------------------
// Rotation
// ------------------------------------------------------------
BinImage rotate_90cw(const BinImage &src)
{
    int out_w = (src.height + 7) & ~7;
    int out_h = src.width;
    BinImage out;
    out.width  = out_w;
    out.height = out_h;
    out.rows.assign(out_h, std::vector<bool>(out_w, false));
    for (int y = 0; y < src.height; y++) {
        for (int x = 0; x < src.width; x++) {
            int dx = src.height - 1 - y;
            int dy = x;
            if (dx < out_w && dy < out_h)
                out.rows[dy][dx] = src.rows[y][x];
        }
    }
    return out;
}

BinImage rotate_90ccw(const BinImage &src)
{
    int out_w = (src.height + 7) & ~7;
    int out_h = src.width;
    BinImage out;
    out.width  = out_w;
    out.height = out_h;
    out.rows.assign(out_h, std::vector<bool>(out_w, false));
    for (int y = 0; y < src.height; y++) {
        for (int x = 0; x < src.width; x++) {
            int dx = y;
            int dy = src.width - 1 - x;
            if (dx < out_w && dy < out_h)
                out.rows[dy][dx] = src.rows[y][x];
        }
    }
    return out;
}

// ------------------------------------------------------------
// Centering / padding helpers
// ------------------------------------------------------------
BinImage center_on_width(const BinImage &src, int target_width)
{
    int tw = (target_width + 7) & ~7;
    BinImage out;
    out.width  = tw;
    out.height = src.height;
    out.rows.assign(src.height, std::vector<bool>(tw, false));

    if (src.width <= tw) {
        int off = (tw - src.width) / 2;
        for (int y = 0; y < src.height; y++)
            for (int x = 0; x < src.width; x++)
                out.rows[y][off + x] = src.rows[y][x];
    } else {
        int off = (src.width - tw) / 2;
        for (int y = 0; y < src.height; y++)
            for (int x = 0; x < tw; x++)
                out.rows[y][x] = src.rows[y][off + x];
    }
    return out;
}

BinImage center_on_height(const BinImage &src, int target_height)
{
    BinImage out;
    out.width  = src.width;
    out.height = target_height;
    out.rows.assign(target_height, std::vector<bool>(src.width, false));

    if (src.height <= target_height) {
        int off = (target_height - src.height) / 2;
        for (int y = 0; y < src.height; y++)
            out.rows[off + y] = src.rows[y];
    } else {
        int off = (src.height - target_height) / 2;
        for (int y = 0; y < target_height; y++)
            out.rows[y] = src.rows[off + y];
    }
    return out;
}

// ------------------------------------------------------------
// Dither enum helpers
// ------------------------------------------------------------
Dither dither_from_string(const std::string &s)
{
    if (s == "mean-threshold")   return Dither::MeanThreshold;
    if (s == "floyd-steinberg")  return Dither::FloydSteinberg;
    if (s == "atkinson")         return Dither::Atkinson;
    if (s == "halftone")         return Dither::Halftone;
    if (s == "none")             return Dither::None;
    throw std::runtime_error("Unknown dither algorithm: " + s);
}

const char *dither_to_string(Dither d)
{
    switch (d) {
        case Dither::MeanThreshold:  return "mean-threshold";
        case Dither::FloydSteinberg: return "floyd-steinberg";
        case Dither::Atkinson:       return "atkinson";
        case Dither::Halftone:       return "halftone";
        case Dither::None:           return "none";
    }
    return "unknown";
}

// ------------------------------------------------------------
// Internal: grayscale pixel buffer (float [0,255])
// ------------------------------------------------------------
struct GrayBuf {
    int w, h;
    std::vector<float> px;

    float &at(int y, int x)       { return px[y * w + x]; }
    float  at(int y, int x) const { return px[y * w + x]; }

    void clamp_pixel(int y, int x, float delta) {
        if (y < 0 || y >= h || x < 0 || x >= w) return;
        at(y, x) = std::max(0.f, std::min(255.f, at(y, x) + delta));
    }
};

// ------------------------------------------------------------
// Dithering algorithms
// ------------------------------------------------------------
static void apply_floyd_steinberg(GrayBuf &buf)
{
    for (int y = 0; y < buf.h; y++) {
        for (int x = 0; x < buf.w; x++) {
            float old_val = buf.at(y, x);
            float new_val = (old_val > 127.f) ? 255.f : 0.f;
            float err     = old_val - new_val;
            buf.at(y, x)  = new_val;
            buf.clamp_pixel(y,     x + 1,  err * 7.f / 16.f);
            buf.clamp_pixel(y + 1, x - 1,  err * 3.f / 16.f);
            buf.clamp_pixel(y + 1, x,      err * 5.f / 16.f);
            buf.clamp_pixel(y + 1, x + 1,  err * 1.f / 16.f);
        }
    }
}

static void apply_atkinson(GrayBuf &buf)
{
    for (int y = 0; y < buf.h; y++) {
        for (int x = 0; x < buf.w; x++) {
            float old_val = buf.at(y, x);
            float new_val = (old_val > 127.f) ? 255.f : 0.f;
            float err     = old_val - new_val;
            buf.at(y, x)  = new_val;
            buf.clamp_pixel(y,     x + 1,  err / 8.f);
            buf.clamp_pixel(y,     x + 2,  err / 8.f);
            buf.clamp_pixel(y + 1, x - 1,  err / 8.f);
            buf.clamp_pixel(y + 1, x,      err / 8.f);
            buf.clamp_pixel(y + 1, x + 1,  err / 8.f);
            buf.clamp_pixel(y + 2, x,      err / 8.f);
        }
    }
}

static void apply_mean_threshold(GrayBuf &buf)
{
    float sum = 0;
    for (float v : buf.px) sum += v;
    float mean = sum / (float)buf.px.size();
    for (float &v : buf.px)
        v = (v > mean) ? 255.f : 0.f;
}

static void apply_halftone(GrayBuf &buf)
{
    static const int bayer4[4][4] = {
        {  0, 128,  32, 160},
        {192,  64, 224,  96},
        { 48, 176,  16, 144},
        {240, 112, 208,  80},
    };
    for (int y = 0; y < buf.h; y++) {
        for (int x = 0; x < buf.w; x++) {
            float threshold = (float)bayer4[y % 4][x % 4];
            buf.at(y, x) = (buf.at(y, x) > threshold) ? 255.f : 0.f;
        }
    }
}

// ------------------------------------------------------------
// load_image
// ------------------------------------------------------------
BinImage load_image(const std::string &filename, int print_width, Dither dither)
{
    int orig_w, orig_h, channels;
    unsigned char *raw = stbi_load(filename.c_str(), &orig_w, &orig_h, &channels, 1);
    if (!raw) {
        throw std::runtime_error(
            std::string("Failed to load image: ") + stbi_failure_reason());
    }

    LOG_VERBOSE("Loaded image: %dx%d px, %d channels", orig_w, orig_h, channels);

    int pw = (print_width + 7) & ~7;

    if (dither == Dither::None && orig_w != pw) {
        stbi_image_free(raw);
        throw std::runtime_error(
            "Dither=none requires image width to be exactly " +
            std::to_string(pw) + " px, got " + std::to_string(orig_w));
    }

    int new_h = (int)((float)orig_h * (float)pw / (float)orig_w);
    if (new_h < 1) new_h = 1;

    std::vector<unsigned char> resized(pw * new_h);
    unsigned char *result = stbir_resize_uint8_linear(raw, orig_w, orig_h, 0,
                                                       resized.data(), pw, new_h, 0,
                                                       STBIR_1CHANNEL);
    if (!result) {
        stbi_image_free(raw);
        throw std::runtime_error("Failed to resize image");
    }
    stbi_image_free(raw);

    LOG_VERBOSE("Resized to: %dx%d px", pw, new_h);

    GrayBuf buf;
    buf.w  = pw;
    buf.h  = new_h;
    buf.px.resize(pw * new_h);
    for (int i = 0; i < pw * new_h; i++)
        buf.px[i] = (float)resized[i];

    switch (dither) {
        case Dither::FloydSteinberg:
            LOG_INFO("Applying Floyd-Steinberg dithering...");
            apply_floyd_steinberg(buf);
            break;
        case Dither::Atkinson:
            LOG_INFO("Applying Atkinson dithering...");
            apply_atkinson(buf);
            break;
        case Dither::MeanThreshold:
            LOG_INFO("Applying mean-threshold binarization...");
            apply_mean_threshold(buf);
            break;
        case Dither::Halftone:
            LOG_INFO("Applying halftone (Bayer 4x4) dithering...");
            apply_halftone(buf);
            break;
        case Dither::None:
            for (float &v : buf.px)
                v = (v > 127.f) ? 255.f : 0.f;
            break;
    }

    BinImage img;
    img.width  = pw;
    img.height = new_h;
    img.rows.resize(new_h, std::vector<bool>(pw, false));

    for (int y = 0; y < new_h; y++) {
        for (int x = 0; x < pw; x++) {
            img.rows[y][x] = (buf.at(y, x) < 128.f);
        }
    }

    LOG_INFO("Image ready: %dx%d px", pw, new_h);
    return img;
}

// ------------------------------------------------------------
// pad_image – add blank margins to an existing BinImage
// ------------------------------------------------------------
BinImage pad_image(const BinImage &src, int left, int right, int top, int bottom) {
    left   = std::max(0, left);
    right  = std::max(0, right);
    top    = std::max(0, top);
    bottom = std::max(0, bottom);

    int new_w = src.width + left + right;
    int new_h = src.height + top + bottom;

    BinImage out;
    out.width  = new_w;
    out.height = new_h;
    out.rows.assign(new_h, std::vector<bool>(new_w, false));

    for (int y = 0; y < src.height; y++)
        for (int x = 0; x < src.width; x++)
            out.rows[top + y][left + x] = src.rows[y][x];

    return out;
}
// ------------------------------------------------------------
// load_and_fit – load, resize to fit, optionally rotate, centre
// ------------------------------------------------------------
BinImage load_and_fit(const std::string &filename,
                      int target_w, int target_h,
                      Dither dither, bool rotate90)
{
    int orig_w, orig_h, channels;
    unsigned char *raw = stbi_load(filename.c_str(), &orig_w, &orig_h, &channels, 1);
    if (!raw)
        throw std::runtime_error(std::string("Failed to load image: ") + stbi_failure_reason());

    // Determine the box the *original* image must fit into.
    // After rotation, width and height are swapped.
    int box_w = rotate90 ? target_h : target_w;
    int box_h = rotate90 ? target_w : target_h;

    // Scale factor to preserve aspect ratio.
    double scale_w = (double)box_w / orig_w;
    double scale_h = (double)box_h / orig_h;
    double scale   = std::min(scale_w, scale_h);
    int new_w = (int)(orig_w * scale + 0.5);
    int new_h = (int)(orig_h * scale + 0.5);
    if (new_w < 1) new_w = 1;
    if (new_h < 1) new_h = 1;

    // Resize with stb_image_resize
    std::vector<unsigned char> resized(new_w * new_h);
    unsigned char *result = stbir_resize_uint8_linear(raw, orig_w, orig_h, 0,
                                                       resized.data(), new_w, new_h, 0,
                                                       STBIR_1CHANNEL);
    stbi_image_free(raw);
    if (!result)
        throw std::runtime_error("Failed to resize image");

    // Convert to float buffer and apply dither
    GrayBuf buf;
    buf.w = new_w;
    buf.h = new_h;
    buf.px.resize(new_w * new_h);
    for (int i = 0; i < new_w * new_h; i++)
        buf.px[i] = (float)resized[i];

    switch (dither) {
        case Dither::FloydSteinberg: apply_floyd_steinberg(buf); break;
        case Dither::Atkinson:       apply_atkinson(buf);       break;
        case Dither::MeanThreshold:  apply_mean_threshold(buf); break;
        case Dither::Halftone:       apply_halftone(buf);       break;
        case Dither::None:
            for (float &v : buf.px) v = (v > 127.f) ? 255.f : 0.f;
            break;
    }

    // Build BinImage from dithered data
    BinImage img;
    img.width  = new_w;
    img.height = new_h;
    img.rows.resize(new_h, std::vector<bool>(new_w, false));
    for (int y = 0; y < new_h; y++)
        for (int x = 0; x < new_w; x++)
            img.rows[y][x] = (buf.at(y, x) < 128.f);

    // Rotate if requested
    if (rotate90)
        img = rotate_90cw(img);

    // Now centre the (possibly smaller) image inside the target rectangle
    BinImage out = center_on_width(img, target_w);
    out = center_on_height(out, target_h);
    return out;
}
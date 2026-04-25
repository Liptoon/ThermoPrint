#include "text_render.h"
#include "font_data.h"
#include "utils.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <string>
#include <algorithm>

// ------------------------------------------------------------
// Font size mapping
// ------------------------------------------------------------
static const int FISCHERO_IDX[6] = {0, 0, 1, 2, 3, 4};  // [0] unused
static const int CAT_IDX[6]      = {0, 1, 2, 3, 5, 6};  // [0] unused

int font_size_index(int font_size, int print_width) {
    if (font_size < 1) font_size = 1;
    if (font_size > 5) font_size = 5;
    if (print_width <= 96)
        return FISCHERO_IDX[font_size];
    return CAT_IDX[font_size];
}

int font_glyph_w(int font_size, int print_width) {
    return FONT_METRICS[font_size_index(font_size, print_width)].cell_w;
}

int font_glyph_h(int font_size, int print_width) {
    return FONT_METRICS[font_size_index(font_size, print_width)].cell_h;
}

int font_glyph_w(int font_size)  { return font_glyph_w(font_size, 384); }
int font_glyph_h(int font_size)  { return font_glyph_h(font_size, 384); }

// ------------------------------------------------------------
// Glyph lookup
// ------------------------------------------------------------
static const uint8_t *get_glyph(uint32_t codepoint, int size_idx) {
    const uint8_t *base  = FONT_GLYPH_TABLE[size_idx];
    int gbytes           = FONT_GLYPH_BYTES[size_idx];
    if (codepoint < FONT_CP_MIN || codepoint > FONT_CP_MAX) {
        codepoint = '?';
        if (codepoint < FONT_CP_MIN || codepoint > FONT_CP_MAX)
            return base;
    }
    return base + (int)(codepoint - FONT_CP_MIN) * gbytes;
}

// ------------------------------------------------------------
// Render one glyph
// ------------------------------------------------------------
static void render_glyph(BinImage &img, uint32_t cp, int x, int y, int size_idx) {
    const FontSizeMetrics &m = FONT_METRICS[size_idx];
    int cell_w    = m.cell_w;
    int cell_h    = m.cell_h;
    int row_bytes = (cell_w + 7) / 8;
    const uint8_t *glyph = get_glyph(cp, size_idx);
    for (int r = 0; r < cell_h; r++) {
        int dy = y + r;
        if (dy < 0 || dy >= img.height) continue;
        for (int c = 0; c < cell_w; c++) {
            int dx = x + c;
            if (dx < 0 || dx >= img.width) continue;
            int byte = glyph[r * row_bytes + c / 8];
            if ((byte >> (7 - (c % 8))) & 1)
                img.rows[dy][dx] = true;
        }
    }
}

// ------------------------------------------------------------
// UTF-8 decoder
// ------------------------------------------------------------
static uint32_t utf8_next(const char **p) {
    unsigned char c = (unsigned char)**p;
    (*p)++;
    if (c < 0x80) return c;
    int n_extra;
    uint32_t cp;
    if      ((c & 0xE0) == 0xC0) { n_extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n_extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n_extra = 3; cp = c & 0x07; }
    else return '?';
    for (int i = 0; i < n_extra; i++) {
        unsigned char next = (unsigned char)**p;
        if ((next & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (next & 0x3F);
        (*p)++;
    }
    return cp;
}

static std::vector<uint32_t> decode_utf8(const std::string &s) {
    std::vector<uint32_t> cps;
    const char *p = s.c_str();
    while (*p)
        cps.push_back(utf8_next(&p));
    return cps;
}

// ------------------------------------------------------------
// Word wrapper
// ------------------------------------------------------------
static std::vector<std::vector<uint32_t>> word_wrap(
        const std::vector<uint32_t> &cps,
        int max_chars,
        bool do_wrap) {
    std::vector<std::vector<uint32_t>> result;
    std::vector<std::vector<uint32_t>> paragraphs;
    {
        std::vector<uint32_t> line;
        for (size_t i = 0; i < cps.size(); i++) {
            uint32_t c = cps[i];
            if (c == '\r') continue;
            if (c == '\n') {
                paragraphs.push_back(line);
                line.clear();
            } else {
                line.push_back(c);
            }
        }
        paragraphs.push_back(line);
    }

    for (auto &para : paragraphs) {
        bool all_space = true;
        for (uint32_t c : para) if (c > ' ') { all_space = false; break; }
        if (all_space) {
            result.push_back({});
            continue;
        }
        if (!do_wrap || max_chars <= 0) {
            result.push_back(para);
            continue;
        }

        std::vector<std::vector<uint32_t>> words;
        std::vector<uint32_t> word;
        for (uint32_t c : para) {
            if (c == ' ' || c == '\t') {
                if (!word.empty()) { words.push_back(word); word.clear(); }
            } else {
                word.push_back(c);
            }
        }
        if (!word.empty()) words.push_back(word);

        std::vector<uint32_t> current;
        for (auto &w : words) {
            while ((int)w.size() > max_chars) {
                std::vector<uint32_t> chunk(w.begin(), w.begin() + max_chars);
                if (!current.empty()) { result.push_back(current); current.clear(); }
                result.push_back(chunk);
                w.erase(w.begin(), w.begin() + max_chars);
            }
            if (w.empty()) continue;
            if (current.empty()) {
                current = w;
            } else if ((int)(current.size() + 1 + w.size()) <= max_chars) {
                current.push_back(' ');
                current.insert(current.end(), w.begin(), w.end());
            } else {
                result.push_back(current);
                current = w;
            }
        }
        if (!current.empty()) result.push_back(current);
    }
    return result;
}

// ------------------------------------------------------------
// render_text
// ------------------------------------------------------------
BinImage render_text(const std::string &text,
                     int print_width,
                     const TextOptions &opts) {
    int pw       = (print_width + 7) & ~7;
    int size_idx = font_size_index(opts.font_size, pw);

    const FontSizeMetrics &m = FONT_METRICS[size_idx];
    int gw       = m.cell_w;
    int gh       = m.cell_h;

    // effective left/right margin from opts, clamped
    int margin_x = std::max(0, opts.margin_x);
    int margin_y = std::max(0, opts.margin_y);
    int avail_w  = pw - 2 * margin_x;
    if (avail_w < gw) avail_w = gw;
    int max_chars = avail_w / gw;
    if (max_chars < 1) max_chars = 1;

    auto codepoints = decode_utf8(text);
    auto lines      = word_wrap(codepoints, max_chars, opts.word_wrap);

    int line_h  = gh + opts.line_spacing;
    int n_lines = (int)lines.size();
    int total_h = margin_y
                + n_lines * line_h
                - (n_lines > 0 ? opts.line_spacing : 0)
                + margin_y;
    if (total_h < 1) total_h = 1;

    BinImage img;
    img.width  = pw;
    img.height = total_h;
    img.rows.assign(total_h, std::vector<bool>(pw, false));

    int y = margin_y;
    for (const auto &line : lines) {
        int line_px = (int)line.size() * gw;   // actual width of this line
        int x;
        switch (opts.alignment) {
            case TextAlign::Left:
                x = margin_x;
                break;
            case TextAlign::Center:
                x = margin_x + (avail_w - line_px) / 2;
                if (x < margin_x) x = margin_x;
                break;
            case TextAlign::Right:
                x = pw - margin_x - line_px;
                if (x < margin_x) x = margin_x;
                break;
            default:
                x = margin_x;
        }
        for (uint32_t cp : line) {
            if (x + gw > pw - margin_x) break;
            render_glyph(img, cp, x, y, size_idx);
            x += gw;
        }
        y += line_h;
    }
    return img;
}

// ------------------------------------------------------------
// load_text_file
// ------------------------------------------------------------
std::string load_text_file(const std::string &path) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot open text file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad())
        throw std::runtime_error("Error reading text file: " + path);
    return ss.str();
}
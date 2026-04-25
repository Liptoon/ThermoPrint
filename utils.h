#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <cstdio>
#include <ctime>

// ------------------------------------------------------------
// Global verbose flag
// ------------------------------------------------------------
extern bool g_verbose;

// ------------------------------------------------------------
// Logging
// ------------------------------------------------------------
#define LOG_INFO(fmt, ...)  do { \
    fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__); \
} while(0)

#define LOG_VERBOSE(fmt, ...)  do { \
    if (g_verbose) fprintf(stdout, "[VERB]  " fmt "\n", ##__VA_ARGS__); \
} while(0)

#define LOG_ERROR(fmt, ...) do { \
    fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); \
} while(0)

#define LOG_WARN(fmt, ...) do { \
    fprintf(stdout, "[WARN]  " fmt "\n", ##__VA_ARGS__); \
} while(0)

// ------------------------------------------------------------
// Byte helpers
// ------------------------------------------------------------
using Bytes = std::vector<uint8_t>;

inline Bytes make_bytes(std::initializer_list<uint8_t> il)
{
    return Bytes(il);
}

inline void append(Bytes &dst, const Bytes &src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

// Hex dump a byte vector (for verbose output)
inline std::string hex_dump(const Bytes &data, size_t max_bytes = 64)
{
    std::string out;
    char buf[8];
    size_t n = (data.size() < max_bytes) ? data.size() : max_bytes;
    for (size_t i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf), "%02X ", data[i]);
        out += buf;
    }
    if (data.size() > max_bytes)
        out += "...";
    return out;
}

inline std::string hex_str(const Bytes &data)
{
    return hex_dump(data, data.size());
}

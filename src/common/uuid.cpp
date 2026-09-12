#include "common/uuid.h"

#include <random>
#include <cstdio>

namespace transcode {

namespace {
std::mt19937_64& rng() {
    static std::mt19937_64 engine(std::random_device{}());
    return engine;
}

std::string toHex(const unsigned char* data, size_t len) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0x0f]);
    }
    return out;
}
} // namespace

std::string generateUuid() {
    // UUID v4
    unsigned char bytes[16];
    for (auto& b : bytes) b = static_cast<unsigned char>(rng()() & 0xff);
    bytes[6] = (bytes[6] & 0x0f) | 0x40; // version 4
    bytes[8] = (bytes[8] & 0x3f) | 0x80; // variant

    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3],
                  bytes[4], bytes[5], bytes[6], bytes[7],
                  bytes[8], bytes[9], bytes[10], bytes[11],
                  bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string(buf);
}

std::string generateToken(int byte_len) {
    unsigned char buf[64];
    byte_len = byte_len > 64 ? 64 : byte_len;
    for (int i = 0; i < byte_len; ++i) buf[i] = static_cast<unsigned char>(rng()() & 0xff);
    return toHex(buf, byte_len);
}

std::string generateSalt() {
    unsigned char buf[16];
    for (auto& b : buf) b = static_cast<unsigned char>(rng()() & 0xff);
    return toHex(buf, 16);
}

} // namespace transcode

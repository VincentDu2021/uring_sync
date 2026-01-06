#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Wire protocol for uring-sync network transfer
// All multi-byte integers are little-endian

namespace protocol {

// Message types
enum class MsgType : uint8_t {
    // Handshake
    HELLO       = 0x01,   // Sender → Receiver: version + secret
    HELLO_OK    = 0x02,   // Receiver → Sender: accepted
    HELLO_FAIL  = 0x03,   // Receiver → Sender: rejected

    // File transfer
    FILE_HDR    = 0x10,   // File metadata (size, mode, path)
    FILE_DATA   = 0x11,   // File content chunk
    FILE_END    = 0x12,   // File complete

    // Control
    ALL_DONE    = 0x20,   // All files transferred
    ERROR       = 0xFF,   // Error with message
};

// Message header: type (1 byte) + length (4 bytes)
constexpr size_t MSG_HEADER_SIZE = 5;

// Protocol version
// Version 1: Original plaintext protocol
// Version 2: Added nonces for kTLS key derivation
constexpr uint8_t PROTOCOL_VERSION = 2;

// Nonce size for kTLS key derivation
constexpr size_t NONCE_SIZE = 16;

// Max sizes
constexpr size_t MAX_SECRET_LEN = 64;
constexpr size_t MAX_PATH_LEN = 4096;
constexpr size_t MAX_ERROR_MSG_LEN = 256;

// ============================================================
// Message Encoding
// ============================================================

inline void write_u16(uint8_t* buf, uint16_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
}

inline void write_u32(uint8_t* buf, uint32_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
    buf[2] = (val >> 16) & 0xFF;
    buf[3] = (val >> 24) & 0xFF;
}

inline void write_u64(uint8_t* buf, uint64_t val) {
    for (int i = 0; i < 8; i++) {
        buf[i] = (val >> (i * 8)) & 0xFF;
    }
}

inline uint16_t read_u16(const uint8_t* buf) {
    return buf[0] | (buf[1] << 8);
}

inline uint32_t read_u32(const uint8_t* buf) {
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

inline uint64_t read_u64(const uint8_t* buf) {
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val |= ((uint64_t)buf[i]) << (i * 8);
    }
    return val;
}

// Write message header
inline void write_header(uint8_t* buf, MsgType type, uint32_t payload_len) {
    buf[0] = static_cast<uint8_t>(type);
    write_u32(buf + 1, payload_len);
}

// Parse message header
inline bool parse_header(const uint8_t* buf, MsgType& type, uint32_t& payload_len) {
    type = static_cast<MsgType>(buf[0]);
    payload_len = read_u32(buf + 1);
    return true;
}

// ============================================================
// Message Builders
// ============================================================

// HELLO message (includes nonce for kTLS key derivation)
// Format: version (1) + secret_len (1) + secret (N) + nonce (16)
inline std::vector<uint8_t> make_hello(const std::string& secret, const uint8_t nonce[NONCE_SIZE]) {
    size_t secret_len = std::min(secret.size(), MAX_SECRET_LEN);
    size_t payload_len = 2 + secret_len + NONCE_SIZE;  // version + secret_len + secret + nonce

    std::vector<uint8_t> msg(MSG_HEADER_SIZE + payload_len);
    write_header(msg.data(), MsgType::HELLO, payload_len);

    msg[5] = PROTOCOL_VERSION;
    msg[6] = static_cast<uint8_t>(secret_len);
    memcpy(msg.data() + 7, secret.data(), secret_len);
    memcpy(msg.data() + 7 + secret_len, nonce, NONCE_SIZE);

    return msg;
}

// HELLO_OK message (includes nonce for kTLS key derivation)
// Format: nonce (16)
inline std::vector<uint8_t> make_hello_ok(const uint8_t nonce[NONCE_SIZE]) {
    std::vector<uint8_t> msg(MSG_HEADER_SIZE + NONCE_SIZE);
    write_header(msg.data(), MsgType::HELLO_OK, NONCE_SIZE);
    memcpy(msg.data() + MSG_HEADER_SIZE, nonce, NONCE_SIZE);
    return msg;
}

// HELLO_FAIL message
inline std::vector<uint8_t> make_hello_fail(uint8_t reason) {
    std::vector<uint8_t> msg(MSG_HEADER_SIZE + 1);
    write_header(msg.data(), MsgType::HELLO_FAIL, 1);
    msg[5] = reason;
    return msg;
}

// FILE_HDR message
inline std::vector<uint8_t> make_file_hdr(uint64_t size, uint32_t mode, const std::string& path) {
    size_t path_len = std::min(path.size(), MAX_PATH_LEN);
    size_t payload_len = 8 + 4 + 2 + path_len;  // size + mode + path_len + path

    std::vector<uint8_t> msg(MSG_HEADER_SIZE + payload_len);
    write_header(msg.data(), MsgType::FILE_HDR, payload_len);

    write_u64(msg.data() + 5, size);
    write_u32(msg.data() + 13, mode);
    write_u16(msg.data() + 17, static_cast<uint16_t>(path_len));
    memcpy(msg.data() + 19, path.data(), path_len);

    return msg;
}

// FILE_DATA message (header only, data sent separately)
inline std::vector<uint8_t> make_file_data_header(uint32_t data_len) {
    std::vector<uint8_t> msg(MSG_HEADER_SIZE);
    write_header(msg.data(), MsgType::FILE_DATA, data_len);
    return msg;
}

// FILE_END message
inline std::vector<uint8_t> make_file_end() {
    std::vector<uint8_t> msg(MSG_HEADER_SIZE);
    write_header(msg.data(), MsgType::FILE_END, 0);
    return msg;
}

// ALL_DONE message
inline std::vector<uint8_t> make_all_done() {
    std::vector<uint8_t> msg(MSG_HEADER_SIZE);
    write_header(msg.data(), MsgType::ALL_DONE, 0);
    return msg;
}

// ERROR message
inline std::vector<uint8_t> make_error(uint8_t code, const std::string& message) {
    size_t msg_len = std::min(message.size(), MAX_ERROR_MSG_LEN);
    size_t payload_len = 1 + 2 + msg_len;

    std::vector<uint8_t> msg(MSG_HEADER_SIZE + payload_len);
    write_header(msg.data(), MsgType::ERROR, payload_len);

    msg[5] = code;
    write_u16(msg.data() + 6, static_cast<uint16_t>(msg_len));
    memcpy(msg.data() + 8, message.data(), msg_len);

    return msg;
}

// ============================================================
// Message Parsers
// ============================================================

struct HelloMsg {
    uint8_t version;
    std::string secret;
    uint8_t nonce[NONCE_SIZE];
};

inline bool parse_hello(const uint8_t* payload, size_t len, HelloMsg& out) {
    if (len < 2) return false;
    out.version = payload[0];
    uint8_t secret_len = payload[1];
    if (len < 2 + secret_len + NONCE_SIZE) return false;
    out.secret.assign(reinterpret_cast<const char*>(payload + 2), secret_len);
    memcpy(out.nonce, payload + 2 + secret_len, NONCE_SIZE);
    return true;
}

struct HelloOkMsg {
    uint8_t nonce[NONCE_SIZE];
};

inline bool parse_hello_ok(const uint8_t* payload, size_t len, HelloOkMsg& out) {
    if (len < NONCE_SIZE) return false;
    memcpy(out.nonce, payload, NONCE_SIZE);
    return true;
}

struct FileHdrMsg {
    uint64_t size;
    uint32_t mode;
    std::string path;
};

inline bool parse_file_hdr(const uint8_t* payload, size_t len, FileHdrMsg& out) {
    if (len < 14) return false;  // 8 + 4 + 2 minimum
    out.size = read_u64(payload);
    out.mode = read_u32(payload + 8);
    uint16_t path_len = read_u16(payload + 12);
    if (len < 14 + path_len) return false;
    out.path.assign(reinterpret_cast<const char*>(payload + 14), path_len);
    return true;
}

// ============================================================
// Path Validation (Security)
// ============================================================

inline bool is_safe_path(const std::string& path) {
    if (path.empty()) return false;
    if (path[0] == '/') return false;  // No absolute paths
    if (path.find("..") != std::string::npos) return false;  // No traversal
    if (path.find('\0') != std::string::npos) return false;  // No null bytes
    return true;
}

}  // namespace protocol

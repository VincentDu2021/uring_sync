#pragma once

// kTLS (Kernel TLS) support for uring-sync
// Uses PSK-based key derivation from shared secret + nonces

#include <cstdint>
#include <cstring>
#include <string>

#include <sys/socket.h>
#include <netinet/tcp.h>
#include <linux/tls.h>

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <fmt/core.h>

namespace ktls {

// Key material sizes for AES-128-GCM
constexpr size_t KEY_SIZE = 16;      // AES-128 key
constexpr size_t IV_SIZE = 4;        // Implicit IV (salt)
constexpr size_t REC_SEQ_SIZE = 8;   // Record sequence number
constexpr size_t NONCE_SIZE = 16;    // Nonce size for key derivation

// Result of key derivation
struct KtlsKeys {
    tls12_crypto_info_aes_gcm_128 tx;  // For sending (sender TX, receiver RX)
    tls12_crypto_info_aes_gcm_128 rx;  // For receiving (sender RX, receiver TX)
};

// Generate a random nonce
inline bool generate_nonce(uint8_t nonce[NONCE_SIZE]) {
    return RAND_bytes(nonce, NONCE_SIZE) == 1;
}

// Derive kTLS keys from shared secret and nonces using HKDF
// Both sender and receiver call this with the same inputs to get the same keys
inline bool derive_keys(const std::string& secret,
                       const uint8_t nonce_sender[NONCE_SIZE],
                       const uint8_t nonce_receiver[NONCE_SIZE],
                       KtlsKeys& keys) {
    // Combine nonces to create salt (32 bytes)
    uint8_t salt[32];
    memcpy(salt, nonce_sender, NONCE_SIZE);
    memcpy(salt + NONCE_SIZE, nonce_receiver, NONCE_SIZE);

    // We need to derive:
    // - TX key (16 bytes) + TX IV (4 bytes) + TX seq (8 bytes) = 28 bytes
    // - RX key (16 bytes) + RX IV (4 bytes) + RX seq (8 bytes) = 28 bytes
    // Total: 56 bytes of key material

    uint8_t key_material[56];

    // Use HKDF to derive key material
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx) return false;

    bool success = false;
    do {
        if (EVP_PKEY_derive_init(ctx) <= 0) break;
        if (EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) <= 0) break;
        if (EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt, sizeof(salt)) <= 0) break;
        if (EVP_PKEY_CTX_set1_hkdf_key(ctx,
                reinterpret_cast<const unsigned char*>(secret.data()),
                secret.size()) <= 0) break;

        // Info string for domain separation
        const char* info = "uring-sync-ktls-v1";
        if (EVP_PKEY_CTX_add1_hkdf_info(ctx,
                reinterpret_cast<const unsigned char*>(info),
                strlen(info)) <= 0) break;

        size_t outlen = sizeof(key_material);
        if (EVP_PKEY_derive(ctx, key_material, &outlen) <= 0) break;

        success = true;
    } while (false);

    EVP_PKEY_CTX_free(ctx);

    if (!success) return false;

    // Fill TX key structure (sender→receiver direction)
    memset(&keys.tx, 0, sizeof(keys.tx));
    keys.tx.info.version = TLS_1_2_VERSION;
    keys.tx.info.cipher_type = TLS_CIPHER_AES_GCM_128;
    memcpy(keys.tx.key, key_material, KEY_SIZE);
    memcpy(keys.tx.iv, key_material + KEY_SIZE, IV_SIZE);
    memcpy(keys.tx.rec_seq, key_material + KEY_SIZE + IV_SIZE, REC_SEQ_SIZE);

    // Fill RX key structure (receiver→sender direction)
    memset(&keys.rx, 0, sizeof(keys.rx));
    keys.rx.info.version = TLS_1_2_VERSION;
    keys.rx.info.cipher_type = TLS_CIPHER_AES_GCM_128;
    memcpy(keys.rx.key, key_material + 28, KEY_SIZE);
    memcpy(keys.rx.iv, key_material + 28 + KEY_SIZE, IV_SIZE);
    memcpy(keys.rx.rec_seq, key_material + 28 + KEY_SIZE + IV_SIZE, REC_SEQ_SIZE);

    // Clear sensitive material
    OPENSSL_cleanse(key_material, sizeof(key_material));

    return true;
}

// Enable kTLS on a socket for the sender side
// Sender uses TX key for sending, RX key for receiving
inline bool enable_sender(int sockfd, const KtlsKeys& keys) {
    // Enable TLS ULP (Upper Layer Protocol)
    if (setsockopt(sockfd, SOL_TCP, TCP_ULP, "tls", sizeof("tls")) < 0) {
        fmt::print(stderr, "kTLS: Failed to set TCP_ULP: {}\n", strerror(errno));
        return false;
    }

    // Set TX crypto info (for sending encrypted data)
    if (setsockopt(sockfd, SOL_TLS, TLS_TX, &keys.tx, sizeof(keys.tx)) < 0) {
        fmt::print(stderr, "kTLS: Failed to set TLS_TX: {}\n", strerror(errno));
        return false;
    }

    // Set RX crypto info (for receiving encrypted data)
    if (setsockopt(sockfd, SOL_TLS, TLS_RX, &keys.rx, sizeof(keys.rx)) < 0) {
        fmt::print(stderr, "kTLS: Failed to set TLS_RX: {}\n", strerror(errno));
        return false;
    }

    return true;
}

// Enable kTLS on a socket for the receiver side
// Receiver uses swapped keys: RX for sending (to sender), TX for receiving (from sender)
inline bool enable_receiver(int sockfd, const KtlsKeys& keys) {
    // Enable TLS ULP
    if (setsockopt(sockfd, SOL_TCP, TCP_ULP, "tls", sizeof("tls")) < 0) {
        fmt::print(stderr, "kTLS: Failed to set TCP_ULP: {}\n", strerror(errno));
        return false;
    }

    // Receiver TX = Sender RX (receiver sends using RX key from sender's perspective)
    if (setsockopt(sockfd, SOL_TLS, TLS_TX, &keys.rx, sizeof(keys.rx)) < 0) {
        fmt::print(stderr, "kTLS: Failed to set TLS_TX: {}\n", strerror(errno));
        return false;
    }

    // Receiver RX = Sender TX (receiver receives using TX key from sender's perspective)
    if (setsockopt(sockfd, SOL_TLS, TLS_RX, &keys.tx, sizeof(keys.tx)) < 0) {
        fmt::print(stderr, "kTLS: Failed to set TLS_RX: {}\n", strerror(errno));
        return false;
    }

    return true;
}

// Check if kTLS is available on this system
inline bool is_available() {
    // Try to create a socket and enable TLS ULP
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return false;

    // This will fail if kTLS module is not loaded
    int ret = setsockopt(sockfd, SOL_TCP, TCP_ULP, "tls", sizeof("tls"));
    close(sockfd);

    // ENOPROTOOPT means TLS ULP is not available
    return ret == 0 || errno != ENOPROTOOPT;
}

}  // namespace ktls

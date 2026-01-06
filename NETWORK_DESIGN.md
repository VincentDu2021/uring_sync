# uring-sync Network Transfer Design

*Secure network file transfer using kTLS + io_uring for internal/trusted networks*

## Overview

Extend uring-sync with network transfer capability:
- **kTLS** (Kernel TLS) for efficient encryption in kernel
- **io_uring** for async batched network I/O
- **PSK (Pre-Shared Key)** - derive TLS keys from `--secret`, no certificates needed
- **Zero-copy** - splice file data directly to encrypted socket

Target use case: Fast file transfer within trusted internal networks.

## Goals

1. **Leverage io_uring** - This is the point of the project
2. **Zero-copy encryption** - splice file → kTLS socket (kernel encrypts)
3. **Simple setup** - No certificates, just `--secret`
4. **Beat SSH tunnel** - Current SSH bottleneck is ~26 MB/s

## Security Model

### Threat Model (Internal Network)

| Threat | Protected? | How |
|--------|------------|-----|
| Network sniffing | ✅ Yes | TLS encryption |
| Unauthorized connections | ✅ Yes | Pre-shared secret |
| Accidental path escape | ✅ Yes | Path validation |
| Man-in-the-middle | ⚠️ Partial | TLS + secret (no cert pinning) |
| Malicious insider | ❌ No | Trusted network assumption |

### Authentication Flow (PSK-based)

No TLS handshake needed - both sides derive keys from shared secret:

```
SENDER                                    RECEIVER
   │                                          │
   │  TCP connect                             │
   ├─────────────────────────────────────────►│
   │                                          │
   │  HELLO { version, nonce_s }              │
   ├─────────────────────────────────────────►│
   │                                     derive keys from
   │                                     secret + nonces
   │  HELLO_OK { nonce_r }                    │
   │◄─────────────────────────────────────────┤
   │                                          │
   │  derive keys from                        │
   │  secret + nonces                         │
   │                                          │
   │  Enable kTLS (both sides)                │
   │                                          │
   │  [encrypted: file transfers begin]       │
   │                                          │
```

### PSK Key Derivation

Both sides derive the same TLS keys from the shared secret:

```cpp
// Derive TLS keys using HKDF (RFC 5869)
// Input: secret (from --secret), nonce_s (sender), nonce_r (receiver)

void derive_ktls_keys(const std::string& secret,
                      const uint8_t nonce_s[16],
                      const uint8_t nonce_r[16],
                      tls12_crypto_info_aes_gcm_128& tx_key,
                      tls12_crypto_info_aes_gcm_128& rx_key) {
    // Combine nonces for uniqueness
    uint8_t salt[32];
    memcpy(salt, nonce_s, 16);
    memcpy(salt + 16, nonce_r, 16);

    // HKDF-Extract
    uint8_t prk[32];
    HKDF_Extract(EVP_sha256(), salt, 32,
                 secret.data(), secret.size(), prk);

    // HKDF-Expand for TX key (sender→receiver)
    uint8_t tx_material[32 + 12];  // key + IV
    HKDF_Expand(EVP_sha256(), prk, 32,
                "uring-sync-tx", 14,
                tx_material, sizeof(tx_material));

    // HKDF-Expand for RX key (receiver→sender)
    uint8_t rx_material[32 + 12];
    HKDF_Expand(EVP_sha256(), prk, 32,
                "uring-sync-rx", 14,
                rx_material, sizeof(rx_material));

    // Fill kTLS structs
    tx_key.info.version = TLS_1_2_VERSION;
    tx_key.info.cipher_type = TLS_CIPHER_AES_GCM_128;
    memcpy(tx_key.key, tx_material, 16);
    memcpy(tx_key.iv, tx_material + 16, 4);
    memcpy(tx_key.rec_seq, tx_material + 20, 8);
    // ... similar for rx_key with swapped direction
}
```

**Why this works:**
- Each connection has unique nonces → unique keys
- HKDF is a standard, proven key derivation function
- No certificates needed - both sides share the secret
- Replay protection via nonces

### Secret Management

Three ways to provide the shared secret:

```bash
# 1. Auto-generate (receiver prints it)
$ uring-sync recv /backup -l 8443
Secret: Kj8mX2pL9qRs    # Copy this to sender

# 2. Command line
$ uring-sync recv /backup -l 8443 --secret "my-secret"
$ uring-sync send /data host:8443 --secret "my-secret"

# 3. File (recommended for scripts)
$ echo "my-secret" > ~/.uring-sync-secret && chmod 600 ~/.uring-sync-secret
$ uring-sync recv /backup -l 8443 --secret-file ~/.uring-sync-secret

# 4. Environment variable
$ export URING_SYNC_SECRET="my-secret"
$ uring-sync recv /backup -l 8443
```

### Why PSK instead of Certificates?

| Approach | Complexity | Setup | Security |
|----------|------------|-------|----------|
| OpenSSL + Certs | High | Generate certs, distribute, manage expiry | Strong (PKI) |
| OpenSSL + PSK | Medium | Need OpenSSL handshake code | Strong |
| **Direct PSK + kTLS** | **Low** | Just `--secret` | Good enough |

For our use case (io_uring showcase, internal networks), direct PSK is the right choice:
- No certificate infrastructure
- No OpenSSL handshake complexity
- Reuse existing `--secret` parameter
- Focus on the interesting part: io_uring + kTLS + splice

## Architecture

### Mode Selection

```
┌────────────────────────────────────────────────────────────────┐
│                        uring-sync                               │
│                                                                 │
│   uring-sync /src /dst              → Local copy (existing)    │
│   uring-sync send /src host:port    → Network sender           │
│   uring-sync recv /dst -l port      → Network receiver         │
│                                                                 │
└────────────────────────────────────────────────────────────────┘
```

### Data Flow (Network Mode)

```
SENDER                                              RECEIVER
┌─────────────────────────────┐                    ┌─────────────────────────────┐
│                             │                    │                             │
│  ┌───────────┐              │                    │              ┌───────────┐  │
│  │   File    │              │                    │              │   File    │  │
│  │  System   │              │                    │              │  System   │  │
│  └─────┬─────┘              │                    │              └─────▲─────┘  │
│        │ read               │                    │               write│        │
│        ▼                    │                    │                    │        │
│  ┌───────────┐              │                    │              ┌───────────┐  │
│  │ io_uring  │              │      TLS 1.3      │              │ io_uring  │  │
│  │  + kTLS   │─────────────────────────────────────────────────│  + kTLS   │  │
│  │  (send)   │              │    encrypted      │              │  (recv)   │  │
│  └───────────┘              │                    │              └───────────┘  │
│                             │                    │                             │
└─────────────────────────────┘                    └─────────────────────────────┘
```

### Component Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              Shared Components                               │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐ │
│  │ RingManager │  │ BufferPool  │  │  WorkQueue  │  │       Stats         │ │
│  │ (extended)  │  │ (existing)  │  │  (existing) │  │     (existing)      │ │
│  └─────────────┘  └─────────────┘  └─────────────┘  └─────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
         │                                      │
         ▼                                      ▼
┌─────────────────────┐              ┌─────────────────────┐
│    Net Sender       │              │    Net Receiver     │
│                     │              │                     │
│  - Scan files       │              │  - Listen socket    │
│  - TLS connect      │              │  - Accept + TLS     │
│  - Send files       │              │  - Receive files    │
│  - Pipeline msgs    │              │  - Write to disk    │
└─────────────────────┘              └─────────────────────┘

┌─────────────────────┐
│   Local Copy        │
│   (existing)        │
└─────────────────────┘
```

## Wire Protocol

### Message Format

```
┌────────────────────────────────────────────────────┐
│                   Message                          │
├──────────┬──────────┬─────────────────────────────┤
│  type    │  length  │         payload             │
│  1 byte  │  4 bytes │       (variable)            │
└──────────┴──────────┴─────────────────────────────┘
```

All multi-byte integers are little-endian.

### Message Types

```cpp
enum class MsgType : uint8_t {
    // Handshake
    HELLO           = 0x01,   // Client sends secret
    HELLO_OK        = 0x02,   // Server accepts
    HELLO_FAIL      = 0x03,   // Server rejects (wrong secret)

    // File transfer
    FILE_HDR        = 0x10,   // File metadata (size, mode, path)
    FILE_DATA       = 0x11,   // File content chunk
    FILE_END        = 0x12,   // File complete

    // Control
    ALL_DONE        = 0x20,   // All files transferred
    ERROR           = 0xFF,   // Error with message
};
```

### Message Definitions

```cpp
// HELLO (sender → receiver)
struct HelloPayload {
    uint8_t  version;         // Protocol version (1)
    uint8_t  secret_len;      // Length of secret (max 64)
    char     secret[64];      // Pre-shared secret
};

// HELLO_OK (receiver → sender)
// No payload

// HELLO_FAIL (receiver → sender)
struct HelloFailPayload {
    uint8_t  reason;          // 1=wrong secret, 2=version mismatch
};

// FILE_HDR (sender → receiver)
struct FileHdrPayload {
    uint64_t size;            // File size in bytes
    uint32_t mode;            // File permissions (e.g., 0644)
    uint16_t path_len;        // Length of path string
    // char  path[path_len];  // Relative path (no leading /)
};

// FILE_DATA (sender → receiver)
// Raw file bytes, no additional header
// Length from message header

// FILE_END (sender → receiver)
// No payload, signals EOF for current file

// ALL_DONE (sender → receiver)
// No payload, signals transfer complete

// ERROR (either direction)
struct ErrorPayload {
    uint8_t  code;            // Error code
    uint16_t msg_len;         // Message length
    // char  msg[msg_len];    // Human-readable error
};
```

### Transfer Sequence

```
SENDER                                              RECEIVER
   │                                                    │
   │──────────── HELLO {secret} ───────────────────────►│
   │                                                    │ verify
   │◄─────────── HELLO_OK {} ──────────────────────────│
   │                                                    │
   │  ┌─── File 1 ───────────────────────────────────┐  │
   │  │ FILE_HDR {size=4096, mode=0644, path="a.txt"}│  │
   │──┼─────────────────────────────────────────────────►│
   │  │ FILE_DATA [4096 bytes]                       │  │  create file
   │──┼─────────────────────────────────────────────────►│  write data
   │  │ FILE_END {}                                  │  │  close file
   │──┼─────────────────────────────────────────────────►│
   │  └──────────────────────────────────────────────┘  │
   │                                                    │
   │  ┌─── File 2 ───────────────────────────────────┐  │
   │  │ FILE_HDR {size=8192, mode=0755, path="b.sh"} │  │
   │──┼─────────────────────────────────────────────────►│
   │  │ FILE_DATA [8192 bytes]                       │  │
   │──┼─────────────────────────────────────────────────►│
   │  │ FILE_END {}                                  │  │
   │──┼─────────────────────────────────────────────────►│
   │  └──────────────────────────────────────────────┘  │
   │                                                    │
   │──────────── ALL_DONE {} ──────────────────────────►│
   │                                                    │
   │◄─────────── (connection close) ───────────────────│
```

### Large File Chunking

Files larger than chunk size are sent as multiple FILE_DATA messages:

```
FILE_HDR {size=1GB, path="large.bin"}
FILE_DATA [128KB]    # chunk 1
FILE_DATA [128KB]    # chunk 2
...
FILE_DATA [128KB]    # chunk N
FILE_END {}
```

## State Machines

### Sender States

```
┌─────────────┐
│   START     │
└──────┬──────┘
       │
       ▼
┌─────────────┐     ┌─────────────┐
│ CONNECTING  │────►│   FAILED    │
└──────┬──────┘     └─────────────┘
       │ connected
       ▼
┌─────────────┐
│ TLS_HANDSHK │────►│   FAILED    │
└──────┬──────┘     └─────────────┘
       │ TLS ready
       ▼
┌─────────────┐
│ SEND_HELLO  │────►│   FAILED    │
└──────┬──────┘     └─────────────┘
       │ HELLO_OK received
       ▼
┌─────────────┐
│ TRANSFERRING│◄────────────────────┐
└──────┬──────┘                     │
       │ get next file              │
       ▼                            │
┌─────────────┐                     │
│ SEND_HDR    │─────────────────────┤ more files
└──────┬──────┘                     │
       │                            │
       ▼                            │
┌─────────────┐                     │
│ SEND_DATA   │◄──┐                 │
└──────┬──────┘   │ more chunks     │
       │──────────┘                 │
       │ EOF                        │
       ▼                            │
┌─────────────┐                     │
│ SEND_END    │─────────────────────┘
└──────┬──────┘
       │ no more files
       ▼
┌─────────────┐
│ SEND_DONE   │
└──────┬──────┘
       │
       ▼
┌─────────────┐
│    DONE     │
└─────────────┘
```

### Receiver States

```
┌─────────────┐
│  LISTENING  │◄────────────────────────────┐
└──────┬──────┘                             │
       │ accept                             │
       ▼                                    │
┌─────────────┐                             │
│ TLS_HANDSHK │────►│ FAILED │              │
└──────┬──────┘     └────────┘              │
       │ TLS ready                          │
       ▼                                    │
┌─────────────┐                             │
│ RECV_HELLO  │────►│ FAILED │              │
└──────┬──────┘     └────────┘              │
       │ secret OK                          │
       ▼                                    │
┌─────────────┐                             │
│ RECV_MSG    │◄────────────────────┐       │
└──────┬──────┘                     │       │
       │                            │       │
       ├── FILE_HDR ──►┌───────────┐│       │
       │               │CREATE_FILE││       │
       │               └─────┬─────┘│       │
       │                     │      │       │
       ├── FILE_DATA ─►┌───────────┐│       │
       │               │WRITE_DATA ││       │
       │               └─────┬─────┘│       │
       │                     │      │       │
       ├── FILE_END ──►┌───────────┐│       │
       │               │CLOSE_FILE │┘       │
       │               └───────────┘        │
       │                                    │
       └── ALL_DONE ──►┌───────────┐        │
                       │ CLEANUP   │────────┘
                       └───────────┘  (next connection)
```

## Integration with Existing Code

### RingManager Extensions (ring.hpp)

```cpp
class RingManager {
public:
    // ... existing methods ...

    // ============================================================
    // Network Operations (NEW)
    // ============================================================

    void prepare_connect(int sockfd, const struct sockaddr* addr,
                        socklen_t addrlen, void* ctx, bool link = false) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_connect(sqe, sockfd, addr, addrlen);
        io_uring_sqe_set_data(sqe, ctx);
        if (link) sqe->flags |= IOSQE_IO_LINK;
    }

    void prepare_accept(int sockfd, struct sockaddr* addr,
                       socklen_t* addrlen, int flags, void* ctx) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_accept(sqe, sockfd, addr, addrlen, flags);
        io_uring_sqe_set_data(sqe, ctx);
    }

    void prepare_send(int sockfd, const void* buf, size_t len,
                     int flags, void* ctx, bool link = false) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_send(sqe, sockfd, buf, len, flags);
        io_uring_sqe_set_data(sqe, ctx);
        if (link) sqe->flags |= IOSQE_IO_LINK;
    }

    void prepare_recv(int sockfd, void* buf, size_t len,
                     int flags, void* ctx, bool link = false) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_recv(sqe, sockfd, buf, len, flags);
        io_uring_sqe_set_data(sqe, ctx);
        if (link) sqe->flags |= IOSQE_IO_LINK;
    }

    // Splice file directly to socket (zero-copy send)
    void prepare_splice_to_socket(int file_fd, int64_t file_off,
                                  int sock_fd, size_t len,
                                  void* ctx, bool link = false) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_splice(sqe, file_fd, file_off, sock_fd, -1, len,
                            SPLICE_F_MORE);
        io_uring_sqe_set_data(sqe, ctx);
        if (link) sqe->flags |= IOSQE_IO_LINK;
    }

    // Splice socket directly to file (zero-copy recv)
    void prepare_splice_from_socket(int sock_fd, int file_fd,
                                    int64_t file_off, size_t len,
                                    void* ctx, bool link = false) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_splice(sqe, sock_fd, -1, file_fd, file_off, len, 0);
        io_uring_sqe_set_data(sqe, ctx);
        if (link) sqe->flags |= IOSQE_IO_LINK;
    }

    void prepare_shutdown(int sockfd, int how, void* ctx) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_shutdown(sqe, sockfd, how);
        io_uring_sqe_set_data(sqe, ctx);
    }
};
```

### New Types (common.hpp additions)

```cpp
// ============================================================
// Network Context - tracks a network file transfer
// ============================================================
struct NetFileContext {
    // File info
    std::string src_path;       // Local path (sender) or relative path (receiver)
    std::string dst_path;       // Relative path (sender) or local path (receiver)
    int file_fd = -1;
    uint64_t file_size = 0;
    uint64_t offset = 0;
    mode_t mode = 0644;

    // Network state
    enum class State {
        // Sender states
        OPENING_FILE,
        STATING_FILE,
        SENDING_HDR,
        SENDING_DATA,
        SENDING_END,
        CLOSING_FILE,

        // Receiver states
        CREATING_FILE,
        RECEIVING_DATA,
        WRITING_DATA,

        DONE,
        FAILED
    } state;

    // Buffer
    char* buffer = nullptr;
    int buffer_index = -1;
    uint32_t bytes_in_buffer = 0;

    // For statx result
    struct statx stx;
};

// Connection context
struct ConnectionContext {
    int sockfd = -1;
    SSL* ssl = nullptr;             // For handshake, nullptr after kTLS enabled
    bool ktls_enabled = false;
    bool authenticated = false;

    // Message receive buffer
    char msg_header[5];             // type (1) + length (4)
    int header_received = 0;
    std::vector<char> payload_buf;
    uint32_t payload_expected = 0;
    uint32_t payload_received = 0;
};
```

### File Structure

```
include/
  common.hpp          # Existing + NetFileContext, ConnectionContext
  ring.hpp            # Existing + network ops
  protocol.hpp        # NEW - message definitions, serialization
  ktls.hpp            # NEW - kTLS setup helper
  net_common.hpp      # NEW - shared network utilities

src/
  main.cpp            # Modified - mode selection
  local_copy.cpp      # Refactored from main.cpp
  net_sender.cpp      # NEW - sender implementation
  net_receiver.cpp    # NEW - receiver implementation
  ktls.cpp            # NEW - kTLS/OpenSSL integration
  protocol.cpp        # NEW - message encode/decode
```

### Config Extensions (main.cpp)

```cpp
struct Config {
    // Existing options
    int num_workers = 0;
    int queue_depth = 64;
    int chunk_size = 128 * 1024;
    bool verbose = false;
    bool preserve = false;
    std::string src_path;
    std::string dst_path;

    // Mode selection (NEW)
    enum class Mode { LOCAL, SEND, RECV } mode = Mode::LOCAL;

    // Network options (NEW)
    std::string remote_host;        // For sender
    uint16_t remote_port = 8443;    // For sender
    uint16_t listen_port = 8443;    // For receiver
    std::string secret;             // Shared secret
    std::string secret_file;        // Path to secret file

    // TLS options (NEW)
    std::string cert_file;          // Override auto-generated cert
    std::string key_file;           // Override auto-generated key
    bool use_ktls = true;           // Enable kernel TLS (disable for debugging)
};
```

## kTLS Integration (PSK-based)

### The Key Insight: Skip OpenSSL Handshake

Traditional kTLS requires:
1. Do TLS handshake with OpenSSL
2. Extract session keys from OpenSSL internals (complex!)
3. Pass keys to kernel via setsockopt

With PSK, we skip all that:
1. Exchange nonces over plain TCP
2. Derive keys from `--secret` + nonces using HKDF
3. Pass keys directly to kernel

### Setup Flow (Sender)

```cpp
// ktls.hpp - simplified PSK-based kTLS
#include <linux/tls.h>

bool enable_ktls_sender(int sockfd, const std::string& secret,
                        const uint8_t nonce_s[16], const uint8_t nonce_r[16]) {
    // 1. Enable TLS ULP on socket
    if (setsockopt(sockfd, SOL_TCP, TCP_ULP, "tls", sizeof("tls")) < 0) {
        return false;  // kTLS not available
    }

    // 2. Derive keys from PSK
    tls12_crypto_info_aes_gcm_128 tx_key = {};
    tls12_crypto_info_aes_gcm_128 rx_key = {};
    derive_ktls_keys(secret, nonce_s, nonce_r, tx_key, rx_key);

    // 3. Set TX key (for sending encrypted data)
    if (setsockopt(sockfd, SOL_TLS, TLS_TX, &tx_key, sizeof(tx_key)) < 0) {
        return false;
    }

    // 4. Set RX key (for receiving encrypted data)
    if (setsockopt(sockfd, SOL_TLS, TLS_RX, &rx_key, sizeof(rx_key)) < 0) {
        return false;
    }

    return true;
}
```

### Complete Handshake Sequence

```cpp
// Sender side
int setup_ktls_connection(const std::string& host, uint16_t port,
                          const std::string& secret) {
    // 1. TCP connect
    int sockfd = connect_to_host(host, port);

    // 2. Generate sender nonce
    uint8_t nonce_s[16];
    RAND_bytes(nonce_s, 16);

    // 3. Send HELLO with our nonce
    send_hello(sockfd, nonce_s);

    // 4. Receive HELLO_OK with receiver's nonce
    uint8_t nonce_r[16];
    recv_hello_ok(sockfd, nonce_r);

    // 5. Enable kTLS with derived keys
    if (!enable_ktls_sender(sockfd, secret, nonce_s, nonce_r)) {
        // Fallback: continue without encryption (or error)
    }

    // 6. Now socket is encrypted - io_uring ops work transparently
    return sockfd;
}
```

### Zero-Copy Data Path

After kTLS is enabled, the magic happens:

```
┌────────────────────────────────────────────────────────────┐
│                     SENDER                                  │
│                                                            │
│   File (disk)                                              │
│       │                                                    │
│       │ io_uring SPLICE (file → pipe)                     │
│       ▼                                                    │
│   Kernel pipe buffer                                       │
│       │                                                    │
│       │ io_uring SPLICE (pipe → kTLS socket)              │
│       ▼                                                    │
│   kTLS layer (kernel encrypts with AES-NI)                │
│       │                                                    │
│       │ TCP send                                          │
│       ▼                                                    │
│   Network                                                  │
│                                                            │
└────────────────────────────────────────────────────────────┘

Data path: file → kernel → network (ZERO userspace copies!)
```

### io_uring + kTLS Splice Implementation

```cpp
// Send file over kTLS socket using io_uring splice
void send_file_zerocopy(RingManager& ring, int file_fd, int ktls_sockfd,
                        int pipe_read, int pipe_write, uint64_t file_size) {
    uint64_t offset = 0;
    const size_t CHUNK = 128 * 1024;

    while (offset < file_size) {
        size_t len = std::min(file_size - offset, (uint64_t)CHUNK);

        // Splice: file → pipe
        ring.prepare_splice(file_fd, offset, pipe_write, -1, len,
                           SPLICE_F_MOVE, &ctx_file_to_pipe);

        // Splice: pipe → kTLS socket (kernel encrypts)
        ring.prepare_splice(pipe_read, -1, ktls_sockfd, -1, len,
                           SPLICE_F_MOVE | SPLICE_F_MORE, &ctx_pipe_to_sock);

        ring.submit_and_wait(2);
        offset += len;
    }
}
```

### Why This Is Fast

| Path | Copies | Encryption | Syscalls/chunk |
|------|--------|------------|----------------|
| SSH tunnel (current) | 4+ | Userspace SSH | ~4 |
| read/send + kTLS | 2 | Kernel | 2 |
| **splice + kTLS** | **0** | **Kernel** | **2 (batched)** |

The splice + kTLS path:
- File data stays in kernel pages throughout
- AES-NI hardware acceleration
- io_uring batches multiple chunks
- Expected: **50-100+ MB/s** vs 26 MB/s SSH

## Path Validation

### Security Check on Receiver

```cpp
// net_common.hpp
enum class PathValidation {
    VALID,
    TRAVERSAL_ATTACK,      // Contains ..
    ABSOLUTE_PATH,         // Starts with /
    INVALID_CHARS,         // Null bytes, etc.
    PATH_TOO_LONG,
};

PathValidation validate_relative_path(const std::string& path) {
    // Reject empty
    if (path.empty()) return PathValidation::INVALID_CHARS;

    // Reject absolute paths
    if (path[0] == '/') return PathValidation::ABSOLUTE_PATH;

    // Reject path traversal
    if (path.find("..") != std::string::npos)
        return PathValidation::TRAVERSAL_ATTACK;

    // Reject null bytes
    if (path.find('\0') != std::string::npos)
        return PathValidation::INVALID_CHARS;

    // Reject overly long paths
    if (path.length() > 4096)
        return PathValidation::PATH_TOO_LONG;

    return PathValidation::VALID;
}

// Build full destination path safely
std::optional<std::string> build_dest_path(const std::string& root,
                                           const std::string& relative) {
    if (validate_relative_path(relative) != PathValidation::VALID) {
        return std::nullopt;
    }

    fs::path full = fs::weakly_canonical(fs::path(root) / relative);
    fs::path root_canonical = fs::weakly_canonical(root);

    // Verify result is under root
    std::string full_str = full.string();
    std::string root_str = root_canonical.string();

    if (full_str.rfind(root_str, 0) != 0) {
        return std::nullopt;  // Escaped root somehow
    }

    return full_str;
}
```

## CLI Design

### Usage

```
uring-sync - High-performance file copier using io_uring

USAGE:
    uring-sync [OPTIONS] <SOURCE> <DEST>           Local copy
    uring-sync send <SOURCE> <HOST:PORT> [OPTIONS] Send to remote
    uring-sync recv <DEST> --listen <PORT> [OPTIONS] Receive from remote

LOCAL COPY:
    uring-sync /src/dir /dst/dir
    uring-sync -j 1 -v /data /backup

NETWORK OPTIONS:
    --secret <STRING>       Pre-shared secret for authentication
    --tls                   Enable kTLS encryption (requires --secret)
    --uring                 Use io_uring async batching (faster)
    --splice                Use zero-copy splice (slower for small files)
    -l, --listen <PORT>     Listen port for recv mode
    -h, --help              Show help

ENCRYPTION MODES:
    1. Plaintext (trusted network):
       uring-sync send /data host:9999 --secret key
       uring-sync recv /dst --listen 9999 --secret key

    2. Native kTLS (not yet implemented):
       uring-sync send /data host:9999 --secret key --tls
       uring-sync recv /dst --listen 9999 --secret key --tls

    3. SSH tunnel (recommended for now):
       # Terminal 1: Create tunnel
       ssh -L 9999:localhost:9999 user@remote-host

       # Remote host
       uring-sync recv /dst --listen 9999 --secret key

       # Local (through tunnel)
       uring-sync send /data localhost:9999 --secret key

EXAMPLES:
    # Plaintext transfer (LAN/trusted network)
    $ uring-sync recv /backup --listen 9999 --secret abc123
    $ uring-sync send /data 192.168.1.100:9999 --secret abc123

    # Using SSH tunnel for encryption
    $ ssh -L 9999:localhost:9999 user@remote  # Terminal 1
    $ uring-sync recv /backup --listen 9999 --secret abc123  # On remote
    $ uring-sync send /data localhost:9999 --secret abc123   # Local
```

### Argument Parsing

```cpp
int main(int argc, char* argv[]) {
    Config cfg;

    // Detect mode from first positional argument
    if (argc >= 2) {
        if (strcmp(argv[1], "send") == 0) {
            cfg.mode = Config::Mode::SEND;
            // Shift arguments
            argc--; argv++;
        } else if (strcmp(argv[1], "recv") == 0) {
            cfg.mode = Config::Mode::RECV;
            argc--; argv++;
        }
        // else: local mode (default)
    }

    // Parse options...

    // Dispatch to appropriate handler
    switch (cfg.mode) {
        case Config::Mode::LOCAL:
            return run_local_copy(cfg);
        case Config::Mode::SEND:
            return run_sender(cfg);
        case Config::Mode::RECV:
            return run_receiver(cfg);
    }
}
```

## Implementation Phases

### Phase 1: PSK + kTLS Foundation (Current Goal)

**Already Done:**
- [x] Basic network sender/receiver (synchronous, plaintext)
- [x] Wire protocol (HELLO, FILE_HDR, FILE_DATA, ALL_DONE)
- [x] `--secret` authentication

**Next Steps:**
- [ ] Update HELLO/HELLO_OK to include nonces
- [ ] Implement HKDF key derivation from secret + nonces
- [ ] Add `enable_ktls()` function with PSK-derived keys
- [ ] Test kTLS encryption (sender TX, receiver RX)
- **Goal**: Encrypted transfer without OpenSSL handshake

### Phase 2: io_uring Network I/O

Replace synchronous send/recv with io_uring:
- [ ] Add `prepare_send()`, `prepare_recv()` to RingManager
- [ ] Async sender state machine (similar to local copy)
- [ ] Async receiver state machine
- [ ] Benchmark: io_uring vs blocking I/O over kTLS
- **Goal**: Async batched network I/O

### Phase 3: Zero-Copy with Splice

The ultimate goal - zero-copy encrypted transfer:
- [ ] Implement splice file → pipe → kTLS socket (sender)
- [ ] Implement splice kTLS socket → pipe → file (receiver)
- [ ] Handle partial splice, EAGAIN
- [ ] Benchmark vs read/send path
- **Goal**: file → network without userspace copies

### Phase 4: Benchmarking & Polish
- [x] Compare vs SSH tunnel (kTLS 16% faster - see BENCHMARKS.md)
- [x] Compare vs rsync for encrypted transfer (see BENCHMARKS.md)
- [ ] Progress reporting
- [ ] Error handling and recovery
- **Goal**: Demonstrate io_uring + kTLS superiority

### Key Milestones

| Milestone | Expected Improvement |
|-----------|---------------------|
| Phase 1: kTLS working | Encryption without SSH |
| Phase 2: io_uring net | Better latency, batching |
| Phase 3: splice | **2-4x faster** than SSH (50-100 MB/s) |

## Performance Expectations

See **BENCHMARKS.md** for actual benchmark results.

### Theoretical vs Actual: kTLS Performance

| Path | Copies | Encryption | Expected | **Actual** |
|------|--------|------------|----------|------------|
| SSH tunnel | 4+ | Userspace SSH | ~26-38 MB/s | 25 MB/s |
| kTLS + read/send | 2 | Kernel AES-NI | ~45 MB/s | **60 MB/s** |
| kTLS + splice | 0 | Kernel AES-NI | 80-100 MB/s | **23 MB/s** ❌ |

### Why kTLS + read/send is Faster Than Expected

1. **No userspace SSH process** - encryption in kernel
2. **AES-NI directly** - hardware acceleration without context switches
3. **Efficient socket buffering** - kernel manages send buffer well

### Why kTLS + splice is SLOWER (Investigation: 2026-01-06)

**Expectation:** Zero-copy splice should be fastest (file → pipe → kTLS socket)

**Reality:** 2.9x slower than read/send (23 MB/s vs 60 MB/s)

**Root cause:** `splice(pipe → socket)` blocks on network send

```
# strace output showing the problem:
splice(file→pipe):   0.000027s (27μs)   ← instant
splice(pipe→socket): 0.032897s (33ms)   ← BLOCKING 1000x slower!
```

**Why this happens:**
1. `splice()` to a socket is synchronous - waits for data to leave socket buffer
2. When TCP send buffer fills, splice blocks waiting for ACKs from receiver
3. kTLS encryption happens inline during splice, compounding the blocking
4. Each 100KB file causes a ~30-70ms block = 3000+ seconds for 100K files

**Why read/send doesn't have this problem:**
1. `sendto()` on kTLS socket buffers more aggressively
2. Kernel can manage encryption + network send asynchronously
3. Less per-call blocking, better pipelining

**Potential fixes (not implemented):**
1. Use `SPLICE_F_NONBLOCK` + poll/epoll between splices
2. Use io_uring `IORING_OP_SPLICE` for async splice
3. Larger pipe buffer to reduce blocking frequency

**Conclusion:** For many-file workloads, `--tls` (read/send) is the right choice.
`--splice` only benefits single large file transfers without kTLS.

## Testing Plan

### Unit Tests
- Protocol encode/decode
- Path validation
- Secret generation/validation

### Integration Tests
```bash
# Start receiver
./uring-sync recv /tmp/recv -l 8443 --secret test123 &

# Send small files
mkdir -p /tmp/send && seq 1 100 | xargs -I{} touch /tmp/send/file_{}
./uring-sync send /tmp/send localhost:8443 --secret test123

# Verify
diff -r /tmp/send /tmp/recv

# Send large file
dd if=/dev/urandom of=/tmp/send/large.bin bs=1M count=100
./uring-sync send /tmp/send localhost:8443 --secret test123
```

### Benchmarks
```bash
# Create test data
mkdir -p /tmp/bench
seq 1 10000 | xargs -P8 -I{} dd if=/dev/urandom of=/tmp/bench/f_{} bs=4K count=1

# Benchmark our tool
time ./uring-sync send /tmp/bench server:8443 --secret xxx

# Compare with rsync+ssh
time rsync -a /tmp/bench/ user@server:/tmp/recv/

# Compare with scp
time scp -r /tmp/bench user@server:/tmp/recv/
```

## References

- [Linux Kernel TLS Documentation](https://docs.kernel.org/networking/tls.html)
- [kTLS header: linux/tls.h](https://github.com/torvalds/linux/blob/master/include/uapi/linux/tls.h)
- [io_uring and Networking in 2023](https://github.com/axboe/liburing/wiki/io_uring-and-networking-in-2023)
- [HKDF RFC 5869](https://datatracker.ietf.org/doc/html/rfc5869)
- [OpenSSL HKDF](https://www.openssl.org/docs/man3.0/man3/EVP_PKEY_CTX_set_hkdf_md.html)
- [liburing API Reference](https://unixism.net/loti/ref-liburing/submission.html)
- [splice(2) man page](https://man7.org/linux/man-pages/man2/splice.2.html)

# uring-sync Network Transfer Design

*Secure network file transfer using kTLS + io_uring for internal/trusted networks*

## Overview

Extend uring-sync with network transfer capability:
- **kTLS** (Kernel TLS) for efficient encryption
- **io_uring** for async batched network I/O
- **Pre-shared key** authentication (simple, no certificate management)
- **Auto-generated TLS certificates** (zero configuration)

Target use case: Fast file transfer within trusted internal networks.

## Goals

1. **Simple setup** - No complex certificate infrastructure
2. **Fast** - Leverage kTLS for kernel-level encryption, splice for zero-copy
3. **Secure enough** - Encrypted transport, shared secret authentication
4. **Compatible** - Works alongside existing local copy mode

## Security Model

### Threat Model (Internal Network)

| Threat | Protected? | How |
|--------|------------|-----|
| Network sniffing | ✅ Yes | TLS encryption |
| Unauthorized connections | ✅ Yes | Pre-shared secret |
| Accidental path escape | ✅ Yes | Path validation |
| Man-in-the-middle | ⚠️ Partial | TLS + secret (no cert pinning) |
| Malicious insider | ❌ No | Trusted network assumption |

### Authentication Flow

```
SENDER                                    RECEIVER
   │                                          │
   │  TCP connect                             │
   ├─────────────────────────────────────────►│
   │                                          │
   │  TLS handshake (self-signed cert OK)     │
   │◄────────────────────────────────────────►│
   │                                          │
   │  HELLO { version, secret }               │
   ├─────────────────────────────────────────►│
   │                                     verify secret
   │                                          │
   │  HELLO_OK { }                            │
   │◄─────────────────────────────────────────┤
   │                                          │
   │  (file transfers begin)                  │
   │                                          │
```

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

### Auto-Generated TLS Certificates

On first run, uring-sync generates a self-signed certificate:

```
~/.uring-sync/
├── server.key      # RSA 2048-bit private key
└── server.crt      # Self-signed cert, valid 10 years
```

No manual setup required. The certificate is only for encryption, not identity verification (the shared secret handles authentication).

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

## kTLS Integration

### Setup Flow

```cpp
// ktls.hpp
#include <linux/tls.h>
#include <openssl/ssl.h>

class KTLSConnection {
public:
    // Create connection and do TLS handshake
    // Returns socket fd with kTLS enabled, or -1 on error
    static int connect(const std::string& host, uint16_t port,
                      const std::string& cert_file,
                      const std::string& key_file);

    // Accept connection and do TLS handshake
    // Returns socket fd with kTLS enabled, or -1 on error
    static int accept(int listen_fd,
                     const std::string& cert_file,
                     const std::string& key_file);

private:
    // Enable kTLS on socket after handshake
    static bool enable_ktls(int sockfd, SSL* ssl);

    // Extract crypto keys from OpenSSL session
    static bool extract_tls_keys(SSL* ssl,
                                 struct tls12_crypto_info_aes_gcm_128* tx,
                                 struct tls12_crypto_info_aes_gcm_128* rx);
};
```

### kTLS Enable Implementation

```cpp
// ktls.cpp
bool KTLSConnection::enable_ktls(int sockfd, SSL* ssl) {
    // 1. Set TLS ULP (Upper Layer Protocol)
    if (setsockopt(sockfd, SOL_TCP, TCP_ULP, "tls", sizeof("tls")) < 0) {
        // Kernel doesn't support kTLS, fall back to userspace
        return false;
    }

    // 2. Extract keys from OpenSSL
    struct tls12_crypto_info_aes_gcm_128 crypto_tx = {};
    struct tls12_crypto_info_aes_gcm_128 crypto_rx = {};

    if (!extract_tls_keys(ssl, &crypto_tx, &crypto_rx)) {
        return false;
    }

    // 3. Set TX (send) crypto parameters
    if (setsockopt(sockfd, SOL_TLS, TLS_TX, &crypto_tx, sizeof(crypto_tx)) < 0) {
        return false;
    }

    // 4. Set RX (receive) crypto parameters
    if (setsockopt(sockfd, SOL_TLS, TLS_RX, &crypto_rx, sizeof(crypto_rx)) < 0) {
        return false;
    }

    return true;
}

// Check if kTLS is available on this system
bool check_ktls_available() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    int ret = setsockopt(fd, SOL_TCP, TCP_ULP, "tls", sizeof("tls"));
    int err = errno;
    close(fd);

    // ENOPROTOOPT means kTLS not available
    // ENOTCONN is expected (socket not connected), means kTLS is available
    return ret == 0 || err == ENOTCONN;
}
```

### Fallback to Userspace TLS

If kTLS is not available (older kernel), fall back to OpenSSL:

```cpp
class TLSConnection {
public:
    // Returns true if kTLS is active, false if using userspace SSL
    bool is_ktls() const { return ktls_enabled_; }

    // For kTLS: use io_uring send/recv directly on sockfd
    // For userspace: must use SSL_read/SSL_write

    ssize_t send(const void* buf, size_t len);
    ssize_t recv(void* buf, size_t len);

private:
    int sockfd_;
    SSL* ssl_;
    bool ktls_enabled_;
};
```

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
    uring-sync send [OPTIONS] <SOURCE> <HOST:PORT> Send to remote
    uring-sync recv [OPTIONS] <DEST> -l <PORT>     Receive from remote

LOCAL COPY:
    uring-sync /src/dir /dst/dir
    uring-sync -j 4 -v /data /backup

SEND MODE:
    uring-sync send /data server.local:8443 --secret "abc123"
    uring-sync send /data server.local:8443 --secret-file ~/.secret

RECV MODE:
    uring-sync recv /incoming -l 8443              # Auto-generate secret
    uring-sync recv /incoming -l 8443 --secret "abc123"

OPTIONS:
    -j, --jobs <N>          Worker threads (default: CPU count)
    -c, --chunk-size <SIZE> I/O buffer size (default: 128K)
    -q, --queue-depth <N>   io_uring queue depth (default: 64)
    -v, --verbose           Verbose output
    -h, --help              Show help

NETWORK OPTIONS:
    -l, --listen <PORT>     Listen port for recv mode (default: 8443)
    --secret <STRING>       Shared secret for authentication
    --secret-file <PATH>    Read secret from file
    --no-ktls               Disable kernel TLS (use OpenSSL)

EXAMPLES:
    # Start receiver, note the generated secret
    $ uring-sync recv /backup -l 8443
    Listening on 0.0.0.0:8443
    Secret: Kj8mX2pL9qRs

    # Send files using the secret
    $ uring-sync send /data 192.168.1.100:8443 --secret Kj8mX2pL9qRs
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

### Phase 1: Basic Network Infrastructure
- [ ] Add network ops to RingManager
- [ ] Implement protocol message encode/decode
- [ ] Basic sender: connect, send files over raw TCP (no TLS yet)
- [ ] Basic receiver: accept, receive files, write to disk
- [ ] Secret validation in HELLO
- **Goal**: Working file transfer without encryption

### Phase 2: TLS Integration
- [ ] Add OpenSSL for TLS handshake
- [ ] Auto-generate self-signed certificates
- [ ] Implement userspace TLS send/recv
- [ ] Test encrypted transfers
- **Goal**: Secure file transfer with userspace TLS

### Phase 3: kTLS Optimization
- [ ] Detect kTLS availability
- [ ] Extract keys and enable kTLS after handshake
- [ ] Switch to io_uring send/recv with kTLS
- [ ] Benchmark vs userspace TLS
- **Goal**: Kernel-level encryption with io_uring

### Phase 4: Zero-Copy Path
- [ ] Implement splice file→socket for sender
- [ ] Test splice socket→file for receiver (may need pipe)
- [ ] Benchmark vs buffered I/O
- **Goal**: Minimize memory copies

### Phase 5: Polish
- [ ] Progress reporting for network transfers
- [ ] Proper error messages
- [ ] Resume interrupted transfers (future)
- [ ] Multiple parallel connections (future)
- **Goal**: Production-ready tool

## Performance Expectations

### Theoretical Analysis

| Path | Copies | Encryption |
|------|--------|------------|
| Userspace TLS | 4 (read→ssl→send→recv→ssl→write) | CPU |
| kTLS buffered | 2 (read→send, recv→write) | Kernel |
| kTLS + splice | 0-1 | Kernel |

### Expected Throughput (10GbE)

| Mode | Expected | Bottleneck |
|------|----------|------------|
| Userspace TLS | 2-4 Gbps | OpenSSL CPU |
| kTLS buffered | 6-8 Gbps | Memory bandwidth |
| kTLS + splice | 8-10 Gbps | Wire speed |

Note: Actual results depend on CPU, NIC, and kernel version.

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
- [io_uring and Networking in 2023](https://github.com/axboe/liburing/wiki/io_uring-and-networking-in-2023)
- [Using Kernel TLS with OpenSSL](https://delthas.fr/blog/2023/kernel-tls/)
- [liburing API Reference](https://unixism.net/loti/ref-liburing/submission.html)

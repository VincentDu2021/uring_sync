// Network sender/receiver for uring-sync
// Supports zero-copy splice for efficient file→socket transfer
// Supports kTLS encryption via PSK key derivation

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

#include <cstring>
#include <filesystem>
#include <vector>
#include <string>

#include <fmt/core.h>
#include "protocol.hpp"
#include "ktls.hpp"

// Global flag to enable/disable splice (for benchmarking)
// Default: false - benchmarks show read/send is 2.5x faster than splice for small files
static bool g_use_splice = false;

namespace fs = std::filesystem;

// ============================================================
// Network Helpers
// ============================================================

static bool send_all(int sockfd, const void* buf, size_t len, int flags = 0) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sockfd, p + sent, len - sent, flags);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static bool recv_all(int sockfd, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t received = 0;
    while (received < len) {
        ssize_t n = recv(sockfd, p + received, len - received, 0);
        if (n <= 0) return false;
        received += n;
    }
    return true;
}

static bool send_msg(int sockfd, const std::vector<uint8_t>& msg, bool more_coming = false) {
    return send_all(sockfd, msg.data(), msg.size(), more_coming ? MSG_MORE : 0);
}

static bool recv_header(int sockfd, protocol::MsgType& type, uint32_t& payload_len) {
    uint8_t header[protocol::MSG_HEADER_SIZE];
    if (!recv_all(sockfd, header, sizeof(header))) return false;
    return protocol::parse_header(header, type, payload_len);
}

// ============================================================
// Sender Implementation
// ============================================================

static int connect_to_host(const std::string& host, uint16_t port) {
    struct addrinfo hints = {}, *result;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    int err = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (err != 0) {
        fmt::print(stderr, "getaddrinfo: {}\n", gai_strerror(err));
        return -1;
    }

    int sockfd = -1;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd < 0) continue;

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;  // Success
        }
        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(result);
    return sockfd;
}

// Splice data from pipe to socket, handling partial writes
static bool splice_to_socket(int pipe_read_fd, int sockfd, size_t len) {
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = splice(pipe_read_fd, nullptr, sockfd, nullptr,
                          remaining, SPLICE_F_MOVE | SPLICE_F_MORE);
        if (n <= 0) {
            if (n < 0 && errno == EAGAIN) continue;
            return false;
        }
        remaining -= n;
    }
    return true;
}

static bool send_file(int sockfd, const std::string& base_path,
                      const std::string& rel_path, char* buffer, size_t buf_size,
                      int pipe_read_fd, int pipe_write_fd) {
    std::string full_path = base_path + "/" + rel_path;

    // Open and stat file
    int fd = open(full_path.c_str(), O_RDONLY);
    if (fd < 0) {
        fmt::print(stderr, "Failed to open {}: {}\n", full_path, strerror(errno));
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return false;
    }

    // Send FILE_HDR
    auto hdr = protocol::make_file_hdr(st.st_size, st.st_mode & 0777, rel_path);
    if (!send_msg(sockfd, hdr)) {
        close(fd);
        return false;
    }

    // Send file data (no per-chunk headers - receiver knows size from FILE_HDR)
    uint64_t remaining = st.st_size;
    loff_t offset = 0;

    while (remaining > 0) {
        size_t chunk_size = std::min(remaining, (uint64_t)buf_size);

        if (g_use_splice && pipe_read_fd >= 0) {
            // Zero-copy path: file → pipe → socket
            ssize_t spliced = splice(fd, &offset, pipe_write_fd, nullptr,
                                    chunk_size, SPLICE_F_MOVE);
            if (spliced <= 0) {
                close(fd);
                return false;
            }

            // Splice from pipe to socket (pure zero-copy, no headers)
            if (!splice_to_socket(pipe_read_fd, sockfd, spliced)) {
                close(fd);
                return false;
            }
            remaining -= spliced;
        } else {
            // Fallback: read → buffer → send
            ssize_t n = read(fd, buffer, chunk_size);
            if (n <= 0) {
                close(fd);
                return false;
            }

            if (!send_all(sockfd, buffer, n)) {
                close(fd);
                return false;
            }
            remaining -= n;
        }
    }

    close(fd);
    return true;
}

static void collect_files(const std::string& base_path, const std::string& rel_path,
                          std::vector<std::string>& files) {
    std::string full_path = rel_path.empty() ? base_path : base_path + "/" + rel_path;

    DIR* dir = opendir(full_path.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        std::string name = entry->d_name;
        std::string new_rel = rel_path.empty() ? name : rel_path + "/" + name;
        std::string new_full = base_path + "/" + new_rel;

        struct stat st;
        if (stat(new_full.c_str(), &st) < 0) continue;

        if (S_ISDIR(st.st_mode)) {
            collect_files(base_path, new_rel, files);
        } else if (S_ISREG(st.st_mode)) {
            files.push_back(new_rel);
        }
    }
    closedir(dir);
}

int run_sender(const std::string& src_path, const std::string& host,
               uint16_t port, const std::string& secret, bool use_splice,
               bool use_tls) {
    g_use_splice = use_splice;

    fmt::print("Connecting to {}:{}...\n", host, port);
    fmt::print("Mode: {}{}\n",
               use_splice ? "splice (zero-copy)" : "read/send",
               use_tls ? " + kTLS encryption" : "");

    int sockfd = connect_to_host(host, port);
    if (sockfd < 0) {
        fmt::print(stderr, "Failed to connect\n");
        return 1;
    }

    fmt::print("Connected. Authenticating...\n");

    // Generate sender nonce for kTLS key derivation
    uint8_t nonce_sender[protocol::NONCE_SIZE];
    if (!ktls::generate_nonce(nonce_sender)) {
        fmt::print(stderr, "Failed to generate nonce\n");
        close(sockfd);
        return 1;
    }

    // Send HELLO with our nonce
    auto hello = protocol::make_hello(secret, nonce_sender);
    if (!send_msg(sockfd, hello)) {
        fmt::print(stderr, "Failed to send HELLO\n");
        close(sockfd);
        return 1;
    }

    // Wait for HELLO_OK with receiver's nonce
    protocol::MsgType type;
    uint32_t payload_len;
    if (!recv_header(sockfd, type, payload_len)) {
        fmt::print(stderr, "Failed to receive response\n");
        close(sockfd);
        return 1;
    }

    if (type != protocol::MsgType::HELLO_OK) {
        fmt::print(stderr, "Authentication failed\n");
        close(sockfd);
        return 1;
    }

    // Parse receiver's nonce from HELLO_OK
    uint8_t nonce_receiver[protocol::NONCE_SIZE];
    if (payload_len >= protocol::NONCE_SIZE) {
        std::vector<uint8_t> ok_payload(payload_len);
        if (!recv_all(sockfd, ok_payload.data(), payload_len)) {
            fmt::print(stderr, "Failed to receive HELLO_OK payload\n");
            close(sockfd);
            return 1;
        }
        protocol::HelloOkMsg hello_ok;
        if (!protocol::parse_hello_ok(ok_payload.data(), payload_len, hello_ok)) {
            fmt::print(stderr, "Failed to parse HELLO_OK\n");
            close(sockfd);
            return 1;
        }
        memcpy(nonce_receiver, hello_ok.nonce, protocol::NONCE_SIZE);
    } else {
        // Old protocol without nonce (shouldn't happen with v2)
        memset(nonce_receiver, 0, protocol::NONCE_SIZE);
    }

    // Enable kTLS if requested
    if (use_tls) {
        fmt::print("Enabling kTLS encryption...\n");

        // Derive keys from shared secret + nonces
        ktls::KtlsKeys keys;
        if (!ktls::derive_keys(secret, nonce_sender, nonce_receiver, keys)) {
            fmt::print(stderr, "Failed to derive kTLS keys\n");
            close(sockfd);
            return 1;
        }

        // Enable kTLS on socket
        if (!ktls::enable_sender(sockfd, keys)) {
            fmt::print(stderr, "Failed to enable kTLS\n");
            close(sockfd);
            return 1;
        }

        fmt::print("kTLS enabled (AES-128-GCM)\n");
    }

    fmt::print("Authenticated. Scanning files...\n");

    // Collect files
    std::vector<std::string> files;
    struct stat st;
    if (stat(src_path.c_str(), &st) < 0) {
        fmt::print(stderr, "Cannot stat {}\n", src_path);
        close(sockfd);
        return 1;
    }

    std::string base_path;
    if (S_ISDIR(st.st_mode)) {
        base_path = src_path;
        collect_files(base_path, "", files);
    } else {
        // Single file
        base_path = fs::path(src_path).parent_path().string();
        files.push_back(fs::path(src_path).filename().string());
    }

    fmt::print("Sending {} files...\n", files.size());

    // Allocate buffer (used for fallback and protocol headers)
    constexpr size_t BUF_SIZE = 128 * 1024;
    char* buffer = new char[BUF_SIZE];

    // Create pipe for splice
    int pipefd[2] = {-1, -1};
    if (use_splice) {
        if (pipe(pipefd) < 0) {
            fmt::print(stderr, "Warning: pipe() failed, falling back to read/send\n");
            g_use_splice = false;
        } else {
            // Set pipe size to match buffer size for optimal throughput
            fcntl(pipefd[0], F_SETPIPE_SZ, BUF_SIZE);
        }
    }

    size_t sent = 0;
    for (const auto& rel_path : files) {
        if (!send_file(sockfd, base_path, rel_path, buffer, BUF_SIZE,
                      pipefd[0], pipefd[1])) {
            fmt::print(stderr, "Failed to send {}\n", rel_path);
            delete[] buffer;
            if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }
            close(sockfd);
            return 1;
        }
        sent++;
        if (sent % 1000 == 0 || sent == files.size()) {
            fmt::print("\rSent {}/{} files", sent, files.size());
            fflush(stdout);
        }
    }
    fmt::print("\n");

    delete[] buffer;
    if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }

    // Send ALL_DONE
    if (!send_msg(sockfd, protocol::make_all_done())) {
        fmt::print(stderr, "Failed to send ALL_DONE\n");
        close(sockfd);
        return 1;
    }

    close(sockfd);
    fmt::print("Transfer complete: {} files\n", files.size());
    return 0;
}

// ============================================================
// Receiver Implementation
// ============================================================

static int create_listen_socket(uint16_t port) {
    int sockfd = socket(AF_INET6, SOCK_STREAM, 0);
    if (sockfd < 0) {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) return -1;
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Allow both IPv4 and IPv6
    int no = 0;
    setsockopt(sockfd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));

    struct sockaddr_in6 addr = {};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    addr.sin6_addr = in6addr_any;

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        // Try IPv4 only
        close(sockfd);
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) return -1;

        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr4 = {};
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(port);
        addr4.sin_addr.s_addr = INADDR_ANY;

        if (bind(sockfd, (struct sockaddr*)&addr4, sizeof(addr4)) < 0) {
            close(sockfd);
            return -1;
        }
    }

    if (listen(sockfd, 5) < 0) {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

static bool receive_file(int sockfd, const std::string& dst_root,
                         char* buffer, size_t buf_size) {
    // Already received FILE_HDR header, now get payload
    protocol::MsgType type;
    uint32_t payload_len;

    // First message should be FILE_HDR (caller already checked type)
    // Read payload
    std::vector<uint8_t> payload(payload_len);
    // Note: caller needs to pass payload_len, let's restructure

    return true;  // Placeholder
}

int run_receiver(const std::string& dst_path, uint16_t port,
                 const std::string& secret, bool use_tls) {
    // Create destination directory
    fs::create_directories(dst_path);

    int listen_fd = create_listen_socket(port);
    if (listen_fd < 0) {
        fmt::print(stderr, "Failed to listen on port {}: {}\n", port, strerror(errno));
        return 1;
    }

    fmt::print("Listening on port {}...{}\n", port, use_tls ? " (kTLS enabled)" : "");
    if (!secret.empty()) {
        fmt::print("Secret: {}\n", secret);
    }

    // Accept connection
    struct sockaddr_storage client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
        fmt::print(stderr, "Accept failed: {}\n", strerror(errno));
        close(listen_fd);
        return 1;
    }

    char addr_str[INET6_ADDRSTRLEN];
    if (client_addr.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in*)&client_addr)->sin_addr,
                  addr_str, sizeof(addr_str));
    } else {
        inet_ntop(AF_INET6, &((struct sockaddr_in6*)&client_addr)->sin6_addr,
                  addr_str, sizeof(addr_str));
    }
    fmt::print("Connection from {}\n", addr_str);

    // Receive HELLO
    protocol::MsgType type;
    uint32_t payload_len;
    if (!recv_header(client_fd, type, payload_len)) {
        fmt::print(stderr, "Failed to receive HELLO\n");
        close(client_fd);
        close(listen_fd);
        return 1;
    }

    if (type != protocol::MsgType::HELLO) {
        fmt::print(stderr, "Expected HELLO, got {}\n", static_cast<int>(type));
        close(client_fd);
        close(listen_fd);
        return 1;
    }

    std::vector<uint8_t> hello_payload(payload_len);
    if (!recv_all(client_fd, hello_payload.data(), payload_len)) {
        close(client_fd);
        close(listen_fd);
        return 1;
    }

    protocol::HelloMsg hello;
    if (!protocol::parse_hello(hello_payload.data(), payload_len, hello)) {
        close(client_fd);
        close(listen_fd);
        return 1;
    }

    // Verify secret
    if (!secret.empty() && hello.secret != secret) {
        fmt::print(stderr, "Wrong secret\n");
        send_msg(client_fd, protocol::make_hello_fail(1));
        close(client_fd);
        close(listen_fd);
        return 1;
    }

    // Generate receiver nonce for kTLS key derivation
    uint8_t nonce_receiver[protocol::NONCE_SIZE];
    if (!ktls::generate_nonce(nonce_receiver)) {
        fmt::print(stderr, "Failed to generate nonce\n");
        close(client_fd);
        close(listen_fd);
        return 1;
    }

    // Send HELLO_OK with our nonce
    if (!send_msg(client_fd, protocol::make_hello_ok(nonce_receiver))) {
        close(client_fd);
        close(listen_fd);
        return 1;
    }

    // Enable kTLS if requested
    if (use_tls) {
        fmt::print("Enabling kTLS encryption...\n");

        // Derive keys from shared secret + nonces
        ktls::KtlsKeys keys;
        if (!ktls::derive_keys(secret, hello.nonce, nonce_receiver, keys)) {
            fmt::print(stderr, "Failed to derive kTLS keys\n");
            close(client_fd);
            close(listen_fd);
            return 1;
        }

        // Enable kTLS on socket (receiver uses swapped keys)
        if (!ktls::enable_receiver(client_fd, keys)) {
            fmt::print(stderr, "Failed to enable kTLS\n");
            close(client_fd);
            close(listen_fd);
            return 1;
        }

        fmt::print("kTLS enabled (AES-128-GCM)\n");
    }

    fmt::print("Authenticated. Receiving files...\n");

    // Allocate buffer
    constexpr size_t BUF_SIZE = 128 * 1024;
    char* buffer = new char[BUF_SIZE];

    size_t files_received = 0;
    bool error = false;

    while (!error) {
        if (!recv_header(client_fd, type, payload_len)) {
            fmt::print(stderr, "Connection lost\n");
            error = true;
            break;
        }

        if (type == protocol::MsgType::ALL_DONE) {
            fmt::print("\nTransfer complete: {} files received\n", files_received);
            break;
        }

        if (type != protocol::MsgType::FILE_HDR) {
            fmt::print(stderr, "Expected FILE_HDR or ALL_DONE, got {}\n", static_cast<int>(type));
            error = true;
            break;
        }

        // Parse FILE_HDR
        std::vector<uint8_t> payload(payload_len);
        if (!recv_all(client_fd, payload.data(), payload_len)) {
            error = true;
            break;
        }

        protocol::FileHdrMsg hdr;
        if (!protocol::parse_file_hdr(payload.data(), payload_len, hdr)) {
            fmt::print(stderr, "Invalid FILE_HDR\n");
            error = true;
            break;
        }

        if (!protocol::is_safe_path(hdr.path)) {
            fmt::print(stderr, "Unsafe path rejected: {}\n", hdr.path);
            error = true;
            break;
        }

        std::string file_path = dst_path + "/" + hdr.path;

        // Create parent directories
        fs::create_directories(fs::path(file_path).parent_path());

        int fd = open(file_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, hdr.mode);
        if (fd < 0) {
            fmt::print(stderr, "Failed to create {}: {}\n", file_path, strerror(errno));
            error = true;
            break;
        }

        // Receive exactly hdr.size bytes of file data (no per-chunk headers)
        uint64_t remaining = hdr.size;
        while (remaining > 0) {
            size_t to_recv = std::min(remaining, (uint64_t)BUF_SIZE);
            if (!recv_all(client_fd, buffer, to_recv)) {
                fmt::print(stderr, "Failed to receive file data\n");
                close(fd);
                error = true;
                break;
            }

            ssize_t written = write(fd, buffer, to_recv);
            if (written != (ssize_t)to_recv) {
                fmt::print(stderr, "Write failed\n");
                close(fd);
                error = true;
                break;
            }
            remaining -= to_recv;
        }

        if (!error) {
            close(fd);
            files_received++;

            if (files_received % 1000 == 0) {
                fmt::print("\rReceived {} files", files_received);
                fflush(stdout);
            }
        }
    }

    delete[] buffer;
    close(client_fd);
    close(listen_fd);

    return error ? 1 : 0;
}

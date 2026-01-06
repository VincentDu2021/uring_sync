// io_uring-based network sender/receiver for uring-sync
// Uses async batching like local copy for high throughput

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <liburing.h>

#include <cstring>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>

#include <fmt/core.h>
#include "protocol.hpp"
#include "common.hpp"

namespace fs = std::filesystem;

// ============================================================
// Configuration
// ============================================================
struct NetConfig {
    size_t queue_depth = 64;       // Files in-flight
    size_t chunk_size = 128 * 1024;  // 128KB chunks
    bool verbose = false;
};

// ============================================================
// Sender State Machine
// ============================================================

enum class SendState {
    PENDING,        // Waiting to start
    OPENING,        // openat submitted
    STATING,        // statx submitted
    READY,          // File opened & stated, waiting for turn to send
    READING,        // read submitted
    CLOSING,        // close submitted
    DONE,           // Complete
    FAILED          // Error
};

// Forward declare
enum class SendOp : uint8_t;

struct SendContext {
    std::string src_path;
    std::string rel_path;
    SendState state = SendState::PENDING;
    SendOp current_op;  // Current operation type for user_data lookup

    int fd = -1;
    struct statx stx;
    uint64_t file_size = 0;
    uint64_t offset = 0;

    char* buffer = nullptr;
    size_t buf_size = 0;
    size_t buffer_idx = 0;  // Which buffer this context is using
    ssize_t last_read = 0;
};

// Operation types for user_data identification
enum class SendOp : uint8_t {
    OPEN,
    STATX,
    READ,
    SEND,
    CLOSE
};

// Store op in context and use raw pointer as user_data (avoids alignment issues)
inline uint64_t make_user_data(SendContext* ctx, SendOp op) {
    ctx->current_op = op;
    return reinterpret_cast<uint64_t>(ctx);
}

inline SendContext* get_context(uint64_t user_data) {
    return reinterpret_cast<SendContext*>(user_data);
}

inline SendOp get_op(SendContext* ctx) {
    return ctx->current_op;
}

// ============================================================
// Sender Implementation
// ============================================================

class AsyncSender {
public:
    AsyncSender(int sockfd, const std::string& base_path, const NetConfig& cfg)
        : sockfd_(sockfd), base_path_(base_path), cfg_(cfg) {

        // Initialize io_uring
        struct io_uring_params params = {};
        if (io_uring_queue_init_params(cfg.queue_depth * 4, &ring_, &params) < 0) {
            throw std::runtime_error("Failed to init io_uring");
        }

        // Allocate buffers and initialize free list
        buffers_.resize(cfg.queue_depth);
        for (size_t i = 0; i < cfg.queue_depth; i++) {
            buffers_[i].resize(cfg.chunk_size);
            free_buffers_.push_back(i);
        }
    }

    ~AsyncSender() {
        io_uring_queue_exit(&ring_);
    }

    bool scan_files() {
        try {
            for (const auto& entry : fs::recursive_directory_iterator(base_path_)) {
                if (entry.is_regular_file()) {
                    std::string rel = fs::relative(entry.path(), base_path_).string();
                    files_.push_back({entry.path().string(), rel, SendState::PENDING});
                }
            }
        } catch (const fs::filesystem_error& e) {
            fmt::print(stderr, "Scan error: {}\n", e.what());
            return false;
        }

        // Sort by inode for sequential access
        std::vector<std::pair<ino_t, size_t>> inode_order;
        for (size_t i = 0; i < files_.size(); i++) {
            struct stat st;
            ino_t inode = 0;
            if (stat(files_[i].src_path.c_str(), &st) == 0) {
                inode = st.st_ino;
            }
            inode_order.push_back({inode, i});
        }
        std::sort(inode_order.begin(), inode_order.end());

        std::vector<SendContext> sorted;
        sorted.reserve(files_.size());
        for (auto& [inode, idx] : inode_order) {
            sorted.push_back(std::move(files_[idx]));
        }
        files_ = std::move(sorted);

        return true;
    }

    bool run() {
        size_t next_to_open = 0;    // Next file to start opening
        size_t next_to_send = 0;    // Next file to send over network (must be sequential)
        size_t in_flight = 0;       // Files with pending io_uring ops
        size_t completed = 0;
        bool sending_active = false; // True if a file is currently being sent

        fmt::print("Sending {} files...\n", files_.size());

        while (completed < files_.size()) {
            // Start opening new files (batch opens/stats for prefetch)
            while (!free_buffers_.empty() && next_to_open < files_.size()) {
                size_t buf_idx = free_buffers_.back();
                free_buffers_.pop_back();

                auto& ctx = files_[next_to_open];
                ctx.buffer = buffers_[buf_idx].data();
                ctx.buf_size = buffers_[buf_idx].size();
                ctx.buffer_idx = buf_idx;

                // Submit openat
                struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
                if (!sqe) break;

                io_uring_prep_openat(sqe, AT_FDCWD, ctx.src_path.c_str(), O_RDONLY, 0);
                io_uring_sqe_set_data64(sqe, make_user_data(&ctx, SendOp::OPEN));
                ctx.state = SendState::OPENING;

                next_to_open++;
                in_flight++;
            }

            // If no file is currently sending AND next file is ready, start it
            if (!sending_active &&
                next_to_send < files_.size() &&
                files_[next_to_send].state == SendState::READY) {

                if (!start_sending_file(&files_[next_to_send])) {
                    files_[next_to_send].state = SendState::FAILED;
                    free_buffers_.push_back(files_[next_to_send].buffer_idx);
                    completed++;
                    in_flight--;
                    next_to_send++;
                } else {
                    sending_active = true;
                    // Don't increment next_to_send until file is done
                }
            }

            // Submit any pending ops
            io_uring_submit(&ring_);

            // If nothing to wait for, we're stuck
            if (in_flight == 0) break;

            // Wait for completions
            struct io_uring_cqe* cqe;
            int ret = io_uring_wait_cqe(&ring_, &cqe);
            if (ret < 0) {
                fmt::print(stderr, "wait_cqe error: {}\n", strerror(-ret));
                return false;
            }

            // Process all available completions
            unsigned head;
            unsigned count = 0;
            io_uring_for_each_cqe(&ring_, head, cqe) {
                count++;
                uint64_t user_data = io_uring_cqe_get_data64(cqe);
                SendContext* ctx = get_context(user_data);
                SendOp op = get_op(ctx);
                int res = cqe->res;

                bool ok = true;
                if (op == SendOp::READ) {
                    // Handle read completion - send data and maybe more reads
                    ok = continue_sending_file(ctx, res);
                } else {
                    // Handle open/stat/close completions
                    ok = advance_open_state(ctx, res);
                }

                if (!ok) {
                    ctx->state = SendState::FAILED;
                }

                if (ctx->state == SendState::DONE || ctx->state == SendState::FAILED) {
                    completed++;
                    in_flight--;

                    // Return buffer to free list
                    free_buffers_.push_back(ctx->buffer_idx);

                    // If this was the file being sent, allow next file to start
                    if (ctx == &files_[next_to_send]) {
                        sending_active = false;
                        next_to_send++;
                    }

                    if (completed % 1000 == 0) {
                        fmt::print("Sent {}/{} files\r", completed, files_.size());
                    }
                }
            }
            io_uring_cq_advance(&ring_, count);
        }

        // Send ALL_DONE
        auto done_msg = protocol::make_all_done();
        send(sockfd_, done_msg.data(), done_msg.size(), 0);

        fmt::print("Transfer complete: {} files\n", completed);
        return true;
    }

private:
    // Process completion - advance state machine for open/stat/close
    bool advance_open_state(SendContext* ctx, int result) {
        if (result < 0) {
            if (cfg_.verbose) {
                fmt::print(stderr, "Error on {}: {}\n", ctx->src_path, strerror(-result));
            }
            if (ctx->fd >= 0) close(ctx->fd);
            return false;
        }

        switch (ctx->state) {
            case SendState::OPENING: {
                ctx->fd = result;

                // Submit statx
                struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
                if (!sqe) return false;

                io_uring_prep_statx(sqe, ctx->fd, "", AT_EMPTY_PATH,
                                   STATX_SIZE | STATX_MODE, &ctx->stx);
                io_uring_sqe_set_data64(sqe, make_user_data(ctx, SendOp::STATX));
                ctx->state = SendState::STATING;
                break;
            }

            case SendState::STATING:
                // File is ready - store size and wait for turn
                ctx->file_size = ctx->stx.stx_size;
                ctx->state = SendState::READY;
                break;

            case SendState::CLOSING:
                ctx->fd = -1;
                ctx->state = SendState::DONE;
                break;

            default:
                return false;
        }

        return true;
    }

    // Start sending a file that's ready (sends FILE_HDR and submits first read)
    bool start_sending_file(SendContext* ctx) {
        if (ctx->state != SendState::READY) {
            fmt::print(stderr, "BUG: start_sending_file called with state != READY (state={})\n", (int)ctx->state);
            return false;
        }

        // Send FILE_HDR synchronously
        auto hdr = protocol::make_file_hdr(ctx->file_size,
                                           ctx->stx.stx_mode & 0777,
                                           ctx->rel_path);
        if (send(sockfd_, hdr.data(), hdr.size(), MSG_MORE) < 0) {
            return false;
        }

        if (ctx->file_size == 0) {
            // Empty file - go to close
            struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (!sqe) return false;
            io_uring_prep_close(sqe, ctx->fd);
            io_uring_sqe_set_data64(sqe, make_user_data(ctx, SendOp::CLOSE));
            ctx->state = SendState::CLOSING;
        } else {
            // Submit read
            struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (!sqe) return false;

            size_t to_read = std::min(ctx->file_size - ctx->offset, ctx->buf_size);
            io_uring_prep_read(sqe, ctx->fd, ctx->buffer, to_read, ctx->offset);
            io_uring_sqe_set_data64(sqe, make_user_data(ctx, SendOp::READ));
            ctx->state = SendState::READING;
        }

        return true;
    }

    // Process read completion and continue sending
    bool continue_sending_file(SendContext* ctx, int result) {
        if (result < 0) {
            if (cfg_.verbose) {
                fmt::print(stderr, "Read error on {}: {}\n", ctx->src_path, strerror(-result));
            }
            return false;
        }

        // Send data synchronously
        int flags = (ctx->offset + result < ctx->file_size) ? MSG_MORE : 0;
        if (send(sockfd_, ctx->buffer, result, flags) < 0) {
            return false;
        }

        ctx->offset += result;

        if (ctx->offset >= ctx->file_size) {
            // Done with file data - close
            struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (!sqe) return false;
            io_uring_prep_close(sqe, ctx->fd);
            io_uring_sqe_set_data64(sqe, make_user_data(ctx, SendOp::CLOSE));
            ctx->state = SendState::CLOSING;
        } else {
            // Read more
            struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (!sqe) return false;

            size_t to_read = std::min(ctx->file_size - ctx->offset, ctx->buf_size);
            io_uring_prep_read(sqe, ctx->fd, ctx->buffer, to_read, ctx->offset);
            io_uring_sqe_set_data64(sqe, make_user_data(ctx, SendOp::READ));
        }

        return true;
    }

    int sockfd_;
    std::string base_path_;
    NetConfig cfg_;
    struct io_uring ring_;
    std::vector<SendContext> files_;
    std::vector<std::vector<char>> buffers_;
    std::vector<size_t> free_buffers_;  // Free buffer indices
};

// ============================================================
// Receiver State Machine
// ============================================================

enum class RecvState {
    RECV_HDR,       // Receiving message header (5 bytes)
    RECV_META,      // Receiving file metadata
    OPENING,        // openat submitted
    RECV_DATA,      // Receiving file data
    WRITING,        // write submitted
    CLOSING,        // close submitted
    DONE,           // Complete
    FAILED          // Error
};

// Forward declare
enum class RecvOp : uint8_t;

struct RecvContext {
    RecvState state = RecvState::RECV_HDR;
    RecvOp current_op;  // Current operation type for user_data lookup

    std::string path;
    int fd = -1;
    uint64_t file_size = 0;
    uint32_t mode = 0;
    uint64_t received = 0;

    char* buffer = nullptr;
    size_t buf_size = 0;
    ssize_t last_recv = 0;
};

enum class RecvOp : uint8_t {
    RECV,
    OPEN,
    WRITE,
    CLOSE
};

// Store op in context and use raw pointer as user_data (avoids alignment issues)
inline uint64_t make_recv_user_data(RecvContext* ctx, RecvOp op) {
    ctx->current_op = op;
    return reinterpret_cast<uint64_t>(ctx);
}

inline RecvContext* get_recv_context(uint64_t user_data) {
    return reinterpret_cast<RecvContext*>(user_data);
}

inline RecvOp get_recv_op(RecvContext* ctx) {
    return ctx->current_op;
}

// ============================================================
// Receiver Implementation
// ============================================================

class AsyncReceiver {
public:
    AsyncReceiver(int sockfd, const std::string& dst_path, const NetConfig& cfg)
        : sockfd_(sockfd), dst_path_(dst_path), cfg_(cfg) {

        // Initialize io_uring
        struct io_uring_params params = {};
        if (io_uring_queue_init_params(cfg.queue_depth * 4, &ring_, &params) < 0) {
            throw std::runtime_error("Failed to init io_uring");
        }

        // Allocate contexts and buffers
        contexts_.resize(cfg.queue_depth);
        buffers_.resize(cfg.queue_depth);
        for (size_t i = 0; i < cfg.queue_depth; i++) {
            buffers_[i].resize(cfg.chunk_size);
            contexts_[i].buffer = buffers_[i].data();
            contexts_[i].buf_size = buffers_[i].size();
        }

        // Header buffer
        hdr_buf_.resize(protocol::MSG_HEADER_SIZE);
        meta_buf_.resize(8 + 4 + 2 + protocol::MAX_PATH_LEN);
    }

    ~AsyncReceiver() {
        io_uring_queue_exit(&ring_);
    }

    bool run() {
        size_t files_completed = 0;
        size_t ctx_idx = 0;

        // Current file being received (TCP ordering means only 1 at a time)
        RecvContext* current = &contexts_[0];
        current->state = RecvState::RECV_HDR;

        // Files in write/close stages (can have multiple)
        std::vector<RecvContext*> writing;

        bool done = false;

        while (!done) {
            // Submit recv for current file if needed
            if (current && current->state == RecvState::RECV_HDR) {
                // Receive header synchronously (5 bytes, fast)
                if (!recv_exact(hdr_buf_.data(), protocol::MSG_HEADER_SIZE)) {
                    fmt::print(stderr, "Failed to receive header\n");
                    return false;
                }

                protocol::MsgType type;
                uint32_t payload_len;
                protocol::parse_header(reinterpret_cast<uint8_t*>(hdr_buf_.data()),
                                      type, payload_len);

                if (type == protocol::MsgType::ALL_DONE) {
                    done = true;
                    current = nullptr;
                    continue;
                }

                if (type != protocol::MsgType::FILE_HDR) {
                    fmt::print(stderr, "Unexpected message type: {}\n", (int)type);
                    return false;
                }

                // Receive metadata
                if (!recv_exact(meta_buf_.data(), payload_len)) {
                    fmt::print(stderr, "Failed to receive metadata\n");
                    return false;
                }

                protocol::FileHdrMsg hdr;
                if (!protocol::parse_file_hdr(reinterpret_cast<uint8_t*>(meta_buf_.data()),
                                             payload_len, hdr)) {
                    fmt::print(stderr, "Failed to parse file header\n");
                    return false;
                }

                if (!protocol::is_safe_path(hdr.path)) {
                    fmt::print(stderr, "Unsafe path: {}\n", hdr.path);
                    return false;
                }

                current->path = (fs::path(dst_path_) / hdr.path).string();
                current->file_size = hdr.size;
                current->mode = hdr.mode;
                current->received = 0;

                // Create parent directories (sync, usually cached)
                fs::create_directories(fs::path(current->path).parent_path());

                // Submit openat via io_uring
                struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
                if (!sqe) return false;

                io_uring_prep_openat(sqe, AT_FDCWD, current->path.c_str(),
                                    O_WRONLY | O_CREAT | O_TRUNC, current->mode & 0777);
                io_uring_sqe_set_data64(sqe, make_recv_user_data(current, RecvOp::OPEN));
                current->state = RecvState::OPENING;
            }

            // Submit any pending ops
            io_uring_submit(&ring_);

            // Wait for completion
            struct io_uring_cqe* cqe;
            int ret = io_uring_wait_cqe(&ring_, &cqe);
            if (ret < 0) {
                fmt::print(stderr, "wait_cqe error: {}\n", strerror(-ret));
                return false;
            }

            // Process completions
            unsigned head;
            unsigned count = 0;
            io_uring_for_each_cqe(&ring_, head, cqe) {
                count++;
                uint64_t user_data = io_uring_cqe_get_data64(cqe);
                RecvContext* ctx = get_recv_context(user_data);
                RecvOp op = get_recv_op(ctx);
                int res = cqe->res;

                if (!advance_recv_state(ctx, op, res)) {
                    ctx->state = RecvState::FAILED;
                }

                if (ctx->state == RecvState::DONE || ctx->state == RecvState::FAILED) {
                    files_completed++;

                    if (files_completed % 1000 == 0) {
                        fmt::print("Received {} files\r", files_completed);
                    }

                    // Recycle context for next file
                    if (ctx == current) {
                        ctx_idx = (ctx_idx + 1) % contexts_.size();
                        current = &contexts_[ctx_idx];
                        current->state = RecvState::RECV_HDR;
                        current->fd = -1;
                    }
                }
            }
            io_uring_cq_advance(&ring_, count);
        }

        // Wait for any remaining writes/closes
        while (true) {
            bool any_pending = false;
            for (auto& ctx : contexts_) {
                if (ctx.state != RecvState::DONE && ctx.state != RecvState::FAILED &&
                    ctx.state != RecvState::RECV_HDR) {
                    any_pending = true;
                    break;
                }
            }
            if (!any_pending) break;

            io_uring_submit(&ring_);

            struct io_uring_cqe* cqe;
            if (io_uring_wait_cqe(&ring_, &cqe) < 0) break;

            unsigned head;
            unsigned count = 0;
            io_uring_for_each_cqe(&ring_, head, cqe) {
                count++;
                uint64_t user_data = io_uring_cqe_get_data64(cqe);
                RecvContext* ctx = get_recv_context(user_data);
                RecvOp op = get_recv_op(ctx);
                advance_recv_state(ctx, op, cqe->res);

                if (ctx->state == RecvState::DONE || ctx->state == RecvState::FAILED) {
                    files_completed++;
                }
            }
            io_uring_cq_advance(&ring_, count);
        }

        fmt::print("Transfer complete: {} files received\n", files_completed);
        return true;
    }

private:
    bool recv_exact(char* buf, size_t len) {
        size_t received = 0;
        while (received < len) {
            ssize_t n = recv(sockfd_, buf + received, len - received, 0);
            if (n <= 0) return false;
            received += n;
        }
        return true;
    }

    bool advance_recv_state(RecvContext* ctx, RecvOp op, int result) {
        if (result < 0 && ctx->state != RecvState::DONE) {
            if (cfg_.verbose) {
                fmt::print(stderr, "Error on {}: {}\n", ctx->path, strerror(-result));
            }
            if (ctx->fd >= 0) close(ctx->fd);
            return false;
        }

        switch (ctx->state) {
            case RecvState::OPENING: {
                ctx->fd = result;

                if (ctx->file_size == 0) {
                    // Empty file - close immediately
                    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
                    if (!sqe) return false;
                    io_uring_prep_close(sqe, ctx->fd);
                    io_uring_sqe_set_data64(sqe, make_recv_user_data(ctx, RecvOp::CLOSE));
                    ctx->state = RecvState::CLOSING;
                } else {
                    // Receive file data (sync recv, async write)
                    size_t to_recv = std::min(ctx->file_size - ctx->received, ctx->buf_size);
                    if (!recv_exact(ctx->buffer, to_recv)) {
                        return false;
                    }
                    ctx->last_recv = to_recv;

                    // Submit write
                    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
                    if (!sqe) return false;
                    io_uring_prep_write(sqe, ctx->fd, ctx->buffer, to_recv, ctx->received);
                    io_uring_sqe_set_data64(sqe, make_recv_user_data(ctx, RecvOp::WRITE));
                    ctx->state = RecvState::WRITING;
                }
                break;
            }

            case RecvState::WRITING: {
                ctx->received += ctx->last_recv;

                if (ctx->received >= ctx->file_size) {
                    // Done - close
                    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
                    if (!sqe) return false;
                    io_uring_prep_close(sqe, ctx->fd);
                    io_uring_sqe_set_data64(sqe, make_recv_user_data(ctx, RecvOp::CLOSE));
                    ctx->state = RecvState::CLOSING;
                } else {
                    // Receive more data
                    size_t to_recv = std::min(ctx->file_size - ctx->received, ctx->buf_size);
                    if (!recv_exact(ctx->buffer, to_recv)) {
                        return false;
                    }
                    ctx->last_recv = to_recv;

                    // Submit write
                    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
                    if (!sqe) return false;
                    io_uring_prep_write(sqe, ctx->fd, ctx->buffer, to_recv, ctx->received);
                    io_uring_sqe_set_data64(sqe, make_recv_user_data(ctx, RecvOp::WRITE));
                }
                break;
            }

            case RecvState::CLOSING:
                ctx->fd = -1;
                ctx->state = RecvState::DONE;
                break;

            default:
                return false;
        }

        return true;
    }

    int sockfd_;
    std::string dst_path_;
    NetConfig cfg_;
    struct io_uring ring_;
    std::vector<RecvContext> contexts_;
    std::vector<std::vector<char>> buffers_;
    std::vector<char> hdr_buf_;
    std::vector<char> meta_buf_;
};

// ============================================================
// Public API
// ============================================================

int run_sender_uring(const std::string& src_path, const std::string& host,
                     uint16_t port, const std::string& secret) {
    // Connect to receiver
    fmt::print("Connecting to {}:{}...\n", host, port);
    fmt::print("Mode: io_uring async\n");

    struct addrinfo hints = {}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) {
        fmt::print(stderr, "Failed to resolve host\n");
        return 1;
    }

    int sockfd = -1;
    for (auto* p = res; p; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) continue;
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);

    if (sockfd < 0) {
        fmt::print(stderr, "Failed to connect\n");
        return 1;
    }

    fmt::print("Connected. Authenticating...\n");

    // Send HELLO (with dummy nonce - TLS not supported with --uring)
    uint8_t dummy_nonce[protocol::NONCE_SIZE] = {};
    auto hello = protocol::make_hello(secret, dummy_nonce);
    if (send(sockfd, hello.data(), hello.size(), 0) < 0) {
        close(sockfd);
        return 1;
    }

    // Receive response
    uint8_t resp_hdr[protocol::MSG_HEADER_SIZE];
    if (recv(sockfd, resp_hdr, sizeof(resp_hdr), MSG_WAITALL) != sizeof(resp_hdr)) {
        fmt::print(stderr, "Failed to receive auth response\n");
        close(sockfd);
        return 1;
    }

    protocol::MsgType type;
    uint32_t len;
    protocol::parse_header(resp_hdr, type, len);

    if (type != protocol::MsgType::HELLO_OK) {
        fmt::print(stderr, "Authentication failed\n");
        close(sockfd);
        return 1;
    }

    fmt::print("Authenticated. Scanning files...\n");

    // Run async sender
    try {
        NetConfig cfg;
        AsyncSender sender(sockfd, src_path, cfg);

        if (!sender.scan_files()) {
            close(sockfd);
            return 1;
        }

        if (!sender.run()) {
            close(sockfd);
            return 1;
        }
    } catch (const std::exception& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        close(sockfd);
        return 1;
    }

    close(sockfd);
    return 0;
}

int run_receiver_uring(const std::string& dst_path, uint16_t port,
                       const std::string& secret) {
    fmt::print("Listening on port {}...\n", port);
    fmt::print("Mode: io_uring async\n");
    fmt::print("Secret: {}\n", secret.empty() ? "(none)" : secret);

    // Create socket
    int listenfd = socket(AF_INET6, SOCK_STREAM, 0);
    if (listenfd < 0) {
        fmt::print(stderr, "Failed to create socket\n");
        return 1;
    }

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Dual-stack
    int no = 0;
    setsockopt(listenfd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));

    struct sockaddr_in6 addr = {};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    addr.sin6_addr = in6addr_any;

    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fmt::print(stderr, "Failed to bind: {}\n", strerror(errno));
        close(listenfd);
        return 1;
    }

    if (listen(listenfd, 1) < 0) {
        close(listenfd);
        return 1;
    }

    // Accept connection
    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);
    int clientfd = accept(listenfd, (struct sockaddr*)&client_addr, &client_len);
    close(listenfd);

    if (clientfd < 0) {
        fmt::print(stderr, "Accept failed\n");
        return 1;
    }

    // Get client address string
    char client_str[INET6_ADDRSTRLEN];
    if (client_addr.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &((struct sockaddr_in6*)&client_addr)->sin6_addr,
                  client_str, sizeof(client_str));
    } else {
        inet_ntop(AF_INET, &((struct sockaddr_in*)&client_addr)->sin_addr,
                  client_str, sizeof(client_str));
    }
    fmt::print("Connection from {}\n", client_str);

    // Receive HELLO
    uint8_t hdr_buf[protocol::MSG_HEADER_SIZE];
    if (recv(clientfd, hdr_buf, sizeof(hdr_buf), MSG_WAITALL) != sizeof(hdr_buf)) {
        fmt::print(stderr, "Failed to receive HELLO\n");
        close(clientfd);
        return 1;
    }

    protocol::MsgType type;
    uint32_t payload_len;
    protocol::parse_header(hdr_buf, type, payload_len);

    if (type != protocol::MsgType::HELLO) {
        fmt::print(stderr, "Expected HELLO, got {}\n", (int)type);
        close(clientfd);
        return 1;
    }

    std::vector<uint8_t> payload(payload_len);
    if (recv(clientfd, payload.data(), payload_len, MSG_WAITALL) != (ssize_t)payload_len) {
        close(clientfd);
        return 1;
    }

    protocol::HelloMsg hello;
    if (!protocol::parse_hello(payload.data(), payload_len, hello)) {
        close(clientfd);
        return 1;
    }

    if (!secret.empty() && hello.secret != secret) {
        fmt::print(stderr, "Invalid secret\n");
        auto fail = protocol::make_hello_fail(1);
        send(clientfd, fail.data(), fail.size(), 0);
        close(clientfd);
        return 1;
    }

    // Send HELLO_OK (with dummy nonce - kTLS not supported with uring receiver)
    uint8_t dummy_nonce[protocol::NONCE_SIZE] = {};
    auto ok = protocol::make_hello_ok(dummy_nonce);
    send(clientfd, ok.data(), ok.size(), 0);

    fmt::print("Authenticated. Receiving files...\n");

    // Create destination directory
    fs::create_directories(dst_path);

    // Run async receiver
    try {
        NetConfig cfg;
        AsyncReceiver receiver(clientfd, dst_path, cfg);

        if (!receiver.run()) {
            close(clientfd);
            return 1;
        }
    } catch (const std::exception& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        close(clientfd);
        return 1;
    }

    close(clientfd);
    return 0;
}

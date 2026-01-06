#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <getopt.h>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <queue>
#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <fmt/core.h>
#include "ring.hpp"
#include "utils.hpp"

// Network mode functions (defined in net.cpp)
int run_sender(const std::string& src_path, const std::string& host,
               uint16_t port, const std::string& secret, bool use_splice,
               bool use_tls);
int run_receiver(const std::string& dst_path, uint16_t port,
                 const std::string& secret, bool use_tls);

// io_uring async network functions (defined in net_uring.cpp)
int run_sender_uring(const std::string& src_path, const std::string& host,
                     uint16_t port, const std::string& secret);
int run_receiver_uring(const std::string& dst_path, uint16_t port,
                       const std::string& secret);

namespace fs = std::filesystem;

// ============================================================
// Configuration
// ============================================================
struct Config {
    int num_workers = 1;              // 1 = optimal for local copy (io_uring provides async parallelism)
    int queue_depth = 64;
    int chunk_size = 128 * 1024;      // 128KB (default, may be auto-tuned)
    bool verbose = false;
    bool use_splice = true;           // Use io_uring splice for zero-copy
    bool sync_mode = false;           // Use synchronous I/O instead of io_uring (better for network storage)
    bool quiet = false;               // Disable progress output
    bool chunk_size_set = false;      // True if user explicitly set chunk size via -c
    std::string src_path;
    std::string dst_path;
};

// ============================================================
// Print Helpers
// ============================================================

void print_usage(const char* prog) {
    fmt::print("Usage: {} [options] <source> <destination>\n", prog);
    fmt::print("\nParallel file copier using io_uring with zero-copy splice\n");
    fmt::print("\nOptions:\n");
    fmt::print("  -j, --jobs <n>       Number of worker threads (default: 1)\n");
    fmt::print("  -c, --chunk-size <n> Chunk size in bytes (default: auto-tuned)\n");
    fmt::print("  -q, --queue-depth <n> io_uring queue depth (default: 64)\n");
    fmt::print("  -v, --verbose        Verbose output\n");
    fmt::print("  --quiet              Disable progress output\n");
    fmt::print("  --no-splice          Use read/write instead of splice\n");
    fmt::print("  --sync               Use synchronous I/O (for network storage)\n");
    fmt::print("  -h, --help           Show this help\n");
    fmt::print("\nExamples:\n");
    fmt::print("  {} src_dir/ dst_dir/           # Copy directory\n", prog);
    fmt::print("  {} -c 262144 src/ dst/         # Fixed 256KB chunks\n", prog);
}

// ============================================================
// Directory Scanner
// ============================================================
struct ScanResult {
    std::vector<FileWorkItem> files;
    SizeStats size_stats;
};

ScanResult scan_files(
    const std::string& src_base,
    const std::string& dst_base
) {
    ScanResult result;

    struct stat st;
    if (stat(src_base.c_str(), &st) != 0) {
        fmt::print(stderr, "Error: Cannot access '{}'\n", src_base);
        return result;
    }

    if (S_ISREG(st.st_mode)) {
        result.files.push_back({src_base, dst_base});
        result.size_stats.observe(st.st_size);
        return result;
    }

    if (!S_ISDIR(st.st_mode)) {
        fmt::print(stderr, "Error: '{}' is not a file or directory\n", src_base);
        return result;
    }

    try {
        fs::create_directories(dst_base);

        for (const auto& entry : fs::recursive_directory_iterator(src_base)) {
            if (entry.is_regular_file()) {
                std::string rel_path = fs::relative(entry.path(), src_base).string();
                std::string src_file = entry.path().string();
                std::string dst_file = (fs::path(dst_base) / rel_path).string();

                fs::create_directories(fs::path(dst_file).parent_path());

                // Get inode for sorting and file size for adaptive chunk sizing
                struct stat file_stat;
                ino_t inode = 0;
                if (stat(src_file.c_str(), &file_stat) == 0) {
                    inode = file_stat.st_ino;
                    result.size_stats.observe(file_stat.st_size);
                }
                result.files.push_back({src_file, dst_file, inode});
            }
        }
    } catch (const fs::filesystem_error& e) {
        fmt::print(stderr, "Filesystem error: {}\n", e.what());
    }

    return result;
}

// ============================================================
// State Machine
// ============================================================
void advance_state(FileContext* ctx, int result, RingManager& ring,
                   Stats& stats, const Config& cfg, PipePool* pipe_pool = nullptr) {
    if (result < 0 && ctx->state != FileState::DONE) {
        // ECANCELED is expected for linked ops when earlier op fails
        if (-result != ECANCELED && cfg.verbose) {
            fmt::print(stderr, "Error on {}: {} (state={})\n",
                      ctx->src_path, strerror(-result), static_cast<int>(ctx->state));
        }
        ctx->state = FileState::FAILED;
        stats.files_failed++;
        if (ctx->src_fd >= 0) close(ctx->src_fd);
        if (ctx->dst_fd >= 0) close(ctx->dst_fd);
        return;
    }

    switch (ctx->state) {
        case FileState::OPENING_SRC:
            ctx->src_fd = result;
            ctx->state = FileState::STATING;
            ctx->current_op = OpType::STATX;
            ring.prepare_statx(ctx->src_fd, "", AT_EMPTY_PATH,
                              STATX_SIZE | STATX_MODE, &ctx->stx, ctx);
            break;

        case FileState::STATING: {
            ctx->file_size = ctx->stx.stx_size;
            ctx->mode = ctx->stx.stx_mode;
            stats.bytes_total += ctx->file_size;

            // Decide whether to use splice (zero-copy via pipe)
            ctx->use_splice = cfg.use_splice;

            ctx->state = FileState::OPENING_DST;
            ctx->current_op = OpType::OPEN_DST;
            ring.prepare_openat(AT_FDCWD, ctx->dst_path.c_str(),
                               O_WRONLY | O_CREAT | O_TRUNC, ctx->mode & 0777, ctx);
            break;
        }

        case FileState::OPENING_DST:
            ctx->dst_fd = result;

            if (ctx->file_size == 0) {
                // Empty file, go straight to closing
                ctx->state = FileState::CLOSING_SRC;
                ctx->current_op = OpType::CLOSE_SRC;
                ring.prepare_close(ctx->src_fd, ctx);
            } else if (ctx->use_splice && pipe_pool) {
                // Use splice for zero-copy (requires pipe)
                auto pipe = pipe_pool->acquire();
                if (pipe.index >= 0) {
                    ctx->pipe_read_fd = pipe.read_fd;
                    ctx->pipe_write_fd = pipe.write_fd;
                    ctx->pipe_index = pipe.index;

                    ctx->state = FileState::SPLICE_IN;
                    ctx->current_op = OpType::SPLICE_IN;
                    uint32_t to_splice = std::min((uint64_t)cfg.chunk_size,
                                                  ctx->file_size - ctx->offset);
                    ring.prepare_splice(ctx->src_fd, ctx->offset,
                                       ctx->pipe_write_fd, -1,
                                       to_splice, SPLICE_F_MOVE, ctx);
                } else {
                    // No pipe available, fall back to read/write
                    ctx->use_splice = false;
                    ctx->state = FileState::READING;
                    ctx->current_op = OpType::READ;
                    uint32_t to_read = std::min((uint64_t)cfg.chunk_size,
                                               ctx->file_size - ctx->offset);
                    ring.prepare_read(ctx->src_fd, ctx->buffer, to_read, ctx->offset, ctx);
                }
            } else {
                // Use read/write
                ctx->state = FileState::READING;
                ctx->current_op = OpType::READ;
                uint32_t to_read = std::min((uint64_t)cfg.chunk_size,
                                           ctx->file_size - ctx->offset);
                ring.prepare_read(ctx->src_fd, ctx->buffer, to_read, ctx->offset, ctx);
            }
            break;

        case FileState::READING:
            ctx->last_read_size = result;
            ctx->state = FileState::WRITING;
            ctx->current_op = OpType::WRITE;
            ring.prepare_write(ctx->dst_fd, ctx->buffer, result, ctx->offset, ctx);
            break;

        case FileState::WRITING:
            ctx->offset += ctx->last_read_size;
            stats.bytes_copied += ctx->last_read_size;

            if (ctx->offset >= ctx->file_size) {
                ctx->state = FileState::CLOSING_SRC;
                ctx->current_op = OpType::CLOSE_SRC;
                ring.prepare_close(ctx->src_fd, ctx);
            } else {
                ctx->state = FileState::READING;
                ctx->current_op = OpType::READ;
                uint32_t to_read = std::min((uint64_t)cfg.chunk_size,
                                           ctx->file_size - ctx->offset);
                ring.prepare_read(ctx->src_fd, ctx->buffer, to_read, ctx->offset, ctx);
            }
            break;

        case FileState::SPLICE_IN:
            // Splice from src_fd to pipe completed
            ctx->splice_len = result;  // Bytes now in pipe
            ctx->state = FileState::SPLICE_OUT;
            ctx->current_op = OpType::SPLICE_OUT;
            // Now splice from pipe to dst_fd (pipe offset is always -1)
            ring.prepare_splice(ctx->pipe_read_fd, -1,
                               ctx->dst_fd, ctx->offset,
                               result, SPLICE_F_MOVE, ctx);
            break;

        case FileState::SPLICE_OUT:
            // Splice from pipe to dst_fd completed
            ctx->offset += result;
            stats.bytes_copied += result;

            if (ctx->offset >= ctx->file_size) {
                // Done with file
                ctx->state = FileState::CLOSING_SRC;
                ctx->current_op = OpType::CLOSE_SRC;
                ring.prepare_close(ctx->src_fd, ctx);
            } else {
                // More data to splice - go back to SPLICE_IN
                ctx->state = FileState::SPLICE_IN;
                ctx->current_op = OpType::SPLICE_IN;
                uint32_t to_splice = std::min((uint64_t)cfg.chunk_size,
                                              ctx->file_size - ctx->offset);
                // Splice from src_fd to pipe (pipe offset is always -1)
                ring.prepare_splice(ctx->src_fd, ctx->offset,
                                   ctx->pipe_write_fd, -1,
                                   to_splice, SPLICE_F_MOVE, ctx);
            }
            break;

        case FileState::CLOSING_SRC:
            ctx->src_fd = -1;
            ctx->state = FileState::CLOSING_DST;
            ctx->current_op = OpType::CLOSE_DST;
            ring.prepare_close(ctx->dst_fd, ctx);
            break;

        case FileState::CLOSING_DST:
            ctx->dst_fd = -1;
            ctx->state = FileState::DONE;
            stats.files_completed++;
            break;

        default:
            break;
    }
}

// ============================================================
// Synchronous Worker Thread (no io_uring, better for network storage)
// ============================================================
void sync_worker_thread(int worker_id, WorkQueue<FileWorkItem>& work_queue,
                        Stats& stats, const Config& cfg) {
    FileWorkItem item;

    while (work_queue.wait_pop(item)) {
        // Open source file
        int src_fd = open(item.src_path.c_str(), O_RDONLY);
        if (src_fd < 0) {
            if (cfg.verbose) {
                fmt::print(stderr, "Failed to open source {}: {}\n",
                          item.src_path, strerror(errno));
            }
            stats.files_failed++;
            continue;
        }

        // Hint sequential read (like cp does)
        posix_fadvise(src_fd, 0, 0, POSIX_FADV_SEQUENTIAL);

        // Get file size
        struct stat st;
        if (fstat(src_fd, &st) < 0) {
            if (cfg.verbose) {
                fmt::print(stderr, "Failed to stat {}: {}\n",
                          item.src_path, strerror(errno));
            }
            close(src_fd);
            stats.files_failed++;
            continue;
        }
        uint64_t file_size = st.st_size;
        stats.bytes_total += file_size;

        // Open destination file
        int dst_fd = open(item.dst_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
        if (dst_fd < 0) {
            if (cfg.verbose) {
                fmt::print(stderr, "Failed to open dest {}: {}\n",
                          item.dst_path, strerror(errno));
            }
            close(src_fd);
            stats.files_failed++;
            continue;
        }

        // Copy data using copy_file_range (zero-copy)
        loff_t off_in = 0, off_out = 0;
        bool success = true;

        while (off_in < (loff_t)file_size) {
            ssize_t copied = copy_file_range(src_fd, &off_in, dst_fd, &off_out,
                                            file_size - off_in, 0);
            if (copied <= 0) {
                if (copied < 0 && cfg.verbose) {
                    fmt::print(stderr, "copy_file_range failed on {}: {}\n",
                              item.src_path, strerror(errno));
                }
                success = false;
                break;
            }
            stats.bytes_copied += copied;
        }

        // Close files
        close(src_fd);
        close(dst_fd);

        if (success) {
            stats.files_completed++;
        } else {
            stats.files_failed++;
        }
    }

    if (cfg.verbose) {
        fmt::print("Sync worker {} finished\n", worker_id);
    }
}

// ============================================================
// io_uring Worker Thread
// ============================================================
void worker_thread(int worker_id, WorkQueue<FileWorkItem>& work_queue,
                   Stats& stats, const Config& cfg) {
    // Each worker has its own io_uring, buffer pool, and pipe pool
    RingManager ring(cfg.queue_depth);
    BufferPool buffer_pool(cfg.queue_depth, cfg.chunk_size);
    PipePool pipe_pool(cfg.queue_depth, cfg.chunk_size);

    std::vector<FileContext*> in_flight;
    in_flight.reserve(cfg.queue_depth);

    auto start_file = [&](const FileWorkItem& item) -> bool {
        auto [buffer, buf_idx] = buffer_pool.acquire();
        if (!buffer) return false;

        FileContext* ctx = new FileContext();
        ctx->src_path = item.src_path;
        ctx->dst_path = item.dst_path;
        ctx->buffer = buffer;
        ctx->buffer_index = buf_idx;
        ctx->state = FileState::OPENING_SRC;
        ctx->current_op = OpType::OPEN_SRC;

        ring.prepare_openat(AT_FDCWD, ctx->src_path.c_str(), O_RDONLY, 0, ctx);
        in_flight.push_back(ctx);
        return true;
    };

    bool queue_exhausted = false;

    while (!queue_exhausted || !in_flight.empty()) {
        // Try to fill pipeline with more work
        while (!queue_exhausted && in_flight.size() < (size_t)cfg.queue_depth) {
            FileWorkItem item;
            if (work_queue.try_pop(item)) {
                if (!start_file(item)) {
                    work_queue.push(std::move(item));
                    break;
                }
            } else {
                if (work_queue.is_done()) {
                    queue_exhausted = true;
                }
                break;
            }
        }

        if (in_flight.empty()) {
            if (!queue_exhausted) {
                FileWorkItem item;
                if (work_queue.wait_pop(item)) {
                    if (!start_file(item)) {
                        // Failed to start - push back and retry
                        work_queue.push(std::move(item));
                        continue;
                    }
                } else {
                    queue_exhausted = true;
                    continue;
                }
            } else {
                break;
            }
        }

        ring.submit();

        // Process completions
        ring.wait_and_process([&](FileContext* ctx, int result) {
            advance_state(ctx, result, ring, stats, cfg, &pipe_pool);

            if (ctx->state == FileState::DONE || ctx->state == FileState::FAILED) {
                buffer_pool.release(ctx->buffer_index);
                pipe_pool.release(ctx->pipe_index);  // Safe even if -1 (no pipe was used)

                auto it = std::find(in_flight.begin(), in_flight.end(), ctx);
                if (it != in_flight.end()) {
                    in_flight.erase(it);
                }

                delete ctx;
            }
        });

        ring.submit();
    }

    if (cfg.verbose) {
        fmt::print("Worker {} finished\n", worker_id);
    }
}

// ============================================================
// Network Mode Helpers
// ============================================================
static bool parse_host_port(const std::string& s, std::string& host, uint16_t& port) {
    size_t colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    host = s.substr(0, colon);
    port = std::atoi(s.substr(colon + 1).c_str());
    return port > 0;
}

static void print_net_usage(const char* prog) {
    fmt::print("Network usage:\n");
    fmt::print("  {} send <source> <host:port> [options]\n", prog);
    fmt::print("  {} recv <dest> --listen <port> [options]\n", prog);
    fmt::print("\nOptions:\n");
    fmt::print("  --secret <s>  Pre-shared secret for authentication\n");
    fmt::print("  --tls         Enable kTLS encryption (requires --secret)\n");
    fmt::print("  --uring       Use io_uring async batching (faster)\n");
    fmt::print("  --splice      Use zero-copy splice (slower for small files)\n");
    fmt::print("\nEncryption modes:\n");
    fmt::print("  Plaintext:    {} send /data host:9999 --secret key\n", prog);
    fmt::print("  Native kTLS:  {} send /data host:9999 --secret key --tls\n", prog);
    fmt::print("  SSH tunnel:   ssh -L 9999:localhost:9999 host  (then use localhost:9999)\n");
    fmt::print("\nExamples:\n");
    fmt::print("  # Plaintext (trusted network or behind SSH tunnel)\n");
    fmt::print("  {} recv /backup --listen 9999 --secret abc123\n", prog);
    fmt::print("  {} send /data 192.168.1.100:9999 --secret abc123\n", prog);
    fmt::print("\n  # With native kTLS encryption\n");
    fmt::print("  {} recv /backup --listen 9999 --secret abc123 --tls\n", prog);
    fmt::print("  {} send /data 192.168.1.100:9999 --secret abc123 --tls\n", prog);
    fmt::print("\n  # Using SSH tunnel (encryption via SSH)\n");
    fmt::print("  ssh -L 9999:localhost:9999 user@remote-host  # Terminal 1\n");
    fmt::print("  {} recv /backup --listen 9999 --secret abc123  # On remote\n", prog);
    fmt::print("  {} send /data localhost:9999 --secret abc123   # Local\n", prog);
}

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[]) {
    // Check for network mode first (before getopt)
    if (argc >= 2) {
        std::string mode = argv[1];

        if (mode == "send") {
            // Send mode: uring-sync send <source> <host:port> [options]
            std::string secret;
            bool use_splice = false;
            bool use_uring = false;
            bool use_tls = false;
            for (int i = 2; i < argc; i++) {
                if (strcmp(argv[i], "--secret") == 0 && i + 1 < argc) {
                    secret = argv[++i];
                } else if (strcmp(argv[i], "--splice") == 0) {
                    use_splice = true;
                } else if (strcmp(argv[i], "--uring") == 0) {
                    use_uring = true;
                } else if (strcmp(argv[i], "--tls") == 0) {
                    use_tls = true;
                } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                    print_net_usage(argv[0]);
                    return 0;
                }
            }

            if (argc < 4) {
                print_net_usage(argv[0]);
                return 1;
            }

            // Validate --tls requires --secret
            if (use_tls && secret.empty()) {
                fmt::print(stderr, "Error: --tls requires --secret\n");
                return 1;
            }

            std::string src = argv[2];
            std::string host_port = argv[3];
            std::string host;
            uint16_t port;

            if (!parse_host_port(host_port, host, port)) {
                fmt::print(stderr, "Invalid host:port: {}\n", host_port);
                return 1;
            }

            if (use_uring) {
                if (use_tls) {
                    fmt::print(stderr, "Error: --tls + --uring not yet supported. Use --tls without --uring.\n");
                    return 1;
                }
                return run_sender_uring(src, host, port, secret);
            }
            return run_sender(src, host, port, secret, use_splice, use_tls);
        }

        if (mode == "recv") {
            // Recv mode: uring-sync recv <dest> --listen <port> [options]
            std::string dest;
            uint16_t port = 0;
            std::string secret;
            bool use_uring = false;
            bool use_tls = false;

            for (int i = 2; i < argc; i++) {
                if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
                    port = std::atoi(argv[++i]);
                } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
                    port = std::atoi(argv[++i]);
                } else if (strcmp(argv[i], "--secret") == 0 && i + 1 < argc) {
                    secret = argv[++i];
                } else if (strcmp(argv[i], "--uring") == 0) {
                    use_uring = true;
                } else if (strcmp(argv[i], "--tls") == 0) {
                    use_tls = true;
                } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                    print_net_usage(argv[0]);
                    return 0;
                } else if (argv[i][0] != '-' && dest.empty()) {
                    dest = argv[i];
                }
            }

            if (dest.empty() || port == 0) {
                print_net_usage(argv[0]);
                return 1;
            }

            // Validate --tls requires --secret
            if (use_tls && secret.empty()) {
                fmt::print(stderr, "Error: --tls requires --secret\n");
                return 1;
            }

            if (use_uring) {
                if (use_tls) {
                    fmt::print(stderr, "Error: --tls + --uring not yet supported. Use --tls without --uring.\n");
                    return 1;
                }
                return run_receiver_uring(dest, port, secret);
            }
            return run_receiver(dest, port, secret, use_tls);
        }
    }

    // Local copy mode (original behavior)
    Config cfg;

    static struct option long_options[] = {
        {"jobs",       required_argument, nullptr, 'j'},
        {"chunk-size", required_argument, nullptr, 'c'},
        {"queue-depth", required_argument, nullptr, 'q'},
        {"verbose",    no_argument,       nullptr, 'v'},
        {"quiet",      no_argument,       nullptr, 'Q'},
        {"no-splice",  no_argument,       nullptr, 'N'},
        {"sync",       no_argument,       nullptr, 'S'},
        {"help",       no_argument,       nullptr, 'h'},
        {nullptr,      0,                 nullptr,  0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "j:c:q:vQNSh", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'j':
                cfg.num_workers = std::atoi(optarg);
                if (cfg.num_workers <= 0) {
                    fmt::print(stderr, "Error: jobs must be positive\n");
                    return 1;
                }
                break;
            case 'c':
                cfg.chunk_size = std::atoi(optarg);
                cfg.chunk_size_set = true;
                if (cfg.chunk_size <= 0) {
                    fmt::print(stderr, "Error: chunk-size must be positive\n");
                    return 1;
                }
                break;
            case 'q':
                cfg.queue_depth = std::atoi(optarg);
                if (cfg.queue_depth <= 0) {
                    fmt::print(stderr, "Error: queue-depth must be positive\n");
                    return 1;
                }
                break;
            case 'v':
                cfg.verbose = true;
                break;
            case 'Q':
                cfg.quiet = true;
                break;
            case 'N':
                cfg.use_splice = false;
                break;
            case 'S':
                cfg.sync_mode = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (argc - optind < 2) {
        fmt::print(stderr, "Error: missing source and/or destination\n");
        print_usage(argv[0]);
        return 1;
    }

    cfg.src_path = argv[optind];
    cfg.dst_path = argv[optind + 1];

    // Ensure at least 1 worker
    if (cfg.num_workers <= 0) {
        cfg.num_workers = 1;
    }

    // ========================================================
    // Phase 1: Scan files
    // ========================================================
    fmt::print("Scanning files...\n");
    auto scan_result = scan_files(cfg.src_path, cfg.dst_path);
    auto& files = scan_result.files;

    // Sort files by inode for sequential disk access (improves network storage performance)
    // Inode order approximates physical disk allocation order on most filesystems
    std::sort(files.begin(), files.end(), [](const FileWorkItem& a, const FileWorkItem& b) {
        return a.inode < b.inode;
    });

    if (files.empty()) {
        fmt::print(stderr, "No files to copy\n");
        return 1;
    }

    // ========================================================
    // Phase 1.5: Auto-tune chunk size based on file size distribution
    // ========================================================
    if (!cfg.chunk_size_set && !scan_result.size_stats.samples.empty()) {
        cfg.chunk_size = scan_result.size_stats.pick_chunk_size();
        if (cfg.verbose) {
            fmt::print("Auto-tuned chunk size based on file distribution:\n");
            scan_result.size_stats.print_summary();
            fmt::print("  Selected chunk_size: {} bytes\n", cfg.chunk_size);
        }
    }

    if (cfg.sync_mode) {
        fmt::print("Found {} files, using {} workers (SYNC mode)\n",
                   files.size(), cfg.num_workers);
    } else {
        fmt::print("Found {} files, using {} workers (queue_depth={}, chunk_size={})\n",
                   files.size(), cfg.num_workers, cfg.queue_depth, cfg.chunk_size);
    }

    // ========================================================
    // Phase 2: Initialize
    // ========================================================
    Stats stats;
    stats.files_total = files.size();

    WorkQueue<FileWorkItem> work_queue;
    work_queue.push_bulk(files);
    work_queue.set_done();

    // ========================================================
    // Phase 3: Start workers
    // ========================================================
    std::vector<std::thread> workers;
    workers.reserve(cfg.num_workers);

    auto start_time = std::chrono::steady_clock::now();

    for (int i = 0; i < cfg.num_workers; i++) {
        if (cfg.sync_mode) {
            workers.emplace_back(sync_worker_thread, i, std::ref(work_queue),
                                std::ref(stats), std::ref(cfg));
        } else {
            workers.emplace_back(worker_thread, i, std::ref(work_queue),
                                std::ref(stats), std::ref(cfg));
        }
    }

    // ========================================================
    // Phase 4: Progress monitoring (main thread)
    // ========================================================
    if (cfg.quiet) {
        // Just wait for completion without progress output
        for (auto& t : workers) {
            t.join();
        }
    } else {
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            uint64_t completed = stats.files_completed.load();
            uint64_t failed = stats.files_failed.load();
            uint64_t total = stats.files_total.load();
            uint64_t bytes = stats.bytes_copied.load();
            uint64_t bytes_total = stats.bytes_total.load();

            if (completed + failed >= total && work_queue.is_done()) {
                break;
            }

            double pct = bytes_total > 0 ? (100.0 * bytes / bytes_total) : 0;
            fmt::print("\rProgress: {}/{} files, {}/{} bytes ({:.1f}%)     ",
                      completed, total, bytes, bytes_total, pct);
            fflush(stdout);
        }
    }

    // ========================================================
    // Phase 5: Wait for workers (skip if already joined in quiet mode)
    // ========================================================
    if (!cfg.quiet) {
        for (auto& t : workers) {
            t.join();
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // ========================================================
    // Phase 6: Summary
    // ========================================================
    double seconds = duration.count() / 1000.0;
    uint64_t bytes_copied = stats.bytes_copied.load();
    uint64_t files_completed = stats.files_completed.load();
    double bytes_per_sec = seconds > 0 ? bytes_copied / seconds : 0;
    double files_per_sec = seconds > 0 ? files_completed / seconds : 0;

    fmt::print("Completed: {} files, {} in {:.2f}s\n",
               files_completed, format_bytes(bytes_copied), seconds);
    fmt::print("Throughput: {}, {:.0f} files/s\n",
               format_throughput(bytes_per_sec), files_per_sec);

    if (stats.files_failed > 0) {
        fmt::print("Failed: {} files\n", stats.files_failed.load());
        return 1;
    }

    return 0;
}

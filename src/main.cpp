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

namespace fs = std::filesystem;

// ============================================================
// Configuration
// ============================================================
struct Config {
    int num_workers = 1;              // 1 = optimal for local copy (io_uring provides async parallelism)
    int queue_depth = 64;
    int chunk_size = 128 * 1024;      // 128KB
    uint64_t splice_threshold = UINT64_MAX;  // Disabled - splice requires pipe
    bool verbose = false;
    bool preserve = false;
    bool use_registered_buffers = false;  // Disabled - causes issues
    bool use_copy_file_range = true;  // Use copy_file_range (kernel 5.19+) - zero-copy, much faster on network storage
    std::string src_path;
    std::string dst_path;
};

// ============================================================
// Print Helpers
// ============================================================

void print_usage(const char* prog) {
    fmt::print("Usage: {} [options] <source> <destination>\n", prog);
    fmt::print("\nParallel file copier using io_uring\n");
    fmt::print("\nOptions:\n");
    fmt::print("  -j, --jobs <n>            Number of worker threads (default: 1, optimal for local copy)\n");
    fmt::print("  -c, --chunk-size <bytes>  I/O buffer size (default: 131072)\n");
    fmt::print("  -q, --queue-depth <n>     io_uring queue depth per worker (default: 64)\n");
    fmt::print("  -s, --splice-threshold <bytes>  Use splice for files larger than this (default: 1MB)\n");
    fmt::print("  -v, --verbose             Verbose output\n");
    fmt::print("  -p, --preserve            Preserve file permissions\n");
    fmt::print("  -h, --help                Show this help message\n");
    fmt::print("\nExamples:\n");
    fmt::print("  {} file.txt copy.txt                  # Single file\n", prog);
    fmt::print("  {} -j 8 src_dir/ dst_dir/             # 8 workers\n", prog);
    fmt::print("  {} -j 4 -c 262144 src_dir/ dst_dir/   # 4 workers, 256KB chunks\n", prog);
}

// ============================================================
// Directory Scanner
// ============================================================
std::vector<FileWorkItem> scan_files(
    const std::string& src_base,
    const std::string& dst_base
) {
    std::vector<FileWorkItem> files;

    struct stat st;
    if (stat(src_base.c_str(), &st) != 0) {
        fmt::print(stderr, "Error: Cannot access '{}'\n", src_base);
        return files;
    }

    if (S_ISREG(st.st_mode)) {
        files.push_back({src_base, dst_base});
        return files;
    }

    if (!S_ISDIR(st.st_mode)) {
        fmt::print(stderr, "Error: '{}' is not a file or directory\n", src_base);
        return files;
    }

    try {
        fs::create_directories(dst_base);

        for (const auto& entry : fs::recursive_directory_iterator(src_base)) {
            if (entry.is_regular_file()) {
                std::string rel_path = fs::relative(entry.path(), src_base).string();
                std::string src_file = entry.path().string();
                std::string dst_file = (fs::path(dst_base) / rel_path).string();

                fs::create_directories(fs::path(dst_file).parent_path());
                files.push_back({src_file, dst_file});
            }
        }
    } catch (const fs::filesystem_error& e) {
        fmt::print(stderr, "Filesystem error: {}\n", e.what());
    }

    return files;
}

// ============================================================
// State Machine
// ============================================================
void advance_state(FileContext* ctx, int result, RingManager& ring,
                   Stats& stats, const Config& cfg) {
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

            // Decide whether to use splice for large files
            ctx->use_splice = (ctx->file_size >= cfg.splice_threshold);

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
            } else if (cfg.use_copy_file_range) {
                // Use copy_file_range syscall (sync) - zero-copy, data stays in kernel
                // This is what 'cp' uses and is much faster on network storage
                loff_t off_in = 0, off_out = 0;
                ssize_t copied;
                while (off_in < (loff_t)ctx->file_size) {
                    copied = copy_file_range(ctx->src_fd, &off_in,
                                            ctx->dst_fd, &off_out,
                                            ctx->file_size - off_in, 0);
                    if (copied <= 0) {
                        if (copied < 0 && cfg.verbose) {
                            fmt::print(stderr, "copy_file_range failed on {}: {}\n",
                                      ctx->src_path, strerror(errno));
                        }
                        // Fall back to read/write if copy_file_range fails
                        break;
                    }
                    stats.bytes_copied += copied;
                }
                ctx->offset = off_in;

                // Go to closing
                ctx->state = FileState::CLOSING_SRC;
                ctx->current_op = OpType::CLOSE_SRC;
                ring.prepare_close(ctx->src_fd, ctx);
            } else if (ctx->use_splice) {
                // Use splice for large files (zero-copy in kernel, requires pipe)
                ctx->state = FileState::SPLICING;
                uint32_t to_splice = std::min((uint64_t)cfg.chunk_size,
                                              ctx->file_size - ctx->offset);
                ring.prepare_splice(ctx->src_fd, ctx->offset,
                                   ctx->dst_fd, ctx->offset,
                                   to_splice, 0, ctx);
            } else {
                // Fallback: use read/write
                ctx->state = FileState::READING;
                ctx->current_op = OpType::READ;
                uint32_t to_read = std::min((uint64_t)cfg.chunk_size,
                                           ctx->file_size - ctx->offset);
                if (ctx->use_fixed_buffers && ring.buffers_registered()) {
                    ring.prepare_read_fixed(ctx->src_fd, ctx->buffer, to_read,
                                           ctx->offset, ctx->buffer_index, ctx);
                } else {
                    ring.prepare_read(ctx->src_fd, ctx->buffer, to_read, ctx->offset, ctx);
                }
            }
            break;

        case FileState::READING:
            ctx->last_read_size = result;
            ctx->state = FileState::WRITING;
            ctx->current_op = OpType::WRITE;
            if (ctx->use_fixed_buffers && ring.buffers_registered()) {
                ring.prepare_write_fixed(ctx->dst_fd, ctx->buffer, result,
                                        ctx->offset, ctx->buffer_index, ctx);
            } else {
                ring.prepare_write(ctx->dst_fd, ctx->buffer, result, ctx->offset, ctx);
            }
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
                if (ctx->use_fixed_buffers && ring.buffers_registered()) {
                    ring.prepare_read_fixed(ctx->src_fd, ctx->buffer, to_read,
                                           ctx->offset, ctx->buffer_index, ctx);
                } else {
                    ring.prepare_read(ctx->src_fd, ctx->buffer, to_read, ctx->offset, ctx);
                }
            }
            break;

        case FileState::SPLICING:
            // Splice completed
            ctx->offset += result;
            stats.bytes_copied += result;

            if (ctx->offset >= ctx->file_size) {
                ctx->state = FileState::CLOSING_SRC;
                ctx->current_op = OpType::CLOSE_SRC;
                ring.prepare_close(ctx->src_fd, ctx);
            } else {
                // More data to splice
                uint32_t to_splice = std::min((uint64_t)cfg.chunk_size,
                                              ctx->file_size - ctx->offset);
                ring.prepare_splice(ctx->src_fd, ctx->offset,
                                   ctx->dst_fd, ctx->offset,
                                   to_splice, 0, ctx);
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
// Worker Thread
// ============================================================
void worker_thread(int worker_id, WorkQueue<FileWorkItem>& work_queue,
                   Stats& stats, const Config& cfg) {
    // Each worker has its own io_uring and buffer pool
    RingManager ring(cfg.queue_depth);
    BufferPool pool(cfg.queue_depth, cfg.chunk_size);

    // Try to register buffers with the kernel
    bool buffers_registered = false;
    if (cfg.use_registered_buffers) {
        buffers_registered = ring.register_buffers(pool.buffers(), pool.buffer_size());
        if (cfg.verbose && buffers_registered) {
            fmt::print("Worker {}: registered {} buffers\n", worker_id, cfg.queue_depth);
        }
    }

    std::vector<FileContext*> in_flight;
    in_flight.reserve(cfg.queue_depth);

    auto start_file = [&](const FileWorkItem& item) -> bool {
        auto [buffer, buf_idx] = pool.acquire();
        if (!buffer) return false;

        FileContext* ctx = new FileContext();
        ctx->src_path = item.src_path;
        ctx->dst_path = item.dst_path;
        ctx->buffer = buffer;
        ctx->buffer_index = buf_idx;
        ctx->state = FileState::OPENING_SRC;
        ctx->current_op = OpType::OPEN_SRC;
        ctx->use_fixed_buffers = buffers_registered;

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
            advance_state(ctx, result, ring, stats, cfg);

            if (ctx->state == FileState::DONE || ctx->state == FileState::FAILED) {
                pool.release(ctx->buffer_index);

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
// Main
// ============================================================
int main(int argc, char* argv[]) {
    Config cfg;

    static struct option long_options[] = {
        {"jobs",             required_argument, nullptr, 'j'},
        {"chunk-size",       required_argument, nullptr, 'c'},
        {"queue-depth",      required_argument, nullptr, 'q'},
        {"splice-threshold", required_argument, nullptr, 's'},
        {"verbose",          no_argument,       nullptr, 'v'},
        {"preserve",         no_argument,       nullptr, 'p'},
        {"help",             no_argument,       nullptr, 'h'},
        {nullptr,            0,                 nullptr,  0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "j:c:q:s:vph", long_options, nullptr)) != -1) {
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
            case 's':
                cfg.splice_threshold = std::atoll(optarg);
                break;
            case 'v':
                cfg.verbose = true;
                break;
            case 'p':
                cfg.preserve = true;
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
    auto files = scan_files(cfg.src_path, cfg.dst_path);

    if (files.empty()) {
        fmt::print(stderr, "No files to copy\n");
        return 1;
    }

    fmt::print("Found {} files, using {} workers (queue_depth={}, chunk_size={}, splice_threshold={})\n",
               files.size(), cfg.num_workers, cfg.queue_depth, cfg.chunk_size, cfg.splice_threshold);

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
        workers.emplace_back(worker_thread, i, std::ref(work_queue),
                            std::ref(stats), std::ref(cfg));
    }

    // ========================================================
    // Phase 4: Progress monitoring (main thread)
    // ========================================================
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

    // ========================================================
    // Phase 5: Wait for workers
    // ========================================================
    for (auto& t : workers) {
        t.join();
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

#pragma once
#include <liburing.h>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <vector>
#include <sys/uio.h>
#include "common.hpp"

class RingManager {
public:
    RingManager(unsigned int depth) : depth_(depth) {
        if (io_uring_queue_init(depth, &ring, 0) < 0) {
            throw std::runtime_error("Failed to initialize io_uring");
        }
    }

    ~RingManager() {
        if (buffers_registered_) {
            io_uring_unregister_buffers(&ring);
        }
        io_uring_queue_exit(&ring);
    }

    // Non-copyable
    RingManager(const RingManager&) = delete;
    RingManager& operator=(const RingManager&) = delete;

    // ============================================================
    // Buffer Registration (reduces per-I/O overhead)
    // ============================================================

    // Register buffers with the kernel for zero-copy I/O
    bool register_buffers(const std::vector<char*>& buffers, size_t buffer_size) {
        if (buffers_registered_) return false;

        iovecs_.resize(buffers.size());
        for (size_t i = 0; i < buffers.size(); i++) {
            iovecs_[i].iov_base = buffers[i];
            iovecs_[i].iov_len = buffer_size;
        }

        int ret = io_uring_register_buffers(&ring, iovecs_.data(), iovecs_.size());
        if (ret < 0) {
            iovecs_.clear();
            return false;
        }

        buffers_registered_ = true;
        return true;
    }

    // ============================================================
    // File Operations
    // ============================================================

    // Open a file asynchronously (with optional linking)
    void prepare_openat(int dirfd, const char* path, int flags, mode_t mode,
                        FileContext* ctx, bool link = false) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_openat(sqe, dirfd, path, flags, mode);
        io_uring_sqe_set_data(sqe, ctx);
        if (link) sqe->flags |= IOSQE_IO_LINK;
    }

    // Get file metadata asynchronously (with optional linking)
    void prepare_statx(int dirfd, const char* path, int flags, unsigned int mask,
                       struct statx* statxbuf, FileContext* ctx, bool link = false) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_statx(sqe, dirfd, path, flags, mask, statxbuf);
        io_uring_sqe_set_data(sqe, ctx);
        if (link) sqe->flags |= IOSQE_IO_LINK;
    }

    // Close a file descriptor asynchronously
    void prepare_close(int fd, FileContext* ctx, bool link = false) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_close(sqe, fd);
        io_uring_sqe_set_data(sqe, ctx);
        if (link) sqe->flags |= IOSQE_IO_LINK;
    }

    // Read from file (using fd)
    void prepare_read(int fd, char* buffer, unsigned len, uint64_t offset,
                      FileContext* ctx, bool link = false) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_read(sqe, fd, buffer, len, offset);
        io_uring_sqe_set_data(sqe, ctx);
        if (link) sqe->flags |= IOSQE_IO_LINK;
    }

    // Read using registered buffer (zero-copy from kernel perspective)
    void prepare_read_fixed(int fd, char* buffer, unsigned len, uint64_t offset,
                            int buf_index, FileContext* ctx, bool link = false) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_read_fixed(sqe, fd, buffer, len, offset, buf_index);
        io_uring_sqe_set_data(sqe, ctx);
        if (link) sqe->flags |= IOSQE_IO_LINK;
    }

    // Write to file (using fd)
    void prepare_write(int fd, char* buffer, unsigned len, uint64_t offset,
                       FileContext* ctx, bool link = false) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_write(sqe, fd, buffer, len, offset);
        io_uring_sqe_set_data(sqe, ctx);
        if (link) sqe->flags |= IOSQE_IO_LINK;
    }

    // Write using registered buffer
    void prepare_write_fixed(int fd, char* buffer, unsigned len, uint64_t offset,
                             int buf_index, FileContext* ctx, bool link = false) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_write_fixed(sqe, fd, buffer, len, offset, buf_index);
        io_uring_sqe_set_data(sqe, ctx);
        if (link) sqe->flags |= IOSQE_IO_LINK;
    }

    // Splice/copy_file_range - kernel-to-kernel zero copy for large files
    void prepare_splice(int fd_in, int64_t off_in, int fd_out, int64_t off_out,
                        unsigned int len, unsigned int flags, FileContext* ctx) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_splice(sqe, fd_in, off_in, fd_out, off_out, len, flags);
        io_uring_sqe_set_data(sqe, ctx);
    }

    // Create directory asynchronously
    void prepare_mkdirat(int dirfd, const char* path, mode_t mode, FileContext* ctx) {
        struct io_uring_sqe* sqe = get_sqe();
        io_uring_prep_mkdirat(sqe, dirfd, path, mode);
        io_uring_sqe_set_data(sqe, ctx);
    }

    // ============================================================
    // Submission and Completion
    // ============================================================

    int submit() {
        return io_uring_submit(&ring);
    }

    FileContext* wait_one(int& res_out) {
        struct io_uring_cqe* cqe;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) return nullptr;

        FileContext* ctx = static_cast<FileContext*>(io_uring_cqe_get_data(cqe));
        res_out = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
        return ctx;
    }

    template<typename Callback>
    int process_completions(Callback&& callback) {
        struct io_uring_cqe* cqe;
        int processed = 0;

        while (io_uring_peek_cqe(&ring, &cqe) == 0) {
            FileContext* ctx = static_cast<FileContext*>(io_uring_cqe_get_data(cqe));
            int res = cqe->res;
            io_uring_cqe_seen(&ring, cqe);

            callback(ctx, res);
            processed++;
        }
        return processed;
    }

    template<typename Callback>
    int wait_and_process(Callback&& callback) {
        struct io_uring_cqe* cqe;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) return -1;

        FileContext* ctx = static_cast<FileContext*>(io_uring_cqe_get_data(cqe));
        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
        callback(ctx, res);

        return 1 + process_completions(std::forward<Callback>(callback));
    }

    bool has_sqe_space() const {
        return io_uring_sq_space_left(&ring) > 0;
    }

    unsigned int depth() const { return depth_; }
    bool buffers_registered() const { return buffers_registered_; }

private:
    struct io_uring ring;
    unsigned int depth_;
    bool buffers_registered_ = false;
    std::vector<struct iovec> iovecs_;

    struct io_uring_sqe* get_sqe() {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (!sqe) {
            io_uring_submit(&ring);
            sqe = io_uring_get_sqe(&ring);
            if (!sqe) {
                throw std::runtime_error("Submission Queue is full!");
            }
        }
        return sqe;
    }
};

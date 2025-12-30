#pragma once
#include <string>
#include <cstdint>
#include <fmt/core.h>

// Format bytes with auto-adaptive units (B, KB, MB, GB, TB)
inline std::string format_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_idx < 4) {
        size /= 1024.0;
        unit_idx++;
    }

    if (unit_idx == 0) {
        return fmt::format("{} {}", bytes, units[unit_idx]);
    }
    return fmt::format("{:.2f} {}", size, units[unit_idx]);
}

// Format throughput with auto-adaptive units (B/s, KB/s, MB/s, GB/s, TB/s)
inline std::string format_throughput(double bytes_per_sec) {
    const char* units[] = {"B/s", "KB/s", "MB/s", "GB/s", "TB/s"};
    int unit_idx = 0;
    double rate = bytes_per_sec;

    while (rate >= 1024.0 && unit_idx < 4) {
        rate /= 1024.0;
        unit_idx++;
    }

    return fmt::format("{:.2f} {}", rate, units[unit_idx]);
}

#include <gtest/gtest.h>
#include "utils.hpp"

// ============================================================
// format_bytes Tests
// ============================================================

TEST(FormatBytesTest, Zero) {
    EXPECT_EQ(format_bytes(0), "0 B");
}

TEST(FormatBytesTest, Bytes) {
    EXPECT_EQ(format_bytes(1), "1 B");
    EXPECT_EQ(format_bytes(100), "100 B");
    EXPECT_EQ(format_bytes(1023), "1023 B");
}

TEST(FormatBytesTest, Kilobytes) {
    EXPECT_EQ(format_bytes(1024), "1.00 KB");
    EXPECT_EQ(format_bytes(1536), "1.50 KB");
    EXPECT_EQ(format_bytes(10240), "10.00 KB");
    EXPECT_EQ(format_bytes(1024 * 1023), "1023.00 KB");
}

TEST(FormatBytesTest, Megabytes) {
    EXPECT_EQ(format_bytes(1024 * 1024), "1.00 MB");
    EXPECT_EQ(format_bytes(1024 * 1024 * 10), "10.00 MB");
    EXPECT_EQ(format_bytes(1024 * 1024 * 100), "100.00 MB");
    EXPECT_EQ(format_bytes(static_cast<uint64_t>(1024) * 1024 * 1023), "1023.00 MB");
}

TEST(FormatBytesTest, Gigabytes) {
    EXPECT_EQ(format_bytes(static_cast<uint64_t>(1024) * 1024 * 1024), "1.00 GB");
    EXPECT_EQ(format_bytes(static_cast<uint64_t>(1024) * 1024 * 1024 * 10), "10.00 GB");
}

TEST(FormatBytesTest, Terabytes) {
    EXPECT_EQ(format_bytes(static_cast<uint64_t>(1024) * 1024 * 1024 * 1024), "1.00 TB");
    EXPECT_EQ(format_bytes(static_cast<uint64_t>(1024) * 1024 * 1024 * 1024 * 5), "5.00 TB");
}

TEST(FormatBytesTest, RealWorldSizes) {
    // Common file sizes
    EXPECT_EQ(format_bytes(14), "14 B");  // "Hello, World!\n"
    EXPECT_EQ(format_bytes(4096), "4.00 KB");  // 4KB block
    EXPECT_EQ(format_bytes(65536), "64.00 KB");  // 64KB chunk
    EXPECT_EQ(format_bytes(1048576), "1.00 MB");  // 1MB
    EXPECT_EQ(format_bytes(10485760), "10.00 MB");  // 10MB
}

// ============================================================
// format_throughput Tests
// ============================================================

TEST(FormatThroughputTest, Zero) {
    EXPECT_EQ(format_throughput(0), "0.00 B/s");
}

TEST(FormatThroughputTest, BytesPerSecond) {
    EXPECT_EQ(format_throughput(1), "1.00 B/s");
    EXPECT_EQ(format_throughput(100), "100.00 B/s");
    EXPECT_EQ(format_throughput(500), "500.00 B/s");
}

TEST(FormatThroughputTest, KilobytesPerSecond) {
    EXPECT_EQ(format_throughput(1024), "1.00 KB/s");
    EXPECT_EQ(format_throughput(10240), "10.00 KB/s");
    EXPECT_EQ(format_throughput(102400), "100.00 KB/s");
}

TEST(FormatThroughputTest, MegabytesPerSecond) {
    EXPECT_EQ(format_throughput(1024 * 1024), "1.00 MB/s");
    EXPECT_EQ(format_throughput(1024 * 1024 * 10), "10.00 MB/s");
    EXPECT_EQ(format_throughput(1024 * 1024 * 100), "100.00 MB/s");
}

TEST(FormatThroughputTest, GigabytesPerSecond) {
    EXPECT_EQ(format_throughput(static_cast<double>(1024) * 1024 * 1024), "1.00 GB/s");
    EXPECT_EQ(format_throughput(static_cast<double>(1024) * 1024 * 1024 * 10), "10.00 GB/s");
}

TEST(FormatThroughputTest, RealWorldThroughputs) {
    // Typical throughputs
    EXPECT_EQ(format_throughput(57.69), "57.69 B/s");  // Small file
    EXPECT_EQ(format_throughput(1500), "1.46 KB/s");  // Slow network
    EXPECT_EQ(format_throughput(10 * 1024 * 1024), "10.00 MB/s");  // HDD
    EXPECT_EQ(format_throughput(500.0 * 1024 * 1024), "500.00 MB/s");  // SSD
    EXPECT_EQ(format_throughput(3.0 * 1024 * 1024 * 1024), "3.00 GB/s");  // NVMe
}

TEST(FormatThroughputTest, FractionalValues) {
    EXPECT_EQ(format_throughput(1.5), "1.50 B/s");
    EXPECT_EQ(format_throughput(1536), "1.50 KB/s");
    EXPECT_EQ(format_throughput(1.5 * 1024 * 1024), "1.50 MB/s");
}

// ============================================================
// Edge Cases
// ============================================================

TEST(FormatBytesTest, LargeValues) {
    // Max uint64_t should not crash
    uint64_t large = UINT64_MAX;
    std::string result = format_bytes(large);
    EXPECT_FALSE(result.empty());
    // Should be in TB range
    EXPECT_TRUE(result.find("TB") != std::string::npos);
}

TEST(FormatThroughputTest, VerySmall) {
    EXPECT_EQ(format_throughput(0.01), "0.01 B/s");
    EXPECT_EQ(format_throughput(0.001), "0.00 B/s");
}

TEST(FormatThroughputTest, Negative) {
    // Shouldn't happen in practice, but shouldn't crash
    std::string result = format_throughput(-100);
    EXPECT_FALSE(result.empty());
}

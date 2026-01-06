# Beating rsync by 58% with Kernel TLS

I built a file transfer tool that's **58% faster than rsync** for large dataset transfers. The secret? Moving encryption from userspace SSH into the Linux kernel using kTLS.

## The Problem: SSH is the Bottleneck

When transferring ML datasets between machines, rsync over SSH is the go-to tool:

```bash
rsync -az /data/ml_dataset user@server:/backup/
```

It works, but it's slow. For a 9.7GB dataset (100K files), rsync took **390 seconds**—a throughput of just 25 MB/s.

The bottleneck isn't the network. It's **encryption in userspace**.

```
┌─────────┐     ┌─────────┐     ┌─────────┐     ┌─────────┐
│  File   │────▶│ rsync   │────▶│  SSH    │────▶│ Network │
│  Read   │     │ (delta) │     │ encrypt │     │  Send   │
└─────────┘     └─────────┘     └─────────┘     └─────────┘
                                     │
                              Context switches,
                              userspace copies,
                              CPU-bound AES
```

Every byte passes through the SSH process, which encrypts it using OpenSSL in userspace. This involves:
- Multiple context switches between kernel and userspace
- Copying data between kernel buffers and userspace buffers
- CPU time for AES encryption (even with AES-NI)

## The Solution: kTLS (Kernel TLS)

Linux 4.13+ supports **kTLS**—TLS encryption handled directly in the kernel. Once you set up the TLS session, the kernel encrypts data as it flows through the socket.

```
┌─────────┐     ┌─────────┐     ┌──────────────────┐
│  File   │────▶│  read   │────▶│ Socket (kTLS)    │
│         │     │         │     │ encrypt + send   │
└─────────┘     └─────────┘     └──────────────────┘
                                        │
                                 One kernel operation,
                                 no userspace copies,
                                 AES-NI in kernel
```

Benefits:
1. **No userspace encryption process** - kernel handles it directly
2. **Fewer copies** - data doesn't bounce through userspace
3. **AES-NI in kernel** - hardware acceleration without context switches

## Implementation

Setting up kTLS requires:

1. **TLS handshake** - Exchange keys (we use a pre-shared secret + HKDF)
2. **Configure kernel** - `setsockopt(SOL_TLS, TLS_TX, ...)` with cipher keys
3. **Send data** - Regular `send()` calls, kernel encrypts automatically

```cpp
// After deriving keys from shared secret...
struct tls12_crypto_info_aes_gcm_128 crypto_info = {
    .info.version = TLS_1_2_VERSION,
    .info.cipher_type = TLS_CIPHER_AES_GCM_128,
};
memcpy(crypto_info.key, key, 16);
memcpy(crypto_info.iv, iv, 8);
memcpy(crypto_info.salt, salt, 4);

setsockopt(sock, SOL_TLS, TLS_TX, &crypto_info, sizeof(crypto_info));
// Now all send() calls are automatically encrypted!
```

## Benchmark Results

Testing on real network: Laptop → GCP VM (public internet)

### The Headline Number

| Dataset | uring-sync + kTLS | rsync (SSH) | Improvement |
|---------|-------------------|-------------|-------------|
| ml_small (60MB, 10K files) | 2.98s | 2.63s | ~equal |
| ml_large (589MB, 100K files) | **16.4s** | 24.8s | **34% faster** |
| ml_images (9.7GB, 100K files) | **165s** | 390s | **58% faster** |

### The Pattern

```
Data size:    60MB  →  589MB  →   9.7GB
Improvement:   0%   →   34%   →    58%
```

**The larger the transfer, the bigger the kTLS advantage.**

Why? Per-connection overhead (handshake, key derivation) is amortized over more data. And SSH's userspace encryption overhead grows linearly with data size.

### Throughput Comparison

| Method | Throughput | CPU Usage |
|--------|------------|-----------|
| rsync (SSH) | 25 MB/s | High (userspace encryption) |
| uring-sync + kTLS | **60 MB/s** | Low (kernel encryption) |

kTLS achieves **2.4x the throughput** of rsync while using less CPU.

## Why Not Zero-Copy Splice?

In theory, kTLS supports splice() for true zero-copy transfers:

```
File → Pipe → kTLS Socket (no userspace copies!)
```

I implemented this and expected it to be fastest. Instead, it was **2.9x slower**.

### The Investigation

Using strace, I found the problem:

```
splice(file→pipe):   27μs    ← instant
splice(pipe→socket): 33ms    ← 1000x slower!
```

The `splice(pipe → kTLS socket)` call **blocks** waiting for TCP ACKs. The kernel can't buffer aggressively like it does with regular `send()` calls.

### The Lesson

Zero-copy isn't always faster. For many-file workloads:
- **read/send**: Kernel manages buffering efficiently
- **splice**: Blocks on each chunk, killing throughput

Splice might help for single huge files, but for ML datasets (many small files), stick with read/send.

## When to Use This

**Use kTLS file transfer when:**
- Transferring large datasets (>500MB)
- Network has bandwidth to spare
- You control both endpoints
- Security is required (not just over VPN)

**Stick with rsync when:**
- You need delta sync (only changed bytes)
- Destination already has partial data
- SSH infrastructure is already in place
- Simplicity matters more than speed

## The Protocol

Our wire protocol is minimal:

```
HELLO (secret hash) ──────────────────▶ Verify
                    ◀────────────────── HELLO_OK (+ enable kTLS)

FILE_HDR (path, size, mode) ──────────▶ Create file
FILE_DATA (chunks) ────────────────────▶ Write data
FILE_END ──────────────────────────────▶ Close file

(repeat for all files)

ALL_DONE ──────────────────────────────▶ Complete
```

No delta encoding, no checksums (kTLS provides integrity via GCM). Just raw file transfer with authentication and encryption.

## Code

Usage:

```bash
# Receiver (on remote host)
uring-sync recv /backup --listen 9999 --secret mykey --tls

# Sender (on local host)
uring-sync send /data remote-host:9999 --secret mykey --tls
```

The implementation uses:
- HKDF for key derivation from shared secret
- AES-128-GCM via kTLS
- Simple TCP protocol (no HTTP, no gRPC)

Full source: [github.com/VincentDu2021/uring_sync](https://github.com/VincentDu2021/uring_sync)

## Conclusion

By moving encryption from userspace SSH to kernel kTLS, we achieved:

- **58% faster** than rsync for large transfers
- **2.4x throughput** (60 MB/s vs 25 MB/s)
- **Lower CPU usage** (kernel AES-NI vs userspace OpenSSL)

The key insight: for bulk data transfer, SSH's flexibility is overhead. A purpose-built tool with kernel encryption wins.

---

## Appendix: Full Benchmark Data

### Test Environment
- Sender: Ubuntu laptop, local NVMe
- Receiver: GCP VM (us-central1-a)
- Network: Public internet
- All tests with cold cache (`echo 3 > /proc/sys/vm/drop_caches`)

### Raw Results

| Dataset | Files | Size | kTLS Time | kTLS Speed | rsync Time | rsync Speed |
|---------|-------|------|-----------|------------|------------|-------------|
| ml_small | 10K | 60MB | 2.98s | 20 MB/s | 2.63s | 23 MB/s |
| ml_large | 100K | 589MB | 16.4s | 36 MB/s | 24.8s | 24 MB/s |
| ml_images | 100K | 9.7GB | 165s | 60 MB/s | 390s | 25 MB/s |

### Splice Investigation (ml_images)

| Mode | Time | Speed | Notes |
|------|------|-------|-------|
| Plaintext + read/send | 146s | 68 MB/s | Fastest (no encryption) |
| Plaintext + splice | 157s | 63 MB/s | +8% overhead |
| kTLS + read/send | 165s | 60 MB/s | +13% (encryption cost) |
| kTLS + splice | 428s | 23 MB/s | 2.9x slower (broken) |

---

*Benchmarks run January 2026. Your mileage may vary depending on network conditions and hardware.*

---

**Tags:** #linux #ktls #tls #rsync #performance #networking #encryption

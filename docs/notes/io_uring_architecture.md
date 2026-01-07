# io_uring System Architecture

## Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                        YOUR PROCESS                                  │
│                                                                      │
│  ┌────────────────────────────────────────────────────────────┐     │
│  │  liburing (userspace helper library)                        │     │
│  │                                                             │     │
│  │  io_uring_get_sqe()   → get empty SQE slot                 │     │
│  │  io_uring_prep_read() → fill in the SQE fields             │     │
│  │  io_uring_submit()    → syscall: tell kernel "go"          │     │
│  │  io_uring_wait_cqe()  → wait for completion                │     │
│  └────────────────────────────────────────────────────────────┘     │
│                                                                      │
│  ┌────────────────────────────────────────────────────────────┐     │
│  │  Submission Queue (SQ) - shared memory, mmap'd              │     │
│  │  ┌────────────┬────────────┬────────────┬────────────┐     │     │
│  │  │   SQE 0    │   SQE 1    │   SQE 2    │   SQE 3    │     │     │
│  │  │ op=READ    │ op=READ    │ op=WRITE   │ op=CLOSE   │     │     │
│  │  │ fd=5       │ fd=6       │ fd=7       │ fd=5       │     │     │
│  │  │ addr=──────────────┐    │            │            │     │     │
│  │  │ len=4096   │       │    │            │            │     │     │
│  │  └────────────┴───────│────┴────────────┴────────────┘     │     │
│  └───────────────────────│────────────────────────────────────┘     │
│                          │                                           │
│                          ▼  (pointer, not data copy!)                │
│  ┌────────────────────────────────────────────────────────────┐     │
│  │  BufferPool (your allocation via aligned_alloc)             │     │
│  │  ┌────────────────────────────────────────────────────┐    │     │
│  │  │ buffer[0]: 4KB  ← kernel writes file data HERE     │    │     │
│  │  │ buffer[1]: 4KB                                      │    │     │
│  │  │ buffer[2]: 4KB                                      │    │     │
│  │  │ buffer[3]: 4KB                                      │    │     │
│  │  └────────────────────────────────────────────────────┘    │     │
│  └────────────────────────────────────────────────────────────┘     │
│                                                                      │
│  ┌────────────────────────────────────────────────────────────┐     │
│  │  Completion Queue (CQ) - kernel writes results here         │     │
│  │  ┌──────────┬──────────┬──────────┬──────────┐             │     │
│  │  │  CQE 0   │  CQE 1   │  CQE 2   │  CQE 3   │  16B each   │     │
│  │  │ res=4096 │ res=1024 │ res=0    │          │             │     │
│  │  │ user_data│ user_data│ user_data│          │             │     │
│  │  └──────────┴──────────┴──────────┴──────────┘             │     │
│  └────────────────────────────────────────────────────────────┘     │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
                                 │
                    io_uring_submit()  ← ONE syscall for all SQEs
                                 │
                                 ▼
┌──────────────────────────────────────────────────────────────────────┐
│                           KERNEL                                      │
│                                                                       │
│   1. Reads SQEs from shared memory                                   │
│   2. Performs I/O (disk read/write)                                  │
│   3. Writes data directly to YOUR buffers (via addr pointer)         │
│   4. Posts CQE with result code (bytes read, or -errno)              │
│                                                                       │
└──────────────────────────────────────────────────────────────────────┘
```

## SQE Structure (64 bytes)

```
┌──────────────────────────────────────────────────────────────┐
│                  Submission Queue Entry                       │
├──────────────────────────────────────────────────────────────┤
│  opcode     (1B)   │ Operation: READ=22, WRITE=23, OPEN=18  │
│  flags      (1B)   │ IOSQE_IO_LINK, IOSQE_FIXED_FILE, etc.  │
│  ioprio     (2B)   │ I/O priority                            │
│  fd         (4B)   │ File descriptor                         │
│  off        (8B)   │ Offset in file                          │
│  addr       (8B)   │ Buffer pointer ──────────► YOUR BUFFER  │
│  len        (4B)   │ Number of bytes                         │
│  user_data  (8B)   │ Your tag (returned in CQE)              │
│  ...        (28B)  │ Other fields (flags, buf_index, etc.)   │
└──────────────────────────────────────────────────────────────┘
                              │
                              │ SQE.addr is a POINTER
                              ▼
                     ┌─────────────────┐
                     │  Your Buffer    │
                     │  4KB, 64KB,     │
                     │  any size       │
                     └─────────────────┘
```

## CQE Structure (16 bytes)

```
┌──────────────────────────────────────────────────────────────┐
│                  Completion Queue Entry                       │
├──────────────────────────────────────────────────────────────┤
│  user_data  (8B)   │ Your tag from SQE (to identify op)      │
│  res        (4B)   │ Result: bytes transferred, or -errno    │
│  flags      (4B)   │ IORING_CQE_F_* flags                    │
└──────────────────────────────────────────────────────────────┘
```

## Data Flow for File Copy

```
    Your App                    Kernel                     Disk
       │                          │                          │
       │  1. get_sqe()            │                          │
       │  2. prep_read(fd,buf)    │                          │
       │  3. submit() ──────────► │                          │
       │     [syscall]            │  4. read from disk ────► │
       │                          │  ◄──── data ─────────────│
       │                          │                          │
       │  ◄─── write to buf ──────│                          │
       │                          │                          │
       │  ◄─── post CQE ──────────│                          │
       │                          │                          │
       │  5. wait_cqe()           │                          │
       │  6. use data in buf      │                          │
       ▼                          ▼                          ▼
```

## Memory Layout

```
Virtual Address Space
─────────────────────

0x7fff00000000  ┌─────────────────────────┐
                │   Stack                  │
                └─────────────────────────┘

0x7f8000000000  ┌─────────────────────────┐  ◄── mmap'd shared memory
                │   SQ Ring (N × 64B)     │      kernel + userspace
                │   CQ Ring (2N × 16B)    │      both see this
                └─────────────────────────┘

0x600000000000  ┌─────────────────────────┐  ◄── Your allocation
                │   BufferPool            │      aligned_alloc()
                │   buffer[0]: 4KB        │      for O_DIRECT
                │   buffer[1]: 4KB        │
                │   buffer[2]: 4KB        │
                │   buffer[3]: 4KB        │
                └─────────────────────────┘

0x400000000000  ┌─────────────────────────┐
                │   Code (.text)          │
                └─────────────────────────┘
```

## Key Concepts

| Concept | Size | Purpose |
|---------|------|---------|
| **SQ (Submission Queue)** | N × 64 bytes | Holds SQE instructions |
| **CQ (Completion Queue)** | 2N × 16 bytes | Holds CQE results |
| **SQE** | 64 bytes fixed | Describes one I/O operation |
| **CQE** | 16 bytes fixed | Result of one I/O operation |
| **Data Buffer** | Any size (you choose) | Where actual data lives |

## Why This is Fast

1. **Shared memory**: No copying SQEs to kernel - kernel reads directly
2. **Batching**: Fill N SQEs, submit with ONE syscall
3. **Async**: Submit and continue working, harvest completions later
4. **Zero-copy potential**: With registered buffers, kernel can DMA directly

## Common Opcodes

| Opcode | Value | Description |
|--------|-------|-------------|
| `IORING_OP_OPENAT` | 18 | Open a file |
| `IORING_OP_CLOSE` | 19 | Close a file descriptor |
| `IORING_OP_STATX` | 21 | Get file metadata |
| `IORING_OP_READ` | 22 | Read from file |
| `IORING_OP_WRITE` | 23 | Write to file |

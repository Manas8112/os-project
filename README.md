# Operating Systems Term Project: xv6-riscv (Project 0 & Project 6)

This repository contains our complete implementations and test suites for two Operating Systems term projects built on **xv6-riscv**:

- **Project 0 — Working with FUSE (`xv6_fuse_project0/`)**: Linux userspace C driver (`libfuse3`) mounting xv6's `fs.img` with full read/write support (`getattr`, `readdir`, `read`, `write`, `create`, `mkdir`, `unlink`, `truncate`), bitmap & singly-indirect block management, and a 12-invariant `fsck` checker (`xv6_fsck`).
- **Project 6 — Copy-on-Write Fork (`xv6-riscv/`)**: Kernel-level Copy-on-Write `fork()` with spinlock-protected per-frame reference counting (`kernel/kalloc.c`), `PTE_COW` mapping (`kernel/riscv.h`, `kernel/vm.c`), store page fault resolution (`scause == 15` in `kernel/trap.c`), COW-aware `copyout()`, `cowstats`/`freemem` syscalls, and tests (`cowtest`, `memtest`, `usertests`).

## Prerequisites (Ubuntu / Debian)

    sudo apt update
    sudo apt install -y build-essential git gdb-multiarch qemu-system-misc \
      gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu fuse3 libfuse3-dev pkg-config

## Quick Start: Project 6 (COW Fork)

    cd xv6-riscv
    make clean && make qemu

Inside xv6 (`$`): run `cowtest`, `memtest`, and `usertests`. Exit QEMU with `Ctrl+A` then `X`.

## Quick Start: Project 0 (FUSE Driver & `xv6_fsck`)

> Always exit QEMU before mounting `fs.img`, and run `fusermount3 -u ./mnt` before booting `make qemu`.

    cd xv6-riscv && make fs.img && cd ..
    cd xv6_fuse_project0 && make
    ./xv6_fsck ../xv6-riscv/fs.img
    mkdir -p mnt
    ./xv6_fuse ../xv6-riscv/fs.img ./mnt
    ls -la ./mnt
    fusermount3 -u ./mnt
    ./xv6_fsck ../xv6-riscv/fs.img

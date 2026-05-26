# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository Overview

This is the Linux kernel source tree, currently at **v7.1-rc3**. Only **arm** and **arm64** architectures are present (all others, including x86, were removed). The repo is used for studying and annotating the kernel — recent commits add detailed Chinese-language comments to subsystems like DRM, IRQ, IPC, I2C, SPI, ASoC, regmap, and the boot path.

## Build Commands

```bash
# Configure for arm64 (the default target in this repo)
make ARCH=arm64 defconfig

# Build the kernel image
make ARCH=arm64 -j$(nproc)

# Build modules
make ARCH=arm64 modules

# Verbose build (see full gcc/ld invocations)
make ARCH=arm64 V=1 -j$(nproc)

# Generate compile_commands.json for LSP/clangd
make ARCH=arm64 defconfig
./scripts/clang-tools/gen_compile_commands.py

# Build documentation
make htmldocs

# Clean build artifacts
make clean          # remove most generated files
make mrproper       # full clean including .config
```

## Architecture

### Top-level directory layout

| Directory | Purpose |
|-----------|---------|
| `arch/arm/`, `arch/arm64/` | Architecture-specific code (only these two arches exist) |
| `kernel/` | Core kernel: scheduling, irq, cgroup, audit, bpf |
| `mm/` | Memory management: page allocator, CMA, compaction, DAMON |
| `fs/` | Filesystems: VFS layer + individual filesystems (btrfs, ext4, etc.) |
| `drivers/` | Device drivers organized by bus/class (block, net, char, gpu, i2c, spi, etc.) |
| `net/` | Networking stack |
| `block/` | Block I/O layer (bio, I/O schedulers like BFQ) |
| `include/` | Kernel headers: `include/linux/` (core), `include/uapi/` (userspace ABI) |
| `security/` | Linux Security Modules (LSM) |
| `sound/` | ALSA/ASoC audio subsystem |
| `scripts/` | Build tooling: kbuild helpers, `checkpatch.pl`, `coccicheck` |
| `tools/` | Userspace tools: perf, objtool, selftests under `tools/testing/selftests/` |
| `init/` | Boot initialization: `main.c` contains `start_kernel()` |
| `lib/` | Kernel library routines (sort, crypto, decompress, debug Kconfig) |
| `ipc/` | System V IPC (msg, sem, shm) and POSIX message queues |
| `rust/` | Rust-for-Linux infrastructure |
| `Documentation/` | RST documentation, organized by subsystem |
| `io_uring/` | io_uring async I/O |

### Boot flow (arm64)

```
arch/arm64/kernel/head.S:primary_entry
  → setup_arch()          [arch/arm64/kernel/setup.c]
  → start_kernel()        [init/main.c]
  → rest_init()           [init/main.c]
  → kernel_init()         [init/main.c]
  → userspace init
```

### kbuild system

The build system uses recursive make with `scripts/Makefile.build` driving per-directory compilation. `obj-y` lists objects built into vmlinux; `obj-m` lists loadable modules. Subsystem configuration comes from `Kconfig` files processed by `scripts/kconfig/`. Each directory's `Kbuild` or `Makefile` defines what gets built.

### Key Kconfig hierarchy

```
init/Kconfig → kernel/Kconfig.freezer → mm/Kconfig → net/Kconfig → drivers/Kconfig → fs/Kconfig → security/Kconfig → crypto/Kconfig → lib/Kconfig → lib/Kconfig.debug
```

## Code Style and Conventions

- Follow `Documentation/process/coding-style.rst` (Linux kernel coding style)
- Indentation: 8-column tabs (never spaces for indentation)
- Line length: 80 columns soft, 100 hard limit
- Kernel-doc comments (`/** ... */`) for API documentation
- Use `checkpatch.pl` before submitting: `./scripts/checkpatch.pl --file <path>`

## Subsystem Navigation

- **Find maintainers**: `./scripts/get_maintainer.pl <file>` or search `MAINTAINERS`
- **Find symbol definitions**: Use `cscope` or `make TAGS` / `make cscope`
- **Kernel config search**: `make menuconfig` (ncurses) or search Kconfig files directly
- **Chinese annotations**: Already-commented subsystems include boot init, IPC, IRQ, I2C, SPI, DRM, ASoC, regmap

## Testing

```bash
# Run subsystem selftests (example: net)
make -C tools/testing/selftests/net run_tests

# Run kselftests (requires kernel config with kselftest enabled)
make kselftest-merge
make -j$(nproc)
# then run individual tests from tools/testing/selftests/
```

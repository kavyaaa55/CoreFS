# CoreFS

A file system built from scratch in C++20 — its own on-disk format, its own allocator, its own directory structure, and a shell to interact with it. No delegation to the host OS's file system beyond raw byte access to a single disk-image file.

## Status

| Layer | Status |
|---|---|
| On-disk format (superblock, bitmaps, inode table) | ✅ Done |
| Block / inode allocator | ✅ Done |
| Directory structure (dirent lookup, add, remove) | ✅ Done |
| `mkfs` tool | ✅ Done |
| Shell: `pwd`, `ls`, `cd`, `mkdir`, `touch`, `stat` | ✅ Done |
| Shell: `cat`, `write`, `echo`, `rm`, `rmdir`, `cp`, `mv`, `df` | ✅ Done |
| Indirect blocks (single + double) for large files | ⏳ Phase 6 |
| `.`/`..` on-disk, timestamps, mode, hard/symlinks | ⏳ Phase 7 |
| Test suite (unit + model-based fuzz) + `fsck` | ⏳ Phase 8 |
| Buffer cache (write-back LRU) | ⏳ Phase 9 |
| Concurrency (per-inode locking, benchmarked) | ⏳ Phase 10 |
| Journaling + crash-sweep tests | ⏳ Phase 11 |
| FUSE mount | ⏳ Phase 12 |
| Benchmarks + documentation | ⏳ Phase 13 |

---

## Architecture

```
┌─────────────────────────┐
│   Shell (REPL)          │  ← all user commands
├─────────────────────────┤
│   Directory             │  ← path resolution, dirent lookup/add/remove
├─────────────────────────┤
│   FSCore                │  ← bitmap allocator, inode I/O, read/write/truncate
├─────────────────────────┤
│   Disk                  │  ← raw block read/write on the image file
├─────────────────────────┤
│   disk.img (flat file)  │  ← the "hardware"
└─────────────────────────┘
```

**On-disk layout:**

```
Block 0              Superblock
Block 1              Inode bitmap
Block 2              Data bitmap
Blocks 3..34         Inode table  (32 blocks for 1024 inodes at 128 bytes each)
Blocks 35..end       Data blocks
```

Each inode has 12 direct block pointers (4 KB blocks → max file size 48 KB in Phase 5).
Directories store arrays of `(inode_number, name)` dirents in their data blocks.
`.` and `..` are **not** stored on disk — `cd` navigation is tracked in-memory as a path stack.

---

## Prerequisites

| Tool | Minimum version | Notes |
|---|---|---|
| C++ compiler | C++20 | g++ 10+, clang++ 14+, MSVC 19.29+ |
| CMake | 3.16 | Optional — a `build.ps1` script is provided for Windows |
| MSYS2 (Windows) | any | Provides g++ via the `ucrt64` environment |

---

## Build

### Windows (MSYS2 UCRT64) — recommended

Install MSYS2 from https://www.msys2.org/, then inside the MSYS2 UCRT64 terminal:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc   # one-time setup

cd /d/coding/OS_CP/simplefs
mkdir -p build

g++ -std=c++20 -Wall -Wextra -Werror -Iinclude \
    src/mkfs_main.cpp src/disk.cpp -o build/mkfs.exe

g++ -std=c++20 -Wall -Wextra -Werror -Iinclude \
    src/shell_main.cpp src/shell.cpp src/directory.cpp \
    src/fs_core.cpp src/disk.cpp -o build/simplefs_shell.exe
```

### Linux / macOS

```bash
cd simplefs
mkdir -p build

g++ -std=c++20 -Wall -Wextra -Werror -Iinclude \
    src/mkfs_main.cpp src/disk.cpp -o build/mkfs

g++ -std=c++20 -Wall -Wextra -Werror -Iinclude \
    src/shell_main.cpp src/shell.cpp src/directory.cpp \
    src/fs_core.cpp src/disk.cpp -o build/simplefs_shell
```

### CMake (any platform)

```bash
cd simplefs
mkdir build && cd build
cmake ..
cmake --build .
```

---

## Quick Start

### Windows (MSYS2 UCRT64) — recommended

Open the **MSYS2 UCRT64** terminal (search for it in the Start menu), then:

```bash
# 1. Go to the project
cd /d/coding/OS_CP/simplefs

# 2. Build
g++ -std=c++20 -Wall -Wextra -Werror -Iinclude \
    src/mkfs_main.cpp src/disk.cpp -o build/mkfs.exe

g++ -std=c++20 -Wall -Wextra -Werror -Iinclude \
    src/shell_main.cpp src/shell.cpp src/directory.cpp \
    src/fs_core.cpp src/disk.cpp -o build/simplefs_shell.exe

# 3. Create a disk image
cd build
./mkfs.exe disk.img

# 4. Start the shell
./simplefs_shell.exe disk.img
```

> Always run the binaries from the **same MSYS2 terminal** you compiled them in.

### Linux / macOS

```bash
cd simplefs

# Build
g++ -std=c++20 -Wall -Wextra -Werror -Iinclude \
    src/mkfs_main.cpp src/disk.cpp -o build/mkfs

g++ -std=c++20 -Wall -Wextra -Werror -Iinclude \
    src/shell_main.cpp src/shell.cpp src/directory.cpp \
    src/fs_core.cpp src/disk.cpp -o build/simplefs_shell

# Create image and start shell
./build/mkfs disk.img
./build/simplefs_shell disk.img
```

After running `mkfs` you'll see:
```
mkfs: created 'disk.img' (16384 blocks, 1024 inodes, 64 MiB)
```

Custom image size:
```bash
./mkfs.exe disk.img 8192 512    # 8192 blocks (~32 MiB), 512 inodes
```

> Running `mkfs` on an existing image **wipes it completely** and starts fresh.

You get an interactive prompt that shows your current path:
```
/ >
```

### Step 3 — Use it

```
/ > mkdir docs
/ > cd docs
/docs > touch notes.txt
/docs > write notes.txt hello from CoreFS
/docs > cat notes.txt
hello from CoreFS
/docs > ls -l
f  18    notes.txt
/docs > cd ..
/ > df
Filesystem blocks : 2 used / 16347 free / 16349 total
Filesystem inodes : 2 used / 1022 free / 1024 total
Block size        : 4096 bytes
Image size        : 64 MiB
/ > exit
```

Reopen the shell on the same image — everything you created is still there.

---

## All Commands

### Navigation

| Command | Description |
|---|---|
| `pwd` | Print current working directory |
| `ls` | List files and directories in current dir |
| `ls -l` | Long listing — shows type (`d`/`f`), size in bytes, name |
| `cd <path>` | Change directory. Supports relative (`docs`), absolute (`/docs/notes`), `.`, `..`, and multi-level paths (`cd ../other/dir`) |

### File & Directory Management

| Command | Description |
|---|---|
| `mkdir <name>` | Create a new directory |
| `touch <name>` | Create a new empty file |
| `rm <name>` | Delete a file (frees disk blocks when no links remain) |
| `rmdir <name>` | Delete an empty directory |
| `cp <src> <dst>` | Copy a file. If `dst` is an existing directory, copies into it |
| `mv <src> <dst>` | Move or rename a file/directory. If `dst` is an existing directory, moves into it |

### File Contents

| Command | Description |
|---|---|
| `cat <name>` | Print file contents to screen |
| `write <name> <text>` | Overwrite file with `text` (truncates first) |
| `echo <text> > <name>` | Overwrite file with `text` (creates file if it doesn't exist) |
| `echo <text> >> <name>` | Append `text` to file (creates file if it doesn't exist) |

### Information

| Command | Description |
|---|---|
| `stat <name>` | Show inode metadata: inode number, type, size, block count, link count |
| `df` | Show free/used block and inode counts for the whole filesystem |

### Shell Control

| Command | Description |
|---|---|
| `exit` / `quit` | Exit the shell (all changes are already on disk — no save needed) |

---

## Example Session

```
$ ./build/mkfs disk.img
mkfs: created 'disk.img' (16384 blocks, 1024 inodes, 64 MiB)

$ ./build/simplefs_shell disk.img
/ > pwd
/
/ > mkdir projects
/ > cd projects
/projects > mkdir os
/projects > touch ideas.txt
/projects > write ideas.txt build a real filesystem
/projects > cat ideas.txt
build a real filesystem
/projects > echo second idea >> ideas.txt
/projects > cat ideas.txt
build a real filesystem
second idea
/projects > ls -l
d  0    os
f  38   ideas.txt
/projects > cp ideas.txt backup.txt
/projects > ls
os
ideas.txt
backup.txt
/projects > mv backup.txt os
/projects > cd os
/projects/os > ls
backup.txt
/projects/os > stat backup.txt
inode: 4  type: file  size: 38  blocks: 1  links: 1
/projects/os > rm backup.txt
/projects/os > ls
/projects/os > cd ../..
/ > df
Filesystem blocks : 3 used / 16346 free / 16349 total
Filesystem inodes : 3 used / 1021 free / 1024 total
Block size        : 4096 bytes
Image size        : 64 MiB
/ > exit

$ ./build/simplefs_shell disk.img   # reopen — state persists
/ > ls
projects
/ >
```

---

## Notes & Limitations (Phase 5)

- **Max filename length:** 27 characters
- **Max file size:** 48 KB (12 direct blocks × 4 KB). Indirect block support comes in Phase 6.
- **Max entries per directory:** 1536 (12 blocks × 128 dirents/block)
- **`.` and `..`** are not stored on disk — `cd ..` works via an in-memory path stack
- **Single-threaded** — no locking needed until Phase 10
- **No crash consistency** — a crash mid-write can leave the image inconsistent. Journaling arrives in Phase 11.
- **No permissions** — mode bits are stored but not enforced this phase

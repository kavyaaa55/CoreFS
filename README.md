# CoreFS

A file system built from scratch in C++20 — its own on-disk format, allocator, directory structure, and shell. No delegation to the OS file system beyond raw byte access to a single disk-image file.

---

## Status

| Layer | Status |
|---|---|
| On-disk format (superblock, bitmaps, inode table) | ✅ Phase 5 |
| Block / inode allocator | ✅ Phase 5 |
| Directory structure (dirent lookup, add, remove) | ✅ Phase 5 |
| `mkfs` tool | ✅ Phase 5 |
| Shell: `pwd`, `ls`, `cd`, `mkdir`, `touch`, `stat` | ✅ Phase 5 |
| Shell: `cat`, `write`, `echo`, `rm`, `rmdir`, `cp`, `mv`, `df` | ✅ Phase 5 |
| Indirect blocks — single + double (~4 GB max file size) | ✅ Phase 6 |
| Timestamps (atime/mtime/ctime), mode bits, `.`/`..` on disk | ✅ Phase 7 |
| Hard links (`ln`), symlinks (`ln -s`), `readlink`, `chmod` | ✅ Phase 7 |
| Unit tests + model-based fuzzer + `simplefs_fsck` | ✅ Phase 8 |
| Buffer cache (write-back LRU) | ⏳ Phase 9 |
| Concurrency (per-inode locking, benchmarked) | ⏳ Phase 10 |
| Journaling + crash-sweep tests | ⏳ Phase 11 |
| FUSE mount | ⏳ Phase 12 |
| Benchmarks + final docs | ⏳ Phase 13 |

---

## Architecture

```
┌─────────────────────────┐
│   Shell (REPL)          │  ← all user commands
├─────────────────────────┤
│   Directory             │  ← dirent lookup/add/remove/replace, path resolution
├─────────────────────────┤
│   FSCore                │  ← bitmap allocator, inode I/O, bmap, read/write/truncate
│                         │    mkdir/rmdir/unlink/rename/hardlink/symlink/resolve
├─────────────────────────┤
│   Disk                  │  ← raw block read/write on the image file
├─────────────────────────┤
│   disk.img (flat file)  │  ← the "hardware"
└─────────────────────────┘
```

**On-disk layout (FS_VERSION = 3):**

```
Block 0              Superblock
Block 1              Inode bitmap
Block 2              Data bitmap
Blocks 3..34         Inode table  (32 blocks for 1024 inodes × 128 bytes)
Blocks 35..end       Data blocks
```

Each inode has 12 direct + 1 single-indirect + 1 double-indirect block pointer → max file size ~4 GB.  
Directories store arrays of `(inode_number, name)` dirents. `.` and `..` are real on-disk dirents (Phase 7+).  
Symlink targets are stored as file data in a `INODE_TYPE_SYMLINK` inode.

---

## Prerequisites

| Tool | Minimum version | Notes |
|---|---|---|
| C++ compiler | C++20 | g++ 10+, clang++ 14+, MSVC 19.29+. An old MinGW (e.g. g++ 6.x in `C:\MinGW`) is **too old** |
| CMake | 3.16 | For the full build (shell, mkfs, fsck, tests, fuzzer). CMake 4.x works with the flag below |
| Ninja | any | Optional; the build generator used below (`make` works too) |
| Git | any | To clone; CMake also uses it to fetch the doctest test framework |
| MSYS2 (Windows) | any | Provides g++/CMake/Ninja via the `ucrt64` environment — https://www.msys2.org |

---

## Build

All builds put the binaries in `build/`: `mkfs`, `simplefs_shell`, `simplefs_fsck`,
`sfs_tests`, `sfs_fuzz` (with `.exe` on Windows).

### 1. Get the code

```bash
git clone https://github.com/kavyaaa55/CoreFS.git
cd CoreFS
```

### 2a. Windows — PowerShell + MSYS2 (recommended)

One-time toolchain install (MSYS2 must already be installed in `C:\msys64`):

```powershell
C:\msys64\usr\bin\bash.exe -lc "pacman -S --noconfirm --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja"
```

Build (run from the `CoreFS` folder):

```powershell
# Put the MSYS2 tools first on PATH for this window (do this in every new window)
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

cmake -S . -B build -G Ninja "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
cmake --build build
```

> **Why the extra flag:** CMake 4.x rejects the old `cmake_minimum_required` in the
> fetched doctest. `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` lets it configure (harmless on CMake 3.x).
> In PowerShell it **must be in quotes**, or PowerShell splits it at the dot.
>
> **Rebuild after editing code:** just `cmake --build build`. If linking fails with
> *Permission denied*, a `simplefs_shell.exe` is still running — `exit` it first.

### 2b. Windows — MSYS2 UCRT64 terminal

Open **MSYS2 UCRT64** from the Start menu, then:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja

cd "/c/path/to/CoreFS"          # e.g. /c/Users/<you>/Desktop/CoreFS
cmake -S . -B build -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build
```

### 2c. Linux / macOS

```bash
cmake -S . -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build
```

### Without CMake (shell, mkfs and fsck only)

```bash
mkdir -p build
g++ -std=c++20 -Wall -Wextra -Werror -Iinclude \
    src/mkfs_main.cpp src/disk.cpp -o build/mkfs.exe
g++ -std=c++20 -Wall -Wextra -Werror -Iinclude \
    src/shell_main.cpp src/shell.cpp src/disk.cpp src/fs_core.cpp src/directory.cpp \
    -o build/simplefs_shell.exe
g++ -std=c++20 -Wall -Wextra -Werror -Iinclude \
    tools/fsck.cpp src/disk.cpp -o build/simplefs_fsck.exe
```

On Windows, `.\build.ps1` does the first two of these (expects g++ at `C:\msys64\ucrt64\bin`).
The tests and fuzzer need the CMake build (it fetches doctest).

---

## Quick Start

```powershell
# PowerShell (bash: same commands with ./ instead of .\)
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"   # if not already set in this window
cd build

# 1. Create a fresh 64 MiB disk image (overwrites an existing disk.img)
.\mkfs.exe disk.img
#    optional size: .\mkfs.exe disk.img <total_blocks> <total_inodes>   (defaults 16384, 1024)

# 2. Open the shell on it — type `help` for commands, `exit` to quit
.\simplefs_shell.exe disk.img

# 3. After exiting, check the image is consistent (no output + exit code 0 = clean)
.\simplefs_fsck.exe disk.img

# 4. Run the tests
.\sfs_tests.exe            # unit + shell tests
.\sfs_fuzz.exe 100 400     # model-based fuzzer
ctest                      # both
```

You'll get an interactive prompt:
```
/ > mkdir -p docs/notes
/ > echo "hello  CoreFS" > docs/notes/a.txt
/ > cd docs/notes
/docs/notes > cat a.txt
hello  CoreFS
/docs/notes > exit
```

Everything is written to `disk.img` as you go, so reopening it with the shell shows the same files.
You can also pipe commands in: `"mkdir x", "ls -l" | .\simplefs_shell.exe disk.img`.

> If Windows refuses to run a freshly built `.exe` (Device Guard / Smart App Control), run it
> from the same terminal session you built it in.

---

## All Commands

Every `<path>` may be absolute (`/a/b`), relative (`a/b`, `../x`) or a bare name.
Text with spaces can be quoted: `"..."` (with `\"` and `\\` escapes) or `'...'`; `\x` escapes one
character. A quoted `">"` is text, not a redirect. `help` prints a summary.

### Navigation

| Command | Description |
|---|---|
| `pwd` | Print current directory path |
| `ls [path...]` | List a directory (default: current); a file path lists just that file |
| `ls -l [path...]` | Long listing — type (`-` file, `d` dir, `l` symlink), mode `rwxrwxrwx`, links, size, name (symlinks show `-> target`) |
| `cd [path]` | Change directory (no path = `/`). Supports `.`, `..`, absolute, relative, multi-level |

### File & Directory Management

| Command | Description |
|---|---|
| `mkdir [-p] <path...>` | Create directories; `-p` also creates missing parents and accepts existing ones |
| `touch <path...>` | Create empty files, or update the timestamps of existing ones |
| `rm <path...>` | Delete files or symlinks (frees blocks when link count reaches 0) |
| `rmdir <path...>` | Delete empty directories (not the current one) |
| `cp <src> <dst>` | Copy a file. If `dst` is an existing directory, copies into it. Copying a file onto itself is refused |
| `mv <src> <dst>` | Move or rename. If `dst` is an existing directory, moves into it. The prompt follows a moved current directory |

### File Contents

| Command | Description |
|---|---|
| `cat <path...>` | Print file contents |
| `write <path> <text...>` | Overwrite an existing file with text |
| `echo <text> > <path>` | Overwrite file (creates if missing; writes through symlinks) |
| `echo <text> >> <path>` | Append to file (creates if missing) |

### Links & Permissions

| Command | Description |
|---|---|
| `ln <src> <dst>` | Hard link — both names point to the same inode. `dst` may be a directory |
| `ln -s <target> <link>` | Symlink — `link` stores `target` as given; a relative target resolves from the link's directory |
| `readlink <path>` | Print the target of a symlink |
| `chmod <octal> <path>` | Set permission bits (follows symlinks) |

**Permissions are enforced** using the owner bits (there is a single user):

| Bit | On a file | On a directory |
|---|---|---|
| `r` (4) | `cat`, `cp` from it | `ls` it |
| `w` (2) | `write`, `echo >`/`>>`, `cp` onto it | create, delete, rename entries in it (`touch`, `mkdir`, `rm`, `rmdir`, `mv`, `cp`, `ln`) |
| `x` (1) | — | enter it (`cd`) or reach anything through it |

`chmod` and `stat` are always allowed. Symlink modes are not checked.

### Information

| Command | Description |
|---|---|
| `stat <path>` | Show inode: number, type, size, blocks, links, mode (octal), atime, mtime, ctime |
| `df` | Show free/used block and inode counts |

### Shell Control

| Command | Description |
|---|---|
| `help` | List commands |
| `exit` / `quit` | Exit the shell (all changes are already on disk) |

---

## Testing (Phase 8)

```bash
cd build

# Unit + integration tests (via doctest)
./sfs_tests

# Model-based fuzzer: 100 seeds × 400 ops each; fsck runs on each image after its seed
./sfs_fuzz 100 400
SFS_FUZZ_TRACE=1 ./sfs_fuzz 1 400   # print every op with model/fs outcome

# fsck on a disk image
./simplefs_fsck disk.img
./simplefs_fsck disk.img --repair   # fix bitmap leaks + link counts
```

**fsck checks:**

| Error key | What it detects |
|---|---|
| `SUPERBLOCK` | Bad magic or wrong version |
| `BLOCK_OUT_OF_RANGE` | Inode points to a block outside the data region |
| `DOUBLE_REF` | Two inodes reference the same data block |
| `REF_BUT_FREE` | Block is referenced but not marked allocated in bitmap |
| `BLOCK_LEAK` | Block is marked allocated but not referenced by any inode |
| `DIRENT_TO_FREE_INODE` | Directory entry points to an unallocated inode |
| `BAD_DOT` | A directory's `.` entry doesn't point to itself |
| `BAD_DOTDOT` | A directory's `..` entry doesn't match the actual parent |
| `DIR_MULTI_PARENT` | A directory is reachable from more than one parent |
| `ORPHAN_INODE` | Allocated inode not reachable from root |
| `LINK_COUNT` | Stored link count doesn't match actual dirent reference count |

---

## Example Session

```
$ ./mkfs.exe disk.img
mkfs: created 'disk.img' (16384 blocks, 1024 inodes, 64 MiB, version 3)

$ ./simplefs_shell.exe disk.img
/ > mkdir projects
/ > cd projects
/projects > touch readme.txt
/projects > write readme.txt this is CoreFS
/projects > cat readme.txt
this is CoreFS
/projects > echo second line >> readme.txt
/projects > cat readme.txt
this is CoreFS
second line
/projects > ls -l
-rw-r--r--  1  29    readme.txt
/projects > ln readme.txt backup.txt
/projects > stat readme.txt
inode: 3  type: file  size: 29  blocks: 1  links: 2  mode: 644  ...
/projects > rm readme.txt
/projects > cat backup.txt
this is CoreFS
second line
/projects > ln -s backup.txt link_to_backup
/projects > readlink link_to_backup
backup.txt
/projects > cat link_to_backup
this is CoreFS
second line
/projects > chmod 600 backup.txt
/projects > cd ..
/ > df
Filesystem blocks : 4 used / 16345 free / 16349 total
Filesystem inodes : 5 used / 1019 free / 1024 total
Block size        : 4096 bytes
Image size        : 64 MiB
/ > exit
```

---

## Notes & Limitations

- **Max filename:** 27 characters
- **Max file size:** ~4 GB (12 direct + single-indirect + double-indirect blocks at 4 KB each)
- **Max entries per directory:** ~134 million — directories use the same direct + single- + double-indirect block map as files (128 dirents per 4 KB block). A directory's size is its allocated bytes (4096 per block)
- **Symlink target length:** max 1023 characters
- **Single-threaded** — no locking until Phase 10
- **No crash consistency** — a crash mid-write can corrupt the image. Journaling arrives in Phase 11
- **Permissions: single user** — owner bits are enforced; group/other bits are stored but unused (no users or groups)
- **`..` of `/` is `/`** — following `..` from root stays at root

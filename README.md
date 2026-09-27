# SimpleFS — Phase 5

A from-scratch file system implemented in C++20, backed by a single flat disk-image file.

## Build

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

On Windows with MSVC the binaries land in `build/Debug/` or `build/Release/`.
On Linux/macOS they land directly in `build/`.

## Usage

### 1. Create a disk image

```bash
./mkfs disk.img
# defaults: 16384 blocks (64 MiB), 1024 inodes

# Custom size:
./mkfs disk.img 8192 512   # 8192 blocks, 512 inodes
```

Running `mkfs` twice on the same path produces a fresh, correctly-zeroed image.

### 2. Open the shell

```bash
./simplefs_shell disk.img
```

### Available commands

| Command       | Description                              |
|---------------|------------------------------------------|
| `pwd`         | Print current working directory          |
| `ls [-l]`     | List directory contents                  |
| `cd <path>`   | Change directory (supports `.`, `..`, absolute and relative paths) |
| `mkdir <name>`| Create a new directory                   |
| `touch <name>`| Create a new empty file                  |
| `stat <name>` | Print inode metadata for a file/dir      |
| `exit` / `quit` | Exit the shell                         |

Phase 6 stubs (print "not implemented yet"):
`cat`, `write`, `rm`, `rmdir`, `cp`, `mv`

### Example session

```
$ ./mkfs disk.img
$ ./simplefs_shell disk.img
/ > pwd
/
/ > mkdir docs
/ > ls
docs
/ > ls -l
d  0    docs
/ > cd docs
/docs > touch notes
/docs > stat notes
inode: 1  type: file  size: 0  blocks: 0  links: 1
/docs > cd ..
/ > cat notes
cat: not implemented yet
/ > exit
```

Re-opening the shell against the same image preserves all created files and directories (persistence is guaranteed by flushing the superblock and bitmaps after every allocation).

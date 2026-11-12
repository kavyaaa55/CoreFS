#pragma once
#include <string>
#include <string_view>
#include <optional>
#include <cstdint>
#include "disk.h"
#include "types.h"

class FSCore {
public:
    explicit FSCore(const std::string& image_path);

    // ── Bitmap allocation ──────────────────────────────────────────────────
    uint32_t allocBlock();
    void     freeBlock(uint32_t block_num);
    uint32_t allocInode();
    void     freeInode(uint32_t inode_num);

    // ── Inode I/O ──────────────────────────────────────────────────────────
    void readInode(uint32_t inode_num, Inode& out);
    void writeInode(uint32_t inode_num, const Inode& in);

    // ── Raw block access ───────────────────────────────────────────────────
    void readBlock(uint32_t block_num, void* buf);
    void writeBlock(uint32_t block_num, const void* buf);

    // ── Superblock ─────────────────────────────────────────────────────────
    Superblock& superblock();
    void        flushSuperblock();

    // ── Byte-offset data path ──────────────────────────────────────────────
    uint32_t readFile(uint32_t ino, uint32_t off, void* buf, uint32_t len);
    uint32_t writeFile(uint32_t ino, uint32_t off, const void* buf, uint32_t len);
    void     truncateFile(uint32_t ino, uint32_t new_size);
    void     freeAllBlocks(uint32_t ino);
    void     unlinkIfUnreferenced(uint32_t ino);

    // ── Phase 7: high-level FS operations ─────────────────────────────────

    // Create a regular file in directory `dir`, link count = 1.
    // Returns new inode number. Throws on error.
    uint32_t createFile(uint32_t dir, std::string_view name, uint16_t mode);

    // Create a directory in `parent`. Writes "." and "..", updates parent link count.
    uint32_t makeDir(uint32_t parent, std::string_view name, uint16_t mode);

    // Remove a non-directory entry from `dir`. Decrements link count, frees if 0.
    void unlinkFile(uint32_t dir, std::string_view name);

    // Remove an empty directory from `parent`.
    void removeDir(uint32_t parent, std::string_view name);

    // Rename/move src entry to dst (cross-dir safe, handles link counts for dirs).
    void renameEntry(uint32_t src_dir, std::string_view src_name,
                     uint32_t dst_dir, std::string_view dst_name);

    // Hard link: add name in dir pointing to same inode as src_ino.
    void hardLink(uint32_t src_ino, uint32_t dir, std::string_view name);

    // Symlink: create inode of type SYMLINK, store target as file data.
    uint32_t makeSymlink(std::string_view target, uint32_t dir, std::string_view name);

    // Read symlink target into string.
    std::string readSymlink(uint32_t ino);

    // Path resolution: handles absolute/relative, ".", "..", symlinks (depth limit 8).
    // follow_last=false => do NOT follow a symlink at the final component (for lstat/readlink).
    // Returns the resolved inode, or throws on error.
    uint32_t resolve(std::string_view path, uint32_t cwd, bool follow_last = true,
                     int depth = 0);

    // Walk ".." from ino up to root; build "/a/b/c" path string.
    std::string inodePath(uint32_t ino);

    // Returns true if `ancestor` is an ancestor of (or equal to) `ino`.
    bool isAncestor(uint32_t ancestor, uint32_t ino);

    // ── Phase 7: timestamp helpers ─────────────────────────────────────────
    static uint32_t now();                      // current unix time
    static void touchMC(Inode& i);              // set mtime + ctime = now()
    static void touchC(Inode& i);               // set ctime = now()
    // Update atime relatime-style (only if atime <= mtime)
    void updateAtime(uint32_t ino);

private:
    Disk       disk_;
    Superblock sb_;

    uint32_t allocBit(uint32_t bitmap_block, uint32_t max_bits);
    void     freeBit(uint32_t bitmap_block, uint32_t bit_index);

    uint32_t bmap(Inode& inode, uint32_t lblock, bool allocate);
    uint32_t readIndirectPtr(uint32_t blk, uint32_t slot);
    void     writeIndirectPtr(uint32_t blk, uint32_t slot, uint32_t val);

    // Drop one link; free inode+blocks if links reach 0, else update ctime.
    void dropLink(uint32_t ino);
};

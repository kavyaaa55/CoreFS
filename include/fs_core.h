#pragma once
#include <string>
#include <cstdint>
#include "disk.h"
#include "types.h"

// Wraps Disk with filesystem semantics: superblock, bitmaps, inode I/O,
// and the byte-offset read/write/truncate data path.
class FSCore {
public:
    explicit FSCore(const std::string& image_path); // loads and validates superblock (checks magic)

    // ── Bitmap allocation ──────────────────────────────────────────────────
    // Finds first free bit in data bitmap, marks used, decrements free_blocks,
    // returns ABSOLUTE block number (already offset by data_block_start).
    // Throws std::runtime_error if no blocks are free.
    uint32_t allocBlock();
    void     freeBlock(uint32_t block_num); // clears bit, increments free_blocks

    // Finds first free bit in inode bitmap, marks used, decrements free_inodes,
    // returns inode number. Throws std::runtime_error if no inodes are free.
    uint32_t allocInode();
    void     freeInode(uint32_t inode_num); // clears bit, increments free_inodes

    // ── Inode I/O ──────────────────────────────────────────────────────────
    void readInode(uint32_t inode_num, Inode& out);
    void writeInode(uint32_t inode_num, const Inode& in);

    // ── Raw block access (forwarded to Disk) ──────────────────────────────
    void readBlock(uint32_t block_num, void* buf);
    void writeBlock(uint32_t block_num, const void* buf);

    // ── Superblock ─────────────────────────────────────────────────────────
    Superblock& superblock();      // in-memory copy; call flushSuperblock() after mutating
    void        flushSuperblock(); // writes in-memory superblock back to block 0

    // ── Byte-offset data path ──────────────────────────────────────────────

    // Read up to `len` bytes from file `ino` at byte offset `off` into `buf`.
    // Clamps to file size; returns 0 at EOF. Throws on I/O error.
    uint32_t readFile(uint32_t ino, uint32_t off, void* buf, uint32_t len);

    // Write `len` bytes from `buf` into file `ino` at byte offset `off`.
    // Allocates data blocks as needed, extends size, persists inode.
    // Partial-block writes do read-modify-write.
    // Throws if disk is full during allocation.
    // Returns number of bytes written.
    uint32_t writeFile(uint32_t ino, uint32_t off, const void* buf, uint32_t len);

    // Resize file `ino` to `new_size` bytes.
    // Shrink: frees blocks past the new size.
    // Grow: extends size (sparse — unallocated blocks read as zeros).
    void truncateFile(uint32_t ino, uint32_t new_size);

    // If inode `ino` has link_count == 0, frees all data blocks and the inode.
    void unlinkIfUnreferenced(uint32_t ino);

    // Free all data blocks for `ino` and reset its block pointers / counts.
    void freeAllBlocks(uint32_t ino);

private:
    Disk       disk_;
    Superblock sb_;

    // Internal bitmap helpers
    uint32_t allocBit(uint32_t bitmap_block, uint32_t max_bits);
    void     freeBit(uint32_t bitmap_block, uint32_t bit_index);

    // Returns the absolute block number for logical block `lblock` in inode.
    // Walks direct -> single_indirect -> double_indirect.
    // allocate=true: allocates missing indirect and data blocks on demand.
    // allocate=false: returns 0 for holes (reads return zeros).
    // Throws "file too big" if lblock >= DOUBLE_MAX.
    // Throws "disk full" if allocation fails; on throw, already-allocated
    // blocks in this call are freed before propagating (no leaks).
    uint32_t bmap(Inode& inode, uint32_t lblock, bool allocate);

    // Helpers used by bmap/truncate for a single level of indirection.
    // Returns the pointer stored at slot `slot` in block `blk` (0 if blk==0).
    uint32_t readIndirectPtr(uint32_t blk, uint32_t slot);
    // Writes `val` into slot `slot` of block `blk` (blk must be valid).
    void     writeIndirectPtr(uint32_t blk, uint32_t slot, uint32_t val);
};

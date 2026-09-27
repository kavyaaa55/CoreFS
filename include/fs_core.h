#pragma once
#include <string>
#include <cstdint>
#include "disk.h"
#include "types.h"

// Wraps Disk with filesystem semantics: superblock, bitmaps, inode I/O.
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
    Superblock& superblock();   // in-memory copy; call flushSuperblock() after mutating
    void        flushSuperblock(); // writes in-memory superblock back to block 0

private:
    Disk       disk_;
    Superblock sb_;

    // Internal bitmap helpers
    uint32_t allocBit(uint32_t bitmap_block, uint32_t max_bits);
    void     freeBit(uint32_t bitmap_block, uint32_t bit_index);
};

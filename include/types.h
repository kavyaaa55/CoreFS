#pragma once
#include <cstdint>
#include "constants.h"

// ── Superblock ─────────────────────────────────────────────────────────────
// Occupies exactly one block (block 0).
// 13 uint32_t fields = 52 bytes; the rest is padding to BLOCK_SIZE.
struct Superblock {
    uint32_t magic;
    uint32_t version;             // FS_VERSION — mount rejects unknown versions
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t total_inodes;
    uint32_t inode_bitmap_block;  // = 1
    uint32_t data_bitmap_block;   // = 2
    uint32_t inode_table_block;   // = 3
    uint32_t inode_table_blocks;  // ceil(total_inodes * INODE_SIZE / BLOCK_SIZE)
    uint32_t data_block_start;    // = 3 + inode_table_blocks
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t root_inode;          // = 0
    uint8_t  reserved[BLOCK_SIZE - 52]; // pad to exactly one block (13 * 4 = 52)
};
static_assert(sizeof(Superblock) == BLOCK_SIZE, "Superblock must be exactly BLOCK_SIZE bytes");

// ── Inode ───────────────────────────────────────────────────────────────────
// Fixed 128-byte record.
// Header:            4 * 4  = 16 bytes
// direct[12]:       12 * 4  = 48 bytes
// single_indirect:   1 * 4  =  4 bytes
// double_indirect:   1 * 4  =  4 bytes
// mode:              1 * 4  =  4 bytes
// Total used:                 76 bytes  -> reserved: 128 - 76 = 52 bytes
struct Inode {
    uint32_t type;                           // INODE_TYPE_*
    uint32_t size;                           // bytes used
    uint32_t link_count;
    uint32_t block_count;                    // number of DATA blocks allocated (not indirect ptr blocks)
    uint32_t direct[NUM_DIRECT_BLOCKS];      // block numbers, 0 = unused / hole
    uint32_t indirect;                       // block number of single-indirect block, 0 = none
    uint32_t double_indirect;                // block number of double-indirect block, 0 = none
    uint32_t mode;                           // reserved, unused until Phase 7
    uint8_t  reserved[INODE_SIZE - (4 * 4 + NUM_DIRECT_BLOCKS * 4 + 4 + 4 + 4)];
};
static_assert(sizeof(Inode) == INODE_SIZE, "Inode must be exactly INODE_SIZE bytes");

// ── Dirent ──────────────────────────────────────────────────────────────────
// Fixed 32-byte record.
// inode_num (4) + name (28) = 32 bytes
// inode_num == 0 means "unused slot".
struct Dirent {
    uint32_t inode_num;                   // 0 = unused slot
    char     name[MAX_FILENAME_LEN + 1];  // null-terminated, 28 bytes
};
static_assert(sizeof(Dirent) == DIRENT_SIZE, "Dirent must be exactly DIRENT_SIZE bytes");

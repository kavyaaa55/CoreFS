#pragma once
#include <cstdint>
#include "constants.h"

// ── Superblock ─────────────────────────────────────────────────────────────
// Occupies exactly one block (block 0).
// 13 uint32_t fields = 52 bytes; rest is padding.
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
    uint8_t  reserved[BLOCK_SIZE - 52]; // 13 * 4 = 52 bytes used
};
static_assert(sizeof(Superblock) == BLOCK_SIZE, "Superblock must be exactly BLOCK_SIZE bytes");

// ── Inode ───────────────────────────────────────────────────────────────────
// Fixed 128-byte record.
// Layout (Phase 7):
//   type        : 4
//   size        : 4
//   link_count  : 4
//   block_count : 4   => 16
//   direct[12]  : 48  => 64
//   indirect    : 4   => 68
//   double_ind  : 4   => 72
//   mode        : 2   => 74
//   _pad        : 2   => 76
//   atime       : 4   => 80
//   mtime       : 4   => 84
//   ctime       : 4   => 88
//   reserved    : 40  => 128
struct Inode {
    uint32_t type;                      // INODE_TYPE_*
    uint32_t size;                      // bytes of file data
    uint32_t link_count;
    uint32_t block_count;               // data blocks allocated (not indirect ptr blocks)
    uint32_t direct[NUM_DIRECT_BLOCKS]; // 0 = hole/unused
    uint32_t indirect;                  // single-indirect block pointer, 0 = none
    uint32_t double_indirect;           // double-indirect block pointer, 0 = none
    uint16_t mode;                      // rwx bits (0777 mask); Phase 7
    uint8_t  _pad[2];                   // alignment padding
    uint32_t atime;                     // last access time (unix seconds); Phase 7
    uint32_t mtime;                     // last modification time; Phase 7
    uint32_t ctime;                     // last metadata change time; Phase 7
    uint8_t  reserved[INODE_SIZE - 88]; // pad to 128 bytes
};
static_assert(sizeof(Inode) == INODE_SIZE, "Inode must be exactly INODE_SIZE bytes");

// ── Dirent ──────────────────────────────────────────────────────────────────
// Fixed 32-byte record. An empty name (name[0] == 0) marks an unused slot;
// inode_num 0 is a real inode (root), so it cannot be the marker.
struct Dirent {
    uint32_t inode_num;
    char     name[MAX_FILENAME_LEN + 1]; // null-terminated, 28 bytes
};
static_assert(sizeof(Dirent) == DIRENT_SIZE, "Dirent must be exactly DIRENT_SIZE bytes");

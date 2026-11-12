#pragma once
#include <cstdint>

constexpr uint32_t BLOCK_SIZE        = 4096;
constexpr uint32_t MAGIC_NUMBER      = 0x53465331; // "SFS1"
constexpr uint32_t FS_VERSION        = 3;          // Phase 7: timestamps + mode + ./.., symlinks
constexpr uint32_t NUM_DIRECT_BLOCKS = 12;
constexpr uint32_t MAX_FILENAME_LEN  = 27;          // + 1 null byte = 28
constexpr uint32_t INODE_SIZE        = 128;         // bytes, must match sizeof(Inode)
constexpr uint32_t DIRENT_SIZE       = 32;          // bytes, must match sizeof(Dirent)

constexpr uint32_t DEFAULT_TOTAL_BLOCKS = 16384;    // 64 MB image at 4KB blocks
constexpr uint32_t DEFAULT_TOTAL_INODES = 1024;

constexpr uint32_t INODE_TYPE_FREE    = 0;
constexpr uint32_t INODE_TYPE_FILE    = 1;
constexpr uint32_t INODE_TYPE_DIR     = 2;
constexpr uint32_t INODE_TYPE_SYMLINK = 3; // Phase 7: symlink target stored as file data

constexpr uint32_t ROOT_INODE_NUM = 0;

// ── Indirect block layout (Phase 6) ───────────────────────────────────────
constexpr uint32_t PTRS_PER_BLOCK = BLOCK_SIZE / sizeof(uint32_t); // 1024

constexpr uint32_t DIRECT_MAX = NUM_DIRECT_BLOCKS;                           // 12
constexpr uint32_t SINGLE_MAX = DIRECT_MAX + PTRS_PER_BLOCK;                 // 1036
constexpr uint32_t DOUBLE_MAX = SINGLE_MAX + PTRS_PER_BLOCK * PTRS_PER_BLOCK;// 1,049,612

constexpr uint64_t MAX_FILE_SIZE =
    static_cast<uint64_t>(DOUBLE_MAX) * BLOCK_SIZE; // ~4.0 GB

// Symlink target is stored as file data; limit the target length to avoid
// needing more than one block.
constexpr uint32_t MAX_SYMLINK_LEN = 1023; // NUL-terminated in one block
constexpr int      SYMLINK_DEPTH_LIMIT = 8; // loop protection

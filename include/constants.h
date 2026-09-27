#pragma once
#include <cstdint>

constexpr uint32_t BLOCK_SIZE        = 4096;
constexpr uint32_t MAGIC_NUMBER      = 0x53465331; // "SFS1"
constexpr uint32_t NUM_DIRECT_BLOCKS = 12;
constexpr uint32_t MAX_FILENAME_LEN  = 27;          // + 1 null byte = 28
constexpr uint32_t INODE_SIZE        = 128;         // bytes, must match sizeof(Inode)
constexpr uint32_t DIRENT_SIZE       = 32;          // bytes, must match sizeof(Dirent)

constexpr uint32_t DEFAULT_TOTAL_BLOCKS = 16384;    // 64 MB image at 4KB blocks
constexpr uint32_t DEFAULT_TOTAL_INODES = 1024;

constexpr uint32_t INODE_TYPE_FREE = 0;
constexpr uint32_t INODE_TYPE_FILE = 1;
constexpr uint32_t INODE_TYPE_DIR  = 2;

constexpr uint32_t ROOT_INODE_NUM = 0;

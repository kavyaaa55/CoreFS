#pragma once
// Helpers shared across all test files.
// Creates a small temporary disk image, runs mkfs on it, returns FSCore.

#include "disk.h"
#include "fs_core.h"
#include "types.h"
#include "constants.h"

#include <cstring>
#include <ctime>
#include <string>
#include <filesystem>
#include <stdexcept>

// put_dirent: write a dirent into an in-memory 4KB buffer at slot idx.
static inline void put_dirent_buf(uint8_t* blk, uint32_t idx,
                                  uint32_t ino, const char* name) {
    uint8_t* p = blk + idx * DIRENT_SIZE;
    std::memset(p, 0, DIRENT_SIZE);
    std::memcpy(p, &ino, sizeof(uint32_t));
    std::strncpy(reinterpret_cast<char*>(p + sizeof(uint32_t)),
                 name, MAX_FILENAME_LEN);
}

// Make a small but valid SimpleFS image at `path`.
// total_blocks defaults to 512 (2 MiB), total_inodes to 64.
static inline void make_test_image(const std::string& path,
                                   uint32_t total_blocks = 512,
                                   uint32_t total_inodes = 64) {
    uint64_t img_size = static_cast<uint64_t>(total_blocks) * BLOCK_SIZE;
    Disk::createImage(path, img_size);
    Disk disk(path);
    uint8_t buf[BLOCK_SIZE];
    uint32_t t = static_cast<uint32_t>(std::time(nullptr));

    uint32_t inode_table_blocks = (total_inodes * INODE_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint32_t data_block_start   = 3 + inode_table_blocks;

    // Superblock
    Superblock sb{};
    sb.magic              = MAGIC_NUMBER;
    sb.version            = FS_VERSION;
    sb.block_size         = BLOCK_SIZE;
    sb.total_blocks       = total_blocks;
    sb.total_inodes       = total_inodes;
    sb.inode_bitmap_block = 1;
    sb.data_bitmap_block  = 2;
    sb.inode_table_block  = 3;
    sb.inode_table_blocks = inode_table_blocks;
    sb.data_block_start   = data_block_start;
    sb.root_inode         = ROOT_INODE_NUM;
    uint32_t root_data = data_block_start;
    uint32_t lf_data   = data_block_start + 1;
    sb.free_blocks = (total_blocks - data_block_start) - 2;
    sb.free_inodes = total_inodes - 2;
    disk.writeBlock(0, &sb);

    // Inode bitmap: bits 0 and 1 set
    std::memset(buf, 0, BLOCK_SIZE); buf[0] = 0x03;
    disk.writeBlock(1, buf);

    // Data bitmap: bits 0 and 1 set
    std::memset(buf, 0, BLOCK_SIZE); buf[0] = 0x03;
    disk.writeBlock(2, buf);

    // Zero inode table
    std::memset(buf, 0, BLOCK_SIZE);
    for (uint32_t i = 0; i < inode_table_blocks; ++i)
        disk.writeBlock(3 + i, buf);

    // Root inode
    disk.readBlock(3, buf);
    Inode root{};
    root.type = INODE_TYPE_DIR; root.mode = 0755; root.link_count = 3;
    root.block_count = 1; root.direct[0] = root_data; root.size = BLOCK_SIZE;
    root.atime = root.mtime = root.ctime = t;
    std::memcpy(buf, &root, sizeof(Inode));
    disk.writeBlock(3, buf);

    // lost+found inode (inode 1)
    disk.readBlock(3, buf);
    Inode lf{};
    lf.type = INODE_TYPE_DIR; lf.mode = 0700; lf.link_count = 2;
    lf.block_count = 1; lf.direct[0] = lf_data; lf.size = BLOCK_SIZE;
    lf.atime = lf.mtime = lf.ctime = t;
    std::memcpy(buf + INODE_SIZE, &lf, sizeof(Inode));
    disk.writeBlock(3, buf);

    // Root data block
    std::memset(buf, 0, BLOCK_SIZE);
    put_dirent_buf(buf, 0, 0, "."); put_dirent_buf(buf, 1, 0, "..");
    put_dirent_buf(buf, 2, 1, "lost+found");
    disk.writeBlock(root_data, buf);

    // lost+found data block
    std::memset(buf, 0, BLOCK_SIZE);
    put_dirent_buf(buf, 0, 1, "."); put_dirent_buf(buf, 1, 0, "..");
    disk.writeBlock(lf_data, buf);
}

// Unique temp path that won't collide between parallel test runs.
static inline std::string tmp_img(const char* tag) {
    static int counter = 0;
    return std::string("sfs_test_") + tag + "_" + std::to_string(++counter) + ".img";
}

// RAII wrapper: creates a fresh image, opens FSCore, deletes on destruction.
struct TempFS {
    std::string path;
    FSCore* fs = nullptr;

    explicit TempFS(const char* tag = "t",
                    uint32_t blocks = 512, uint32_t inodes = 64)
        : path(tmp_img(tag)) {
        make_test_image(path, blocks, inodes);
        fs = new FSCore(path);
    }
    ~TempFS() { delete fs; std::remove(path.c_str()); }
    FSCore& operator*()  { return *fs; }
    FSCore* operator->() { return fs; }

    // Reopen (simulates remount — proves persistence)
    void reopen() { delete fs; fs = new FSCore(path); }
};

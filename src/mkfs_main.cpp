#include "disk.h"
#include "types.h"
#include "constants.h"

#include <iostream>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>

// Write a dirent into an in-memory 4KB buffer at slot `idx`.
static void put_dirent(uint8_t* block_buf, uint32_t idx, uint32_t ino, const char* name) {
    static_assert(BLOCK_SIZE / DIRENT_SIZE >= 1);
    uint8_t* p = block_buf + idx * DIRENT_SIZE;
    std::memset(p, 0, DIRENT_SIZE);
    std::memcpy(p, &ino, sizeof(uint32_t));
    std::strncpy(reinterpret_cast<char*>(p + sizeof(uint32_t)), name, MAX_FILENAME_LEN);
}

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 4) {
        std::cerr << "usage: mkfs <image_path> [total_blocks] [total_inodes]\n";
        return 1;
    }

    std::string image_path = argv[1];
    uint32_t total_blocks  = DEFAULT_TOTAL_BLOCKS;
    uint32_t total_inodes  = DEFAULT_TOTAL_INODES;

    try {
        if (argc >= 3) total_blocks = static_cast<uint32_t>(std::stoul(argv[2]));
        if (argc >= 4) total_inodes = static_cast<uint32_t>(std::stoul(argv[3]));
    } catch (const std::exception& e) {
        std::cerr << "mkfs: invalid argument: " << e.what() << "\n"; return 1;
    }

    uint64_t image_size = static_cast<uint64_t>(total_blocks) * BLOCK_SIZE;
    try { Disk::createImage(image_path, image_size); }
    catch (const std::exception& e) { std::cerr << "mkfs: " << e.what() << "\n"; return 1; }

    Disk disk(image_path);
    uint8_t buf[BLOCK_SIZE];
    uint32_t t = static_cast<uint32_t>(std::time(nullptr));

    // ── Layout ───────────────────────────────────────────────────────────
    uint32_t inode_table_blocks = (total_inodes * INODE_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint32_t data_block_start   = 3 + inode_table_blocks;
    if (data_block_start + 3 >= total_blocks) {
        std::cerr << "mkfs: image too small\n"; return 1;
    }

    // We need 3 data blocks: root dir, lost+found dir entry block, lost+found dir data block
    // root data: bit 0, lost+found data: bit 1
    // inode 0 = root, inode 1 = lost+found
    uint32_t root_data_blk  = data_block_start + 0;
    uint32_t lf_data_blk    = data_block_start + 1;

    uint32_t free_blocks = (total_blocks - data_block_start) - 2; // 2 pre-allocated
    uint32_t free_inodes = total_inodes - 2;                       // inode 0 + 1 used

    // ── Superblock ────────────────────────────────────────────────────────
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
    sb.free_blocks        = free_blocks;
    sb.free_inodes        = free_inodes;
    sb.root_inode         = ROOT_INODE_NUM;
    disk.writeBlock(0, &sb);

    // ── Inode bitmap: set bits 0 (root) and 1 (lost+found) ───────────────
    std::memset(buf, 0, BLOCK_SIZE);
    buf[0] = 0x03; // bits 0 and 1
    disk.writeBlock(1, buf);

    // ── Data bitmap: set bits 0 and 1 ────────────────────────────────────
    std::memset(buf, 0, BLOCK_SIZE);
    buf[0] = 0x03;
    disk.writeBlock(2, buf);

    // ── Zero inode table ─────────────────────────────────────────────────
    std::memset(buf, 0, BLOCK_SIZE);
    for (uint32_t i = 0; i < inode_table_blocks; ++i) disk.writeBlock(3 + i, buf);

    // ── Root inode (inode 0): DIR, link_count=2 ("."+parent entry from lf "..")
    // Actually root link_count = 2 + number_of_subdirs.
    // We create lost+found inside root, so root link_count = 3:
    //   the mount point itself (1) + "." (self ref, doesn't add link in POSIX) = 2 base
    //   + lost+found's ".." pointing to root = +1  =>  3
    {
        uint32_t byte_w = ROOT_INODE_NUM * INODE_SIZE;
        disk.readBlock(3 + byte_w / BLOCK_SIZE, buf);
        Inode root{};
        root.type        = INODE_TYPE_DIR;
        root.mode        = 0755;
        root.link_count  = 3;    // 2 base + 1 for lost+found's ".."
        root.block_count = 1;
        root.direct[0]   = root_data_blk;
        root.atime = root.mtime = root.ctime = t;
        std::memcpy(buf + byte_w % BLOCK_SIZE, &root, sizeof(Inode));
        disk.writeBlock(3 + byte_w / BLOCK_SIZE, buf);
    }

    // ── lost+found inode (inode 1): DIR, link_count=2 ────────────────────
    {
        uint32_t byte_w = 1 * INODE_SIZE;
        disk.readBlock(3 + byte_w / BLOCK_SIZE, buf);
        Inode lf{};
        lf.type        = INODE_TYPE_DIR;
        lf.mode        = 0700;
        lf.link_count  = 2;
        lf.block_count = 1;
        lf.direct[0]   = lf_data_blk;
        lf.atime = lf.mtime = lf.ctime = t;
        std::memcpy(buf + byte_w % BLOCK_SIZE, &lf, sizeof(Inode));
        disk.writeBlock(3 + byte_w / BLOCK_SIZE, buf);
    }

    // ── Root data block: "." -> 0, ".." -> 0, "lost+found" -> 1 ─────────
    std::memset(buf, 0, BLOCK_SIZE);
    put_dirent(buf, 0, ROOT_INODE_NUM, ".");
    put_dirent(buf, 1, ROOT_INODE_NUM, "..");
    put_dirent(buf, 2, 1,              "lost+found");
    disk.writeBlock(root_data_blk, buf);

    // ── lost+found data block: "." -> 1, ".." -> 0 ───────────────────────
    std::memset(buf, 0, BLOCK_SIZE);
    put_dirent(buf, 0, 1,              ".");
    put_dirent(buf, 1, ROOT_INODE_NUM, "..");
    disk.writeBlock(lf_data_blk, buf);

    std::cout << "mkfs: created '" << image_path << "'"
              << " (" << total_blocks << " blocks, "
              << total_inodes << " inodes, "
              << image_size / (1024 * 1024) << " MiB, version "
              << FS_VERSION << ")\n";
    return 0;
}

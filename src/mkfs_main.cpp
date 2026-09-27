#include "disk.h"
#include "types.h"
#include "constants.h"

#include <iostream>
#include <cstring>
#include <stdexcept>
#include <string>

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
        std::cerr << "mkfs: invalid argument: " << e.what() << "\n";
        return 1;
    }

    // ── 1. Create / truncate the image file ──────────────────────────────
    uint64_t image_size = static_cast<uint64_t>(total_blocks) * BLOCK_SIZE;
    try {
        Disk::createImage(image_path, image_size);
    } catch (const std::exception& e) {
        std::cerr << "mkfs: " << e.what() << "\n";
        return 1;
    }

    // Open the image for block-level access
    Disk disk(image_path);

    uint8_t buf[BLOCK_SIZE];

    // ── 2. Compute layout ────────────────────────────────────────────────
    // inode_table_blocks = ceil(total_inodes * INODE_SIZE / BLOCK_SIZE)
    uint32_t inode_table_blocks =
        (total_inodes * INODE_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint32_t data_block_start = 3 + inode_table_blocks;

    if (data_block_start >= total_blocks) {
        std::cerr << "mkfs: image too small for the requested inode count\n";
        return 1;
    }

    uint32_t total_data_blocks = total_blocks - data_block_start;
    // root already consumes bit 0 of the data bitmap, so free = total_data - 1
    uint32_t free_blocks = total_data_blocks - 1;
    // root inode (0) already allocated, so free = total_inodes - 1
    uint32_t free_inodes = total_inodes - 1;

    // ── 3. Build and write the Superblock (block 0) ──────────────────────
    Superblock sb{};
    sb.magic               = MAGIC_NUMBER;
    sb.block_size          = BLOCK_SIZE;
    sb.total_blocks        = total_blocks;
    sb.total_inodes        = total_inodes;
    sb.inode_bitmap_block  = 1;
    sb.data_bitmap_block   = 2;
    sb.inode_table_block   = 3;
    sb.inode_table_blocks  = inode_table_blocks;
    sb.data_block_start    = data_block_start;
    sb.free_blocks         = free_blocks;
    sb.free_inodes         = free_inodes;
    sb.root_inode          = ROOT_INODE_NUM;
    disk.writeBlock(0, &sb);

    // ── 4. Inode bitmap (block 1): zero, then set bit 0 (root) ──────────
    std::memset(buf, 0, BLOCK_SIZE);
    buf[0] = 0x01; // bit 0 = root inode allocated
    disk.writeBlock(1, buf);

    // ── 5. Data bitmap (block 2): zero, then set bit 0 (root's data block)
    std::memset(buf, 0, BLOCK_SIZE);
    buf[0] = 0x01; // bit 0 = root's pre-allocated data block
    disk.writeBlock(2, buf);

    // ── 6. Zero every inode-table block ──────────────────────────────────
    std::memset(buf, 0, BLOCK_SIZE);
    for (uint32_t i = 0; i < inode_table_blocks; ++i) {
        disk.writeBlock(3 + i, buf);
    }

    // ── 7. Write root inode (inode 0) ────────────────────────────────────
    // Root's pre-allocated data block has absolute number = data_block_start
    // (bit 0 in the data bitmap → absolute = data_block_start + 0).
    {
        // Read the inode-table block that contains inode 0, patch it, write back.
        uint32_t byte_off    = ROOT_INODE_NUM * INODE_SIZE; // = 0
        uint32_t blk_off     = byte_off / BLOCK_SIZE;       // = 0
        uint32_t byte_within = byte_off % BLOCK_SIZE;       // = 0
        disk.readBlock(3 + blk_off, buf);

        Inode root{};
        root.type        = INODE_TYPE_DIR;
        root.size        = 0;
        root.link_count  = 1;
        root.block_count = 1;
        root.direct[0]   = data_block_start; // absolute block number
        root.indirect    = 0;
        root.mode        = 0;

        std::memcpy(buf + byte_within, &root, sizeof(Inode));
        disk.writeBlock(3 + blk_off, buf);
    }

    // ── 8. Zero root's data block ─────────────────────────────────────────
    std::memset(buf, 0, BLOCK_SIZE);
    disk.writeBlock(data_block_start, buf);

    // ── 9. Done ───────────────────────────────────────────────────────────
    std::cout << "mkfs: created '" << image_path << "'"
              << " (" << total_blocks << " blocks, "
              << total_inodes << " inodes, "
              << image_size / (1024 * 1024) << " MiB)\n";
    return 0;
}

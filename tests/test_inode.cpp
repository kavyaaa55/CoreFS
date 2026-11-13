#include <doctest/doctest.h>
#include <vector>
#include "test_helpers.h"
#include "fs_core.h"
#include "constants.h"

TEST_CASE("bmap: direct range") {
    TempFS t("bmap");
    FSCore& fs = *t;
    uint32_t ino = fs.allocInode();
    Inode i{}; i.type = INODE_TYPE_FILE; i.link_count = 1; fs.writeInode(ino, i);

    // Write 1 byte at start of each direct block, verify distinct physical blocks
    std::vector<uint32_t> phys;
    for (uint32_t lb = 0; lb < NUM_DIRECT_BLOCKS; ++lb) {
        uint8_t ch = static_cast<uint8_t>('A' + lb);
        uint32_t off = lb * BLOCK_SIZE;
        fs.writeFile(ino, off, &ch, 1);
        uint8_t rd = 0;
        fs.readFile(ino, off, &rd, 1);
        CHECK(rd == ch);
    }
    fs.freeAllBlocks(ino);
    fs.freeInode(ino);
}

TEST_CASE("bmap: single-indirect boundary") {
    TempFS t("si", 1024, 32);
    FSCore& fs = *t;
    uint32_t ino = fs.allocInode();
    Inode i{}; i.type = INODE_TYPE_FILE; i.link_count = 1; fs.writeInode(ino, i);

    // Write at exact boundary: last direct (block 11) and first indirect (block 12)
    uint8_t v1 = 0xAA, v2 = 0xBB, rd = 0;
    uint32_t off_direct   = (DIRECT_MAX - 1) * BLOCK_SIZE;
    uint32_t off_indirect = DIRECT_MAX * BLOCK_SIZE;
    fs.writeFile(ino, off_direct,   &v1, 1);
    fs.writeFile(ino, off_indirect, &v2, 1);
    fs.readFile(ino, off_direct,   &rd, 1); CHECK(rd == v1);
    fs.readFile(ino, off_indirect, &rd, 1); CHECK(rd == v2);

    fs.freeAllBlocks(ino); fs.freeInode(ino);
}

TEST_CASE("truncate: shrink frees blocks, grow is sparse") {
    TempFS t("trunc", 512, 32);
    FSCore& fs = *t;
    uint32_t ino = fs.allocInode();
    Inode i{}; i.type = INODE_TYPE_FILE; i.link_count = 1; fs.writeInode(ino, i);
    uint32_t free0 = fs.superblock().free_blocks;

    // Write 3 blocks
    std::vector<uint8_t> data(BLOCK_SIZE * 3, 0x55);
    fs.writeFile(ino, 0, data.data(), static_cast<uint32_t>(data.size()));
    CHECK(fs.superblock().free_blocks == free0 - 3);

    // Truncate to 1 block
    fs.truncateFile(ino, BLOCK_SIZE);
    CHECK(fs.superblock().free_blocks == free0 - 1);

    // Grow (sparse) — no blocks allocated
    fs.truncateFile(ino, BLOCK_SIZE * 5);
    CHECK(fs.superblock().free_blocks == free0 - 1);

    // Hole reads as zeros
    uint8_t rd[8] = {0xFF};
    fs.readFile(ino, BLOCK_SIZE * 3, rd, 8);
    for (auto b : rd) CHECK(b == 0);

    fs.freeAllBlocks(ino); fs.freeInode(ino);
    CHECK(fs.superblock().free_blocks == free0);
}

TEST_CASE("write/read across block boundary") {
    TempFS t("rw", 512, 32);
    FSCore& fs = *t;
    uint32_t ino = fs.allocInode();
    Inode i{}; i.type = INODE_TYPE_FILE; i.link_count = 1; fs.writeInode(ino, i);

    // Write 8 bytes straddling block 0/1 boundary
    uint8_t w[8] = {1,2,3,4,5,6,7,8};
    fs.writeFile(ino, BLOCK_SIZE - 4, w, 8);
    uint8_t r[8] = {};
    fs.readFile(ino, BLOCK_SIZE - 4, r, 8);
    for (int k = 0; k < 8; ++k) CHECK(r[k] == w[k]);

    // Byte before the write is a zero-filled hole; byte after is past EOF
    // (size = BLOCK_SIZE + 4), so readFile must return nothing there.
    uint8_t before = 0xFF, after = 0xFF;
    CHECK(fs.readFile(ino, BLOCK_SIZE - 5, &before, 1) == 1); CHECK(before == 0);
    CHECK(fs.readFile(ino, BLOCK_SIZE + 4, &after,  1) == 0); CHECK(after  == 0xFF);

    fs.freeAllBlocks(ino); fs.freeInode(ino);
}

#include <doctest/doctest.h>
#include <vector>
#include "test_helpers.h"
#include "fs_core.h"

TEST_CASE("bitmap: alloc/free/reuse") {
    TempFS t("bm");
    FSCore& fs = *t;
    const Superblock& sb = fs.superblock();
    uint32_t free_before = sb.free_blocks;

    // Alloc one block
    uint32_t b1 = fs.allocBlock();
    CHECK(fs.superblock().free_blocks == free_before - 1);

    // Alloc another
    uint32_t b2 = fs.allocBlock();
    CHECK(b2 != b1);
    CHECK(fs.superblock().free_blocks == free_before - 2);

    // Free first, realloc should get it back
    fs.freeBlock(b1);
    CHECK(fs.superblock().free_blocks == free_before - 1);
    uint32_t b3 = fs.allocBlock();
    CHECK(b3 == b1); // bitmap scan is linear, so freed slot is reused
    fs.freeBlock(b2);
    fs.freeBlock(b3);
    CHECK(fs.superblock().free_blocks == free_before);
}

TEST_CASE("bitmap: inode alloc/free/reuse") {
    TempFS t("ino");
    FSCore& fs = *t;
    uint32_t free_before = fs.superblock().free_inodes;

    uint32_t i1 = fs.allocInode();
    uint32_t i2 = fs.allocInode();
    CHECK(i1 != i2);
    CHECK(fs.superblock().free_inodes == free_before - 2);

    fs.freeInode(i1);
    uint32_t i3 = fs.allocInode();
    CHECK(i3 == i1);
    fs.freeInode(i2);
    fs.freeInode(i3);
    CHECK(fs.superblock().free_inodes == free_before);
}

TEST_CASE("bitmap: exhaustion throws") {
    // Tiny image: 128 blocks, 16 inodes
    TempFS t("ex", 128, 16);
    FSCore& fs = *t;
    // Drain all free inodes
    std::vector<uint32_t> inodes;
    while (fs.superblock().free_inodes > 0)
        inodes.push_back(fs.allocInode());
    CHECK_THROWS(fs.allocInode());
    // Free them all
    for (auto i : inodes) fs.freeInode(i);
    CHECK(fs.superblock().free_inodes == 16 - 2); // 2 pre-allocated

    // Drain data blocks
    std::vector<uint32_t> blks;
    while (fs.superblock().free_blocks > 0)
        blks.push_back(fs.allocBlock());
    CHECK_THROWS(fs.allocBlock());
    for (auto b : blks) fs.freeBlock(b);
}

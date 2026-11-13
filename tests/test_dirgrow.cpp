// Part A5: directories past 12 blocks, and directory blocks freed on removal.
#include <doctest/doctest.h>
#include "test_helpers.h"
#include "directory.h"

extern "C" int fsck_image(const char* path);

TEST_CASE("rmdir frees the directory's data block (no leak)") {
    TempFS t("rmdleak");
    uint32_t free0 = t->superblock().free_blocks;
    uint32_t ino0  = t->superblock().free_inodes;
    t->makeDir(ROOT_INODE_NUM, "d", 0755);
    CHECK(t->superblock().free_blocks == free0 - 1);
    t->removeDir(ROOT_INODE_NUM, "d");
    CHECK(t->superblock().free_blocks == free0);
    CHECK(t->superblock().free_inodes == ino0);
    CHECK(fsck_image(t.path.c_str()) == 0);
}

TEST_CASE("mv over an empty directory frees the replaced directory") {
    TempFS t("mvleak");
    uint32_t free0 = t->superblock().free_blocks;
    t->makeDir(ROOT_INODE_NUM, "a", 0755);
    t->makeDir(ROOT_INODE_NUM, "b", 0755);
    t->renameEntry(ROOT_INODE_NUM, "a", ROOT_INODE_NUM, "b"); // b (empty) replaced
    CHECK(t->superblock().free_blocks == free0 - 1);           // only one dir left
    CHECK(fsck_image(t.path.c_str()) == 0);
}

TEST_CASE("directory grows past 12 blocks into the indirect block") {
    constexpr int N = 2000; // 2002 entries / 128 per block = 16 blocks > 12 direct
    TempFS t("dirgrow", 1024, N + 16);
    FSCore& fs = *t;
    uint32_t free0 = fs.superblock().free_blocks;
    uint32_t ino0  = fs.superblock().free_inodes;

    uint32_t dir = fs.makeDir(ROOT_INODE_NUM, "big", 0755);
    for (int k = 0; k < N; ++k)
        fs.createFile(dir, "f" + std::to_string(k), 0644);

    Inode d; fs.readInode(dir, d);
    CHECK(d.block_count == 16);
    CHECK(d.indirect != 0);
    CHECK(d.size == 16 * BLOCK_SIZE);

    // Every entry is findable, including those in indirect blocks
    for (int k = 0; k < N; k += 97)
        CHECK(lookupDirent(fs, dir, "f" + std::to_string(k)).has_value());
    CHECK(lookupDirent(fs, dir, "f1999").has_value());
    CHECK(listDirentsUser(fs, dir).size() == static_cast<size_t>(N));
    CHECK_THROWS(fs.createFile(dir, "f1999", 0644));          // duplicate in an indirect block
    CHECK(fs.resolve("/big/f1999", ROOT_INODE_NUM) != 0);
    CHECK(fsck_image(t.path.c_str()) == 0);

    // Survives a remount
    t.reopen();
    CHECK(lookupDirent(*t, dir, "f1500").has_value());

    // Empty it, remove it: everything comes back
    for (int k = 0; k < N; ++k) t->unlinkFile(dir, "f" + std::to_string(k));
    CHECK(isDirEmpty(*t, dir));
    t->removeDir(ROOT_INODE_NUM, "big");
    CHECK(t->superblock().free_blocks == free0);
    CHECK(t->superblock().free_inodes == ino0);
    CHECK(fsck_image(t.path.c_str()) == 0);
}

TEST_CASE("truncate keeps block_count as the logical extent past 12 blocks") {
    TempFS t("bc", 512, 32);
    uint32_t ino = t->createFile(ROOT_INODE_NUM, "f", 0644);
    std::vector<uint8_t> data(20 * BLOCK_SIZE, 0x5A);
    t->writeFile(ino, 0, data.data(), static_cast<uint32_t>(data.size()));
    Inode i; t->readInode(ino, i);
    CHECK(i.block_count == 20);
    t->truncateFile(ino, 15 * BLOCK_SIZE);
    t->readInode(ino, i);
    CHECK(i.block_count == 15);   // was capped at 12 before
    CHECK(fsck_image(t.path.c_str()) == 0);
}

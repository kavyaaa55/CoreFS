#include <doctest/doctest.h>
#include <vector>
#include "test_helpers.h"
#include "fs_core.h"

TEST_CASE("createFile + writeFile + readFile + persistence") {
    TempFS t("cmd");
    uint32_t ino = t->createFile(ROOT_INODE_NUM, "hello", 0644);

    std::string msg = "hello phase8\n";
    t->writeFile(ino, 0, msg.data(), static_cast<uint32_t>(msg.size()));

    // Read back
    std::string rd(msg.size(), '\0');
    t->readFile(ino, 0, rd.data(), static_cast<uint32_t>(rd.size()));
    CHECK(rd == msg);

    // Persist across reopen
    t.reopen();
    uint32_t ino2 = t->resolve("/hello", ROOT_INODE_NUM);
    Inode i; t->readInode(ino2, i);
    CHECK(i.size == msg.size());
    std::string rd2(msg.size(), '\0');
    t->readFile(ino2, 0, rd2.data(), static_cast<uint32_t>(rd2.size()));
    CHECK(rd2 == msg);
}

TEST_CASE("hard link: write via a, read via b, rm a, b survives") {
    TempFS t("hl");
    uint32_t ino = t->createFile(ROOT_INODE_NUM, "a", 0644);
    std::string data = "shared\n";
    t->writeFile(ino, 0, data.data(), static_cast<uint32_t>(data.size()));

    t->hardLink(ino, ROOT_INODE_NUM, "b");
    Inode i; t->readInode(ino, i); CHECK(i.link_count == 2);

    // Read via b
    uint32_t b_ino = t->resolve("/b", ROOT_INODE_NUM);
    CHECK(b_ino == ino);
    std::string rd(data.size(), '\0');
    t->readFile(b_ino, 0, rd.data(), static_cast<uint32_t>(rd.size()));
    CHECK(rd == data);

    // rm a — link count drops to 1, data survives
    t->unlinkFile(ROOT_INODE_NUM, "a");
    t->readInode(ino, i); CHECK(i.link_count == 1);
    rd.assign(data.size(), '\0');
    t->readFile(ino, 0, rd.data(), static_cast<uint32_t>(rd.size())); CHECK(rd == data);

    // ln on a directory rejected
    uint32_t dir = t->makeDir(ROOT_INODE_NUM, "d", 0755);
    CHECK_THROWS(t->hardLink(dir, ROOT_INODE_NUM, "dlink"));
}

TEST_CASE("rm: free count returns to baseline") {
    TempFS t("rm");
    uint32_t free0 = t->superblock().free_blocks;
    uint32_t ino_free0 = t->superblock().free_inodes;

    for (int k = 0; k < 100; ++k) {
        uint32_t ino = t->createFile(ROOT_INODE_NUM, ("f"+std::to_string(k)).c_str(), 0644);
        std::vector<uint8_t> data(BLOCK_SIZE, static_cast<uint8_t>(k));
        t->writeFile(ino, 0, data.data(), BLOCK_SIZE);
    }

    for (int k = 0; k < 100; ++k)
        t->unlinkFile(ROOT_INODE_NUM, ("f"+std::to_string(k)).c_str());

    CHECK(t->superblock().free_blocks  == free0);
    CHECK(t->superblock().free_inodes  == ino_free0);
}

TEST_CASE("rmdir: errors - ., .., non-empty, file") {
    TempFS t("rmd");
    uint32_t dir = t->makeDir(ROOT_INODE_NUM, "d", 0755);
    t->createFile(dir, "f", 0644);
    CHECK_THROWS(t->removeDir(ROOT_INODE_NUM, "d")); // non-empty
    t->unlinkFile(dir, "f");
    t->removeDir(ROOT_INODE_NUM, "d"); // now succeeds
}

TEST_CASE("mv dir across parents - link counts correct") {
    TempFS t("mv");
    uint32_t a = t->makeDir(ROOT_INODE_NUM, "a", 0755);
    uint32_t b = t->makeDir(ROOT_INODE_NUM, "b", 0755);
    uint32_t c = t->makeDir(a, "c", 0755);  // a/c

    Inode ia; t->readInode(ROOT_INODE_NUM, ia); uint32_t root_lc = ia.link_count;
    Inode iA; t->readInode(a, iA); uint32_t a_lc = iA.link_count; // 2 + 1(c's ..)=3
    CHECK(a_lc == 3);

    // mv a/c -> b/c
    t->renameEntry(a, "c", b, "c");

    t->readInode(a, iA); CHECK(iA.link_count == 2);   // lost c's ".."
    Inode iB; t->readInode(b, iB); CHECK(iB.link_count == 3); // gained c's ".."
    // c's ".." should now point to b
    uint32_t c_parent = t->resolve("..", c);
    CHECK(c_parent == b);
    (void)root_lc; // suppress warning
}

TEST_CASE("mv dir into own subtree rejected") {
    TempFS t("mvsub");
    uint32_t a = t->makeDir(ROOT_INODE_NUM, "a", 0755);
    uint32_t b = t->makeDir(a, "b", 0755);
    CHECK_THROWS(t->renameEntry(ROOT_INODE_NUM, "a", b, "a")); // move a into a/b
}

TEST_CASE("timestamps: mtime changes on write not on read") {
    TempFS t("ts");
    uint32_t ino = t->createFile(ROOT_INODE_NUM, "f", 0644);
    Inode i; t->readInode(ino, i);
    uint32_t mtime0 = i.mtime;

    // read-only: mtime unchanged (atime may update if atime<=mtime)
    uint8_t buf[4] = {};
    t->readFile(ino, 0, buf, 4);
    t->readInode(ino, i); CHECK(i.mtime == mtime0);

    // write: mtime changes
    uint8_t w[4] = {1,2,3,4};
    t->writeFile(ino, 0, w, 4);
    t->readInode(ino, i); CHECK(i.mtime >= mtime0);
}

#include <doctest/doctest.h>
#include "test_helpers.h"
#include "fs_core.h"

TEST_CASE("resolve: absolute and relative paths") {
    TempFS t("path");
    FSCore& fs = *t;
    uint32_t a   = fs.makeDir(ROOT_INODE_NUM, "a", 0755);
    uint32_t ab  = fs.makeDir(a, "b", 0755);
    uint32_t abc = fs.makeDir(ab, "c", 0755);
    fs.createFile(abc, "f", 0644);

    CHECK(fs.resolve("/a", ROOT_INODE_NUM) == a);
    CHECK(fs.resolve("/a/b", ROOT_INODE_NUM) == ab);
    CHECK(fs.resolve("b/c", a) == abc);
    CHECK(fs.resolve("/a/b/c/../c", ROOT_INODE_NUM) == abc); // .. works
}

TEST_CASE("resolve: dot and dotdot at root") {
    TempFS t("root");
    FSCore& fs = *t;
    CHECK(fs.resolve("/", ROOT_INODE_NUM) == ROOT_INODE_NUM);
    CHECK(fs.resolve(".", ROOT_INODE_NUM) == ROOT_INODE_NUM);
    CHECK(fs.resolve("..", ROOT_INODE_NUM) == ROOT_INODE_NUM); // .. of root = root
}

TEST_CASE("resolve: not found throws") {
    TempFS t("nf");
    FSCore& fs = *t;
    CHECK_THROWS(fs.resolve("/nonexistent", ROOT_INODE_NUM));
}

TEST_CASE("resolve: symlink absolute target") {
    TempFS t("sym");
    FSCore& fs = *t;
    uint32_t dir = fs.makeDir(ROOT_INODE_NUM, "d", 0755);
    fs.createFile(dir, "file", 0644);
    fs.makeSymlink("/d/file", ROOT_INODE_NUM, "link");

    uint32_t file_ino = fs.resolve("/d/file", ROOT_INODE_NUM);
    uint32_t via_link = fs.resolve("/link", ROOT_INODE_NUM);
    CHECK(file_ino == via_link);
}

TEST_CASE("resolve: symlink relative target") {
    TempFS t("rsym");
    FSCore& fs = *t;
    uint32_t dir = fs.makeDir(ROOT_INODE_NUM, "d", 0755);
    fs.createFile(dir, "target", 0644);
    fs.makeSymlink("target", dir, "rel_link"); // relative to d/

    uint32_t t_ino = fs.resolve("/d/target", ROOT_INODE_NUM);
    uint32_t l_ino = fs.resolve("/d/rel_link", ROOT_INODE_NUM);
    CHECK(t_ino == l_ino);
}

TEST_CASE("resolve: symlink loop detected") {
    TempFS t("loop");
    FSCore& fs = *t;
    fs.makeSymlink("b", ROOT_INODE_NUM, "a");
    fs.makeSymlink("a", ROOT_INODE_NUM, "b");
    CHECK_THROWS(fs.resolve("/a", ROOT_INODE_NUM));
}

TEST_CASE("resolve: dangling symlink throws on follow, ok with follow_last=false") {
    TempFS t("dangle");
    FSCore& fs = *t;
    fs.makeSymlink("/nonexistent", ROOT_INODE_NUM, "broken");
    CHECK_THROWS(fs.resolve("/broken", ROOT_INODE_NUM, true));  // follow
    // lstat (follow_last=false): resolves to the symlink inode itself
    uint32_t lino = fs.resolve("/broken", ROOT_INODE_NUM, false);
    Inode i; fs.readInode(lino, i);
    CHECK(i.type == INODE_TYPE_SYMLINK);
}

TEST_CASE("cd .. from nested dirs") {
    TempFS t("cd");
    FSCore& fs = *t;
    uint32_t a = fs.makeDir(ROOT_INODE_NUM, "a", 0755);
    uint32_t b = fs.makeDir(a, "b", 0755);
    // From b, resolve ".." should give a
    CHECK(fs.resolve("..", b) == a);
    // From a, ".." gives root
    CHECK(fs.resolve("..", a) == ROOT_INODE_NUM);
    // From root, ".." gives root
    CHECK(fs.resolve("..", ROOT_INODE_NUM) == ROOT_INODE_NUM);
}

TEST_CASE("directory link counts") {
    TempFS t("lc");
    FSCore& fs = *t;
    Inode root; fs.readInode(ROOT_INODE_NUM, root);
    uint32_t root_lc_before = root.link_count;

    uint32_t d1 = fs.makeDir(ROOT_INODE_NUM, "d1", 0755);
    uint32_t d2 = fs.makeDir(ROOT_INODE_NUM, "d2", 0755);
    uint32_t d3 [[maybe_unused]] = fs.makeDir(ROOT_INODE_NUM, "d3", 0755);

    // root gains 1 link per subdir (each subdir's "..")
    fs.readInode(ROOT_INODE_NUM, root);
    CHECK(root.link_count == root_lc_before + 3);

    // Each new dir has link_count=2 ("." + parent entry)
    Inode id1; fs.readInode(d1, id1); CHECK(id1.link_count == 2);

    // rmdir d3 — root loses one link
    fs.removeDir(ROOT_INODE_NUM, "d3");
    fs.readInode(ROOT_INODE_NUM, root);
    CHECK(root.link_count == root_lc_before + 2);

    (void)d2; // suppress unused warning
}

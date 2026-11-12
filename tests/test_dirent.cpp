#include <doctest/doctest.h>
#include "test_helpers.h"
#include "directory.h"
#include "fs_core.h"

TEST_CASE("dirent: add/lookup/remove/reuse slot") {
    TempFS t("dir");
    FSCore& fs = *t;
    uint32_t dir = fs.makeDir(ROOT_INODE_NUM, "d", 0755);

    // Add and find
    CHECK(addDirent(fs, dir, "foo", 42));
    auto r = lookupDirent(fs, dir, "foo");
    REQUIRE(r.has_value()); CHECK(*r == 42);

    // Duplicate rejected
    CHECK_FALSE(addDirent(fs, dir, "foo", 99));

    // Remove, then slot is reusable
    CHECK(removeDirent(fs, dir, "foo"));
    CHECK_FALSE(lookupDirent(fs, dir, "foo").has_value());

    // Add with same name after remove — succeeds, slot reused
    CHECK(addDirent(fs, dir, "foo", 7));
    CHECK(lookupDirent(fs, dir, "foo").value() == 7);
}

TEST_CASE("dirent: isDirEmpty respects only . and ..") {
    TempFS t("de");
    FSCore& fs = *t;
    uint32_t dir = fs.makeDir(ROOT_INODE_NUM, "e", 0755);
    CHECK(isDirEmpty(fs, dir));
    addDirent(fs, dir, "x", 5);
    CHECK_FALSE(isDirEmpty(fs, dir));
    removeDirent(fs, dir, "x");
    CHECK(isDirEmpty(fs, dir));
}

TEST_CASE("dirent: replaceDirent") {
    TempFS t("rep");
    FSCore& fs = *t;
    uint32_t dir = fs.makeDir(ROOT_INODE_NUM, "r", 0755);
    addDirent(fs, dir, "k", 10);
    CHECK(replaceDirent(fs, dir, "k", 20));
    CHECK(lookupDirent(fs, dir, "k").value() == 20);
    CHECK_FALSE(replaceDirent(fs, dir, "nosuch", 1));
}

TEST_CASE("dirent: listDirentsUser excludes . and ..") {
    TempFS t("lst");
    FSCore& fs = *t;
    uint32_t dir = fs.makeDir(ROOT_INODE_NUM, "lst", 0755);
    addDirent(fs, dir, "a", 5);
    addDirent(fs, dir, "b", 6);
    auto u = listDirentsUser(fs, dir);
    CHECK(u.size() == 2);
    for (const auto& d : u) {
        CHECK(std::string(d.name) != ".");
        CHECK(std::string(d.name) != "..");
    }
}

// Part A: shell paths (A1), ls -l type char (A2), quoting (A3), permissions (A4).
#include <doctest/doctest.h>
#include "test_helpers.h"
#include "shell.h"

#include <iostream>
#include <sstream>

extern "C" int fsck_image(const char* path);

// Runs shell command lines against a fresh image and captures their output.
struct ShellT {
    std::string path;
    Shell* sh;
    explicit ShellT(const char* tag) : path(tmp_img(tag)) {
        make_test_image(path, 512, 64);
        sh = new Shell(path);
    }
    ~ShellT() { delete sh; std::remove(path.c_str()); }
    std::string operator()(const std::string& line) {
        std::ostringstream os;
        struct Restore { std::streambuf* old; ~Restore() { std::cout.rdbuf(old); } }
            r{std::cout.rdbuf(os.rdbuf())};
        sh->execLine(line);
        return os.str();
    }
    int fsck() { return fsck_image(path.c_str()); }
};

static bool has(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

// ── A1: paths ──────────────────────────────────────────────────────────────
TEST_CASE("shell: every command takes paths") {
    ShellT s("sh_paths");
    CHECK(s("mkdir -p a/b/c") == "");
    CHECK(s("ls a/b") == "c\n");
    CHECK(s("mkdir a/b/c") != "");                 // already exists -> error message
    CHECK(s("touch a/b/c/f") == "");
    CHECK(s("echo hi > a/b/c/f") == "");
    CHECK(s("cat /a/b/c/f") == "hi\n");
    CHECK(s("echo more >> /a/b/c/f") == "");
    CHECK(s("cd a/b") == "");
    CHECK(s("pwd") == "/a/b\n");
    CHECK(s("cat c/f") == "hi\nmore\n");
    CHECK(s("cat ../../a/b/c/f") == "hi\nmore\n");
    CHECK(s("write c/f replaced") == "");
    CHECK(s("cat c/f") == "replaced\n");
    CHECK(s("cd") == "");                           // no arg -> /
    CHECK(s("pwd") == "/\n");
    CHECK(s("rm a/b/c/f") == "");
    CHECK(s("ls a/b/c") == "");
    CHECK(s("rm a/b/c") != "");                     // a directory
    CHECK(s("rmdir a/b/c") == "");
    CHECK(s("ls a/b") == "");
    CHECK(s("rmdir a") != "");                      // not empty
    CHECK(s("rmdir a/b a") == "");                  // several at once, in order
    CHECK(s("ls nothere") != "");
    CHECK(s.fsck() == 0);
}

TEST_CASE("shell: cp / mv / ln with paths and directory destinations") {
    ShellT s("sh_cpmv");
    s("mkdir docs"); s("mkdir notes");
    s("echo x > docs/a");
    CHECK(s("cp docs/a notes/") == "");             // into dir, keeps the name
    CHECK(s("cat notes/a") == "x\n");
    CHECK(s("cp docs/a /notes/b") == "");           // new name in another dir
    CHECK(s("cat notes/b") == "x\n");
    CHECK(has(s("cp docs/a docs"), "same file"));   // onto itself via its dir
    CHECK(has(s("cp docs/a docs/a"), "same file"));
    CHECK(s("cat docs/a") == "x\n");                // and the data is intact
    CHECK(has(s("cp docs/a missing/"), "not a directory"));
    CHECK(has(s("cp docs notes"), "Is a directory"));

    CHECK(s("mv docs/a notes/c") == "");            // move + rename across dirs
    CHECK(s("ls docs") == "");
    CHECK(s("mv notes docs/") == "");               // move a dir into a dir
    CHECK(s("ls docs") == "notes\n");
    CHECK(s("cat docs/notes/c") == "x\n");
    CHECK(has(s("mv docs docs/notes"), "subtree")); // into its own subtree

    CHECK(s("ln docs/notes/c docs/hard") == "");
    CHECK(has(s("stat docs/hard"), "links: 2"));
    CHECK(s("ln -s notes/c docs/soft") == "");      // relative to the link's dir
    CHECK(s("cat docs/soft") == "x\n");
    CHECK(s("readlink docs/soft") == "notes/c\n");
    CHECK(s("ln -s /docs/notes/b docs") == "");     // into a dir: link takes the target's name
    CHECK(s("cat docs/b") == "x\n");
    CHECK(has(s("ln -s /docs/notes/b docs/notes"), "already exists")); // docs/notes/b is taken
    CHECK(s("rm docs/b") == "");
    CHECK(s("rm docs/soft") == "");                 // removes the link, not the target
    CHECK(s("cat docs/notes/c") == "x\n");
    CHECK(s("rm docs/hard") == "");
    CHECK(has(s("stat docs/notes/c"), "links: 1"));
    CHECK(s.fsck() == 0);
}

TEST_CASE("shell: moving the current directory keeps the prompt correct") {
    ShellT s("sh_mvcwd");
    s("mkdir -p x/y");
    s("cd x/y");
    CHECK(s("mv /x /z") == "");
    CHECK(s("pwd") == "/z/y\n");
    CHECK(has(s("rmdir /z/y"), "current directory"));
    s("cd /");
    CHECK(s("rmdir /z/y") == "");
    CHECK(s.fsck() == 0);
}

// ── A2: ls -l ──────────────────────────────────────────────────────────────
TEST_CASE("shell: ls -l uses - for files, d for dirs, l for symlinks") {
    ShellT s("sh_lsl");
    s("echo y > f"); s("mkdir d"); s("ln -s f l");
    CHECK(s("ls -l f").rfind("-rw-r--r--  1  2\tf", 0) == 0);
    std::string out = s("ls -l");
    CHECK(has(out, "drwxr-xr-x  2  4096\td\n"));
    CHECK(has(out, "lrwxrwxrwx  1  1\tl -> f\n"));
    CHECK(has(out, "-rw-r--r--  1  2\tf\n"));
}

// ── A3: quoting ────────────────────────────────────────────────────────────
TEST_CASE("shell: quotes and escapes") {
    ShellT s("sh_quote");
    s("echo \"a  b\" > q");
    CHECK(s("cat q") == "a  b\n");                  // spacing kept
    s("echo '>' > g");
    CHECK(s("cat g") == ">\n");                     // quoted > is text, not a redirect
    s("echo a\\ b > h");
    CHECK(s("cat h") == "a b\n");
    s("echo \"say \\\"hi\\\"\" > e");
    CHECK(s("cat e") == "say \"hi\"\n");
    CHECK(s("touch \"my file\"") == "");
    CHECK(has(s("ls"), "my file\n"));
    CHECK(s("rm 'my file'") == "");
    CHECK(has(s("echo \"oops > x"), "unterminated"));
    CHECK(s("echo plain words") == "plain words\n");
}

// ── A4: permissions ────────────────────────────────────────────────────────
TEST_CASE("shell: file permissions are enforced") {
    ShellT s("sh_permf");
    s("echo s > f");
    s("chmod 000 f");
    CHECK(has(s("cat f"), "Permission denied"));
    CHECK(has(s("cp f g"), "Permission denied"));
    s("chmod 400 f");
    CHECK(s("cat f") == "s\n");
    CHECK(has(s("echo t > f"), "Permission denied"));
    CHECK(has(s("write f t"), "Permission denied"));
    CHECK(has(s("echo t >> f"), "Permission denied"));
    s("echo other > g");
    CHECK(has(s("cp g f"), "Permission denied"));   // overwrite needs w on dst
    CHECK(s("cat f") == "s\n");
    CHECK(has(s("stat f"), "mode: 400"));
    CHECK(s("rm f") == "");                         // deleting needs w on the dir, not the file
}

TEST_CASE("shell: directory permissions are enforced") {
    ShellT s("sh_permd");
    s("mkdir d"); s("echo z > d/f");

    s("chmod 600 d");                               // rw-, no x: cannot enter or search
    CHECK(has(s("cd d"), "Permission denied"));
    CHECK(has(s("cat d/f"), "Permission denied"));
    CHECK(has(s("touch d/n"), "Permission denied"));

    s("chmod 500 d");                               // r-x, no w: cannot create/delete/rename in it
    CHECK(s("cat d/f") == "z\n");
    CHECK(has(s("touch d/n"), "Permission denied"));
    CHECK(has(s("rm d/f"), "Permission denied"));
    CHECK(has(s("mv d/f g"), "Permission denied"));
    CHECK(has(s("mkdir d/sub"), "Permission denied"));
    CHECK(s("echo w > d/f") == "");                 // the file itself is still writable
    CHECK(s("cd d") == "");
    CHECK(s("cd /") == "");

    s("chmod 300 d");                               // -wx, no r: cannot list
    CHECK(has(s("ls d"), "Permission denied"));
    CHECK(s("cat d/f") == "w\n");

    s("chmod 755 d");
    CHECK(s("ls d") == "f\n");
    CHECK(s("rm d/f") == "");
    CHECK(s("rmdir d") == "");
    CHECK(s.fsck() == 0);
}

#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include "fs_core.h"

class Shell {
public:
    explicit Shell(const std::string& image_path);
    void run();

    // Parse and run one command line (output goes to std::cout).
    // Returns false if the line was "exit"/"quit". Used by run() and by tests.
    bool execLine(const std::string& line);

private:
    FSCore   fs_;
    uint32_t cwd_;          // current directory inode

    std::string cwdPath();  // walks ".." up to root via on-disk dirents
    void dispatch(const std::vector<std::string>& args_with_cmd,
                  const std::vector<bool>& is_redirect);

    // Path helpers. Every command accepts absolute or relative paths.
    // splitPath("a/b/c") -> (inode of dir "a/b", "c"); needs search (x) on the dir.
    std::pair<uint32_t, std::string> splitPath(const std::string& path);
    // Inode `path` names if it exists, else 0 with ok=false.
    uint32_t tryResolve(const std::string& path, bool follow_last, bool& ok);
    // Where a copy/move/link named `dst` goes: into `dst` if it is an existing
    // directory (keeping `src_name`), else the parent of `dst` with its last name.
    std::pair<uint32_t, std::string> destination(const std::string& dst,
                                                 const std::string& src_name);
    void printLong(const std::string& name, uint32_t ino);

    void cmd_pwd();
    void cmd_ls(const std::vector<std::string>& args);
    void cmd_cd(const std::vector<std::string>& args);
    void cmd_mkdir(const std::vector<std::string>& args);
    void cmd_touch(const std::vector<std::string>& args);
    void cmd_stat(const std::vector<std::string>& args);
    void cmd_cat(const std::vector<std::string>& args);
    void cmd_write(const std::vector<std::string>& args);
    void cmd_echo(const std::string& path, const std::string& text, bool append);
    void cmd_rm(const std::vector<std::string>& args);
    void cmd_rmdir(const std::vector<std::string>& args);
    void cmd_cp(const std::vector<std::string>& args);
    void cmd_mv(const std::vector<std::string>& args);
    void cmd_df();
    void cmd_ln(const std::vector<std::string>& args);
    void cmd_readlink(const std::vector<std::string>& args);
    void cmd_chmod(const std::vector<std::string>& args);
    void cmd_help();
};

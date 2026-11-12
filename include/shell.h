#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "fs_core.h"

class Shell {
public:
    explicit Shell(const std::string& image_path);
    void run();

private:
    FSCore   fs_;
    uint32_t cwd_;          // current directory inode (Phase 7: single inode, not a stack)

    std::string cwdPath();  // walks ".." up to root via on-disk dirents
    void dispatch(const std::vector<std::string>& tokens);

    void cmd_pwd();
    void cmd_ls(const std::vector<std::string>& args);
    void cmd_cd(const std::vector<std::string>& args);
    void cmd_mkdir(const std::vector<std::string>& args);
    void cmd_touch(const std::vector<std::string>& args);
    void cmd_stat(const std::vector<std::string>& args);
    void cmd_cat(const std::vector<std::string>& args);
    void cmd_write(const std::vector<std::string>& args);
    void cmd_echo(const std::vector<std::string>& args, bool append);
    void cmd_rm(const std::vector<std::string>& args);
    void cmd_rmdir(const std::vector<std::string>& args);
    void cmd_cp(const std::vector<std::string>& args);
    void cmd_mv(const std::vector<std::string>& args);
    void cmd_df();
    // Phase 7
    void cmd_ln(const std::vector<std::string>& args);
    void cmd_readlink(const std::vector<std::string>& args);
    void cmd_chmod(const std::vector<std::string>& args);
};

#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include "fs_core.h"

class Shell {
public:
    explicit Shell(const std::string& image_path); // constructs FSCore, sets cwd to root
    void run(); // REPL loop: print prompt, read line, dispatch, repeat until "exit"/EOF

private:
    FSCore fs_;

    // Each entry: {inode_num, name_entered_by}.
    // Empty stack = at root ("/").
    std::vector<std::pair<uint32_t, std::string>> cwd_stack_;

    // Returns current directory inode: root if stack empty, else cwd_stack_.back().first
    uint32_t cwdInode() const;

    // Builds printable path string from cwd_stack_
    std::string cwdPath() const;

    // Tokenizes a line and dispatches to command handlers
    void dispatch(const std::vector<std::string>& tokens);

    // ── Implemented commands ───────────────────────────────────────────────
    void cmd_pwd();
    void cmd_ls(const std::vector<std::string>& args);
    void cmd_cd(const std::vector<std::string>& args);
    void cmd_mkdir(const std::vector<std::string>& args);
    void cmd_touch(const std::vector<std::string>& args);
    void cmd_stat(const std::vector<std::string>& args);

    // ── Phase 6 stubs ──────────────────────────────────────────────────────
    void cmd_cat(const std::vector<std::string>& args);
    void cmd_write(const std::vector<std::string>& args);
    void cmd_rm(const std::vector<std::string>& args);
    void cmd_rmdir(const std::vector<std::string>& args);
    void cmd_cp(const std::vector<std::string>& args);
    void cmd_mv(const std::vector<std::string>& args);
};

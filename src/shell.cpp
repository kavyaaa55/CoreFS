#include "shell.h"
#include "directory.h"
#include "constants.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <algorithm>

// ── Constructor ────────────────────────────────────────────────────────────
Shell::Shell(const std::string& image_path)
    : fs_(image_path)
{
    // cwd_stack_ is empty → at root
}

// ── Helpers ────────────────────────────────────────────────────────────────
uint32_t Shell::cwdInode() const {
    if (cwd_stack_.empty()) return ROOT_INODE_NUM;
    return cwd_stack_.back().first;
}

std::string Shell::cwdPath() const {
    if (cwd_stack_.empty()) return "/";
    std::string path;
    for (const auto& [ino, name] : cwd_stack_) {
        path += '/';
        path += name;
    }
    return path;
}

// ── Tokeniser ──────────────────────────────────────────────────────────────
static std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream ss(line);
    std::string tok;
    while (ss >> tok) tokens.push_back(tok);
    return tokens;
}

// ── REPL ───────────────────────────────────────────────────────────────────
void Shell::run() {
    std::string line;
    while (true) {
        std::cout << cwdPath() << " > " << std::flush;
        if (!std::getline(std::cin, line)) break; // EOF

        auto tokens = tokenize(line);
        if (tokens.empty()) continue;

        const std::string& cmd = tokens[0];
        if (cmd == "exit" || cmd == "quit") break;

        try {
            dispatch(tokens);
        } catch (const std::exception& e) {
            std::cout << "error: " << e.what() << "\n";
        }
    }
}

// ── Dispatch ───────────────────────────────────────────────────────────────
void Shell::dispatch(const std::vector<std::string>& tokens) {
    const std::string& cmd = tokens[0];
    std::vector<std::string> args(tokens.begin() + 1, tokens.end());

    if      (cmd == "pwd")   cmd_pwd();
    else if (cmd == "ls")    cmd_ls(args);
    else if (cmd == "cd")    cmd_cd(args);
    else if (cmd == "mkdir") cmd_mkdir(args);
    else if (cmd == "touch") cmd_touch(args);
    else if (cmd == "stat")  cmd_stat(args);
    else if (cmd == "cat")   cmd_cat(args);
    else if (cmd == "write") cmd_write(args);
    else if (cmd == "rm")    cmd_rm(args);
    else if (cmd == "rmdir") cmd_rmdir(args);
    else if (cmd == "cp")    cmd_cp(args);
    else if (cmd == "mv")    cmd_mv(args);
    else {
        std::cout << cmd << ": command not found\n";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// IMPLEMENTED COMMANDS
// ═══════════════════════════════════════════════════════════════════════════

// ── pwd ────────────────────────────────────────────────────────────────────
void Shell::cmd_pwd() {
    std::cout << cwdPath() << "\n";
}

// ── ls [-l] ────────────────────────────────────────────────────────────────
void Shell::cmd_ls(const std::vector<std::string>& args) {
    bool long_fmt = false;
    for (const auto& a : args) {
        if (a == "-l") {
            long_fmt = true;
        } else {
            std::cout << "usage: ls [-l]\n";
            return;
        }
    }

    auto dirents = listDirents(fs_, cwdInode());

    for (const auto& d : dirents) {
        if (!long_fmt) {
            std::cout << d.name << "\n";
        } else {
            Inode inode;
            fs_.readInode(d.inode_num, inode);
            char type_char = (inode.type == INODE_TYPE_DIR) ? 'd' : 'f';
            std::cout << type_char << "  " << inode.size << "\t" << d.name << "\n";
        }
    }
}

// ── cd <path> ──────────────────────────────────────────────────────────────
void Shell::cmd_cd(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "usage: cd <path>\n";
        return;
    }
    const std::string& path = args[0];

    // Build a temporary stack following §7.4 algorithm.
    std::vector<std::pair<uint32_t, std::string>> tmp_stack;

    if (path[0] == '/') {
        // Absolute path — start from root (empty stack)
        tmp_stack.clear();
    } else {
        // Relative path — start from current stack
        tmp_stack = cwd_stack_;
    }

    // Split the path into components
    std::vector<std::string> components;
    {
        std::istringstream ss(path[0] == '/' ? path.substr(1) : path);
        std::string part;
        while (std::getline(ss, part, '/')) {
            if (!part.empty()) components.push_back(part);
        }
    }

    for (const auto& comp : components) {
        if (comp == ".") {
            continue; // no-op
        } else if (comp == "..") {
            if (!tmp_stack.empty()) tmp_stack.pop_back();
            // already at root — silently ignore
        } else {
            // Look up comp in the directory implied by tmp_stack
            uint32_t parent_inode = tmp_stack.empty() ? ROOT_INODE_NUM
                                                       : tmp_stack.back().first;
            auto found = lookupDirent(fs_, parent_inode, comp);
            if (!found) {
                std::cout << "cd: " << path << ": No such file or directory\n";
                return;
            }
            Inode inode;
            fs_.readInode(*found, inode);
            if (inode.type != INODE_TYPE_DIR) {
                std::cout << "cd: " << path << ": Not a directory\n";
                return;
            }
            tmp_stack.push_back({*found, comp});
        }
    }

    cwd_stack_ = std::move(tmp_stack);
}

// ── mkdir <name> ───────────────────────────────────────────────────────────
void Shell::cmd_mkdir(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "usage: mkdir <name>\n";
        return;
    }
    const std::string& name = args[0];

    if (name.empty() || name.find('/') != std::string::npos ||
        name.size() > MAX_FILENAME_LEN) {
        std::cout << "mkdir: invalid name\n";
        return;
    }

    uint32_t cur = cwdInode();

    if (lookupDirent(fs_, cur, name)) {
        std::cout << "mkdir: " << name << ": File exists\n";
        return;
    }

    uint32_t new_ino = fs_.allocInode();

    Inode inode{};
    inode.type        = INODE_TYPE_DIR;
    inode.size        = 0;
    inode.link_count  = 1;
    inode.block_count = 0;
    inode.indirect    = 0;
    inode.mode        = 0;
    fs_.writeInode(new_ino, inode);

    if (!addDirent(fs_, cur, name, new_ino)) {
        fs_.freeInode(new_ino);
        std::cout << "mkdir: " << name << ": No space left in directory\n";
        return;
    }
}

// ── touch <name> ───────────────────────────────────────────────────────────
void Shell::cmd_touch(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "usage: touch <name>\n";
        return;
    }
    const std::string& name = args[0];

    if (name.empty() || name.find('/') != std::string::npos ||
        name.size() > MAX_FILENAME_LEN) {
        std::cout << "touch: invalid name\n";
        return;
    }

    uint32_t cur = cwdInode();

    if (lookupDirent(fs_, cur, name)) {
        std::cout << "touch: " << name << ": File exists\n";
        return;
    }

    uint32_t new_ino = fs_.allocInode();

    Inode inode{};
    inode.type        = INODE_TYPE_FILE;
    inode.size        = 0;
    inode.link_count  = 1;
    inode.block_count = 0;
    inode.indirect    = 0;
    inode.mode        = 0;
    fs_.writeInode(new_ino, inode);

    if (!addDirent(fs_, cur, name, new_ino)) {
        fs_.freeInode(new_ino);
        std::cout << "touch: " << name << ": No space left in directory\n";
        return;
    }
}

// ── stat <name> ────────────────────────────────────────────────────────────
void Shell::cmd_stat(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "usage: stat <name>\n";
        return;
    }
    const std::string& name = args[0];
    uint32_t cur = cwdInode();

    auto found = lookupDirent(fs_, cur, name);
    if (!found) {
        std::cout << "stat: " << name << ": No such file or directory\n";
        return;
    }

    Inode inode;
    fs_.readInode(*found, inode);

    const char* type_str = (inode.type == INODE_TYPE_DIR)  ? "directory" :
                           (inode.type == INODE_TYPE_FILE) ? "file"      :
                                                              "unknown";

    std::cout << "inode: "  << *found
              << "  type: "  << type_str
              << "  size: "  << inode.size
              << "  blocks: " << inode.block_count
              << "  links: "  << inode.link_count
              << "\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// PHASE 6 STUBS
// ═══════════════════════════════════════════════════════════════════════════

void Shell::cmd_cat(const std::vector<std::string>& /*args*/) {
    // TODO(phase6): implement
    std::cout << "cat: not implemented yet\n";
}

void Shell::cmd_write(const std::vector<std::string>& /*args*/) {
    // TODO(phase6): implement
    std::cout << "write: not implemented yet\n";
}

void Shell::cmd_rm(const std::vector<std::string>& /*args*/) {
    // TODO(phase6): implement
    std::cout << "rm: not implemented yet\n";
}

void Shell::cmd_rmdir(const std::vector<std::string>& /*args*/) {
    // TODO(phase6): implement
    std::cout << "rmdir: not implemented yet\n";
}

void Shell::cmd_cp(const std::vector<std::string>& /*args*/) {
    // TODO(phase6): implement
    std::cout << "cp: not implemented yet\n";
}

void Shell::cmd_mv(const std::vector<std::string>& /*args*/) {
    // TODO(phase6): implement
    std::cout << "mv: not implemented yet\n";
}

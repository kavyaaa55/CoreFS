#include "shell.h"
#include "directory.h"
#include "constants.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>

// ── Constructor ────────────────────────────────────────────────────────────
Shell::Shell(const std::string& image_path)
    : fs_(image_path)
{
    // cwd_stack_ empty → at root
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
        if (!std::getline(std::cin, line)) break;

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

    // Handle:  echo <text...> > <file>   and   echo <text...> >> <file>
    if (cmd == "echo") {
        // Look for > or >> at any position in args
        bool found_redirect = false;
        bool append = false;
        size_t redir_pos = 0;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == ">>" || args[i] == ">") {
                append = (args[i] == ">>");
                redir_pos = i;
                found_redirect = true;
                break;
            }
        }
        if (found_redirect) {
            if (redir_pos + 1 >= args.size()) {
                std::cout << "echo: missing filename after redirect\n";
                return;
            }
            // Text is everything before the redirect operator
            std::string text;
            for (size_t i = 0; i < redir_pos; ++i) {
                if (i > 0) text += ' ';
                text += args[i];
            }
            const std::string& filename = args[redir_pos + 1];
            std::vector<std::string> echo_args = {filename, text};
            cmd_echo(echo_args, append);
            return;
        }
        // Plain echo with no redirect — just print to stdout
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) std::cout << ' ';
            std::cout << args[i];
        }
        std::cout << '\n';
        return;
    }

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
    else if (cmd == "df")    cmd_df();
    else {
        std::cout << cmd << ": command not found\n";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PHASE 5 ORIGINAL COMMANDS
// ═══════════════════════════════════════════════════════════════════════════

void Shell::cmd_pwd() {
    std::cout << cwdPath() << "\n";
}

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

void Shell::cmd_cd(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "usage: cd <path>\n";
        return;
    }
    const std::string& path = args[0];

    std::vector<std::pair<uint32_t, std::string>> tmp_stack;

    if (path[0] == '/') {
        tmp_stack.clear();
    } else {
        tmp_stack = cwd_stack_;
    }

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
            continue;
        } else if (comp == "..") {
            if (!tmp_stack.empty()) tmp_stack.pop_back();
        } else {
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

void Shell::cmd_mkdir(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "usage: mkdir <name>\n";
        return;
    }
    const std::string& name = args[0];

    if (name.empty() || name == "." || name == ".." ||
        name.find('/') != std::string::npos || name.size() > MAX_FILENAME_LEN) {
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
    }
}

void Shell::cmd_touch(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "usage: touch <name>\n";
        return;
    }
    const std::string& name = args[0];

    if (name.empty() || name == "." || name == ".." ||
        name.find('/') != std::string::npos || name.size() > MAX_FILENAME_LEN) {
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
    }
}

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

    std::cout << "inode: "   << *found
              << "  type: "  << type_str
              << "  size: "  << inode.size
              << "  blocks: " << inode.block_count
              << "  links: "  << inode.link_count
              << "\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// NEWLY IMPLEMENTED COMMANDS
// ═══════════════════════════════════════════════════════════════════════════

// ── cat <name> ─────────────────────────────────────────────────────────────
void Shell::cmd_cat(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "usage: cat <name>\n";
        return;
    }
    const std::string& name = args[0];
    uint32_t cur = cwdInode();

    auto found = lookupDirent(fs_, cur, name);
    if (!found) {
        std::cout << "cat: " << name << ": No such file or directory\n";
        return;
    }

    Inode inode;
    fs_.readInode(*found, inode);
    if (inode.type == INODE_TYPE_DIR) {
        std::cout << "cat: " << name << ": Is a directory\n";
        return;
    }

    if (inode.size == 0) return; // empty file — nothing to print

    // Read in chunks and write to stdout
    static constexpr uint32_t CHUNK = 4096;
    uint8_t buf[CHUNK];
    uint32_t off = 0;
    while (off < inode.size) {
        uint32_t n = fs_.readFile(*found, off, buf, CHUNK);
        if (n == 0) break;
        std::cout.write(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(n));
        off += n;
    }
    // Ensure a trailing newline if file doesn't end with one
    if (inode.size > 0) {
        uint8_t last = 0;
        fs_.readFile(*found, inode.size - 1, &last, 1);
        if (last != '\n') std::cout << '\n';
    }
}

// ── write <name> <text...> ─────────────────────────────────────────────────
// Truncates the file and writes the given text (tokens joined with spaces).
void Shell::cmd_write(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cout << "usage: write <name> <text...>\n";
        return;
    }
    const std::string& name = args[0];
    uint32_t cur = cwdInode();

    auto found = lookupDirent(fs_, cur, name);
    if (!found) {
        std::cout << "write: " << name << ": No such file or directory\n";
        return;
    }

    Inode inode;
    fs_.readInode(*found, inode);
    if (inode.type == INODE_TYPE_DIR) {
        std::cout << "write: " << name << ": Is a directory\n";
        return;
    }

    // Build the text from remaining args
    std::string text;
    for (size_t i = 1; i < args.size(); ++i) {
        if (i > 1) text += ' ';
        text += args[i];
    }
    text += '\n';

    // Truncate then write from offset 0
    fs_.truncateFile(*found, 0);
    fs_.writeFile(*found, 0,
                  reinterpret_cast<const uint8_t*>(text.c_str()),
                  static_cast<uint32_t>(text.size()));
}

// ── cmd_echo: internal helper for  echo text > file  and  echo text >> file
void Shell::cmd_echo(const std::vector<std::string>& args, bool append) {
    // args[0] = filename, args[1] = text to write
    if (args.size() < 1) return;
    const std::string& name = args[0];
    std::string text = (args.size() >= 2) ? args[1] : "";
    text += '\n';

    uint32_t cur = cwdInode();

    // Create the file if it doesn't exist
    auto found = lookupDirent(fs_, cur, name);
    if (!found) {
        // auto-create
        uint32_t new_ino = fs_.allocInode();
        Inode ni{};
        ni.type = INODE_TYPE_FILE;
        ni.size = 0;
        ni.link_count = 1;
        ni.block_count = 0;
        ni.indirect = 0;
        ni.mode = 0;
        fs_.writeInode(new_ino, ni);
        if (!addDirent(fs_, cur, name, new_ino)) {
            fs_.freeInode(new_ino);
            std::cout << "echo: " << name << ": No space left in directory\n";
            return;
        }
        found = new_ino;
    }

    Inode inode;
    fs_.readInode(*found, inode);
    if (inode.type == INODE_TYPE_DIR) {
        std::cout << "echo: " << name << ": Is a directory\n";
        return;
    }

    uint32_t write_off = append ? inode.size : 0u;
    if (!append) fs_.truncateFile(*found, 0);

    fs_.writeFile(*found, write_off,
                  reinterpret_cast<const uint8_t*>(text.c_str()),
                  static_cast<uint32_t>(text.size()));
}

// ── rm <name> ──────────────────────────────────────────────────────────────
void Shell::cmd_rm(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "usage: rm <name>\n";
        return;
    }
    const std::string& name = args[0];
    uint32_t cur = cwdInode();

    auto found = lookupDirent(fs_, cur, name);
    if (!found) {
        std::cout << "rm: " << name << ": No such file or directory\n";
        return;
    }

    Inode inode;
    fs_.readInode(*found, inode);
    if (inode.type == INODE_TYPE_DIR) {
        std::cout << "rm: " << name << ": Is a directory (use rmdir)\n";
        return;
    }

    // Remove from directory, decrement link count, free if zero
    removeDirent(fs_, cur, name);
    inode.link_count--;
    fs_.writeInode(*found, inode);
    fs_.unlinkIfUnreferenced(*found);
}

// ── rmdir <name> ───────────────────────────────────────────────────────────
void Shell::cmd_rmdir(const std::vector<std::string>& args) {
    if (args.size() != 1) {
        std::cout << "usage: rmdir <name>\n";
        return;
    }
    const std::string& name = args[0];
    uint32_t cur = cwdInode();

    auto found = lookupDirent(fs_, cur, name);
    if (!found) {
        std::cout << "rmdir: " << name << ": No such file or directory\n";
        return;
    }

    Inode inode;
    fs_.readInode(*found, inode);
    if (inode.type != INODE_TYPE_DIR) {
        std::cout << "rmdir: " << name << ": Not a directory\n";
        return;
    }

    if (!isDirEmpty(fs_, *found)) {
        std::cout << "rmdir: " << name << ": Directory not empty\n";
        return;
    }

    // Guard: refuse to rmdir our current working directory
    for (const auto& [ino, n] : cwd_stack_) {
        if (ino == *found) {
            std::cout << "rmdir: " << name << ": Cannot remove current working directory\n";
            return;
        }
    }

    removeDirent(fs_, cur, name);
    inode.link_count--;
    fs_.writeInode(*found, inode);
    fs_.unlinkIfUnreferenced(*found);
}

// ── cp <src> <dst> ─────────────────────────────────────────────────────────
// Copies a file. If dst is an existing directory, copies src into it.
// Overwrites existing dst file (truncates first).
void Shell::cmd_cp(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        std::cout << "usage: cp <src> <dst>\n";
        return;
    }
    const std::string& src_name = args[0];
    std::string        dst_name = args[1];
    uint32_t cur = cwdInode();

    // Resolve src
    auto src_ino = lookupDirent(fs_, cur, src_name);
    if (!src_ino) {
        std::cout << "cp: " << src_name << ": No such file or directory\n";
        return;
    }
    Inode src_inode;
    fs_.readInode(*src_ino, src_inode);
    if (src_inode.type == INODE_TYPE_DIR) {
        std::cout << "cp: " << src_name << ": Is a directory\n";
        return;
    }

    // Determine destination directory and final filename
    uint32_t dst_dir_ino = cur;
    auto dst_check = lookupDirent(fs_, cur, dst_name);
    if (dst_check) {
        Inode dst_check_inode;
        fs_.readInode(*dst_check, dst_check_inode);
        if (dst_check_inode.type == INODE_TYPE_DIR) {
            // Copy into the directory with the same filename
            dst_dir_ino = *dst_check;
            dst_name    = src_name;
        }
        // else: overwrite existing file — dst_dir_ino stays cur, dst_name stays
    }

    if (dst_name.empty() || dst_name.find('/') != std::string::npos ||
        dst_name.size() > MAX_FILENAME_LEN) {
        std::cout << "cp: invalid destination name\n";
        return;
    }

    // Get or create destination inode
    uint32_t dst_ino;
    auto existing = lookupDirent(fs_, dst_dir_ino, dst_name);
    if (existing) {
        Inode ex_inode;
        fs_.readInode(*existing, ex_inode);
        if (ex_inode.type == INODE_TYPE_DIR) {
            std::cout << "cp: " << dst_name << ": Is a directory\n";
            return;
        }
        fs_.truncateFile(*existing, 0);
        dst_ino = *existing;
    } else {
        dst_ino = fs_.allocInode();
        Inode new_inode{};
        new_inode.type       = INODE_TYPE_FILE;
        new_inode.size       = 0;
        new_inode.link_count = 1;
        new_inode.block_count = 0;
        new_inode.indirect   = 0;
        new_inode.mode       = 0;
        fs_.writeInode(dst_ino, new_inode);
        if (!addDirent(fs_, dst_dir_ino, dst_name, dst_ino)) {
            fs_.freeInode(dst_ino);
            std::cout << "cp: " << dst_name << ": No space left in directory\n";
            return;
        }
    }

    // Copy contents block-by-block
    static constexpr uint32_t CHUNK = 4096;
    uint8_t buf[CHUNK];
    uint32_t off = 0;
    while (off < src_inode.size) {
        uint32_t n = fs_.readFile(*src_ino, off, buf, CHUNK);
        if (n == 0) break;
        fs_.writeFile(dst_ino, off, buf, n);
        off += n;
    }
}

// ── mv <src> <dst> ─────────────────────────────────────────────────────────
// Rename/move a file or empty directory.
// If dst is an existing directory, moves src into it.
// Add new entry first, then remove old (crash-safer ordering).
void Shell::cmd_mv(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        std::cout << "usage: mv <src> <dst>\n";
        return;
    }
    const std::string& src_name = args[0];
    std::string        dst_name = args[1];
    uint32_t cur = cwdInode();

    // Resolve src
    auto src_ino = lookupDirent(fs_, cur, src_name);
    if (!src_ino) {
        std::cout << "mv: " << src_name << ": No such file or directory\n";
        return;
    }
    Inode src_inode;
    fs_.readInode(*src_ino, src_inode);

    // Determine destination directory and final name
    uint32_t dst_dir_ino = cur;
    auto dst_check = lookupDirent(fs_, cur, dst_name);
    if (dst_check) {
        Inode dst_check_inode;
        fs_.readInode(*dst_check, dst_check_inode);
        if (dst_check_inode.type == INODE_TYPE_DIR) {
            // Guard: reject moving a directory into its own subtree
            if (src_inode.type == INODE_TYPE_DIR && *dst_check == *src_ino) {
                std::cout << "mv: cannot move directory into itself\n";
                return;
            }
            dst_dir_ino = *dst_check;
            dst_name    = src_name;
        }
    }

    if (dst_name.empty() || dst_name.find('/') != std::string::npos ||
        dst_name.size() > MAX_FILENAME_LEN) {
        std::cout << "mv: invalid destination name\n";
        return;
    }

    // If overwriting an existing file at dst, remove it first
    auto existing_dst = lookupDirent(fs_, dst_dir_ino, dst_name);
    if (existing_dst && *existing_dst != *src_ino) {
        Inode ex_inode;
        fs_.readInode(*existing_dst, ex_inode);
        if (ex_inode.type == INODE_TYPE_DIR) {
            std::cout << "mv: " << dst_name << ": Is a directory\n";
            return;
        }
        // Remove and free the existing destination file
        removeDirent(fs_, dst_dir_ino, dst_name);
        ex_inode.link_count--;
        fs_.writeInode(*existing_dst, ex_inode);
        fs_.unlinkIfUnreferenced(*existing_dst);
    }

    // Add new entry first (crash-safer), then remove old
    if (dst_dir_ino != cur || dst_name != src_name) {
        if (!addDirent(fs_, dst_dir_ino, dst_name, *src_ino)) {
            std::cout << "mv: " << dst_name << ": No space left in directory\n";
            return;
        }
        removeDirent(fs_, cur, src_name);
    }

    // If the entry moved from cwd, invalidate the cwd stack if it pointed here
    if (src_inode.type == INODE_TYPE_DIR) {
        for (auto& [ino, n] : cwd_stack_) {
            if (ino == *src_ino) {
                n = dst_name; // update the name in stack
            }
        }
    }
}

// ── df ─────────────────────────────────────────────────────────────────────
void Shell::cmd_df() {
    const Superblock& sb = fs_.superblock();
    uint32_t total_data  = sb.total_blocks - sb.data_block_start;
    uint32_t used_blocks = total_data - sb.free_blocks;
    uint32_t used_inodes = sb.total_inodes - sb.free_inodes;

    std::cout << "Filesystem blocks : "
              << used_blocks << " used / "
              << sb.free_blocks << " free / "
              << total_data << " total\n";
    std::cout << "Filesystem inodes : "
              << used_inodes << " used / "
              << sb.free_inodes << " free / "
              << sb.total_inodes << " total\n";
    std::cout << "Block size        : " << sb.block_size << " bytes\n";
    std::cout << "Image size        : "
              << (static_cast<uint64_t>(sb.total_blocks) * sb.block_size) / (1024 * 1024)
              << " MiB\n";
}

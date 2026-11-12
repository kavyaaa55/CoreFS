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
    : fs_(image_path), cwd_(ROOT_INODE_NUM) {}

// ── cwdPath: walk ".." on-disk up to root ─────────────────────────────────
std::string Shell::cwdPath() {
    return fs_.inodePath(cwd_);
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
        if (tokens[0] == "exit" || tokens[0] == "quit") break;
        try { dispatch(tokens); }
        catch (const std::exception& e) { std::cout << "error: " << e.what() << "\n"; }
    }
}

// ── Dispatch ───────────────────────────────────────────────────────────────
void Shell::dispatch(const std::vector<std::string>& tokens) {
    const std::string& cmd = tokens[0];
    std::vector<std::string> args(tokens.begin() + 1, tokens.end());

    if (cmd == "echo") {
        bool found = false; bool append = false; size_t rpos = 0;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == ">>" || args[i] == ">") {
                append = (args[i] == ">>"); rpos = i; found = true; break;
            }
        }
        if (found) {
            if (rpos + 1 >= args.size()) { std::cout << "echo: missing filename\n"; return; }
            std::string text;
            for (size_t i = 0; i < rpos; ++i) { if (i) text += ' '; text += args[i]; }
            cmd_echo({args[rpos + 1], text}, append);
        } else {
            for (size_t i = 0; i < args.size(); ++i) { if (i) std::cout << ' '; std::cout << args[i]; }
            std::cout << '\n';
        }
        return;
    }

    if      (cmd == "pwd")      cmd_pwd();
    else if (cmd == "ls")       cmd_ls(args);
    else if (cmd == "cd")       cmd_cd(args);
    else if (cmd == "mkdir")    cmd_mkdir(args);
    else if (cmd == "touch")    cmd_touch(args);
    else if (cmd == "stat")     cmd_stat(args);
    else if (cmd == "cat")      cmd_cat(args);
    else if (cmd == "write")    cmd_write(args);
    else if (cmd == "rm")       cmd_rm(args);
    else if (cmd == "rmdir")    cmd_rmdir(args);
    else if (cmd == "cp")       cmd_cp(args);
    else if (cmd == "mv")       cmd_mv(args);
    else if (cmd == "df")       cmd_df();
    else if (cmd == "ln")       cmd_ln(args);
    else if (cmd == "readlink") cmd_readlink(args);
    else if (cmd == "chmod")    cmd_chmod(args);
    else std::cout << cmd << ": command not found\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// COMMANDS
// ═══════════════════════════════════════════════════════════════════════════

void Shell::cmd_pwd() { std::cout << cwdPath() << "\n"; }

void Shell::cmd_ls(const std::vector<std::string>& args) {
    bool lfmt = false;
    for (const auto& a : args) {
        if (a == "-l") lfmt = true;
        else { std::cout << "usage: ls [-l]\n"; return; }
    }
    auto dirents = listDirentsUser(fs_, cwd_);
    for (const auto& d : dirents) {
        if (!lfmt) { std::cout << d.name << "\n"; continue; }
        Inode i; fs_.readInode(d.inode_num, i);
        char tc = (i.type == INODE_TYPE_DIR) ? 'd'
                : (i.type == INODE_TYPE_SYMLINK) ? 'l' : 'f';
        // mode string (simplified rwx)
        char ms[10] = "---------";
        if (i.mode & 0400) { ms[0]='r'; } if (i.mode & 0200) { ms[1]='w'; } if (i.mode & 0100) { ms[2]='x'; }
        if (i.mode & 0040) { ms[3]='r'; } if (i.mode & 0020) { ms[4]='w'; } if (i.mode & 0010) { ms[5]='x'; }
        if (i.mode & 0004) { ms[6]='r'; } if (i.mode & 0002) { ms[7]='w'; } if (i.mode & 0001) { ms[8]='x'; }
        std::cout << tc << ms << "  " << i.link_count << "  " << i.size << "\t" << d.name;
        if (i.type == INODE_TYPE_SYMLINK) {
            try { std::cout << " -> " << fs_.readSymlink(d.inode_num); } catch (...) {}
        }
        std::cout << "\n";
    }
}

void Shell::cmd_cd(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cout << "usage: cd <path>\n"; return; }
    try {
        uint32_t target = fs_.resolve(args[0], cwd_, true);
        Inode i; fs_.readInode(target, i);
        if (i.type != INODE_TYPE_DIR) { std::cout << "cd: " << args[0] << ": Not a directory\n"; return; }
        cwd_ = target;
    } catch (const std::exception& e) {
        std::cout << "cd: " << args[0] << ": " << e.what() << "\n";
    }
}

void Shell::cmd_mkdir(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cout << "usage: mkdir <name>\n"; return; }
    try { fs_.makeDir(cwd_, args[0], 0755); }
    catch (const std::exception& e) { std::cout << "mkdir: " << e.what() << "\n"; }
}

void Shell::cmd_touch(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cout << "usage: touch <name>\n"; return; }
    try { fs_.createFile(cwd_, args[0], 0644); }
    catch (const std::exception& e) { std::cout << "touch: " << e.what() << "\n"; }
}

void Shell::cmd_stat(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cout << "usage: stat <name>\n"; return; }
    uint32_t ino;
    try { ino = fs_.resolve(args[0], cwd_, false); }  // lstat: don't follow final symlink
    catch (const std::exception& e) { std::cout << "stat: " << args[0] << ": " << e.what() << "\n"; return; }
    Inode i; fs_.readInode(ino, i);
    const char* ts = i.type == INODE_TYPE_DIR     ? "directory"
                   : i.type == INODE_TYPE_SYMLINK  ? "symlink"
                   : i.type == INODE_TYPE_FILE     ? "file" : "unknown";
    std::cout << "inode: "   << ino
              << "  type: "  << ts
              << "  size: "  << i.size
              << "  blocks: "<< i.block_count
              << "  links: " << i.link_count
              << "  mode: "  << std::oct << i.mode << std::dec
              << "  atime: " << i.atime
              << "  mtime: " << i.mtime
              << "  ctime: " << i.ctime
              << "\n";
}

void Shell::cmd_cat(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cout << "usage: cat <name>\n"; return; }
    uint32_t ino;
    try { ino = fs_.resolve(args[0], cwd_, true); }
    catch (const std::exception& e) { std::cout << "cat: " << args[0] << ": " << e.what() << "\n"; return; }
    Inode i; fs_.readInode(ino, i);
    if (i.type == INODE_TYPE_DIR) { std::cout << "cat: " << args[0] << ": Is a directory\n"; return; }
    if (!i.size) return;
    uint8_t buf[4096]; uint32_t off = 0;
    while (off < i.size) {
        uint32_t n = fs_.readFile(ino, off, buf, 4096);
        if (!n) break;
        std::cout.write(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(n));
        off += n;
    }
    if (i.size) { uint8_t last=0; fs_.readFile(ino, i.size-1, &last, 1); if (last != '\n') std::cout << '\n'; }
    fs_.updateAtime(ino);
}

void Shell::cmd_write(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cout << "usage: write <name> <text...>\n"; return; }
    uint32_t ino;
    try { ino = fs_.resolve(args[0], cwd_, true); }
    catch (const std::exception& e) { std::cout << "write: " << args[0] << ": " << e.what() << "\n"; return; }
    Inode i; fs_.readInode(ino, i);
    if (i.type == INODE_TYPE_DIR) { std::cout << "write: " << args[0] << ": Is a directory\n"; return; }
    std::string text;
    for (size_t j = 1; j < args.size(); ++j) { if (j>1) text += ' '; text += args[j]; }
    text += '\n';
    fs_.truncateFile(ino, 0);
    fs_.writeFile(ino, 0, text.c_str(), static_cast<uint32_t>(text.size()));
}

void Shell::cmd_echo(const std::vector<std::string>& args, bool append) {
    if (args.empty()) return;
    const std::string& name = args[0];
    std::string text = (args.size() >= 2) ? args[1] : "";
    text += '\n';
    uint32_t ino;
    auto found = lookupDirent(fs_, cwd_, name);
    if (!found) {
        try { ino = fs_.createFile(cwd_, name, 0644); }
        catch (const std::exception& e) { std::cout << "echo: " << e.what() << "\n"; return; }
    } else {
        ino = *found;
    }
    Inode i; fs_.readInode(ino, i);
    if (i.type == INODE_TYPE_DIR) { std::cout << "echo: " << name << ": Is a directory\n"; return; }
    uint32_t off = append ? i.size : 0u;
    if (!append) fs_.truncateFile(ino, 0);
    fs_.writeFile(ino, off, text.c_str(), static_cast<uint32_t>(text.size()));
}

void Shell::cmd_rm(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cout << "usage: rm <name>\n"; return; }
    try { fs_.unlinkFile(cwd_, args[0]); }
    catch (const std::exception& e) { std::cout << "rm: " << e.what() << "\n"; }
}

void Shell::cmd_rmdir(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cout << "usage: rmdir <name>\n"; return; }
    // Guard: refuse to rmdir cwd
    auto found = lookupDirent(fs_, cwd_, args[0]);
    if (found && *found == cwd_) {
        std::cout << "rmdir: cannot remove current working directory\n"; return;
    }
    try { fs_.removeDir(cwd_, args[0]); }
    catch (const std::exception& e) { std::cout << "rmdir: " << e.what() << "\n"; }
}

void Shell::cmd_cp(const std::vector<std::string>& args) {
    if (args.size() != 2) { std::cout << "usage: cp <src> <dst>\n"; return; }
    const std::string& src_name = args[0];
    std::string dst_name = args[1];

    uint32_t src_ino;
    try { src_ino = fs_.resolve(src_name, cwd_, true); }
    catch (const std::exception& e) { std::cout << "cp: " << src_name << ": " << e.what() << "\n"; return; }
    Inode si; fs_.readInode(src_ino, si);
    if (si.type == INODE_TYPE_DIR) { std::cout << "cp: " << src_name << ": Is a directory\n"; return; }

    // If dst is an existing dir, copy into it
    uint32_t dst_dir = cwd_;
    auto dst_check = lookupDirent(fs_, cwd_, dst_name);
    if (dst_check) {
        Inode di; fs_.readInode(*dst_check, di);
        if (di.type == INODE_TYPE_DIR) { dst_dir = *dst_check; dst_name = src_name; }
    }

    // Get or create destination inode
    uint32_t dst_ino;
    auto existing = lookupDirent(fs_, dst_dir, dst_name);
    if (existing) {
        Inode ei; fs_.readInode(*existing, ei);
        if (ei.type == INODE_TYPE_DIR) { std::cout << "cp: " << dst_name << ": Is a directory\n"; return; }
        fs_.truncateFile(*existing, 0); dst_ino = *existing;
    } else {
        try { dst_ino = fs_.createFile(dst_dir, dst_name, si.mode); }
        catch (const std::exception& e) { std::cout << "cp: " << e.what() << "\n"; return; }
    }

    uint8_t buf[4096]; uint32_t off = 0;
    while (off < si.size) {
        uint32_t n = fs_.readFile(src_ino, off, buf, 4096);
        if (!n) break;
        fs_.writeFile(dst_ino, off, buf, n);
        off += n;
    }
}

void Shell::cmd_mv(const std::vector<std::string>& args) {
    if (args.size() != 2) { std::cout << "usage: mv <src> <dst>\n"; return; }
    std::string src_name = args[0], dst_name = args[1];
    uint32_t dst_dir = cwd_;
    // If dst is an existing directory, move into it
    auto dst_check = lookupDirent(fs_, cwd_, dst_name);
    if (dst_check) {
        Inode di; fs_.readInode(*dst_check, di);
        if (di.type == INODE_TYPE_DIR) { dst_dir = *dst_check; dst_name = src_name; }
    }
    try { fs_.renameEntry(cwd_, src_name, dst_dir, dst_name); }
    catch (const std::exception& e) { std::cout << "mv: " << e.what() << "\n"; }
    // If we moved cwd itself, cwd becomes invalid — reset to root
    // (This is a known limitation, same as before.)
}

void Shell::cmd_df() {
    const Superblock& sb = fs_.superblock();
    uint32_t total_data = sb.total_blocks - sb.data_block_start;
    std::cout << "Filesystem blocks : "
              << (total_data - sb.free_blocks) << " used / "
              << sb.free_blocks << " free / " << total_data << " total\n"
              << "Filesystem inodes : "
              << (sb.total_inodes - sb.free_inodes) << " used / "
              << sb.free_inodes << " free / " << sb.total_inodes << " total\n"
              << "Block size        : " << sb.block_size << " bytes\n"
              << "Image size        : "
              << (static_cast<uint64_t>(sb.total_blocks) * sb.block_size) / (1024*1024)
              << " MiB\n";
}

// ── ln [-s] <src> <dst> ────────────────────────────────────────────────────
void Shell::cmd_ln(const std::vector<std::string>& args) {
    bool symlink = (!args.empty() && args[0] == "-s");
    std::vector<std::string> a = symlink
        ? std::vector<std::string>(args.begin() + 1, args.end()) : args;
    if (a.size() != 2) {
        std::cout << "usage: ln [-s] <src|target> <dst>\n"; return;
    }
    if (symlink) {
        // ln -s <target> <linkname>
        try { fs_.makeSymlink(a[0], cwd_, a[1]); }
        catch (const std::exception& e) { std::cout << "ln -s: " << e.what() << "\n"; }
    } else {
        // ln <src> <dst>  — hard link
        uint32_t src_ino;
        try { src_ino = fs_.resolve(a[0], cwd_, true); }
        catch (const std::exception& e) { std::cout << "ln: " << a[0] << ": " << e.what() << "\n"; return; }
        try { fs_.hardLink(src_ino, cwd_, a[1]); }
        catch (const std::exception& e) { std::cout << "ln: " << e.what() << "\n"; }
    }
}

// ── readlink <name> ───────────────────────────────────────────────────────
void Shell::cmd_readlink(const std::vector<std::string>& args) {
    if (args.size() != 1) { std::cout << "usage: readlink <name>\n"; return; }
    uint32_t ino;
    try { ino = fs_.resolve(args[0], cwd_, false); } // lstat: don't follow
    catch (const std::exception& e) { std::cout << "readlink: " << args[0] << ": " << e.what() << "\n"; return; }
    Inode i; fs_.readInode(ino, i);
    if (i.type != INODE_TYPE_SYMLINK) { std::cout << "readlink: " << args[0] << ": not a symlink\n"; return; }
    std::cout << fs_.readSymlink(ino) << "\n";
}

// ── chmod <octal> <name> ───────────────────────────────────────────────────
void Shell::cmd_chmod(const std::vector<std::string>& args) {
    if (args.size() != 2) { std::cout << "usage: chmod <octal> <name>\n"; return; }
    uint32_t mode_val;
    try { mode_val = std::stoul(args[0], nullptr, 8); }
    catch (...) { std::cout << "chmod: invalid mode '" << args[0] << "'\n"; return; }
    uint32_t ino;
    try { ino = fs_.resolve(args[1], cwd_, false); }
    catch (const std::exception& e) { std::cout << "chmod: " << args[1] << ": " << e.what() << "\n"; return; }
    Inode i; fs_.readInode(ino, i);
    i.mode = static_cast<uint16_t>(mode_val & 0777);
    FSCore::touchC(i);
    fs_.writeInode(ino, i);
}

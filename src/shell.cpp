#include "shell.h"
#include "directory.h"
#include "constants.h"

#include <iostream>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>

// Permission bits checked by the shell (owner bits; there is a single user).
static constexpr uint16_t P_R = 0400, P_W = 0200, P_X = 0100;

[[noreturn]] static void fail(const std::string& msg) { throw std::runtime_error(msg); }

// ── Constructor ────────────────────────────────────────────────────────────
Shell::Shell(const std::string& image_path)
    : fs_(image_path), cwd_(ROOT_INODE_NUM) {
    fs_.setEnforcePermissions(true);
}

// ── cwdPath: walk ".." on-disk up to root ─────────────────────────────────
std::string Shell::cwdPath() {
    return fs_.inodePath(cwd_);
}

// ── Tokeniser ──────────────────────────────────────────────────────────────
// Splits on whitespace, honouring quotes:
//   '...'  literal text
//   "..."  text where \" and \\ are escapes
//   \x     outside quotes: literal x
// A token is a redirect only if it is exactly > or >> with nothing quoted, so
// `echo ">" > f` writes a ">" into f.
static void tokenize(const std::string& line, std::vector<std::string>& toks,
                     std::vector<bool>& is_redirect) {
    std::string cur;
    bool in_tok = false, quoted = false;
    auto flush = [&]() {
        if (!in_tok) return;
        toks.push_back(cur);
        is_redirect.push_back(!quoted && (cur == ">" || cur == ">>"));
        cur.clear(); in_tok = false; quoted = false;
    };
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { flush(); continue; }
        in_tok = true;
        if (c == '\'') {
            quoted = true;
            size_t j = line.find('\'', i + 1);
            if (j == std::string::npos) fail("unterminated ' quote");
            cur += line.substr(i + 1, j - i - 1);
            i = j;
        } else if (c == '"') {
            quoted = true;
            size_t j = i + 1;
            for (; j < line.size() && line[j] != '"'; ++j) {
                if (line[j] == '\\' && j + 1 < line.size() &&
                    (line[j + 1] == '"' || line[j + 1] == '\\')) ++j;
                cur += line[j];
            }
            if (j >= line.size()) fail("unterminated \" quote");
            i = j;
        } else if (c == '\\' && i + 1 < line.size()) {
            quoted = true;
            cur += line[++i];
        } else {
            cur += c;
        }
    }
    flush();
}

// ── REPL ───────────────────────────────────────────────────────────────────
void Shell::run() {
    std::string line;
    bool first = true;
    while (true) {
        std::cout << cwdPath() << " > " << std::flush;
        if (!std::getline(std::cin, line)) break;
        // Drop a UTF-8 byte-order mark (PowerShell adds one to piped input)
        if (first && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        first = false;
        if (!execLine(line)) break;
    }
}

bool Shell::execLine(const std::string& line) {
    std::vector<std::string> toks; std::vector<bool> redir;
    try { tokenize(line, toks, redir); }
    catch (const std::exception& e) { std::cout << "error: " << e.what() << "\n"; return true; }
    if (toks.empty()) return true;
    if (toks[0] == "exit" || toks[0] == "quit") return false;
    try { dispatch(toks, redir); }
    catch (const std::exception& e) { std::cout << toks[0] << ": " << e.what() << "\n"; }
    return true;
}

// ── Dispatch ───────────────────────────────────────────────────────────────
void Shell::dispatch(const std::vector<std::string>& toks, const std::vector<bool>& redir) {
    const std::string& cmd = toks[0];
    std::vector<std::string> args(toks.begin() + 1, toks.end());

    if (cmd == "echo") {
        size_t r = 1;
        while (r < toks.size() && !redir[r]) ++r;
        std::string text;
        for (size_t i = 1; i < r; ++i) { if (i > 1) text += ' '; text += toks[i]; }
        if (r == toks.size()) { std::cout << text << '\n'; return; }
        if (r + 2 != toks.size()) fail("usage: echo <text...> > <path>   (or >> to append)");
        cmd_echo(toks[r + 1], text, toks[r] == ">>");
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
    else if (cmd == "help")     cmd_help();
    else std::cout << cmd << ": command not found (try 'help')\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// PATH HELPERS
// ═══════════════════════════════════════════════════════════════════════════

static std::string stripTrailingSlashes(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}
static std::string baseName(const std::string& path) {
    std::string s = stripTrailingSlashes(path);
    auto pos = s.rfind('/');
    return pos == std::string::npos ? s : s.substr(pos + 1);
}
static bool endsWithSlash(const std::string& s) { return s.size() > 1 && s.back() == '/'; }

std::pair<uint32_t, std::string> Shell::splitPath(const std::string& path) {
    std::string s = stripTrailingSlashes(path);
    if (s.empty() || s == "/") fail("'" + path + "': invalid path");
    auto pos = s.rfind('/');
    uint32_t dir = cwd_;
    std::string name = s;
    if (pos != std::string::npos) {
        std::string parent = (pos == 0) ? "/" : s.substr(0, pos);
        name = s.substr(pos + 1);
        dir = fs_.resolve(parent, cwd_, true);
        Inode pi; fs_.readInode(dir, pi);
        if (pi.type != INODE_TYPE_DIR) fail(parent + ": not a directory");
    }
    fs_.checkPerm(dir, P_X); // looking up `name` in dir needs search permission
    return {dir, name};
}

uint32_t Shell::tryResolve(const std::string& path, bool follow_last, bool& ok) {
    try { uint32_t ino = fs_.resolve(path, cwd_, follow_last); ok = true; return ino; }
    catch (const std::exception& e) {
        // Permission errors must surface, not look like "missing"
        if (std::string(e.what()) == "Permission denied") throw;
        ok = false; return 0;
    }
}

std::pair<uint32_t, std::string> Shell::destination(const std::string& dst,
                                                    const std::string& src_name) {
    bool ok; uint32_t ino = tryResolve(dst, true, ok);
    if (ok) {
        Inode i; fs_.readInode(ino, i);
        if (i.type == INODE_TYPE_DIR) { fs_.checkPerm(ino, P_X); return {ino, src_name}; }
    }
    if (endsWithSlash(dst)) fail(dst + ": not a directory");
    return splitPath(dst);
}

// ═══════════════════════════════════════════════════════════════════════════
// COMMANDS
// ═══════════════════════════════════════════════════════════════════════════

void Shell::cmd_pwd() { std::cout << cwdPath() << "\n"; }

void Shell::printLong(const std::string& name, uint32_t ino) {
    Inode i; fs_.readInode(ino, i);
    char tc = (i.type == INODE_TYPE_DIR) ? 'd'
            : (i.type == INODE_TYPE_SYMLINK) ? 'l' : '-';
    char ms[10] = "---------";
    const char* rwx = "rwxrwxrwx";
    for (int b = 0; b < 9; ++b)
        if (i.mode & (0400 >> b)) ms[b] = rwx[b];
    std::cout << tc << ms << "  " << i.link_count << "  " << i.size << "\t" << name;
    if (i.type == INODE_TYPE_SYMLINK) {
        try { std::cout << " -> " << fs_.readSymlink(ino); } catch (...) {}
    }
    std::cout << "\n";
}

// ls [-l] [path...]
void Shell::cmd_ls(const std::vector<std::string>& args) {
    bool lfmt = false;
    std::vector<std::string> paths;
    for (const auto& a : args) {
        if (a == "-l") lfmt = true;
        else if (!a.empty() && a[0] == '-') fail("usage: ls [-l] [path...]");
        else paths.push_back(a);
    }
    if (paths.empty()) paths.push_back(".");
    for (size_t k = 0; k < paths.size(); ++k) {
        const std::string& p = paths[k];
        uint32_t ino;
        try { ino = fs_.resolve(p, cwd_, true); }
        catch (const std::exception& e) { std::cout << "ls: " << p << ": " << e.what() << "\n"; continue; }
        Inode i; fs_.readInode(ino, i);
        if (i.type != INODE_TYPE_DIR) {           // a file: show just that entry
            if (lfmt) printLong(p, ino); else std::cout << p << "\n";
            continue;
        }
        try { fs_.checkPerm(ino, P_R); }
        catch (const std::exception& e) { std::cout << "ls: " << p << ": " << e.what() << "\n"; continue; }
        if (paths.size() > 1) std::cout << (k ? "\n" : "") << p << ":\n";
        for (const auto& d : listDirentsUser(fs_, ino)) {
            if (lfmt) printLong(d.name, d.inode_num); else std::cout << d.name << "\n";
        }
    }
}

// cd [path]   (no path = /)
void Shell::cmd_cd(const std::vector<std::string>& args) {
    if (args.size() > 1) fail("usage: cd [path]");
    std::string p = args.empty() ? "/" : args[0];
    uint32_t target = fs_.resolve(p, cwd_, true);
    Inode i; fs_.readInode(target, i);
    if (i.type != INODE_TYPE_DIR) fail(p + ": Not a directory");
    fs_.checkPerm(target, P_X);
    cwd_ = target;
}

// mkdir [-p] <path...>
void Shell::cmd_mkdir(const std::vector<std::string>& args) {
    bool parents = false;
    std::vector<std::string> paths;
    for (const auto& a : args) { if (a == "-p") parents = true; else paths.push_back(a); }
    if (paths.empty()) fail("usage: mkdir [-p] <path...>");
    for (const auto& p : paths) {
        try {
            if (!parents) {
                auto [dir, name] = splitPath(p);
                fs_.checkPerm(dir, P_W);
                fs_.makeDir(dir, name, 0755);
                continue;
            }
            // -p: create each missing component; existing dirs are fine
            std::string s = stripTrailingSlashes(p), prefix = (s[0] == '/') ? "/" : "";
            size_t i = 0;
            while (i < s.size()) {
                size_t j = s.find('/', i);
                if (j == std::string::npos) j = s.size();
                std::string comp = s.substr(i, j - i);
                i = j + 1;
                if (comp.empty()) continue;
                prefix += (prefix.empty() || prefix.back() == '/') ? comp : "/" + comp;
                bool ok; uint32_t ino = tryResolve(prefix, true, ok);
                if (ok) {
                    Inode in; fs_.readInode(ino, in);
                    if (in.type != INODE_TYPE_DIR) fail(prefix + ": exists and is not a directory");
                    continue;
                }
                auto [dir, name] = splitPath(prefix);
                fs_.checkPerm(dir, P_W);
                fs_.makeDir(dir, name, 0755);
            }
        } catch (const std::exception& e) { std::cout << "mkdir: " << p << ": " << e.what() << "\n"; }
    }
}

// touch <path...>   (creates, or updates the times of an existing entry)
void Shell::cmd_touch(const std::vector<std::string>& args) {
    if (args.empty()) fail("usage: touch <path...>");
    for (const auto& p : args) {
        try {
            bool ok; uint32_t ino = tryResolve(p, true, ok);
            if (ok) {
                Inode i; fs_.readInode(ino, i);
                i.atime = i.mtime = i.ctime = FSCore::now();
                fs_.writeInode(ino, i);
                continue;
            }
            auto [dir, name] = splitPath(p);
            fs_.checkPerm(dir, P_W);
            fs_.createFile(dir, name, 0644);
        } catch (const std::exception& e) { std::cout << "touch: " << p << ": " << e.what() << "\n"; }
    }
}

void Shell::cmd_stat(const std::vector<std::string>& args) {
    if (args.size() != 1) fail("usage: stat <path>");
    uint32_t ino = fs_.resolve(args[0], cwd_, false);  // lstat: don't follow final symlink
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
    if (args.empty()) fail("usage: cat <path...>");
    for (const auto& p : args) {
        try {
            uint32_t ino = fs_.resolve(p, cwd_, true);
            Inode i; fs_.readInode(ino, i);
            if (i.type == INODE_TYPE_DIR) fail("Is a directory");
            fs_.checkPerm(ino, P_R);
            uint8_t buf[4096]; uint32_t off = 0; uint8_t last = '\n';
            while (off < i.size) {
                uint32_t n = fs_.readFile(ino, off, buf, sizeof buf);
                if (!n) break;
                std::cout.write(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(n));
                last = buf[n - 1];
                off += n;
            }
            if (last != '\n') std::cout << '\n';
            fs_.updateAtime(ino);
        } catch (const std::exception& e) { std::cout << "cat: " << p << ": " << e.what() << "\n"; }
    }
}

// write <path> <text...>   (overwrites an existing file)
void Shell::cmd_write(const std::vector<std::string>& args) {
    if (args.size() < 2) fail("usage: write <path> <text...>");
    uint32_t ino = fs_.resolve(args[0], cwd_, true);
    Inode i; fs_.readInode(ino, i);
    if (i.type == INODE_TYPE_DIR) fail(args[0] + ": Is a directory");
    fs_.checkPerm(ino, P_W);
    std::string text;
    for (size_t j = 1; j < args.size(); ++j) { if (j > 1) text += ' '; text += args[j]; }
    text += '\n';
    fs_.truncateFile(ino, 0);
    fs_.writeFile(ino, 0, text.c_str(), static_cast<uint32_t>(text.size()));
}

// echo <text> > <path>  /  echo <text> >> <path>   (creates the file if missing)
void Shell::cmd_echo(const std::string& path, const std::string& text_in, bool append) {
    std::string text = text_in + '\n';
    bool ok; uint32_t ino = tryResolve(path, true, ok);   // follows symlinks
    if (!ok) {
        if (endsWithSlash(path)) fail(path + ": not a directory");
        auto [dir, name] = splitPath(path);
        fs_.checkPerm(dir, P_W);
        ino = fs_.createFile(dir, name, 0644);
    }
    Inode i; fs_.readInode(ino, i);
    if (i.type == INODE_TYPE_DIR) fail(path + ": Is a directory");
    if (ok) fs_.checkPerm(ino, P_W);
    uint32_t off = append ? i.size : 0u;
    if (!append) fs_.truncateFile(ino, 0);
    fs_.writeFile(ino, off, text.c_str(), static_cast<uint32_t>(text.size()));
}

// rm <path...>   (files and symlinks; a symlink is removed, not its target)
void Shell::cmd_rm(const std::vector<std::string>& args) {
    if (args.empty()) fail("usage: rm <path...>");
    for (const auto& p : args) {
        try {
            if (endsWithSlash(p)) fail("is a directory");
            auto [dir, name] = splitPath(p);
            fs_.checkPerm(dir, P_W);
            fs_.unlinkFile(dir, name);
        } catch (const std::exception& e) { std::cout << "rm: " << p << ": " << e.what() << "\n"; }
    }
}

// rmdir <path...>   (empty directories only)
void Shell::cmd_rmdir(const std::vector<std::string>& args) {
    if (args.empty()) fail("usage: rmdir <path...>");
    for (const auto& p : args) {
        try {
            auto [dir, name] = splitPath(p);
            if (name == "." || name == "..") fail("invalid name");
            auto found = lookupDirent(fs_, dir, name);
            if (!found) fail("no such file or directory");
            if (*found == cwd_) fail("cannot remove the current directory");
            fs_.checkPerm(dir, P_W);
            fs_.removeDir(dir, name);
        } catch (const std::exception& e) { std::cout << "rmdir: " << p << ": " << e.what() << "\n"; }
    }
}

// cp <src> <dst>   (dst may be an existing directory: copies into it)
void Shell::cmd_cp(const std::vector<std::string>& args) {
    if (args.size() != 2) fail("usage: cp <src> <dst>");
    const std::string& src = args[0];
    const std::string& dst = args[1];

    uint32_t src_ino = fs_.resolve(src, cwd_, true);
    Inode si; fs_.readInode(src_ino, si);
    if (si.type == INODE_TYPE_DIR) fail(src + ": Is a directory");
    fs_.checkPerm(src_ino, P_R);

    auto [dir, name] = destination(dst, baseName(src));

    uint32_t dst_ino;
    auto existing = lookupDirent(fs_, dir, name);
    if (existing) {
        uint32_t target = *existing;
        Inode ei; fs_.readInode(target, ei);
        if (ei.type == INODE_TYPE_SYMLINK) {                // write through the link
            target = fs_.resolve(fs_.inodePath(dir) + "/" + name, cwd_, true);
            fs_.readInode(target, ei);
        }
        if (ei.type == INODE_TYPE_DIR) fail(name + ": Is a directory");
        // Copying a file onto itself would truncate the source first and lose its data
        if (target == src_ino) fail("'" + src + "' and '" + dst + "' are the same file");
        fs_.checkPerm(target, P_W);
        fs_.truncateFile(target, 0);
        dst_ino = target;
    } else {
        fs_.checkPerm(dir, P_W);
        dst_ino = fs_.createFile(dir, name, si.mode);
    }

    uint8_t buf[4096]; uint32_t off = 0;
    while (off < si.size) {
        uint32_t n = fs_.readFile(src_ino, off, buf, sizeof buf);
        if (!n) break;
        fs_.writeFile(dst_ino, off, buf, n);
        off += n;
    }
}

// mv <src> <dst>   (dst may be an existing directory: moves into it)
void Shell::cmd_mv(const std::vector<std::string>& args) {
    if (args.size() != 2) fail("usage: mv <src> <dst>");
    auto [sp, sn] = splitPath(args[0]);
    if (sn == "." || sn == "..") fail(args[0] + ": invalid source");
    auto src_ino = lookupDirent(fs_, sp, sn);
    if (!src_ino) fail(args[0] + ": no such file or directory");
    Inode si; fs_.readInode(*src_ino, si);

    auto [dp, dn] = destination(args[1], sn);
    if (auto victim = lookupDirent(fs_, dp, dn); victim && *victim == cwd_ && *victim != *src_ino)
        fail("cannot replace the current directory");

    fs_.checkPerm(sp, P_W);
    fs_.checkPerm(dp, P_W);
    // Moving a directory to a new parent rewrites its ".." entry
    if (si.type == INODE_TYPE_DIR && sp != dp) fs_.checkPerm(*src_ino, P_W);
    fs_.renameEntry(sp, sn, dp, dn);
    // cwd_ is an inode number, so the prompt follows a moved directory.
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

// ln <src> <dst>  /  ln -s <target> <linkpath>
void Shell::cmd_ln(const std::vector<std::string>& args) {
    bool symlink = (!args.empty() && args[0] == "-s");
    std::vector<std::string> a = symlink
        ? std::vector<std::string>(args.begin() + 1, args.end()) : args;
    if (a.size() != 2) fail("usage: ln [-s] <src|target> <dst>");
    auto [dir, name] = destination(a[1], baseName(a[0]));
    fs_.checkPerm(dir, P_W);
    if (symlink) {
        fs_.makeSymlink(a[0], dir, name);   // target stored as given (may dangle)
    } else {
        uint32_t src_ino = fs_.resolve(a[0], cwd_, true);
        fs_.hardLink(src_ino, dir, name);
    }
}

void Shell::cmd_readlink(const std::vector<std::string>& args) {
    if (args.size() != 1) fail("usage: readlink <path>");
    uint32_t ino = fs_.resolve(args[0], cwd_, false); // lstat: don't follow
    Inode i; fs_.readInode(ino, i);
    if (i.type != INODE_TYPE_SYMLINK) fail(args[0] + ": not a symlink");
    std::cout << fs_.readSymlink(ino) << "\n";
}

// chmod <octal> <path>   (owner can always chmod; follows symlinks like Unix)
void Shell::cmd_chmod(const std::vector<std::string>& args) {
    if (args.size() != 2) fail("usage: chmod <octal> <path>");
    uint32_t mode_val;
    size_t used = 0;
    try { mode_val = std::stoul(args[0], &used, 8); } catch (...) { used = 0; }
    if (used != args[0].size() || mode_val > 07777) fail("invalid mode '" + args[0] + "'");
    uint32_t ino = fs_.resolve(args[1], cwd_, true);
    Inode i; fs_.readInode(ino, i);
    i.mode = static_cast<uint16_t>(mode_val & 0777);
    FSCore::touchC(i);
    fs_.writeInode(ino, i);
}

void Shell::cmd_help() {
    std::cout <<
        "Paths may be absolute (/a/b) or relative (a/b, ../x). Quote text with \"...\" or '...'.\n"
        "  pwd                      ls [-l] [path...]        cd [path]\n"
        "  mkdir [-p] <path...>     touch <path...>          rmdir <path...>\n"
        "  rm <path...>             cp <src> <dst>           mv <src> <dst>\n"
        "  cat <path...>            write <path> <text...>   stat <path>\n"
        "  echo <text> > <path>     echo <text> >> <path>    df\n"
        "  ln <src> <dst>           ln -s <target> <link>    readlink <path>\n"
        "  chmod <octal> <path>     help                     exit | quit\n"
        "Permissions (owner bits) are enforced: r = read/list, w = write/create/delete,\n"
        "x = enter/search a directory.\n";
}

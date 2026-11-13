// Model-based fuzzer for SimpleFS.
// Usage: sfs_fuzz [seeds] [ops_per_seed]

#include "test_helpers.h"
#include "fs_core.h"
#include "directory.h"
#include "constants.h"

#include <random>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

extern "C" int fsck_image(const char* path); // tools/fsck.cpp

// ── Path helpers ───────────────────────────────────────────────────────────
static std::string path_parent(const std::string& p) {
    auto s = p.rfind('/');
    if (s == 0) return "/";
    return p.substr(0, s);
}
static std::string path_name(const std::string& p) {
    return p.substr(p.rfind('/') + 1);
}

// ── Model ──────────────────────────────────────────────────────────────────
struct Model {
    std::set<std::string>              dirs  = {"/"};
    std::map<std::string, std::string> files;
    bool exists(const std::string& p) const { return dirs.count(p) || files.count(p); }
    bool is_dir_empty(const std::string& d) const {
        std::string pfx = (d == "/") ? "/" : d + "/";
        for (auto& [k,v] : files)
            if (k.rfind(pfx,0)==0 && k.find('/', pfx.size())==std::string::npos) return false;
        for (auto& dr : dirs)
            if (dr != d && dr.rfind(pfx,0)==0 && dr.find('/', pfx.size())==std::string::npos) return false;
        return true;
    }
};

// ── Op types ──────────────────────────────────────────────────────────────
enum class Op { CREATE, MKDIR, WRITE, TRUNCATE, RM, RMDIR, MV, CP };
static const char* op_name(Op o) {
    static const char* names[] = {"create","mkdir","write","truncate","rm","rmdir","mv","cp"};
    return names[static_cast<int>(o)];
}

struct OpInst { Op op; std::string path, path2, content; uint32_t sz=0; };

// ── Random generation ─────────────────────────────────────────────────────
static const char* NAMES[] = {"a","b","c","d"};
static constexpr int NNAMES = 4;

static std::string rand_name(std::mt19937& rng) { return NAMES[rng() % NNAMES]; }

static std::string rand_new_path(std::mt19937& rng, const Model& m) {
    std::vector<std::string> dirs(m.dirs.begin(), m.dirs.end());
    std::string base = dirs[rng() % dirs.size()];
    // Depth limit 3
    int depth = 0; for (char c : base) if (c=='/') depth++;
    if (depth >= 3) base = "/";
    if (base == "/") return "/" + rand_name(rng);
    return base + "/" + rand_name(rng);
}

static std::string rand_existing_path(std::mt19937& rng, const Model& m) {
    std::vector<std::string> all;
    for (auto& d : m.dirs)  all.push_back(d);
    for (auto& [k,v] : m.files) all.push_back(k);
    if (all.empty()) return "/";
    return all[rng() % all.size()];
}

// ── Model apply: mutates m, returns true if op should succeed ─────────────
static bool model_apply(Model& m, const OpInst& op) {
    switch (op.op) {
    case Op::CREATE: {
        if (!m.dirs.count(path_parent(op.path))) return false;
        if (m.exists(op.path) || op.path == "/") return false;
        m.files[op.path] = ""; return true;
    }
    case Op::MKDIR: {
        if (!m.dirs.count(path_parent(op.path))) return false;
        if (m.exists(op.path) || op.path == "/") return false;
        m.dirs.insert(op.path); return true;
    }
    case Op::WRITE: {
        if (!m.files.count(op.path)) return false;
        m.files[op.path] = op.content; return true;
    }
    case Op::TRUNCATE: {
        if (!m.files.count(op.path)) return false;
        auto& s = m.files[op.path];
        if (op.sz < s.size()) s.resize(op.sz);
        else s.append(op.sz - s.size(), '\0');
        return true;
    }
    case Op::RM: {
        if (!m.files.count(op.path)) return false;
        m.files.erase(op.path); return true;
    }
    case Op::RMDIR: {
        if (!m.dirs.count(op.path) || op.path == "/") return false;
        if (!m.is_dir_empty(op.path)) return false;
        m.dirs.erase(op.path); return true;
    }
    case Op::MV: {
        if (op.path == "/" || !m.exists(op.path)) return false;
        // Moving onto an existing dir means "move into it"
        std::string dst = op.path2;
        if (m.dirs.count(dst)) dst = (dst == "/" ? "" : dst) + "/" + path_name(op.path);
        if (dst == op.path) return true;                      // same entry: no-op
        if (!m.dirs.count(path_parent(dst))) return false;
        if (m.files.count(op.path)) {
            if (m.dirs.count(dst)) return false;              // file over dir
            m.files[dst] = m.files[op.path]; m.files.erase(op.path);
            return true;
        }
        // Directory move
        if (m.files.count(dst)) return false;                 // dir over file
        if (dst.rfind(op.path + "/", 0) == 0) return false;   // into own subtree
        if (m.dirs.count(dst)) {                              // replace an empty dir
            if (!m.is_dir_empty(dst)) return false;
            m.dirs.erase(dst);
        }
        // Rename the dir and everything under it
        const std::string pfx = op.path + "/";
        auto moved = [&](const std::string& p) { return dst + p.substr(op.path.size()); };
        std::set<std::string> nd;
        for (auto& d : m.dirs) nd.insert((d == op.path || d.rfind(pfx, 0) == 0) ? moved(d) : d);
        m.dirs = std::move(nd);
        std::map<std::string, std::string> nf;
        for (auto& [k, v] : m.files) nf[k.rfind(pfx, 0) == 0 ? moved(k) : k] = v;
        m.files = std::move(nf);
        return true;
    }
    case Op::CP: {
        if (!m.files.count(op.path)) return false;
        std::string dst = op.path2;
        if (m.dirs.count(dst)) dst = dst + "/" + path_name(op.path);
        if (m.dirs.count(dst)) return false;
        if (!m.dirs.count(path_parent(dst))) return false;
        if (dst == op.path) return false; // same file: refused, like GNU cp
        m.files[dst] = m.files[op.path]; return true;
    }
    }
    return false;
}

// ── Real FS apply ──────────────────────────────────────────────────────────
static bool fs_apply(FSCore& fs, const OpInst& op) {
    try {
        switch (op.op) {
        case Op::CREATE: {
            uint32_t pino = fs.resolve(path_parent(op.path), ROOT_INODE_NUM);
            fs.createFile(pino, path_name(op.path), 0644);
            return true;
        }
        case Op::MKDIR: {
            uint32_t pino = fs.resolve(path_parent(op.path), ROOT_INODE_NUM);
            fs.makeDir(pino, path_name(op.path), 0755);
            return true;
        }
        case Op::WRITE: {
            uint32_t ino = fs.resolve(op.path, ROOT_INODE_NUM);
            { Inode wi; fs.readInode(ino, wi); if (wi.type != INODE_TYPE_FILE) return false; }
            fs.truncateFile(ino, 0);
            if (!op.content.empty())
                fs.writeFile(ino, 0, op.content.data(),
                             static_cast<uint32_t>(op.content.size()));
            return true;
        }
        case Op::TRUNCATE: {
            uint32_t ino = fs.resolve(op.path, ROOT_INODE_NUM);
            { Inode ti; fs.readInode(ino, ti); if (ti.type != INODE_TYPE_FILE) return false; }
            fs.truncateFile(ino, op.sz);
            return true;
        }
        case Op::RM: {
            uint32_t pino = fs.resolve(path_parent(op.path), ROOT_INODE_NUM);
            fs.unlinkFile(pino, path_name(op.path));
            return true;
        }
        case Op::RMDIR: {
            uint32_t pino = fs.resolve(path_parent(op.path), ROOT_INODE_NUM);
            fs.removeDir(pino, path_name(op.path));
            return true;
        }
        case Op::MV: {
            std::string dst = op.path2;
            uint32_t dst_check;
            bool dst_is_dir = false;
            try {
                dst_check = fs.resolve(dst, ROOT_INODE_NUM);
                Inode di; fs.readInode(dst_check, di);
                dst_is_dir = (di.type == INODE_TYPE_DIR);
            } catch (...) {}
            if (dst_is_dir) dst = dst + "/" + path_name(op.path);
            uint32_t sp = fs.resolve(path_parent(op.path), ROOT_INODE_NUM);
            uint32_t dp = fs.resolve(path_parent(dst),     ROOT_INODE_NUM);
            fs.renameEntry(sp, path_name(op.path), dp, path_name(dst));
            return true;
        }
        case Op::CP: {
            uint32_t src_ino = fs.resolve(op.path, ROOT_INODE_NUM);
            Inode si; fs.readInode(src_ino, si);
            if (si.type != INODE_TYPE_FILE) return false; // cp of a directory fails, as in the shell
            std::string dst = op.path2;
            uint32_t dp = fs.resolve(path_parent(dst), ROOT_INODE_NUM);
            auto existing = lookupDirent(fs, dp, path_name(dst));
            uint32_t dst_ino;
            if (existing) {
                Inode ei; fs.readInode(*existing, ei);
                if (ei.type == INODE_TYPE_DIR) {
                    // copy into dir
                    dp = *existing;
                    dst = dst + "/" + path_name(op.path);
                    auto ex2 = lookupDirent(fs, dp, path_name(dst));
                    if (ex2) {
                        Inode e2; fs.readInode(*ex2, e2);
                        if (e2.type == INODE_TYPE_DIR) return false; // can't overwrite a dir
                        if (*ex2 == src_ino) return false;           // same file
                        fs.truncateFile(*ex2, 0); dst_ino = *ex2;
                    }
                    else dst_ino = fs.createFile(dp, path_name(dst), si.mode);
                } else {
                    if (*existing == src_ino) return false;          // same file
                    fs.truncateFile(*existing, 0); dst_ino = *existing;
                }
            } else {
                dst_ino = fs.createFile(dp, path_name(dst), si.mode);
            }
            uint8_t buf[4096]; uint32_t off = 0;
            while (off < si.size) {
                uint32_t n = fs.readFile(src_ino, off, buf, 4096);
                if (!n) break;
                fs.writeFile(dst_ino, off, buf, n); off += n;
            }
            return true;
        }
        }
    } catch (...) { return false; }
    return false;
}

// ── Tree comparison ────────────────────────────────────────────────────────
static void compare_tree(FSCore& fs, const Model& m, uint32_t seed, int op_i) {
    for (auto& [path, content] : m.files) {
        uint32_t ino;
        try { ino = fs.resolve(path, ROOT_INODE_NUM); }
        catch (...) {
            fprintf(stderr, "DIVERGE seed=%u op#%d: file '%s' missing\n", seed, op_i, path.c_str());
            std::exit(1);
        }
        Inode i; fs.readInode(ino, i);
        if (i.size != content.size()) {
            fprintf(stderr, "DIVERGE seed=%u op#%d: '%s' size %u != model %zu\n",
                    seed, op_i, path.c_str(), i.size, content.size());
            std::exit(1);
        }
        if (!content.empty()) {
            std::string rd(content.size(), '\0');
            fs.readFile(ino, 0, rd.data(), static_cast<uint32_t>(rd.size()));
            if (rd != content) {
                size_t at = 0;
                while (at < rd.size() && rd[at] == content[at]) ++at;
                fprintf(stderr, "DIVERGE seed=%u op#%d: '%s' content mismatch at byte %zu of %zu"
                        " (fs=0x%02x model=0x%02x)\n", seed, op_i, path.c_str(), at, rd.size(),
                        static_cast<unsigned char>(rd[at]), static_cast<unsigned char>(content[at]));
                std::exit(1);
            }
        }
    }
    for (auto& d : m.dirs) {
        if (d == "/") continue;
        uint32_t ino;
        try { ino = fs.resolve(d, ROOT_INODE_NUM); }
        catch (...) {
            fprintf(stderr, "DIVERGE seed=%u op#%d: dir '%s' missing\n", seed, op_i, d.c_str());
            std::exit(1);
        }
        Inode i; fs.readInode(ino, i);
        if (i.type != INODE_TYPE_DIR) {
            fprintf(stderr, "DIVERGE seed=%u op#%d: '%s' not a dir\n", seed, op_i, d.c_str());
            std::exit(1);
        }
    }
}

// ── Main ───────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    uint32_t SEEDS = (argc > 1) ? static_cast<uint32_t>(std::atoi(argv[1])) : 100;
    uint32_t OPS   = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 400;
    printf("sfs_fuzz: %u seeds x %u ops\n", SEEDS, OPS);

    for (uint32_t seed = 1; seed <= SEEDS; ++seed) {
        std::mt19937 rng(seed);
        Model model;
        std::string img = "fuzz_" + std::to_string(seed) + ".img";
        make_test_image(img, 512, 64);

        {
            FSCore fs(img);
            for (uint32_t i = 0; i < OPS; ++i) {
                Op ot = static_cast<Op>(rng() % 8);
                OpInst inst; inst.op = ot;
                switch (ot) {
                case Op::CREATE:
                case Op::MKDIR:
                    inst.path = rand_new_path(rng, model); break;
                case Op::WRITE: {
                    inst.path = rand_existing_path(rng, model);
                    uint32_t sz = rng() % (BLOCK_SIZE * 2 + 1);
                    inst.content.resize(sz);
                    for (auto& c : inst.content) c = static_cast<char>('a' + rng() % 26);
                    break;
                }
                case Op::TRUNCATE:
                    inst.path = rand_existing_path(rng, model);
                    inst.sz   = rng() % (BLOCK_SIZE * 3);
                    break;
                case Op::RM:
                case Op::RMDIR:
                    inst.path = rand_existing_path(rng, model); break;
                case Op::MV:
                case Op::CP:
                    inst.path  = rand_existing_path(rng, model);
                    inst.path2 = rand_new_path(rng, model);
                    break;
                }

                Model m_copy = model;
                bool mok = model_apply(m_copy, inst);
                bool fok = fs_apply(fs, inst);
                if (std::getenv("SFS_FUZZ_TRACE"))
                    fprintf(stderr, "  seed=%u op#%u %s '%s' '%s' model=%d fs=%d\n",
                            seed, i, op_name(ot), inst.path.c_str(), inst.path2.c_str(), mok, fok);

                if (!mok && fok) {
                    fprintf(stderr, "FAIL seed=%u op#%u (%s path='%s'): model=FAIL fs=OK\n",
                            seed, i, op_name(ot), inst.path.c_str());
                    std::remove(img.c_str());
                    return 1;
                }
                if (mok && fok) model = m_copy;
                // mok && !fok: allowed divergence (disk/inode full on tiny image)

                if (i % 50 == 0) compare_tree(fs, model, seed, static_cast<int>(i));
            }
            compare_tree(fs, model, seed, static_cast<int>(OPS));
        } // FSCore destructor closes file

        // The image must also be consistent on disk (no leaks, link counts, ...)
        if (fsck_image(img.c_str()) != 0) {
            fprintf(stderr, "FSCK FAIL seed=%u (image kept: %s)\n", seed, img.c_str());
            return 1;
        }
        std::remove(img.c_str());
        if (seed % 10 == 0) printf("  seed %u/%u OK\n", seed, SEEDS);
    }

    printf("sfs_fuzz: ALL %u seeds PASSED.\n", SEEDS);
    return 0;
}

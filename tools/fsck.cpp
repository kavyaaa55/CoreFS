// simplefs_fsck <image> [--repair]
// Exit 0 = clean, 1 = errors found.

#include "disk.h"
#include "types.h"
#include "constants.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <functional>

// ── Report ─────────────────────────────────────────────────────────────────
struct Report {
    std::map<std::string, int> errs;
    void err(const char* key, const std::string& detail) {
        errs[key]++;
        fprintf(stderr, "[%s] %s\n", key, detail.c_str());
    }
    bool clean() const { return errs.empty(); }
};

// ── Bitmap helpers ─────────────────────────────────────────────────────────
struct Bitmap {
    std::vector<uint8_t> data;
    explicit Bitmap(size_t n_bytes) : data(n_bytes, 0) {}
    bool get(uint32_t bit) const {
        return (data[bit / 8] >> (bit % 8)) & 1;
    }
    void set(uint32_t bit) { data[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8)); }
    void clear(uint32_t bit) { data[bit / 8] &= static_cast<uint8_t>(~(1u << (bit % 8))); }
};

static Bitmap load_bitmap(Disk& disk, uint32_t block, uint32_t n_bits) {
    Bitmap bm(BLOCK_SIZE);
    disk.readBlock(block, bm.data.data());
    (void)n_bits;
    return bm;
}

// ── Inode reader ───────────────────────────────────────────────────────────
static Inode read_inode(Disk& disk, const Superblock& sb, uint32_t ino) {
    uint32_t byte_off   = ino * INODE_SIZE;
    uint32_t abs_block  = sb.inode_table_block + byte_off / BLOCK_SIZE;
    uint32_t byte_within = byte_off % BLOCK_SIZE;
    uint8_t buf[BLOCK_SIZE];
    disk.readBlock(abs_block, buf);
    Inode i;
    std::memcpy(&i, buf + byte_within, sizeof(Inode));
    return i;
}

// Walk all data blocks of an inode (direct + single + double), call cb for each non-zero pointer.
// Also calls cb for the indirect pointer blocks themselves (they consume disk space).
static void for_each_block(Disk& disk, const Inode& in,
                           const std::function<void(uint32_t)>& cb) {
    // Direct blocks
    for (uint32_t lb = 0; lb < NUM_DIRECT_BLOCKS; ++lb)
        if (in.direct[lb]) cb(in.direct[lb]);

    // Single indirect
    if (in.indirect) {
        cb(in.indirect); // the pointer block itself
        uint8_t buf[BLOCK_SIZE];
        disk.readBlock(in.indirect, buf);
        for (uint32_t s = 0; s < PTRS_PER_BLOCK; ++s) {
            uint32_t p; std::memcpy(&p, buf + s*4, 4);
            if (p) cb(p);
        }
    }

    // Double indirect
    if (in.double_indirect) {
        cb(in.double_indirect);
        uint8_t dbl[BLOCK_SIZE];
        disk.readBlock(in.double_indirect, dbl);
        for (uint32_t l1 = 0; l1 < PTRS_PER_BLOCK; ++l1) {
            uint32_t l1b; std::memcpy(&l1b, dbl + l1*4, 4);
            if (!l1b) continue;
            cb(l1b);
            uint8_t l2buf[BLOCK_SIZE];
            disk.readBlock(l1b, l2buf);
            for (uint32_t l2 = 0; l2 < PTRS_PER_BLOCK; ++l2) {
                uint32_t p; std::memcpy(&p, l2buf + l2*4, 4);
                if (p) cb(p);
            }
        }
    }
}

// Walk all dirents of a directory inode.
static void for_each_dirent(Disk& disk, const Inode& in,
                            const std::function<void(const Dirent&)>& cb) {
    uint8_t buf[BLOCK_SIZE];
    for (uint32_t b = 0; b < in.block_count && b < NUM_DIRECT_BLOCKS; ++b) {
        if (!in.direct[b]) continue;
        disk.readBlock(in.direct[b], buf);
        const auto* d = reinterpret_cast<const Dirent*>(buf);
        for (uint32_t i = 0; i < BLOCK_SIZE / DIRENT_SIZE; ++i)
            if (d[i].inode_num != 0) cb(d[i]);
    }
}

// ── fsck core ──────────────────────────────────────────────────────────────
static int do_fsck(const std::string& img_path, bool repair) {
    Disk disk(img_path);
    Report R;

    // Read superblock
    uint8_t sb_buf[BLOCK_SIZE];
    disk.readBlock(0, sb_buf);
    Superblock sb;
    std::memcpy(&sb, sb_buf, sizeof(Superblock));

    // Check 1: superblock sanity
    if (sb.magic != MAGIC_NUMBER) {
        R.err("SUPERBLOCK", "bad magic number");
        // Can't proceed
        fprintf(stderr, "SUPERBLOCK: 1\n"); return 1;
    }
    if (sb.version != FS_VERSION) {
        R.err("SUPERBLOCK", "wrong version " + std::to_string(sb.version));
    }
    if (sb.block_size != BLOCK_SIZE) {
        R.err("SUPERBLOCK", "block_size mismatch");
    }
    if (sb.data_block_start >= sb.total_blocks) {
        R.err("SUPERBLOCK", "data_block_start out of range");
        fprintf(stderr, "SUPERBLOCK: %d\n", (int)R.errs.size()); return 1;
    }

    // Load bitmaps
    Bitmap ibm = load_bitmap(disk, sb.inode_bitmap_block, sb.total_inodes);
    Bitmap dbm = load_bitmap(disk, sb.data_bitmap_block,  sb.total_blocks);

    // Per-inode reference count (how many dirents point to it)
    std::vector<uint32_t> seen(sb.total_inodes, 0);
    // Per-block reference count
    std::vector<uint32_t> bref(sb.total_blocks, 0);
    // Parent of each inode (from DFS)
    std::vector<uint32_t> parent_of(sb.total_inodes, 0);
    // ".." inode for dirs
    std::vector<uint32_t> dotdot(sb.total_inodes, 0);
    // Reachability
    std::vector<bool>     reach(sb.total_inodes, false);

    auto in_data = [&](uint32_t b) {
        return b >= sb.data_block_start && b < sb.total_blocks;
    };

    // BFS from root
    std::vector<uint32_t> stack = {ROOT_INODE_NUM};
    reach[ROOT_INODE_NUM] = true;
    parent_of[ROOT_INODE_NUM] = ROOT_INODE_NUM;

    while (!stack.empty()) {
        uint32_t ino = stack.back(); stack.pop_back();
        Inode in = read_inode(disk, sb, ino);

        // Check blocks
        for_each_block(disk, in, [&](uint32_t b) {
            if (!in_data(b)) {
                R.err("BLOCK_OUT_OF_RANGE", "inode " + std::to_string(ino) + " blk " + std::to_string(b));
                return;
            }
            bref[b]++;
            if (bref[b] > 1)
                R.err("DOUBLE_REF", "inode " + std::to_string(ino) + " blk " + std::to_string(b));
            if (!dbm.get(b))
                R.err("REF_BUT_FREE", "inode " + std::to_string(ino) + " blk " + std::to_string(b));
        });

        if (in.type != INODE_TYPE_DIR) continue;

        // Walk dirents
        for_each_dirent(disk, in, [&](const Dirent& e) {
            uint32_t eino = e.inode_num;
            std::string ename(e.name, strnlen(e.name, MAX_FILENAME_LEN + 1));

            if (eino >= sb.total_inodes || !ibm.get(eino)) {
                R.err("DIRENT_TO_FREE_INODE",
                      "dir " + std::to_string(ino) + " entry '" + ename + "' -> " + std::to_string(eino));
                return;
            }
            seen[eino]++;

            if (ename == ".") {
                if (eino != ino)
                    R.err("BAD_DOT", "dir " + std::to_string(ino) + " '.' -> " + std::to_string(eino));
                return;
            }
            if (ename == "..") {
                dotdot[ino] = eino; return;
            }

            if (!reach[eino]) {
                reach[eino] = true;
                parent_of[eino] = ino;
                stack.push_back(eino);
            } else {
                Inode ci = read_inode(disk, sb, eino);
                if (ci.type == INODE_TYPE_DIR)
                    R.err("DIR_MULTI_PARENT",
                          "dir " + std::to_string(ino) + " entry '" + ename + "'");
            }
        });
    }

    // Per-inode checks
    for (uint32_t i = 0; i < sb.total_inodes; ++i) {
        if (!ibm.get(i)) continue;
        Inode in = read_inode(disk, sb, i);

        if (!reach[i]) {
            R.err("ORPHAN_INODE", "inode " + std::to_string(i) +
                  " type=" + std::to_string(in.type));
            continue;
        }

        if (seen[i] != in.link_count)
            R.err("LINK_COUNT", "inode " + std::to_string(i) +
                  " stored=" + std::to_string(in.link_count) +
                  " counted=" + std::to_string(seen[i]));

        if (in.type == INODE_TYPE_DIR && dotdot[i] != parent_of[i])
            R.err("BAD_DOTDOT", "dir " + std::to_string(i) +
                  " dotdot=" + std::to_string(dotdot[i]) +
                  " parent=" + std::to_string(parent_of[i]));
    }

    // Block leak check
    for (uint32_t b = sb.data_block_start; b < sb.total_blocks; ++b)
        if (dbm.get(b) && bref[b] == 0)
            R.err("BLOCK_LEAK", "blk " + std::to_string(b));

    // ── Summary ───────────────────────────────────────────────────────────
    if (!R.errs.empty()) {
        for (auto& [k, v] : R.errs)
            fprintf(stderr, "%s: %d occurrence(s)\n", k.c_str(), v);
    }

    // ── Repair (limited) ──────────────────────────────────────────────────
    if (repair && !R.errs.empty()) {
        fprintf(stderr, "REPAIR: fixing bitmap leaks and link counts\n");
        // Rebuild data bitmap from bref
        uint8_t new_dbm[BLOCK_SIZE] = {};
        for (uint32_t b = 0; b < sb.total_blocks; ++b)
            if (bref[b] > 0 || b < sb.data_block_start)
                new_dbm[b / 8] |= static_cast<uint8_t>(1u << (b % 8));
        // Also mark all metadata blocks
        for (uint32_t b = 0; b < sb.data_block_start; ++b)
            new_dbm[b / 8] |= static_cast<uint8_t>(1u << (b % 8));
        disk.writeBlock(sb.data_bitmap_block, new_dbm);

        // Fix link counts for reachable inodes
        for (uint32_t i = 0; i < sb.total_inodes; ++i) {
            if (!ibm.get(i) || !reach[i]) continue;
            Inode in = read_inode(disk, sb, i);
            if (in.link_count != seen[i]) {
                in.link_count = seen[i];
                uint32_t byte_off = i * INODE_SIZE;
                uint32_t ab = sb.inode_table_block + byte_off / BLOCK_SIZE;
                uint32_t bw = byte_off % BLOCK_SIZE;
                uint8_t buf[BLOCK_SIZE];
                disk.readBlock(ab, buf);
                std::memcpy(buf + bw, &in, sizeof(Inode));
                disk.writeBlock(ab, buf);
            }
        }
        fprintf(stderr, "REPAIR: done (bitmap leaks + link counts fixed)\n");
    }

    return R.clean() ? 0 : 1;
}

// ── fsck_image: library entry point for tests ──────────────────────────────
extern "C" int fsck_image(const char* path) {
    return do_fsck(path, false);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: simplefs_fsck <image> [--repair]\n");
        return 2;
    }
    bool repair = (argc >= 3 && std::string(argv[2]) == "--repair");
    return do_fsck(argv[1], repair);
}

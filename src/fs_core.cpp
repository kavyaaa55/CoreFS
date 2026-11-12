#include "fs_core.h"
#include "directory.h"
#include "constants.h"

#include <stdexcept>
#include <cstring>
#include <ctime>
#include <string>
#include <sstream>
#include <algorithm>

// ── Constructor ────────────────────────────────────────────────────────────
FSCore::FSCore(const std::string& image_path) : disk_(image_path) {
    disk_.readBlock(0, &sb_);
    if (sb_.magic != MAGIC_NUMBER)
        throw std::runtime_error("FSCore: bad magic - not a SimpleFS image");
    if (sb_.version != FS_VERSION)
        throw std::runtime_error(
            "FSCore: version " + std::to_string(sb_.version) +
            " != " + std::to_string(FS_VERSION) + " - re-run mkfs");
    if (sb_.block_size != BLOCK_SIZE)
        throw std::runtime_error("FSCore: block size mismatch");
}

// ── Superblock ─────────────────────────────────────────────────────────────
Superblock& FSCore::superblock() { return sb_; }
void FSCore::flushSuperblock()   { disk_.writeBlock(0, &sb_); }

// ── Raw block forwarding ───────────────────────────────────────────────────
void FSCore::readBlock (uint32_t b, void*       p) { disk_.readBlock(b, p); }
void FSCore::writeBlock(uint32_t b, const void* p) { disk_.writeBlock(b, p); }

// ── Bitmap helpers ─────────────────────────────────────────────────────────
uint32_t FSCore::allocBit(uint32_t bitmap_block, uint32_t max_bits) {
    uint8_t bm[BLOCK_SIZE];
    disk_.readBlock(bitmap_block, bm);
    for (uint32_t by = 0; by < BLOCK_SIZE; ++by) {
        if (bm[by] == 0xFF) continue;
        for (int b = 0; b < 8; ++b) {
            uint32_t idx = by * 8 + static_cast<uint32_t>(b);
            if (idx >= max_bits) throw std::runtime_error("no free bits");
            if (!(bm[by] & (1u << b))) {
                bm[by] |= static_cast<uint8_t>(1u << b);
                disk_.writeBlock(bitmap_block, bm);
                return idx;
            }
        }
    }
    throw std::runtime_error("no free bits");
}

void FSCore::freeBit(uint32_t bitmap_block, uint32_t bit) {
    uint8_t bm[BLOCK_SIZE];
    disk_.readBlock(bitmap_block, bm);
    bm[bit / 8] &= static_cast<uint8_t>(~(1u << (bit % 8)));
    disk_.writeBlock(bitmap_block, bm);
}

// ── Block allocation ───────────────────────────────────────────────────────
uint32_t FSCore::allocBlock() {
    if (sb_.free_blocks == 0) throw std::runtime_error("disk is full");
    uint32_t bit = allocBit(sb_.data_bitmap_block,
                            sb_.total_blocks - sb_.data_block_start);
    --sb_.free_blocks;
    flushSuperblock();
    return sb_.data_block_start + bit;
}

void FSCore::freeBlock(uint32_t blk) {
    if (blk < sb_.data_block_start)
        throw std::runtime_error("freeBlock: out of data region");
    freeBit(sb_.data_bitmap_block, blk - sb_.data_block_start);
    ++sb_.free_blocks;
    flushSuperblock();
}

// ── Inode allocation ───────────────────────────────────────────────────────
uint32_t FSCore::allocInode() {
    if (sb_.free_inodes == 0) throw std::runtime_error("no free inodes");
    uint32_t ino = allocBit(sb_.inode_bitmap_block, sb_.total_inodes);
    --sb_.free_inodes;
    flushSuperblock();
    return ino;
}

void FSCore::freeInode(uint32_t ino) {
    freeBit(sb_.inode_bitmap_block, ino);
    ++sb_.free_inodes;
    flushSuperblock();
}

// ── Inode I/O ──────────────────────────────────────────────────────────────
void FSCore::readInode(uint32_t ino, Inode& out) {
    if (ino >= sb_.total_inodes)
        throw std::runtime_error("readInode: out of range");
    uint32_t byte_off   = ino * INODE_SIZE;
    uint32_t abs_block  = sb_.inode_table_block + byte_off / BLOCK_SIZE;
    uint32_t byte_within = byte_off % BLOCK_SIZE;
    uint8_t buf[BLOCK_SIZE];
    disk_.readBlock(abs_block, buf);
    std::memcpy(&out, buf + byte_within, sizeof(Inode));
}

void FSCore::writeInode(uint32_t ino, const Inode& in) {
    if (ino >= sb_.total_inodes)
        throw std::runtime_error("writeInode: out of range");
    uint32_t byte_off    = ino * INODE_SIZE;
    uint32_t abs_block   = sb_.inode_table_block + byte_off / BLOCK_SIZE;
    uint32_t byte_within = byte_off % BLOCK_SIZE;
    uint8_t buf[BLOCK_SIZE];
    disk_.readBlock(abs_block, buf);
    std::memcpy(buf + byte_within, &in, sizeof(Inode));
    disk_.writeBlock(abs_block, buf);
}

// ── Indirect pointer helpers ───────────────────────────────────────────────
uint32_t FSCore::readIndirectPtr(uint32_t blk, uint32_t slot) {
    if (blk == 0) return 0;
    uint8_t buf[BLOCK_SIZE];
    disk_.readBlock(blk, buf);
    uint32_t v;
    std::memcpy(&v, buf + slot * sizeof(uint32_t), sizeof(uint32_t));
    return v;
}

void FSCore::writeIndirectPtr(uint32_t blk, uint32_t slot, uint32_t val) {
    uint8_t buf[BLOCK_SIZE];
    disk_.readBlock(blk, buf);
    std::memcpy(buf + slot * sizeof(uint32_t), &val, sizeof(uint32_t));
    disk_.writeBlock(blk, buf);
}

// ── bmap ───────────────────────────────────────────────────────────────────
uint32_t FSCore::bmap(Inode& inode, uint32_t lblock, bool allocate) {
    if (static_cast<uint64_t>(lblock) >= DOUBLE_MAX)
        throw std::runtime_error("file too big");

    auto alloc_zeroed = [&]() -> uint32_t {
        uint32_t b = allocBlock();
        uint8_t z[BLOCK_SIZE] = {};
        disk_.writeBlock(b, z);
        return b;
    };

    if (lblock < DIRECT_MAX) {
        if (inode.direct[lblock]) return inode.direct[lblock];
        if (!allocate) return 0;
        uint32_t b = alloc_zeroed();
        inode.direct[lblock] = b;
        if (lblock >= inode.block_count) inode.block_count = lblock + 1;
        return b;
    }

    if (lblock < SINGLE_MAX) {
        uint32_t slot = lblock - DIRECT_MAX;
        bool new_si = false;
        if (!inode.indirect) {
            if (!allocate) return 0;
            inode.indirect = alloc_zeroed(); new_si = true;
        }
        uint32_t d = readIndirectPtr(inode.indirect, slot);
        if (d) return d;
        if (!allocate) return 0;
        uint32_t b = 0;
        try { b = alloc_zeroed(); }
        catch (...) { if (new_si) { freeBlock(inode.indirect); inode.indirect = 0; } throw; }
        writeIndirectPtr(inode.indirect, slot, b);
        if (lblock >= inode.block_count) inode.block_count = lblock + 1;
        return b;
    }

    // double indirect
    uint32_t rel     = lblock - SINGLE_MAX;
    uint32_t l1_slot = rel / PTRS_PER_BLOCK;
    uint32_t l2_slot = rel % PTRS_PER_BLOCK;
    bool new_dbl = false;
    if (!inode.double_indirect) {
        if (!allocate) return 0;
        inode.double_indirect = alloc_zeroed(); new_dbl = true;
    }
    uint32_t l1 = readIndirectPtr(inode.double_indirect, l1_slot);
    bool new_l1 = false;
    if (!l1) {
        if (!allocate) return 0;
        try { l1 = alloc_zeroed(); }
        catch (...) { if (new_dbl) { freeBlock(inode.double_indirect); inode.double_indirect = 0; } throw; }
        writeIndirectPtr(inode.double_indirect, l1_slot, l1); new_l1 = true;
    }
    uint32_t d = readIndirectPtr(l1, l2_slot);
    if (d) return d;
    if (!allocate) return 0;
    uint32_t b = 0;
    try { b = alloc_zeroed(); }
    catch (...) {
        if (new_l1) { writeIndirectPtr(inode.double_indirect, l1_slot, 0); freeBlock(l1); }
        if (new_dbl) { freeBlock(inode.double_indirect); inode.double_indirect = 0; }
        throw;
    }
    writeIndirectPtr(l1, l2_slot, b);
    if (lblock >= inode.block_count) inode.block_count = lblock + 1;
    return b;
}

// ── readFile ───────────────────────────────────────────────────────────────
uint32_t FSCore::readFile(uint32_t ino, uint32_t off, void* buf, uint32_t len) {
    Inode inode; readInode(ino, inode);
    if (off >= inode.size) return 0;
    len = std::min(len, inode.size - off);
    if (!len) return 0;
    uint32_t done = 0; auto* out = static_cast<uint8_t*>(buf);
    while (done < len) {
        uint32_t cur = off + done;
        uint32_t lb  = cur / BLOCK_SIZE, bo = cur % BLOCK_SIZE;
        uint32_t n   = std::min(len - done, BLOCK_SIZE - bo);
        uint32_t phys = bmap(inode, lb, false);
        if (!phys) { std::memset(out + done, 0, n); }
        else {
            uint8_t bb[BLOCK_SIZE]; disk_.readBlock(phys, bb);
            std::memcpy(out + done, bb + bo, n);
        }
        done += n;
    }
    return done;
}

// ── writeFile ──────────────────────────────────────────────────────────────
uint32_t FSCore::writeFile(uint32_t ino, uint32_t off, const void* buf, uint32_t len) {
    if (!len) return 0;
    if (static_cast<uint64_t>(off) + len > MAX_FILE_SIZE)
        throw std::runtime_error("file too big");
    Inode inode; readInode(ino, inode);
    uint32_t done = 0; const auto* in = static_cast<const uint8_t*>(buf);
    while (done < len) {
        uint32_t cur = off + done;
        uint32_t lb  = cur / BLOCK_SIZE, bo = cur % BLOCK_SIZE;
        uint32_t n   = std::min(len - done, BLOCK_SIZE - bo);
        uint32_t phys = bmap(inode, lb, true);
        uint8_t bb[BLOCK_SIZE];
        if (bo || n != BLOCK_SIZE) disk_.readBlock(phys, bb);
        else std::memset(bb, 0, BLOCK_SIZE);
        std::memcpy(bb + bo, in + done, n);
        disk_.writeBlock(phys, bb);
        done += n;
    }
    if (off + done > inode.size) inode.size = off + done;
    touchMC(inode);
    writeInode(ino, inode);
    return done;
}

// ── truncateFile ───────────────────────────────────────────────────────────
void FSCore::truncateFile(uint32_t ino, uint32_t new_size) {
    Inode inode; readInode(ino, inode);
    if (new_size >= inode.size) {
        inode.size = new_size; touchMC(inode); writeInode(ino, inode); return;
    }
    uint32_t first_free = (new_size == 0) ? 0 : (new_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    // zero tail of last block
    if (new_size > 0 && (new_size % BLOCK_SIZE)) {
        uint32_t phys = bmap(inode, (new_size - 1) / BLOCK_SIZE, false);
        if (phys) {
            uint8_t bb[BLOCK_SIZE]; disk_.readBlock(phys, bb);
            uint32_t zs = new_size % BLOCK_SIZE;
            std::memset(bb + zs, 0, BLOCK_SIZE - zs);
            disk_.writeBlock(phys, bb);
        }
    }
    for (uint32_t lb = first_free; lb < DIRECT_MAX; ++lb)
        if (inode.direct[lb]) { freeBlock(inode.direct[lb]); inode.direct[lb] = 0; }
    if (inode.indirect) {
        uint32_t si_buf[PTRS_PER_BLOCK]; uint8_t raw[BLOCK_SIZE];
        disk_.readBlock(inode.indirect, raw);
        std::memcpy(si_buf, raw, BLOCK_SIZE);
        uint32_t si_start = first_free > DIRECT_MAX ? first_free - DIRECT_MAX : 0;
        bool any = false;
        for (uint32_t s = 0; s < PTRS_PER_BLOCK; ++s) {
            if (!si_buf[s]) continue;
            if (s >= si_start) { freeBlock(si_buf[s]); si_buf[s] = 0; }
            else any = true;
        }
        if (!any) { freeBlock(inode.indirect); inode.indirect = 0; }
        else { std::memcpy(raw, si_buf, BLOCK_SIZE); disk_.writeBlock(inode.indirect, raw); }
    }
    if (inode.double_indirect) {
        uint32_t dbl[PTRS_PER_BLOCK]; uint8_t rdbl[BLOCK_SIZE];
        disk_.readBlock(inode.double_indirect, rdbl);
        std::memcpy(dbl, rdbl, BLOCK_SIZE);
        uint32_t dff   = first_free > SINGLE_MAX ? first_free - SINGLE_MAX : 0;
        uint32_t l1s   = dff / PTRS_PER_BLOCK, l2s = dff % PTRS_PER_BLOCK;
        bool any_l1 = false;
        for (uint32_t l1 = 0; l1 < PTRS_PER_BLOCK; ++l1) {
            if (!dbl[l1]) continue;
            if (l1 < l1s) { any_l1 = true; continue; }
            uint32_t l2b[PTRS_PER_BLOCK]; uint8_t rl2[BLOCK_SIZE];
            disk_.readBlock(dbl[l1], rl2); std::memcpy(l2b, rl2, BLOCK_SIZE);
            uint32_t l2_start = (l1 == l1s) ? l2s : 0; bool any_l2 = false;
            for (uint32_t l2 = 0; l2 < PTRS_PER_BLOCK; ++l2) {
                if (!l2b[l2]) continue;
                if (l2 >= l2_start) { freeBlock(l2b[l2]); l2b[l2] = 0; }
                else any_l2 = true;
            }
            if (!any_l2) { freeBlock(dbl[l1]); dbl[l1] = 0; }
            else { std::memcpy(rl2, l2b, BLOCK_SIZE); disk_.writeBlock(dbl[l1], rl2); any_l1 = true; }
        }
        if (!any_l1) { freeBlock(inode.double_indirect); inode.double_indirect = 0; }
        else { std::memcpy(rdbl, dbl, BLOCK_SIZE); disk_.writeBlock(inode.double_indirect, rdbl); }
    }
    inode.block_count = first_free < DIRECT_MAX ? first_free : DIRECT_MAX;
    inode.size        = new_size;
    touchMC(inode);
    writeInode(ino, inode);
}

void FSCore::freeAllBlocks(uint32_t ino) { truncateFile(ino, 0); }

void FSCore::unlinkIfUnreferenced(uint32_t ino) {
    Inode i; readInode(ino, i);
    if (i.link_count == 0) { freeAllBlocks(ino); freeInode(ino); }
}

// ═══════════════════════════════════════════════════════════════════════════
// PHASE 7
// ═══════════════════════════════════════════════════════════════════════════

uint32_t FSCore::now() { return static_cast<uint32_t>(std::time(nullptr)); }
void FSCore::touchMC(Inode& i) { i.mtime = i.ctime = now(); }
void FSCore::touchC (Inode& i) { i.ctime = now(); }

void FSCore::updateAtime(uint32_t ino) {
    Inode i; readInode(ino, i);
    if (i.atime <= i.mtime) { i.atime = now(); writeInode(ino, i); }
}

// dropLink: decrement link_count; free inode+blocks if it hits 0.
void FSCore::dropLink(uint32_t ino) {
    Inode i; readInode(ino, i);
    if (i.link_count > 0) i.link_count--;
    if (i.link_count == 0) { freeAllBlocks(ino); freeInode(ino); return; }
    touchC(i); writeInode(ino, i);
}

// ── createFile ─────────────────────────────────────────────────────────────
uint32_t FSCore::createFile(uint32_t dir, std::string_view name, uint16_t mode) {
    std::string nm(name);
    if (nm.empty() || nm == "." || nm == ".." ||
        nm.find('/') != std::string::npos || nm.size() > MAX_FILENAME_LEN)
        throw std::runtime_error("createFile: invalid name");
    if (lookupDirent(*this, dir, nm))
        throw std::runtime_error("createFile: " + nm + ": already exists");
    uint32_t ino = allocInode();
    Inode i{}; i.type = INODE_TYPE_FILE; i.mode = mode; i.link_count = 1;
    i.atime = i.mtime = i.ctime = now();
    writeInode(ino, i);
    if (!addDirent(*this, dir, nm, ino)) { freeInode(ino); throw std::runtime_error("createFile: dir full"); }
    // update parent mtime
    Inode p; readInode(dir, p); touchMC(p); writeInode(dir, p);
    return ino;
}

// ── makeDir ────────────────────────────────────────────────────────────────
uint32_t FSCore::makeDir(uint32_t parent, std::string_view name, uint16_t mode) {
    std::string nm(name);
    if (nm.empty() || nm == "." || nm == ".." ||
        nm.find('/') != std::string::npos || nm.size() > MAX_FILENAME_LEN)
        throw std::runtime_error("mkdir: invalid name");
    if (lookupDirent(*this, parent, nm))
        throw std::runtime_error("mkdir: " + nm + ": already exists");

    uint32_t ino = allocInode();
    Inode d{}; d.type = INODE_TYPE_DIR; d.mode = mode; d.link_count = 2;
    d.atime = d.mtime = d.ctime = now();
    writeInode(ino, d);

    // "." and ".."
    bool ok = addDirent(*this, ino, ".", ino);
    if (ok) ok = addDirent(*this, ino, "..", parent);
    if (!ok) { freeAllBlocks(ino); freeInode(ino); throw std::runtime_error("mkdir: dir full"); }

    if (!addDirent(*this, parent, nm, ino)) {
        freeAllBlocks(ino); freeInode(ino);
        throw std::runtime_error("mkdir: parent dir full");
    }

    // Parent gains one link (child's ".." points to it)
    Inode p; readInode(parent, p); p.link_count++; touchMC(p); writeInode(parent, p);
    return ino;
}

// ── unlinkFile ─────────────────────────────────────────────────────────────
void FSCore::unlinkFile(uint32_t dir, std::string_view name) {
    std::string nm(name);
    auto found = lookupDirent(*this, dir, nm);
    if (!found) throw std::runtime_error("rm: " + nm + ": not found");
    Inode i; readInode(*found, i);
    if (i.type == INODE_TYPE_DIR) throw std::runtime_error("rm: " + nm + ": is a directory");
    removeDirent(*this, dir, nm);
    dropLink(*found);
    Inode p; readInode(dir, p); touchMC(p); writeInode(dir, p);
}

// ── removeDir ──────────────────────────────────────────────────────────────
void FSCore::removeDir(uint32_t parent, std::string_view name) {
    std::string nm(name);
    if (nm == "." || nm == "..") throw std::runtime_error("rmdir: invalid name");
    auto found = lookupDirent(*this, parent, nm);
    if (!found) throw std::runtime_error("rmdir: " + nm + ": not found");
    Inode i; readInode(*found, i);
    if (i.type != INODE_TYPE_DIR) throw std::runtime_error("rmdir: " + nm + ": not a directory");
    if (!isDirEmpty(*this, *found)) throw std::runtime_error("rmdir: " + nm + ": directory not empty");
    removeDirent(*this, parent, nm);
    // Free the dir's data blocks and inode (link_count was 2: "." + parent entry)
    freeAllBlocks(*found);
    freeInode(*found);
    // Parent loses one link (the removed dir's ".." no longer points to it)
    Inode p; readInode(parent, p);
    if (p.link_count > 0) p.link_count--;
    touchMC(p); writeInode(parent, p);
}

// ── isAncestor ─────────────────────────────────────────────────────────────
bool FSCore::isAncestor(uint32_t ancestor, uint32_t ino) {
    uint32_t cur = ino;
    for (;;) {
        if (cur == ancestor) return true;
        if (cur == ROOT_INODE_NUM) return false;
        auto parent = lookupDirent(*this, cur, "..");
        if (!parent) return false;
        if (*parent == cur) return false; // root: ".." == self
        cur = *parent;
    }
}

// ── renameEntry ────────────────────────────────────────────────────────────
void FSCore::renameEntry(uint32_t sp, std::string_view sn,
                         uint32_t dp, std::string_view dn) {
    std::string src(sn), dst(dn);
    if (dst.empty() || dst == "." || dst == ".." ||
        dst.find('/') != std::string::npos || dst.size() > MAX_FILENAME_LEN)
        throw std::runtime_error("mv: invalid destination name");

    auto src_ino_opt = lookupDirent(*this, sp, src);
    if (!src_ino_opt) throw std::runtime_error("mv: " + src + ": not found");
    uint32_t src_ino = *src_ino_opt;
    Inode si; readInode(src_ino, si);

    // Guard: cannot move a directory into its own subtree
    if (si.type == INODE_TYPE_DIR && isAncestor(src_ino, dp))
        throw std::runtime_error("mv: cannot move directory into its own subtree");

    auto dst_ino_opt = lookupDirent(*this, dp, dst);
    bool exists = dst_ino_opt.has_value();
    uint32_t dst_ino = exists ? *dst_ino_opt : 0;

    if (exists) {
        if (dst_ino == src_ino) return; // same entry — no-op
        Inode vi; readInode(dst_ino, vi);
        if (si.type == INODE_TYPE_DIR) {
            if (vi.type != INODE_TYPE_DIR)
                throw std::runtime_error("mv: destination is not a directory");
            if (!isDirEmpty(*this, dst_ino))
                throw std::runtime_error("mv: destination directory not empty");
        } else if (vi.type == INODE_TYPE_DIR) {
            throw std::runtime_error("mv: cannot overwrite directory with file");
        }
        // Replace dst dirent with src inode
        replaceDirent(*this, dp, dst, src_ino);
    } else {
        if (!addDirent(*this, dp, dst, src_ino))
            throw std::runtime_error("mv: destination directory full");
    }

    removeDirent(*this, sp, src);

    // Free/drop whatever was at dst
    if (exists) {
        Inode vi; readInode(dst_ino, vi);
        if (vi.type == INODE_TYPE_DIR) {
            freeAllBlocks(dst_ino); freeInode(dst_ino);
        } else {
            dropLink(dst_ino);
        }
    }

    // If moving a dir across parents, rewrite its ".." and fix link counts
    if (si.type == INODE_TYPE_DIR && sp != dp) {
        replaceDirent(*this, src_ino, "..", dp);
        // sp loses one link (child no longer ".." to it)
        Inode a; readInode(sp, a);
        if (a.link_count > 0) a.link_count--;
        touchMC(a); writeInode(sp, a);
        // dp gains one link (new child ".." to it), unless it already did from an overwritten dir
        Inode b; readInode(dp, b);
        if (!exists || [&]{ Inode v; readInode(dst_ino, v); return v.type != INODE_TYPE_DIR; }())
            b.link_count++;
        // If we replaced a dir at dst, dp already lost one (the removed dir's ".." is gone)
        // The replaceDirent above already handled that via freeAllBlocks — so no extra decrement needed
        // The net for dp when replacing an empty dir: -1 (removed "..") +1 (new "..") = 0 net
        if (exists) {
            // We replaced dst dir: dp loses one ".." ref from victim, gains one from src — net 0
            // But we already incremented above; undo it
            if (b.link_count > 0) b.link_count--;
        }
        touchMC(b); writeInode(dp, b);
    }

    // Update ctime of moved inode
    Inode s; readInode(src_ino, s); touchC(s); writeInode(src_ino, s);
    // Update mtime of src and dst parent dirs
    if (sp != dp) {
        Inode pd; readInode(dp, pd); touchMC(pd); writeInode(dp, pd);
    }
    Inode ps; readInode(sp, ps); touchMC(ps); writeInode(sp, ps);
}

// ── hardLink ───────────────────────────────────────────────────────────────
void FSCore::hardLink(uint32_t src_ino, uint32_t dir, std::string_view name) {
    std::string nm(name);
    Inode s; readInode(src_ino, s);
    if (s.type == INODE_TYPE_DIR)
        throw std::runtime_error("ln: cannot hard-link a directory");
    if (nm.empty() || nm.find('/') != std::string::npos || nm.size() > MAX_FILENAME_LEN)
        throw std::runtime_error("ln: invalid name");
    if (lookupDirent(*this, dir, nm))
        throw std::runtime_error("ln: " + nm + ": already exists");
    if (!addDirent(*this, dir, nm, src_ino))
        throw std::runtime_error("ln: directory full");
    s.link_count++; touchC(s); writeInode(src_ino, s);
    Inode p; readInode(dir, p); touchMC(p); writeInode(dir, p);
}

// ── makeSymlink ────────────────────────────────────────────────────────────
uint32_t FSCore::makeSymlink(std::string_view target, uint32_t dir,
                              std::string_view name) {
    std::string nm(name), tgt(target);
    if (tgt.empty() || tgt.size() > MAX_SYMLINK_LEN)
        throw std::runtime_error("ln -s: invalid target");
    if (nm.empty() || nm.find('/') != std::string::npos || nm.size() > MAX_FILENAME_LEN)
        throw std::runtime_error("ln -s: invalid name");
    if (lookupDirent(*this, dir, nm))
        throw std::runtime_error("ln -s: " + nm + ": already exists");

    uint32_t ino = allocInode();
    Inode i{}; i.type = INODE_TYPE_SYMLINK; i.mode = 0777; i.link_count = 1;
    i.atime = i.mtime = i.ctime = now();
    writeInode(ino, i);

    // Store target as file data (writeFile sets mtime, that's fine)
    writeFile(ino, 0, tgt.data(), static_cast<uint32_t>(tgt.size()));

    if (!addDirent(*this, dir, nm, ino)) {
        freeAllBlocks(ino); freeInode(ino);
        throw std::runtime_error("ln -s: directory full");
    }
    Inode p; readInode(dir, p); touchMC(p); writeInode(dir, p);
    return ino;
}

// ── readSymlink ────────────────────────────────────────────────────────────
std::string FSCore::readSymlink(uint32_t ino) {
    Inode i; readInode(ino, i);
    if (i.type != INODE_TYPE_SYMLINK)
        throw std::runtime_error("readlink: not a symlink");
    if (i.size == 0) return {};
    std::string buf(i.size, '\0');
    readFile(ino, 0, buf.data(), i.size);
    return buf;
}

// ── resolve ────────────────────────────────────────────────────────────────
uint32_t FSCore::resolve(std::string_view path, uint32_t cwd,
                          bool follow_last, int depth) {
    if (depth > SYMLINK_DEPTH_LIMIT)
        throw std::runtime_error("too many levels of symbolic links");

    uint32_t cur = (!path.empty() && path[0] == '/') ? ROOT_INODE_NUM : cwd;

    // Split on '/'
    std::vector<std::string> parts;
    {
        std::string s(path);
        std::istringstream ss(s);
        std::string c;
        while (std::getline(ss, c, '/'))
            if (!c.empty()) parts.push_back(c);
    }

    for (size_t k = 0; k < parts.size(); ++k) {
        const std::string& comp = parts[k];
        bool last = (k + 1 == parts.size());

        if (comp == ".") continue;

        if (comp == "..") {
            // Follow ".." dirent (or stay at root)
            auto p = lookupDirent(*this, cur, "..");
            if (p) cur = *p;
            continue;
        }

        Inode ci; readInode(cur, ci);
        if (ci.type != INODE_TYPE_DIR)
            throw std::runtime_error("resolve: not a directory: " + comp);

        auto next_opt = lookupDirent(*this, cur, comp);
        if (!next_opt)
            throw std::runtime_error(comp + ": no such file or directory");
        uint32_t next = *next_opt;

        Inode ni; readInode(next, ni);
        bool do_follow = (ni.type == INODE_TYPE_SYMLINK) && (!last || follow_last);
        if (do_follow) {
            std::string tgt = readSymlink(next);
            // Relative targets resolve from the link's directory (cur)
            next = resolve(tgt, cur, true, depth + 1);
            readInode(next, ni);
        }
        cur = next;
    }
    return cur;
}

// ── inodePath ──────────────────────────────────────────────────────────────
// Walk ".." from ino up to root; collect names, then reverse to build the path.
std::string FSCore::inodePath(uint32_t ino) {
    if (ino == ROOT_INODE_NUM) return "/";
    std::vector<std::string> parts;
    uint32_t cur = ino;
    for (int depth = 0; depth < 128; ++depth) {
        auto parent_opt = lookupDirent(*this, cur, "..");
        if (!parent_opt) break;
        uint32_t parent = *parent_opt;
        if (parent == cur) break; // root
        // Find our name in parent
        auto entries = listDirents(*this, parent);
        std::string found_name;
        for (const auto& e : entries)
            if (e.inode_num == cur && std::string(e.name) != ".." && std::string(e.name) != ".")
                { found_name = e.name; break; }
        if (found_name.empty()) break;
        parts.push_back(found_name);
        if (parent == ROOT_INODE_NUM) break;
        cur = parent;
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        path += '/'; path += *it;
    }
    if (path.empty()) path = "/";
    return path;
}

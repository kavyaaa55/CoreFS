#include "fs_core.h"
#include "constants.h"

#include <stdexcept>
#include <cstring>
#include <string>
#include <algorithm>

// ── Constructor ────────────────────────────────────────────────────────────
FSCore::FSCore(const std::string& image_path)
    : disk_(image_path)
{
    disk_.readBlock(0, &sb_);
    if (sb_.magic != MAGIC_NUMBER) {
        throw std::runtime_error("FSCore: bad magic number - is this a SimpleFS image?");
    }
    if (sb_.version != FS_VERSION) {
        throw std::runtime_error(
            "FSCore: unsupported version " + std::to_string(sb_.version) +
            " (expected " + std::to_string(FS_VERSION) +
            ") - re-run mkfs to create a fresh image");
    }
    if (sb_.block_size != BLOCK_SIZE) {
        throw std::runtime_error("FSCore: block size mismatch in superblock");
    }
}

// ── Superblock ─────────────────────────────────────────────────────────────
Superblock& FSCore::superblock() { return sb_; }

void FSCore::flushSuperblock() {
    disk_.writeBlock(0, &sb_);
}

// ── Raw block access (forwarded) ───────────────────────────────────────────
void FSCore::readBlock(uint32_t block_num, void* buf) {
    disk_.readBlock(block_num, buf);
}

void FSCore::writeBlock(uint32_t block_num, const void* buf) {
    disk_.writeBlock(block_num, buf);
}

// ── Internal bitmap helpers ────────────────────────────────────────────────
uint32_t FSCore::allocBit(uint32_t bitmap_block, uint32_t max_bits) {
    uint8_t bitmap[BLOCK_SIZE];
    disk_.readBlock(bitmap_block, bitmap);

    for (uint32_t byte_idx = 0; byte_idx < BLOCK_SIZE; ++byte_idx) {
        if (bitmap[byte_idx] == 0xFF) continue;
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t index = byte_idx * 8 + static_cast<uint32_t>(bit);
            if (index >= max_bits) {
                throw std::runtime_error("FSCore::allocBit: no free bits available");
            }
            if (!(bitmap[byte_idx] & (1u << bit))) {
                bitmap[byte_idx] |= static_cast<uint8_t>(1u << bit);
                disk_.writeBlock(bitmap_block, bitmap);
                return index;
            }
        }
    }
    throw std::runtime_error("FSCore::allocBit: no free bits available");
}

void FSCore::freeBit(uint32_t bitmap_block, uint32_t bit_index) {
    uint8_t bitmap[BLOCK_SIZE];
    disk_.readBlock(bitmap_block, bitmap);
    uint32_t byte_idx = bit_index / 8;
    uint32_t bit      = bit_index % 8;
    bitmap[byte_idx] &= static_cast<uint8_t>(~(1u << bit));
    disk_.writeBlock(bitmap_block, bitmap);
}

// ── Block allocation ───────────────────────────────────────────────────────
uint32_t FSCore::allocBlock() {
    if (sb_.free_blocks == 0) {
        throw std::runtime_error("disk is full");
    }
    uint32_t total_data_blocks = sb_.total_blocks - sb_.data_block_start;
    uint32_t bit = allocBit(sb_.data_bitmap_block, total_data_blocks);
    --sb_.free_blocks;
    flushSuperblock();
    return sb_.data_block_start + bit;
}

void FSCore::freeBlock(uint32_t block_num) {
    if (block_num < sb_.data_block_start) {
        throw std::runtime_error("FSCore::freeBlock: block_num out of data region");
    }
    uint32_t bit = block_num - sb_.data_block_start;
    freeBit(sb_.data_bitmap_block, bit);
    ++sb_.free_blocks;
    flushSuperblock();
}

// ── Inode allocation ───────────────────────────────────────────────────────
uint32_t FSCore::allocInode() {
    if (sb_.free_inodes == 0) {
        throw std::runtime_error("no free inodes");
    }
    uint32_t inode_num = allocBit(sb_.inode_bitmap_block, sb_.total_inodes);
    --sb_.free_inodes;
    flushSuperblock();
    return inode_num;
}

void FSCore::freeInode(uint32_t inode_num) {
    freeBit(sb_.inode_bitmap_block, inode_num);
    ++sb_.free_inodes;
    flushSuperblock();
}

// ── Inode I/O ──────────────────────────────────────────────────────────────
void FSCore::readInode(uint32_t inode_num, Inode& out) {
    if (inode_num >= sb_.total_inodes) {
        throw std::runtime_error("FSCore::readInode: inode_num out of range");
    }
    uint32_t byte_offset  = inode_num * INODE_SIZE;
    uint32_t block_offset = byte_offset / BLOCK_SIZE;
    uint32_t byte_within  = byte_offset % BLOCK_SIZE;
    uint32_t abs_block    = sb_.inode_table_block + block_offset;

    uint8_t buf[BLOCK_SIZE];
    disk_.readBlock(abs_block, buf);
    std::memcpy(&out, buf + byte_within, sizeof(Inode));
}

void FSCore::writeInode(uint32_t inode_num, const Inode& in) {
    if (inode_num >= sb_.total_inodes) {
        throw std::runtime_error("FSCore::writeInode: inode_num out of range");
    }
    uint32_t byte_offset  = inode_num * INODE_SIZE;
    uint32_t block_offset = byte_offset / BLOCK_SIZE;
    uint32_t byte_within  = byte_offset % BLOCK_SIZE;
    uint32_t abs_block    = sb_.inode_table_block + block_offset;

    uint8_t buf[BLOCK_SIZE];
    disk_.readBlock(abs_block, buf);
    std::memcpy(buf + byte_within, &in, sizeof(Inode));
    disk_.writeBlock(abs_block, buf);
}

// ── Indirect block pointer helpers ─────────────────────────────────────────

// Read the uint32_t pointer at slot `slot` inside block `blk`.
// Returns 0 if blk == 0 (unallocated indirect block = all holes).
uint32_t FSCore::readIndirectPtr(uint32_t blk, uint32_t slot) {
    if (blk == 0) return 0;
    uint8_t buf[BLOCK_SIZE];
    disk_.readBlock(blk, buf);
    uint32_t val;
    std::memcpy(&val, buf + slot * sizeof(uint32_t), sizeof(uint32_t));
    return val;
}

// Write `val` into slot `slot` of block `blk` (blk must already be allocated).
void FSCore::writeIndirectPtr(uint32_t blk, uint32_t slot, uint32_t val) {
    uint8_t buf[BLOCK_SIZE];
    disk_.readBlock(blk, buf);
    std::memcpy(buf + slot * sizeof(uint32_t), &val, sizeof(uint32_t));
    disk_.writeBlock(blk, buf);
}

// ── bmap: logical block -> physical block ──────────────────────────────────
//
// Logical block layout:
//   [0 .. DIRECT_MAX-1]           direct[0..11]
//   [DIRECT_MAX .. SINGLE_MAX-1]  single_indirect (1024 blocks)
//   [SINGLE_MAX .. DOUBLE_MAX-1]  double_indirect (1024*1024 blocks)
//
// pointer value 0 == hole everywhere.
// allocate=true: allocates indirect + data blocks as needed.
// allocate=false: returns 0 for any hole (caller fills zeros).
//
// On allocation failure (disk full), already-allocated blocks in THIS call
// are freed before the exception propagates -- no leaks.
uint32_t FSCore::bmap(Inode& inode, uint32_t lblock, bool allocate) {
    if (static_cast<uint64_t>(lblock) >= DOUBLE_MAX) {
        throw std::runtime_error("file too big: exceeds maximum file size");
    }

    // Helper: allocate a new zeroed block; throws on disk full.
    auto alloc_zeroed = [&]() -> uint32_t {
        uint32_t b = allocBlock();
        uint8_t zero[BLOCK_SIZE] = {};
        disk_.writeBlock(b, zero);
        return b;
    };

    // ── Direct ────────────────────────────────────────────────────────────
    if (lblock < DIRECT_MAX) {
        if (inode.direct[lblock] != 0) return inode.direct[lblock];
        if (!allocate) return 0;
        uint32_t b = alloc_zeroed();
        inode.direct[lblock] = b;
        if (lblock >= inode.block_count) inode.block_count = lblock + 1;
        return b;
    }

    // ── Single indirect ───────────────────────────────────────────────────
    if (lblock < SINGLE_MAX) {
        uint32_t slot = lblock - DIRECT_MAX;

        // Allocate the indirect block itself if needed
        bool new_indirect = false;
        if (inode.indirect == 0) {
            if (!allocate) return 0;
            inode.indirect = alloc_zeroed();
            new_indirect = true;
        }

        uint32_t data_blk = readIndirectPtr(inode.indirect, slot);
        if (data_blk != 0) return data_blk;
        if (!allocate) return 0;

        uint32_t b = 0;
        try {
            b = alloc_zeroed();
        } catch (...) {
            // If we just allocated the indirect block, free it back
            if (new_indirect) {
                freeBlock(inode.indirect);
                inode.indirect = 0;
            }
            throw;
        }
        writeIndirectPtr(inode.indirect, slot, b);
        if (lblock >= inode.block_count) inode.block_count = lblock + 1;
        return b;
    }

    // ── Double indirect ───────────────────────────────────────────────────
    {
        uint32_t rel        = lblock - SINGLE_MAX;
        uint32_t l1_slot    = rel / PTRS_PER_BLOCK;  // index into double-indirect block
        uint32_t l2_slot    = rel % PTRS_PER_BLOCK;  // index into single-indirect block

        bool new_dbl = false;
        if (inode.double_indirect == 0) {
            if (!allocate) return 0;
            inode.double_indirect = alloc_zeroed();
            new_dbl = true;
        }

        uint32_t l1_blk = readIndirectPtr(inode.double_indirect, l1_slot);
        bool new_l1 = false;
        if (l1_blk == 0) {
            if (!allocate) return 0;
            try {
                l1_blk = alloc_zeroed();
            } catch (...) {
                if (new_dbl) { freeBlock(inode.double_indirect); inode.double_indirect = 0; }
                throw;
            }
            writeIndirectPtr(inode.double_indirect, l1_slot, l1_blk);
            new_l1 = true;
        }

        uint32_t data_blk = readIndirectPtr(l1_blk, l2_slot);
        if (data_blk != 0) return data_blk;
        if (!allocate) return 0;

        uint32_t b = 0;
        try {
            b = alloc_zeroed();
        } catch (...) {
            if (new_l1) {
                writeIndirectPtr(inode.double_indirect, l1_slot, 0);
                freeBlock(l1_blk);
            }
            if (new_dbl) { freeBlock(inode.double_indirect); inode.double_indirect = 0; }
            throw;
        }
        writeIndirectPtr(l1_blk, l2_slot, b);
        if (lblock >= inode.block_count) inode.block_count = lblock + 1;
        return b;
    }
}

// ── readFile ───────────────────────────────────────────────────────────────
uint32_t FSCore::readFile(uint32_t ino, uint32_t off, void* buf, uint32_t len) {
    Inode inode;
    readInode(ino, inode);

    if (off >= inode.size) return 0;
    len = std::min(len, inode.size - off);
    if (len == 0) return 0;

    uint32_t total_read = 0;
    auto* out = static_cast<uint8_t*>(buf);

    while (total_read < len) {
        uint32_t cur_off  = off + total_read;
        uint32_t lblock   = cur_off / BLOCK_SIZE;
        uint32_t blk_off  = cur_off % BLOCK_SIZE;
        uint32_t can_read = std::min(len - total_read, BLOCK_SIZE - blk_off);

        uint32_t phys = bmap(inode, lblock, false);
        if (phys == 0) {
            std::memset(out + total_read, 0, can_read); // sparse hole
        } else {
            uint8_t block_buf[BLOCK_SIZE];
            disk_.readBlock(phys, block_buf);
            std::memcpy(out + total_read, block_buf + blk_off, can_read);
        }
        total_read += can_read;
    }
    return total_read;
}

// ── writeFile ──────────────────────────────────────────────────────────────
uint32_t FSCore::writeFile(uint32_t ino, uint32_t off, const void* buf, uint32_t len) {
    if (len == 0) return 0;

    // Check MAX_FILE_SIZE before touching the disk
    if (static_cast<uint64_t>(off) + len > MAX_FILE_SIZE) {
        throw std::runtime_error("write: file too big (exceeds MAX_FILE_SIZE)");
    }

    Inode inode;
    readInode(ino, inode);

    uint32_t total_written = 0;
    const auto* in = static_cast<const uint8_t*>(buf);

    while (total_written < len) {
        uint32_t cur_off   = off + total_written;
        uint32_t lblock    = cur_off / BLOCK_SIZE;
        uint32_t blk_off   = cur_off % BLOCK_SIZE;
        uint32_t can_write = std::min(len - total_written, BLOCK_SIZE - blk_off);

        uint32_t phys = bmap(inode, lblock, true);

        uint8_t block_buf[BLOCK_SIZE];
        if (blk_off != 0 || can_write != BLOCK_SIZE) {
            disk_.readBlock(phys, block_buf); // read-modify-write
        } else {
            std::memset(block_buf, 0, BLOCK_SIZE);
        }
        std::memcpy(block_buf + blk_off, in + total_written, can_write);
        disk_.writeBlock(phys, block_buf);

        total_written += can_write;
    }

    uint32_t new_end = off + total_written;
    if (new_end > inode.size) inode.size = new_end;
    writeInode(ino, inode);
    return total_written;
}

// ── truncateFile ───────────────────────────────────────────────────────────
// Shrink: free all data blocks (and empty indirect blocks) past new_size.
// Grow: just extend size (sparse).
void FSCore::truncateFile(uint32_t ino, uint32_t new_size) {
    Inode inode;
    readInode(ino, inode);

    if (new_size >= inode.size) {
        // Grow or no-op — just update size (sparse)
        inode.size = new_size;
        writeInode(ino, inode);
        return;
    }

    // Shrink: determine the first logical block that is fully past new_size
    uint32_t first_free_lblock = (new_size == 0) ? 0
                                                  : (new_size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // Zero the tail of the last surviving block (if new_size not block-aligned)
    if (new_size > 0 && (new_size % BLOCK_SIZE) != 0) {
        uint32_t last_lb = (new_size - 1) / BLOCK_SIZE;
        uint32_t phys = bmap(inode, last_lb, false);
        if (phys != 0) {
            uint8_t block_buf[BLOCK_SIZE];
            disk_.readBlock(phys, block_buf);
            uint32_t zero_start = new_size % BLOCK_SIZE;
            std::memset(block_buf + zero_start, 0, BLOCK_SIZE - zero_start);
            disk_.writeBlock(phys, block_buf);
        }
    }

    // ── Free direct blocks past first_free_lblock ─────────────────────────
    for (uint32_t lb = first_free_lblock; lb < DIRECT_MAX; ++lb) {
        if (inode.direct[lb] != 0) {
            freeBlock(inode.direct[lb]);
            inode.direct[lb] = 0;
        }
    }

    // ── Free single-indirect range ────────────────────────────────────────
    if (inode.indirect != 0) {
        uint32_t si_buf[PTRS_PER_BLOCK];
        uint8_t raw[BLOCK_SIZE];
        disk_.readBlock(inode.indirect, raw);
        std::memcpy(si_buf, raw, BLOCK_SIZE);

        uint32_t si_start = (first_free_lblock > DIRECT_MAX)
                                ? (first_free_lblock - DIRECT_MAX) : 0;
        bool any_remaining = false;
        for (uint32_t s = 0; s < PTRS_PER_BLOCK; ++s) {
            if (si_buf[s] != 0) {
                if (s >= si_start) {
                    freeBlock(si_buf[s]);
                    si_buf[s] = 0;
                } else {
                    any_remaining = true;
                }
            }
        }
        if (!any_remaining) {
            freeBlock(inode.indirect);
            inode.indirect = 0;
        } else {
            std::memcpy(raw, si_buf, BLOCK_SIZE);
            disk_.writeBlock(inode.indirect, raw);
        }
    }

    // ── Free double-indirect range ────────────────────────────────────────
    if (inode.double_indirect != 0) {
        uint32_t dbl_buf[PTRS_PER_BLOCK];
        uint8_t  raw_dbl[BLOCK_SIZE];
        disk_.readBlock(inode.double_indirect, raw_dbl);
        std::memcpy(dbl_buf, raw_dbl, BLOCK_SIZE);

        // first l1 slot we start freeing from (relative to double-indirect)
        uint32_t dbl_first_free = (first_free_lblock > SINGLE_MAX)
                                      ? (first_free_lblock - SINGLE_MAX) : 0;
        uint32_t l1_start_slot  = dbl_first_free / PTRS_PER_BLOCK;
        uint32_t l2_start_slot  = dbl_first_free % PTRS_PER_BLOCK;

        bool any_l1_remaining = false;

        for (uint32_t l1 = 0; l1 < PTRS_PER_BLOCK; ++l1) {
            if (dbl_buf[l1] == 0) continue;

            if (l1 < l1_start_slot) {
                any_l1_remaining = true; // still live
                continue;
            }

            uint32_t l2_buf[PTRS_PER_BLOCK];
            uint8_t  raw_l2[BLOCK_SIZE];
            disk_.readBlock(dbl_buf[l1], raw_l2);
            std::memcpy(l2_buf, raw_l2, BLOCK_SIZE);

            uint32_t l2_start = (l1 == l1_start_slot) ? l2_start_slot : 0;
            bool any_l2_remaining = false;

            for (uint32_t l2 = 0; l2 < PTRS_PER_BLOCK; ++l2) {
                if (l2_buf[l2] != 0) {
                    if (l2 >= l2_start) {
                        freeBlock(l2_buf[l2]);
                        l2_buf[l2] = 0;
                    } else {
                        any_l2_remaining = true;
                    }
                }
            }

            if (!any_l2_remaining) {
                freeBlock(dbl_buf[l1]);
                dbl_buf[l1] = 0;
            } else {
                std::memcpy(raw_l2, l2_buf, BLOCK_SIZE);
                disk_.writeBlock(dbl_buf[l1], raw_l2);
                any_l1_remaining = true;
            }
        }

        if (!any_l1_remaining) {
            freeBlock(inode.double_indirect);
            inode.double_indirect = 0;
        } else {
            std::memcpy(raw_dbl, dbl_buf, BLOCK_SIZE);
            disk_.writeBlock(inode.double_indirect, raw_dbl);
        }
    }

    // Update block_count to reflect blocks still in use
    inode.block_count = first_free_lblock < DIRECT_MAX ? first_free_lblock : DIRECT_MAX;
    inode.size        = new_size;
    writeInode(ino, inode);
}

// ── freeAllBlocks ──────────────────────────────────────────────────────────
void FSCore::freeAllBlocks(uint32_t ino) {
    truncateFile(ino, 0);
}

// ── unlinkIfUnreferenced ───────────────────────────────────────────────────
void FSCore::unlinkIfUnreferenced(uint32_t ino) {
    Inode inode;
    readInode(ino, inode);
    if (inode.link_count == 0) {
        freeAllBlocks(ino);
        freeInode(ino);
    }
}

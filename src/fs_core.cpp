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
        throw std::runtime_error("FSCore: bad magic number — is this a SimpleFS image?");
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

// ── bmap: logical block → physical block ───────────────────────────────────
// Phase 5 uses only direct blocks (direct[0..11]).
// Block 0 is the superblock and never a valid data block pointer, so 0 == hole.
uint32_t FSCore::bmap(Inode& inode, uint32_t lblock, bool allocate) {
    if (lblock >= NUM_DIRECT_BLOCKS) {
        // Phase 5 only supports direct blocks.
        throw std::runtime_error("bmap: file too large for Phase 5 (no indirect blocks)");
    }

    if (inode.direct[lblock] != 0) {
        return inode.direct[lblock];
    }

    // Hole or unallocated
    if (!allocate) {
        return 0; // hole — reads as zeros
    }

    // Allocate a new block, zero it out, record in inode
    uint32_t new_block = allocBlock();
    uint8_t zero[BLOCK_SIZE] = {};
    disk_.writeBlock(new_block, zero);

    inode.direct[lblock] = new_block;
    // block_count tracks the highest logical block allocated + 1
    if (lblock >= inode.block_count) {
        inode.block_count = lblock + 1;
    }
    return new_block;
}

// ── readFile ───────────────────────────────────────────────────────────────
uint32_t FSCore::readFile(uint32_t ino, uint32_t off, void* buf, uint32_t len) {
    Inode inode;
    readInode(ino, inode);

    if (off >= inode.size) return 0; // past EOF
    len = std::min(len, inode.size - off); // clamp to file size
    if (len == 0) return 0;

    uint32_t total_read = 0;
    auto* out = static_cast<uint8_t*>(buf);

    while (total_read < len) {
        uint32_t cur_off   = off + total_read;
        uint32_t lblock    = cur_off / BLOCK_SIZE;
        uint32_t blk_off   = cur_off % BLOCK_SIZE;
        uint32_t can_read  = std::min(len - total_read, BLOCK_SIZE - blk_off);

        uint32_t phys = bmap(inode, lblock, false);
        if (phys == 0) {
            // Sparse hole — return zeros
            std::memset(out + total_read, 0, can_read);
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

    Inode inode;
    readInode(ino, inode);

    uint32_t total_written = 0;
    const auto* in = static_cast<const uint8_t*>(buf);

    while (total_written < len) {
        uint32_t cur_off   = off + total_written;
        uint32_t lblock    = cur_off / BLOCK_SIZE;
        uint32_t blk_off   = cur_off % BLOCK_SIZE;
        uint32_t can_write = std::min(len - total_written, BLOCK_SIZE - blk_off);

        uint32_t phys = bmap(inode, lblock, true); // allocate if needed

        uint8_t block_buf[BLOCK_SIZE];
        if (blk_off != 0 || can_write != BLOCK_SIZE) {
            // Partial block: read-modify-write
            disk_.readBlock(phys, block_buf);
        } else {
            std::memset(block_buf, 0, BLOCK_SIZE);
        }
        std::memcpy(block_buf + blk_off, in + total_written, can_write);
        disk_.writeBlock(phys, block_buf);

        total_written += can_write;
    }

    // Extend file size if needed
    uint32_t new_end = off + total_written;
    if (new_end > inode.size) {
        inode.size = new_end;
    }

    writeInode(ino, inode);
    return total_written;
}

// ── truncateFile ───────────────────────────────────────────────────────────
void FSCore::truncateFile(uint32_t ino, uint32_t new_size) {
    Inode inode;
    readInode(ino, inode);

    if (new_size < inode.size) {
        // Shrink: free blocks that are entirely past new_size
        uint32_t first_free_lblock = (new_size + BLOCK_SIZE - 1) / BLOCK_SIZE;

        for (uint32_t lb = first_free_lblock; lb < inode.block_count; ++lb) {
            if (lb < NUM_DIRECT_BLOCKS && inode.direct[lb] != 0) {
                freeBlock(inode.direct[lb]);
                inode.direct[lb] = 0;
            }
        }
        inode.block_count = first_free_lblock;

        // Zero the tail of the last remaining block if new_size is not block-aligned
        if (new_size > 0 && (new_size % BLOCK_SIZE) != 0) {
            uint32_t last_lblock = (new_size - 1) / BLOCK_SIZE;
            if (last_lblock < NUM_DIRECT_BLOCKS && inode.direct[last_lblock] != 0) {
                uint8_t block_buf[BLOCK_SIZE];
                disk_.readBlock(inode.direct[last_lblock], block_buf);
                uint32_t zero_start = new_size % BLOCK_SIZE;
                std::memset(block_buf + zero_start, 0, BLOCK_SIZE - zero_start);
                disk_.writeBlock(inode.direct[last_lblock], block_buf);
            }
        }
    }
    // Grow: just update size (sparse — unallocated blocks read as zeros)

    inode.size = new_size;
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

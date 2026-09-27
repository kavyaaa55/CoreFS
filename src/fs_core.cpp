#include "fs_core.h"
#include "constants.h"

#include <stdexcept>
#include <cstring>
#include <string>

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

// Scans bitmap_block for the first 0 bit among [0, max_bits).
// Sets it to 1, writes the block back, and returns the bit index.
// Throws if no free bit is found.
uint32_t FSCore::allocBit(uint32_t bitmap_block, uint32_t max_bits) {
    uint8_t bitmap[BLOCK_SIZE];
    disk_.readBlock(bitmap_block, bitmap);

    for (uint32_t byte_idx = 0; byte_idx < BLOCK_SIZE; ++byte_idx) {
        if (bitmap[byte_idx] == 0xFF) continue; // all 8 bits set
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t index = byte_idx * 8 + bit;
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
        throw std::runtime_error("FSCore::allocBlock: disk is full");
    }
    // Bit index in the data bitmap corresponds to a relative block number.
    // Absolute block number = data_block_start + bit_index.
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
        throw std::runtime_error("FSCore::allocInode: no free inodes");
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
// Inode i lives in inode_table_block + (i * INODE_SIZE / BLOCK_SIZE),
// at byte offset (i * INODE_SIZE) % BLOCK_SIZE within that block.
void FSCore::readInode(uint32_t inode_num, Inode& out) {
    if (inode_num >= sb_.total_inodes) {
        throw std::runtime_error("FSCore::readInode: inode_num out of range");
    }
    uint32_t byte_offset  = inode_num * INODE_SIZE;
    uint32_t block_offset = byte_offset / BLOCK_SIZE; // which block within inode table
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

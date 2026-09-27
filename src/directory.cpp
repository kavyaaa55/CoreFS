#include "directory.h"
#include "constants.h"

#include <cstring>
#include <stdexcept>
#include <sstream>

// Number of dirents that fit in one block
static constexpr uint32_t DIRENTS_PER_BLOCK = BLOCK_SIZE / DIRENT_SIZE;

// ── lookupDirent ───────────────────────────────────────────────────────────
std::optional<uint32_t> lookupDirent(FSCore& fs, uint32_t dir_inode_num,
                                     const std::string& name) {
    Inode dir_inode;
    fs.readInode(dir_inode_num, dir_inode);

    uint8_t block_buf[BLOCK_SIZE];

    for (uint32_t b = 0; b < dir_inode.block_count; ++b) {
        uint32_t block_num = dir_inode.direct[b];
        if (block_num == 0) continue;
        fs.readBlock(block_num, block_buf);

        const auto* dirents = reinterpret_cast<const Dirent*>(block_buf);
        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; ++i) {
            if (dirents[i].inode_num != 0 &&
                std::strncmp(dirents[i].name, name.c_str(), MAX_FILENAME_LEN + 1) == 0) {
                return dirents[i].inode_num;
            }
        }
    }
    return std::nullopt;
}

// ── addDirent ──────────────────────────────────────────────────────────────
bool addDirent(FSCore& fs, uint32_t dir_inode_num,
               const std::string& name, uint32_t inode_num) {
    Inode dir_inode;
    fs.readInode(dir_inode_num, dir_inode);

    uint8_t block_buf[BLOCK_SIZE];

    // 1. Look for a free slot in existing blocks, and check for duplicate names.
    for (uint32_t b = 0; b < dir_inode.block_count; ++b) {
        uint32_t block_num = dir_inode.direct[b];
        if (block_num == 0) continue;
        fs.readBlock(block_num, block_buf);

        auto* dirents = reinterpret_cast<Dirent*>(block_buf);
        // Check for name collision in this block
        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; ++i) {
            if (dirents[i].inode_num != 0 &&
                std::strncmp(dirents[i].name, name.c_str(), MAX_FILENAME_LEN + 1) == 0) {
                return false; // name already exists
            }
        }
        // Find a free slot in this block
        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; ++i) {
            if (dirents[i].inode_num == 0) {
                dirents[i].inode_num = inode_num;
                std::memset(dirents[i].name, 0, MAX_FILENAME_LEN + 1);
                std::strncpy(dirents[i].name, name.c_str(), MAX_FILENAME_LEN);
                fs.writeBlock(block_num, block_buf);
                return true;
            }
        }
    }

    // 2. No free slot found — try to allocate a new block.
    if (dir_inode.block_count >= NUM_DIRECT_BLOCKS) {
        return false; // directory is full (1536-entry cap)
    }

    uint32_t new_block = fs.allocBlock();

    // Zero the new block and place the dirent in slot 0.
    std::memset(block_buf, 0, BLOCK_SIZE);
    auto* dirents = reinterpret_cast<Dirent*>(block_buf);
    dirents[0].inode_num = inode_num;
    std::memset(dirents[0].name, 0, MAX_FILENAME_LEN + 1);
    std::strncpy(dirents[0].name, name.c_str(), MAX_FILENAME_LEN);
    fs.writeBlock(new_block, block_buf);

    // Update directory inode.
    dir_inode.direct[dir_inode.block_count] = new_block;
    ++dir_inode.block_count;
    fs.writeInode(dir_inode_num, dir_inode);

    return true;
}

// ── listDirents ────────────────────────────────────────────────────────────
std::vector<Dirent> listDirents(FSCore& fs, uint32_t dir_inode_num) {
    Inode dir_inode;
    fs.readInode(dir_inode_num, dir_inode);

    std::vector<Dirent> results;
    uint8_t block_buf[BLOCK_SIZE];

    for (uint32_t b = 0; b < dir_inode.block_count; ++b) {
        uint32_t block_num = dir_inode.direct[b];
        if (block_num == 0) continue;
        fs.readBlock(block_num, block_buf);

        const auto* dirents = reinterpret_cast<const Dirent*>(block_buf);
        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; ++i) {
            if (dirents[i].inode_num != 0) {
                results.push_back(dirents[i]);
            }
        }
    }
    return results;
}

// ── resolvePath ────────────────────────────────────────────────────────────
// Handles absolute ("/a/b") and relative ("a/b") paths.
// Does NOT handle ".." — that is dealt with by the Shell's cwd_stack_.
std::optional<uint32_t> resolvePath(FSCore& fs, uint32_t cwd_inode_num,
                                    const std::string& path) {
    if (path.empty()) return std::nullopt;

    uint32_t current_inode;
    std::string remaining;

    if (path[0] == '/') {
        current_inode = ROOT_INODE_NUM;
        remaining = path.substr(1);
    } else {
        current_inode = cwd_inode_num;
        remaining = path;
    }

    // Split on '/' and walk each component
    std::istringstream ss(remaining);
    std::string component;

    while (std::getline(ss, component, '/')) {
        if (component.empty() || component == ".") continue;
        // ".." is intentionally not handled here — the Shell handles it
        auto found = lookupDirent(fs, current_inode, component);
        if (!found) return std::nullopt;

        // If this is not the last component, it must be a directory
        Inode inode;
        fs.readInode(*found, inode);
        if (inode.type != INODE_TYPE_DIR) {
            // Check if there are more components; peek by checking ss
            std::string rest;
            if (std::getline(ss, rest)) {
                return std::nullopt; // non-final component is not a dir
            }
        }
        current_inode = *found;
    }

    return current_inode;
}

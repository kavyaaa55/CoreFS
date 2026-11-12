#include "directory.h"
#include "constants.h"
#include <cstring>

static constexpr uint32_t DIRENTS_PER_BLOCK = BLOCK_SIZE / DIRENT_SIZE;

// ── lookupDirent ───────────────────────────────────────────────────────────
std::optional<uint32_t> lookupDirent(FSCore& fs, uint32_t dir_ino,
                                     const std::string& name) {
    Inode dir; fs.readInode(dir_ino, dir);
    uint8_t buf[BLOCK_SIZE];
    for (uint32_t b = 0; b < dir.block_count; ++b) {
        if (dir.direct[b] == 0) continue;
        fs.readBlock(dir.direct[b], buf);
        const auto* d = reinterpret_cast<const Dirent*>(buf);
        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; ++i)
            if (d[i].inode_num != 0 &&
                std::strncmp(d[i].name, name.c_str(), MAX_FILENAME_LEN + 1) == 0)
                return d[i].inode_num;
    }
    return std::nullopt;
}

// ── addDirent ──────────────────────────────────────────────────────────────
bool addDirent(FSCore& fs, uint32_t dir_ino,
               const std::string& name, uint32_t inode_num) {
    Inode dir; fs.readInode(dir_ino, dir);
    uint8_t buf[BLOCK_SIZE];

    for (uint32_t b = 0; b < dir.block_count; ++b) {
        if (dir.direct[b] == 0) continue;
        fs.readBlock(dir.direct[b], buf);
        auto* d = reinterpret_cast<Dirent*>(buf);
        // Check collision first
        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; ++i)
            if (d[i].inode_num != 0 &&
                std::strncmp(d[i].name, name.c_str(), MAX_FILENAME_LEN + 1) == 0)
                return false;
        // Find free slot
        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; ++i) {
            if (d[i].inode_num == 0) {
                d[i].inode_num = inode_num;
                std::memset(d[i].name, 0, MAX_FILENAME_LEN + 1);
                std::strncpy(d[i].name, name.c_str(), MAX_FILENAME_LEN);
                fs.writeBlock(dir.direct[b], buf);
                return true;
            }
        }
    }

    // Allocate new data block
    if (dir.block_count >= NUM_DIRECT_BLOCKS) return false;
    uint32_t nb = fs.allocBlock();
    std::memset(buf, 0, BLOCK_SIZE);
    auto* d = reinterpret_cast<Dirent*>(buf);
    d[0].inode_num = inode_num;
    std::memset(d[0].name, 0, MAX_FILENAME_LEN + 1);
    std::strncpy(d[0].name, name.c_str(), MAX_FILENAME_LEN);
    fs.writeBlock(nb, buf);
    dir.direct[dir.block_count++] = nb;
    fs.writeInode(dir_ino, dir);
    return true;
}

// ── removeDirent ───────────────────────────────────────────────────────────
bool removeDirent(FSCore& fs, uint32_t dir_ino, const std::string& name) {
    Inode dir; fs.readInode(dir_ino, dir);
    uint8_t buf[BLOCK_SIZE];
    for (uint32_t b = 0; b < dir.block_count; ++b) {
        if (dir.direct[b] == 0) continue;
        fs.readBlock(dir.direct[b], buf);
        auto* d = reinterpret_cast<Dirent*>(buf);
        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; ++i) {
            if (d[i].inode_num != 0 &&
                std::strncmp(d[i].name, name.c_str(), MAX_FILENAME_LEN + 1) == 0) {
                d[i].inode_num = 0;
                std::memset(d[i].name, 0, MAX_FILENAME_LEN + 1);
                fs.writeBlock(dir.direct[b], buf);
                return true;
            }
        }
    }
    return false;
}

// ── replaceDirent ──────────────────────────────────────────────────────────
bool replaceDirent(FSCore& fs, uint32_t dir_ino,
                   const std::string& name, uint32_t new_inode_num) {
    Inode dir; fs.readInode(dir_ino, dir);
    uint8_t buf[BLOCK_SIZE];
    for (uint32_t b = 0; b < dir.block_count; ++b) {
        if (dir.direct[b] == 0) continue;
        fs.readBlock(dir.direct[b], buf);
        auto* d = reinterpret_cast<Dirent*>(buf);
        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; ++i) {
            if (d[i].inode_num != 0 &&
                std::strncmp(d[i].name, name.c_str(), MAX_FILENAME_LEN + 1) == 0) {
                d[i].inode_num = new_inode_num;
                fs.writeBlock(dir.direct[b], buf);
                return true;
            }
        }
    }
    return false;
}

// ── isDirEmpty ─────────────────────────────────────────────────────────────
// Returns true if directory has no entries other than "." and "..".
bool isDirEmpty(FSCore& fs, uint32_t dir_ino) {
    Inode dir; fs.readInode(dir_ino, dir);
    uint8_t buf[BLOCK_SIZE];
    for (uint32_t b = 0; b < dir.block_count; ++b) {
        if (dir.direct[b] == 0) continue;
        fs.readBlock(dir.direct[b], buf);
        const auto* d = reinterpret_cast<const Dirent*>(buf);
        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; ++i) {
            if (d[i].inode_num == 0) continue;
            if (std::strncmp(d[i].name, ".",  MAX_FILENAME_LEN + 1) == 0) continue;
            if (std::strncmp(d[i].name, "..", MAX_FILENAME_LEN + 1) == 0) continue;
            return false;
        }
    }
    return true;
}

// ── listDirents ────────────────────────────────────────────────────────────
std::vector<Dirent> listDirents(FSCore& fs, uint32_t dir_ino) {
    Inode dir; fs.readInode(dir_ino, dir);
    std::vector<Dirent> out;
    uint8_t buf[BLOCK_SIZE];
    for (uint32_t b = 0; b < dir.block_count; ++b) {
        if (dir.direct[b] == 0) continue;
        fs.readBlock(dir.direct[b], buf);
        const auto* d = reinterpret_cast<const Dirent*>(buf);
        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; ++i)
            if (d[i].inode_num != 0) out.push_back(d[i]);
    }
    return out;
}

// ── listDirentsUser ────────────────────────────────────────────────────────
// Same as listDirents but skips "." and "..".
std::vector<Dirent> listDirentsUser(FSCore& fs, uint32_t dir_ino) {
    auto all = listDirents(fs, dir_ino);
    std::vector<Dirent> out;
    for (const auto& e : all) {
        if (std::strncmp(e.name, ".",  MAX_FILENAME_LEN + 1) == 0) continue;
        if (std::strncmp(e.name, "..", MAX_FILENAME_LEN + 1) == 0) continue;
        out.push_back(e);
    }
    return out;
}

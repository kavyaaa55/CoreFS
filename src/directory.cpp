#include "directory.h"
#include "constants.h"
#include <cstring>
#include <exception>

static constexpr uint32_t DIRENTS_PER_BLOCK = BLOCK_SIZE / DIRENT_SIZE;

// A slot is in use when its name is non-empty. inode_num 0 is the root inode,
// so it cannot double as the "free slot" marker.
static bool slotUsed(const Dirent& e) { return e.name[0] != '\0'; }
static bool nameIs(const Dirent& e, const std::string& name) {
    return slotUsed(e) && std::strncmp(e.name, name.c_str(), MAX_FILENAME_LEN + 1) == 0;
}
static bool isDotOrDotDot(const Dirent& e) {
    return nameIs(e, ".") || nameIs(e, "..");
}

// What a scan callback wants to happen after looking at one slot.
enum class Scan { Next, Stop, WriteStop };

// Visit every slot of every block of a directory. Directories use the same
// block map as files (12 direct + single + double indirect), so they are no
// longer limited to 12 blocks / 1536 entries. block_count is the logical
// extent of the directory. Returns true if the callback stopped the scan.
template <class Fn>
static bool scanDir(FSCore& fs, uint32_t dir_ino, Fn&& fn) {
    Inode dir; fs.readInode(dir_ino, dir);
    uint8_t buf[BLOCK_SIZE];
    for (uint32_t lb = 0; lb < dir.block_count; ++lb) {
        uint32_t phys = fs.mapBlock(dir, lb, false);
        if (!phys) continue;
        fs.readBlock(phys, buf);
        auto* d = reinterpret_cast<Dirent*>(buf);
        for (uint32_t i = 0; i < DIRENTS_PER_BLOCK; ++i) {
            Scan s = fn(d[i]);
            if (s == Scan::WriteStop) { fs.writeBlock(phys, buf); return true; }
            if (s == Scan::Stop) return true;
        }
    }
    return false;
}

static void fillDirent(Dirent& e, const std::string& name, uint32_t inode_num) {
    e.inode_num = inode_num;
    std::memset(e.name, 0, MAX_FILENAME_LEN + 1);
    std::strncpy(e.name, name.c_str(), MAX_FILENAME_LEN);
}

// ── lookupDirent ───────────────────────────────────────────────────────────
std::optional<uint32_t> lookupDirent(FSCore& fs, uint32_t dir_ino,
                                     const std::string& name) {
    std::optional<uint32_t> out;
    scanDir(fs, dir_ino, [&](Dirent& e) {
        if (!nameIs(e, name)) return Scan::Next;
        out = e.inode_num; return Scan::Stop;
    });
    return out;
}

// ── addDirent ──────────────────────────────────────────────────────────────
bool addDirent(FSCore& fs, uint32_t dir_ino,
               const std::string& name, uint32_t inode_num) {
    // Collision check over the whole directory first (a duplicate may sit in
    // a later block than the first free slot).
    if (lookupDirent(fs, dir_ino, name)) return false;

    // Reuse a free slot if there is one
    if (scanDir(fs, dir_ino, [&](Dirent& e) {
            if (slotUsed(e)) return Scan::Next;
            fillDirent(e, name, inode_num); return Scan::WriteStop;
        }))
        return true;

    // Grow the directory by one block (direct or indirect)
    Inode dir; fs.readInode(dir_ino, dir);
    uint32_t lb = dir.block_count;
    uint32_t phys;
    try { phys = fs.mapBlock(dir, lb, true); }        // allocates a zeroed block
    catch (const std::exception&) { return false; }    // disk full / dir at max size
    uint8_t buf[BLOCK_SIZE];
    std::memset(buf, 0, BLOCK_SIZE);
    fillDirent(reinterpret_cast<Dirent*>(buf)[0], name, inode_num);
    fs.writeBlock(phys, buf);
    dir.size = dir.block_count * BLOCK_SIZE;            // dirs report allocated bytes
    fs.writeInode(dir_ino, dir);
    return true;
}

// ── removeDirent ───────────────────────────────────────────────────────────
bool removeDirent(FSCore& fs, uint32_t dir_ino, const std::string& name) {
    return scanDir(fs, dir_ino, [&](Dirent& e) {
        if (!nameIs(e, name)) return Scan::Next;
        e.inode_num = 0;
        std::memset(e.name, 0, MAX_FILENAME_LEN + 1);
        return Scan::WriteStop;
    });
}

// ── replaceDirent ──────────────────────────────────────────────────────────
bool replaceDirent(FSCore& fs, uint32_t dir_ino,
                   const std::string& name, uint32_t new_inode_num) {
    return scanDir(fs, dir_ino, [&](Dirent& e) {
        if (!nameIs(e, name)) return Scan::Next;
        e.inode_num = new_inode_num; return Scan::WriteStop;
    });
}

// ── isDirEmpty ─────────────────────────────────────────────────────────────
// Returns true if directory has no entries other than "." and "..".
bool isDirEmpty(FSCore& fs, uint32_t dir_ino) {
    bool found = scanDir(fs, dir_ino, [&](Dirent& e) {
        return (slotUsed(e) && !isDotOrDotDot(e)) ? Scan::Stop : Scan::Next;
    });
    return !found;
}

// ── listDirents ────────────────────────────────────────────────────────────
std::vector<Dirent> listDirents(FSCore& fs, uint32_t dir_ino) {
    std::vector<Dirent> out;
    scanDir(fs, dir_ino, [&](Dirent& e) {
        if (slotUsed(e)) out.push_back(e);
        return Scan::Next;
    });
    return out;
}

// ── listDirentsUser ────────────────────────────────────────────────────────
// Same as listDirents but skips "." and "..".
std::vector<Dirent> listDirentsUser(FSCore& fs, uint32_t dir_ino) {
    std::vector<Dirent> out;
    scanDir(fs, dir_ino, [&](Dirent& e) {
        if (slotUsed(e) && !isDotOrDotDot(e)) out.push_back(e);
        return Scan::Next;
    });
    return out;
}

#include "disk.h"
#include "constants.h"

#include <stdexcept>
#include <string>
#include <cerrno>
#include <cstring>

// Windows / POSIX portability
#ifdef _WIN32
#  include <io.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  define OPEN_RW(p)   ::_open((p), _O_RDWR | _O_BINARY, 0)
#  define OPEN_CREAT(p) ::_open((p), _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY, \
                                _S_IREAD | _S_IWRITE)
#  define CLOSE(fd)    ::_close(fd)
#  define PREAD(fd, buf, n, off)  disk_pread((fd), (buf), (n), (off))
#  define PWRITE(fd, buf, n, off) disk_pwrite((fd), (buf), (n), (off))

#include <cstdio>
static ssize_t disk_pread(int fd, void* buf, size_t count, int64_t offset) {
    if (::_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    return ::_read(fd, buf, static_cast<unsigned int>(count));
}
static ssize_t disk_pwrite(int fd, const void* buf, size_t count, int64_t offset) {
    if (::_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    return ::_write(fd, buf, static_cast<unsigned int>(count));
}
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  define OPEN_RW(p)    ::open((p), O_RDWR)
#  define OPEN_CREAT(p) ::open((p), O_RDWR | O_CREAT | O_TRUNC, 0644)
#  define CLOSE(fd)     ::close(fd)
#  define PREAD(fd, buf, n, off)  ::pread((fd), (buf), (n), (off))
#  define PWRITE(fd, buf, n, off) ::pwrite((fd), (buf), (n), (off))
#endif

// ── Constructor ────────────────────────────────────────────────────────────
Disk::Disk(const std::string& path) {
    fd_ = OPEN_RW(path.c_str());
    if (fd_ < 0) {
        throw std::runtime_error("Disk: cannot open image '" + path +
                                 "': " + std::strerror(errno));
    }
}

Disk::~Disk() {
    if (fd_ >= 0) {
        CLOSE(fd_);
    }
}

// ── readBlock ──────────────────────────────────────────────────────────────
void Disk::readBlock(uint32_t block_num, void* buf) {
    int64_t offset = static_cast<int64_t>(block_num) * BLOCK_SIZE;
    auto n = PREAD(fd_, buf, BLOCK_SIZE, offset);
    if (n != static_cast<decltype(n)>(BLOCK_SIZE)) {
        throw std::runtime_error("Disk::readBlock: short read on block " +
                                 std::to_string(block_num) +
                                 " (errno=" + std::strerror(errno) + ")");
    }
}

// ── writeBlock ─────────────────────────────────────────────────────────────
void Disk::writeBlock(uint32_t block_num, const void* buf) {
    int64_t offset = static_cast<int64_t>(block_num) * BLOCK_SIZE;
    auto n = PWRITE(fd_, buf, BLOCK_SIZE, offset);
    if (n != static_cast<decltype(n)>(BLOCK_SIZE)) {
        throw std::runtime_error("Disk::writeBlock: short write on block " +
                                 std::to_string(block_num) +
                                 " (errno=" + std::strerror(errno) + ")");
    }
}

// ── createImage (used by mkfs only) ───────────────────────────────────────
void Disk::createImage(const std::string& path, uint64_t total_bytes) {
    int fd = OPEN_CREAT(path.c_str());
    if (fd < 0) {
        throw std::runtime_error("Disk::createImage: cannot create '" + path +
                                 "': " + std::strerror(errno));
    }

    // Write zeros in BLOCK_SIZE chunks
    static const uint8_t zero_block[BLOCK_SIZE] = {};
    uint64_t written = 0;
    while (written < total_bytes) {
        auto n = PWRITE(fd, zero_block, BLOCK_SIZE,
                        static_cast<int64_t>(written));
        if (n != static_cast<decltype(n)>(BLOCK_SIZE)) {
            CLOSE(fd);
            throw std::runtime_error("Disk::createImage: write failure at offset " +
                                     std::to_string(written));
        }
        written += BLOCK_SIZE;
    }
    CLOSE(fd);
}

#pragma once
#include <string>
#include <cstdint>

// Raw fixed-size-block access to a disk image file.
// No knowledge of superblock / inodes / directories.
class Disk {
public:
    explicit Disk(const std::string& path); // opens existing image file for read/write
    ~Disk();

    // Reads exactly BLOCK_SIZE bytes from the given block number into buf.
    // Throws std::runtime_error on any I/O failure.
    void readBlock(uint32_t block_num, void* buf);

    // Writes exactly BLOCK_SIZE bytes from buf to the given block number.
    // Throws std::runtime_error on any I/O failure.
    void writeBlock(uint32_t block_num, const void* buf);

    // Creates (or truncates) a new image file of total_bytes size.
    // Used only by mkfs — not part of the normal read/write interface.
    static void createImage(const std::string& path, uint64_t total_bytes);

private:
    int fd_;
};

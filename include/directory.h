#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include "fs_core.h"
#include "types.h"

// Low-level dirent helpers — do NOT touch link counts.

std::optional<uint32_t> lookupDirent(FSCore& fs, uint32_t dir_inode_num,
                                     const std::string& name);

bool addDirent(FSCore& fs, uint32_t dir_inode_num,
               const std::string& name, uint32_t inode_num);

bool removeDirent(FSCore& fs, uint32_t dir_inode_num, const std::string& name);

// Replace the inode_num of an existing dirent `name` in-place.
// Returns false if not found.
bool replaceDirent(FSCore& fs, uint32_t dir_inode_num,
                   const std::string& name, uint32_t new_inode_num);

// Returns true if directory has no entries other than "." and "..".
bool isDirEmpty(FSCore& fs, uint32_t dir_inode_num);

// Returns all non-zero dirents (includes "." and "..").
std::vector<Dirent> listDirents(FSCore& fs, uint32_t dir_inode_num);

// Returns all non-zero dirents EXCLUDING "." and "..".
std::vector<Dirent> listDirentsUser(FSCore& fs, uint32_t dir_inode_num);

#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include "fs_core.h"
#include "types.h"

// Returns the inode number for `name` inside directory dir_inode_num,
// or std::nullopt if not found.
std::optional<uint32_t> lookupDirent(FSCore& fs, uint32_t dir_inode_num,
                                     const std::string& name);

// Adds a new dirent (name -> inode_num) into directory dir_inode_num.
// Allocates a new data block for the directory if all currently-allocated
// blocks are full. Returns false if:
//   - the name already exists, or
//   - the directory is at its direct-block capacity and cannot grow further.
bool addDirent(FSCore& fs, uint32_t dir_inode_num,
               const std::string& name, uint32_t inode_num);

// Returns every non-empty dirent (inode_num != 0) in the directory.
std::vector<Dirent> listDirents(FSCore& fs, uint32_t dir_inode_num);

// Resolves a path (absolute "/a/b/c" or relative "a/b") starting from
// cwd_inode_num for relative paths, or root for absolute paths.
// Does NOT handle "..": that is handled by the Shell's cwd_stack_.
// Returns the resolved inode number, or std::nullopt if any component
// doesn't exist or a non-final component isn't a directory.
std::optional<uint32_t> resolvePath(FSCore& fs, uint32_t cwd_inode_num,
                                    const std::string& path);

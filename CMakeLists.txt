cmake_minimum_required(VERSION 3.16)
project(simplefs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warning flags
if(MSVC)
    add_compile_options(/W4 /WX /wd4996)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

include_directories(include)

# ── Shared library sources (everything except the two mains) ──────────────
set(FS_SOURCES
    src/disk.cpp
    src/fs_core.cpp
    src/directory.cpp
    src/shell.cpp
)

# ── mkfs executable ───────────────────────────────────────────────────────
add_executable(mkfs
    src/mkfs_main.cpp
    src/disk.cpp          # mkfs only needs Disk directly
)
target_include_directories(mkfs PRIVATE include)

# ── simplefs_shell executable ─────────────────────────────────────────────
add_executable(simplefs_shell
    ${FS_SOURCES}
    src/shell_main.cpp
)
target_include_directories(simplefs_shell PRIVATE include)

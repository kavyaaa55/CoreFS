cmake_minimum_required(VERSION 3.16)
project(simplefs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sanitizer build option
option(SFS_SANITIZE "Build with AddressSanitizer + UBSan" OFF)

if(MSVC)
    add_compile_options(/W4 /WX /wd4996)
else()
    add_compile_options(-Wall -Wextra -Werror)
    if(SFS_SANITIZE)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# Fetch doctest (single-header test framework)
include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
)
FetchContent_MakeAvailable(doctest)

include_directories(include)

# ── Core FS library (shared by shell, fsck, tests) ────────────────────────
add_library(sfs_core STATIC
    src/disk.cpp
    src/fs_core.cpp
    src/directory.cpp
)
target_include_directories(sfs_core PUBLIC include)

# ── mkfs executable ───────────────────────────────────────────────────────
add_executable(mkfs src/mkfs_main.cpp)
target_link_libraries(mkfs PRIVATE sfs_core)

# ── simplefs_shell executable ─────────────────────────────────────────────
add_executable(simplefs_shell
    src/shell.cpp
    src/shell_main.cpp
)
target_link_libraries(simplefs_shell PRIVATE sfs_core)

# ── simplefs_fsck executable ──────────────────────────────────────────────
add_executable(simplefs_fsck tools/fsck.cpp)
target_link_libraries(simplefs_fsck PRIVATE sfs_core)

# ── Unit + integration tests ──────────────────────────────────────────────
add_executable(sfs_tests
    tests/test_main.cpp
    tests/test_bitmap.cpp
    tests/test_inode.cpp
    tests/test_dirent.cpp
    tests/test_path.cpp
    tests/test_commands.cpp
)
target_link_libraries(sfs_tests PRIVATE sfs_core doctest::doctest)
target_include_directories(sfs_tests PRIVATE include)

# ── Model-based fuzzer ────────────────────────────────────────────────────
add_executable(sfs_fuzz tests/fuzz.cpp)
target_link_libraries(sfs_fuzz PRIVATE sfs_core)
target_include_directories(sfs_fuzz PRIVATE include)

# ── CTest integration ─────────────────────────────────────────────────────
enable_testing()
add_test(NAME unit_tests  COMMAND sfs_tests)
add_test(NAME fuzz_100    COMMAND sfs_fuzz 100 400)   # 100 seeds, 400 ops each

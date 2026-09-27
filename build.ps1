# SimpleFS build script for Windows (MSYS2 g++)
# Usage: .\build.ps1
# Output: build\mkfs.exe, build\simplefs_shell.exe

$GPP  = "C:\msys64\ucrt64\bin\g++.exe"
$ROOT = $PSScriptRoot
$INC  = "$ROOT\include"
$SRC  = "$ROOT\src"
$OUT  = "$ROOT\build"

if (-not (Test-Path $GPP)) {
    Write-Error "g++ not found at $GPP — install MSYS2 ucrt64 g++"
    exit 1
}

New-Item -ItemType Directory -Force -Path $OUT | Out-Null

$FLAGS = @("-std=c++20", "-Wall", "-Wextra", "-Werror", "-I$INC")

Write-Host "Building mkfs..."
& $GPP @FLAGS "$SRC\mkfs_main.cpp" "$SRC\disk.cpp" -o "$OUT\mkfs.exe"
if ($LASTEXITCODE -ne 0) { Write-Error "mkfs build failed"; exit 1 }

Write-Host "Building simplefs_shell..."
& $GPP @FLAGS `
    "$SRC\shell_main.cpp" `
    "$SRC\shell.cpp" `
    "$SRC\directory.cpp" `
    "$SRC\fs_core.cpp" `
    "$SRC\disk.cpp" `
    -o "$OUT\simplefs_shell.exe"
if ($LASTEXITCODE -ne 0) { Write-Error "simplefs_shell build failed"; exit 1 }

Write-Host "Build complete. Binaries in: $OUT"

# Aphelion Research — Build Script
# Run from the aphelion directory

$ErrorActionPreference = "Stop"

$VS_ROOT = "C:\Program Files\Microsoft Visual Studio\18\Community"
$VCPKG_ROOT = "$VS_ROOT\VC\vcpkg"
$CMAKE = "$VS_ROOT\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$VCPKG = "$VCPKG_ROOT\vcpkg.exe"
$TOOLCHAIN = "$VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"

Write-Host "================================================================"
Write-Host " Aphelion Research - Build System"
Write-Host "================================================================"
Write-Host " CMake:     $CMAKE"
Write-Host " vcpkg:     $VCPKG"
Write-Host " Toolchain: $TOOLCHAIN"
Write-Host "================================================================"

# Step 1: Install dependencies
Write-Host "`n[step 1] Installing vcpkg dependencies (this may take 10-20 min on first run)..."
& $VCPKG install --triplet x64-windows --allow-unsupported
if ($LASTEXITCODE -ne 0) {
    Write-Error "vcpkg install failed"
    exit 1
}

# Step 2: Configure
Write-Host "`n[step 2] Configuring CMake..."
& $CMAKE -B build -S . `
    "-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN" `
    "-DVCPKG_TARGET_TRIPLET=x64-windows" `
    "-DCMAKE_BUILD_TYPE=Release"
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configure failed"
    exit 1
}

# Step 3: Build
Write-Host "`n[step 3] Building..."
& $CMAKE --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed"
    exit 1
}

Write-Host "`n================================================================"
Write-Host " BUILD COMPLETE"
Write-Host " Binary: build\Release\aphelion.exe"
Write-Host "================================================================"

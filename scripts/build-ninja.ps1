# ==============================================================================
# OpenTune - Ninja 一键构建脚本（Windows / VS2022 MSVC 工具链）
# 用法:
#   .\scripts\build-ninja.ps1                     # ARA2 VST3 Release 增量构建
#   .\scripts\build-ninja.ps1 -Config Debug       # 调试构建
#   .\scripts\build-ninja.ps1 -Clean              # 清空构建目录后重新配置
# ==============================================================================
param(
    [ValidateSet("Release", "Debug")][string]$Config = "Release",
    [switch]$Clean
)
$ErrorActionPreference = "Stop"

$root       = Split-Path -Parent $PSScriptRoot
$vcvars     = "F:\VSC\VC\Auxiliary\Build\vcvars64.bat"
$cmake      = "F:\VSC\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$preset     = "windows-ara-ninja"
$buildDir   = Join-Path $root "build-ara-ninja"

if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }
if (-not (Test-Path $cmake))  { throw "cmake not found: $cmake" }

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir ..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

# vcvars64 提供 cl.exe / INCLUDE / LIB 环境，Ninja 生成器必须在此环境下 configure
Write-Host "== Configure: $preset ($Config) ==" -ForegroundColor Cyan
& cmd /c "call `"$vcvars`" >nul && `"$cmake`" --preset $preset -DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)" }

Write-Host "== Build: $buildDir ==" -ForegroundColor Cyan
& cmd /c "call `"$vcvars`" >nul && `"$cmake`" --build `"$buildDir`" --parallel"
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }

Write-Host "Build OK." -ForegroundColor Green

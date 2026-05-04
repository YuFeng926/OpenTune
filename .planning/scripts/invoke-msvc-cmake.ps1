param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,

    [string]$Target = "OpenTuneTests",

    [string]$Config = "Release"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---------- 检测 VS 安装 ----------
# 定义支持的 VS 版本（优先新版）
$vsSpecs = @(
    @{ Generator = "Visual Studio 18 2026"; MajorVer = 18; DirNames = @("18") },
    @{ Generator = "Visual Studio 17 2022"; MajorVer = 17; DirNames = @("2022") }
)

$generator = $null
$instanceArg = $null

# 方法1: vswhere（标准检测）
$vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vsWhere)
{
    foreach ($spec in $vsSpecs)
    {
        $verRange = "[$($spec.MajorVer).0,$($spec.MajorVer + 1).0)"
        $vsPath = & $vsWhere -all -prerelease -version $verRange -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath | Select-Object -First 1
        if ($vsPath -and (Test-Path $vsPath))
        {
            $generator = $spec.Generator
            Write-Host "vswhere detected: $generator at $vsPath"
            break
        }
    }
}

# 方法2: 目录扫描 + CMAKE_GENERATOR_INSTANCE（vswhere 注册缺失时的回退）
if (-not $generator)
{
    $roots = @("$env:ProgramFiles\Microsoft Visual Studio", "${env:ProgramFiles(x86)}\Microsoft Visual Studio")
    foreach ($spec in $vsSpecs)
    {
        foreach ($root in $roots)
        {
            foreach ($dirName in $spec.DirNames)
            {
                $candidates = Get-ChildItem "$root\$dirName" -Directory -ErrorAction SilentlyContinue
                foreach ($edition in $candidates)
                {
                    $devenv = Join-Path $edition.FullName "Common7\IDE\devenv.exe"
                    $vctools = Join-Path $edition.FullName "VC\Tools\MSVC"
                    if ((Test-Path $devenv) -or (Test-Path $vctools))
                    {
                        # 提取版本号
                        $isolationFile = Join-Path $edition.FullName "Common7\IDE\devenv.isolation.ini"
                        $version = ""
                        if (Test-Path $isolationFile)
                        {
                            $match = Select-String -Path $isolationFile -Pattern "InstallationVersion=(.+)" | Select-Object -First 1
                            if ($match) { $version = $match.Matches[0].Groups[1].Value.Trim() }
                        }
                        if (-not $version -and (Test-Path $devenv))
                        {
                            $version = (Get-Item $devenv).VersionInfo.ProductVersion
                        }

                        $generator = $spec.Generator
                        $instanceArg = "$($edition.FullName),version=$version"
                        Write-Host "Directory scan detected: $generator at $($edition.FullName) (version=$version)"
                        break
                    }
                }
                if ($generator) { break }
            }
            if ($generator) { break }
        }
        if ($generator) { break }
    }
}

if (-not $generator)
{
    throw "No supported Visual Studio with C++ tools found (need VS 2022 or 2026)"
}

# ---------- Configure ----------
$configArgs = @(
    "-S", ".",
    "-B", $BuildDir,
    "-G", $generator,
    "-A", "x64"
)

if ($instanceArg)
{
    $configArgs += "-DCMAKE_GENERATOR_INSTANCE=$instanceArg"
}

Write-Host "CMake configure: cmake $($configArgs -join ' ')"
& cmake @configArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)" }

# ---------- Build (MSBuild) ----------
$buildArgs = @(
    "--build", $BuildDir,
    "--target", $Target,
    "--config", $Config,
    "--parallel"
)

Write-Host "CMake build: cmake $($buildArgs -join ' ')"
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw "CMake build failed (exit $LASTEXITCODE)" }

Write-Host "Build succeeded: $Target ($Config)"

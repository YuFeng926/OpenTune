# Download the Windows dependencies listed in BUILDING.md into this runner workspace.
# Nothing here is written back to the repository.
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root

$JuceRef = "8.0.12"
$JuceCommit = "29396c22c93392d6738e021b83196283d6e4d850"
$AraRef = "releases/2.3.0"
$AraCommit = "0005c532bb03eb210e7bb490329fc6d7ba1792c7"
$SoundTouchRef = "2.3.2"
$SoundTouchCommit = "29fba832a7920a04eab956b3990c50e13d8c93f9"
$SoundTouchArchiveSha256 = "ed714f84a3e748de87b24f385ec69d3bdc51ca47b7f4710d2048b84b2761e7ff"
$OnnxRuntimeDirectMlSha256 = "57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681"
$DirectMlSha256 = "4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9"
$Direct3DSha256 = "0e9bcf32aac9a79343ede9b21e4864950ee54577e3d8e19bfcdf002bb4e9bfd6"
$OnnxRuntimeCpuSha256 = "d2319fddfb6ea4db99ccc4b60c85c517bcd855721f5daa6a06d40d7cb2ee2357"
$OnnxRuntimeDmlMtDllSha256 = "402856ef7f06a9c598eef0b14b62a6be242c63908b0811563f0d5d6a13ea2d51"

$Work = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { Join-Path $env:TEMP "opentune-ci" }
New-Item -ItemType Directory -Force -Path $Work, (Join-Path $Root "ThirdParty"), (Join-Path $Root "models") | Out-Null

function Invoke-Checked {
    param([string]$File, [string[]]$CommandArguments)
    & $File @CommandArguments
    if ($LASTEXITCODE -ne 0) {
        throw "$File failed with exit code $LASTEXITCODE"
    }
}

function Save-Url {
    param([string]$Url, [string]$Destination)
    Invoke-Checked "curl.exe" @("-L", "--fail", "--retry", "5", "--retry-delay", "2", "-o", $Destination, $Url)
}

function Assert-Sha256 {
    param([string]$Path, [string]$Expected, [string]$Label)
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Expected) {
        throw "$Label SHA-256 mismatch: expected $Expected, got $actual"
    }
}

$Python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $Python) {
    throw "python was not found; cannot patch JUCE or download models"
}
$env:PYTHONUTF8 = "1"
$env:PYTHONIOENCODING = "utf-8"
$PatchJuce = Join-Path $Root "scripts\github_actions\patch-juce-ara.py"

function Install-Juce {
    $JuceDir = Join-Path $Root "JUCE-master"
    if (Test-Path -LiteralPath $JuceDir) {
        # JUCE-master may hold user data; never delete it automatically.
        $head = & git -C $JuceDir rev-parse HEAD
        if ($LASTEXITCODE -ne 0) {
            throw "JUCE-master exists but is not a recognizable Git checkout; refusing to modify or delete it. Resolve it manually (remove it yourself or fix the checkout)."
        }
        if ($head.Trim() -ne $JuceCommit) {
            throw "Existing JUCE-master is not at the pinned revision: expected $JuceCommit, got $($head.Trim()). Refusing to modify or delete it; resolve it manually."
        }
        & $Python $PatchJuce
        if ($LASTEXITCODE -ne 0) {
            throw "patch-juce-ara.py failed on the existing JUCE-master; the directory is left untouched. Fix or remove it manually and re-run."
        }
        return
    }
    Invoke-Checked "git" @(
        "-c", "core.longpaths=true",
        "clone", "--depth", "1", "--branch", $JuceRef,
        "https://github.com/juce-framework/JUCE.git",
        $JuceDir
    )
    $head = & git -C $JuceDir rev-parse HEAD
    if ($LASTEXITCODE -ne 0) {
        throw "Fresh JUCE-master clone is not a readable Git checkout; keeping the failed directory for inspection"
    }
    if ($head.Trim() -ne $JuceCommit) {
        throw "JUCE $JuceRef tag revision mismatch: expected $JuceCommit, got $($head.Trim()); keeping the failed JUCE-master directory for inspection"
    }
    & $Python $PatchJuce
    if ($LASTEXITCODE -ne 0) {
        throw "patch-juce-ara.py failed on the freshly cloned JUCE-master; keeping the directory for inspection"
    }
}

function Test-CleanGitTree {
    param([string]$Directory)
    $status = & git -C $Directory status --porcelain --untracked-files=all --ignored=matching
    if ($LASTEXITCODE -ne 0 -or @($status).Count -gt 0) { return $false }
    return $true
}

function Test-AraCheckout {
    param([string]$Directory)
    if (-not (Test-CleanGitTree $Directory)) { return $false }
    # Do not use `git submodule foreach` here. Windows PowerShell splits a
    # quoted $(...) command, and git then runs `test -z $(git "$@"` which
    # fails before it can read submodule status.
    $submodules = & git -C $Directory -c core.longpaths=true submodule status --recursive
    if ($LASTEXITCODE -ne 0) { return $false }
    foreach ($line in @($submodules)) {
        if ($null -eq $line -or $line.Length -eq 0) { continue }
        if ($line[0] -ne " ") { return $false }
        if ($line -notmatch '^ [0-9a-f]+ (\S+)') { return $false }
        $relative = $Matches[1].Replace('/', [IO.Path]::DirectorySeparatorChar)
        if (-not (Test-CleanGitTree (Join-Path $Directory $relative))) { return $false }
    }
    return $true
}

function Install-Ara {
    $AraDir = Join-Path $Root "ThirdParty\ARA_SDK-releases-2.3.0"
    $ready = $false
    if (Test-Path -LiteralPath $AraDir) {
        $head = & git -C $AraDir rev-parse HEAD
        if ($LASTEXITCODE -eq 0 -and $head.Trim() -eq $AraCommit -and (Test-AraCheckout $AraDir)) {
            $ready = $true
        } else {
            Remove-Item -LiteralPath $AraDir -Recurse -Force
        }
    }
    if (-not $ready) {
        Invoke-Checked "git" @(
            "-c", "core.longpaths=true",
            "clone", "--depth", "1", "--branch", $AraRef,
            "https://github.com/Celemony/ARA_SDK.git",
            $AraDir
        )
        $head = & git -C $AraDir rev-parse HEAD
        if ($LASTEXITCODE -ne 0) { throw "ARA_SDK-releases-2.3.0 is not a readable Git checkout" }
        if ($head.Trim() -ne $AraCommit) {
            Remove-Item -LiteralPath $AraDir -Recurse -Force
            throw "ARA $AraRef tag revision mismatch: expected $AraCommit, got $($head.Trim())"
        }
        Invoke-Checked "git" @("-C", $AraDir, "-c", "core.longpaths=true", "submodule", "update", "--init", "--recursive")
        if (-not (Test-AraCheckout $AraDir)) {
            Remove-Item -LiteralPath $AraDir -Recurse -Force
            throw "ARA SDK checkout or submodules are not clean at the pinned revisions"
        }
    }
}

Install-Juce

Install-Ara

function Test-SoundTouchSource {
    param([string]$Directory)
    $configure = Join-Path $Directory "configure.ac"
    if (-not (Test-Path -LiteralPath $configure)) { return $false }
    if (-not (Select-String -LiteralPath $configure -Pattern 'AC_INIT\(\[SoundTouch\],\[2\.3\.2\]' -Quiet)) { return $false }
    return Test-Path -LiteralPath (Join-Path $Directory "source\SoundTouch\SoundTouch.cpp")
}

function Install-SoundTouch {
    $dest = Join-Path $Root "ThirdParty\soundtouch-2.3.2"
    $marker = Join-Path $dest "source\SoundTouch\SoundTouch.cpp"
    if (Test-Path -LiteralPath $dest) {
        if (Test-Path -LiteralPath (Join-Path $dest ".git")) {
            $head = & git -C $dest rev-parse HEAD
            if ($LASTEXITCODE -eq 0 -and $head.Trim() -eq $SoundTouchCommit -and (Test-CleanGitTree $dest) -and (Test-Path -LiteralPath $marker)) {
                return
            }
            Remove-Item -LiteralPath $dest -Recurse -Force
        } elseif (Test-SoundTouchSource $dest) {
            return
        } else {
            Remove-Item -LiteralPath $dest -Recurse -Force
            Install-SoundTouchArchive
            return
        }
    }
    for ($attempt = 1; $attempt -le 5; $attempt++) {
        if (Test-Path -LiteralPath $dest) {
            Remove-Item -LiteralPath $dest -Recurse -Force
        }
        & git clone --depth 1 --branch $SoundTouchRef "https://codeberg.org/soundtouch/soundtouch.git" $dest
        if ($LASTEXITCODE -eq 0) {
            $head = & git -C $dest rev-parse HEAD
            if ($LASTEXITCODE -ne 0) { throw "SoundTouch checkout has no readable Git HEAD" }
            if ($head.Trim() -ne $SoundTouchCommit) {
                Remove-Item -LiteralPath $dest -Recurse -Force
                throw "SoundTouch $SoundTouchRef tag revision mismatch: expected $SoundTouchCommit, got $($head.Trim())"
            }
            if (-not (Test-CleanGitTree $dest) -or -not (Test-Path -LiteralPath $marker)) {
                Remove-Item -LiteralPath $dest -Recurse -Force
                throw "SoundTouch checkout is not clean at the pinned revision"
            }
            return
        }
        Write-Host "SoundTouch git clone failed (attempt $attempt/5)"
        Start-Sleep -Seconds (5 * $attempt)
    }
    Install-SoundTouchArchive
}

function Install-SoundTouchArchive {
    $dest = Join-Path $Root "ThirdParty\soundtouch-2.3.2"
    $marker = Join-Path $dest "source\SoundTouch\SoundTouch.cpp"
    Write-Host "Fetching the verified SoundTouch $SoundTouchRef source archive"
    if (Test-Path -LiteralPath $dest) {
        Remove-Item -LiteralPath $dest -Recurse -Force
    }
    $archive = Join-Path $Work "soundtouch-$SoundTouchRef.tar.gz"
    $extract = Join-Path $Work "soundtouch-extract"
    if (Test-Path -LiteralPath $extract) {
        Remove-Item -LiteralPath $extract -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $extract | Out-Null
    Save-Url "https://codeberg.org/soundtouch/soundtouch/archive/$SoundTouchRef.tar.gz" $archive
    Assert-Sha256 -Path $archive -Expected $SoundTouchArchiveSha256 -Label "SoundTouch archive"
    Invoke-Checked "tar.exe" @("--exclude=*/LazarusTest/*", "-xzf", $archive, "-C", $extract)
    $unpacked = Join-Path $extract "soundtouch"
    if (-not (Test-Path -LiteralPath (Join-Path $unpacked "source\SoundTouch\SoundTouch.cpp"))) {
        throw "SoundTouch archive is missing source/SoundTouch/SoundTouch.cpp"
    }
    Move-Item -LiteralPath $unpacked -Destination $dest
    if (-not (Test-Path -LiteralPath $marker)) {
        Remove-Item -LiteralPath $dest -Recurse -Force
        throw "SoundTouch archive is missing source/SoundTouch/SoundTouch.cpp"
    }
}

Install-SoundTouch

function Expand-Nupkg {
    param([string]$Url, [string]$Destination, [string]$Sha256)
    $zip = Join-Path $Work ((Split-Path $Destination -Leaf) + ".zip")
    Save-Url $Url $zip
    Assert-Sha256 -Path $zip -Expected $Sha256 -Label $Destination
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Invoke-Checked "tar.exe" @("-xf", $zip, "-C", $Destination)
}

Expand-Nupkg `
    "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.24.4" `
    (Join-Path $Root "ThirdParty\onnxruntime-dml-1.24.4") `
    $OnnxRuntimeDirectMlSha256
Expand-Nupkg `
    "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.4" `
    (Join-Path $Root "ThirdParty\microsoft.ai.directml.1.15.4") `
    $DirectMlSha256
Expand-Nupkg `
    "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.619.5" `
    (Join-Path $Root "ThirdParty\microsoft.direct3d.d3d12.1.619.5") `
    $Direct3DSha256

$OrtZip = Join-Path $Work "onnxruntime-win-x64-1.24.4.zip"
$OrtExtract = Join-Path $Work "onnxruntime-win-x64-1.24.4"
Save-Url "https://github.com/microsoft/onnxruntime/releases/download/v1.24.4/onnxruntime-win-x64-1.24.4.zip" $OrtZip
Assert-Sha256 -Path $OrtZip -Expected $OnnxRuntimeCpuSha256 -Label "ONNX Runtime CPU archive"
if (Test-Path -LiteralPath $OrtExtract) {
    Remove-Item -LiteralPath $OrtExtract -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $OrtExtract | Out-Null
Invoke-Checked "tar.exe" @("-xf", $OrtZip, "-C", $OrtExtract)
$header = Get-ChildItem -Path $OrtExtract -Recurse -Filter "onnxruntime_cxx_api.h" | Select-Object -First 1
if (-not $header) {
    throw "ONNX Runtime CPU package has no include/onnxruntime_cxx_api.h"
}
$packageRoot = $header.Directory.Parent.FullName
$ortTarget = Join-Path $Root "ThirdParty\onnxruntime-win-x64-1.24.4"
if (Test-Path -LiteralPath $ortTarget) {
    Remove-Item -LiteralPath $ortTarget -Recurse -Force
}
Move-Item -LiteralPath $packageRoot -Destination $ortTarget

# Replace the upstream NuGet onnxruntime.dll with the project-maintained
# /MT RelWithDebInfo build tracked under scripts/github_actions/assets/.
$mtDll = Join-Path $Root "scripts\github_actions\assets\onnxruntime-1.24.4-mt.dll"
$dmlNative = Join-Path $Root "ThirdParty\onnxruntime-dml-1.24.4\runtimes\win-x64\native"
if (-not (Test-Path -LiteralPath $mtDll)) {
    throw "Project-maintained ONNX Runtime /MT DLL is missing: scripts\github_actions\assets\onnxruntime-1.24.4-mt.dll"
}
if (-not (Test-Path -LiteralPath (Join-Path $dmlNative "onnxruntime.dll"))) {
    throw "DML NuGet package did not unpack onnxruntime.dll under $dmlNative; cannot apply the /MT override"
}
Assert-Sha256 -Path $mtDll -Expected $OnnxRuntimeDmlMtDllSha256 -Label "onnxruntime-1.24.4-mt.dll"
Copy-Item -LiteralPath $mtDll -Destination (Join-Path $dmlNative "onnxruntime.dll") -Force

Invoke-Checked $Python @((Join-Path $Root "scripts\github_actions\fetch-models.py"))

$required = @(
    "ThirdParty\onnxruntime-win-x64-1.24.4\include\onnxruntime_cxx_api.h",
    "ThirdParty\onnxruntime-dml-1.24.4\build\native\include\dml_provider_factory.h",
    "ThirdParty\onnxruntime-dml-1.24.4\runtimes\win-x64\native\onnxruntime.dll",
    "ThirdParty\microsoft.ai.directml.1.15.4\include\DirectML.h",
    "ThirdParty\microsoft.ai.directml.1.15.4\bin\x64-win\DirectML.dll",
    "ThirdParty\microsoft.ai.directml.1.15.4\bin\x64-win\DirectML.lib",
    "ThirdParty\microsoft.direct3d.d3d12.1.619.5\build\native\include\d3d12.h",
    "ThirdParty\microsoft.direct3d.d3d12.1.619.5\build\native\include\d3dx12",
    "ThirdParty\microsoft.direct3d.d3d12.1.619.5\build\native\bin\x64\D3D12Core.dll",
    "ThirdParty\microsoft.direct3d.d3d12.1.619.5\build\native\bin\x64\d3d12SDKLayers.dll",
    "ThirdParty\r8brain-free-src-master\r8bbase.h",
    "ThirdParty\soundtouch-2.3.2\source\SoundTouch\SoundTouch.cpp",
    "ThirdParty\ARA_SDK-releases-2.3.0\ARA_Library",
    "models\fcpe.onnx",
    "pc_nsf_hifigan_44.1k_ONNX\pc_nsf_hifigan_44.1k_hop512_128bin_2025.02.onnx"
)
foreach ($relative in $required) {
    $path = Join-Path $Root $relative
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing dependency required by BUILDING.md: $relative"
    }
}
Write-Host "Windows dependencies are ready."

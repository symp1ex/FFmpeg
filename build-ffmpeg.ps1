param(
    [string]$Msys2Root = "C:\msys64",
    [string]$FfmpegSource = "",
    [string]$InstallPrefix = "",
    [int]$Jobs = 0,
    [switch]$SkipConfigure
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
if ($FfmpegSource -eq "") {
    $FfmpegSource = Join-Path $RepoRoot "ffmpeg"
}
if ($InstallPrefix -eq "") {
    $InstallPrefix = Join-Path $FfmpegSource "rdagent-build"
}
if ($Jobs -le 0) {
    $Jobs = [Math]::Max(1, [Environment]::ProcessorCount)
}

$Bash = Join-Path $Msys2Root "usr\bin\bash.exe"
if (!(Test-Path $Bash)) {
    throw "MSYS2 bash was not found at $Bash. Install MSYS2, then install mingw-w64-ucrt-x86_64-toolchain, nasm, pkgconf, make, and libvpx."
}
if (!(Test-Path (Join-Path $FfmpegSource "configure"))) {
    throw "FFmpeg source directory not found: $FfmpegSource"
}

function Convert-ToMsysPath([string]$Path) {
    $Full = [System.IO.Path]::GetFullPath($Path)
    $Drive = $Full.Substring(0, 1).ToLowerInvariant()
    $Rest = $Full.Substring(2).Replace("\", "/")
    return "/$Drive$Rest"
}

$SourceMsys = Convert-ToMsysPath $FfmpegSource
$PrefixMsys = Convert-ToMsysPath $InstallPrefix
$MsysHome = Join-Path $RepoRoot ".msys2-home"
$MsysTmp = Join-Path $RepoRoot ".msys2-tmp"
New-Item -ItemType Directory -Force -Path $MsysHome | Out-Null
New-Item -ItemType Directory -Force -Path $MsysTmp | Out-Null
$HomeMsys = Convert-ToMsysPath $MsysHome
$TmpMsys = Convert-ToMsysPath $MsysTmp
$env:HOME = $MsysHome
$env:TMPDIR = $MsysTmp
$env:TMP = $MsysTmp
$env:TEMP = $MsysTmp
$env:PATH = (Join-Path $Msys2Root "ucrt64\bin") + ";" + (Join-Path $Msys2Root "usr\bin") + ";" + $env:PATH

$ConfigureArgs = @(
    "--prefix=$PrefixMsys",
    "--target-os=win64",
    "--arch=x86_64",
    "--enable-libvpx",
    "--enable-mediafoundation",
    "--enable-encoder=libvpx_vp8,h264_mf,av1_mf",
    "--enable-filter=ddagrab,hwdownload,format,scale_d3d11",
    "--enable-muxer=ivf,h264",
    "--enable-indev=lavfi",
    "--enable-protocol=pipe",
    "--disable-debug"
)

$ConfigureLine = "./configure " + ($ConfigureArgs -join " ")
$Script = @"
set -euo pipefail
export PATH=/ucrt64/bin:/usr/bin:`$PATH
export HOME="$HomeMsys"
export TMPDIR="$TmpMsys"
export TMP="$TmpMsys"
export TEMP="$TmpMsys"
cd "$SourceMsys"
if [ "$($SkipConfigure.IsPresent)" != "True" ]; then
  $ConfigureLine
fi
make -j$Jobs
"@

Write-Host "Building FFmpeg from $FfmpegSource"
Write-Host "Output: $(Join-Path $FfmpegSource "ffmpeg.exe")"
Write-Host "Dependencies: MSYS2 UCRT64 toolchain, nasm, pkgconf/pkg-config, make, libvpx, Windows SDK MediaFoundation headers/libs."

& $Bash -lc $Script
if ($LASTEXITCODE -ne 0) {
    throw "FFmpeg build failed with exit code $LASTEXITCODE"
}

$BuiltExe = Join-Path $FfmpegSource "ffmpeg.exe"
if (!(Test-Path $BuiltExe)) {
    throw "Build finished but ffmpeg.exe was not found at $BuiltExe"
}

New-Item -ItemType Directory -Force -Path (Join-Path $InstallPrefix "bin") | Out-Null
Copy-Item -Force $BuiltExe (Join-Path $InstallPrefix "bin\ffmpeg.exe")

$RuntimeDlls = @(
    "libgcc_s_seh-1.dll",
    "libiconv-2.dll",
    "libwinpthread-1.dll",
    "libstdc++-6.dll",
    "libvpx-1.dll",
    "zlib1.dll"
)
$UcrtBin = Join-Path $Msys2Root "ucrt64\bin"
foreach ($Dll in $RuntimeDlls) {
    $SourceDll = Join-Path $UcrtBin $Dll
    if (Test-Path $SourceDll) {
        Copy-Item -Force $SourceDll (Join-Path $FfmpegSource $Dll)
        Copy-Item -Force $SourceDll (Join-Path $InstallPrefix ("bin\" + $Dll))
    }
}

$Encoders = (& $BuiltExe -hide_banner -encoders) -join "`n"
foreach ($Encoder in @("libvpx", "h264_mf", "av1_mf")) {
    if ($Encoders -notmatch [regex]::Escape($Encoder)) {
        throw "Built ffmpeg.exe is missing encoder $Encoder"
    }
}

Write-Host "FFmpeg build completed."

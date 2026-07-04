param(
    [string]$Msys2Root = "C:\msys64",
    [string]$FfmpegSource = "",
    [string]$OutputDir = "",
    [string]$InstallPrefix = "",
    [int]$Jobs = 0,
    [switch]$SkipConfigure,
    [switch]$EnableSVTAV1
)

$ErrorActionPreference = "Stop"

# Repo root is the directory where this script is located.
# This prevents _ffmpeg_build from being created one level above the repository.
# Repo root is normally the directory where this script is located.
# If the script is placed inside the FFmpeg source tree, use this directory
# as both repository root and FFmpeg source directory.
$RepoRoot = $PSScriptRoot

if ($FfmpegSource -eq "") {
    if (Test-Path (Join-Path $PSScriptRoot "configure")) {
        $FfmpegSource = $PSScriptRoot
    } else {
        $FfmpegSource = Join-Path $RepoRoot "ffmpeg"
    }
}

# Final runtime output: only ffmpeg.exe and required DLLs should remain here.
if ($OutputDir -eq "") {
    $OutputDir = Join-Path $RepoRoot "_ffmpeg_build"
}

# Configure/install output must not live inside final _ffmpeg_build,
# otherwise helper scripts, headers, pkg-config files, import libs, etc. remain there.
if ($InstallPrefix -eq "") {
    $InstallPrefix = Join-Path $RepoRoot "_ffmpeg_install"
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

function Copy-RequiredDll {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DllName,

        [Parameter(Mandatory = $true)]
        [string]$SourceDir,

        [Parameter(Mandatory = $true)]
        [string]$DestinationDir,

        [switch]$Required
    )

    $SourceDll = Join-Path $SourceDir $DllName
    $DestinationDll = Join-Path $DestinationDir $DllName

    if (Test-Path $SourceDll) {
        Copy-Item -Force $SourceDll $DestinationDll
        Write-Host "Copied runtime DLL: $DllName"
        return
    }

    if ($Required) {
        throw "Required runtime DLL was not found: $SourceDll"
    }

    Write-Host "Optional runtime DLL was not found, skipped: $SourceDll"
}

function Save-TextUtf8NoBom {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Text
    )

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

$SourceMsys = Convert-ToMsysPath $FfmpegSource
$PrefixMsys = Convert-ToMsysPath $InstallPrefix
$OutputMsys = Convert-ToMsysPath $OutputDir

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

# Minimal shared/DLL FFmpeg build for rdagent.
$ConfigureArgs = @(
    "--prefix=$PrefixMsys",
    "--target-os=mingw32",
    "--arch=x86_64",

    "--disable-static",
    "--enable-shared",
    "--disable-autodetect",
    "--disable-debug",
    "--disable-doc",
    "--disable-network",
    "--enable-small",
    "--disable-runtime-cpudetect",
    "--disable-everything",
    "--disable-ffplay",
    "--disable-ffprobe",
    "--enable-ffmpeg",

    "--enable-d3d11va",
    "--enable-dxva2",
    "--enable-mediafoundation",
    "--enable-libvpx",

    "--enable-indev=lavfi",

    "--enable-filter=ddagrab",
    "--enable-filter=hwdownload",
    "--enable-filter=scale",
    "--enable-filter=scale_d3d11",
    "--enable-filter=format",
    "--enable-filter=null",
    "--enable-filter=lutrgb",
    "--enable-swscale",

    "--enable-decoder=wrapped_avframe",

    "--enable-encoder=libvpx_vp8",
    "--enable-encoder=h264_mf",
    "--enable-encoder=av1_mf",

    "--enable-muxer=ivf",
    "--enable-muxer=h264",
    "--enable-protocol=pipe",

    "--enable-indev=lavfi",
    "--enable-indev=gdigrab"
)

if ($EnableSVTAV1) {
    $ConfigureArgs += @(
        "--enable-libsvtav1",
        "--enable-encoder=libsvt_av1"
    )
}

$ConfigureLine = "./configure " + ($ConfigureArgs -join " ")

# Keep the generated build script outside _ffmpeg_build too.
$BuildScriptPath = Join-Path $RepoRoot "build_local_ffmpeg_shared.generated.sh"

$Script = @"
set -euo pipefail

export PATH=/ucrt64/bin:/usr/bin:`$PATH
export HOME="$HomeMsys"
export TMPDIR="$TmpMsys"
export TMP="$TmpMsys"
export TEMP="$TmpMsys"

cd "$SourceMsys"

if [ "$($SkipConfigure.IsPresent)" != "True" ]; then
  make distclean >/dev/null 2>&1 || true
  rm -rf "$PrefixMsys"
  mkdir -p "$PrefixMsys"

  $ConfigureLine
fi

make -j$Jobs
make install

if [ ! -f "$PrefixMsys/bin/ffmpeg.exe" ]; then
  echo "ffmpeg.exe was not installed. Trying direct program build/install..."
  make ffmpeg.exe -j$Jobs
  mkdir -p "$PrefixMsys/bin"
  if [ -f "ffmpeg.exe" ]; then
    cp -f "ffmpeg.exe" "$PrefixMsys/bin/ffmpeg.exe"
  fi
fi

if [ ! -f "$PrefixMsys/bin/ffmpeg.exe" ]; then
  echo "ERROR: ffmpeg.exe was not produced." >&2
  exit 1
fi

strip "$PrefixMsys/bin/ffmpeg.exe" 2>/dev/null || true
strip "$PrefixMsys/bin"/*.dll 2>/dev/null || true

mkdir -p "$OutputMsys"

# Final runtime directory must contain only ffmpeg.exe and required DLLs.
rm -rf "$OutputMsys"/*

cp -f "$PrefixMsys/bin/ffmpeg.exe" "$OutputMsys/ffmpeg.exe"
cp -f "$PrefixMsys/bin"/*.dll "$OutputMsys/" 2>/dev/null || true

echo ""
echo "Configured linkage:"
grep -E '^CONFIG_STATIC=|^CONFIG_SHARED=' config.h || true

echo ""
echo "Configured encoders:"
grep -E 'CONFIG_(LIBVPX_VP8|H264_MF|AV1_MF|LIBSVT_AV1)_ENCODER' config.h || true

echo ""
echo "Configured capture/conversion components:"
grep -E 'CONFIG_(DDAGRAB|HWDOWNLOAD|SCALE|SCALE_D3D11|FORMAT|NULL|LUTRGB)_FILTER|CONFIG_SWSCALE|CONFIG_(D3D11VA|DXVA2|MEDIAFOUNDATION)' config.h || true
"@

Save-TextUtf8NoBom -Path $BuildScriptPath -Text $Script

Write-Host "Building minimal shared FFmpeg from local source: $FfmpegSource"
Write-Host "Final output directory: $OutputDir"
Write-Host "Install prefix: $InstallPrefix"
Write-Host "SVT-AV1 enabled: $($EnableSVTAV1.IsPresent)"
Write-Host "Dependencies: MSYS2 UCRT64 toolchain, nasm, pkgconf/pkg-config, make, libvpx, Windows SDK MediaFoundation headers/libs."
Write-Host "Generated MSYS2 build script: $BuildScriptPath"

& $Bash -lc "bash '$(Convert-ToMsysPath $BuildScriptPath)'"
if ($LASTEXITCODE -ne 0) {
    throw "FFmpeg build failed with exit code $LASTEXITCODE"
}

$BuiltExe = Join-Path $OutputDir "ffmpeg.exe"
if (!(Test-Path $BuiltExe)) {
    throw "Build finished but ffmpeg.exe was not found at $BuiltExe"
}

$UcrtBin = Join-Path $Msys2Root "ucrt64\bin"

# Required runtime DLLs copied immediately into _ffmpeg_build.
$RequiredRuntimeDlls = @(
    "libgcc_s_seh-1.dll",
    "libiconv-2.dll",
    "libstdc++-6.dll",
    "libvpx-1.dll",
    "libwinpthread-1.dll",
    "zlib1.dll"
)

foreach ($Dll in $RequiredRuntimeDlls) {
    Copy-RequiredDll -DllName $Dll -SourceDir $UcrtBin -DestinationDir $OutputDir -Required
}

# SVT-AV1 runtime is only allowed when -EnableSVTAV1 is explicitly used.
if ($EnableSVTAV1) {
    Copy-RequiredDll -DllName "libSvtAv1Enc-4.dll" -SourceDir $UcrtBin -DestinationDir $OutputDir -Required
} else {
    $SvtDllInOutput = Join-Path $OutputDir "libSvtAv1Enc-4.dll"
    if (Test-Path $SvtDllInOutput) {
        Remove-Item -Force $SvtDllInOutput
        Write-Host "Removed libSvtAv1Enc-4.dll because -EnableSVTAV1 was not specified."
    }
}

$Encoders = (& $BuiltExe -hide_banner -encoders) -join "`n"
foreach ($Encoder in @("libvpx", "h264_mf", "av1_mf")) {
    if ($Encoders -notmatch [regex]::Escape($Encoder)) {
        throw "Built ffmpeg.exe is missing encoder $Encoder"
    }
}

if ($EnableSVTAV1) {
    if ($Encoders -notmatch [regex]::Escape("libsvt_av1")) {
        throw "Built ffmpeg.exe is missing encoder libsvt_av1"
    }
}

$Devices = (& $BuiltExe -hide_banner -devices) -join "`n"

foreach ($Device in @("gdigrab", "lavfi")) {
    if ($Devices -notmatch [regex]::Escape($Device)) {
        throw "Built ffmpeg.exe is missing input device $Device"
    }
}

# Safety cleanup: keep only ffmpeg.exe and DLL files in _ffmpeg_build.
Get-ChildItem -Path $OutputDir -Force |
    Where-Object {
        $_.PSIsContainer -or
        ($_.Extension.ToLowerInvariant() -ne ".exe" -and $_.Extension.ToLowerInvariant() -ne ".dll")
    } |
    Remove-Item -Recurse -Force

Write-Host ""
Write-Host "Copied files:"
Get-ChildItem -Path $OutputDir -File -Include *.exe,*.dll |
    Sort-Object Name |
    Select-Object Name, Length |
    Format-Table -AutoSize

Write-Host ""
Write-Host "FFmpeg minimal shared build completed."
Write-Host "Keep ffmpeg.exe and all DLL files together."
Write-Host "Final output: $OutputDir"
param(
    [string]$Msys2Root = "C:\msys64",
    [string]$FfmpegSource = "",
    [string]$OutputDir = "",
    [string]$InstallPrefix = "",
    [int]$Jobs = 0,
    [switch]$SkipConfigure,
    [switch]$EnableSVTAV1,
    [switch]$EnableAOMAV1
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

# Build the Windows application manifest as a COFF resource object.
# The object is linked directly into ffmpeg.exe.
$ManifestBuildDir = Join-Path $RepoRoot "_ffmpeg_manifest"
$ManifestPath = Join-Path $ManifestBuildDir "ffmpeg.exe.manifest"
$ManifestRcPath = Join-Path $ManifestBuildDir "ffmpeg-manifest.rc"
$ManifestObjectPath = Join-Path $ManifestBuildDir "ffmpeg-manifest.o"
$Windres = Join-Path $Msys2Root "ucrt64\bin\windres.exe"

if (!(Test-Path $Windres)) {
    throw "MSYS2 windres was not found at $Windres. Install the mingw-w64-ucrt-x86_64-toolchain package."
}

New-Item -ItemType Directory -Force -Path $ManifestBuildDir | Out-Null

$ManifestXml = @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">
        PerMonitorV2
      </dpiAwareness>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">
        true/pm
      </dpiAware>
    </windowsSettings>
  </application>
</assembly>
'@

Save-TextUtf8NoBom -Path $ManifestPath -Text $ManifestXml

# windres reads the manifest path from the RC file.
# Forward slashes are used so that the absolute Windows path is parsed correctly.
$ManifestPathForRc = [System.IO.Path]::GetFullPath($ManifestPath).Replace("\", "/")

$ManifestRc = @"
1 24 "$ManifestPathForRc"
"@

Save-TextUtf8NoBom -Path $ManifestRcPath -Text $ManifestRc

Remove-Item -Force -ErrorAction SilentlyContinue $ManifestObjectPath

& $Windres `
    --input-format=rc `
    --output-format=coff `
    --target=pe-x86-64 `
    --input=$ManifestRcPath `
    --output=$ManifestObjectPath

if ($LASTEXITCODE -ne 0) {
    throw "Failed to compile the FFmpeg Windows manifest. windres exit code: $LASTEXITCODE"
}

if (!(Test-Path $ManifestObjectPath)) {
    throw "windres completed but the manifest object was not created: $ManifestObjectPath"
}

$ManifestObjectMsys = Convert-ToMsysPath $ManifestObjectPath

Write-Host "FFmpeg manifest created: $ManifestPath"
Write-Host "FFmpeg manifest resource object created: $ManifestObjectPath"

# Minimal shared/DLL FFmpeg build for rdagent.
$ConfigureArgs = @(
    "--prefix=$PrefixMsys",
    "--target-os=mingw32",
    "--arch=x86_64",

    "--enable-static",
    "--disable-shared",
    "--pkg-config-flags=--static",
    "--extra-ldflags='-static -static-libgcc -static-libstdc++ $ManifestObjectMsys'",

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

    "--enable-gpl",

    "--enable-d3d11va",
    "--enable-dxva2",
    "--enable-mediafoundation",
    "--enable-libvpx",
    "--enable-libx264",

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
    "--enable-encoder=libx264",

    "--enable-muxer=ivf",
    "--enable-muxer=h264",
    "--enable-protocol=pipe",

    "--enable-indev=lavfi",
    "--enable-indev=gdigrab"
)

if ($EnableSVTAV1.IsPresent) {
    $ConfigureArgs += @(
        "--enable-libsvtav1",
        "--enable-encoder=libsvtav1"
    )
}

if ($EnableAOMAV1.IsPresent) {
    $ConfigureArgs += @(
        "--enable-libaom",
        "--enable-encoder=libaom_av1"
    )
}

$UcrtLib = Join-Path $Msys2Root "ucrt64\lib"

$RequiredStaticLibs = @(
    "libvpx.a",
    "libx264.a",
    "libz.a"
)

if ($EnableSVTAV1.IsPresent) {
    $RequiredStaticLibs += "libSvtAv1Enc.a"
}

if ($EnableAOMAV1.IsPresent) {
    $RequiredStaticLibs += "libaom.a"
}

foreach ($Lib in $RequiredStaticLibs) {
    $LibPath = Join-Path $UcrtLib $Lib

    if (!(Test-Path $LibPath)) {
        throw "Required static library was not found: $LibPath"
    }

    Write-Host "Static library found: $Lib"
}

if ($EnableSVTAV1.IsPresent) {
    & $Bash -lc "PKG_CONFIG_PATH=/ucrt64/lib/pkgconfig pkg-config --exists --static SvtAv1Enc"
    if ($LASTEXITCODE -ne 0) {
        throw "pkg-config could not find static SvtAv1Enc. Check C:\msys64\ucrt64\lib\pkgconfig\SvtAv1Enc.pc"
    }

    Write-Host "pkg-config static package found: SvtAv1Enc"
}

if ($EnableAOMAV1.IsPresent) {
    & $Bash -lc "PKG_CONFIG_PATH=/ucrt64/lib/pkgconfig pkg-config --exists --static aom"
    if ($LASTEXITCODE -ne 0) {
        throw "pkg-config could not find static aom. Check C:\msys64\ucrt64\lib\pkgconfig\aom.pc"
    }

    Write-Host "pkg-config static package found: aom"
}

$ConfigureLine = "./configure " + ($ConfigureArgs -join " ")

# Keep the generated build script outside _ffmpeg_build too.
$BuildScriptPath = Join-Path $RepoRoot "build_local_ffmpeg_static.generated.sh"

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

mkdir -p "$OutputMsys"

# Final runtime directory must contain only ffmpeg.exe and required DLLs.
rm -rf "$OutputMsys"/*

cp -f "$PrefixMsys/bin/ffmpeg.exe" "$OutputMsys/ffmpeg.exe"

echo ""
echo "Configured linkage:"
grep -E '^CONFIG_STATIC=|^CONFIG_SHARED=' config.h || true

echo ""
echo "Configured encoders:"
grep -E 'CONFIG_(LIBVPX_VP8|H264_MF|AV1_MF|LIBSVTAV1|LIBAOM_AV1)_ENCODER' config.h || true

echo ""
echo "Configured capture/conversion components:"
grep -E 'CONFIG_(DDAGRAB|HWDOWNLOAD|SCALE|SCALE_D3D11|FORMAT|NULL|LUTRGB)_FILTER|CONFIG_SWSCALE|CONFIG_(D3D11VA|DXVA2|MEDIAFOUNDATION)' config.h || true
"@

Save-TextUtf8NoBom -Path $BuildScriptPath -Text $Script

Write-Host "Building minimal static FFmpeg from local source: $FfmpegSource"
Write-Host "Final output directory: $OutputDir"
Write-Host "Install prefix: $InstallPrefix"
Write-Host "SVT-AV1 enabled: $($EnableSVTAV1.IsPresent)"
Write-Host "libaom AV1 enabled: $($EnableAOMAV1.IsPresent)"
Write-Host "Dependencies: MSYS2 UCRT64 toolchain, nasm, pkgconf/pkg-config, make, libvpx, Windows SDK MediaFoundation headers/libs."
Write-Host "Generated MSYS2 build script: $BuildScriptPath"
Write-Host "Windows manifest: $ManifestPath"
Write-Host "Windows manifest object: $ManifestObjectPath"
Write-Host "Windows DPI awareness: PerMonitorV2"

& $Bash -lc "bash '$(Convert-ToMsysPath $BuildScriptPath)'"
if ($LASTEXITCODE -ne 0) {
    throw "FFmpeg build failed with exit code $LASTEXITCODE"
}

$BuiltExe = Join-Path $OutputDir "ffmpeg.exe"
if (!(Test-Path $BuiltExe)) {
    throw "Build finished but ffmpeg.exe was not found at $BuiltExe"
}

$Encoders = (& $BuiltExe -hide_banner -encoders) -join "`n"
foreach ($Encoder in @("libvpx", "libx264", "h264_mf", "av1_mf")) {
    if ($Encoders -notmatch [regex]::Escape($Encoder)) {
        throw "Built ffmpeg.exe is missing encoder $Encoder"
    }
}

if ($EnableSVTAV1.IsPresent) {
    if ($Encoders -notmatch [regex]::Escape("libsvtav1")) {
        throw "Built ffmpeg.exe is missing encoder libsvtav1"
    }
}

if ($EnableAOMAV1.IsPresent) {
    if ($Encoders -notmatch [regex]::Escape("libaom-av1")) {
        throw "Built ffmpeg.exe is missing encoder libaom-av1"
    }
}

$Devices = (& $BuiltExe -hide_banner -devices) -join "`n"

foreach ($Device in @("gdigrab", "lavfi")) {
    if ($Devices -notmatch [regex]::Escape($Device)) {
        throw "Built ffmpeg.exe is missing input device $Device"
    }
}

$LddOutput = (& $Bash -lc "ldd '$(Convert-ToMsysPath $BuiltExe)'") -join "`n"

Write-Host ""
Write-Host "Runtime dependencies:"
Write-Host $LddOutput

if ($LddOutput -match "msys|ucrt64|libgcc|libstdc\+\+|libwinpthread|libvpx|libx264|libaom|SvtAv1") {
    throw "ffmpeg.exe still depends on MSYS2/UCRT runtime DLLs. Some dependency was linked dynamically instead of statically."
}

# Safety cleanup: keep only ffmpeg.exe and DLL files in _ffmpeg_build.
Get-ChildItem -Path $OutputDir -Force |
    Where-Object {
        $_.PSIsContainer -or
        $_.Name -ne "ffmpeg.exe"
    } |
    Remove-Item -Recurse -Force

Write-Host ""
Write-Host "Copied files:"
Get-ChildItem -Path $OutputDir -File -Filter "ffmpeg.exe" |
    Select-Object Name, Length |
    Format-Table -AutoSize

if (!(Test-Path $ManifestPath)) {
    throw "FFmpeg build completed but the source manifest was not found: $ManifestPath"
}

if (!(Test-Path $ManifestObjectPath)) {
    throw "FFmpeg build completed but the compiled manifest object was not found: $ManifestObjectPath"
}

Write-Host ""
Write-Host "FFmpeg static single-exe build completed."
Write-Host "Final output: $BuiltExe"
Write-Host "Embedded manifest source: $ManifestPath"
Write-Host "Embedded DPI awareness: PerMonitorV2"
# Portable ZIP packaging for Video Wallpaper (M14).
# Produces: dist/VideoWallpaper-<tag>.zip containing:
#   VideoWallpaper.exe          (Release build)
#   msvcp140.dll / vcruntime140*.dll  (MSVC CRT - the only non-system deps)
#   LICENSE, README.md
# No debug builds, no PDBs, no test assets, no sample videos.
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$exe = Join-Path $root "build\release\Release\VideoWallpaper.exe"
if (-not (Test-Path $exe)) { Write-Error "Release exe not found: $exe - build Release first"; exit 1 }

# Tag from git (or fall back to a timestamp).
$tag = (git -C $root describe --tags --always 2>$null)
if (-not $tag) { $tag = "v1.0" }
$dist = Join-Path $root "dist"
$stage = Join-Path $dist "VideoWallpaper"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

Copy-Item $exe $stage

# MSVC CRT from the BuildTools redist (the only non-system dependencies,
# verified via the exe's import table: msvcp140/vcruntime140/vcruntime140_1).
$redist = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC"
$crt = Get-ChildItem $redist -Directory | Sort-Object Name -Descending |
       ForEach-Object { Join-Path $_.FullName "x64\Microsoft.VC143.CRT" } |
       Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $crt) { Write-Error "MSVC CRT redist not found under $redist"; exit 1 }
foreach ($dll in "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll") {
    $src = Join-Path $crt $dll
    if (-not (Test-Path $src)) { Write-Error "CRT DLL missing: $src"; exit 1 }
    Copy-Item $src $stage
}

Copy-Item (Join-Path $root "LICENSE") $stage
Copy-Item (Join-Path $root "README.md") $stage

# FFmpeg shared libraries (required for hardware decode)
$ffmpegBin = Join-Path $root "ext\ffmpeg\ffmpeg-N-126207-g21bbd98e7b-win64-gpl-shared\bin"
if (Test-Path $ffmpegBin) {
    foreach ($dll in Get-ChildItem $ffmpegBin -Filter "*.dll") {
        Copy-Item $dll.FullName $stage
    }
} else {
    Write-Warning "FFmpeg DLLs not found at $ffmpegBin — package will be incomplete"
}

$zip = Join-Path $dist "VideoWallpaper-$tag.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip

$exeSize = (Get-Item (Join-Path $stage "VideoWallpaper.exe")).Length
$zipSize = (Get-Item $zip).Length
$fileCount = (Get-ChildItem $stage -File).Count
Write-Output "PACKAGED: $zip"
Write-Output ("  files: $fileCount (exe $exeSize bytes; zip $zipSize bytes)")
Write-Output "  contents:"
Get-ChildItem $stage -File | ForEach-Object {
    Write-Output ("    {0,-22} {1} bytes" -f $_.Name, $_.Length)
}
Remove-Item $stage -Recurse -Force

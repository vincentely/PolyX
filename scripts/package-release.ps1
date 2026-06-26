param(
    [string]$BuildDir = "out/build/x64-release",
    [string]$PackageDir = "dist/PolyX-0.2.0-win64",
    [string]$Version = "0.2.0"
)

$ErrorActionPreference = "Stop"

$exePath = Join-Path $BuildDir "PolyX.exe"
if (-not (Test-Path $exePath)) {
    throw "Release executable not found: $exePath"
}

if (Test-Path $PackageDir) {
    Remove-Item $PackageDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $PackageDir | Out-Null

Copy-Item $exePath -Destination $PackageDir -Force

$readmePath = Join-Path $PackageDir "README.txt"
$readme = @"
PolyX $Version

Usage (drag and drop):
  Drag a PolyX manifest .json onto PolyX.exe.
  Output is written to an 'output' folder next to PolyX.exe:
    atlas.png       the merged atlas
    <FBXs>          meshes with UVs remapped into the atlas (mirrored layout)
    result.json     per-mesh status
  The window pauses at the end so you can read the summary.

Or from a command line:
  PolyX.exe path\to\manifest.json

The manifest is produced by the Unity exporter
(menu: Tools > PolyX > Manifest Exporter).

Notes:
  - Static build: does not require libfbxsdk.dll. GDI+ is provided by Windows.
"@
Set-Content -Path $readmePath -Value $readme -Encoding ASCII

$zipPath = "$PackageDir.zip"
if (Test-Path $zipPath) {
    Remove-Item $zipPath -Force
}

Compress-Archive -Path (Join-Path $PackageDir '*') -DestinationPath $zipPath -Force

Write-Host "Created package: $zipPath"

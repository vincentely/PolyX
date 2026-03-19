param(
    [string]$BuildDir = "out/release-static/Release",
    [string]$PackageDir = "dist/PolyX-0.1.0-win64",
    [string]$Version = "0.1.0"
)

$ErrorActionPreference = "Stop"

$exePath = Join-Path $BuildDir "PolyX.exe"
if (-not (Test-Path $exePath)) {
    throw "Release executable not found: $exePath"
}

New-Item -ItemType Directory -Force -Path $PackageDir | Out-Null

Copy-Item $exePath -Destination $PackageDir -Force

$readmePath = Join-Path $PackageDir "README.txt"
$readme = @"
PolyX $Version

Usage:
  1. Put your source assets under a root folder with `input` and `output` subfolders.
  2. Run `PolyX.exe <root-folder>` or launch it and enter the root folder path when prompted.
  3. Results will be written to `<root-folder>\output`.

Notes:
  - This package was built as a static release and does not require libfbxsdk.dll.
  - On Windows, GDI+ is provided by the operating system.
"@
Set-Content -Path $readmePath -Value $readme -Encoding ASCII

$zipPath = "$PackageDir.zip"
if (Test-Path $zipPath) {
    Remove-Item $zipPath -Force
}

Compress-Archive -Path (Join-Path $PackageDir '*') -DestinationPath $zipPath -Force

Write-Host "Created package: $zipPath"

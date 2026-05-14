# Sync new shim logs from %TEMP% into the repo's raw/ archive.
#
# Run after each Tech2Win SecurityAccess capture. Idempotent: re-running
# with no new logs is safe and prints "0 new". Filenames are preserved
# verbatim (cstech2win_shim_YYYYMMDD-HHMMSS.log) so a lex sort matches
# chronological order.
#
# Usage:
#   powershell -NoProfile -File shim\cstech2win\scripts\sync_raw_logs.ps1

$ErrorActionPreference = 'Stop'

$dest = Join-Path $PSScriptRoot '..\captures\raw'
if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest | Out-Null }
$dest = (Resolve-Path $dest).Path

$pattern = Join-Path $env:TEMP 'cstech2win_shim_*.log'
$srcFiles = Get-ChildItem -Path $pattern -ErrorAction SilentlyContinue

if (-not $srcFiles) {
    Write-Host "no cstech2win_shim_*.log files in $env:TEMP - nothing to sync"
    exit 0
}

$copied = 0
$skipped = 0
$newNames = @()
foreach ($f in $srcFiles) {
    $target = Join-Path $dest $f.Name
    if (Test-Path $target) {
        $skipped++
    } else {
        Copy-Item $f.FullName $target
        $copied++
        $newNames += $f.Name
    }
}

Write-Host "synced raw shim logs: $copied new, $skipped already present"
Write-Host "dest: $dest"
if ($copied -gt 0) {
    Write-Host ""
    Write-Host "new files:"
    $newNames | ForEach-Object { Write-Host "  $_" }
    Write-Host ""
    Write-Host "next step: cd to repo root, then run"
    Write-Host "  git add shim/cstech2win/captures/raw/"
    Write-Host "  git commit -m 'captures: sync raw shim logs'"
    Write-Host "  git push"
}

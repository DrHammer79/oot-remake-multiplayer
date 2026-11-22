# Player row 238 proof, both variants in one go (lane 1, kits as kit_pair.py setup made them):
#   proof-join-hearts-238.ps1 -KitA <kit-A> -KitB <kit-B>
# 1. hs-join-hearts-238 (rejoin after B gained a heart offline), 2. hs-join-fresh-238 (B's baseline save edited to 4
# hearts, B never joined: a fresh join). B's baseline is put back from a copy in a finally block. No PASS from here:
# the shots decide (read the run folders printed); exit 0 when both runs ended.
param(
    [Parameter(Mandatory)][string]$KitA,
    [Parameter(Mandatory)][string]$KitB,
    [int]$Timeout = 600,
    [string]$Server = ''
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not $Server) { $Server = [System.IO.Path]::GetFullPath((Join-Path $here '..\..\..\integrate-0112\build\server\coop_server.exe')) }
$repo = [System.IO.Path]::GetFullPath((Join-Path $here '..\..'))
$py = Join-Path $repo '..\tools-venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $py)) { $py = Join-Path $repo '..\oot-remake-recomp\.venv\Scripts\python.exe' }
$coopKit = Split-Path -Parent (Split-Path -Parent ([System.IO.Path]::GetFullPath($KitA)))
$runPs1 = Join-Path $here 'run.ps1'   # this checkout's own (its scenarios; run.ps1 finds coop-kit by walking up)
function Get-GameDir([string]$Kit) {
    $note = Join-Path $Kit 'hookshot\game-folder.txt'
    $g = if (Test-Path -LiteralPath $note) { "$(Get-Content -LiteralPath $note -TotalCount 1)".Trim() } else { 'hookshot\game' }
    if (-not [System.IO.Path]::IsPathRooted($g)) { $g = Join-Path $Kit $g }
    return [System.IO.Path]::GetFullPath($g)
}
function Invoke-Run([string]$Scenario) {
    Write-Host "== $Scenario"
    & powershell -NoProfile -ExecutionPolicy Bypass -File $runPs1 -Runtime hookshot -Lane 1 -Kit "$KitA,$KitB" -Scenario $Scenario -Timeout $Timeout -Server $Server
    Write-Host "== $Scenario exit $LASTEXITCODE"
}
$base = Join-Path (Get-GameDir $KitB) 'autotest-save'
$keep = Join-Path $coopKit 'proof-join-hearts-238-save'
Invoke-Run 'hs-join-hearts-238'
if (Test-Path -LiteralPath $keep) { Remove-Item -LiteralPath $keep -Recurse -Force }
Copy-Item -LiteralPath $base -Destination $keep -Recurse
try {
    foreach ($sav in Get-ChildItem -Path $base -Recurse -Filter progress.sav) {
        & $py -I (Join-Path $here 'save_flags.py') set $sav.FullName int:0xfbe01da1=16 int:0x31ab5580=16
        if ($LASTEXITCODE -ne 0) { throw "save_flags set failed on $($sav.FullName)" }
    }
    Invoke-Run 'hs-join-fresh-238'
} finally {
    Remove-Item -LiteralPath $base -Recurse -Force
    Copy-Item -LiteralPath $keep -Destination $base -Recurse
    Write-Host "B's save baseline put back"
}

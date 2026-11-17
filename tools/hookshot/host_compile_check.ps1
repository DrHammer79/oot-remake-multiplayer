# Compiles the Windows host code our Longshot patches touch with Longshot's own compiler (llvm-mingw's clang, as the
# kit's game build uses), syntax only: no game, no link, a few minutes. The CI windows job runs it, so a patch whose
# Windows-only host code does not compile (0.11.4's patch 0025: a '\\' that lost its backslash) fails there and
# never reaches a game build. The Linux jobs compile only the renderer (RX_VK_ONLY), and nothing else compiles
# host/*.c for Windows.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\hookshot\host_compile_check.ps1 [-Cache <folder>] [-Work <folder>]
#
# 1. Longshot's tools.lock of the release in tools\release\kit\hookshot\TOOLS.txt, its llvm-mingw and Python for
#    windows-x64, and the source archive of hookshot\PIN's commit: from the co-op mirror first, then Longshot's
#    servers, each checked against its SHA-256 (the kit's Get-KitDownload). -Cache keeps the downloads.
# 2. The source unpacked into -Work and hookshot\patches\*.patch applied with the kit's own apply_patch.py.
# 3. host\*.c that a patch changes, and host\*.c that include a host header a patch changes (host\posix is the
#    Linux build's): clang -fsyntax-only with build.py's host flags for Windows (nx2/build.py host_fl,
#    toolchain.native_target with llvm-mingw), the generated a64.h (nx2/rtgen.py) beside them. Unknown escape
#    sequences and multi-character constants are errors here (a lost backslash makes either).
# 4. Every .py a patch changes: compiled by Longshot's Python (py_compile).
# Exit status 0 only when every file compiled.
param(
    [string]$Cache = '',
    [string]$Work = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$kitScripts = Join-Path $repo 'tools\release\kit\scripts'
. (Join-Path $kitScripts 'kit-paths.ps1')   # (Get-KitDownload, Set-KitTls)
Set-KitTls
function Fail([string]$message) { Write-Host "host_compile_check: $message"; exit 1 }
function Read-KeyFile([string]$Path) {
    $map = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.+?)\s*$') { $map[$Matches[1]] = $Matches[2] }
    }
    return $map
}
function Expand-Zip([string]$Zip, [string]$To, [int]$Strip = 0, [string]$Member = '') {
    New-Item -ItemType Directory -Force -Path $To | Out-Null
    $tarArgs = @('-x', '-f', $Zip, '-C', $To)
    if ($Member) { $Strip = @($Member -split '/').Count }
    if ($Strip) { $tarArgs += "--strip-components=$Strip" }
    if ($Member) { $tarArgs += "$Member/" }
    & "$env:SystemRoot\System32\tar.exe" @tarArgs
    if ($LASTEXITCODE -ne 0) { Fail "unpacking $(Split-Path -Leaf $Zip) failed (tar $LASTEXITCODE)" }
}
function Get-Pinned([string[]]$Urls, [string]$To, [string]$Sha256) {
    $why = Get-KitDownload $Urls $To $Sha256 @()
    if ($why) { Fail "download of $(Split-Path -Leaf $To) failed: $why" }
}

$temp = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
if (-not $Cache) { $Cache = Join-Path $temp 'longshot-dl' }
if (-not $Work) { $Work = Join-Path $temp 'longshot-host-check' }
New-Item -ItemType Directory -Force -Path $Cache | Out-Null

$pin = Read-KeyFile (Join-Path $repo 'hookshot\PIN')
$toolsPin = Read-KeyFile (Join-Path $repo 'tools\release\kit\hookshot\TOOLS.txt')
if ($toolsPin.source_commit -ne $pin.commit) { Fail "TOOLS.txt pins the source of $($toolsPin.source_commit), hookshot\PIN says $($pin.commit)" }
if ($pin.repo -notmatch '^(https://git\.magicbean\.dev)/([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+?)(\.git)?$') { Fail "hookshot\PIN: repo=$($pin.repo) is not a git.magicbean.dev repository" }
$repoUrl = "$($Matches[1])/$($Matches[2])"
$apiUrl = "$($Matches[1])/api/v1/repos/$($Matches[2])"
$mirror = "$($toolsPin.mirror)".TrimEnd('/')
$patches = @(Get-ChildItem -LiteralPath (Join-Path $repo 'hookshot\patches') -Filter '*.patch' -File | Sort-Object Name)
if (-not $patches) { Fail 'no hookshot\patches\*.patch' }

# --- 1: tools.lock, llvm-mingw, Python, the source -------------------------------------------
$lock = Join-Path $Cache "tools-$($toolsPin.release).lock"
Get-Pinned @("$mirror/tools.lock", "$repoUrl/releases/download/$($toolsPin.release)/tools.lock") $lock $toolsPin.lock_sha256
$parts = @{}
foreach ($line in Get-Content -LiteralPath $lock) {
    $f = $line.Trim() -split '\s+'
    if ($f[0] -ne 'tool' -or $f.Count -lt 9 -or $f[3] -ne 'windows-x64' -or @('llvm-mingw', 'python') -notcontains $f[1]) { continue }
    $sha = $f[4].ToLowerInvariant()
    $urls = @("$mirror/$sha-$($f[1]).zip") + @($f[8..($f.Count - 1)] | Where-Object { $_ -match '^https://' })
    $parts[$f[1]] = [pscustomobject]@{ Sha = $sha; Prefix = $f[7]; Urls = $urls; File = (Join-Path $Cache "$($f[1])-$($sha.Substring(0, 12)).zip") }
}
foreach ($n in 'llvm-mingw', 'python') {
    if (-not $parts[$n]) { Fail "tools.lock of $($toolsPin.release) has no windows-x64 $n" }
    Get-Pinned $parts[$n].Urls $parts[$n].File $parts[$n].Sha
}
$sourceZip = Join-Path $Cache "longshot-$($pin.commit).zip"
Get-Pinned @("$mirror/longshot-$($pin.commit).zip", "$apiUrl/archive/$($pin.commit).zip") $sourceZip $toolsPin.source_sha256

# --- 2: the patched source ------------------------------------------------------------------
if (Test-Path -LiteralPath $Work) { Remove-Item -LiteralPath $Work -Recurse -Force }
$src = Join-Path $Work 'src'
Write-Host '  unpacking ...'
Expand-Zip $sourceZip $src 1
foreach ($n in 'llvm-mingw', 'python') {
    $member = if ($parts[$n].Prefix -eq '-') { '' } else { $parts[$n].Prefix.TrimEnd('/') }
    Expand-Zip $parts[$n].File (Join-Path $Work $n) -Member $member
}
$python = Join-Path $Work 'python\python.exe'
$clang = Join-Path $Work 'llvm-mingw\bin\clang.exe'
foreach ($p in $python, $clang, (Join-Path $src 'nx2\rtgen.py')) { if (-not (Test-Path -LiteralPath $p)) { Fail "no $p after unpacking" } }
Write-Host "  applying $($patches.Count) co-op patch(es) ..."
& $python -I -B (Join-Path $kitScripts 'apply_patch.py') $src @($patches | ForEach-Object FullName)
if ($LASTEXITCODE -ne 0) { Fail "the co-op patches do not apply to Longshot $($pin.commit)" }

# --- 3: the host C files --------------------------------------------------------------------
$touched = @{}
foreach ($p in $patches) {
    foreach ($line in Get-Content -LiteralPath $p.FullName) {
        if ($line -match '^\+\+\+ b/(.+?)\s*$') { $touched[$Matches[1]] = $true }
    }
}
$hostDir = Join-Path $src 'host'
$headers = @($touched.Keys | Where-Object { $_ -match '^(host|gpu/vk)/[^/]+\.h$' } | ForEach-Object { Split-Path -Leaf $_ })
$cFiles = @{}
foreach ($t in $touched.Keys) { if ($t -match '^host/[^/]+\.c$') { $cFiles[(Split-Path -Leaf $t)] = $true } }
foreach ($c in Get-ChildItem -LiteralPath $hostDir -Filter '*.c' -File) {
    $text = [System.IO.File]::ReadAllText($c.FullName)
    foreach ($h in $headers) {
        if ($text -match ('#\s*include\s*"([^"]*/)?' + [regex]::Escape($h) + '"')) { $cFiles[$c.Name] = $true; break }
    }
}
$rt64 = Join-Path $Work 'rt64'
$gen = Join-Path $Work 'gen'
New-Item -ItemType Directory -Force -Path $gen | Out-Null
& $python -I -B -c "import sys; sys.path.insert(0, sys.argv[1]); import rtgen; rtgen.write(sys.argv[2])" (Join-Path $src 'nx2') $rt64
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $rt64 'a64.h'))) { Fail 'nx2/rtgen.py did not write a64.h' }
# (nx2/build.py: host_fl = target() + CPU + -I host, rt64, gen, -Wno-everything, -mcx16, -DA64_POLLING, WRITE_TRACK; and opus's
# include for every host file, zstd's for ZSTD_HOST. target() is toolchain.native_target() with llvm-mingw.)
$flags = @('--target=x86_64-w64-mingw32', '-fms-extensions', '-D_CRT_SECURE_NO_WARNINGS', '-march=x86-64-v3', '-ffp-contract=off',
    "-I$hostDir", "-I$rt64", "-I$gen", "-I$(Join-Path $src 'third_party\opus\include')", '-mcx16', '-DA64_POLLING', '-DA64_WRITE_TRACK',
    '-fsyntax-only', '-Wno-everything', '-Wunknown-escape-sequence', '-Werror=unknown-escape-sequence', '-Wmultichar', '-Werror=multichar')
$zstdHost = @('prompts.c', 'hos_zstd.c')
$failed = @()
# (Windows PowerShell 5.1 turns a native program's stderr under 2>&1 into error records: Stop would end the script at
# the first warning instead of listing every file)
$ErrorActionPreference = 'Continue'
foreach ($name in ($cFiles.Keys | Sort-Object)) {
    $extra = if ($zstdHost -contains $name) { @("-I$(Join-Path $src 'third_party\zstd\lib')") } else { @() }
    $out = & $clang @flags @extra (Join-Path $hostDir $name) 2>&1
    if ($LASTEXITCODE -ne 0) {
        $failed += "host/$name"
        Write-Host "FAIL host/$name"
        $out | ForEach-Object { Write-Host "  $_" }
    } else {
        Write-Host "ok   host/$name"
    }
}

# --- 4: the patched Python files ------------------------------------------------------------
foreach ($t in ($touched.Keys | Where-Object { $_ -match '\.py$' } | Sort-Object)) {
    $out = & $python -I -B -c "import sys, py_compile; py_compile.compile(sys.argv[1], cfile=sys.argv[2], doraise=True)" (Join-Path $src $t) (Join-Path $Work 'py.pyc') 2>&1
    if ($LASTEXITCODE -ne 0) {
        $failed += $t
        Write-Host "FAIL $t"
        $out | ForEach-Object { Write-Host "  $_" }
    } else {
        Write-Host "ok   $t"
    }
}

if ($failed) { Fail "$($failed.Count) file(s) did not compile: $($failed -join ', ')" }
Write-Host "host_compile_check: $($cFiles.Count) host C file(s) and the patched Python files compile for Windows"

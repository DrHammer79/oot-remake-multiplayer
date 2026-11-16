# One Fritchii run of the ExeFS module (subsdk1), whole PC, time boxed. docs/exefs.md, "Run harness".
#
#   powershell -ExecutionPolicy Bypass -File tools\switch_mod\run_fritchii.ps1 -Id <TEST-TURN id> -Preset proto|nullguards|codemem [-Sha <commit>]
#
# Refuses to start unless TEST-TURN's LANE1 names -Id (LANE2 names it too or is CLOSED), no game or server runs,
# no build/lane lock exists, and no saves backup or old install is left from an earlier run. Takes the lane1 and
# lane2 locks (tools/autotest/lanes.py) and build.lock for the seconds of the module build. Saves are backed up and
# restored in `finally` (the backup is deleted only after a hash-checked restore); the mod folder is removed after.
# Screenshots use PrintWindow only (the window's own content, even when covered); no screen copy ever. Verdict: tools/switch_mod/run_check.py (PASS/FAIL per check, RESULT line).
param(
    [Parameter(Mandatory = $true)][string]$Id,
    [ValidateSet('proto', 'nullguards', 'codemem')][string]$Preset = 'nullguards',
    [string]$Sha = '',
    [int]$Seconds = 0,                     # game time; 0 = the preset's default
    [string]$Scenario = '',                # LICHII_COOP_TEST_SCRIPT; default scenarios\play.txt
    [string]$Kit = '',                     # default <workspace>\coop-kit\auto-A\emulator
    [string]$Game = '',                    # default the one .nup in <workspace> (your own dump)
    [string]$Python = '',                  # default <workspace>\oot-remake-recomp\.venv\Scripts\python.exe
    [string]$Out = '',                     # default coop-kit\shots\exefs-<preset>
    [string]$TestTurn = ''                 # default <workspace>\coord-multiplayer\TEST-TURN
)
$ErrorActionPreference = 'Stop'
$here   = $PSScriptRoot
$wt     = (Resolve-Path "$here\..\..").Path                 # the checkout this script is in: it builds that one
$main   = Split-Path -Parent (git -C $wt rev-parse --path-format=absolute --git-common-dir).Trim()   # main checkout: locks live there
$lanes  = "$main\tools\autotest\lanes.py"
$root   = Split-Path -Parent $main                          # the workspace folder that holds the checkout and the kits
if (-not $Kit) { $Kit = "$root\coop-kit\auto-A\emulator" }
if (-not $Python) { $Python = "$root\oot-remake-recomp\.venv\Scripts\python.exe" }
if (-not $TestTurn) { $TestTurn = "$root\coord-multiplayer\TEST-TURN" }
if (-not $Game) {
    $nups = @(Get-ChildItem -LiteralPath $root -Filter *.nup -File -ErrorAction SilentlyContinue)
    if ($nups.Count -ne 1) { Write-Output "FAIL start: pass -Game (found $($nups.Count) .nup files in $root)"; exit 2 }
    $Game = $nups[0].FullName
}
if (-not $Seconds) { $Seconds = @{ proto = 190; nullguards = 240; codemem = 200 }[$Preset] }
if (-not $Scenario) { $Scenario = "$here\scenarios\play.txt" }
if (-not $Out) { $Out = (Resolve-Path "$Kit\..\..").Path + "\shots\exefs-$Preset" }
$datagrams = @{ proto = 3; nullguards = 0; codemem = 0 }[$Preset]
$saves  = "$Kit\user\nand\user\save\0000000000000000"
$bak    = "$Out\saves-backup"
$modDir = "$Kit\user\load\0400486024DEC000\coop-proto"
$log    = "$Kit\user\log\eden_log.txt"
$owner  = "run_fritchii $Id (pid $PID)"

function Fail-Start($why) { Write-Output "FAIL start: $why"; exit 2 }

# ---- Refuse to start unless the PC is ours and clean ----
$t = Get-Content $TestTurn
$idRe = "\b$([regex]::Escape($Id))\b"
if (-not ($t | Where-Object { $_ -match "^LANE1:" -and $_ -match $idRe }) -or
    -not ($t | Where-Object { ($_ -match "^LANE2:" -and $_ -match $idRe) -or $_ -match '^LANE2:\s*CLOSED' })) {
    Fail-Start "TEST-TURN's LANE1 line must name $Id and LANE2 must name it too or be CLOSED (whole PC)"
}
if (Get-Process lichii, longshot, hookshot, eden, yuzu, coop_server -ErrorAction SilentlyContinue) { Fail-Start 'a game or server is running' }
foreach ($l in 'build.lock', 'build-game.lock', 'build-hs.lock', 'lane1.lock', 'lane2.lock') {
    if (Test-Path "$main\$l") { Fail-Start "$l exists (someone holds it; never take or delete it)" }
}
foreach ($f in $Kit, $Game, $Python, $lanes, $Scenario) { if (-not (Test-Path -LiteralPath $f)) { Fail-Start "missing: $f" } }
if (Test-Path $bak) { Fail-Start "$bak exists: an earlier run may not have restored the kit's saves; restore by hand, then delete it" }
if (Test-Path $modDir) { Fail-Start "$modDir exists: remove the old install first" }
if ($Sha) {
    $head = (git -C $wt rev-parse HEAD).Trim()
    if (-not $head.StartsWith($Sha)) { Fail-Start "checkout is at $head, not $Sha" }
}
New-Item -ItemType Directory -Force $Out | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type -Namespace RunFritchii -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool PrintWindow(System.IntPtr h, System.IntPtr dc, uint flags);
public struct RECT { public int L, T, R, B; }
'@ -ErrorAction SilentlyContinue

# Mostly-black image? (PrintWindow on a GPU window can give black.) Samples a grid.
function Test-Blank($bmp) {
    $lit = 0
    for ($y = 1; $y -lt 8; $y++) { for ($x = 1; $x -lt 8; $x++) {
        $c = $bmp.GetPixel([int]($bmp.Width * $x / 8), [int]($bmp.Height * $y / 8))
        if ($c.R + $c.G + $c.B -gt 30) { $lit++ }
    } }
    return $lit -lt 3
}
# Privacy: a shot only ever holds the game window's own pixels. PrintWindow asks the window itself for its content,
# so nothing on the screen (another window, a topmost one, the desktop) can get in. There is NO screen copy: when
# PrintWindow fails or gives a black image, the shot is skipped and the line says why. (run_check_test.py fails if
# a screen-copy call appears in this file.)
function Shot($proc, $name) {
    try {
        $proc.Refresh(); $h = $proc.MainWindowHandle
        if ($h -eq [IntPtr]::Zero) { Write-Output "shot ${name}: skipped (no window)"; return }
        $r = New-Object RunFritchii.Win+RECT; [RunFritchii.Win]::GetWindowRect($h, [ref]$r) | Out-Null
        $w = $r.R - $r.L; $hh = $r.B - $r.T; if ($w -le 0 -or $hh -le 0) { Write-Output "shot ${name}: skipped (window has no size)"; return }
        $bmp = New-Object System.Drawing.Bitmap $w, $hh
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $dc = $g.GetHdc(); $ok = [RunFritchii.Win]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)   # 2 = PW_RENDERFULLCONTENT
        $g.Dispose()
        if (-not $ok) { $bmp.Dispose(); Write-Output "shot ${name}: skipped (PrintWindow failed)"; return }
        if (Test-Blank $bmp) { $bmp.Dispose(); Write-Output "shot ${name}: skipped (PrintWindow gave a black image)"; return }
        $bmp.Save("$Out\$name.png"); $bmp.Dispose()
        Write-Output "shot ${name}: saved (PrintWindow)"
    } catch { Write-Output "shot ${name}: skipped ($_)" }
}

# Relative path + size + SHA-256 of every file under a folder, for comparing a copy with its source.
function Get-Tree($dir) {
    $base = (Resolve-Path -LiteralPath $dir).Path
    @(Get-ChildItem -LiteralPath $base -Recurse -File | Sort-Object FullName | ForEach-Object {
        "$($_.FullName.Substring($base.Length))|$($_.Length)|$((Get-FileHash -LiteralPath $_.FullName).Hash)" })
}

function Same-Tree($a, $b) { ((Get-Tree $a) -join "`n") -ceq ((Get-Tree $b) -join "`n") }

$held = @(); $p = $null; $listen = $null; $backedUp = $false; $alive = 0
try {
    foreach ($n in 1, 2) {
        & $Python -I $lanes lock acquire --lane $n --owner $owner --pid $PID --wait 60
        if ($LASTEXITCODE -ne 0) { throw "lane $n lock not taken" }
        $held += $n
    }
    New-Item -ItemType Directory "$main\build.lock" | Out-Null
    try {
        Set-Content "$main\build.lock\owner.txt" $owner -Encoding utf8
        & $Python "$here\build.py"
        if ($LASTEXITCODE -ne 0) { throw 'module build failed' }
    } finally { Remove-Item -Recurse -Force "$main\build.lock" }

    # The kit's own co-op hook also rewrites main+0x6e70f8: keep it off (environment only, no kit file changes).
    foreach ($v in 'LICHII_COOP_PUPPET', 'LICHII_COOP_DEBUG_PUPPET', 'LICHII_COOP_SPAWN', 'LICHII_COOP_LIVE_EVENTS',
                   'LICHII_COOP_OBJECTS_AI_HOLD') { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }

    # Back up the saves and check the copy (file names + hashes) before anything runs; a bad copy stops the run
    # with the kit's saves untouched (and the restore in finally never uses it).
    Copy-Item -Recurse $saves $bak
    if (-not (Same-Tree $saves $bak)) {
        Remove-Item -Recurse -Force $bak   # the kit's saves were not touched; a bad copy is no backup
        throw "the saves backup did not match the saves (removed it): not started"
    }
    $backedUp = $true
    # Only this run's log may be judged: move the kit's old log and this folder's old results away first.
    if (Test-Path $log) { Move-Item $log "$Out\eden_log.before.txt" -Force }
    if (Test-Path $log) { throw "could not move the old $log away: not started" }
    foreach ($f in 'eden_log.txt', 'listen.txt', 'listen-err.txt', 'title.png', 'playing.png', 'end.png') {
        Remove-Item "$Out\$f" -Force -ErrorAction SilentlyContinue
        if (Test-Path "$Out\$f") { throw "could not remove the old $Out\${f}: not started" }
    }
    New-Item -ItemType Directory -Force "$modDir\exefs" | Out-Null
    Copy-Item "$wt\build\switch-proto\subsdk1" "$modDir\exefs\subsdk1" -Force

    if ($datagrams) {
        $listen = Start-Process $Python -ArgumentList "-I `"$here\listen.py`" --seconds $($Seconds + 20)" -PassThru `
            -WindowStyle Minimized -RedirectStandardOutput "$Out\listen.txt" -RedirectStandardError "$Out\listen-err.txt"
    }
    $env:LICHII_COOP_TEST_SCRIPT = $Scenario
    $p = Start-Process "$Kit\lichii.exe" -ArgumentList "-g `"$Game`"" -WorkingDirectory $Kit -PassThru
    $start = Get-Date
    $marks = @(@{ at = 50; n = 'title' }, @{ at = [int]($Seconds * 0.6); n = 'playing' }, @{ at = $Seconds; n = 'end' })
    foreach ($m in $marks) {
        while (((Get-Date) - $start).TotalSeconds -lt $m.at -and -not $p.HasExited) { Start-Sleep 2 }
        if ($p.HasExited) { Write-Output "game exited early (code $($p.ExitCode))"; break }
        Shot $p $m.n
    }
    if (-not $p.HasExited) { $alive = 1 }
} catch {
    Write-Output "ERROR: $_"
} finally {
    # Each step on its own: one failing never skips the others. Saves first.
    try { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(20000) | Out-Null } } catch { Write-Output "stop game: $_" }
    try { if ($listen -and -not $listen.HasExited) { $listen.WaitForExit(30000) | Out-Null; if (-not $listen.HasExited) { Stop-Process -Id $listen.Id -Force } } } catch { Write-Output "stop listener: $_" }
    Remove-Item Env:LICHII_COOP_TEST_SCRIPT -ErrorAction SilentlyContinue
    try { if (Test-Path $log) { Copy-Item $log "$Out\eden_log.txt" -Force } } catch { Write-Output "copy log: $_" }
    if ($backedUp) {
        try {
            if (Test-Path $saves) { Remove-Item -Recurse -Force $saves }
            Copy-Item -Recurse $bak $saves
            if (-not (Same-Tree $bak $saves)) { Write-Output "SAVES NOT VERIFIED: backup kept at $bak" }
            else { Remove-Item -Recurse -Force $bak; Write-Output 'saves restored and verified' }
        } catch { Write-Output "SAVES RESTORE FAILED ($_): backup kept at $bak" }
    }
    try { if (Test-Path $modDir) { Remove-Item -Recurse -Force $modDir } } catch { Write-Output "MOD NOT REMOVED ($_): delete $modDir by hand" }
    foreach ($n in $held) { try { & $Python -I $lanes lock release --lane $n --owner $owner | Out-Null } catch { Write-Output "lane $n lock release: $_" } }
}

& $Python -I "$here\run_check.py" --log "$Out\eden_log.txt" --preset $Preset --listen "$Out\listen.txt" --alive $alive
exit $LASTEXITCODE

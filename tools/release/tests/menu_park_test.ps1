# Tests for the kit start's size-table step without a working Python (player report row 235):
# Invoke-KitSizeTableMerge / Move-KitMenuFilesAside in tools/release/kit/scripts/kit-paths.ps1.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\menu_park_test.ps1
# Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'kit\scripts\kit-paths.ps1')

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}

$rel = 'Mals/EUen.Nin_NX_NVN.zs'
$res = 'System\Resource'
$table = 'ResourceSizeTable.Product.100.rsizetable.zs'

# A fresh exe folder: the pack (its menu file, manifest, own size table), a kit with merge_tables.py; $WithOther adds
# Character Select with a size table of the same name.
function New-Tree([bool]$WithOther) {
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-menupark-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    $pack = Join-Path $root 'exe\mods\contents\0400486024DEC000\coop-puppet'
    New-Item -ItemType Directory -Force -Path (Join-Path $pack 'romfs\Mals'), (Join-Path $pack "romfs\$res"), (Join-Path $root 'kit\tools\puppet_pack') | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $pack 'romfs\Mals\EUen.Nin_NX_NVN.zs'), 'menu')
    [System.IO.File]::WriteAllText((Join-Path $pack "romfs\$res\$table"), 'pack table')
    [System.IO.File]::WriteAllText((Join-Path $pack 'coop_puppet.json'), "{`"title_menu`": [`"$rel`"], `"pause_menu`": []}")
    [System.IO.File]::WriteAllText((Join-Path $root 'kit\tools\puppet_pack\merge_tables.py'), '')
    if ($WithOther) {
        $cs = Join-Path $root 'exe\mods\OoT Character Select\romfs\System\Resource'
        New-Item -ItemType Directory -Force -Path $cs | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $cs $table), 'cs table')
    }
    return [pscustomobject]@{ Root = $root; Kit = (Join-Path $root 'kit'); Exe = (Join-Path $root 'exe'); Pack = $pack }
}
function In-Play($t) { Test-Path -LiteralPath (Join-Path $t.Pack 'romfs\Mals\EUen.Nin_NX_NVN.zs') }
function Kept($t) { Test-Path -LiteralPath (Join-Path $t.Pack '_coop-menus\Mals\EUen.Nin_NX_NVN.zs') }

$made = @()
try {
    # 1. No Python, another mod has a size table: the menu file goes aside, the line says so.
    $t = New-Tree $true; $made += $t
    $out = @(Invoke-KitSizeTableMerge $t.Kit $t.Exe)
    Check ((-not (In-Play $t)) -and (Kept $t)) 'no Python + another size table: the menu file is kept aside'
    Check (($out -join ' ') -match 'kept aside') 'the window says so'

    # 2. No Python, the pack's table is the only one: nothing moves.
    $t = New-Tree $false; $made += $t
    $out = @(Invoke-KitSizeTableMerge $t.Kit $t.Exe)
    Check ((In-Play $t) -and -not (Kept $t)) 'no Python, pack only: the menus stay'

    # 3. A "Python" that dies before main (a Traceback, exit 1): same as 1.
    $fake = Join-Path $env:TEMP ('oot-coop-fakepy-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.cmd')
    [System.IO.File]::WriteAllText($fake, "@echo off`r`necho Traceback (most recent call last):`r`necho ImportError: nope`r`nexit /b 1`r`n")
    $t = New-Tree $true; $made += $t
    $out = @(Invoke-KitSizeTableMerge $t.Kit $t.Exe $fake)
    Check ((-not (In-Play $t)) -and (Kept $t)) 'Python dies before main: the menu file is kept aside'
    $t = New-Tree $false; $made += $t
    $null = @(Invoke-KitSizeTableMerge $t.Kit $t.Exe $fake)
    Check ((In-Play $t) -and -not (Kept $t)) 'Python dies before main, pack only: the menus stay'

    # 4. A "Python" that says its own Menus line and exits 1 (the merge ran and reported): nothing more is moved here.
    $fake2 = Join-Path $env:TEMP ('oot-coop-fakepy2-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) + '.cmd')
    [System.IO.File]::WriteAllText($fake2, "@echo off`r`necho Menus: said by merge_tables.py`r`nexit /b 1`r`n")
    $t = New-Tree $true; $made += $t
    $null = @(Invoke-KitSizeTableMerge $t.Kit $t.Exe $fake2)
    Check (In-Play $t) 'Python that reported its own Menus line: the kit parks nothing on top'
    Remove-Item -LiteralPath $fake, $fake2 -Force -ErrorAction SilentlyContinue
} finally {
    foreach ($t in $made) { Remove-Item -LiteralPath $t.Root -Recurse -Force -ErrorAction SilentlyContinue }
}

if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'all checks passed'

# Tests for the kit's crash reports and report sending (tools/release/kit/scripts/send-report.ps1): how a session's log
# ended (Longshot's own crash and exit lines), the crash report's zip and its masking, each session looked at once, the
# relay file (the kill switch), send_reports in coop-options.ini, and a whole start with send_reports = yes posting the
# zip to a listener on 127.0.0.1 (no other network). Made-up log lines only.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\send_report_test.ps1
# Exits 1 when a check fails.
$ErrorActionPreference = 'Stop'
$scripts = Join-Path (Split-Path -Parent $PSScriptRoot) 'kit\scripts'
. (Join-Path $scripts 'log-mask.ps1')
. (Join-Path $scripts 'kit-paths.ps1')
. (Join-Path $scripts 'problem-report.ps1')
. (Join-Path $scripts 'send-report.ps1')
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}
$utf8 = New-Object System.Text.UTF8Encoding $false
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('send-report-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null

function New-Log([string]$Path, [string[]]$Lines, [int]$Filler = 0, [datetime]$When = (Get-Date)) {
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append("[hos host] longshot v3.0.0-rc.6 starting, run $([guid]::NewGuid())`r`n")
    [void]$sb.Append("[hos sead::MainThread] coop: host: player 2 joined: Mido`r`n")
    for ($i = 0; $i -lt $Filler; $i++) { [void]$sb.Append("[hos host] vk: a frame: 0.00 MB unswizzled on the CPU in 0.00 ms ($i)`r`n") }
    foreach ($l in $Lines) { [void]$sb.Append($l + "`r`n") }
    [System.IO.File]::WriteAllText($Path, $sb.ToString(), $utf8)
    (Get-Item -LiteralPath $Path).LastWriteTime = $When
    $Path
}
function Get-End([string]$Path) {
    $s = Open-ProblemLog $Path
    try { Get-LogSessionEnd $s } finally { $s.Dispose() }
}
function Read-ZipText([string]$Zip) {
    $a = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $e = $a.Entries | Where-Object { $_.FullName -eq 'crash-report.txt' }
        $r = New-Object System.IO.StreamReader($e.Open())
        try { $r.ReadToEnd() } finally { $r.Dispose() }
    } finally { $a.Dispose() }
}

$crashLines = @(
    '[hos sead::MainThread] coop: on (from the menu), server myhost.duckdns.org:27510, name ''Saria'', join password set',
    '[hos ModuleSystemWorker2] CRASH: exception 0xc0000005 (read 0x100000000150, guest 0x150) at longshot.exe+0x12c2daa6',
    '[hos ModuleSystemWorker2] CRASH: host backtrace: longshot.exe+0x12c2daa6 longshot.exe+0x1964de9b',
    '[hos ModuleSystemWorker2] CRASH: thread ModuleSystemWorker2, sp 0x10075e30a0',
    '[hos ModuleSystemWorker2] CRASH:   x0  00000010073d8348  x1  00000010075e3648  x2  000000001d6189da  x3  00000000796246ea',
    '[hos host] loaded C:\Users\Testuser\Squadala\game\hookshot_coop.dll; Mido at 192.168.1.20',
    '[hos ModuleSystemWorker2] CRASH: guest backtrace: lr 0x8076d954 0x8076d954 0x8076d708')

# --- How a session ended (Get-LogSessionEnd) ---
$d = Join-Path $tmp 'end'; New-Item -ItemType Directory -Path $d | Out-Null
$e = Get-End (New-Log "$d\crash.txt" $crashLines)
Check ($e.Kind -eq 'crash') "CRASH: exception = crash (got $($e.Kind))"
Check ($e.Crash.Count -eq 5 -and $e.Crash[0] -match 'CRASH: exception' -and $e.Crash[-1] -match 'guest backtrace') "the crash lines, from the exception on (got $($e.Crash.Count))"
Check ((Get-End (New-Log "$d\fatal.txt" @('[hos sead::MainThread] FATAL: the game aborted (nn::diag::detail::AbortImpl) (lr 0x8351c290 sp 0x1100930)', '[hos] guest backtrace: lr 0x8351c290'))).Kind -eq 'crash') 'FATAL: (the game aborted) = crash'
Check ((Get-End (New-Log "$d\vk.txt" @('vk: FATAL: the GPU device was lost (VK_ERROR_DEVICE_LOST): the driver stopped'))).Kind -eq 'crash') 'vk: FATAL: (device lost) = crash'
Check ((Get-End (New-Log "$d\quit.txt" @('[hos host] control: quit asked by a client', '[hos host] control: exiting'))).Kind -eq 'clean') 'control: exiting = clean'
Check ((Get-End (New-Log "$d\closed.txt" @('[hos host] display: window closed, exiting', '[hos AudioFrameworkThread] vcores: 1 takes'))).Kind -eq 'clean') 'window closed, exiting (a line after it) = clean'
Check ((Get-End (New-Log "$d\restart.txt" @('[hos host] display: window closed for a restart (the Insert menu''s Restart now)'))).Kind -eq 'clean') 'window closed for a restart = clean'
Check ((Get-End (New-Log "$d\cut.txt" @('[hos host] vk: imports: 3 live'))).Kind -eq 'unclean') 'no exit line, no crash = unclean'
Check ((Get-End (New-Log "$d\words.txt" @('[hos host] coop: the crash handler is on', '[hos host] control: exiting'))).Kind -eq 'clean') 'a line that only mentions a crash is not one'
$big = Get-End (New-Log "$d\big.txt" @('[hos host] control: exiting') 12000)
Check ($big.Kind -eq 'clean' -and $big.Tail.Count -eq $script:ReportTailLines -and $big.Tail[-1] -match 'control: exiting') "a long log: its end only, $script:ReportTailLines lines (got $($big.Tail.Count))"

# --- Crash reports (New-CrashReports): masked, each session once, newest few, recent only ---
$kit = Join-Path $tmp 'kit'; $logs = Join-Path $kit 'logs'
New-Item -ItemType Directory -Path $logs -Force | Out-Null
$now = Get-Date
$l1 = New-Log "$logs\longshot_log.1.txt" $crashLines 9000 $now.AddMinutes(-30)   # (the name line is far before the end)
$l2 = New-Log "$logs\longshot_log.2.txt" @('[hos host] control: exiting') 0 $now.AddHours(-2)
$l3 = New-Log "$logs\longshot_log.3.txt" @('[hos host] vk: imports: 3 live') 0 $now.AddHours(-3)
$l4 = New-Log "$logs\longshot_log.4.txt" @('[hos host] vk: imports: 3 live') 0 $now.AddDays(-9)
$state = @{ checked = @() }
$made = @(New-CrashReports @($l1, $l2, $l3, $l4) $state $logs 'v0.11.4' 'Testuser' 'TESTPC-01' $now)
Check ($made.Count -eq 1 -and $made[0].Kind -eq 'crash') "newest $script:ReportLogsPerStart looked at: the crash has a report, the clean one none (got $($made.Count))"
Check ($state.checked.Count -eq 3) "looked at or too old: marked (the 3rd newest waits for the next start; got $($state.checked.Count))"
$text = Read-ZipText $made[0].Zip
Check ($made[0].Zip -match '\\crash-report-\d{8}-\d{6}-crash\.zip$') "logs\crash-report-<date>-crash.zip (got $(Split-Path -Leaf $made[0].Zip))"
Check ($text -match 'Kit: v0\.11\.4' -and $text -match 'the game crashed' -and $text -match 'Log: longshot_log\.1\.txt') 'the report names the kit, the kind and the log'
Check ($text -match 'CRASH: exception 0xc0000005' -and $text -match 'guest backtrace') 'the crash lines are in'
Check ($text -match 'player 2 joined: <name>' -and $text -match '<name> at <lan-1>' -and $text -notmatch 'Mido') 'a name learnt at the log''s start is masked at its end'
Check ($text -match "name '<name>'" -and $text -notmatch 'Saria') "the own name is masked"
Check ($text -match 'server <host-1>:27510' -and $text -notmatch 'duckdns') 'the host name is masked'
Check ($text -match 'C:\\Users\\<user>\\' -and $text -notmatch 'Testuser' -and $text -notmatch '192\.168') 'user name and address masked'
$again = @(New-CrashReports @($l1, $l2, $l3, $l4) $state $logs 'v0.11.4' 'Testuser' 'TESTPC-01' $now)
Check ($again.Count -eq 1 -and $again[0].Kind -eq 'unclean') "next start: only the one left, an unclean end (got $($again.Count) $($again.Kind))"
Check ((Read-ZipText $again[0].Zip) -match 'ended without closing') 'the unclean report says so'
Check (@(New-CrashReports @($l1, $l2, $l3, $l4) $state $logs 'v0.11.4' 'Testuser' 'TESTPC-01' $now).Count -eq 0) 'then nothing new'
$held = [System.IO.File]::Open((New-Log "$logs\longshot_log.5.txt" $crashLines), 'Open', 'ReadWrite', 'None')
try { $n = @(New-CrashReports @("$logs\longshot_log.5.txt") $state $logs 'v' 'Testuser' 'TESTPC-01' $now).Count } finally { $held.Dispose() }
Check ($n -eq 0 -and @(New-CrashReports @("$logs\longshot_log.5.txt") $state $logs 'v' 'Testuser' 'TESTPC-01' $now).Count -eq 1) 'a log held by another program is tried again next time'
$p = Join-Path $tmp 'state.json'
Write-ReportState $p $state
Check (@((Read-ReportState $p).checked).Count -eq $state.checked.Count) 'the state is written and read back'

# --- The relay file, send_reports, the version ---
New-Item -ItemType Directory -Path "$kit\scripts" -Force | Out-Null
$relay = "$kit\scripts\report-relay.txt"
Check ((Get-KitReportRelay $kit) -eq '') 'no relay file: off'
[System.IO.File]::WriteAllText($relay, "# comment`r`n`r`n", $utf8)
Check ((Get-KitReportRelay $kit) -eq '') 'an empty relay file (the kill switch): off'
[System.IO.File]::WriteAllText($relay, "# comment`r`nhttps://squadala-crash-relay.example.workers.dev/report`r`n", $utf8)
Check ((Get-KitReportRelay $kit) -eq 'https://squadala-crash-relay.example.workers.dev/report') 'the https URL'
foreach ($bad in @('http://relay.example/report', 'ftp://x/report', 'https://x/report?u=1', 'https://discord.com/api/webhooks/1/x y')) {
    [System.IO.File]::WriteAllText($relay, $bad, $utf8)
    Check ((Get-KitReportRelay $kit) -eq '') "'$bad' is not a relay"
}
$opt = "$kit\coop-options.ini"
Check ((Get-KitSendReports $opt) -eq 'ask') 'no options file: ask'
[System.IO.File]::WriteAllLines($opt, [string[]]@('auto_update = ask', '# send_reports = yes', 'color = blue'))
Check ((Get-KitSendReports $opt) -eq 'ask') 'a line behind # is not read'
Set-KitSendReports $opt 'yes'
$o = @(Get-Content -LiteralPath $opt)
Check ((Get-KitSendReports $opt) -eq 'yes' -and $o.Count -eq 3 -and $o[1] -eq 'send_reports = yes' -and $o[2] -eq 'color = blue') "written over its own line, the rest kept (got $($o -join ' | '))"
Set-KitSendReports $opt 'no'
Check ((Get-KitSendReports $opt) -eq 'no' -and @(Get-Content -LiteralPath $opt).Count -eq 3) 'and changed again'
[System.IO.File]::WriteAllLines($opt, [string[]]@('auto_update = ask'))
Set-KitSendReports $opt 'yes'
Check ((Get-KitSendReports $opt) -eq 'yes') 'added when the file has no such line'
[System.IO.File]::WriteAllLines($opt, [string[]]@('send_reports = maybe'))
Check ((Get-KitSendReports $opt) -eq 'ask') 'an unknown value is ask'
Check (-not (Read-KitOptions (Join-Path $scripts '..\coop-options.ini')).Contains('send_reports')) 'the kit''s own options file has the line, and it is not passed to the game'
Check ((Get-KitSendReports (Join-Path $scripts '..\coop-options.ini')) -eq 'ask') 'the kit ships send_reports = ask'
[System.IO.File]::WriteAllLines("$kit\VERSION.txt", [string[]]@('kit: Squadala-v0.11.4-windows (release v0.11.4)', 'kind: hookshot'))
Check ((Get-KitReportVersion $kit) -eq 'v0.11.4') 'the version from VERSION.txt'
[System.IO.File]::WriteAllLines("$kit\VERSION.txt", [string[]]@('kit: oot-coop-dev', 'kind: hookshot'))
Check ((Get-KitReportVersion $kit) -eq 'v0.0.0-dev') 'a test kit: v0.0.0-dev'
Check ((Get-Content -LiteralPath (Join-Path $scripts 'report-relay.txt') | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith('#') }).Count -eq 0 -or
       (Get-KitReportRelay (Split-Path -Parent $scripts)) -match '^https://') 'the kit''s relay file is empty or an https URL'

# --- Problem reports waiting to be sent ---
$pz = Join-Path $logs 'coop-report-20990101-000000.zip'
[System.IO.File]::WriteAllText($pz, 'x', $utf8)
Check (@(Get-UnsentProblemReports $kit).Count -eq 1) 'a new problem report waits'
Add-Content -LiteralPath (Get-ReportSentFile $kit) -Value 'coop-report-20990101-000000.zip 2099-01-01 00:00:00'
Check (@(Get-UnsentProblemReports $kit).Count -eq 0) 'not once it is listed as sent'

# --- A whole start: the zip posted to the relay (a listener on 127.0.0.1), from the hidden sender ---
function Get-FreePort {
    $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $l.Start(); try { $l.LocalEndpoint.Port } finally { $l.Stop() }
}
$k2 = Join-Path $tmp 'kit2'
New-Item -ItemType Directory -Path "$k2\scripts", "$k2\logs" -Force | Out-Null
foreach ($f in @('send-report.ps1', 'log-mask.ps1', 'kit-paths.ps1', 'problem-report.ps1', 'coop-ini.ps1')) {
    if (Test-Path -LiteralPath (Join-Path $scripts $f)) { Copy-Item -LiteralPath (Join-Path $scripts $f) -Destination "$k2\scripts\$f" }
}
[System.IO.File]::WriteAllLines("$k2\VERSION.txt", [string[]]@('kit: Squadala-v0.11.4-windows (release v0.11.4)'))
[void](New-Log "$k2\logs\longshot_log.1.txt" $crashLines)
$port = Get-FreePort
[System.IO.File]::WriteAllText("$k2\scripts\report-relay.txt", "http://127.0.0.1:$port/report`r`n", $utf8)
$ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"

# send_reports = ask without a console: nothing asked, nothing sent, the report kept for /bugreport.
[System.IO.File]::WriteAllLines("$k2\coop-options.ini", [string[]]@('send_reports = ask'))
$env:OOT_COOP_KIT = $k2
$out = '' | & $ps -NoProfile -ExecutionPolicy Bypass -File "$k2\scripts\send-report.ps1" -AtStart 2>&1 | Out-String
Check ($out -match 'Your last game crashed' -and $out -notmatch 'Sending') "ask, no console: the report is made, not sent (got: $($out.Trim()))"
Check ((Get-KitSendReports "$k2\coop-options.ini") -eq 'ask') 'and the answer is still ask'
Check (@(Get-ChildItem "$k2\logs" -Filter 'crash-report-*.zip').Count -eq 1) 'the crash report is in logs\'

# yes: the next new crash goes to the relay.
& $ps -NoProfile -ExecutionPolicy Bypass -File "$k2\scripts\send-report.ps1" yes | Out-Null
Check ((Get-KitSendReports "$k2\coop-options.ini") -eq 'yes') 'send-report.cmd yes writes send_reports = yes'
[void](New-Log "$k2\logs\longshot_log.txt" $crashLines)
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://127.0.0.1:$port/")
$listener.Start()
try {
    $out = '' | & $ps -NoProfile -ExecutionPolicy Bypass -File "$k2\scripts\send-report.ps1" -AtStart 2>&1 | Out-String
    Check ($out -match 'Sending 1 report') "yes: one report is sent in the background (got: $($out.Trim()))"
    $wait = $listener.BeginGetContext($null, $null)
    if ($wait.AsyncWaitHandle.WaitOne(60000)) {
        $ctx = $listener.EndGetContext($wait)
        $req = $ctx.Request
        $ms = New-Object System.IO.MemoryStream
        $req.InputStream.CopyTo($ms)
        $body = $ms.ToArray()
        Check ($req.HttpMethod -eq 'POST' -and $req.Url.AbsolutePath -eq '/report') 'POST /report'
        Check ($req.ContentType -eq 'application/zip') "Content-Type application/zip (got $($req.ContentType))"
        Check ($req.Headers['X-Squadala-Kind'] -eq 'crash' -and $req.Headers['X-Squadala-Kit'] -eq 'v0.11.4') 'the kind and the version'
        Check ($body.Length -gt 22 -and $body[0] -eq 0x50 -and $body[1] -eq 0x4b -and $body[2] -eq 3 -and $body[3] -eq 4) 'the body is the zip'
        $ctx.Response.StatusCode = 202
        $ctx.Response.Close()
        $sent = ''
        for ($i = 0; $i -lt 100 -and -not $sent; $i++) {
            Start-Sleep -Milliseconds 100
            $sent = Get-Content -LiteralPath "$k2\logs\reports-sent.txt" -Raw -ErrorAction SilentlyContinue
        }
        Check ($sent -match '^crash-report-\d{8}-\d{6}-crash\.zip ') "listed in logs\reports-sent.txt (got '$sent')"
    } else {
        Check $false 'the hidden sender posted within 60 s'
    }
} finally {
    $listener.Stop()
    Remove-Item Env:\OOT_COOP_KIT -ErrorAction SilentlyContinue
}
# no: nothing is sent.
& $ps -NoProfile -ExecutionPolicy Bypass -File "$k2\scripts\send-report.ps1" no | Out-Null
$env:OOT_COOP_KIT = $k2
[void](New-Log "$k2\logs\longshot_log.txt" $crashLines)
$out = '' | & $ps -NoProfile -ExecutionPolicy Bypass -File "$k2\scripts\send-report.ps1" -AtStart 2>&1 | Out-String
Remove-Item Env:\OOT_COOP_KIT -ErrorAction SilentlyContinue
Check ($out -match 'Your last game crashed' -and $out -notmatch 'Sending' -and $out -match '/bugreport') "no: made, not sent, /bugreport named (got: $($out.Trim()))"

Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'all send-report checks passed'

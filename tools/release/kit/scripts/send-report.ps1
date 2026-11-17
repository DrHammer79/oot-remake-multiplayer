# Crash reports, and sending reports by themselves (docs/problem-reports.md). Opt-in, asked once (default no).
#
# At each start (start.ps1 calls it with -AtStart, after problem-report.ps1) it looks at the game's finished sessions
# not looked at before (the newest ReportLogsPerStart of the last ReportMaxAgeDays days), from the end of each log only:
#   crash    Longshot's crash handler or a fatal error wrote 'CRASH: exception' / 'FATAL:' (host\mkw_glue.c, hos_thread.c,
#            gpu\vk\vk_replay.cpp in Longshot's source)
#   unclean  neither that nor a clean exit ('control: exiting', 'display: window closed, ...': host\control.c,
#            host\hos_relaunch.c): the game was ended from outside (Task Manager, the PC turned off) or died silently
#   clean    a clean exit: nothing to report
# For a crash or an unclean end it writes logs\crash-report-<date>.zip: the log's start, its crash lines and its end,
# masked as collect-logs masks them (log-mask.ps1 Protect-LogText: addresses, host names, MAC addresses, the user, PC
# and players' names, invite codes, passwords).
#
# Sending: coop-options.ini's send_reports (ask, yes, no; scripts\send-report.cmd yes|no changes it). With ask, the first
# report asks once (no after 20 s or without a console) and keeps the answer there. With yes, each new crash report and
# each new problem report (problem-report.ps1's logs\coop-report-*.zip) is sent over HTTPS to the relay named in
# scripts\report-relay.txt (the kit's own file; empty: nothing is sent, whatever the answer), by a hidden process, so the
# start never waits for it. The relay passes it to the developers' Discord channel; nothing goes anywhere else.
# Sent zips are listed in logs\reports-sent.txt. Dot-source for the functions (tools\release\tests\send_report_test.ps1).
param([Parameter(Position = 0)][ValidateSet('yes', 'no', 'ask')][string]$Turn, [switch]$AtStart, [string]$Upload,
      [ValidateSet('crash', 'unclean', 'problem')][string]$Kind)

$script:ReportCrashLine = '\] CRASH: |(?:^|\] |vk: )FATAL: '
$script:ReportCrashStart = '\] CRASH: exception|(?:^|\] |vk: )FATAL: '
$script:ReportCleanExit = '\] control: exiting|\] display: window closed'
$script:ReportTailBytes = 512KB      # where a session's end is looked for (the crash handler's lines are its last)
$script:ReportHeadLines = 60
$script:ReportTailLines = 300
$script:ReportCrashLinesMax = 150
$script:ReportMaxAgeDays = 7
$script:ReportLogsPerStart = 2
$script:ReportZipsKept = 10
$script:ReportSelf = $PSCommandPath   # (this file, for the hidden sender)
# A relay URL: https only (plain http to 127.0.0.1 is for the tests' own listener).
$script:ReportRelayUrl = '^(?:https://[A-Za-z0-9.-]+|http://127\.0\.0\.1)(?::\d{1,5})?/[\w./-]*$'

function Get-ReportStateFile([string]$Kit) { Join-Path $Kit 'logs\report-sending.json' }
function Get-ReportSentFile([string]$Kit) { Join-Path $Kit 'logs\reports-sent.txt' }

# The relay's URL from scripts\report-relay.txt: its first line that is not a comment, when it is an https:// URL;
# else '' (nothing is sent). A release can empty it (the kill switch), and the updater replaces this file.
function Get-KitReportRelay([string]$Kit) {
    $file = Join-Path $Kit 'scripts\report-relay.txt'
    foreach ($line in @(Get-Content -LiteralPath $file -ErrorAction SilentlyContinue)) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        if ($t -match $script:ReportRelayUrl) { return $t }
        return ''
    }
    ''
}

# coop-options.ini's send_reports: ask (the default, and for a line that is not one of the three), yes or no.
function Get-KitSendReports([string]$Options) {
    $v = 'ask'
    foreach ($line in @(Get-Content -LiteralPath $Options -ErrorAction SilentlyContinue)) {
        if ($line -match '^\s*send_reports\s*=\s*(\w*)\s*(?:[#;].*)?$') {
            $v = if ($Matches[1] -match '^(?i)(ask|yes|no)$') { $Matches[1].ToLowerInvariant() } else { 'ask' }
        }
    }
    $v
}

# Writes send_reports = <value> into coop-options.ini: over its line (also one behind a '#'), else at the end.
function Set-KitSendReports([string]$Options, [string]$Value) {
    $lines = @()
    if (Test-Path -LiteralPath $Options) { $lines = @([System.IO.File]::ReadAllLines($Options)) }
    $done = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*#?\s*send_reports\s*=') {
            if ($done) { $lines[$i] = '# ' + $lines[$i].TrimStart('#', ' ') } else { $lines[$i] = "send_reports = $Value"; $done = $true }
        }
    }
    if (-not $done) { $lines += @('', "send_reports = $Value") }
    [System.IO.File]::WriteAllLines($Options, [string[]]$lines, (New-Object System.Text.UTF8Encoding $false))
}

# The kit's version for the relay: VERSION.txt's '(release v0.11.5)', else v0.0.0-dev (a kit built for testing).
function Get-KitReportVersion([string]$Kit) {
    $first = ''
    try { $first = Get-Content -LiteralPath (Join-Path $Kit 'VERSION.txt') -TotalCount 1 -ErrorAction Stop } catch { }
    if ($first -match '\(release (v?\d+\.\d+(?:\.\d+)?(?:[-+][0-9A-Za-z.-]{1,32})?)\)') { return $Matches[1] }
    'v0.0.0-dev'
}

# The state: the sessions looked at (their keys, problem-report.ps1's Get-ProblemRunKey).
function Read-ReportState([string]$Path) {
    $state = @{ checked = @() }
    try {
        $j = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop | ConvertFrom-Json
        if ($j.checked) { $state.checked = @($j.checked | ForEach-Object { [string]$_ }) }
    } catch { }
    $state
}
function Write-ReportState([string]$Path, [hashtable]$State) {
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $json = [ordered]@{ checked = @($State.checked | Select-Object -Last 200) } | ConvertTo-Json -Depth 2
    [System.IO.File]::WriteAllText("$Path.new", $json, (New-Object System.Text.UTF8Encoding $false))
    Move-Item -LiteralPath "$Path.new" -Destination $Path -Force
}

# How a session's log ends, from its last ReportTailBytes: @{ Kind = crash|unclean|clean; Tail = its last lines;
# Crash = its crash lines (from the first 'CRASH: exception' or 'FATAL:' on) }. The stream is put back at its start.
function Get-LogSessionEnd([System.IO.Stream]$Stream) {
    $from = [Math]::Max(0, $Stream.Length - $script:ReportTailBytes)
    $Stream.Position = $from
    $reader = New-Object System.IO.StreamReader($Stream, (New-Object System.Text.UTF8Encoding $false), $true, 65536, $true)
    try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
    $Stream.Position = 0
    $lines = @($text -split '\r?\n')
    if ($from -gt 0 -and $lines.Count -gt 1) { $lines = $lines[1..($lines.Count - 1)] }   # (a part line)
    while ($lines.Count -gt 0 -and -not $lines[-1]) { $lines = @($lines | Select-Object -SkipLast 1) }
    $crash = @()
    $in = $false
    foreach ($l in $lines) {
        if (-not $in -and $l -match $script:ReportCrashStart) { $in = $true }
        if ($in -and $l -match $script:ReportCrashLine -and $crash.Count -lt $script:ReportCrashLinesMax) { $crash += $l }
    }
    $kind = if ($crash.Count) { 'crash' } elseif (@($lines -match $script:ReportCleanExit).Count) { 'clean' } else { 'unclean' }
    @{ Kind = $kind; Tail = @($lines | Select-Object -Last $script:ReportTailLines); Crash = $crash }
}

# The players' names a whole log writes, learnt for the mask (one pass of one regex over the log: only for a report).
function Read-LogNames([System.IO.Stream]$Stream, $Names) {
    $Stream.Position = 0
    $reader = New-Object System.IO.StreamReader($Stream, (New-Object System.Text.UTF8Encoding $false), $true, 65536, $true)
    try {
        while ($null -ne ($line = $reader.ReadLine())) {
            if ($line.Contains('player ') -or $line.Contains("name '")) { Add-LogNames $line $Names }
        }
    } finally { $reader.Dispose() }
    $Stream.Position = 0
}

# The first lines of a log (what was started: versions, the graphics card, the co-op settings).
function Read-LogHead([System.IO.Stream]$Stream, [int]$Count) {
    $Stream.Position = 0
    $reader = New-Object System.IO.StreamReader($Stream, (New-Object System.Text.UTF8Encoding $false), $true, 65536, $true)
    $head = @()
    try { while ($head.Count -lt $Count -and $null -ne ($line = $reader.ReadLine())) { $head += $line } }
    finally { $reader.Dispose() }
    $Stream.Position = 0
    $head
}

# A crash report for one log: logs\crash-report-<date>-<kind>.zip with crash-report.txt (masked), or $null for a clean
# end. $End: Get-LogSessionEnd's.
function New-CrashReport([System.IO.Stream]$Stream, [hashtable]$End, [string]$LogName, [datetime]$Written,
                         [string]$OutDir, [string]$Version, [string]$UserName = $env:USERNAME,
                         [string]$PcName = $env:COMPUTERNAME) {
    if ($End.Kind -eq 'clean') { return $null }
    $mask = New-MaskState
    Read-LogNames $Stream $mask.Names
    $m = { param($l) Protect-LogText $l $mask $UserName $PcName }
    $what = if ($End.Kind -eq 'crash') { 'the game crashed (its crash lines are below)' }
            else { 'the game ended without closing (no crash line and no clean exit: ended from outside, the PC turned off, or a silent crash)' }
    $lines = @("Squadala! crash report: $what",
        "Kit: $Version",
        "Log: $LogName, last written $($Written.ToString('yyyy-MM-dd HH:mm'))",
        'Addresses, host names, user, PC and players'' names, codes and passwords are masked (as collect-logs masks them).',
        '', "--- the crash ($($End.Crash.Count) lines)")
    $lines += @($End.Crash | ForEach-Object { & $m $_ })
    $lines += @('', "--- the start of the log (first $script:ReportHeadLines lines)")
    $lines += @(Read-LogHead $Stream $script:ReportHeadLines | ForEach-Object { & $m $_ })
    $lines += @('', "--- the end of the log (last $($End.Tail.Count) lines)")
    $lines += @($End.Tail | ForEach-Object { & $m $_ })
    if (-not (Test-Path -LiteralPath $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
    $stage = Join-Path ([System.IO.Path]::GetTempPath()) ('crash-report-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    try {
        [System.IO.File]::WriteAllText((Join-Path $stage 'crash-report.txt'), ($lines -join "`r`n") + "`r`n",
            (New-Object System.Text.UTF8Encoding $false))
        $zip = Join-Path $OutDir ('crash-report-' + $Written.ToString('yyyyMMdd-HHmmss') + "-$($End.Kind).zip")
        Write-LogZip $stage $zip
    } finally {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
    Get-ChildItem -LiteralPath $OutDir -Filter 'crash-report-*.zip' -File -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -Skip $script:ReportZipsKept |
        Remove-Item -Force -ErrorAction SilentlyContinue
    $zip
}

# The crash reports of the finished sessions not looked at before: @(@{ Zip; Kind }). The newest ReportLogsPerStart
# logs of the last ReportMaxAgeDays days are looked at (older ones are only marked as looked at, the rest wait for the
# next start); a log that cannot be opened is tried again next time. $State.checked is brought up to date (the caller writes it).
function New-CrashReports([string[]]$Logs, [hashtable]$State, [string]$OutDir, [string]$Version,
                          [string]$UserName = $env:USERNAME, [string]$PcName = $env:COMPUTERNAME, [datetime]$Now = (Get-Date)) {
    $made = @()
    $looked = 0
    foreach ($log in @($Logs | Sort-Object { (Get-Item -LiteralPath $_).LastWriteTime } -Descending)) {
        try {
            $written = (Get-Item -LiteralPath $log).LastWriteTime
            $stream = Open-ProblemLog $log
            try {
                $key = Get-ProblemRunKey $stream
                if ($State.checked -contains $key) { continue }
                if (($Now - $written).TotalDays -gt $script:ReportMaxAgeDays) { $State.checked = @($State.checked) + $key; continue }
                if ($looked -ge $script:ReportLogsPerStart) { continue }   # (looked at next time)
                $looked++
                $end = Get-LogSessionEnd $stream
                $zip = New-CrashReport $stream $end (Split-Path -Leaf $log) $written $OutDir $Version $UserName $PcName
                if ($zip) { $made += @{ Zip = $zip; Kind = $end.Kind } }
                $State.checked = @($State.checked) + $key
            } finally { $stream.Dispose() }
        } catch { continue }
    }
    $made
}

# The problem reports (problem-report.ps1) of the last day not sent yet, the newest 2.
function Get-UnsentProblemReports([string]$Kit, [datetime]$Now = (Get-Date)) {
    $sent = @(Get-Content -LiteralPath (Get-ReportSentFile $Kit) -ErrorAction SilentlyContinue | ForEach-Object { ($_ -split ' ')[0] })
    @(Get-ChildItem -LiteralPath (Join-Path $Kit 'logs') -Filter 'coop-report-*.zip' -File -ErrorAction SilentlyContinue |
        Where-Object { ($Now - $_.LastWriteTime).TotalDays -le 1 -and $sent -notcontains $_.Name } |
        Sort-Object Name -Descending | Select-Object -First 2 | ForEach-Object { @{ Zip = $_.FullName; Kind = 'problem' } })
}

# Yes or no, NO when no key comes in $Seconds or there is no console to ask on.
function Read-ReportYesNo([string]$Question, [int]$Seconds) {
    $redirected = $true
    try { $redirected = [Console]::IsInputRedirected -or -not [Environment]::UserInteractive } catch { }
    if ($redirected) { return $null }
    Write-Host -NoNewline "$Question [y/N] (no in $Seconds s) "
    $end = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $end) {
        if ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)
            if ($key.Key -eq 'Y') { Write-Host 'y'; return $true }
            if ($key.Key -eq 'N' -or $key.Key -eq 'Enter' -or $key.Key -eq 'Escape') { Write-Host 'n'; return $false }
        }
        Start-Sleep -Milliseconds 100
    }
    Write-Host 'n'
    $false
}

# One zip to the relay: POST, Content-Type application/zip, the kind and the kit's version in headers. The HTTP status,
# or 0 when it could not be reached.
function Send-KitReport([string]$Url, [string]$Zip, [string]$Kind, [string]$Version, [int]$TimeoutSec = 60) {
    if ($Url -notmatch $script:ReportRelayUrl) { return 0 }
    try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch { }
    try {
        $r = Invoke-WebRequest -Uri $Url -Method Post -InFile $Zip -ContentType 'application/zip' -UseBasicParsing `
            -TimeoutSec $TimeoutSec -Headers @{ 'X-Squadala-Kind' = $Kind; 'X-Squadala-Kit' = $Version }
        [int]$r.StatusCode
    } catch {
        $resp = $_.Exception.Response
        if ($resp) { try { return [int]$resp.StatusCode } catch { } }
        0
    }
}

# Starts the hidden sender for each zip (this script -Upload), so the start goes on at once.
function Start-ReportUpload([string]$Zip, [string]$Kind) {
    $ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    Start-Process -FilePath $ps -WindowStyle Hidden -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        "`"$script:ReportSelf`"", '-Upload', "`"$Zip`"", '-Kind', $Kind) | Out-Null
}

if ($MyInvocation.InvocationName -eq '.') { return }   # (dot-sourced: the functions only)

# (problem-report.ps1 has parameters of its own: dot-sourcing it sets $Turn and $AtStart here (with its own checks), so ours are read from $want)
$want = @{ Turn = $Turn; AtStart = [bool]$AtStart; Upload = $Upload; Kind = $Kind }
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'log-mask.ps1')
. (Join-Path $PSScriptRoot 'kit-paths.ps1')
. (Join-Path $PSScriptRoot 'problem-report.ps1')
$kit = if ($env:OOT_COOP_KIT) { $env:OOT_COOP_KIT } else { Split-Path -Parent $PSScriptRoot }
$options = Join-Path $kit 'coop-options.ini'

if ($want.Turn) {
    Set-KitSendReports $options $want.Turn
    switch ($want.Turn) {
        'yes' { Write-Host 'Reports are sent by themselves now (send_reports = yes in coop-options.ini).' }
        'no' { Write-Host 'Reports are not sent (send_reports = no). Crash reports stay in logs\ for /bugreport.' }
        default { Write-Host 'The kit asks again at the next report (send_reports = ask).' }
    }
    if (-not (Get-KitReportRelay $kit)) { Write-Host 'This kit has no relay set (scripts\report-relay.txt is empty): nothing is sent.' }
    return
}

if ($want.Upload) {
    # The hidden sender: one zip, its result in logs\reports-sent.txt (sent) or logs\report-sending.log (not).
    $url = Get-KitReportRelay $kit
    if (-not $url -or (Get-KitSendReports $options) -ne 'yes' -or -not (Test-Path -LiteralPath $want.Upload)) { return }
    $code = Send-KitReport $url $want.Upload $want.Kind (Get-KitReportVersion $kit)
    $stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    $name = Split-Path -Leaf $want.Upload
    if ($code -ge 200 -and $code -lt 300) { Add-Content -LiteralPath (Get-ReportSentFile $kit) -Value "$name $stamp" -Encoding UTF8 }
    else { Add-Content -LiteralPath (Join-Path $kit 'logs\report-sending.log') -Value "$stamp $name not sent (HTTP $code)" -Encoding UTF8 }
    return
}

if (-not $want.AtStart) { Write-Host 'Usage: send-report.cmd yes | no | ask'; return }
$stateFile = Get-ReportStateFile $kit
$state = Read-ReportState $stateFile
$version = Get-KitReportVersion $kit
$crashes = @(New-CrashReports @(Get-ProblemLogs $kit) $state (Join-Path $kit 'logs') $version)
Write-ReportState $stateFile $state
$url = Get-KitReportRelay $kit
$choice = Get-KitSendReports $options
$todo = @($crashes)
if ($url -and $choice -ne 'no') { $todo += @(Get-UnsentProblemReports $kit) }
foreach ($c in $crashes) {
    $what = if ($c.Kind -eq 'crash') { 'Your last game crashed.' } else { 'Your last game ended without closing.' }
    Write-Host "$what A crash report (masked): $($c.Zip)"
}
if (-not $todo.Count) { return }
if (-not $url) {
    if ($crashes.Count) { Write-Host 'Please send it with /bugreport in the Squadala! Discord.' }
    return
}
if ($choice -eq 'ask') {
    Write-Host ''
    Write-Host 'Reports: the kit can send its crash and problem reports to the Squadala! developers by itself. They hold'
    Write-Host 'parts of your game log with addresses, host names, user, PC and players'' names and codes masked, go to the'
    Write-Host 'developers'' Discord channel only, and are never sent anywhere else. Change it later: scripts\send-report.cmd'
    Write-Host 'yes (or no), or send_reports in coop-options.ini.'
    $answer = Read-ReportYesNo 'Send reports by themselves?' 20
    if ($null -eq $answer) { return }   # (no console: asked at a start that has one)
    $choice = if ($answer) { 'yes' } else { 'no' }
    Set-KitSendReports $options $choice
}
if ($choice -ne 'yes') {
    if ($crashes.Count) { Write-Host 'You can send it yourself with /bugreport in the Squadala! Discord.' }
    return
}
foreach ($t in $todo) { Start-ReportUpload $t.Zip $t.Kind }
Write-Host "Sending $($todo.Count) report(s) to the developers in the background (send_reports = yes)."

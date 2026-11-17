# Masks what identifies a player in text that collect-logs.ps1 puts into the zip, and writes that zip (Write-LogZip, at the end).
# Dot-source it. Each IPv4/IPv6 address becomes its kind with a number, so a reader can
# still see which lines mean the same machine: <lan-1>, <tailscale-1>, <hamachi-1>,
# <radmin-1>, <loopback-1>, <public-1>. Ports stay. MAC addresses become <mac>, the
# Windows user name (in C:\Users\<name> and on its own) becomes <user>, the PC name <pc>,
# a host name (myhost.duckdns.org, also with its :port or in a URL) <host-1>, <host-2>, ...,
# a player's name <name> (learnt where the module or coop.ini writes one, then everywhere),
# and a set password (password = ..., server_password = ...) becomes (removed).
# Works on strings only; tools\release\tests\log_mask_test.ps1 checks it.

# A new state: one per zip, so the same address gets the same tag in every file. Names: the players' names learnt so
# far (Add-LogNames), masked in every later text of the same state too.
function New-MaskState { @{ Tags = @{}; Counts = @{}; Names = (New-Object 'System.Collections.Generic.HashSet[string]') } }

# Words that are not a person's name on their own (a short or common user name, a player named Link): never masked as
# a name or a user name, or every log line would be mangled.
$script:LogCommonWords = @('admin', 'administrator', 'user', 'users', 'owner', 'player', 'guest', 'link', 'zelda', 'navi',
    'default', 'public', 'host', 'server', 'none', 'unknown')

# Where a player's name is written: the server's 'player 2 joined: Mido' and 'player 2 left: Mido (...)', a game's
# "player 2 'Mido' joined", 'Mods: player 2 (Mido)', our own "name 'Mido'", and coop.ini's 'name = Mido'.
$script:LogNameAt = '(?m)player \d+ (?:joined|left): (?<n>[^(\r\n]+?)(?= \(|\r?$)|player \d+ ''(?<n>[^''\r\n]+)''|' +
    'Mods: player \d+ \((?<n>[^)\r\n]+)\)|name ''(?<n>[^''\r\n]+)''|^[ \t]*name[ \t]*=[ \t]*(?<n>[^\s#;][^\r\n#;]*?)[ \t]*\r?$'

# A dotted word whose last part is one of these is a file name, not a host name.
$script:LogFileExt = '^(?:exe|dll|so|sys|txt|log|ini|json|list|zip|gz|tar|zst|zs|cpp|hpp|cc|c|h|inc|ps1|psm1|cmd|bat|sh|py|' +
    'pak|sav|bin|png|jpg|jpeg|bmp|dds|tga|otf|ttf|ttc|cfg|toml|md|nup|pdb|map|nso|nca|nsp|xci|sarc|szs|bfres|byml|bgyml|' +
    'msbt|bntx|bfstm|bars|xml|yml|yaml|html|htm|js|css|dat|db|tmp|old|new|bak|part|lock|appimage|desktop|conf|config|wav|' +
    'ogg|opus|mp4|webm|glsl|spv|cache|pipeline|lst|csv|sha256|sig|pem|crt|key|mod|nro|kip|ips|pchtxt)$'

# Learns the players' names a text writes (LogNameAt) into $Names.
function Add-LogNames([string]$Text, $Names) {
    if (-not $Text) { return }
    foreach ($m in [regex]::Matches($Text, $script:LogNameAt)) {
        $n = $m.Groups['n'].Value.Trim()
        if ($n.Length -ge 2 -and $n -notmatch '^<[a-z]+(-\d+)?>$' -and $script:LogCommonWords -notcontains $n.ToLowerInvariant()) {
            [void]$Names.Add($n)
        }
    }
}

# The text with every name known (those it writes included) as <name>: whole words, the longest first.
function Protect-LogNames([string]$Text, $Names) {
    Add-LogNames $Text $Names
    if (-not $Text) { return $Text }
    foreach ($n in @($Names | Sort-Object Length -Descending)) {
        $Text = [regex]::Replace($Text, '(?<!\w)' + [regex]::Escape($n) + '(?!\w)', '<name>')
    }
    $Text
}

# The kind of an IPv4 address, or '' when it should stay (not an address, 0.0.0.0,
# 255.255.255.255).
function Get-MaskKind4([string]$Address) {
    if ($Address -notmatch '^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$') { return '' }
    $o = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], [int]$Matches[4])
    foreach ($x in $o) { if ($x -gt 255) { return '' } }
    if ($Address -eq '0.0.0.0' -or $Address -eq '255.255.255.255') { return '' }
    if ($o[0] -eq 127) { return 'loopback' }
    if ($o[0] -eq 10 -or ($o[0] -eq 172 -and $o[1] -ge 16 -and $o[1] -le 31) -or
        ($o[0] -eq 192 -and $o[1] -eq 168) -or ($o[0] -eq 169 -and $o[1] -eq 254)) { return 'lan' }
    if ($o[0] -eq 100 -and $o[1] -ge 64 -and $o[1] -le 127) { return 'tailscale' }
    if ($o[0] -eq 25) { return 'hamachi' }
    if ($o[0] -eq 26) { return 'radmin' }
    return 'public'
}

# The kind of an IPv6 address, or '' when it is not one (or is :: alone).
function Get-MaskKind6([string]$Address) {
    $a = $Address -replace '%.*$', ''
    if ($a -notmatch ':' ) { return '' }
    $ip = $null
    if (-not [System.Net.IPAddress]::TryParse($a, [ref]$ip)) { return '' }
    if ($ip.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetworkV6) { return '' }
    $b = $ip.GetAddressBytes()
    if (($b | Measure-Object -Sum).Sum -eq 0) { return '' }
    if ([System.Net.IPAddress]::IsLoopback($ip)) { return 'loopback' }
    if ($b[0] -eq 0xfe -and ($b[1] -band 0xc0) -eq 0x80) { return 'lan' }
    if ($b[0] -eq 0xfd -and $b[1] -eq 0x7a -and $b[2] -eq 0x11 -and $b[3] -eq 0x5c -and $b[4] -eq 0xa1 -and $b[5] -eq 0xe0) { return 'tailscale' }
    if (($b[0] -band 0xfe) -eq 0xfc) { return 'lan' }
    if ($b[0] -eq 0x26 -and $b[1] -eq 0x20 -and $b[2] -eq 0x00 -and $b[3] -eq 0x9b -and
        (($b[4..11] | Measure-Object -Sum).Sum -eq 0)) { return 'hamachi' }
    return 'public'
}

function Get-MaskTag([hashtable]$State, [string]$Kind, [string]$Key) {
    $key = $Key.ToLowerInvariant()
    if (-not $State.Tags.ContainsKey($key)) {
        $n = 1 + [int]$State.Counts[$Kind]
        $State.Counts[$Kind] = $n
        $State.Tags[$key] = "<$Kind-$n>"
    }
    $State.Tags[$key]
}

# The masked text. $UserName and $PcName default to this PC's; tests pass their own.
function Protect-LogText([string]$Text, [hashtable]$State,
                         [string]$UserName = $env:USERNAME, [string]$PcName = $env:COMPUTERNAME) {
    if (-not $Text) { return $Text }
    $t = $Text
    # Passwords and other secrets (password = ..., server_password=..., Password : ...,
    # --password x): a set value becomes (removed); an empty one stays empty, so a reader
    # sees none was set.
    $t = [regex]::Replace($t, '(?im)^([ \t]*[\w.]*(?:password|secret|token)\w*[ \t]*[=:])[ \t]*[^\s][^\r\n]*', '$1 (removed)')
    $t = [regex]::Replace($t, '(?i)(?<![\w-])(--?[\w-]*(?:password|secret|token)[\w-]*)(?:[ \t]+|=)(?!-)\S+', '$1 (removed)')
    # An invite code holds the join password (docs/play-online.md): never in a shared log.
    $t = [regex]::Replace($t, '(?i)(invite code:?)[ \t]*[A-Z2-9][A-Z2-9-]{9,}', '$1 (removed)')
    # MAC addresses first (their colon form would otherwise look like IPv6 groups), also
    # the dotted form aabb.ccdd.eeff.
    $t = [regex]::Replace($t, '(?<![0-9A-Fa-f:-])(?:[0-9A-Fa-f]{2}([:-]))(?:[0-9A-Fa-f]{2}\1){4}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:-])', '<mac>')
    $t = [regex]::Replace($t, '(?<![\w.])[0-9A-Fa-f]{4}\.[0-9A-Fa-f]{4}\.[0-9A-Fa-f]{4}(?![\w.])', '<mac>')
    # IPv6 (before IPv4, so ::ffff:1.2.3.4 is one address: the dotted ending is tried
    # first). A mapped IPv4 (::ffff:a.b.c.d) gets the tag of that IPv4.
    $t = [regex]::Replace($t, '(?<![\w:.])(?:[0-9A-Fa-f]{0,4}:){2,7}(?:\d{1,3}(?:\.\d{1,3}){3}|[0-9A-Fa-f]{0,4})(?:%[\w.-]+)?(?![\w:]|\.\d)', {
        param($m)
        $kind = Get-MaskKind6 $m.Value
        if (-not $kind) { return $m.Value }
        $ip = [System.Net.IPAddress]::Parse(($m.Value -replace '%.*$', ''))
        if ($ip.IsIPv4MappedToIPv6) {
            $v4 = $ip.MapToIPv4().ToString()
            $kind4 = Get-MaskKind4 $v4
            if ($kind4) { return Get-MaskTag $State $kind4 $v4 }
        }
        Get-MaskTag $State $kind $ip.ToString()
    })
    # IPv4: four numbers, not part of a longer dotted number and not a version (1.1.1,
    # 10.0.26200.0 and v1.2.3.4 stay).
    $t = [regex]::Replace($t, '(?<![\d.vV])\d{1,3}(?:\.\d{1,3}){3}(?![\d]|\.\d)', {
        param($m)
        $kind = Get-MaskKind4 $m.Value
        if (-not $kind) { return $m.Value }
        Get-MaskTag $State $kind $m.Value
    })
    # A host name typed as the server (server = myhost.example.org) becomes <host>;
    # localhost and masked addresses stay.
    $t = [regex]::Replace($t, '(?im)^([ \t]*server[ \t]*=[ \t]*)(?!localhost\b)([A-Za-z][\w.-]*)', '$1<host>')
    # Any other host name: a dotted name whose last part is lower-case letters and not a file's extension
    # (myhost.duckdns.org, also before :27510 or after https://); the same name gets the same tag. File names
    # (hookshot_coop.dll, coop-mods.list) and the names in a path (C:\x\coop.ini) stay.
    $t = [regex]::Replace($t, '(?:(?<=//)|(?<![\w.\\/<>-]))(?:[A-Za-z0-9](?:[\w-]*[A-Za-z0-9])?\.)+(?<tld>[a-z]{2,24})(?![\w-]|\.\w)', {
        param($m)
        if ($m.Groups['tld'].Value -match $script:LogFileExt) { return $m.Value }
        Get-MaskTag $State 'host' $m.Value
    })
    # The user name in paths (C:\Users\name, C:/Users/name, \\?\C:\Users\name).
    $t = [regex]::Replace($t, '(?i)([A-Z]:[\\/]+Users[\\/]+)[^\\/\r\n"''<>|:*?]+', '$1<user>')
    # The user name and the PC name on their own, whole words only. A short or common
    # user name (Link, User, Admin, Player) would mangle every log line, so the user name
    # needs 5 letters or more and must not be one of those.
    $common = $script:LogCommonWords
    foreach ($p in @(@($UserName, '<user>', 5), @($PcName, '<pc>', 3))) {
        if ($p[0] -and $p[0].Length -ge $p[2] -and $common -notcontains $p[0].ToLowerInvariant()) {
            $t = [regex]::Replace($t, '(?i)(?<![\w])' + [regex]::Escape($p[0]) + '(?![\w])', $p[1])
        }
    }
    # Players' names: the ones this text writes, and every one learnt from an earlier text of the same state.
    if (-not $State.ContainsKey('Names')) { $State.Names = New-Object 'System.Collections.Generic.HashSet[string]' }
    Protect-LogNames $t $State.Names
}

# Writes the staged folder into a standard zip: '/' between folders (Compress-Archive in Windows
# PowerShell 5.1 writes '\', which unzip tools warn about), one entry per file, no './' and no
# folder entries. .NET's ZipArchive only, so a PC without System32\tar.exe gets the same zip.
# A zip left half-written by an error is removed before the error goes on.
function Write-LogZip([string]$Stage, [string]$Zip) {
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $root = [System.IO.Path]::GetFullPath($Stage).TrimEnd('\', '/') + '\'
    if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }
    $done = $false
    $archive = [System.IO.Compression.ZipFile]::Open($Zip, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in @(Get-ChildItem -LiteralPath $root -Recurse -File -Force | Sort-Object FullName)) {
            $name = $f.FullName.Substring($root.Length).Replace('\', '/')
            [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $name,
                [System.IO.Compression.CompressionLevel]::Optimal)
        }
        $done = $true
    } finally {
        $archive.Dispose()
        if (-not $done) { Remove-Item -LiteralPath $Zip -Force -ErrorAction SilentlyContinue }
    }
}

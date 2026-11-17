# Masks what identifies a player in log files, as the Windows kit's log-mask.ps1 does (lib.sh kit_mask runs it; used by
# collect-logs.sh and send-report.sh). Run as: awk -v user=U -v pc=P -f log-mask.awk pass=1 FILE... pass=2 FILE...
# Pass 1 learns the players' names every file writes (so a name learnt in one file is masked in all of them); pass 2
# writes FILE.masked for each file:
#   a set password (password = ..., server_password = ..., relay_key = ..., --password x) and an invite code: (removed)
#   MAC addresses: <mac>; each IPv4 address its kind with a number (<lan-1>, <public-1>, ...; the same address the same
#   tag in every file); a host name typed as the server (server = myhost.example.org): <host>; any other host name (a
#   dotted name whose last part is lower-case letters and not a file's extension, also before :port or after //):
#   <host-1>, <host-2>, ...; the user name (in /home/<name> and on its own): <user>; the PC name: <pc>; players' names
#   (learnt where the server, the module or coop.ini writes one): <name>.
# POSIX awk (mawk too): no {n} intervals, no gawk functions.
BEGIN {
    sq = sprintf("%c", 39)
    # Words that are not a person's name on their own (log-mask.ps1 LogCommonWords).
    ncommon = split("admin administrator user users owner player guest link zelda navi default public host server none unknown deck", c, " ")
    for (i = 1; i <= ncommon; i++) common[c[i]] = 1
    # A dotted word whose last part is one of these is a file name, not a host name (log-mask.ps1 LogFileExt).
    n = split("exe dll so sys txt log ini json list zip gz tar zst zs cpp hpp cc c h inc ps1 psm1 cmd bat sh py " \
        "pak sav bin png jpg jpeg bmp dds tga otf ttf ttc cfg toml md nup pdb map nso nca nsp xci sarc szs bfres byml bgyml " \
        "msbt bntx bfstm bars xml yml yaml html htm js css dat db tmp old new bak part lock appimage desktop conf config wav " \
        "ogg opus mp4 webm glsl spv cache pipeline lst csv sha256 sig pem crt key mod nro kip ips pchtxt awk", e, " ")
    for (i = 1; i <= n; i++) fileext[e[i]] = 1
    nnames = 0
}
function kind(a,   o, n, i) {
    n = split(a, o, ".")
    for (i = 1; i <= 4; i++) if (length(o[i]) > 3 || o[i] + 0 > 255) return ""
    if (a == "0.0.0.0" || a == "255.255.255.255") return ""
    o[1] += 0; o[2] += 0
    if (o[1] == 127) return "loopback"
    if (o[1] == 10 || (o[1] == 172 && o[2] >= 16 && o[2] <= 31) || (o[1] == 192 && o[2] == 168) || (o[1] == 169 && o[2] == 254)) return "lan"
    if (o[1] == 100 && o[2] >= 64 && o[2] <= 127) return "tailscale"
    if (o[1] == 25) return "hamachi"
    if (o[1] == 26) return "radmin"
    return "public"
}
function tag(k, a) {
    if (!((k SUBSEP a) in tags)) { count[k]++; tags[k SUBSEP a] = "<" k "-" count[k] ">" }
    return tags[k SUBSEP a]
}
# t with the whole word w as r (ci: any case). A word character is a letter, a digit or _.
function words(t, w, r, ci,   out, before, after, lt, lw, i) {
    if (w == "") return t
    out = ""; lw = ci ? tolower(w) : w
    while ((i = index(ci ? tolower(t) : t, lw)) > 0) {
        before = substr(t, i - 1, 1); after = substr(t, i + length(w), 1)
        if ((i == 1 || before !~ /[A-Za-z0-9_]/) && after !~ /[A-Za-z0-9_]/) out = out substr(t, 1, i - 1) r
        else out = out substr(t, 1, i - 1 + length(w))
        t = substr(t, i + length(w))
    }
    return out t
}
function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
function addname(n,   i, j, k) {
    n = trim(n)
    if (length(n) < 2 || n ~ /^<[a-z]+(-[0-9]+)?>$/ || (tolower(n) in common) || (n in known)) return
    known[n] = 1
    # (kept longest first: a longer name is masked before a shorter one inside it)
    for (i = nnames; i >= 1 && length(names[i]) < length(n); i--) names[i + 1] = names[i]
    names[i + 1] = n; nnames++
}
# The players' names a line writes (log-mask.ps1 LogNameAt): the server's 'player 2 joined: Mido' and 'player 2 left:
# Mido (...)', a game's "player 2 'Mido' joined", 'Mods: player 2 (Mido)', our own "name 'Mido'", coop.ini's 'name = Mido'.
function learn(t,   s, n, i) {
    if (!index(t, "player ") && !index(t, "name")) return   # (most lines: quick)
    sub(/\r$/, "", t)
    s = t
    while (match(s, /player [0-9]+ (joined|left): /)) {
        s = substr(s, RSTART + RLENGTH); n = s
        if ((i = index(n, "(")) > 0) { if (substr(n, i - 1, 1) != " ") continue; n = substr(n, 1, i - 2) }
        addname(n)
    }
    s = t
    while (match(s, /player [0-9]+ /)) {
        s = substr(s, RSTART + RLENGTH)
        if (substr(s, 1, 1) == sq && (i = index(substr(s, 2), sq)) > 1) addname(substr(s, 2, i - 1))
    }
    s = t
    while (match(s, /Mods: player [0-9]+ \(/)) {
        s = substr(s, RSTART + RLENGTH)
        if ((i = index(s, ")")) > 1) addname(substr(s, 1, i - 1))
    }
    s = t
    while ((i = index(s, "name " sq)) > 0) {
        s = substr(s, i + 6)
        if ((i = index(s, sq)) > 1) addname(substr(s, 1, i - 1))
    }
    if (match(t, /^[ \t]*name[ \t]*=[ \t]*[^ \t#;]/)) {
        n = substr(t, RSTART + RLENGTH - 1)
        sub(/[#;].*$/, "", n)
        addname(n)
    }
}
# Host names: a dotted name whose last part is 2 to 24 lower-case letters, not after a word character, '.', '\', '/',
# '<', '>' or '-' (but after '//'), not before a word character, '-' or '.' and a word character.
function hosts(t,   out, m, before, after, last) {
    out = ""
    while (match(t, /([A-Za-z0-9]([A-Za-z0-9_-]*[A-Za-z0-9])?\.)+[a-z][a-z]+/)) {
        m = substr(t, RSTART, RLENGTH)
        before = RSTART > 1 ? substr(t, RSTART - 1, 1) : ""
        after = substr(t, RSTART + RLENGTH, 2)
        last = m; sub(/^.*\./, "", last)
        if ((before == "" || before !~ /[A-Za-z0-9_.\\\/<>-]/ || (RSTART > 2 && substr(t, RSTART - 2, 2) == "//")) &&
            after !~ /^[A-Za-z0-9_-]/ && after !~ /^\.[A-Za-z0-9_]/ && length(last) <= 24 && !(last in fileext))
            m = tag("host", m)
        out = out substr(t, 1, RSTART - 1) m
        t = substr(t, RSTART + RLENGTH)
    }
    return out t
}
pass == 1 { learn($0); next }
FNR == 1 { if (outfile != "") close(outfile); outfile = FILENAME ".masked" }
{
    t = $0
    if (match(t, /^[ \t]*[A-Za-z0-9_.]*([Pp][Aa][Ss][Ss][Ww][Oo][Rr][Dd]|[Ss][Ee][Cc][Rr][Ee][Tt]|[Tt][Oo][Kk][Ee][Nn]|[Kk][Ee][Yy])[A-Za-z0-9_]*[ \t]*[=:][ \t]*[^ \t]/))
        t = substr(t, 1, RLENGTH - 1) "(removed)"
    gsub(/--?[A-Za-z0-9-]*password[A-Za-z0-9-]*[ \t=]+[^ \t-][^ \t]*/, "--password (removed)", t)
    gsub(/[Ii]nvite code:?[ \t]*[A-Z2-9][A-Z2-9-][A-Z2-9-][A-Z2-9-][A-Z2-9-][A-Z2-9-][A-Z2-9-][A-Z2-9-][A-Z2-9-][A-Z2-9-]+/, "invite code: (removed)", t)
    gsub(/[0-9A-Fa-f][0-9A-Fa-f][:-][0-9A-Fa-f][0-9A-Fa-f][:-][0-9A-Fa-f][0-9A-Fa-f][:-][0-9A-Fa-f][0-9A-Fa-f][:-][0-9A-Fa-f][0-9A-Fa-f][:-][0-9A-Fa-f][0-9A-Fa-f]/, "<mac>", t)
    out = ""
    while (match(t, /[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+/)) {
        a = substr(t, RSTART, RLENGTH)
        before = substr(t, RSTART - 1, 1); after = substr(t, RSTART + RLENGTH, 2)
        k = (RSTART > 1 && before ~ /[0-9.vV]/) || after ~ /^\.[0-9]/ ? "" : kind(a)
        out = out substr(t, 1, RSTART - 1) (k == "" ? a : tag(k, a))
        t = substr(t, RSTART + RLENGTH)
    }
    t = out t
    if (match(t, /^[ \t]*server[ \t]*=[ \t]*[A-Za-z][A-Za-z0-9_.-]*/) && t !~ /=[ \t]*localhost([^A-Za-z0-9_]|$)/) {
        p = t; sub(/=[ \t]*[A-Za-z][A-Za-z0-9_.-]*/, "= <host>", p); t = p
    }
    t = hosts(t)
    gsub("/home/" user "(/|$)", "/home/<user>/", t)
    if (length(user) >= 5 && !(tolower(user) in common)) t = words(t, user, "<user>", 1)
    if (length(pc) >= 3 && !(tolower(pc) in common)) t = words(t, pc, "<pc>", 1)
    learn(t)
    for (i = 1; i <= nnames; i++) t = words(t, names[i], "<name>", 0)
    print t > outfile
}

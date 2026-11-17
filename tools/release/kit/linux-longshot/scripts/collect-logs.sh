#!/usr/bin/env bash
# Packs what is needed to see what went wrong into coop-logs-<time>.zip beside start-squadala.sh:
# the game's longshot_log.txt files, coop.ini, the kit's logs (start, build, game output),
# VERSION.txt, coop-options.ini, the puppet pack's manifest and a short note about this PC.
# bash start-squadala.sh --logs runs it. The player attaches that zip to /bugreport on the Squadala! Discord
# (the bot takes a .zip only, so a .zip it is; a .tar.gz only when this PC has no way to write a zip).
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
stamp=$(date +%Y%m%d-%H%M%S)
host=$(hostname 2>/dev/null || uname -n)
out=$KIT/coop-logs-$stamp.zip  # no PC name: it is masked inside too
stage=$(mktemp -d "${TMPDIR:-/tmp}/coop-logs-XXXXXX")
# Says at once that it runs, then one line per file (issue #67: a big game log takes a while to mask).
echo 'Collecting your logs (this can take a few minutes)...'
found=0
pick() {  # pick FROM TO
    if [[ -f $1 ]]; then
        mkdir -p "$stage/$(dirname "$2")"
        echo "  $2 ($(du -h -- "$1" 2>/dev/null | cut -f1))"
        cp -- "$1" "$stage/$2" && found=$((found + 1))
    else
        echo "Missing: ${1#"$KIT"/}"
    fi
}
game_logs=0
# The game's log: in data/game, else beside a longshot running now (a game started from elsewhere), and any a player
# copied into data/logs (what to do when none is found, at the end).
log_dirs=("$GAME")
for p in /proc/[0-9]*; do
    e=$(readlink "$p/exe" 2>/dev/null) || continue
    [[ ${e##*/} == longshot ]] && log_dirs+=("${e%/*}")
done
for d in "${log_dirs[@]}"; do
    # (Longshot rc.4 writes its log into logs/ there, older games beside the program)
    for f in "$d"/longshot_log*.txt; do [[ -f $f ]] && pick "$f" "game/${f##*/}" && game_logs=$((game_logs + 1)); done
    for f in "$d"/logs/longshot_log*.txt; do [[ -f $f ]] && pick "$f" "game/logs/${f##*/}" && game_logs=$((game_logs + 1)); done
    ((game_logs)) && break
done
for f in "$LOGS"/longshot_log*.txt; do [[ -f $f ]] && pick "$f" "game/copied/${f##*/}" && game_logs=$((game_logs + 1)); done
pick "$GAME/coop.ini" game/coop.ini
for f in "$LOGS"/*.log; do [[ -f $f ]] && pick "$f" "logs/${f##*/}"; done
pick "$KIT/VERSION.txt" VERSION.txt
pick "$DATA/coop-options.ini" coop-options.ini
pick "$GAME/mods/contents/0400486024DEC000/coop-puppet/coop_puppet.json" game/coop_puppet.json
# OOTRR's output when a randomizer seed failed (masked below like every file)
[[ -f $GAME/mods/oot-remake-randomizer/last-build.txt ]] && pick "$GAME/mods/oot-remake-randomizer/last-build.txt" game/last-build.txt
{
    echo "pc: $host"
    echo "collected: $(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "system: $(uname -srm)"
    [[ -f /etc/os-release ]] && . /etc/os-release && echo "distribution: ${PRETTY_NAME:-$NAME}"
    echo "glibc: $(getconf GNU_LIBC_VERSION 2>/dev/null)"
    echo "session: ${XDG_SESSION_TYPE:-?} ${XDG_CURRENT_DESKTOP:-} ${SteamDeck:+SteamDeck=$SteamDeck}"
    command -v lspci >/dev/null && lspci 2>/dev/null | grep -Ei 'vga|3d|display' | sed 's/^/gpu: /'
    command -v vulkaninfo >/dev/null && vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverInfo|apiVersion' | sed 's/^[[:space:]]*/vulkan: /'
    command -v ip >/dev/null && ip -4 -o addr show 2>/dev/null | awk '$4 !~ /^169\.254\./ { print "ip: " $4 " (" $2 ")" }'
    df -h "$DATA" 2>/dev/null | tail -n1 | sed 's/^/disk: /'
    # Memory now (issue 85: a host's game crashed with the commit limit nearly used up): RAM, swap,
    # the commit charge and its limit, and the 10 programs using the most memory (resident, summed
    # by name; names only, masked below like the rest).
    awk '/^(MemTotal|MemAvailable|SwapTotal|SwapFree|CommitLimit|Committed_AS):/ { v[substr($1, 1, length($1) - 1)] = $2 / 1048576 }
         END { printf "memory now: RAM %.1f GB available of %.1f GB; swap %.1f GB free of %.1f GB; commit %.1f GB used of %.1f GB limit\n",
               v["MemAvailable"], v["MemTotal"], v["SwapFree"], v["SwapTotal"], v["Committed_AS"], v["CommitLimit"] }' /proc/meminfo 2>/dev/null
    command -v ps >/dev/null && ps -eo comm=,rss= 2>/dev/null |
        awk '{ n = NF - 1; name = $1; for (i = 2; i <= n; i++) name = name " " $i; kb[name] += $NF; c[name]++ }
             END { for (k in kb) printf "%d\t%s\t%d\n", kb[k], k, c[k] }' |
        sort -rn | head -n 10 | awk -F '\t' '{ printf "top memory: %s (%d running), %.2f GB resident\n", $2, $3, $1 / 1048576 }'
    kit_game_running && echo 'running: the game'
} >"$stage/about-this-pc.txt"
# What identifies a player is masked (lib.sh kit_mask, scripts/log-mask.awk, as the Windows kit's log-mask.ps1): a set
# password and an invite code become (removed), MAC addresses <mac>, each IPv4 address its kind with a number (<lan-1>,
# <public-1>, ...; the same address the same tag in every file), host names <host>/<host-1>, the user name <user>, the
# PC name <pc> and players' names <name> (learnt in any of the files). Crash dumps stay as they are.
echo 'Masking addresses and names in the copies, then packing them...'
mapfile -d '' masked < <(find "$stage" -type f ! -path '*/crash_dumps/*' -print0)
kit_mask "${masked[@]}" || true   # (a file it could not mask is left out: never an unmasked file in the zip)
# A zip: python3 (the system's, else the one the first start set up), else zip, else bsdtar. None of them: a .tar.gz.
py=$(command -v python3 || true)
[[ -n $py ]] || py=$DATA/builder/image/usr/python/bin/python3
if [[ -x $py ]]; then
    "$py" - "$out" "$stage" <<'ZIP'
import os, sys, zipfile
out, root = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, root))
ZIP
elif command -v zip >/dev/null 2>&1; then
    (cd "$stage" && zip -qr "$out" .)
elif command -v bsdtar >/dev/null 2>&1; then
    bsdtar -a -cf "$out" -C "$stage" .
else
    out=${out%.zip}.tar.gz
    tar -czf "$out" -C "$stage" .
fi
rm -rf "$stage"
echo "Wrote $out ($((found + 1)) files, $game_logs of them game logs)."
echo 'IP addresses, host names, MAC addresses, passwords, the invite code, your user, PC and players'"'"' names are masked in it.'
if ((game_logs == 0)); then
    echo
    # (as in the zip: the user name masked)
    u=$(id -un 2>/dev/null)
    {
        echo "The game's log (longshot_log.txt) is not in this file: none in $GAME or $GAME/logs."
    if [[ -x $GAME/longshot ]]; then
        echo '  Start the game once (it writes the log when it starts), close it, then run this again (bash start-squadala.sh --logs).'
    else
        echo '  The game was not found there: it is built by the first start (bash start-squadala.sh). Its log is beside the game.'
    fi
    echo "  Found it somewhere else? Copy longshot_log.txt into $LOGS and run this again, or send it with this file."
    } | sed -e "s#/home/$u/#/home/<user>/#g"
fi
echo
echo 'To report it: on the Squadala! Discord (https://discord.gg/GwtRKAqng5) type /bugreport, say what happened'
echo 'and attach this file. Do not attach it to a public issue.'
[[ $out == *.zip ]] || echo 'The bot takes a .zip only: zip this file first (no python3, zip or bsdtar here to do it).'
echo "Send this file: $out"

#!/usr/bin/env bash
# Crash reports, and sending them by themselves (docs/problem-reports.md): the Linux twin of the Windows kit's
# scripts/send-report.ps1. Opt-in, asked once (default no).
#   bash start-squadala.sh --send-reports yes|no|ask   change data/coop-options.ini's send_reports
#   bash scripts/send-report.sh --at-start             what start.sh runs before it starts the game
#   bash scripts/send-report.sh --upload ZIP KIND      the background sender (one zip)
#
# At each start it looks at the game's finished sessions not looked at before (the newest 2 of the last 7 days), from
# the end of each log only (its last 512 KB):
#   crash    Longshot's crash handler or a fatal error wrote 'CRASH: exception' / 'FATAL:' (host/mkw_glue.c,
#            hos_thread.c, gpu/vk/vk_replay.cpp in Longshot's source)
#   unclean  neither that nor a clean exit ('control: exiting', 'display: window closed, ...': host/control.c,
#            host/hos_relaunch.c): the game was ended from outside (killed, the PC turned off) or died silently
#   clean    a clean exit: nothing to report
# For a crash or an unclean end it writes data/logs/crash-report-<date>-<kind>.zip: the log's crash lines, its start and
# its end, masked as collect-logs.sh masks them (lib.sh kit_mask: addresses, host names, MAC addresses, the user, PC and
# players' names, invite codes, passwords; the names learnt from the whole log).
# Sending: send_reports = ask (the default: the first report asks once on a terminal, no after 20 s, and keeps the
# answer), yes or no. With yes each new crash report is sent over HTTPS (curl) to the relay named in
# scripts/report-relay.txt (the kit's own file; empty: nothing is sent, whatever the answer), in the background, so the
# start never waits. The relay passes it to the developers' Discord channel; nothing goes anywhere else. Sent zips are
# listed in data/logs/reports-sent.txt, failures in data/logs/report-sending.log.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

REPORT_TAIL_BYTES=524288   # where a session's end is looked for (the crash handler's lines are its last)
REPORT_HEAD_LINES=60
REPORT_TAIL_LINES=300
REPORT_CRASH_LINES_MAX=150
REPORT_MAX_AGE_DAYS=7
REPORT_LOGS_PER_START=2
REPORT_ZIPS_KEPT=10
report_crash_line='] CRASH: |(^|] |vk: )FATAL: '
report_crash_start='] CRASH: exception|(^|] |vk: )FATAL: '
report_clean_exit='] control: exiting|] display: window closed'
# A relay URL: https only (plain http to 127.0.0.1 is for the tests' own listener).
report_relay_re='^(https://[A-Za-z0-9.-]+|http://127\.0\.0\.1)(:[0-9]{1,5})?/[A-Za-z0-9_./-]*$'
REPORT_STATE=$LOGS/report-sending.state
REPORT_SENT=$LOGS/reports-sent.txt
OPTIONS=$DATA/coop-options.ini

# The relay's URL from scripts/report-relay.txt: its first line that is not a comment, when it is an https:// URL; else
# nothing (nothing is sent). A release can empty it (the kill switch), and the updater replaces this file.
report_relay() {
    local line
    [[ -f $KIT/scripts/report-relay.txt ]] || return 0
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        line=${line#"${line%%[![:space:]]*}"}; line=${line%"${line##*[![:space:]]}"}
        [[ -z $line || $line == '#'* ]] && continue
        [[ $line =~ $report_relay_re ]] && printf '%s\n' "$line"
        return 0
    done <"$KIT/scripts/report-relay.txt"
}

# coop-options.ini's send_reports: ask (the default, and for a value that is not one of the three), yes or no.
report_choice() {
    local v
    v=$(sed -n 's/^[[:space:]]*send_reports[[:space:]]*=[[:space:]]*\([A-Za-z]*\).*/\1/p' "$OPTIONS" 2>/dev/null | tail -n1)
    case ${v,,} in yes | no) printf '%s\n' "${v,,}" ;; *) echo ask ;; esac
}

# Writes send_reports = VALUE into coop-options.ini: over its line (also one behind a '#'), else at the end.
report_set_choice() {
    mkdir -p "$(dirname "$OPTIONS")"
    [[ -f $OPTIONS ]] || : >"$OPTIONS"
    awk -v v="$1" '/^[ \t]*#?[ \t]*send_reports[ \t]*=/ {
            if (done) { sub(/^[ \t#]*/, "# "); print } else { print "send_reports = " v; done = 1 }
            next
        }
        { print }
        END { if (!done) printf "\nsend_reports = %s\n", v }' "$OPTIONS" >"$OPTIONS.new" && mv -f "$OPTIONS.new" "$OPTIONS"
}

# The kit's version for the relay: VERSION.txt's '(release v0.11.4)', else v0.0.0-dev (a kit built for testing).
report_version() {
    local v
    v=$(kit_version)
    if [[ $v =~ ^v?[0-9]+\.[0-9]+(\.[0-9]+)?([-+][0-9A-Za-z.-]{1,32})?$ ]]; then printf '%s\n' "$v"; else echo v0.0.0-dev; fi
}

# The game's logs, newest first: in data/game (and its logs/, Longshot rc.4 and newer) and any a player copied into
# data/logs. The log of a game running now is not finished: left for the next start.
report_logs() {
    local f running=0
    kit_game_running && running=1
    for f in "$GAME"/longshot_log*.txt "$GAME"/logs/longshot_log*.txt "$LOGS"/longshot_log*.txt; do
        [[ -f $f ]] || continue
        ((running)) && [[ ${f##*/} == longshot_log.txt && $f != "$LOGS"/* ]] && continue
        printf '%s %s\n' "$(stat -c %Y -- "$f")" "$f"
    done | sort -rn | cut -d' ' -f2-
}

# A session's key (as the Windows kit's Get-ProblemRunKey): its run_id, else the file's size and the SHA-1 of its
# first 64 KB.
report_run_key() {
    local id
    id=$(head -c 65536 -- "$1" | grep -a -o -m1 'run_id: [0-9a-f]\{16,\}' | head -n1)
    if [[ -n $id ]]; then printf '%s\n' "${id#run_id: }"; return; fi
    printf 'file-%s-%s\n' "$(stat -c %s -- "$1")" "$(head -c 65536 -- "$1" | sha1sum | cut -c1-20)"
}

# report_end LOG TAILFILE: writes the log's last REPORT_TAIL_BYTES (from its first whole line) into TAILFILE and says how
# the session ended: crash, unclean or clean.
report_end() {
    local size
    size=$(stat -c %s -- "$1")
    if ((size > REPORT_TAIL_BYTES)); then tail -c "$REPORT_TAIL_BYTES" -- "$1" | tail -n +2; else cat -- "$1"; fi | tr -d '\r' >"$2"
    if grep -aEq -- "$report_crash_start" "$2"; then echo crash
    elif grep -aEq -- "$report_clean_exit" "$2"; then echo clean
    else echo unclean; fi
}

# zip_one ZIP FILE: FILE alone in ZIP (Python's zipfile, else zip). Fails when neither is here.
zip_one() {
    local py=$DATA/builder/image/usr/python/bin/python3
    [[ -x $py ]] || py=$(command -v python3 || true)
    rm -f -- "$1"
    if [[ -n $py ]]; then
        "$py" -I -B -c 'import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    z.write(sys.argv[2], "crash-report.txt")' "$1" "$2" 2>/dev/null && return 0
    fi
    command -v zip >/dev/null 2>&1 && zip -q -j "$1" "$2" >/dev/null && return 0
    rm -f -- "$1"
    return 1
}

# report_make LOG KIND TAILFILE: the crash report for one log; prints the zip's path.
report_make() {
    local log=$1 kind=$2 tailf=$3 stage what written start zip
    written=$(date -d "@$(stat -c %Y -- "$log")" '+%Y%m%d-%H%M%S')
    stage=$(mktemp -d "${TMPDIR:-/tmp}/crash-report-XXXXXX") || return 1
    if [[ $kind == crash ]]; then what='the game crashed (its crash lines are below)'
    else what='the game ended without closing (no crash line and no clean exit: ended from outside, the PC turned off, or a silent crash)'; fi
    start=$(grep -aEn -m1 -- "$report_crash_start" "$tailf" | cut -d: -f1)
    {
        echo "Squadala! crash report: $what"
        echo "Kit: $(report_version) (Linux)"
        echo "Log: ${log##*/}, last written $(date -d "@$(stat -c %Y -- "$log")" '+%Y-%m-%d %H:%M')"
        echo "Addresses, host names, user, PC and players' names, codes and passwords are masked (as collect-logs masks them)."
        echo
        if [[ -n $start ]]; then
            tail -n "+$start" -- "$tailf" | grep -aE -- "$report_crash_line" | head -n "$REPORT_CRASH_LINES_MAX" >"$stage/crash"
        else
            : >"$stage/crash"
        fi
        echo "--- the crash ($(wc -l <"$stage/crash") lines)"
        cat -- "$stage/crash"
        echo
        echo "--- the start of the log (first $REPORT_HEAD_LINES lines)"
        head -n "$REPORT_HEAD_LINES" -- "$log" | tr -d '\r'
        echo
        echo "--- the end of the log (last $(tail -n "$REPORT_TAIL_LINES" -- "$tailf" | wc -l) lines)"
        tail -n "$REPORT_TAIL_LINES" -- "$tailf"
    } >"$stage/crash-report.txt"
    rm -f -- "$stage/crash"
    zip=$LOGS/crash-report-$written-$kind.zip
    if kit_mask --learn "$log" "$stage/crash-report.txt" && zip_one "$zip" "$stage/crash-report.txt"; then
        rm -rf -- "$stage"
        # (the newest REPORT_ZIPS_KEPT are kept)
        ls -1 -- "$LOGS"/crash-report-*.zip 2>/dev/null | sort -r | tail -n "+$((REPORT_ZIPS_KEPT + 1))" |
            while IFS= read -r f; do rm -f -- "$f"; done
        printf '%s\n' "$zip"
        return 0
    fi
    rm -rf -- "$stage"
    return 1
}

# report_send URL ZIP KIND VERSION: one zip to the relay (POST, application/zip, the kind and the kit's version in
# headers). Prints the HTTP status, 000 when it could not be reached.
report_send() {
    [[ $1 =~ $report_relay_re ]] || { echo 000; return; }
    command -v curl >/dev/null 2>&1 || { echo 000; return; }
    curl -sS --max-time 60 -o /dev/null -w '%{http_code}' -H 'Content-Type: application/zip' \
        -H "X-Squadala-Kind: $3" -H "X-Squadala-Kit: $4" --data-binary "@$2" "$1" 2>/dev/null || true
}

report_at_start() {
    local url choice log key kind tailf zip answer i looked=0 age now
    local -a made=() kinds=() state=()
    mkdir -p "$LOGS" || return 0
    [[ -f $REPORT_STATE ]] && mapfile -t state <"$REPORT_STATE"
    now=$(date +%s)
    tailf=$(mktemp "${TMPDIR:-/tmp}/crash-tail-XXXXXX") || return 0
    while IFS= read -r log; do
        [[ -n $log ]] || continue
        key=$(report_run_key "$log") || continue
        printf '%s\n' "${state[@]}" | grep -qxF -- "$key" && continue
        age=$(((now - $(stat -c %Y -- "$log")) / 86400))
        if ((age > REPORT_MAX_AGE_DAYS)); then state+=("$key"); continue; fi
        ((looked >= REPORT_LOGS_PER_START)) && continue   # (looked at next time)
        looked=$((looked + 1))
        kind=$(report_end "$log" "$tailf")
        if [[ $kind != clean ]]; then
            if zip=$(report_make "$log" "$kind" "$tailf"); then
                made+=("$zip"); kinds+=("$kind")
            else
                say "A crash report could not be written (no python3 or zip to pack it): send your logs with bash start-squadala.sh --logs."
            fi
        fi
        state+=("$key")
    done < <(report_logs)
    rm -f -- "$tailf"
    printf '%s\n' "${state[@]}" | grep -v '^$' | tail -n 200 >"$REPORT_STATE.new" && mv -f "$REPORT_STATE.new" "$REPORT_STATE"
    ((${#made[@]})) || return 0
    for i in "${!made[@]}"; do
        if [[ ${kinds[i]} == crash ]]; then say "Your last game crashed. A crash report (masked): ${made[i]}"
        else say "Your last game ended without closing. A crash report (masked): ${made[i]}"; fi
    done
    url=$(report_relay)
    if [[ -z $url ]]; then say 'Please send it with /bugreport in the Squadala! Discord.'; return 0; fi
    choice=$(report_choice)
    if [[ $choice == ask ]]; then
        kit_tty || return 0   # (no terminal: asked at a start that has one)
        say ''
        say 'Reports: the kit can send its crash reports to the Squadala! developers by itself. They hold parts of your'
        say "game log with addresses, host names, user, PC and players' names and codes masked, go to the developers'"
        say 'Discord channel only, and are never sent anywhere else. Change it later: bash start-squadala.sh'
        say '--send-reports yes (or no), or send_reports in data/coop-options.ini.'
        answer=''
        read -r -t 20 -n 1 -p 'Send reports by themselves? [y/N] (no in 20 s) ' answer || true
        echo
        if [[ $answer == [yY] ]]; then choice=yes; else choice=no; fi
        report_set_choice "$choice"
    fi
    if [[ $choice != yes ]]; then say 'You can send it yourself with /bugreport in the Squadala! Discord.'; return 0; fi
    for i in "${!made[@]}"; do
        nohup bash "$KIT/scripts/send-report.sh" --upload "${made[i]}" "${kinds[i]}" >/dev/null 2>&1 </dev/null &
    done
    say "Sending ${#made[@]} report(s) to the developers in the background (send_reports = yes)."
}

report_upload() {
    local url code stamp why
    url=$(report_relay)
    [[ -n $url && $(report_choice) == yes && -f $1 ]] || return 0
    code=$(report_send "$url" "$1" "$2" "$(report_version)")
    stamp=$(date '+%Y-%m-%d %H:%M:%S')
    if [[ $code == 2[0-9][0-9] ]]; then echo "${1##*/} $stamp" >>"$REPORT_SENT"
    else
        why="HTTP $code"; command -v curl >/dev/null 2>&1 || why="no curl"
        echo "$stamp ${1##*/} not sent ($why)" >>"$LOGS/report-sending.log"
    fi
}

[[ ${BASH_SOURCE[0]} == "$0" ]] || return 0   # (sourced: the functions only, for the tests)

case ${1-} in
    yes | no | ask)
        report_set_choice "$1"
        case $1 in
            yes) say 'Reports are sent by themselves now (send_reports = yes in data/coop-options.ini).' ;;
            no) say 'Reports are not sent (send_reports = no). Crash reports stay in data/logs for /bugreport.' ;;
            ask) say 'The kit asks again at the next report (send_reports = ask).' ;;
        esac
        [[ -n $(report_relay) ]] || say 'This kit has no relay set (scripts/report-relay.txt is empty): nothing is sent.'
        ;;
    --at-start) report_at_start ;;
    --upload) [[ -n ${2-} && ${3-} =~ ^(crash|unclean)$ ]] && report_upload "$2" "$3" ;;
    *) say 'Usage: bash start-squadala.sh --send-reports yes | no | ask'; exit 2 ;;
esac
exit 0

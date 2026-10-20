#!/usr/bin/env bash
# Live dashboard for the bench Wii's queue: who has the Wii, what is queued and running, and
# everything that went wrong, on this workstation and (through the lease server) every other.
# Usage: bash tools/wii-bench/monitor.sh [refresh_seconds] [--page=NAME] [--hours=N] [--once]
#                                        [--sort=SECTION:FIELD[:DIR]] [--filter=SECTION:TEXT]
#        (default refresh: 3s; 1-4, n/p, or a click on a tab to change page, q to quit)
#
#   1 queue    the Wii's owner now, this workstation's running and queued jobs, every
#              workstation waiting for the Wii, and the last jobs here. The default page.
#   2 errors   every problem in the window (24 h by default), newest first: failed and timed
#              out jobs, a Wii left out of HBC, an expired lease, a workstation that left the
#              line, the lease server unreachable, a dispatcher that crashed.
#   3 history  per workstation over the window: jobs, failures, Wii minutes, turns and waits,
#              and the longest waits for the Wii.
#   4 log      the end of this workstation's dispatcher.log.
#   5 manage   every job that is running, waiting or queued, on every workstation, to cancel or
#              stop: one, an agent's, a workstation's, or all; and give up waiting for a frozen
#              Wii to come back. Each asks y first.
#
# IT NEVER TOUCHES THE WII. Everything comes from this workstation's queue files and
# dispatcher.log, and from the lease server's status and history: the queue's rule that
# nothing reaches the Wii during a run holds for a dashboard left open all day too. The
# Wii's state is what the queue last recorded (a job's hbc_back_s, a release that left it
# out of HBC), never a fresh probe.
#
# The drawing (redraw in place, pages, clickable tabs, click-to-sort headers, click-to-copy
# cells) is lib/monitor_lib.sh, a verbatim copy of the AI server's /ai/bin/lib/monitor_lib.sh
# (see README.md for how to refresh it). The data comes from one `wiibench.py snapshot` per
# refresh, which also does the sorting and filtering, so jq is not needed on any OS.

if [[ "$*" == *"--help"* || " $* " == *" -h "* ]]; then
    cat << 'EOF'
monitor.sh -- live dashboard for the bench Wii's queue: the Wii's owner, this
workstation's jobs, every workstation waiting, and every problem in the window.

PURPOSE
  See who has the Wii and what is waiting for it, on every workstation.
  See what is queued and running on this workstation.
  Find failed jobs, a Wii left out of HBC, and lease or dispatcher trouble.

HOW IT WORKS
  The dashboard has 4 pages. Press a number key, or click a tab, to switch.
  Click a column header to sort by it (ascending, descending, off).
  Click a cell to copy its full value (a job id, a whole error) to the
  clipboard.
  It never contacts the Wii: it reads the queue's files and the lease server.

USAGE
  bash tools/wii-bench/monitor.sh [refresh_seconds] [--page=NAME] [--hours=N]
                                  [--once] [--sort=SECTION:FIELD[:DIR]]
                                  [--filter=SECTION:TEXT]

OPTIONS
  refresh_seconds   Redraw interval, in seconds. Default: 3.
  --page=NAME       Page to open: queue, errors, history, log or manage.
                    Default: queue.
  --hours=N         The window for errors and history. Default: 24.
  --once            Print one frame and exit (plain text when not a terminal).
  --sort=SECTION:FIELD[:DIR]
                    Sort a section by FIELD. DIR: a/asc (default) or d/desc.
  --filter=SECTION:TEXT
                    Show only rows of SECTION containing TEXT (any column,
                    ignoring case).

SECTIONS AND FIELDS
  wiis      name ip state line last
  running   id name agent started elapsed timeout left wii
  waiting   place host name since waited
  pending   turn host id name agent age wii
  done      finished id name exit secs hbc chained wii
  errors    when severity kind host job id detail
  hosts     host jobs failed wii_min turns wait_med wait_max
  waits     when waited host name
  manage    state turn host id name agent folder age

KEYS
  1-5 page   n/p or Tab next/previous page   q quit   any other key: refresh now
  On the manage page: click a job (or j/k, or the arrow keys) to select it, then
    c   cancel it (stop it, if it is running)
    a   cancel every queued job of its agent
    w   cancel every queued job of its workstation
    X   cancel every queued job, on every workstation
    g   give up waiting for a frozen Wii to come back (its data is not saved)
  Each asks y first. Another workstation's job is cancelled by its own
  dispatcher, through the lease server, within a few seconds.

NEEDS
  bash 4.3 or newer (macOS: brew install bash), Python 3.8+, a terminal
  with mouse reporting for clicks (Windows Terminal, iTerm2, most xterms).
  The state folder and lease server are wiibench.py's: $WII_BENCH_HOME and
  $WII_BENCH_SERVER, else what `wiibench.py setup` wrote.
EOF
    exit 0
fi

if [ -z "${BASH_VERSINFO:-}" ] || (( BASH_VERSINFO[0] < 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] < 3) )); then
    echo "monitor.sh needs bash 4.3 or newer; this is ${BASH_VERSION:-not bash}." >&2
    echo "On macOS: brew install bash, then run it with that bash (\$(brew --prefix)/bin/bash $0)." >&2
    exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="$HERE/wiibench.py"
# shellcheck source=lib/monitor_lib.sh
source "$HERE/lib/monitor_lib.sh"

PY=""
for p in ${WII_BENCH_PYTHON:-} python3 python; do
    # The Windows Store's python3 stub exists but runs nothing; ask each one for a version.
    if command -v "$p" >/dev/null 2>&1 && "$p" -c 'import sys; sys.exit(sys.version_info < (3, 8))' 2>/dev/null; then
        PY="$p"; break
    fi
done
[ -n "$PY" ] || { echo "monitor.sh needs Python 3.8 or newer on PATH (or \$WII_BENCH_PYTHON)." >&2; exit 2; }

PAGE="queue"; HOURS=24; ONCE=0; REST=()
for a in "$@"; do
    case "$a" in
        --page=*)  PAGE="${a#--page=}" ;;
        --hours=*) HOURS="${a#--hours=}" ;;
        --once)    ONCE=1 ;;
        *)         REST+=("$a") ;;
    esac
done
declare -A SORT_FIELD SORT_DESC FILTER_VAL
MONITOR_DEFAULT_REFRESH=3 monitor_parse_args "" "${REST[@]+"${REST[@]}"}"
[[ "$HOURS" =~ ^[0-9]+([.][0-9]+)?$ ]] && [ "$HOURS" != "0" ] || { echo "monitor.sh: --hours takes a number of hours, not '$HOURS'" >&2; exit 2; }
[[ "$REFRESH" =~ ^[0-9]+([.][0-9]+)?$ ]] || { echo "monitor.sh: the refresh is a number of seconds, not '$REFRESH'" >&2; exit 2; }
monitor_pages_init "$PAGE" queue errors history log manage

ERRFILE="$(mktemp 2>/dev/null || echo "${TMPDIR:-/tmp}/wiibench-monitor.$$")"
declare -A KV ROWS NROWS

# NO FORKS PER FRAME. On Git Bash a fork costs 14-23 ms (measured 2026-10-02), and the first
# version forked about 165 times a frame: `tput cup` and `tput el` for every line in the
# library's monitor_draw_frame, and a "$(...)" for every looked-up value, trimmed cell, heading
# and row. A frame took about 4 s to appear. Everything below sets variables instead (printf -v,
# fork-free twins of the library's text helpers), the frame goes out in one write, and the data
# comes from one long-running snapshot process. lib/monitor_lib.sh stays a verbatim copy: the
# two library functions that fork per line or per cell are redefined here, after it is sourced.

# ---------------------------------------------------------------- data: the snapshot coprocess

# One `wiibench.py snapshot --serve` answers every refresh: no Python start per frame, the
# lease server's name looked up once, and only what changed read again (see SnapCache).
SNAP_PID=""
# Stop the coprocess and reap it: killed and not waited for, it stays a zombie wherever
# nothing reaps orphans (a container's first process often does not).
snap_stop() {
    [ -n "${SNAP_PID:-}" ] || return 0
    kill "$SNAP_PID" 2>/dev/null
    wait "$SNAP_PID" 2>/dev/null
    SNAP_PID=""
}
snap_start() {
    snap_stop                                                    # one that hung
    coproc SNAP { exec "$PY" "$BENCH" snapshot --serve --hours "$HOURS" 2>"$ERRFILE"; }
}

# collect -- one snapshot: KV[key]=value, ROWS[section:i]=tab-separated fields, NROWS[section].
# Sorting and filtering happen in the snapshot (SORT_FIELD/SORT_DESC/FILTER_VAL passed along).
collect() {
    local sorts="" filters="" sec line rest n tries
    for sec in "${!SORT_FIELD[@]}"; do
        [ -n "${SORT_FIELD[$sec]}" ] && sorts+="$sec:${SORT_FIELD[$sec]}:${SORT_DESC[$sec]:-0}"$'\x1f'
    done
    for sec in "${!FILTER_VAL[@]}"; do
        [ -n "${FILTER_VAL[$sec]}" ] && filters+="$sec:${FILTER_VAL[$sec]}"$'\x1f'
    done
    KV=(); ROWS=(); NROWS=()
    for tries in 1 2; do                           # a coprocess that died is started again, once
        if [ "$tries" = 2 ] || [ -z "${SNAP_PID:-}" ] || [ -z "${SNAP[1]:-}" ] || ! kill -0 "$SNAP_PID" 2>/dev/null; then
            snap_start
        fi
        printf '%s\t%s\t%s\n' "$HOURS" "$sorts" "$filters" >&"${SNAP[1]}" 2>/dev/null || continue
        # A timeout on the first line only: `read -t` waits in select() before every read, and
        # on a Git Bash pipe that is polled, about 1.6 ms a line (400 ms a snapshot, measured
        # 2026-10-02; 75 ms without). Once the answer has started, a plain read cannot hang:
        # a snapshot that dies closes the pipe.
        IFS= read -r -t 60 line <&"${SNAP[0]}" || continue
        while :; do
            line="${line%$'\r'}"
            case "$line" in
                "@end") return 0 ;;
                "@kv"$'\t'*)
                    rest="${line#@kv$'\t'}"; KV["${rest%%$'\t'*}"]="${rest#*$'\t'}" ;;
                "@row"$'\t'*)
                    rest="${line#@row$'\t'}"; sec="${rest%%$'\t'*}"; n="${NROWS[$sec]:-0}"
                    ROWS["$sec:$n"]="${rest#*$'\t'}"; NROWS[$sec]=$(( n + 1 )) ;;
            esac
            IFS= read -r line <&"${SNAP[0]}" || break
        done
        KV=(); ROWS=(); NROWS=()                   # no @end: it died or hung; start a new one
    done
    return 1
}

# collect_once -- the same, from a one-shot `snapshot` (for --once: no coprocess to manage)
collect_once() {
    local sorts=() sec line rest n
    for sec in "${!SORT_FIELD[@]}"; do
        [ -n "${SORT_FIELD[$sec]}" ] && sorts+=(--sort "$sec:${SORT_FIELD[$sec]}:${SORT_DESC[$sec]:-0}")
    done
    for sec in "${!FILTER_VAL[@]}"; do
        [ -n "${FILTER_VAL[$sec]}" ] && sorts+=(--filter "$sec:${FILTER_VAL[$sec]}")
    done
    KV=(); ROWS=(); NROWS=()
    while IFS= read -r line; do
        line="${line%$'\r'}"
        case "$line" in
            "@kv"$'\t'*)
                rest="${line#@kv$'\t'}"; KV["${rest%%$'\t'*}"]="${rest#*$'\t'}" ;;
            "@row"$'\t'*)
                rest="${line#@row$'\t'}"; sec="${rest%%$'\t'*}"; n="${NROWS[$sec]:-0}"
                ROWS["$sec:$n"]="${rest#*$'\t'}"; NROWS[$sec]=$(( n + 1 )) ;;
        esac
    done < <("$PY" "$BENCH" snapshot --hours "$HOURS" "${sorts[@]+"${sorts[@]}"}" 2>"$ERRFILE")
}

# k <key> [default] -- sets V to KV[key] ("-", the snapshot's empty, reads as empty)
k() { V="${KV[$1]:-}"; [ "$V" = "-" ] && V=""; [ -z "$V" ] && V="${2:-}"; :; }

# fit <text> <width> -- sets V to text trimmed to width with a trailing ~
fit() {
    local w="$2"
    (( w < 2 )) && w=2
    if (( ${#1} > w )); then V="${1:0:$(( w - 1 ))}~"; else V="$1"; fi
}

# ---------------------------------------------------------------- fork-free drawing

# The library's monitor_draw_frame runs `tput cup` and `tput el` per line. The same in-place
# redraw, with the escape codes tput prints for an xterm-like terminal (every terminal the
# library supports), built into one string and written once: cursor to row N, the line, clear
# to the end of it; then clear everything below the last line. No line ends in a newline, so a
# frame exactly as tall as the screen does not scroll it.
monitor_draw_frame() {
    local -n _lines=$1
    local out="" i=0 line max="${ROWS_MAX:-0}"
    for line in "${_lines[@]}"; do
        (( max > 0 && i >= max )) && break
        out+=$'\e['"$(( i + 1 ))"$';1H'"${line}"$'\e[K'
        (( i++ ))
    done
    out+=$'\e['"$(( i + 1 ))"$';1H\e[J'
    printf '%s' "$out"
}

# The library's monitor_header_colspans, assigning instead of printing (same layout rule:
# a %s field is max(declared width, label length) wide), so a row's cells cost no fork.
colspans() {
    local fmt="$1" sort_field="$2" sort_desc="$3"; shift 3
    local -a fields=() labels=()
    local colspec field label rest="$fmt" idx=0 col=1 pre num width
    for colspec in "$@"; do
        field="${colspec%%:*}"; label="${colspec#*:}"
        if [ -n "$sort_field" ] && [ "$field" = "$sort_field" ]; then
            if [ "$sort_desc" -eq 1 ]; then label="${label} v"; else label="${label} ^"; fi
        fi
        fields+=("$field"); labels+=("$label")
    done
    V=""
    while [[ "$rest" =~ ^([^%]*)%-?([0-9]*)s(.*)$ ]]; do
        pre="${BASH_REMATCH[1]}"; num="${BASH_REMATCH[2]}"; rest="${BASH_REMATCH[3]}"
        col=$(( col + ${#pre} ))
        width="${num:-0}"
        [ "${#labels[$idx]}" -gt "$width" ] && width=${#labels[$idx]}
        V+="${fields[$idx]}:${col}:$(( col + width - 1 )) "
        col=$(( col + width ))
        (( idx++ ))
    done
}

# The library's monitor_register_row_cells, with colspans above instead of its "$(...)".
monitor_register_row_cells() {
    local row="$1" fmt="$2" colspec; shift 2
    for colspec in "$@"; do MONITOR_CELL_VALUES["${row}:${colspec%%:*}"]="${colspec#*:}"; done
    colspans "$fmt" "" 0 "$@"
    MONITOR_CELL_COLS["$row"]="$V"
}

# Fork-free twins of the library's monitor_section_heading and monitor_placeholder: they
# append the line to `lines` themselves.
heading() {
    local color="$1" title="$2" subtitle="${3:-}" sec="${4:-}"
    local out="${c_bold}${color}${title}${c_reset}"
    [ -n "$subtitle" ] && out+="  ${c_gray}(${subtitle})${c_reset}"
    [ -n "$sec" ] && [ -n "${FILTER_VAL[$sec]:-}" ] && out+="  filter: ${FILTER_VAL[$sec]}"
    lines+=("$out")
}
placeholder() { lines+=("  ${c_gray}$1${c_reset}"); }

# table_header SECTION FMT field:LABEL ...   (same format string as the rows: columns line up)
table_header() {
    local sec="$1" fmt="$2"; shift 2
    local sf="${SORT_FIELD[$sec]:-}" sd="${SORT_DESC[$sec]:-0}" spec field label h
    local -a labels=()
    MONITOR_HEADER_ROW[$sec]=$(( ${#lines[@]} + 1 ))
    colspans "$fmt" "$sf" "$sd" "$@"
    MONITOR_HEADER_COLS[$sec]="$V"
    for spec in "$@"; do                           # monitor_render_header's sort arrows
        field="${spec%%:*}"; label="${spec#*:}"
        if [ -n "$sf" ] && [ "$field" = "$sf" ]; then
            if [ "$sd" -eq 1 ]; then label+=" v"; else label+=" ^"; fi
        fi
        labels+=("$label")
    done
    printf -v h "$fmt" "${labels[@]}"
    lines+=("${c_bold}${h}${c_reset}")
}

# table_row FMT COLOR field:shown ...  -- one row, cells clickable. Sets ROW_AT (its screen
# row) so a caller can make a cell copy the full value instead of the trimmed one shown.
table_row() {
    local fmt="$1" color="$2" spec out; shift 2
    local -a vals=()
    for spec in "$@"; do vals+=("${spec#*:}"); done
    ROW_AT=$(( ${#lines[@]} + 1 ))
    monitor_register_row_cells "$ROW_AT" "$fmt" "$@"
    printf -v out "$fmt" "${vals[@]}"
    lines+=("${color}${out}${c_reset}")
}

# row_fields SECTION I -- splits ROWS[SECTION:I] into the array F
row_fields() { IFS=$'\t' read -r -a F <<< "${ROWS[$1:$2]}"; }

# name_width FIXED -- sets W: what is left of the screen for a name column, at least 16
name_width() { W=$(( COLS - $1 )); (( W < 16 )) && W=16; :; }

# ---------------------------------------------------------------- the header

MONITOR_HEADER_TITLE="=== WII BENCH ==="

monitor_prepare_header() {
    printf -v MONITOR_HEADER_TS '%(%H:%M:%S)T' -1
    monitor_render_tabs $(( ${#MONITOR_HEADER_TITLE} + 2 + ${#MONITOR_HEADER_TS} + 2 ))
}

header_line() {
    printf -v V "${c_bold}${c_cyan}${MONITOR_HEADER_TITLE}${c_reset}  %s  %s ${c_gray}n/p cycle · q quit · %ss${c_reset}%s" \
        "$MONITOR_HEADER_TS" "$MONITOR_TABS_RENDERED" "$REFRESH" "${1:-}"
    lines+=("$V")
}

# many_wiis -- true with more than one Wii in the pool: the tables get a WII column
many_wiis() { (( ${KV[wiis_n]:-1} > 1 )); }

# wii_lines -- with more than one Wii, a line for each instead of the one "Wii:" line
wii_lines() {
    local i w plain color
    for (( i = 0; i < ${NROWS[wiis]:-0}; i++ )); do
        row_fields wiis "$i"
        case "${F[2]}" in
            left)    color="${c_bold}${c_red}" ;;
            mine)    color="$c_cyan" ;;
            other)   color="$c_yellow" ;;
            unknown) color="$c_gray" ;;
            *)       color="$c_green" ;;
        esac
        [ "${F[3]}" = "-" ] && F[3]="free"
        fit "Wii ${F[0]} (${F[1]}): ${F[3]}" $(( COLS - 2 )); plain="$V"
        w="${color}${plain}${c_reset}"
        if [ -n "${F[4]:-}" ] && [ "${F[4]}" != "-" ] && (( COLS - ${#plain} - 9 >= 20 )); then
            fit "${F[4]}" $(( COLS - ${#plain} - 9 )); w+="  ${c_gray}last: $V${c_reset}"
        fi
        lines+=("$w")
    done
}

# status_lines -- the lines under the tab bar on every page: who has the Wii (each Wii, with
# more than one), and how the lease server, this dispatcher and the window's problems stand.
status_lines() {
    local w plain host holder e n room
    k host; host="$V"; k holder_host; holder="$V"
    k left_host
    if many_wiis; then
        wii_lines
    elif [ -n "$V" ]; then
        plain="Wii LEFT OUT OF HBC by $V's "
        k left_name; plain+="$V: "; k left_wii; plain+="$V ("; k left_ago; plain+="$V)"
        w="${c_bold}${c_red}${plain}${c_reset}"
    elif [ -n "$holder" ]; then
        if [ "$holder" = "$host" ]; then plain="Wii: this workstation has it"; w="${c_cyan}${plain}${c_reset}"
        else plain="Wii: $holder has it"; w="${c_yellow}${plain}${c_reset}"; fi
        local for_ phase=""; k holder_for; for_="$V"
        k holder_phase
        if [[ "$V" == "waiting for HBC"* ]]; then      # it has the lease, the Wii is not in HBC
            phase="$V"; k holder_phase_for; [ -n "$V" ] && phase+=" for $V"
        fi
        k holder_name; fit "$V" $(( COLS - ${#plain} - ${#for_} - ${#phase} - 12 > 50 ? 50 : COLS - ${#plain} - ${#for_} - ${#phase} - 12 ))
        plain+=" for $V ($for_)"; w+=" for $V ${c_gray}($for_)${c_reset}"
        [ -n "$phase" ] && { plain+=": $phase"; w+=": ${c_yellow}${phase}${c_reset}"; }
    elif [ "${KV[server]:-}" = "none" ] && [ "${KV[running_n]:-0}" != "0" ]; then
        plain="Wii: in use by a job here"; w="${c_cyan}${plain}${c_reset}"
    else
        plain="Wii: free"; w="${c_green}${plain}${c_reset}"
    fi
    if ! many_wiis; then
        # "last:" gets what is left of the line, and goes when too little is
        room=$(( COLS - ${#plain} - 9 ))
        k wii_last; (( room >= 20 )) && [ -n "$V" ] && { fit "$V" "$room"; w+="  ${c_gray}last: $V${c_reset}"; }
        lines+=("$w")
    fi
    case "${KV[server_ok]:-}" in
        yes) w="${c_green}lease server ok${c_reset}" ;;
        no)  w="${c_red}lease server unreachable${c_reset}" ;;
        *)   w="${c_gray}no lease server${c_reset}" ;;
    esac
    k dispatcher_pid
    if [ -n "$V" ]; then
        w+="  ·  dispatcher $V"
        (( ${KV[dispatcher_turns]:-0} > 1 )) && w+=" (${KV[dispatcher_turns]} turns)"
        k dispatcher_detail; [ -n "$V" ] && { fit "$V" 60; w+=": $V"; }
    else
        w+="  ·  ${c_gray}no dispatcher${c_reset}"
    fi
    e="${KV[errors_err]:-0}"; n="${KV[errors_warn]:-0}"
    if (( e > 0 )); then w+="  ·  ${c_bold}${c_red}${e} error(s)${c_reset}"
    elif (( n > 0 )); then w+="  ·  ${c_yellow}${n} warning(s)${c_reset}"
    else w+="  ·  ${c_green}no problems${c_reset}"; fi
    (( e > 0 && n > 0 )) && w+=" ${c_yellow}${n} warning(s)${c_reset}"
    w+=" ${c_gray}in ${HOURS} h${c_reset}"
    lines+=("$w")
}

# ---------------------------------------------------------------- page: queue

render_queue() {
    local i fmt room color nm host
    # RUNNING HERE
    heading "$c_cyan" "RUNNING HERE" "this workstation's dispatcher" running
    if (( ${NROWS[running]:-0} == 0 )); then
        placeholder "nothing running here"
    else
        if many_wiis; then
            name_width $(( 2 + 22 + 1 + 10 + 1 + 12 + 1 + 11 + 1 + 8 + 1 + 7 + 1 + 7 + 2 ))
            printf -v fmt "  %%-22s %%-10s %%-%ds %%-12s %%-11s %%8s %%7s %%7s" "$W"
            table_header running "$fmt" id:ID wii:WII name:NAME agent:AGENT started:STARTED elapsed:ELAPSED timeout:TIMEOUT left:LEFT
        else
            name_width $(( 2 + 22 + 1 + 12 + 1 + 11 + 1 + 8 + 1 + 7 + 1 + 7 + 2 ))
            printf -v fmt "  %%-22s %%-%ds %%-12s %%-11s %%8s %%7s %%7s" "$W"
            table_header running "$fmt" id:ID name:NAME agent:AGENT started:STARTED elapsed:ELAPSED timeout:TIMEOUT left:LEFT
        fi
        for (( i = 0; i < ${NROWS[running]}; i++ )); do
            row_fields running "$i"; fit "${F[1]}" "$W"; nm="$V"
            if many_wiis; then
                fit "${F[7]}" 10
                table_row "$fmt" "" id:"${F[0]}" wii:"$V" name:"$nm" agent:"${F[2]}" started:"${F[3]}" \
                    elapsed:"${F[4]}" timeout:"${F[5]}" left:"${F[6]}"
            else
                table_row "$fmt" "" id:"${F[0]}" name:"$nm" agent:"${F[2]}" started:"${F[3]}" \
                    elapsed:"${F[4]}" timeout:"${F[5]}" left:"${F[6]}"
            fi
            MONITOR_CELL_VALUES["$ROW_AT:name"]="${F[1]}"
        done
    fi
    lines+=("")
    # WAITING FOR THE WII (every workstation)
    heading "$c_cyan" "WAITING FOR THE WII" "every workstation, in order" waiting
    if [ "${KV[server]:-}" = "none" ]; then
        if many_wiis; then placeholder "no lease server: this workstation has the Wiis to itself"
        else placeholder "no lease server: this workstation has the Wii to itself"; fi
    elif (( ${NROWS[waiting]:-0} == 0 )); then
        placeholder "nobody waiting"
    else
        name_width $(( 2 + 3 + 1 + 24 + 1 + 11 + 1 + 8 + 2 ))
        printf -v fmt "  %%3s %%-24s %%-%ds %%-11s %%8s" "$W"
        table_header waiting "$fmt" place:'#' host:WORKSTATION name:JOB since:SINCE waited:WAITED
        for (( i = 0; i < ${NROWS[waiting]}; i++ )); do
            row_fields waiting "$i"; fit "${F[1]}" 24; host="$V"; fit "${F[2]}" "$W"; nm="$V"
            table_row "$fmt" "" place:"${F[0]}" host:"$host" name:"$nm" since:"${F[3]}" waited:"${F[4]}"
            MONITOR_CELL_VALUES["$ROW_AT:name"]="${F[2]}"
        done
    fi
    lines+=("")
    # QUEUED: every workstation's jobs, in the order their turns will come (the lease server
    # has each workstation's list; without a server, this workstation's alone)
    local qtitle="QUEUED, EVERY WORKSTATION" qsub="in the order received: the Wii takes them that way"
    [ "${KV[server]:-}" = "none" ] && { qtitle="QUEUED HERE"; qsub="oldest first"; }
    [ "${KV[server_ok]:-}" = "no" ] && qsub="this workstation's only: the lease server does not answer"
    heading "$c_cyan" "$qtitle" "${KV[queued_n]:-0} job(s), $qsub" pending
    if (( ${NROWS[pending]:-0} == 0 )); then
        placeholder "nothing queued"
    else
        local hostname; k host; hostname="$V"
        if many_wiis; then
            name_width $(( 2 + 4 + 1 + 18 + 1 + 22 + 1 + 10 + 1 + 12 + 1 + 8 + 2 ))
            printf -v fmt "  %%4s %%-18s %%-22s %%-10s %%-%ds %%-12s %%8s" "$W"
            table_header pending "$fmt" turn:TURN host:WORKSTATION id:ID wii:FOR name:NAME agent:AGENT age:WAITING
        else
            name_width $(( 2 + 4 + 1 + 18 + 1 + 22 + 1 + 12 + 1 + 8 + 2 ))
            printf -v fmt "  %%4s %%-18s %%-22s %%-%ds %%-12s %%8s" "$W"
            table_header pending "$fmt" turn:TURN host:WORKSTATION id:ID name:NAME agent:AGENT age:WAITING
        fi
        room=$(( ROWS_MAX - ${#lines[@]} - 8 ))
        for (( i = 0; i < ${NROWS[pending]}; i++ )); do
            if (( i >= room && i < ${NROWS[pending]} - 1 )); then
                placeholder "... and $(( ${NROWS[pending]} - i )) more"; break
            fi
            row_fields pending "$i"; fit "${F[1]}" 18; host="$V"; fit "${F[3]}" "$W"; nm="$V"
            local color=""; [ "${F[1]}" = "$hostname" ] && color="$c_cyan"     # this workstation's
            if many_wiis; then
                fit "${F[6]}" 10
                table_row "$fmt" "$color" turn:"${F[0]}" host:"$host" id:"${F[2]}" wii:"$V" name:"$nm" agent:"${F[4]}" age:"${F[5]}"
            else
                table_row "$fmt" "$color" turn:"${F[0]}" host:"$host" id:"${F[2]}" name:"$nm" agent:"${F[4]}" age:"${F[5]}"
            fi
            MONITOR_CELL_VALUES["$ROW_AT:name"]="${F[3]}"
            MONITOR_CELL_VALUES["$ROW_AT:host"]="${F[1]}"
        done
        (( ${KV[queued_more]:-0} > 0 )) && placeholder "and ${KV[queued_more]} more beyond the 50 a workstation sends"
    fi
    lines+=("")
    # RECENT JOBS HERE -- as many as fit
    heading "$c_cyan" "RECENT JOBS HERE" "newest first; HBC: ok at the end, Ns later, or LEFT" done
    if (( ${NROWS[done]:-0} == 0 )); then
        placeholder "no finished jobs yet"
        return
    fi
    if many_wiis; then
        name_width $(( 2 + 11 + 1 + 22 + 1 + 10 + 1 + 9 + 1 + 6 + 1 + 6 + 1 + 7 + 2 ))
        printf -v fmt "  %%-11s %%-22s %%-10s %%-%ds %%9s %%6s %%6s %%-7s" "$W"
        table_header done "$fmt" finished:FINISHED id:ID wii:WII name:NAME exit:EXIT secs:SECS hbc:HBC chained:CHAINED
    else
        name_width $(( 2 + 11 + 1 + 22 + 1 + 9 + 1 + 6 + 1 + 6 + 1 + 7 + 2 ))
        printf -v fmt "  %%-11s %%-22s %%-%ds %%9s %%6s %%6s %%-7s" "$W"
        table_header done "$fmt" finished:FINISHED id:ID name:NAME exit:EXIT secs:SECS hbc:HBC chained:CHAINED
    fi
    room=$(( ROWS_MAX - ${#lines[@]} ))
    for (( i = 0; i < ${NROWS[done]} && i < room; i++ )); do
        row_fields done "$i"; fit "${F[2]}" "$W"; nm="$V"
        color=""
        if [ "${F[3]}" != "0" ] || [ "${F[5]}" = "LEFT" ]; then color="$c_red"; fi
        if many_wiis; then
            local wii; fit "${F[7]}" 10; wii="$V"
            table_row "$fmt" "$color" finished:"${F[0]}" id:"${F[1]}" wii:"$wii" name:"$nm" exit:"${F[3]}" \
                secs:"${F[4]}" hbc:"${F[5]}" chained:"${F[6]}"
        else
            table_row "$fmt" "$color" finished:"${F[0]}" id:"${F[1]}" name:"$nm" exit:"${F[3]}" \
                secs:"${F[4]}" hbc:"${F[5]}" chained:"${F[6]}"
        fi
        MONITOR_CELL_VALUES["$ROW_AT:name"]="${F[2]}"
    done
}

# ---------------------------------------------------------------- page: errors

render_errors() {
    local i fmt job_w det_w room color kind host job det
    k history_src "this workstation"
    heading "$c_red" "PROBLEMS" "last ${HOURS} h, newest first; jobs from $V; click a cell to copy it" errors
    if (( ${NROWS[errors]:-0} == 0 )); then
        placeholder "nothing went wrong in the last ${HOURS} h"
        return
    fi
    job_w=28
    det_w=$(( COLS - 2 - 11 - 1 - 4 - 1 - 18 - 1 - 18 - 1 - job_w - 1 - 2 ))
    (( det_w < 20 )) && det_w=20
    printf -v fmt "  %%-11s %%-4s %%-18s %%-18s %%-%ds %%s" "$job_w"
    table_header errors "$fmt" when:WHEN severity:SEV kind:KIND host:WORKSTATION job:JOB detail:DETAIL
    room=$(( ROWS_MAX - ${#lines[@]} ))
    for (( i = 0; i < ${NROWS[errors]}; i++ )); do
        if (( i >= room - 1 && i < ${NROWS[errors]} - 1 )); then
            placeholder "... and $(( ${NROWS[errors]} - i )) more: --hours= or --filter=errors:TEXT"; break
        fi
        row_fields errors "$i"
        case "${F[1]}" in err) color="$c_red" ;; warn) color="$c_yellow" ;; *) color="$c_gray" ;; esac
        fit "${F[2]}" 18; kind="$V"; fit "${F[3]}" 18; host="$V"; fit "${F[4]}" "$job_w"; job="$V"; fit "${F[6]}" "$det_w"; det="$V"
        table_row "$fmt" "$color" when:"${F[0]}" severity:"${F[1]}" kind:"$kind" host:"$host" job:"$job" detail:"$det"
        MONITOR_CELL_VALUES["$ROW_AT:job"]="${F[4]} ${F[5]}"
        MONITOR_CELL_VALUES["$ROW_AT:detail"]="${F[6]}"
    done
}

# ---------------------------------------------------------------- page: history

render_history() {
    local i fmt color host nm
    k history_src "this workstation"
    heading "$c_cyan" "WORKSTATIONS" "last ${HOURS} h, from $V" hosts
    if (( ${NROWS[hosts]:-0} == 0 )); then
        placeholder "no history in the last ${HOURS} h"
    else
        fmt="  %-26s %5s %6s %8s %6s %9s %9s"
        table_header hosts "$fmt" host:WORKSTATION jobs:JOBS failed:FAILED wii_min:WII_MIN turns:TURNS \
            wait_med:WAIT_MED wait_max:WAIT_MAX
        for (( i = 0; i < ${NROWS[hosts]}; i++ )); do
            row_fields hosts "$i"; fit "${F[0]}" 26; host="$V"
            color=""; [ "${F[2]}" != "0" ] && color="$c_yellow"
            table_row "$fmt" "$color" host:"$host" jobs:"${F[1]}" failed:"${F[2]}" wii_min:"${F[3]}" \
                turns:"${F[4]}" wait_med:"${F[5]}" wait_max:"${F[6]}"
        done
    fi
    lines+=("  ${c_gray}Wii in use by queue jobs: ${KV[window_busy]:-0%} of the window, ${KV[window_jobs]:-0} job(s)${c_reset}")
    k handover; [ -n "$V" ] && lines+=("  ${c_gray}$V${c_reset}")
    lines+=("")
    heading "$c_cyan" "LONGEST WAITS FOR THE WII" "a minute or more" waits
    if (( ${NROWS[waits]:-0} == 0 )); then
        placeholder "nobody waited a minute or more"
        return
    fi
    name_width $(( 2 + 11 + 1 + 8 + 1 + 24 + 2 ))
    printf -v fmt "  %%-11s %%8s %%-24s %%-%ds" "$W"
    table_header waits "$fmt" when:WHEN waited:WAITED host:WORKSTATION name:JOB
    for (( i = 0; i < ${NROWS[waits]}; i++ )); do
        row_fields waits "$i"; fit "${F[2]}" 24; host="$V"; fit "${F[3]}" "$W"; nm="$V"
        table_row "$fmt" "" when:"${F[0]}" waited:"${F[1]}" host:"$host" name:"$nm"
        MONITOR_CELL_VALUES["$ROW_AT:name"]="${F[3]}"
    done
}

# ---------------------------------------------------------------- page: log

render_log() {
    local n="${NROWS[log]:-0}" room first i
    heading "$c_cyan" "DISPATCHER LOG" "${KV[home]:-}/dispatcher.log, newest last"
    if (( n == 0 )); then
        placeholder "no dispatcher.log yet"
        return
    fi
    room=$(( ROWS_MAX - ${#lines[@]} ))
    first=$(( n > room ? n - room : 0 ))
    for (( i = first; i < n; i++ )); do
        fit "${ROWS[log:$i]}" $(( COLS - 2 ))
        case "$V" in
            *"lease lost"*|*Traceback*|*"did not come back"*|*Error*) lines+=("  ${c_red}${V}${c_reset}") ;;
            *unreachable*|*"history:"*)                              lines+=("  ${c_yellow}${V}${c_reset}") ;;
            *"lease taken"*|*chained*)                               lines+=("  ${c_cyan}${V}${c_reset}") ;;
            *)                                                       lines+=("  ${V}") ;;
        esac
    done
}

# ---------------------------------------------------------------- page: manage

SELECTED=""                                        # the selected job's id
epoch() { printf -v EPOCH '%(%s)T' -1; }           # EPOCHSECONDS without bash 5
declare -A MANAGE_AT                               # screen row -> job id, this frame
CONFIRM=""; CONFIRM_ARGS=()                        # an action waiting for its y
MSG=""; MSG_UNTIL=0                                # the last action's answer, shown a while

render_manage() {
    local i fmt nm host color mark n="${NROWS[manage]:-0}" ids=() sel_ok=0
    MANAGE_AT=()
    heading "$c_cyan" "MANAGE" "click a job or j/k to select; c cancel/stop it, a its agent's, w its workstation's, X all queued, g give up a freeze wait" manage
    if [ -n "$CONFIRM" ]; then
        lines+=("  ${c_bold}${c_yellow}${CONFIRM}  y to do it, any other key to leave it${c_reset}")
    elif epoch; [ -n "$MSG" ] && (( EPOCH < MSG_UNTIL )); then
        lines+=("  ${c_green}${MSG}${c_reset}")
    else
        lines+=("")
    fi
    if [ -n "${KV[recovering_id]:-}" ]; then
        local which="the Wii"; many_wiis && [ -n "${KV[recovering_wii]:-}" ] && [ "${KV[recovering_wii]}" != "-" ] && which="Wii ${KV[recovering_wii]}"
        lines+=("  ${c_red}This workstation keeps $which to save ${KV[recovering_name]}'s data after a freeze (${KV[recovering_for]}): reset or power-cycle it, or g to give up.${c_reset}")
    fi
    lines+=("")
    if (( n == 0 )); then
        placeholder "nothing running or queued anywhere"
        SELECTED=""
        return
    fi
    name_width $(( 2 + 1 + 8 + 1 + 4 + 1 + 16 + 1 + 22 + 1 + 12 + 1 + 14 + 1 + 8 + 2 ))
    printf -v fmt "%%1s %%-8s %%4s %%-16s %%-22s %%-%ds %%-12s %%-14s %%8s" "$W"
    table_header manage "$fmt" mark:' ' state:STATE turn:TURN host:WORKSTATION id:ID name:NAME agent:AGENT folder:FOLDER age:FOR
    for (( i = 0; i < n; i++ )); do
        row_fields manage "$i"; ids+=("${F[3]}")
        [ "${F[3]}" = "$SELECTED" ] && sel_ok=1
    done
    if (( ! sel_ok )); then SELECTED=""; fi
    for (( i = 0; i < n && ${#lines[@]} < ROWS_MAX; i++ )); do
        row_fields manage "$i"
        fit "${F[2]}" 16; host="$V"; fit "${F[4]}" "$W"; nm="$V"
        mark=" "; color=""
        case "${F[0]}" in running) color="$c_cyan" ;; waiting) color="$c_yellow" ;; esac
        if [ -n "$SELECTED" ] && [ "${F[3]}" = "$SELECTED" ]; then mark=">"; color=$'\e[7m'; fi
        table_row "$fmt" "$color" mark:"$mark" state:"${F[0]}" turn:"${F[1]}" host:"$host" id:"${F[3]}" name:"$nm" \
            agent:"${F[5]}" folder:"${F[6]}" age:"${F[7]}"
        MANAGE_AT[$ROW_AT]="${F[3]}"
    done
    MANAGE_IDS=("${ids[@]}")
}

# manage_field ID FIELD_INDEX -- sets V to a field of the manage row for job ID
manage_field() {
    local i
    V=""
    for (( i = 0; i < ${NROWS[manage]:-0}; i++ )); do
        row_fields manage "$i"
        [ "${F[3]}" = "$1" ] && { V="${F[$2]}"; return; }
    done
}

manage_move() {                                    # j/k: the next or previous job
    local i n="${#MANAGE_IDS[@]}" at=-1
    (( n == 0 )) && return
    for (( i = 0; i < n; i++ )); do [ "${MANAGE_IDS[$i]}" = "$SELECTED" ] && at=$i; done
    at=$(( at + $1 ))
    (( at < 0 )) && at=0
    (( at >= n )) && at=$(( n - 1 ))
    [ "${MANAGE_IDS[$at]}" = "-" ] || SELECTED="${MANAGE_IDS[$at]}"
}

# manage_key KEY -- 0 if the manage page took the key
manage_key() {
    local key="$1" state agent host
    if [ -n "$CONFIRM" ]; then                     # the answer to "...? y"
        if [ "$key" = "y" ] || [ "$key" = "Y" ]; then
            MSG="$("$PY" "$BENCH" "${CONFIRM_ARGS[@]}" 2>&1 | tr '\n' ' ')"
            epoch; MSG_UNTIL=$(( EPOCH + 8 ))
            collect
        else
            MSG="left as it was"; epoch; MSG_UNTIL=$(( EPOCH + 3 ))
        fi
        CONFIRM=""; CONFIRM_ARGS=()
        return 0
    fi
    case "$key" in
        j|DOWN) manage_move 1; return 0 ;;
        k|UP)   manage_move -1; return 0 ;;
        c)
            [ -n "$SELECTED" ] || { MSG="select a job first: click it, or j/k"; epoch; MSG_UNTIL=$(( EPOCH + 4 )); return 0; }
            manage_field "$SELECTED" 0; state="$V"; manage_field "$SELECTED" 4
            if [ "$state" = "queued" ]; then
                CONFIRM="Cancel $SELECTED ($V)?"; CONFIRM_ARGS=(cancel "$SELECTED")
            else
                CONFIRM="Stop $SELECTED ($V) now? It ends like a timeout."; CONFIRM_ARGS=(stop "$SELECTED")
            fi ;;
        a)
            [ -n "$SELECTED" ] || return 0
            manage_field "$SELECTED" 5; agent="$V"
            [ -n "$agent" ] && [ "$agent" != "-" ] || { MSG="that job has no agent"; epoch; MSG_UNTIL=$(( EPOCH + 4 )); return 0; }
            CONFIRM="Cancel every queued job of agent $agent?"; CONFIRM_ARGS=(cancel --agent "$agent") ;;
        w)
            [ -n "$SELECTED" ] || return 0
            manage_field "$SELECTED" 2; host="$V"
            CONFIRM="Cancel every queued job on $host?"; CONFIRM_ARGS=(cancel --host "$host") ;;
        X)
            CONFIRM="Cancel EVERY queued job, on every workstation?"; CONFIRM_ARGS=(cancel --all) ;;
        g)
            [ -n "${KV[recovering_id]:-}" ] || { MSG="no freeze being waited out here"; epoch; MSG_UNTIL=$(( EPOCH + 4 )); return 0; }
            CONFIRM="Give up waiting for the Wii to save ${KV[recovering_name]}'s data?"; CONFIRM_ARGS=(stop "${KV[recovering_id]}") ;;
        *) return 1 ;;
    esac
    return 0
}

# The library's monitor_read_input, which reads an arrow key's ESC [ A as a no-op: the same,
# with UP and DOWN for the arrows (the manage page moves its selection with them).
monitor_read_input() {
    local timeout="$1" key rest seq="" c cb col row term
    MONITOR_INPUT_KEY=""; MONITOR_INPUT_COL=""; MONITOR_INPUT_ROW=""
    read -s -t "$timeout" -n 1 key || return 1
    if [ "$key" != $'\e' ]; then
        MONITOR_INPUT_KEY="$key"
        return 0
    fi
    read -s -t 0.05 -n 2 rest 2>/dev/null
    case "$rest" in
        "[A") MONITOR_INPUT_KEY="UP"; return 0 ;;
        "[B") MONITOR_INPUT_KEY="DOWN"; return 0 ;;
        "[<") ;;
        *) return 0 ;;
    esac
    while read -s -t 0.05 -n 1 c 2>/dev/null; do
        seq+="$c"
        { [ "$c" = "M" ] || [ "$c" = "m" ]; } && break
    done
    [[ "$seq" =~ ^([0-9]+)\;([0-9]+)\;([0-9]+)(M|m)$ ]] || return 0
    cb="${BASH_REMATCH[1]}"; col="${BASH_REMATCH[2]}"; row="${BASH_REMATCH[3]}"; term="${BASH_REMATCH[4]}"
    { [ "$term" = "M" ] && [ "$cb" = "0" ]; } || return 0
    MONITOR_INPUT_COL="$col"; MONITOR_INPUT_ROW="$row"
    return 0
}

# ---------------------------------------------------------------- frame and loop

# The terminal's size: asked once (two forks), then again only when it changes (SIGWINCH).
term_size() {
    COLS=$(tput cols 2>/dev/null || echo 120); ROWS_MAX=$(tput lines 2>/dev/null || echo 40)
    [[ "$COLS" =~ ^[0-9]+$ ]] || COLS=120; [[ "$ROWS_MAX" =~ ^[0-9]+$ ]] || ROWS_MAX=40
}

build_frame() {
    lines=()
    MONITOR_HEADER_ROW=(); MONITOR_HEADER_COLS=()      # only this frame's sections are clickable
    MONITOR_CELL_COLS=(); MONITOR_CELL_VALUES=()
    monitor_prepare_header
    header_line
    if [ -z "${KV[now]:-}" ]; then
        local why="${KV[error]:-}"
        [ -z "$why" ] && [ -s "$ERRFILE" ] && why="$(tail -n 1 "$ERRFILE" 2>/dev/null)"
        lines+=("" "  ${c_red}the snapshot failed:${c_reset} ${why:-no answer}")
        lines+=("  ${c_gray}$PY $BENCH snapshot${c_reset}")
        return
    fi
    status_lines
    lines+=("")
    case "$MONITOR_PAGE" in
        queue)   render_queue ;;
        errors)  render_errors ;;
        history) render_history ;;
        log)     render_log ;;
        manage)  render_manage ;;
    esac
}

cleanup() {
    snap_stop
    rm -f "$ERRFILE"
}

if [ "$ONCE" -eq 1 ]; then
    COLS="${COLUMNS:-$(tput cols 2>/dev/null || echo 120)}"; ROWS_MAX=200
    collect_once
    build_frame
    if [ -t 1 ]; then printf '%s\n' "${lines[@]}"
    else printf '%s\n' "${lines[@]}" | sed $'s/\033\\[[0-9;]*m//g'; fi
    cleanup
    exit 0
fi

restore_cursor() { printf '\e[?7h'; monitor_restore_cursor; cleanup; echo; exit 0; }
trap restore_cursor INT TERM
trap term_size WINCH
term_size
monitor_setup_screen
# No line wrap while the dashboard is up (DECAWM off, back on in restore_cursor): a line wider
# than the terminal is cut at its edge instead of pushing every line below it down one.
printf '\e[?7l'

# $WII_BENCH_MONITOR_TIMES=FILE appends each frame's milliseconds: collect, build, draw.
TIMES="${WII_BENCH_MONITOR_TIMES:-}"
ms() { local t="${EPOCHREALTIME/[.,]/}"; V=$(( ${t:-0} / 1000 )); }
[ -n "$TIMES" ] && [ -z "${EPOCHREALTIME:-}" ] && TIMES=""      # bash 5 has it, 4.3 does not

ms; T0="$V"
collect
ms; T_COLLECT=$(( V - T0 ))
while true; do
    ms; T0="$V"
    build_frame
    ms; T1="$V"
    monitor_draw_frame lines
    if [ -n "$TIMES" ]; then
        ms; printf '%s %s %s\n' "$T_COLLECT" $(( T1 - T0 )) $(( V - T1 )) >> "$TIMES"
    fi
    T_COLLECT=0
    if monitor_read_input "$REFRESH"; then
        if [ -n "$MONITOR_INPUT_COL" ]; then
            # Page bar first, then a column header (re-sorts, so collect again), then a cell.
            if monitor_mouse_click "$MONITOR_INPUT_COL" "$MONITOR_INPUT_ROW"; then
                continue                                 # the same snapshot, another page: instant
            elif monitor_header_click "$MONITOR_INPUT_COL" "$MONITOR_INPUT_ROW"; then
                collect
            elif [ "$MONITOR_PAGE" = manage ] && [ -n "${MANAGE_AT[$MONITOR_INPUT_ROW]:-}" ]; then
                [ "${MANAGE_AT[$MONITOR_INPUT_ROW]}" != "-" ] && SELECTED="${MANAGE_AT[$MONITOR_INPUT_ROW]}"
                continue                                 # selected: redraw at once
            elif monitor_cell_click "$MONITOR_INPUT_COL" "$MONITOR_INPUT_ROW"; then
                monitor_clipboard_copy "$MONITOR_CELL_VALUE"
                monitor_flash_cell "$MONITOR_INPUT_ROW" "$MONITOR_CELL_COL_LO" "$MONITOR_CELL_COL_HI" "$MONITOR_CELL_VALUE"
            fi
        else
            if [ "$MONITOR_PAGE" = manage ] && [ -n "$MONITOR_INPUT_KEY" ] && manage_key "$MONITOR_INPUT_KEY"; then
                continue                                 # handled: redraw at once
            fi
            case "$MONITOR_INPUT_KEY" in
                q|Q) restore_cursor ;;
                *)   monitor_page_key "$MONITOR_INPUT_KEY" || collect ;;   # a page key redraws at once
            esac
        fi
    else
        ms; T0="$V"
        collect
        ms; T_COLLECT=$(( V - T0 ))
    fi
done

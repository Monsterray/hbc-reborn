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
  --page=NAME       Page to open: queue, errors, history or log. Default: queue.
  --hours=N         The window for errors and history. Default: 24.
  --once            Print one frame and exit (plain text when not a terminal).
  --sort=SECTION:FIELD[:DIR]
                    Sort a section by FIELD. DIR: a/asc (default) or d/desc.
  --filter=SECTION:TEXT
                    Show only rows of SECTION containing TEXT (any column,
                    ignoring case).

SECTIONS AND FIELDS
  running   id name agent started elapsed timeout left
  waiting   place host name since waited
  pending   place id name agent added age
  done      finished id name exit secs hbc chained
  errors    when severity kind host job id detail
  hosts     host jobs failed wii_min turns wait_med wait_max
  waits     when waited host name

KEYS
  1-4 page   n/p or Tab next/previous page   q quit   any other key: refresh now

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
monitor_pages_init "$PAGE" queue errors history log

ERRFILE="$(mktemp 2>/dev/null || echo "${TMPDIR:-/tmp}/wiibench-monitor.$$")"
declare -A KV ROWS NROWS

# ---------------------------------------------------------------- data

# collect -- one snapshot: KV[key]=value, ROWS[section:i]=tab-separated fields, NROWS[section].
# Sorting and filtering happen in the snapshot (SORT_FIELD/SORT_DESC/FILTER_VAL passed along).
collect() {
    local args=(snapshot --hours "$HOURS") sec line rest n
    for sec in "${!SORT_FIELD[@]}"; do
        [ -n "${SORT_FIELD[$sec]}" ] && args+=(--sort "$sec:${SORT_FIELD[$sec]}:${SORT_DESC[$sec]:-0}")
    done
    for sec in "${!FILTER_VAL[@]}"; do
        [ -n "${FILTER_VAL[$sec]}" ] && args+=(--filter "$sec:${FILTER_VAL[$sec]}")
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
    done < <("$PY" "$BENCH" "${args[@]}" 2>"$ERRFILE")
}

kv() { local v="${KV[$1]:-}"; [ "$v" = "-" ] && v=""; printf '%s' "${v:-${2:-}}"; }

# fit <text> <width> -- trims to width with a trailing ~ (only where a column must fit the screen)
fit() {
    local s="$1" w="$2"
    (( w < 2 )) && w=2
    if (( ${#s} > w )); then printf '%s~' "${s:0:$(( w - 1 ))}"; else printf '%s' "$s"; fi
}

# ---------------------------------------------------------------- tables

# table_header SECTION FMT field:LABEL ...   (same format string as the rows: columns line up)
table_header() {
    local sec="$1" fmt="$2"; shift 2
    local sf="${SORT_FIELD[$sec]:-}" sd="${SORT_DESC[$sec]:-0}"
    MONITOR_HEADER_ROW[$sec]=$(( ${#lines[@]} + 1 ))
    MONITOR_HEADER_COLS[$sec]=$(monitor_header_colspans "$fmt" "$sf" "$sd" "$@")
    lines+=("$(monitor_render_header "$fmt" "$sf" "$sd" "$@")")
}

# table_row FMT COLOR field:shown ...  -- one row, cells clickable. Sets ROW_AT (its screen
# row) so a caller can make a cell copy the full value instead of the trimmed one shown.
table_row() {
    local fmt="$1" color="$2"; shift 2
    local vals=() spec
    for spec in "$@"; do vals+=("${spec#*:}"); done
    ROW_AT=$(( ${#lines[@]} + 1 ))
    monitor_register_row_cells "$ROW_AT" "$fmt" "$@"
    lines+=("${color}$(printf "$fmt" "${vals[@]}")${c_reset}")
}

# row_fields SECTION I -- splits ROWS[SECTION:I] into the array F
row_fields() { IFS=$'\t' read -r -a F <<< "${ROWS[$1:$2]}"; }

sev_color() { case "$1" in err) printf '%s' "$c_red" ;; warn) printf '%s' "$c_yellow" ;; *) printf '%s' "$c_gray" ;; esac; }

# ---------------------------------------------------------------- the header

MONITOR_HEADER_TITLE="=== WII BENCH ==="

monitor_prepare_header() {
    MONITOR_HEADER_TS="$(date '+%H:%M:%S')"
    monitor_render_tabs $(( ${#MONITOR_HEADER_TITLE} + 2 + ${#MONITOR_HEADER_TS} + 2 ))
}

header_line() {
    printf "${c_bold}${c_cyan}${MONITOR_HEADER_TITLE}${c_reset}  %s  %s ${c_gray}n/p cycle · q quit · %ss${c_reset}%s" \
        "$MONITOR_HEADER_TS" "$MONITOR_TABS_RENDERED" "$REFRESH" "${1:-}"
}

# status_lines -- the two lines under the tab bar on every page: who has the Wii, and how
# the lease server, this dispatcher and the window's problems stand.
status_lines() {
    local host w e n
    host="$(kv host)"
    if [ -n "$(kv left_host)" ]; then
        w="${c_bold}${c_red}Wii LEFT OUT OF HBC${c_reset}${c_red} by $(kv left_host)'s $(kv left_name): $(kv left_wii) ($(kv left_ago))${c_reset}"
    elif [ -n "$(kv holder_host)" ]; then
        if [ "$(kv holder_host)" = "$host" ]; then w="${c_cyan}Wii: this workstation has it${c_reset}"
        else w="${c_yellow}Wii: $(kv holder_host) has it${c_reset}"; fi
        w+=" for $(fit "$(kv holder_name)" 60) $(monitor_gray_paren "$(kv holder_for)")"
    elif [ "$(kv server)" = "none" ] && [ "$(kv running_n 0)" != "0" ]; then
        w="${c_cyan}Wii: in use by a job here${c_reset}"
    else
        w="${c_green}Wii: free${c_reset}"
    fi
    [ -n "$(kv wii_last)" ] && w+="  ${c_gray}last: $(fit "$(kv wii_last)" 70)${c_reset}"
    lines+=("$w")
    case "$(kv server_ok)" in
        yes) w="${c_green}lease server ok${c_reset}" ;;
        no)  w="${c_red}lease server unreachable${c_reset}" ;;
        *)   w="${c_gray}no lease server${c_reset}" ;;
    esac
    if [ "$(kv dispatcher_pid)" != "" ]; then
        w+="  ·  dispatcher $(kv dispatcher_pid)"
        [ -n "$(kv dispatcher_detail)" ] && w+=": $(fit "$(kv dispatcher_detail)" 60)"
    else
        w+="  ·  ${c_gray}no dispatcher${c_reset}"
    fi
    e="$(kv errors_err 0)"; n="$(kv errors_warn 0)"
    if (( e > 0 )); then w+="  ·  ${c_bold}${c_red}${e} error(s)${c_reset}"
    elif (( n > 0 )); then w+="  ·  ${c_yellow}${n} warning(s)${c_reset}"
    else w+="  ·  ${c_green}no problems${c_reset}"; fi
    (( e > 0 && n > 0 )) && w+=" ${c_yellow}${n} warning(s)${c_reset}"
    w+=" ${c_gray}in ${HOURS} h${c_reset}"
    lines+=("$w")
}

# ---------------------------------------------------------------- page: queue

render_queue() {
    local i fmt w name_w cols="$COLS" room
    # RUNNING HERE
    lines+=("$(monitor_section_heading "$c_cyan" "RUNNING HERE" "this workstation's dispatcher" running)")
    if (( ${NROWS[running]:-0} == 0 )); then
        lines+=("$(monitor_placeholder "nothing running here")")
    else
        name_w=$(( cols - 2 - 22 - 1 - 12 - 1 - 11 - 1 - 8 - 1 - 7 - 1 - 7 - 2 ))
        (( name_w < 16 )) && name_w=16
        printf -v fmt "  %%-22s %%-%ds %%-12s %%-11s %%8s %%7s %%7s" "$name_w"
        table_header running "$fmt" id:ID name:NAME agent:AGENT started:STARTED elapsed:ELAPSED timeout:TIMEOUT left:LEFT
        for (( i = 0; i < ${NROWS[running]}; i++ )); do
            row_fields running "$i"
            table_row "$fmt" "" id:"${F[0]}" name:"$(fit "${F[1]}" "$name_w")" agent:"${F[2]}" started:"${F[3]}" \
                elapsed:"${F[4]}" timeout:"${F[5]}" left:"${F[6]}"
            MONITOR_CELL_VALUES["$ROW_AT:name"]="${F[1]}"
        done
    fi
    lines+=("")
    # WAITING FOR THE WII (every workstation)
    lines+=("$(monitor_section_heading "$c_cyan" "WAITING FOR THE WII" "every workstation, in order" waiting)")
    if [ "$(kv server)" = "none" ]; then
        lines+=("$(monitor_placeholder "no lease server: this workstation has the Wii to itself")")
    elif (( ${NROWS[waiting]:-0} == 0 )); then
        lines+=("$(monitor_placeholder "nobody waiting")")
    else
        name_w=$(( cols - 2 - 3 - 1 - 24 - 1 - 11 - 1 - 8 - 2 ))
        (( name_w < 16 )) && name_w=16
        printf -v fmt "  %%3s %%-24s %%-%ds %%-11s %%8s" "$name_w"
        table_header waiting "$fmt" place:'#' host:WORKSTATION name:JOB since:SINCE waited:WAITED
        for (( i = 0; i < ${NROWS[waiting]}; i++ )); do
            row_fields waiting "$i"
            table_row "$fmt" "" place:"${F[0]}" host:"$(fit "${F[1]}" 24)" name:"$(fit "${F[2]}" "$name_w")" \
                since:"${F[3]}" waited:"${F[4]}"
            MONITOR_CELL_VALUES["$ROW_AT:name"]="${F[2]}"
        done
    fi
    lines+=("")
    # QUEUED HERE
    lines+=("$(monitor_section_heading "$c_cyan" "QUEUED HERE" "$(kv pending_n 0) job(s), oldest first" pending)")
    if (( ${NROWS[pending]:-0} == 0 )); then
        lines+=("$(monitor_placeholder "nothing queued here")")
    else
        name_w=$(( cols - 2 - 3 - 1 - 22 - 1 - 12 - 1 - 11 - 1 - 8 - 2 ))
        (( name_w < 16 )) && name_w=16
        printf -v fmt "  %%3s %%-22s %%-%ds %%-12s %%-11s %%8s" "$name_w"
        table_header pending "$fmt" place:'#' id:ID name:NAME agent:AGENT added:ADDED age:WAITING
        room=$(( ROWS_MAX - ${#lines[@]} - 8 ))
        for (( i = 0; i < ${NROWS[pending]}; i++ )); do
            if (( i >= room && i < ${NROWS[pending]} - 1 )); then
                lines+=("$(monitor_placeholder "... and $(( ${NROWS[pending]} - i )) more")"); break
            fi
            row_fields pending "$i"
            table_row "$fmt" "" place:"${F[0]}" id:"${F[1]}" name:"$(fit "${F[2]}" "$name_w")" agent:"${F[3]}" \
                added:"${F[4]}" age:"${F[5]}"
            MONITOR_CELL_VALUES["$ROW_AT:name"]="${F[2]}"
        done
    fi
    lines+=("")
    # RECENT JOBS HERE -- as many as fit
    lines+=("$(monitor_section_heading "$c_cyan" "RECENT JOBS HERE" "newest first; HBC: how soon HBC came back" done)")
    if (( ${NROWS[done]:-0} == 0 )); then
        lines+=("$(monitor_placeholder "no finished jobs yet")")
        return
    fi
    name_w=$(( cols - 2 - 11 - 1 - 22 - 1 - 9 - 1 - 6 - 1 - 6 - 1 - 7 - 2 ))
    (( name_w < 16 )) && name_w=16
    printf -v fmt "  %%-11s %%-22s %%-%ds %%9s %%6s %%6s %%-7s" "$name_w"
    table_header done "$fmt" finished:FINISHED id:ID name:NAME exit:EXIT secs:SECS hbc:HBC chained:CHAINED
    room=$(( ROWS_MAX - ${#lines[@]} ))
    for (( i = 0; i < ${NROWS[done]} && i < room; i++ )); do
        row_fields done "$i"
        local color=""
        if [ "${F[3]}" != "0" ] || [ "${F[5]}" = "left" ]; then color="$c_red"; fi
        table_row "$fmt" "$color" finished:"${F[0]}" id:"${F[1]}" name:"$(fit "${F[2]}" "$name_w")" exit:"${F[3]}" \
            secs:"${F[4]}" hbc:"${F[5]}" chained:"${F[6]}"
        MONITOR_CELL_VALUES["$ROW_AT:name"]="${F[2]}"
    done
}

# ---------------------------------------------------------------- page: errors

render_errors() {
    local i fmt job_w det_w src room color
    src="$(kv history_src "this workstation")"
    lines+=("$(monitor_section_heading "$c_red" "PROBLEMS" "last ${HOURS} h, newest first; jobs from $src; click a cell to copy it" errors)")
    if (( ${NROWS[errors]:-0} == 0 )); then
        lines+=("$(monitor_placeholder "nothing went wrong in the last ${HOURS} h")")
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
            lines+=("$(monitor_placeholder "... and $(( ${NROWS[errors]} - i )) more: --hours= or --filter=errors:TEXT")"); break
        fi
        row_fields errors "$i"
        color="$(sev_color "${F[1]}")"
        table_row "$fmt" "$color" when:"${F[0]}" severity:"${F[1]}" kind:"$(fit "${F[2]}" 18)" host:"$(fit "${F[3]}" 18)" \
            job:"$(fit "${F[4]}" "$job_w")" detail:"$(fit "${F[6]}" "$det_w")"
        MONITOR_CELL_VALUES["$ROW_AT:job"]="${F[4]} ${F[5]}"
        MONITOR_CELL_VALUES["$ROW_AT:detail"]="${F[6]}"
    done
}

# ---------------------------------------------------------------- page: history

render_history() {
    local i fmt name_w
    lines+=("$(monitor_section_heading "$c_cyan" "WORKSTATIONS" "last ${HOURS} h, from $(kv history_src "this workstation")" hosts)")
    if (( ${NROWS[hosts]:-0} == 0 )); then
        lines+=("$(monitor_placeholder "no history in the last ${HOURS} h")")
    else
        fmt="  %-26s %5s %6s %8s %6s %9s %9s"
        table_header hosts "$fmt" host:WORKSTATION jobs:JOBS failed:FAILED wii_min:WII_MIN turns:TURNS \
            wait_med:WAIT_MED wait_max:WAIT_MAX
        for (( i = 0; i < ${NROWS[hosts]}; i++ )); do
            row_fields hosts "$i"
            local color=""; [ "${F[2]}" != "0" ] && color="$c_yellow"
            table_row "$fmt" "$color" host:"$(fit "${F[0]}" 26)" jobs:"${F[1]}" failed:"${F[2]}" wii_min:"${F[3]}" \
                turns:"${F[4]}" wait_med:"${F[5]}" wait_max:"${F[6]}"
        done
    fi
    lines+=("  ${c_gray}Wii in use by queue jobs: $(kv window_busy 0%) of the window, $(kv window_jobs 0) job(s)${c_reset}")
    [ -n "$(kv handover)" ] && lines+=("  ${c_gray}$(kv handover)${c_reset}")
    lines+=("")
    lines+=("$(monitor_section_heading "$c_cyan" "LONGEST WAITS FOR THE WII" "a minute or more" waits)")
    if (( ${NROWS[waits]:-0} == 0 )); then
        lines+=("$(monitor_placeholder "nobody waited a minute or more")")
        return
    fi
    name_w=$(( COLS - 2 - 11 - 1 - 8 - 1 - 24 - 2 ))
    (( name_w < 16 )) && name_w=16
    printf -v fmt "  %%-11s %%8s %%-24s %%-%ds" "$name_w"
    table_header waits "$fmt" when:WHEN waited:WAITED host:WORKSTATION name:JOB
    for (( i = 0; i < ${NROWS[waits]}; i++ )); do
        row_fields waits "$i"
        table_row "$fmt" "" when:"${F[0]}" waited:"${F[1]}" host:"$(fit "${F[2]}" 24)" name:"$(fit "${F[3]}" "$name_w")"
        MONITOR_CELL_VALUES["$ROW_AT:name"]="${F[3]}"
    done
}

# ---------------------------------------------------------------- page: log

render_log() {
    local n="${NROWS[log]:-0}" room first i l
    lines+=("$(monitor_section_heading "$c_cyan" "DISPATCHER LOG" "$(kv home)/dispatcher.log, newest last")")
    if (( n == 0 )); then
        lines+=("$(monitor_placeholder "no dispatcher.log yet")")
        return
    fi
    room=$(( ROWS_MAX - ${#lines[@]} ))
    first=$(( n > room ? n - room : 0 ))
    for (( i = first; i < n; i++ )); do
        l="$(fit "${ROWS[log:$i]}" $(( COLS - 2 )))"
        case "$l" in
            *"lease lost"*|*Traceback*|*"did not come back"*|*Error*) lines+=("  ${c_red}${l}${c_reset}") ;;
            *unreachable*|*"history:"*)                              lines+=("  ${c_yellow}${l}${c_reset}") ;;
            *"lease taken"*|*chained*)                               lines+=("  ${c_cyan}${l}${c_reset}") ;;
            *)                                                       lines+=("  ${l}") ;;
        esac
    done
}

# ---------------------------------------------------------------- frame and loop

build_frame() {
    lines=()
    MONITOR_HEADER_ROW=(); MONITOR_HEADER_COLS=()      # only this frame's sections are clickable
    MONITOR_CELL_COLS=(); MONITOR_CELL_VALUES=()
    COLS=$(tput cols 2>/dev/null || echo 120); ROWS_MAX=$(tput lines 2>/dev/null || echo 40)
    [ "$ONCE" -eq 1 ] && ROWS_MAX=200
    monitor_prepare_header
    lines+=("$(header_line)")
    if [ -z "${KV[now]:-}" ]; then
        lines+=("" "  ${c_red}the snapshot failed:${c_reset} $(tail -n 1 "$ERRFILE" 2>/dev/null)")
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
    esac
}

cleanup() { rm -f "$ERRFILE"; }

if [ "$ONCE" -eq 1 ]; then
    collect
    build_frame
    if [ -t 1 ]; then printf '%s\n' "${lines[@]}"
    else printf '%s\n' "${lines[@]}" | sed $'s/\033\\[[0-9;]*m//g'; fi
    cleanup
    exit 0
fi

restore_cursor() { monitor_restore_cursor; cleanup; echo; exit 0; }
trap restore_cursor INT TERM
monitor_setup_screen

collect
while true; do
    build_frame
    monitor_draw_frame lines
    if monitor_read_input "$REFRESH"; then
        if [ -n "$MONITOR_INPUT_COL" ]; then
            # Page bar first, then a column header (re-sorts, so collect again), then a cell.
            if monitor_mouse_click "$MONITOR_INPUT_COL" "$MONITOR_INPUT_ROW"; then
                continue                                 # the same snapshot, another page: instant
            elif monitor_header_click "$MONITOR_INPUT_COL" "$MONITOR_INPUT_ROW"; then
                collect
            elif monitor_cell_click "$MONITOR_INPUT_COL" "$MONITOR_INPUT_ROW"; then
                monitor_clipboard_copy "$MONITOR_CELL_VALUE"
                monitor_flash_cell "$MONITOR_INPUT_ROW" "$MONITOR_CELL_COL_LO" "$MONITOR_CELL_COL_HI" "$MONITOR_CELL_VALUE"
            fi
        else
            case "$MONITOR_INPUT_KEY" in
                q|Q) restore_cursor ;;
                *)   monitor_page_key "$MONITOR_INPUT_KEY" || collect ;;   # a page key redraws at once
            esac
        fi
    else
        collect
    fi
done

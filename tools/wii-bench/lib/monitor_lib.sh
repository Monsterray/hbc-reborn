#!/usr/bin/env bash
# Shared helpers for this server's live terminal dashboards. Sourced (never executed
# directly) by ai_monitor.sh and download_monitor.sh.
#
# WHY THIS FILE EXISTS: both scripts independently grew the same flicker-free
# tput-based redraw loop, the same --sort=/--filter= parsing into associative arrays,
# and the same jq-based sort/filter-then-render pattern for each table. Pulled out
# once both were doing it identically, rather than drifting into two slightly
# different copies the next time one gets a fix the other doesn't.
#
# A monitor using this file does these steps. Steps 6-8 are optional. Skip
# any of them and the rest of this file still works the same way.
#   1. declare -A SORT_FIELD SORT_DESC FILTER_VAL. Do this before step 2.
#   2. Call monitor_parse_args <default_section_or_empty> "$@".
#   3. Build each section's rows as one jq -n JSON object per row. Collect
#      them with jq -s into one array.
#   4. Call apply_sort_filter on that array. Then call monitor_render_header,
#      then a jq -r '... | @tsv' loop to print the rows. Use the SAME printf
#      format string for the header and the rows, so columns line up.
#   5. Call monitor_draw_frame on the finished `lines` array, once per redraw.
#   6. Add paging once the dashboard outgrows one screen. Call
#      monitor_pages_init once. Render only the current $MONITOR_PAGE's
#      sections. Put monitor_render_tabs in the header. Hand each keypress
#      to monitor_page_key. See the PAGING section below.
#   7. Add clickable tabs. Use monitor_read_input instead of a bare `read`
#      in the poll loop. On a click (MONITOR_INPUT_COL is set), call
#      monitor_mouse_click. See MOUSE INPUT below.
#   8. Add a second tab strip to ONE page. Call monitor_subtabs_init once.
#      Put monitor_render_subtabs in that page's own render function. Hand
#      , and . to monitor_subtab_key. Hand a click to monitor_subtab_click.
#      See SUB-PAGING below.
#
# This file also carries shared helpers outside that flow:
#   - monitor_section_heading -- one line for a page section's title.
#   - VALUE FORMATTERS -- sizes, ages, and threshold colours.
#   - GPU FACTS -- per-process GPU attribution, device-handle holders,
#     port-to-provider mapping.
# These live here, not in one monitor, for two reasons. Getting GPU
# attribution wrong has already cost a healthy instance once. "Red at 90%"
# must mean the same thing on every screen.

c_reset=$'\033[0m'; c_bold=$'\033[1m'
c_cyan=$'\033[36m'; c_green=$'\033[32m'; c_yellow=$'\033[33m'; c_red=$'\033[31m'
c_magenta=$'\033[35m'; c_blue=$'\033[34m'; c_gray=$'\033[90m'

# monitor_parse_args <default_section|""> "$@"
# Sets $REFRESH and populates SORT_FIELD[]/SORT_DESC[]/FILTER_VAL[] (associative
# arrays the caller must already have declared -A before calling this).
#
# If default_section is non-empty (a script with exactly one sortable section, e.g.
# download_monitor.sh's "downloads"), --sort=field[:dir] and --filter=text are parsed
# with NO section prefix and stored under that one section key -- this is what keeps
# download_monitor.sh's own command line free of a section name it doesn't need.
# If default_section is empty (a script with several sections, e.g. ai_monitor.sh),
# --sort=section:field[:dir] and --filter=section:text are required, section being
# whatever precedes the first ':'.
#
# dir accepts asc/a (default -- so leaving it off is the normal case) or desc/d.
monitor_parse_args() {
    local default_section="$1"; shift
    REFRESH="${MONITOR_DEFAULT_REFRESH:-2}"
    local arg spec sec rest field dir
    for arg in "$@"; do
        case "$arg" in
            --sort=*)
                spec="${arg#--sort=}"
                if [ -n "$default_section" ]; then
                    sec="$default_section"; rest="$spec"
                else
                    sec="${spec%%:*}"; rest="${spec#*:}"
                fi
                if [[ "$rest" == *:* ]]; then
                    field="${rest%%:*}"; dir="${rest#*:}"
                else
                    field="$rest"; dir=""
                fi
                SORT_FIELD[$sec]="$field"
                case "$dir" in
                    desc|d|descending) SORT_DESC[$sec]=1 ;;
                    *)                 SORT_DESC[$sec]=0 ;;
                esac
                ;;
            --filter=*)
                spec="${arg#--filter=}"
                if [ -n "$default_section" ]; then
                    sec="$default_section"
                    FILTER_VAL[$sec]="$spec"
                else
                    sec="${spec%%:*}"
                    FILTER_VAL[$sec]="${spec#*:}"
                fi
                ;;
            [0-9]*)
                REFRESH="$arg"
                ;;
        esac
    done
}

# apply_sort_filter <json_array> <section> <filter_field>
# Filters on FILTER_VAL[section] (case-insensitive substring match against
# filter_field), then sorts on SORT_FIELD[section] (numeric or string -- jq's
# sort_by handles both transparently), reversed if SORT_DESC[section]=1. Falls back
# to the unfiltered/unsorted input on any jq error (e.g. a typo'd field name) rather
# than blanking the section -- a bad --sort= value should degrade, not hide data.
apply_sort_filter() {
    local json="$1" section="$2" filter_field="$3"
    local fval="${FILTER_VAL[$section]:-}"
    local sfield="${SORT_FIELD[$section]:-}"
    local sdesc="${SORT_DESC[$section]:-0}"
    if [ -n "$fval" ] && [ -n "$filter_field" ]; then
        local fval_lc
        fval_lc=$(tr '[:upper:]' '[:lower:]' <<< "$fval")
        json=$(jq --arg f "$filter_field" --arg v "$fval_lc" \
            '[.[] | select((.[$f] // "" | tostring | ascii_downcase) | contains($v))]' <<< "$json" 2>/dev/null || printf '%s' "$json")
    fi
    if [ -n "$sfield" ]; then
        if [ "$sdesc" -eq 1 ]; then
            json=$(jq --arg f "$sfield" '(sort_by(.[$f])) | reverse' <<< "$json" 2>/dev/null || printf '%s' "$json")
        else
            json=$(jq --arg f "$sfield" 'sort_by(.[$f])' <<< "$json" 2>/dev/null || printf '%s' "$json")
        fi
    fi
    printf '%s' "$json"
}

# monitor_section_status <section>
# The "  filter: text" suffix a section's own title line appends, so an active
# filter is visible without checking the command line. Sort state is deliberately
# NOT repeated here -- monitor_render_header already marks the sorted column
# directly on the header row (^/v), which is the more legible place for it; showing
# it a second time on the title line was redundant.
monitor_section_status() {
    local sec="$1" bits=""
    [ -n "${FILTER_VAL[$sec]:-}" ] && bits+="  filter: ${FILTER_VAL[$sec]}"
    printf '%s' "$bits"
}

# monitor_section_heading <color> <TITLE> [subtitle] [status_section]
# The recurring page-section title: bold TITLE in <color>, an optional gray parenthetical
# subtitle, and an optional trailing filter-status suffix (monitor_section_status) -- pulled
# out once a dozen-plus call sites had it byte-for-byte identical except for these four values,
# the same reasoning monitor_render_header already applies to table header rows. <color> is a
# color VARIABLE's value (pass "$c_cyan", not the literal word), matching how every other
# color-taking helper in this file works.
#
# Covers the dominant heading style in this codebase only -- a handful of headings (the
# OLLAMA/VLLM catalogue subsections on the models page) use a different, deliberately distinct
# em-dash inline style with no subtitle color, and stay hand-rolled rather than being forced
# through a parameter that doesn't fit their shape.
monitor_section_heading() {
    local color="$1" title="$2" subtitle="${3:-}" status_section="${4:-}"
    local out="${c_bold}${color}${title}${c_reset}"
    [ -n "$subtitle" ] && out+="  ${c_gray}(${subtitle})${c_reset}"
    [ -n "$status_section" ] && out+="$(monitor_section_status "$status_section")"
    printf '%s' "$out"
}

# monitor_render_header <printf_format> <sort_field> <sort_desc:0|1> <field:Label> [field:Label ...]
# Renders a BOLD header row using the exact same printf format string the section's
# data rows use (pass the plain, uncolored version if the data rows wrap individual
# %s args in semantic color codes -- those codes live in the format string around a
# %s, not inside the value, so they don't affect column width either way; only the
# header's own copy of the format string needs to skip them).
#
# Appends "^" to whichever column's field name matches sort_field (ascending, the
# default) or "v" if SORT_DESC says descending. No column gets a marker when nothing
# is being sorted in this section right now.
monitor_render_header() {
    local fmt="$1" sort_field="$2" sort_desc="$3"
    shift 3
    local args=() colspec field label
    for colspec in "$@"; do
        field="${colspec%%:*}"
        label="${colspec#*:}"
        if [ -n "$sort_field" ] && [ "$field" = "$sort_field" ]; then
            if [ "$sort_desc" -eq 1 ]; then
                label="${label} v"
            else
                label="${label} ^"
            fi
        fi
        args+=("$label")
    done
    printf "${c_bold}${fmt}${c_reset}" "${args[@]}"
}

# ============================================================================
# CLICKABLE TABLE HEADERS: click a column to toggle its sort -- ascending, then
# descending, then off. Same idea as the page-bar and sub-tab click support, one level
# further in: this time MULTIPLE header rows can be on screen at once (the focus page
# alone has two, GPU LOAD and LOADED MODELS, each its own section), so tracking can't
# reuse a single global column-span array the way the page bar and sub-tab strip do.
# Storage is keyed by SECTION name instead: MONITOR_HEADER_ROW[section] is the screen
# row that section's header last rendered on, MONITOR_HEADER_COLS[section] is its
# column spans as a space-separated "field:lo:hi" string.
#
#   MONITOR_HEADER_ROW[focusgpu]=$(( ${#lines[@]} + 1 ))   # BEFORE the header line
#   MONITOR_HEADER_COLS[focusgpu]=$(monitor_header_colspans "$FMT" "$sf" "$sd" field:LABEL ...)
#   lines+=("$(monitor_render_header "$FMT" "$sf" "$sd" field:LABEL ...)")
#   ... in the key-handling loop, after the page bar and sub-tab checks ...
#       monitor_header_click "$col" "$row" && repaint_cached_page
#
# Both assignments above are plain statements, safe as written -- unlike
# monitor_render_tabs/monitor_render_subtabs, monitor_header_colspans returns its answer
# over stdout, captured by the CALLER's own "$(...)" into the caller's own global array.
# That is a real command substitution reading a function's PRINTED output, not a function
# setting a variable INSIDE a subshell -- the distinction that caused the real bug in the
# page-bar click work (see monitor_render_tabs's own comment). Output over stdout always
# survives; a variable assignment made purely inside a subshell never does.
declare -A MONITOR_HEADER_ROW MONITOR_HEADER_COLS

# monitor_header_colspans <printf_fmt> <sort_field> <sort_desc> <field:Label> [field:Label ...]
# Same arguments as monitor_render_header (so a caller passes the identical call twice, once
# to each function, with zero risk of the two disagreeing). Prints "field:lo:hi field:lo:hi
# ..." (1-based, absolute columns, assuming the header starts at column 1 -- every header row
# in this codebase is its own full line, never sharing a line with other text).
#
# Every header format string in this codebase uses ONLY %s-family conversions (%s, %-Ns,
# %Ns -- confirmed live against every FMT/header_fmt/fmt variable that reaches
# monitor_render_header, none use %d or %f). printf never truncates a %s value that is wider
# than its declared width; it only pads a NARROWER one. That means the true printed width of
# a %s-family field is always max(declared_width, len(actual_label)) -- a fixed, simple rule
# for this one conversion type, safe to compute directly rather than re-invoking printf.
# (Re-invoking printf with later fields blanked, tried first, does NOT work: a blank value in
# a %6s slot still pads out to 6 spaces, so it can never make a later field disappear to
# isolate an earlier one's boundary -- confirmed live, wrong spans on the very first test.)
# Column advance for the sort arrow (" ^"/" v") on the currently-sorted field falls out of
# this same len(actual_label) rule for free, since the arrow is appended to the label first.
monitor_header_colspans() {
    local fmt="$1" sort_field="$2" sort_desc="$3"
    shift 3
    local -a fields=() labels=()
    local colspec field label
    for colspec in "$@"; do
        field="${colspec%%:*}"
        label="${colspec#*:}"
        if [ -n "$sort_field" ] && [ "$field" = "$sort_field" ]; then
            if [ "$sort_desc" -eq 1 ]; then label="${label} v"; else label="${label} ^"; fi
        fi
        fields+=("$field"); labels+=("$label")
    done
    local rest="$fmt" idx=0 col=1 spans="" pre num width
    while [[ "$rest" =~ ^([^%]*)%-?([0-9]*)s(.*)$ ]]; do
        pre="${BASH_REMATCH[1]}"; num="${BASH_REMATCH[2]}"; rest="${BASH_REMATCH[3]}"
        col=$(( col + ${#pre} ))   # literal text before this spec just advances the column
        width="${num:-0}"
        [ "${#labels[$idx]}" -gt "$width" ] && width=${#labels[$idx]}
        spans+="${fields[$idx]}:${col}:$(( col + width - 1 )) "
        col=$(( col + width ))
        ((idx++))
    done
    printf '%s' "$spans"
}

# monitor_toggle_sort <section> <field>
# Cycles SECTION's sort state for FIELD: unsorted -> ascending -> descending -> unsorted.
# Clicking a DIFFERENT column while one is already sorted starts the new column fresh at
# ascending -- it does not resume wherever the old column's own cycle was.
monitor_toggle_sort() {
    local section="$1" field="$2"
    if [ "${SORT_FIELD[$section]:-}" != "$field" ]; then
        SORT_FIELD[$section]="$field"; SORT_DESC[$section]=0
    elif [ "${SORT_DESC[$section]:-0}" -eq 0 ]; then
        SORT_DESC[$section]=1
    else
        SORT_FIELD[$section]=""; SORT_DESC[$section]=0
    fi
}

# monitor_header_click <col> <row> -- finds the section whose header last rendered on ROW,
# hit-tests COL against its recorded column spans, and toggles that column's sort on a hit.
# Returns 0 on a hit, 1 otherwise (row matched no section, or column matched no span).
monitor_header_click() {
    local col="$1" row="$2" section span field rest lo hi
    for section in "${!MONITOR_HEADER_ROW[@]}"; do
        [ "${MONITOR_HEADER_ROW[$section]}" = "$row" ] || continue
        for span in ${MONITOR_HEADER_COLS[$section]:-}; do
            field="${span%%:*}"; rest="${span#*:}"
            lo="${rest%%:*}"; hi="${rest#*:}"
            if [ "$col" -ge "$lo" ] && [ "$col" -le "$hi" ]; then
                monitor_toggle_sort "$section" "$field"
                return 0
            fi
        done
        return 1
    done
    return 1
}


# ============================================================================
# CLICKABLE TABLE CELLS: click a data cell to copy its raw value to the system clipboard.
#
# One level below header clicks: a header row's columns apply to every data row beneath it,
# but a DATA row's own column boundaries can differ from the header's -- a value wider than
# both its spec width and its header label (e.g. "41555/46068" under "USED/TOTAL") still
# isn't truncated by printf, so it pushes every later column right, same as a sort arrow does
# to a header. Each row needs its OWN column map, not a copy of the header's.
#
# Storage is keyed by SCREEN ROW (not section): MONITOR_CELL_COLS[row] is that row's own
# "field:lo:hi" spans (same format monitor_header_colspans already produces -- reused as-is,
# since it is a pure text-layout function that has no idea whether the "labels" it is handed
# are header text or real data values). MONITOR_CELL_VALUES["row:field"] is the RAW value for
# that cell, exactly as it should be copied -- not the color-coded, truncated, or padded
# string that actually prints, which is why the caller passes both the display fmt AND the
# real values separately, the same split monitor_render_header/monitor_header_colspans uses
# for labels vs the sort arrow.
#
#   row=$(( ${#lines[@]} + 1 ))                                    # BEFORE the data line
#   monitor_register_row_cells "$row" "$fmt" gpu:"$idx" util:"$util" ...
#   lines+=("$(printf "$fmt" "$idx" "$util" ...)")
#   ... in the key-handling loop, after the header-click check ...
#       monitor_cell_click "$col" "$row" && repaint_cached_page
#
# The action on a hit (today: copy to the clipboard) is NOT hardcoded into monitor_cell_click
# itself -- it sets MONITOR_CELL_FIELD/MONITOR_CELL_VALUE (same output-globals convention as
# MONITOR_INPUT_KEY/COL/ROW) and returns 0, leaving the CALLER to decide what a hit means. A
# future feature (jump to a detail page for one field, say) is a different function reading
# those same two globals -- monitor_cell_click's own job stays purely "what did this click
# land on," identical in spirit to how monitor_row_click would separate lookup from policy.
declare -A MONITOR_CELL_COLS MONITOR_CELL_VALUES

# monitor_register_row_cells <row> <printf_fmt> <field:value> [field:value ...]
# Records one data row's real column spans and raw values. Call this once per printed row,
# right before appending that row to `lines` -- same "compute the row number from
# ${#lines[@]} first" convention monitor_header_click's own callers already use.
monitor_register_row_cells() {
    local row="$1" fmt="$2"
    shift 2
    local -a colspecs=() field value
    for colspec in "$@"; do
        field="${colspec%%:*}"; value="${colspec#*:}"
        colspecs+=("${field}:${value}")
        MONITOR_CELL_VALUES["${row}:${field}"]="$value"
    done
    MONITOR_CELL_COLS["$row"]="$(monitor_header_colspans "$fmt" "" 0 "${colspecs[@]}")"
}

# monitor_cell_click <col> <row> -- hit-tests a click against ROW's recorded column spans.
# Sets MONITOR_CELL_FIELD/MONITOR_CELL_VALUE and returns 0 on a hit, 1 otherwise (no cell
# registered for this row, or the column missed every span).
monitor_cell_click() {
    local col="$1" row="$2" span field rest lo hi
    MONITOR_CELL_FIELD=""; MONITOR_CELL_VALUE=""; MONITOR_CELL_COL_LO=""; MONITOR_CELL_COL_HI=""
    [ -n "${MONITOR_CELL_COLS[$row]:-}" ] || return 1
    for span in ${MONITOR_CELL_COLS[$row]}; do
        field="${span%%:*}"; rest="${span#*:}"
        lo="${rest%%:*}"; hi="${rest#*:}"
        if [ "$col" -ge "$lo" ] && [ "$col" -le "$hi" ]; then
            MONITOR_CELL_FIELD="$field"
            MONITOR_CELL_VALUE="${MONITOR_CELL_VALUES[${row}:${field}]:-}"
            MONITOR_CELL_COL_LO="$lo"; MONITOR_CELL_COL_HI="$hi"
            return 0
        fi
    done
    return 1
}

# monitor_clipboard_copy <text>
# Writes TEXT to the terminal's system clipboard via OSC 52 -- the one escape sequence a
# terminal app can use to reach the REAL system clipboard, not just an in-app selection
# buffer. Supported by every terminal this dashboard is actually run in (iTerm2, Windows
# Terminal, most tmux configs with `set -g allow-passthrough on`); a terminal that doesn't
# recognize it just ignores the sequence, so this is safe to call unconditionally with no
# capability check. Base64 is the wire format OSC 52 itself requires, not a choice made here.
monitor_clipboard_copy() {
    local text="$1" b64
    b64=$(printf '%s' "$text" | base64 | tr -d '\n')
    printf '\033]52;c;%s\a' "$b64"
}

# monitor_flash [duration_seconds]
# Briefly swaps the WHOLE screen to reverse video and back -- DECSCNM (terminal mode 5), a
# real, standard capability (xterm and every derivative: iTerm2, Windows Terminal's ConPTY,
# tmux passthrough), not a custom re-render. Exists because a click that succeeds silently
# gives no confirmation it registered at all -- unlike a page switch or a header's sort arrow,
# where the SCREEN CONTENT itself visibly changes, a cell copy leaves every character on
# screen identical before and after, so there is nothing else to signal it happened. Blocks
# for duration_seconds (default 0.15) -- short enough to read as a flash, long enough to
# actually be seen; this is a deliberate pause, not a bug, and the caller decides whether an
# action deserves one (see cell-click's own use of this next to header-click/page-click,
# which already get their feedback for free from the content change).
monitor_flash() {
    local duration="${1:-0.15}"
    printf '\033[?5h'
    sleep "$duration"
    printf '\033[?5l'
}

# monitor_flash_cell <row> <col_lo> <col_hi> <text> [duration_seconds]
# Briefly re-prints TEXT in reverse video at the cell's own on-screen position (row, col_lo
# through col_hi) -- confirms a click landed without touching anything else on screen, unlike
# monitor_flash's whole-screen blink (which a single cell-copy click made feel like an alarm,
# not a confirmation -- real feedback, not guessed). Reverse video, not a specific background
# color: it inverts whatever the terminal's own current colors are, so it reads correctly on
# a light or a dark theme without this file having to pick one.
#
# No separate "restore" step. The caller's own next full redraw is already imminent --
# ai_monitor.sh's main loop returns straight to build_frame() right after handling a click --
# so this only ever needs to SHOW the flash; the very next frame overwrites it back to the
# real, correctly formatted cell content on its own.
monitor_flash_cell() {
    local row="$1" col_lo="$2" col_hi="$3" text="$4" duration="${5:-0.15}"
    local width=$(( col_hi - col_lo + 1 ))
    tput cup $(( row - 1 )) $(( col_lo - 1 ))
    printf '\033[7m%-*.*s\033[0m' "$width" "$width" "$text"
    sleep "$duration"
}

# monitor_draw_frame <array_name>
# Flicker-free redraw: move the cursor back to the top-left and overwrite in place
# instead of clear-ing the whole screen. Each line is individually erased to its own
# end (tput el) before the next frame's content lands on it -- this is what handles a
# line getting SHORTER (e.g. fewer installed models) without leaving stray characters
# trailing off the end of that row. tput ed at the very end handles the frame's TOTAL
# line count shrinking, clearing any full rows left over below the new content.
monitor_draw_frame() {
    local -n _lines=$1
    local i=0
    for line in "${_lines[@]}"; do
        tput cup "$i" 0
        printf '%s' "$line"
        tput el
        ((i++))
    done
    tput ed
}

# monitor_setup_screen
# Hides the cursor and does the one full clear a dashboard should ever do (every
# subsequent frame uses monitor_draw_frame's in-place redraw instead). Call
# monitor_restore_cursor yourself in an INT/TERM trap and at normal exit -- it's
# deliberately not wired up automatically here, since a script may want to do its own
# cleanup (e.g. removing a tmpdir) alongside restoring the cursor.
monitor_setup_screen() {
    tput civis 2>/dev/null
    clear
    # Button-event mouse tracking (1000) + SGR extended coordinates (1006, needed past 223
    # columns/rows and gives an unambiguous release byte -- the older X10/normal mode encodes
    # coordinates as raw bytes offset by 32, which breaks past column 223 and is genuinely
    # ambiguous to parse). Both are a real terminal mode switch, not a formatting trick -- MUST
    # be paired with the disable sequence in monitor_restore_cursor, or a terminal left in this
    # mode after the dashboard exits reports every click as garbage escape-sequence text.
    printf '\e[?1000h\e[?1006h'
}

monitor_restore_cursor() {
    printf '\e[?1000l\e[?1006l'
    tput cnorm 2>/dev/null
}

# ============================================================================
# PAGING
#
# Added 2026-09-02 because ai_monitor.sh had outgrown a single screen: every section
# rendered into one frame and the bottom of it was effectively invisible. Sections now
# group onto pages you switch with a keypress. Lives here, not in ai_monitor.sh, so any
# monitor gets paging for free the moment it outgrows one screen too -- the same reason
# the redraw loop and the sort/filter parsing ended up in this file.
#
# A paged monitor does:
#   monitor_pages_init "$WANTED_PAGE" focus gpu system models   # "" = start on the first
#   ... build the frame, using $MONITOR_PAGE to pick what to render, and
#       monitor_render_tabs for the header's page bar ...
#   if read -s -t "$REFRESH" -n 1 key; then
#       case "$key" in q|Q) quit ;; *) monitor_page_key "$key" ;; esac
#   fi
# ============================================================================

# monitor_pages_init <wanted_page|""> <name> [name ...]
# Declares the page set and picks the starting page. An unknown or empty <wanted_page>
# falls back to the first page rather than erroring -- a typo'd --page= should land you
# somewhere useful, not abort a dashboard.
monitor_pages_init() {
    local want="$1"; shift
    MONITOR_PAGES=("$@")
    MONITOR_PAGE="${MONITOR_PAGES[0]}"
    if [ -n "$want" ]; then
        case " ${MONITOR_PAGES[*]} " in *" $want "*) MONITOR_PAGE="$want" ;; esac
    fi
}

# monitor_page_index [page]   -- position of $1 (default: the current page) in MONITOR_PAGES
monitor_page_index() {
    local target="${1:-$MONITOR_PAGE}" i=0 p
    for p in "${MONITOR_PAGES[@]}"; do
        [ "$p" = "$target" ] && { printf '%d' "$i"; return; }
        ((i++))
    done
    printf '0'
}

# monitor_render_tabs [prefix_visible_cols]
# The header's page bar: "[1 focus] 2 gpu  3 system  4 models", current page bracketed and
# bold. Numbers are 1-based because they double as the keys that jump to each page.
#
# prefix_visible_cols is how many VISIBLE (non-ANSI) columns already sit on the header line
# before this string starts (the caller's own title/timestamp text, measured with plain
# ${#...} since none of it carries color codes) -- needed to record where each tab ACTUALLY
# lands on screen, for mouse hit-testing (see MOUSE INPUT below).
#
# SETS MONITOR_TABS_RENDERED (the printable string) AND MONITOR_TAB_COLS -- does NOT print to
# stdout, and the caller MUST invoke it as a plain statement, never inside "$(...)". A command
# substitution forks a subshell; every variable this function sets would be assigned in that
# child and vanish the instant it exits, so MONITOR_TAB_COLS would look permanently empty to
# everything else (confirmed live: this was the ACTUAL bug on the first pass of mouse support --
# monitor_mouse_click always found zero columns to hit-test against, silently, because
# header_line() had been calling this via "$(monitor_render_tabs ...)").
monitor_render_tabs() {
    local prefix_len="${1:-0}" out="" i=0 p col=$(( ${1:-0} + 1 )) label seg
    MONITOR_TAB_COLS=()
    for p in "${MONITOR_PAGES[@]}"; do
        ((i++))
        # PLAIN rendered text, brackets included when this is the current page -- NOT just
        # "$i $p", which undercounts the current tab by 2 (the brackets are real, printed
        # characters that move every later column). Real bug this caused, confirmed live:
        # every tab AFTER the selected one was predicted 2 columns to the left of where it
        # actually sits on screen, so a click precisely on a later tab's visible label could
        # register as the tab before it instead.
        if [ "$p" = "$MONITOR_PAGE" ]; then label="[$i $p]"; else label="$i $p"; fi
        seg="${label} "
        # 1-based inclusive column span this tab occupies, INCLUDING its trailing space --
        # a click anywhere in that gap still clearly means "this tab", the same as clicking
        # the label itself.
        MONITOR_TAB_COLS+=("${col}:$(( col + ${#seg} - 1 ))")
        if [ "$p" = "$MONITOR_PAGE" ]; then out+="${c_bold}${c_cyan}${label}${c_reset} "
        else                                out+="${c_gray}${label}${c_reset} "; fi
        col=$(( col + ${#seg} ))
    done
    MONITOR_TABS_RENDERED="$out"
}

# ============================================================================
# MOUSE INPUT
#
# xterm SGR mouse reporting (enabled in monitor_setup_screen) sends a click as its own
# multi-byte escape sequence -- ESC [ < Cb ; Cx ; Cy M (press) or ...m (release) -- not a
# single key. The existing read pattern (`read -s -t "$REFRESH" -n 1 key`) only ever grabs
# the leading ESC byte and leaves the rest to be misread as spurious keypresses on the next
# poll cycle. monitor_read_input replaces that bare read for a caller wanting mouse support:
# same blocking-up-to-timeout contract, but understands both shapes of input.
# ============================================================================

# monitor_read_input <timeout>
# Sets MONITOR_INPUT_KEY (a single character, exactly like a raw `read` result) for a
# keypress, or leaves it empty and sets MONITOR_INPUT_COL/MONITOR_INPUT_ROW (1-based,
# matching monitor_render_tabs's own column spans) for a mouse event. Returns 0 if
# something was read before the timeout, 1 on a plain timeout -- same convention `read`
# itself uses, so a caller's existing `if monitor_read_input ...; then` still works.
monitor_read_input() {
    local timeout="$1" key rest seq="" c cb col row term
    MONITOR_INPUT_KEY=""; MONITOR_INPUT_COL=""; MONITOR_INPUT_ROW=""
    read -s -t "$timeout" -n 1 key || return 1
    if [ "$key" != $'\e' ]; then
        MONITOR_INPUT_KEY="$key"
        return 0
    fi
    # Got ESC -- a real mouse report is one atomic terminal write, so the rest of it
    # arrives within milliseconds; a short fixed timeout here is safe. A bare Escape
    # keypress with nothing to follow (this dashboard has no text entry, so that's the
    # only other source of a lone ESC) falls through the same path as a non-match below
    # and is treated as a no-op rather than misread as a navigation key.
    read -s -t 0.05 -n 2 rest 2>/dev/null
    [ "$rest" != "[<" ] && return 0
    while read -s -t 0.05 -n 1 c 2>/dev/null; do
        seq+="$c"
        { [ "$c" = "M" ] || [ "$c" = "m" ]; } && break
    done
    [[ "$seq" =~ ^([0-9]+)\;([0-9]+)\;([0-9]+)(M|m)$ ]] || return 0
    cb="${BASH_REMATCH[1]}"; col="${BASH_REMATCH[2]}"; row="${BASH_REMATCH[3]}"; term="${BASH_REMATCH[4]}"
    # Only a PRESS (M) of the left button (Cb, low bits 0) counts as a click -- a release,
    # a drag, or a scroll-wheel event (Cb has its own distinct bits set for those) is
    # intentionally ignored rather than double-firing once per press+release pair.
    { [ "$term" = "M" ] && [ "$cb" = "0" ]; } || return 0
    MONITOR_INPUT_COL="$col"; MONITOR_INPUT_ROW="$row"
    return 0
}

# monitor_mouse_click <col> <row>
# Hit-tests a click (1-based, from monitor_read_input) against the top-level page bar's
# recorded column spans (monitor_render_tabs). The page bar is always the dashboard's
# first drawn line, so ROW must be 1 -- any other row isn't this function's business (a
# page's own content hit-testing, if one ever wants it, is a separate concern). Returns 0
# and switches MONITOR_PAGE if a tab was hit, 1 otherwise.
monitor_mouse_click() {
    local col="$1" row="$2" i span lo hi
    [ "$row" != "1" ] && return 1
    for i in "${!MONITOR_TAB_COLS[@]}"; do
        span="${MONITOR_TAB_COLS[$i]}"
        lo="${span%%:*}"; hi="${span##*:}"
        if [ "$col" -ge "$lo" ] && [ "$col" -le "$hi" ]; then
            MONITOR_PAGE="${MONITOR_PAGES[$i]}"
            return 0
        fi
    done
    return 1
}

# monitor_page_key <key>
# Handles the page-navigation keys: 1-9 jump to that page, n/N/Tab next, p/P previous.
# Returns 0 if the key was a navigation key, 1 if the caller should handle it (q, or
# anything else -- an unhandled key still forces an early redraw, which usefully doubles
# as "refresh now").
monitor_page_key() {
    local key="$1" n="${#MONITOR_PAGES[@]}" idx
    case "$key" in
        [1-9])
            idx=$(( key - 1 ))
            [ "$idx" -lt "$n" ] && MONITOR_PAGE="${MONITOR_PAGES[$idx]}"
            return 0 ;;
        n|N|$'\t')
            MONITOR_PAGE="${MONITOR_PAGES[$(( ( $(monitor_page_index) + 1 ) % n ))]}"
            return 0 ;;
        p|P)
            MONITOR_PAGE="${MONITOR_PAGES[$(( ( $(monitor_page_index) + n - 1 ) % n ))]}"
            return 0 ;;
    esac
    return 1
}

# ============================================================================
# SUB-PAGING: a second, independent tab strip WITHIN one top-level page
#
# The top-level page bar above (MONITOR_PAGES/monitor_page_key) means "switch to a whole
# different screen" and owns the 1-9/n/p/Tab keys. A page can also want its OWN tab strip
# one level down -- e.g. ai_monitor.sh's provider page, one tab per backend, everything
# that backend exposes. Reusing 1-9 for that would be ambiguous (does "3" mean "top-level
# page 3" or "sub-tab 3"? -- answer depends on which page you're already on), so this uses
# , and . (the unshifted < and > keys) instead, a key pair nothing else in this file claims.
#
#   monitor_subtabs_init ollama vllm colibri llamacppedge   # once, alongside monitor_pages_init
#   ... in the page's own render function, called as a PLAIN STATEMENT, never inside "$(...)"
#   ... (a subshell would discard MONITOR_SUBTAB_COLS/MONITOR_SUBTAB_ROW same as the top-level
#   ... tab bar's own bug -- see monitor_render_tabs's comment for the real incident) ...
#       MONITOR_SUBTAB_ROW=$(( ${#lines[@]} + 1 ))   # which screen row this line will land on
#       monitor_render_subtabs "$prefix_len"          # sets MONITOR_SUBTABS_RENDERED + _COLS
#       lines+=("...prefix text...${MONITOR_SUBTABS_RENDERED}")
#       case "${MONITOR_SUBTABS[$MONITOR_SUBTAB_IDX]}" in ollama) ... ;; esac
#   ... in the key-handling loop, guarded by the caller to the ONE page that owns these ...
#       [ "$MONITOR_PAGE" = "provider" ] && monitor_subtab_key "$key" && repaint_cached_page
#       [ "$MONITOR_PAGE" = "provider" ] && monitor_subtab_click "$col" "$row" && repaint_cached_page
#
# Deliberately global/singular (one MONITOR_SUBTABS registration, not a stack or a map keyed
# by page) -- only one page in this codebase needs sub-tabs today, and the page-scoping guard
# already has to live in the CALLER anyway (this file has no idea what "provider" means, same
# as monitor_page_key never knowing what any top-level page name means). Add a second
# registration mechanism only once a second script actually needs two independent sub-tab
# groups alive at once -- YAGNI until then.
# ============================================================================

declare -a MONITOR_SUBTABS=()
MONITOR_SUBTAB_IDX=0
# 1-based screen row the sub-tab strip last rendered on, and its per-tab column spans -- the
# caller sets the row (only IT knows what line its own content lands on; this file has no idea
# what "provider" means, same as it never knows what any top-level page name means either), and
# monitor_render_subtabs fills in the columns as a side effect of building the display string.
MONITOR_SUBTAB_ROW=0
declare -a MONITOR_SUBTAB_COLS=()

# monitor_subtabs_init <name> [name ...] -- declares the sub-tab set, always starting at the
# first one. Call once, at the same place monitor_pages_init is called.
monitor_subtabs_init() {
    MONITOR_SUBTABS=("$@")
    MONITOR_SUBTAB_IDX=0
}

# monitor_render_subtabs [prefix_visible_cols]
# The sub-tab strip itself: "[ollama] vllm colibri llamacppedge", current tab bracketed and
# bold, same visual language as monitor_render_tabs one level up -- including the same
# prefix_visible_cols convention (how many VISIBLE, non-ANSI columns already sit on this line
# before the strip starts, needed to record where each tab actually lands for click hit-
# testing) and the same SETS-A-GLOBAL-INSTEAD-OF-STDOUT contract for the same reason: called
# via "$(...)", the column spans it records would be set in a forked subshell and discarded the
# instant that subshell exits. Sets MONITOR_SUBTABS_RENDERED (the printable string) and
# MONITOR_SUBTAB_COLS -- never prints to stdout, and the caller MUST invoke this as a plain
# statement.
monitor_render_subtabs() {
    local prefix_len="${1:-0}" out="" i=0 t col=$(( ${1:-0} + 1 )) label seg
    MONITOR_SUBTAB_COLS=()
    for t in "${MONITOR_SUBTABS[@]}"; do
        # Same fix as monitor_render_tabs, same real bug: PLAIN rendered text, brackets
        # included when this is the selected tab -- "$t" alone undercounts it by 2, shifting
        # every later tab's predicted column 2 short of where it actually renders.
        if [ "$i" -eq "$MONITOR_SUBTAB_IDX" ]; then label="[$t]"; else label="$t"; fi
        seg="${label} "
        MONITOR_SUBTAB_COLS+=("${col}:$(( col + ${#seg} - 1 ))")
        if [ "$i" -eq "$MONITOR_SUBTAB_IDX" ]; then out+="${c_bold}${c_cyan}${label}${c_reset} "
        else                                          out+="${c_gray}${label}${c_reset} "; fi
        col=$(( col + ${#seg} ))
        ((i++))
    done
    MONITOR_SUBTABS_RENDERED="$out"
}

# monitor_subtab_click <col> <row> -- hit-tests a click against the sub-tab strip's recorded
# column spans, but only on the exact row it last rendered on (MONITOR_SUBTAB_ROW) -- a click
# at that same row/col on a DIFFERENT page (where the sub-tab strip isn't even showing) would
# otherwise silently rotate MONITOR_SUBTAB_IDX for a page the viewer isn't looking at, which is
# why the caller is expected to also gate this on its own page-name check (see the usage
# comment above) rather than relying on the row match alone. Returns 0 and switches
# MONITOR_SUBTAB_IDX if a tab was hit, 1 otherwise.
monitor_subtab_click() {
    local col="$1" row="$2" i span lo hi
    [ "$row" != "$MONITOR_SUBTAB_ROW" ] && return 1
    for i in "${!MONITOR_SUBTAB_COLS[@]}"; do
        span="${MONITOR_SUBTAB_COLS[$i]}"
        lo="${span%%:*}"; hi="${span##*:}"
        if [ "$col" -ge "$lo" ] && [ "$col" -le "$hi" ]; then
            MONITOR_SUBTAB_IDX="$i"
            return 0
        fi
    done
    return 1
}

# monitor_subtab_key <key> -- , / . rotate MONITOR_SUBTAB_IDX. Returns 0 if the key was , or .
# AND at least one sub-tab is registered (so an unrelated script sourcing this file gets a
# harmless no-op, never a rotation into an empty array), 1 otherwise -- same "0 = I handled
# this, 1 = caller's problem" convention monitor_page_key already uses.
monitor_subtab_key() {
    local key="$1" n="${#MONITOR_SUBTABS[@]}"
    [ "$n" -eq 0 ] && return 1
    case "$key" in
        .) MONITOR_SUBTAB_IDX=$(( (MONITOR_SUBTAB_IDX + 1) % n )); return 0 ;;
        ,) MONITOR_SUBTAB_IDX=$(( (MONITOR_SUBTAB_IDX + n - 1) % n )); return 0 ;;
    esac
    return 1
}

# ============================================================================
# VALUE FORMATTERS AND VALUE-DEPENDENT COLOURS
#
# Generic enough that every dashboard wants them, and small enough that each was getting
# re-typed slightly differently per script. The colour thresholds are shared on purpose:
# "red at 90%" should mean the same thing on every screen here.
# ============================================================================

monitor_pct_color() {
    local pct="${1:-0}"
    if   (( pct >= 90 )); then printf '%s' "$c_red"
    elif (( pct >= 70 )); then printf '%s' "$c_yellow"
    else                       printf '%s' "$c_green"
    fi
}

monitor_temp_color() {
    local t="${1:-0}"
    if   (( t >= 80 )); then printf '%s' "$c_red"
    elif (( t >= 60 )); then printf '%s' "$c_yellow"
    else                     printf '%s' "$c_green"
    fi
}

# monitor_fmt_gib <mib>   -- 44118 -> "43.1 GiB". Takes MiB in, because that is the unit every
# source on this box natively reports (nvidia-smi directly, /proc and free after a /1048576).
# The unit is spelled GiB, not G, so it cannot be mistaken for the decimal GB a vendor spec
# sheet would quote -- these are all binary multiples, and mixing the two is how a "755 GB"
# machine appears to be missing 20 GB of RAM.
monitor_fmt_gib() { awk -v m="${1:-0}" 'BEGIN{printf "%.1f GiB", m/1024}'; }

# monitor_etime_to_secs <etime>
# The inverse of monitor_fmt_age, for when you are handed ps's `etime` column rather than
# choosing the ps format yourself. ps's etime changes FORMAT with magnitude (SS, then
# MM:SS, then HH:MM:SS, then DD-HH:MM:SS) -- sorting those strings lexically is wrong
# across a magnitude boundary (e.g. "9:23" sorts after "10:23:45" despite being far
# shorter). Converts to plain seconds so a --sort= on elapsed time is actually correct.
# If you control the ps call, prefer `ps -o etimes=` (plural), which gives seconds
# directly and needs none of this -- that is what monitor_collect_gpu_procs does.
monitor_etime_to_secs() {
    local t="$1" days=0 rest="$1" h=0 m=0 s=0 parts
    if [[ "$t" == *-* ]]; then
        days="${t%%-*}"
        rest="${t#*-}"
    fi
    IFS=: read -ra parts <<< "$rest"
    case "${#parts[@]}" in
        1) s="${parts[0]}" ;;
        2) m="${parts[0]}"; s="${parts[1]}" ;;
        3) h="${parts[0]}"; m="${parts[1]}"; s="${parts[2]}" ;;
    esac
    printf '%d' $(( 10#$days*86400 + 10#$h*3600 + 10#$m*60 + 10#$s ))
}

# monitor_fmt_age <seconds>   -- 2040 -> "34m", 16620 -> "4h37m", 191000 -> "2d5h".
# Age is how you tell a thing that just started from one that has held a GPU since
# yesterday, which is half of deciding whether it is safe to touch.
monitor_fmt_age() {
    local s="${1:-0}"
    if   [ "$s" -lt 3600 ]  ; then printf '%dm' $((s/60))
    elif [ "$s" -lt 86400 ] ; then printf '%dh%dm' $((s/3600)) $(((s%3600)/60))
    else printf '%dd%dh' $((s/86400)) $(((s%86400)/3600)); fi
}

# monitor_fmt_until <iso8601_expires_at>
# Ollama reports a per-model absolute unload time (expires_at, ISO-8601), not a countdown --
# this converts it to the same "4m"/"1h2m" shape monitor_fmt_age already uses for elapsed
# age, so a loaded model's remaining keep_alive reads the same way its age does. A vLLM/
# colibri instance has no keep_alive concept at all (manual start/stop only) -- pass "" for
# those and get "-" back rather than a fabricated countdown.
#
# A PAST expires_at ON A STILL-LOADED MODEL IS "overdue": the keep_alive timer fired and the model
# is still there, i.e. the unload is pending or stuck. It is NOT a sign of use -- measured on Ollama
# 0.33.1 (2026-09-30), while a request is in flight /api/ps reports expires_at as query-time +
# keep_alive, recomputed on every call, so it can never be in the past during use (see
# ollama_busy_models in ai_monitor.sh, which uses exactly that to detect use). Every caller passes
# a model /api/ps still lists. Seen 2026-09-29: muse-glimmer:30b 7 min overdue, still loaded.
monitor_fmt_until() {
    local iso="${1:-}" now exp delta
    [ -z "$iso" ] && { printf -- '-'; return; }
    exp=$(date -d "$iso" +%s 2>/dev/null) || { printf -- '-'; return; }
    now=$(date +%s)
    delta=$(( exp - now ))
    if [ "$delta" -le 0 ]; then
        printf 'overdue'
    else
        monitor_fmt_age "$delta"
    fi
}

# monitor_human_to_bytes <num> <unit>
# Ollama's "SIZE" column is a human string like "18 GB" -- converted to bytes so a
# --sort= on it is a real numeric sort, not lexical ("9 GB" sorting before "18 GB").
monitor_human_to_bytes() {
    local num="$1" unit="$2"
    case "$unit" in
        GB|GiB) awk -v n="$num" 'BEGIN{printf "%d", n*1073741824}' ;;
        MB|MiB) awk -v n="$num" 'BEGIN{printf "%d", n*1048576}' ;;
        KB|KiB) awk -v n="$num" 'BEGIN{printf "%d", n*1024}' ;;
        *)      printf '0' ;;
    esac
}

# monitor_truncate <string> [maxlen]
# Drops any "org/" prefix first (meta-models/Muse-Glimmer-30B -> Muse-Glimmer-30B), since
# the identifying part of a model id is the tail, then trims with a trailing ~ if still long.
# Reach for this ONLY where the column has a hard real-estate limit that cannot grow (the
# logs page's tmux-style panes, sized against the actual terminal width) -- everywhere else,
# a truncated name is losing information for no reason a wide terminal can't afford. Prefer
# monitor_colwidth to size the column to its real content instead.
monitor_truncate() {
    local s="${1##*/}" max="${2:-26}"
    [ "${#s}" -gt "$max" ] && s="${s:0:$((max-1))}~"
    printf '%s' "$s"
}

# monitor_colwidth <min_width> [value ...]
# Widest string length among min_width and every value given. Use this to size a column
# to its actual widest entry for THIS frame instead of truncating to an arbitrary constant
# -- a name column should show the name, not "qwen2.5-vl-72b-a~". Build the printf format
# string with the result (e.g. `printf -v FMT "  %%-%ds %%s" "$w"`) rather than baking a
# fixed width into a top-level constant.
monitor_colwidth() {
    local w="$1"; shift
    local v
    for v in "$@"; do
        [ "${#v}" -gt "$w" ] && w="${#v}"
    done
    printf '%d' "$w"
}

# monitor_gray_paren <text>
# "(${text})" wrapped in grey, nothing else -- the standard way a line adds a de-emphasized
# aside after a plain-colored value (e.g. "755.2 GiB" in the normal text color, then a grey
# "(1%)" after it). Reach for this instead of wrapping the WHOLE value string in ${c_gray} --
# that swallows real data into a color meant for de-emphasis, which is exactly the bug the
# CPU/RAM section's "of 755.2 GiB (1%)" line had (the "of 755.2 GiB" part is data, not an
# aside, and reads as data everywhere else on this page).
monitor_gray_paren() {
    printf '%s(%s)%s' "$c_gray" "$1" "$c_reset"
}

# monitor_placeholder <text>
# The standard empty-state line: two-space indent, grey, nothing else. Every "nothing to
# show" message in a monitor should go through this one function -- ad hoc placeholders
# drift out of style one at a time otherwise, which is exactly how GPU PROCESSES' own
# "(no GPU processes running)" ended up the only ungreyed placeholder on the page.
# Use this ONLY as a whole standalone line (lines+=("$(monitor_placeholder ...)")) -- its
# own leading "  " assumes it owns the line's entire indentation. For a grey VALUE slotted
# into an existing label:value row (whose format string already indents via the label
# column), use monitor_gray_text instead -- using this one there double-indents the value,
# which is exactly what broke LOG ROLLOVER SETTINGS' alignment once already.
monitor_placeholder() {
    printf '  %s%s%s' "$c_gray" "$1" "$c_reset"
}

# monitor_gray_text <text>
# Grey-wraps text with NO added indent -- the sibling of monitor_placeholder for a value
# slotted into an existing row's %s (a label:value line already gets its indentation from
# the label column's own leading spaces).
monitor_gray_text() {
    printf '%s%s%s' "$c_gray" "$1" "$c_reset"
}

# ============================================================================
# GPU FACTS
#
# Shared because attributing GPU memory to a process correctly is subtle, and getting it
# wrong here has already cost a healthy instance once (ledger #31): nvidia-smi's
# compute-app rows report gpu_uuid, NOT index, and their order does NOT match index order,
# so anything wanting "which GPU is this on" must map through the uuid.
# ============================================================================

# monitor_collect_gpu_procs
# Populates, keyed by pid: MON_P_GPUS (comma-joined GPU indices), MON_P_MEM (summed MiB
# across those GPUs), MON_P_USER (owner -- the "who started it" answer), MON_P_AGE
# (seconds), MON_P_COMM (short command), MON_P_ARGS (full argv). Also MON_UUID2IDX. The
# caller must have declared all seven with `declare -A` first.
#
# MON_P_ARGS exists because a short command name is often not enough to identify what a
# process is actually serving: every Ollama model runner is called "llama-server", and the
# only thing distinguishing them is the blob sha in `--model .../sha256-<digest>`, which a
# caller can match against Ollama's own /api/ps digests to recover the model name.
monitor_collect_gpu_procs() {
    MON_P_GPUS=(); MON_P_MEM=(); MON_P_USER=(); MON_P_AGE=(); MON_P_COMM=(); MON_P_ARGS=(); MON_UUID2IDX=()
    command -v nvidia-smi &>/dev/null || return 0
    local idx uuid pid mem pname psinfo rest gi
    # nvidia-smi's CSV always puts exactly one space after each comma (verified against real
    # output, never more, never a leading/trailing one anywhere else) -- "${v# }" strips that
    # in the shell itself. The previous "$(echo "$v"|xargs)" forked two processes PER FIELD to
    # do the same trim; with ~4 GPUs and several compute-app rows every 2s refresh, that was
    # dozens of avoidable forks on every single frame, focus/gpu page or not (this function
    # runs unconditionally from build_frame). Confirmed live 2026-09-20 via bash -x timing:
    # this and the two per-page copies below were measurable, real per-frame overhead.
    while IFS=',' read -r idx uuid; do
        MON_UUID2IDX["${uuid# }"]="${idx# }"
    done < <(nvidia-smi --query-gpu=index,uuid --format=csv,noheader 2>/dev/null)
    while IFS=',' read -r pid uuid mem pname; do
        pid="${pid# }"; uuid="${uuid# }"
        mem="${mem# }"; pname="${pname# }"
        [ -z "$pid" ] && continue
        gi="${MON_UUID2IDX[$uuid]:-?}"
        if [ -n "${MON_P_GPUS[$pid]:-}" ]; then
            MON_P_GPUS[$pid]="${MON_P_GPUS[$pid]},$gi"
            MON_P_MEM[$pid]=$(( ${MON_P_MEM[$pid]:-0} + ${mem:-0} ))
        else
            MON_P_GPUS[$pid]="$gi"; MON_P_MEM[$pid]="${mem:-0}"
            # One ps call, not two -- comm= is always a single space-free token (confirmed:
            # "llama-server", "VLLM::Worker_TP0", etc.), so a plain `read` can split
            # user/etimes/comm off the front and let args (which DOES contain spaces) fall
            # into the last variable, which bash's read leaves unsplit. This used to be a
            # separate `ps -o args=` (plus the awk and head each call above it needed) --
            # 4 forked processes per unique GPU pid down to 1.
            psinfo=$(ps -o user=,etimes=,comm=,args= --no-headers -p "$pid" 2>/dev/null)
            read -r MON_P_USER[$pid] MON_P_AGE[$pid] MON_P_COMM[$pid] MON_P_ARGS[$pid] <<< "$psinfo"
            # A pid that exited between the two nvidia-smi calls has no ps row -- keep the
            # nvidia-reported process name rather than rendering a blank row.
            [ -z "${MON_P_USER[$pid]:-}" ] && { MON_P_USER[$pid]="?"; MON_P_AGE[$pid]=0; MON_P_COMM[$pid]="$pname"; }
        fi
    done < <(nvidia-smi --query-compute-apps=pid,gpu_uuid,used_memory,process_name --format=csv,noheader,nounits 2>/dev/null)
}

# monitor_gpu_handle_holders
# Emits "pid<TAB>comm<TAB>devs" for every process holding a /dev/nvidiaN handle. This is
# NOT the same set as nvidia-smi's compute apps: a process can hold a device handle (and a
# few MB of driver context with it) while never appearing in nvidia-smi at all. That is
# exactly what an otherwise-idle GPU reporting a few MiB is, and only lsof can see it.
# CUDA_VISIBLE_DEVICES does not prevent it -- that filters the CUDA runtime's device list,
# not kernel-level access, so NCCL/NVML topology probes still touch every physical GPU.
monitor_gpu_handle_holders() {
    lsof -Fpcn /dev/nvidia[0-9]* 2>/dev/null | awk '
        /^p/{pid=substr($0,2)} /^c/{comm=substr($0,2)}
        /^n\/dev\/nvidia[0-9]/{d=substr($0,13); if(!seen[pid":"d]++) devs[pid]=devs[pid] (devs[pid]?",":"") d; cm[pid]=comm}
        END{for(p in devs) printf "%s\t%s\t%s\n", p, cm[p], devs[p]}'
}

# monitor_provider_for_port <port>
# Which provider owns a listening port, per docs/PORT-ALLOCATION.md (revised 2026-09-15 --
# bands widened from a single 11000-11100 window to the full 11000-11999 decade, ~100 ports
# per provider). Both the pre-migration 8xxx numbering and the current 11xxx bands are
# accepted, so this keeps working across the migration with no edit here -- and anything
# needing to attribute a port (a monitor, a cleanup script, a court probe) gets the same
# answer from one place.
monitor_provider_for_port() {
    local p="$1"
    case "$p" in
        11434|11435) printf 'ollama'; return ;;
        8080) printf 'llamacpp-edge'; return ;;   # pre-profile llamacpp-edge single-port default
    esac
    if   [ "$p" -ge 11000 ] && [ "$p" -le 11099 ]; then printf 'vllm'
    elif [ "$p" -ge 11100 ] && [ "$p" -le 11199 ]; then printf 'vllm-edge'
    elif [ "$p" -ge 11200 ] && [ "$p" -le 11299 ]; then printf 'colibri'
    elif [ "$p" -ge 11300 ] && [ "$p" -le 11399 ]; then printf 'llamacpp-edge'
    elif [ "$p" -ge 11400 ] && [ "$p" -le 11499 ]; then printf 'ollama'
    elif [ "$p" -ge 11500 ] && [ "$p" -le 11599 ]; then printf 'support'
    elif [ "$p" -ge 11600 ] && [ "$p" -le 11699 ]; then printf 'comfyui'
    elif [ "$p" -ge 11700 ] && [ "$p" -le 11999 ]; then printf 'reserved'
    elif [ "$p" -ge 8000 ]  && [ "$p" -le 8099 ];  then printf 'vllm'
    elif [ "$p" -ge 8100 ]  && [ "$p" -le 8149 ];  then printf 'colibri'
    elif [ "$p" -ge 8200 ]  && [ "$p" -le 8249 ];  then printf 'llamacpp-edge'
    else printf '?'
    fi
}

# monitor_pid_in_tree <pid> <ancestor_pid> [max_hops]
# True if PID is ANCESTOR_PID itself, or a descendant of it anywhere in the process tree --
# not just a DIRECT child. Real bug this fixes, confirmed live 2026-09-18 against a real TP=2
# Qwen2.5-VL-72B launch: vLLM's TP>1 launches fork wrapper -> EngineCore -> Worker_TP0/TP1/...,
# so a worker's own PPID is EngineCore, never the wrapper. Every PID-attribution loop in this
# codebase used to check only the direct PPID against the wrapper -- correct for TP=1 (no
# separate EngineCore fork), silently wrong for TP>1 (every worker skipped, so a real,
# GPU-resident, fully loaded model showed 0 VRAM and no GPU anywhere on the dashboard). This
# walks the real ancestry instead of assuming a fixed depth.
# max_hops (default 5) bounds the walk -- the real hierarchy here is at most 2 hops deep
# (wrapper -> EngineCore -> worker), so 5 is generous headroom without risking an unbounded
# loop if `ps` ever returns something unexpected.
monitor_pid_in_tree() {
    local cur="$1" ancestor="$2" max_hops="${3:-5}" hops=0
    while [ -n "$cur" ] && [ "$cur" != "0" ] && [ "$hops" -le "$max_hops" ]; do
        [ "$cur" = "$ancestor" ] && return 0
        cur=$(ps -o ppid= -p "$cur" 2>/dev/null | tr -d ' ')
        hops=$((hops + 1))
    done
    return 1
}

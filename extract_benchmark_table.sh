#!/bin/bash

DIR="${1}"
TRACE_BASE="${2:-./traces}"

if [ -z "$DIR" ]; then
    echo "Usage: $0 <benchmark_results_directory> [traces_directory]"
    exit 1
fi

extract_metrics() {
    local file=$1
    local set_line=$(tr '\r' '\n' < "$file" | grep "^SET: " | tail -1)
    local get_line=$(tr '\r' '\n' < "$file" | grep "^GET: " | tail -1)
    local set_rps=$(echo "$set_line" | awk '{print $2}')
    local set_p50=$(echo "$set_line" | awk -F'p50=' '{print $2}' | awk '{print $1}')
    local get_rps=$(echo "$get_line" | awk '{print $2}')
    local get_p50=$(echo "$get_line" | awk -F'p50=' '{print $2}' | awk '{print $1}')
    echo "$set_rps $get_rps $set_p50 $get_p50"
}

pct_change() {
    local baseline=$1
    local value=$2
    if [ -z "$baseline" ] || [ -z "$value" ] || [ "$baseline" = "0" ]; then
        echo "N/A"
    else
        awk "BEGIN { printf \"%.1f%%\", (($value - $baseline) / $baseline) * 100 }"
    fi
}

trace_size() {
    local label=$1
    local trace_dir="$TRACE_BASE/$label"
    if [ -d "$trace_dir" ]; then
        du -sh "$trace_dir" | awk '{print $1}'
    else
        echo "-"
    fi
}

read set1 get1 setp1 getp1 <<< $(extract_metrics "$DIR/1_main_no_lttng.txt")
read set2 get2 setp2 getp2 <<< $(extract_metrics "$DIR/2_main_lttng_no_trace.txt")
read set3 get3 setp3 getp3 <<< $(extract_metrics "$DIR/3_main_lttng_exits.txt")
read set4 get4 setp4 getp4 <<< $(extract_metrics "$DIR/4_head_no_lttng.txt")
read set5 get5 setp5 getp5 <<< $(extract_metrics "$DIR/5_head_lttng_no_trace.txt")
read set6 get6 setp6 getp6 <<< $(extract_metrics "$DIR/6_head_lttng_exits.txt")
read set7 get7 setp7 getp7 <<< $(extract_metrics "$DIR/7_head_lttng_exits_no_vtid.txt")
read set8 get8 setp8 getp8 <<< $(extract_metrics "$DIR/8_head_lttng_all.txt")
read set9 get9 setp9 getp9 <<< $(extract_metrics "$DIR/9_head_lttng_all_no_vtid.txt")

printf "| Configuration | SET (req/s) | GET (req/s) | SET p50 | GET p50 | SET %% chg | GET %% chg | Trace Size |\n"
printf "|---------------|-------------|-------------|---------|---------|-----------|----------|------------|\n"
printf "| Main - No LTTng | %s | %s | %s | %s | baseline | baseline | - |\n" "$set1" "$get1" "$setp1" "$getp1"
printf "| Main - LTTng No Trace | %s | %s | %s | %s | %s | %s | - |\n" "$set2" "$get2" "$setp2" "$getp2" "$(pct_change "$set1" "$set2")" "$(pct_change "$get1" "$get2")"
printf "| Main - LTTng Exits (no vtid) | %s | %s | %s | %s | %s | %s | %s |\n" "$set3" "$get3" "$setp3" "$getp3" "$(pct_change "$set1" "$set3")" "$(pct_change "$get1" "$get3")" "$(trace_size 3_main_lttng_exits)"
printf "| HEAD - No LTTng | %s | %s | %s | %s | %s | %s | - |\n" "$set4" "$get4" "$setp4" "$getp4" "$(pct_change "$set1" "$set4")" "$(pct_change "$get1" "$get4")"
printf "| HEAD - LTTng No Trace | %s | %s | %s | %s | %s | %s | - |\n" "$set5" "$get5" "$setp5" "$getp5" "$(pct_change "$set1" "$set5")" "$(pct_change "$get1" "$get5")"
printf "| HEAD - LTTng Exits | %s | %s | %s | %s | %s | %s | %s |\n" "$set6" "$get6" "$setp6" "$getp6" "$(pct_change "$set1" "$set6")" "$(pct_change "$get1" "$get6")" "$(trace_size 6_head_lttng_exits)"
printf "| HEAD - LTTng Exits (no vtid) | %s | %s | %s | %s | %s | %s | %s |\n" "$set7" "$get7" "$setp7" "$getp7" "$(pct_change "$set1" "$set7")" "$(pct_change "$get1" "$get7")" "$(trace_size 7_head_lttng_exits_no_vtid)"
printf "| HEAD - LTTng All | %s | %s | %s | %s | %s | %s | %s |\n" "$set8" "$get8" "$setp8" "$getp8" "$(pct_change "$set1" "$set8")" "$(pct_change "$get1" "$get8")" "$(trace_size 8_head_lttng_all)"
printf "| HEAD - LTTng All (no vtid) | %s | %s | %s | %s | %s | %s | %s |\n" "$set9" "$get9" "$setp9" "$getp9" "$(pct_change "$set1" "$set9")" "$(pct_change "$get1" "$get9")" "$(trace_size 9_head_lttng_all_no_vtid)"

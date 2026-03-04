#!/bin/bash

set -e

RESULTS_DIR="benchmark_results_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"

echo "Benchmark results will be saved to: $RESULTS_DIR"

SERVER_TASKSET="${SERVER_TASKSET:-}"
BENCH_TASKSET="${BENCH_TASKSET:-}"
LAST_BUILD=""

# Function to run benchmark
run_benchmark() {
    local git_ref=$1
    local use_lttng=$2
    local trace_mode=$3
    local label=$4
    local add_context=${5:-yes}
    local threads=${6:-4}
    
    local resolved_ref
    resolved_ref=$(git rev-parse "$git_ref")
    local build_key="${resolved_ref}_${use_lttng}"

    echo "=========================================="
    echo "Running: $label"
    echo "Git ref: $git_ref ($(echo $resolved_ref | head -c 10))"
    echo "USE_LTTNG: $use_lttng, Trace: $trace_mode, Threads: $threads"
    echo "Build: $build_key (cached: $LAST_BUILD)"
    echo "=========================================="

    if [ "$LAST_BUILD" != "$build_key" ]; then
        git checkout "$git_ref"
        make distclean
        if [ "$use_lttng" = "yes" ]; then
            make USE_LTTNG=yes -j4
        else
            make -j4
        fi
        LAST_BUILD="$build_key"
    fi
    
    # Start server
    $SERVER_TASKSET ./src/valkey-server --daemonize yes --port 6379 --save "" --appendonly no
    sleep 2
    
    # Verify server is running
    if ! ./src/valkey-cli ping > /dev/null 2>&1; then
        echo "ERROR: Server failed to start for $label"
        return 1
    fi
    
    # Warmup run for first benchmark only
    if [ "$label" = "1_main_no_lttng_t1" ]; then
        echo "Running warmup to fill caches..."
        $BENCH_TASKSET ./src/valkey-benchmark -t get,set -n 100000 --threads "$threads" -q > /dev/null
        echo "Warmup complete"
    fi
    
    # Setup tracing if needed
    if [ "$trace_mode" != "none" ]; then
        lttng destroy "$label" 2>/dev/null || true
        TRACE_DIR="$(pwd)/traces/${label}"
        mkdir -p "$(pwd)/traces"
        lttng create "$label" -o "$TRACE_DIR"

        if [ "$trace_mode" = "exits" ]; then
            lttng enable-event -u 'valkey_server:command_unblocking,valkey_server:while_blocked_cron,valkey_server:eventloop,valkey_server:eventloop_cron,valkey_server:module_acquire_gil,valkey_server:command,valkey_server:fast_command'
            lttng enable-event -u 'valkey_commands:command_call'
            lttng enable-event -u 'valkey_db:expire_del,valkey_db:active_defrag_cycle,valkey_db:eviction_del,valkey_db:eviction_lazyfree,valkey_db:eviction_cycle,valkey_db:expire_cycle,valkey_db:expire_cycle_keys,valkey_db:expire_cycle_fields'
            lttng enable-event -u 'valkey_aof:fork,valkey_aof:aof_write_pending_fsync,valkey_aof:aof_write_active_child,valkey_aof:aof_write_alone,valkey_aof:aof_write,valkey_aof:aof_fsync_always,valkey_aof:aof_fstat,valkey_aof:aof_rename,valkey_aof:aof_flush'
            lttng enable-event -u 'valkey_rdb:fork,valkey_rdb:rdb_unlink_temp_file'
            lttng enable-event -u 'valkey_cluster:cluster_config_open,valkey_cluster:cluster_config_write,valkey_cluster:cluster_config_fsync,valkey_cluster:cluster_config_rename,valkey_cluster:cluster_config_dir_fsync,valkey_cluster:cluster_config_close,valkey_cluster:cluster_config_unlink,valkey_cluster:fork'
        elif [ "$trace_mode" = "optimized" ]; then
            lttng enable-event -u 'valkey_server:command_unblocking,valkey_server:while_blocked_cron,valkey_server:eventloop,valkey_server:eventloop_cron,valkey_server:module_acquire_gil,valkey_server:fast_command'
            lttng enable-event -u 'valkey_commands:command_call'
            lttng enable-event -u 'valkey_db:expire_del,valkey_db:active_defrag_cycle,valkey_db:eviction_del,valkey_db:eviction_lazyfree,valkey_db:eviction_cycle,valkey_db:expire_cycle,valkey_db:expire_cycle_keys,valkey_db:expire_cycle_fields'
            lttng enable-event -u 'valkey_aof:fork,valkey_aof:aof_write_pending_fsync,valkey_aof:aof_write_active_child,valkey_aof:aof_write_alone,valkey_aof:aof_write,valkey_aof:aof_fsync_always,valkey_aof:aof_fstat,valkey_aof:aof_rename,valkey_aof:aof_flush'
            lttng enable-event -u 'valkey_rdb:fork,valkey_rdb:rdb_unlink_temp_file'
            lttng enable-event -u 'valkey_cluster:cluster_config_open,valkey_cluster:cluster_config_write,valkey_cluster:cluster_config_fsync,valkey_cluster:cluster_config_rename,valkey_cluster:cluster_config_dir_fsync,valkey_cluster:cluster_config_close,valkey_cluster:cluster_config_unlink,valkey_cluster:fork'
        elif [ "$trace_mode" = "all" ]; then
            lttng enable-event -u 'valkey_server:*'
            lttng enable-event -u 'valkey_commands:*'
            lttng enable-event -u 'valkey_db:*'
            lttng enable-event -u 'valkey_aof:*'
            lttng enable-event -u 'valkey_rdb:*'
            lttng enable-event -u 'valkey_cluster:*'
        fi

        if [ "$add_context" = "yes" ]; then
            lttng add-context -u -t vtid
        fi
        lttng start
    fi
    
    # Run benchmark
    $BENCH_TASKSET ./src/valkey-benchmark -t get,set -n 1000000 --threads "$threads" -q > "$RESULTS_DIR/${label}.txt"
    
    # Stop tracing if needed
    if [ "$trace_mode" != "none" ]; then
        lttng stop
        lttng destroy "$label"
    fi
    
    # Stop server
    ./src/valkey-cli shutdown nosave 2>/dev/null || true
    sleep 1
    
    echo "Results saved to: $RESULTS_DIR/${label}.txt"
    cat "$RESULTS_DIR/${label}.txt"
    echo ""
}

# Run all benchmarks - 1 thread
run_benchmark "base" "no" "none" "1_main_no_lttng_t1" "yes" 1
run_benchmark "base" "yes" "none" "2_main_lttng_no_trace_t1" "yes" 1
run_benchmark "base" "yes" "exits" "3_main_lttng_exits_t1" "no" 1
run_benchmark "test" "no" "none" "4_head_no_lttng_t1" "yes" 1
run_benchmark "test" "yes" "none" "5_head_lttng_no_trace_t1" "yes" 1
run_benchmark "test" "yes" "exits" "6_head_lttng_exits_t1" "yes" 1
run_benchmark "test" "yes" "exits" "7_head_lttng_exits_no_vtid_t1" "no" 1
run_benchmark "test" "yes" "all" "8_head_lttng_all_t1" "yes" 1
run_benchmark "test" "yes" "all" "9_head_lttng_all_no_vtid_t1" "no" 1
run_benchmark "test" "yes" "optimized" "10_head_lttng_optimized_t1" "yes" 1

# Run all benchmarks - 2 threads
run_benchmark "base" "no" "none" "1_main_no_lttng_t2" "yes" 2
run_benchmark "base" "yes" "none" "2_main_lttng_no_trace_t2" "yes" 2
run_benchmark "base" "yes" "exits" "3_main_lttng_exits_t2" "no" 2
run_benchmark "test" "no" "none" "4_head_no_lttng_t2" "yes" 2
run_benchmark "test" "yes" "none" "5_head_lttng_no_trace_t2" "yes" 2
run_benchmark "test" "yes" "exits" "6_head_lttng_exits_t2" "yes" 2
run_benchmark "test" "yes" "exits" "7_head_lttng_exits_no_vtid_t2" "no" 2
run_benchmark "test" "yes" "all" "8_head_lttng_all_t2" "yes" 2
run_benchmark "test" "yes" "all" "9_head_lttng_all_no_vtid_t2" "no" 2
run_benchmark "test" "yes" "optimized" "10_head_lttng_optimized_t2" "yes" 2

# Run all benchmarks - 4 threads
run_benchmark "base" "no" "none" "1_main_no_lttng_t4" "yes" 4
run_benchmark "base" "yes" "none" "2_main_lttng_no_trace_t4" "yes" 4
run_benchmark "base" "yes" "exits" "3_main_lttng_exits_t4" "no" 4
run_benchmark "test" "no" "none" "4_head_no_lttng_t4" "yes" 4
run_benchmark "test" "yes" "none" "5_head_lttng_no_trace_t4" "yes" 4
run_benchmark "test" "yes" "exits" "6_head_lttng_exits_t4" "yes" 4
run_benchmark "test" "yes" "exits" "7_head_lttng_exits_no_vtid_t4" "no" 4
run_benchmark "test" "yes" "all" "8_head_lttng_all_t4" "yes" 4
run_benchmark "test" "yes" "all" "9_head_lttng_all_no_vtid_t4" "no" 4
run_benchmark "test" "yes" "optimized" "10_head_lttng_optimized_t4" "yes" 4

echo "=========================================="
echo "All benchmarks complete!"
echo "Results directory: $RESULTS_DIR"
echo "=========================================="
echo ""

# Call extract_benchmark_table.sh to generate formatted table
bash extract_benchmark_table.sh "$RESULTS_DIR"

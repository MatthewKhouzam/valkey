#!/bin/bash

set -e

BENCH_CPU=3
SERVER_CPU=2
BENCH_GOVERNOR_PATH="/sys/devices/system/cpu/cpu${BENCH_CPU}/cpufreq/scaling_governor"
SERVER_GOVERNOR_PATH="/sys/devices/system/cpu/cpu${SERVER_CPU}/cpufreq/scaling_governor"

# Save original settings
ORIGINAL_ASPM=$(cat /sys/module/pcie_aspm/parameters/policy 2>/dev/null | grep -o '\[.*\]' | tr -d '[]' || echo "unknown")
ORIGINAL_TURBO=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo "unknown")
ORIGINAL_BENCH_GOVERNOR=$(cat "$BENCH_GOVERNOR_PATH" 2>/dev/null || echo "unknown")
ORIGINAL_SERVER_GOVERNOR=$(cat "$SERVER_GOVERNOR_PATH" 2>/dev/null || echo "unknown")

# Cleanup function
cleanup() {
    echo "Restoring original settings..."
    if [ "$ORIGINAL_ASPM" != "unknown" ]; then
        echo "$ORIGINAL_ASPM" | sudo tee /sys/module/pcie_aspm/parameters/policy > /dev/null
    fi
    if [ "$ORIGINAL_TURBO" != "unknown" ]; then
        echo "$ORIGINAL_TURBO" | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo > /dev/null
    fi
    if [ "$ORIGINAL_BENCH_GOVERNOR" != "unknown" ]; then
        echo "$ORIGINAL_BENCH_GOVERNOR" | sudo tee "$BENCH_GOVERNOR_PATH" > /dev/null
    fi
    if [ "$ORIGINAL_SERVER_GOVERNOR" != "unknown" ]; then
        echo "$ORIGINAL_SERVER_GOVERNOR" | sudo tee "$SERVER_GOVERNOR_PATH" > /dev/null
    fi
    echo "Cleanup complete"
}

trap cleanup EXIT

# Set performance mode
echo "Setting PCIe ASPM to performance mode..."
echo "performance" | sudo tee /sys/module/pcie_aspm/parameters/policy > /dev/null

echo "Disabling turbo boost..."
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo > /dev/null

echo "Setting CPU ${BENCH_CPU} governor to performance..."
echo "performance" | sudo tee "$BENCH_GOVERNOR_PATH" > /dev/null

echo "Setting CPU ${SERVER_CPU} governor to performance..."
echo "performance" | sudo tee "$SERVER_GOVERNOR_PATH" > /dev/null

# Run benchmark as the invoking user (not root), even if this script is run via sudo
echo "Running benchmark with server on CPU ${SERVER_CPU}, benchmark on CPU ${BENCH_CPU}..."
export SERVER_TASKSET="taskset -c $SERVER_CPU"
export BENCH_TASKSET="taskset -c $BENCH_CPU"
if [ -n "${SUDO_USER:-}" ]; then
    sudo -u "$SUDO_USER" --preserve-env=SERVER_TASKSET,BENCH_TASKSET bash benchmark_tracing.sh
else
    bash benchmark_tracing.sh
fi

#!/bin/bash
# Setup an isolated benchmarking environment on an EC2 instance.
#
# Usage:
#   1. First run:  sudo ./setup_isolated.sh setup
#      → Configures GRUB with isolcpus, reboots.
#   2. After reboot: sudo ./setup_isolated.sh verify
#      → Verifies isolation, moves IRQs, prints status.

set -euo pipefail

ISOLATED_CORES="2,3"
NON_ISOLATED="0-1"

setup_grub() {
    echo "=== Configuring GRUB ==="

    local grub_file="/etc/default/grub"
    local params="isolcpus=${ISOLATED_CORES} nohz_full=${ISOLATED_CORES} rcu_nocbs=${ISOLATED_CORES}"

    if grep -q "isolcpus" "$grub_file"; then
        echo "isolcpus already present in $grub_file — skipping."
    else
        # Append params to GRUB_CMDLINE_LINUX_DEFAULT
        sed -i "s/\(GRUB_CMDLINE_LINUX_DEFAULT=\"[^\"]*\)/\1 ${params}/" "$grub_file"
        echo "Added: ${params}"
    fi

    echo ""
    echo "=== Current GRUB config ==="
    grep "GRUB_CMDLINE_LINUX" "$grub_file"

    echo ""
    echo "=== Regenerating GRUB ==="
    if command -v grub2-mkconfig &>/dev/null; then
        grub2-mkconfig -o /boot/grub2/grub.cfg
    elif command -v update-grub &>/dev/null; then
        update-grub
    else
        echo "ERROR: No grub update command found."
        exit 1
    fi

    echo ""
    echo "=== GRUB configured. Rebooting in 5 seconds... ==="
    echo "After reboot, run: sudo $0 verify"
    sleep 5
    reboot
}

verify_isolation() {
    echo "=== Verifying CPU isolation ==="

    local isolated
    isolated=$(cat /sys/devices/system/cpu/isolated)
    if [ -z "$isolated" ]; then
        echo "FAIL: No isolated CPUs. Did you run 'setup' and reboot?"
        exit 1
    fi
    echo "Isolated CPUs: ${isolated}"

    echo ""
    echo "=== CPU sibling topology ==="
    for cpu in 2 3; do
        local siblings
        siblings=$(cat /sys/devices/system/cpu/cpu${cpu}/topology/thread_siblings_list)
        echo "  Core ${cpu} siblings: ${siblings}"
    done

    echo ""
    echo "=== Moving IRQs to cores ${NON_ISOLATED} ==="
    local moved=0
    for irq in /proc/irq/*/smp_affinity_list; do
        echo "$NON_ISOLATED" > "$irq" 2>/dev/null && ((moved++)) || true
    done
    echo "Moved ${moved} IRQ affinities"

    echo ""
    echo "=== Kernel command line ==="
    cat /proc/cmdline

    echo ""
    echo "=== Environment ready ==="
    echo "Pin matching thread to core 2, network thread to core 3."
    echo "Run: cd build && cmake .. && make -j\$(nproc) && ./server"
}

case "${1:-}" in
    setup)
        setup_grub
        ;;
    verify)
        verify_isolation
        ;;
    *)
        echo "Usage: sudo $0 {setup|verify}"
        echo ""
        echo "  setup   — Configure GRUB and reboot (run once)"
        echo "  verify  — Verify isolation and move IRQs (run after reboot)"
        exit 1
        ;;
esac

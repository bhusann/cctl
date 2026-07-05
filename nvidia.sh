#!/bin/bash
# nvidia.sh — Unload/load NVIDIA kernel modules in correct dependency order
#
# Unload: nvidia_drm → nvidia_modeset → nvidia_uvm → nvidia
# Load:   nvidia → nvidia_uvm → nvidia_modeset → nvidia_drm

set -e

Unload() {
    echo "Unloading NVIDIA modules..."
    modprobe -r nvidia_drm    && echo "  ✓ nvidia_drm"    || echo "  - nvidia_drm (not loaded)"
    modprobe -r nvidia_modeset && echo "  ✓ nvidia_modeset" || echo "  - nvidia_modeset (not loaded)"
    modprobe -r nvidia_uvm    && echo "  ✓ nvidia_uvm"    || echo "  - nvidia_uvm (not loaded)"
    modprobe -r nvidia        && echo "  ✓ nvidia"        || echo "  - nvidia (not loaded)"
    echo "Done."
}

Load() {
    echo "Loading NVIDIA modules..."
    modprobe nvidia           && echo "  ✓ nvidia"        || echo "  ✗ nvidia failed"
    modprobe nvidia_uvm       && echo "  ✓ nvidia_uvm"    || echo "  ✗ nvidia_uvm failed"
    modprobe nvidia_modeset   && echo "  ✓ nvidia_modeset" || echo "  ✗ nvidia_modeset failed"
    modprobe nvidia_drm       && echo "  ✓ nvidia_drm"    || echo "  ✗ nvidia_drm failed"
    echo "Done."
}

Status() {
    echo "NVIDIA module status:"
    for mod in nvidia nvidia_uvm nvidia_modeset nvidia_drm; do
        if lsmod | grep -q "^${mod} "; then
            echo "  ✓ $mod (loaded)"
        else
            echo "  - $mod (not loaded)"
        fi
    done
}

case "${1:-}" in
    unload|off)  Unload ;;
    load|on)     Load ;;
    status)      Status ;;
    restart)
        Unload
        Load
        ;;
    *)
        echo "Usage: $0 {load|unload|status|restart}"
        echo "  load/unload/on/off  — Load or unload NVIDIA modules"
        echo "  restart             — Unload then reload"
        echo "  status              — Show loaded state"
        exit 1
        ;;
esac

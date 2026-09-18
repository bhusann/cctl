#!/bin/bash
set -euo pipefail

# ─── Configuration ──────────────────────────────────────────────────────────
PACKAGE="tuxedo-drivers"
VERSION="1.0"
MODULES=("clevo_acpi" "tuxedo_keyboard" "tuxedo_io")
MODPROBE_FILE="/etc/modprobe.d/tuxedo_keyboard.conf"
MODPROBE_OPTIONS="options tuxedo_keyboard force_backlight_type=6"
SOURCE_TARGET="/usr/src/${PACKAGE}-${VERSION}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL="$(uname -r)"
KERNEL_BUILD="/lib/modules/${KERNEL}/build"

# ─── Colors ─────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
ok()   { echo -e "  ${GREEN}✓${NC} $1"; }
warn() { echo -e "  ${YELLOW}⚠${NC} $1"; }
fail() { echo -e "  ${RED}✗${NC} $1"; }
info() { echo -e "  ${CYAN}→${NC} $1"; }
header() { echo -e "\n${CYAN}══ $1 ══${NC}"; }

# ─── Package Manager ────────────────────────────────────────────────────────
detect_pkg_manager() {
    # Immutable distros (Bazzite, Silverblue, etc.) have dnf but DKMS
    # doesn't survive image updates — treat as unsupported.
    if command -v rpm-ostree &>/dev/null; then echo "immutable"; return; fi

    if command -v pacman &>/dev/null; then echo "pacman"
    elif command -v apt-get &>/dev/null; then echo "apt"
    elif command -v dnf &>/dev/null; then echo "dnf"
    elif command -v zypper &>/dev/null; then echo "zypper"
    elif command -v xbps-install &>/dev/null; then echo "xbps"
    elif command -v emerge &>/dev/null; then echo "emerge"
    elif command -v eopkg &>/dev/null; then echo "eopkg"
    else echo "unsupported"
    fi
}

get_headers_pkg() {
    case "$1" in
        pacman)
            # Derive from the package that owns the running kernel's modules
            local kpkg
            kpkg="$(pacman -Qoq "/lib/modules/${KERNEL}/" 2>/dev/null | head -1 || true)"
            echo "${kpkg:-linux}-headers"
            ;;
        apt)    echo "linux-headers-${KERNEL}" ;;
        dnf)    echo "kernel-devel" ;;
        zypper) echo "kernel-devel" ;;
        xbps)
            # Void: headers package matches kernel, e.g. linux6.6-headers
            local kpkg
            kpkg="$(xbps-query -o "/lib/modules/${KERNEL}/" 2>/dev/null | head -1 | awk '{print $2}' | sed 's/-[0-9]*$//' || true)"
            echo "${kpkg:-linux}-headers"
            ;;
        emerge) echo "sys-kernel/linux-headers" ;;
        eopkg)
            # Solus: linux-current-headers or linux-lts-headers
            if eopkg info linux-current &>/dev/null 2>&1; then
                echo "linux-current-headers"
            else
                echo "linux-lts-headers"
            fi
            ;;
        *)      echo "" ;;
    esac
}

install_packages() {
    local pm="$1"; shift
    case "$pm" in
        pacman) pacman -S --needed --noconfirm "$@" ;;
        apt)    apt-get install -y "$@" ;;
        dnf)    dnf install -y "$@" ;;
        zypper) zypper install -y "$@" ;;
        xbps)   xbps-install -Sy "$@" ;;
        emerge) emerge --ask "$@" ;;
        eopkg)  eopkg install -y "$@" ;;
    esac
}

# ─── Prerequisites Check ────────────────────────────────────────────────────
check_prereqs() {
    local need_dkms=0 need_headers=0
    command -v dkms &>/dev/null || need_dkms=1
    [ -d "$KERNEL_BUILD" ] || need_headers=1

    # Nothing missing — carry on
    if [ $need_dkms -eq 0 ] && [ $need_headers -eq 0 ]; then
        return 0
    fi

    # Show what's missing
    header "Missing Prerequisites"
    [ $need_dkms -eq 1 ] && fail "dkms not found"
    [ $need_headers -eq 1 ] && fail "Kernel headers not found at $KERNEL_BUILD"

    # Detect package manager
    local pm
    pm="$(detect_pkg_manager)"
    if [ "$pm" = "immutable" ]; then
        echo
        warn "Immutable OS detected (rpm-ostree / Bazzite / Silverblue)."
        fail "DKMS modules do not survive image updates on immutable systems."
        info "Modules must be layered into the OS image to persist across updates."
        info "Refer to your distro's documentation for building and layering kernel modules."
        info "You can also use an AI assistant (ChatGPT, Gemini, etc.) for step-by-step guidance."
        info "Driver source is located at: ${CYAN}${SCRIPT_DIR}${NC}"
        echo
        exit 1
    fi
    if [ "$pm" = "unsupported" ]; then
        echo
        warn "Could not detect a supported package manager for your distro."
        info "Driver source is located at: ${CYAN}${SCRIPT_DIR}${NC}"
        info "You need 'dkms' and kernel headers installed to build these modules."
        info "Refer to your distro's documentation for manual DKMS installation."
        info "You can also use an AI assistant (ChatGPT, Gemini, etc.) for step-by-step guidance."
        echo
        exit 1
    fi

    # Build package list
    local pkgs=()
    [ $need_dkms -eq 1 ] && pkgs+=("dkms")
    if [ $need_headers -eq 1 ]; then
        local hpkg
        hpkg="$(get_headers_pkg "$pm")"
        [ -n "$hpkg" ] && pkgs+=("$hpkg")
    fi

    echo
    info "Detected package manager: $pm"
    info "Will install: ${pkgs[*]}"
    echo -n "  Proceed? [Y/n] "
    read -r ans
    case "$ans" in
        [nN]|[nN][oO])
            echo -e "${RED}Cannot proceed without prerequisites.${NC}" >&2
            exit 1
            ;;
    esac

    install_packages "$pm" "${pkgs[@]}"
    ok "Prerequisites installed"
}

# ─── Fix missing autoconf.h (cachyos-headers bug workaround) ────────────────
fix_autoconf() {
    local autoconf="${KERNEL_BUILD}/include/generated/autoconf.h"
    local autoconf_src="${KERNEL_BUILD}/include/config/auto.conf"
    if [ -f "$autoconf" ] && [ -s "$autoconf" ]; then
        return 0  # already fine
    fi
    if [ ! -f "$autoconf_src" ]; then
        fail "Cannot generate autoconf.h: ${autoconf_src} not found"
        return 1
    fi
    warn "autoconf.h missing — generating from auto.conf"
    awk -F= '/^CONFIG_/ {
        if ($2 == "y") print "#define " $1 " 1"
        else if ($2 == "m") print "#define " $1 "_MODULE 1"
        else if ($2 == "")  print "#define " $1 ""
        else                print "#define " $1 " " $2
    }' "$autoconf_src" > "$autoconf"
    ok "autoconf.h generated (${KERNEL})"
}

# ─── Detection ──────────────────────────────────────────────────────────────
detect_state() {
    local state="absent"

    # Check DKMS
    local dkms_out
    dkms_out="$(dkms status "${PACKAGE}/${VERSION}" 2>/dev/null || true)"
    if echo "$dkms_out" | grep -q "installed"; then
        state="installed"
    elif echo "$dkms_out" | grep -q "added\|built"; then
        state="partial"
    elif echo "$dkms_out" | grep -q "broken" || [ -d "/var/lib/dkms/${PACKAGE}" ]; then
        state="broken"
    fi

    # Check modprobe config
    local modprobe_ok=0
    [ -f "$MODPROBE_FILE" ] && modprobe_ok=1

    echo "$state|$modprobe_ok"
}

print_status() {
    header "Status"
    IFS='|' read -r state modprobe < <(detect_state)

    case "$state" in
        installed) ok "DKMS: installed" ;;
        partial)   warn "DKMS: partially registered (not installed)" ;;
        broken)    warn "DKMS: broken/stale registration" ;;
        *)         fail "DKMS: not registered" ;;
    esac

    if [ "$modprobe" -eq 1 ]; then
        ok "modprobe config: present ($MODPROBE_FILE)"
    else
        fail "modprobe config: missing"
    fi

    # Per-module load state — this is exactly what "Modules loaded" counts
    local lsmod_out loaded=0 line
    lsmod_out="$(lsmod)"
    echo
    info "Module load state:"
    for m in "${MODULES[@]}"; do
        if echo "$lsmod_out" | grep -q "^${m}[[:space:]]"; then
            ok "$m: loaded"
            loaded=$((loaded + 1))
        else
            fail "$m: NOT loaded"
        fi
    done
    info "Modules loaded: ${loaded}/${#MODULES[@]}"

    # Show the raw lsmod lines the script greps for each module
    echo
    info "Raw lsmod input the script checks (runs: lsmod | grep '^<module> '):"
    for m in "${MODULES[@]}"; do
        line="$(echo "$lsmod_out" | grep "^${m}[[:space:]]")"
        if [ -n "$line" ]; then
            echo "    $line"
        else
            echo "    (no lsmod line for $m)"
        fi
    done
    echo
}

# ─── Install ────────────────────────────────────────────────────────────────
do_install() {
    header "Install"

    # 1. Source → /usr/src
    if [ -d "$SOURCE_TARGET" ]; then
        warn "Replacing existing source at ${SOURCE_TARGET}"
        rm -rf "$SOURCE_TARGET"
    fi
    cp -r "$SCRIPT_DIR" "$SOURCE_TARGET"
    ok "Source copied to ${SOURCE_TARGET}"

    # 2. DKMS add
    if dkms status "${PACKAGE}/${VERSION}" 2>/dev/null | grep -q "added\|installed"; then
        warn "DKMS already registered, skipping add"
    else
        dkms add "${PACKAGE}/${VERSION}"
        ok "DKMS registered"
    fi

    # 3. Fix autoconf.h if needed
    fix_autoconf || true

    # 4. DKMS build
    info "Building modules..."
    if ! dkms build "${PACKAGE}/${VERSION}" 2>/dev/null; then
        # Retry with autoconf fix if it failed
        fix_autoconf
        dkms build "${PACKAGE}/${VERSION}"
    fi
    ok "Build successful"

    # 5. DKMS install — force to overwrite any stale .ko files
    dkms install --force "${PACKAGE}/${VERSION}"
    ok "Install successful"

    # 6. Modprobe config
    echo "$MODPROBE_OPTIONS" > "$MODPROBE_FILE"
    ok "modprobe config written"

    # 7. Clean stale updates/src/ if it lingered
    local updates_src="/lib/modules/${KERNEL}/updates/src"
    if [ -d "$updates_src" ]; then
        rm -rf "$updates_src"
        ok "Cleaned stale updates/src/"
    fi

    # 8. depmod
    depmod -a
    ok "Module dependencies updated"

    # 9. Summary
    echo
    echo -e "  ${GREEN}── Installed ──${NC}"
    for m in "${MODULES[@]}"; do
        local f
        f="$(modinfo -F filename "$m" 2>/dev/null || echo "not found")"
        echo "    $m → $f"
    done
    echo

    # 10. Load kernel modules
    info "Loading kernel modules..."
    if modprobe -a "${MODULES[@]}"; then
        ok "Kernel modules loaded successfully"
    else
        warn "Could not auto-load modules (reboot may be required)"
    fi

    # 11. Verify loaded modules with lsmod
    echo
    info "Active driver modules in kernel:"
    lsmod | grep -E "clevo_acpi|tuxedo_keyboard|tuxedo_io" || true
    echo

    # 12. Initialize keyboard backlight to 100% brightness and Green color
    local kbd="/sys/class/leds/rgb:kbd_backlight"
    if [ -d "$kbd" ]; then
        echo 255 > "$kbd/brightness" 2>/dev/null || true
        if [ -f "$kbd/multi_intensity" ]; then
            echo "0 255 0" > "$kbd/multi_intensity" 2>/dev/null || true
        fi
        ok "Keyboard backlight initialized to 100% brightness (Green)"
    fi

    echo
    ok "Done."
}

# ─── Uninstall ──────────────────────────────────────────────────────────────
do_uninstall() {
    header "Uninstall"

    # 1. Unload active kernel modules first (before deleting module files or running depmod)
    # clevo_acpi and tuxedo_io depend on tuxedo_keyboard, so they must unload first
    local unloaded_any=0
    for m in "clevo_acpi" "tuxedo_io" "tuxedo_keyboard"; do
        if grep -q "^${m} " /proc/modules; then
            if rmmod "$m" 2>/dev/null || modprobe -r "$m" 2>/dev/null; then
                ok "Unloaded $m"
                unloaded_any=1
            else
                warn "Could not unload $m (module in use)"
            fi
        fi
    done
    [ $unloaded_any -eq 0 ] && info "No active modules were loaded"

    # 2. DKMS remove
    if dkms status "${PACKAGE}/${VERSION}" 2>/dev/null | grep -q "added\|built\|installed"; then
        dkms remove "${PACKAGE}/${VERSION}" --all 2>/dev/null || true
        ok "DKMS module removed"
    fi

    # Clean any leftover /var/lib/dkms directory (handles broken states)
    local dkms_lib="/var/lib/dkms/${PACKAGE}"
    if [ -d "$dkms_lib" ]; then
        rm -rf "$dkms_lib"
        ok "Cleaned DKMS state from ${dkms_lib}"
    fi

    # 3. Remove built .ko files from updates/ across all installed kernels
    local cleaned_any=0
    for udir in /lib/modules/*/updates; do
        if [ -d "$udir" ]; then
            for m in "${MODULES[@]}"; do
                find "$udir" -name "${m}.ko*" -delete 2>/dev/null || true
            done
            find "$udir" -type d -empty -delete 2>/dev/null || true
            cleaned_any=1
        fi
    done
    [ $cleaned_any -eq 1 ] && ok "Removed built modules from kernel update directories"

    # 4. Remove modprobe config
    if [ -f "$MODPROBE_FILE" ]; then
        rm -f "$MODPROBE_FILE"
        ok "Removed ${MODPROBE_FILE}"
    else
        warn "modprobe config not found, skipping"
    fi

    # 5. Remove source from /usr/src
    if [ -d "$SOURCE_TARGET" ]; then
        rm -rf "$SOURCE_TARGET"
        ok "Removed ${SOURCE_TARGET}"
    fi

    # 6. Also clean original_module backups
    local backup="/var/lib/dkms/${PACKAGE}/original_module"
    if [ -d "$backup" ]; then
        rm -rf "$backup"
        ok "Cleaned DKMS original module backups"
    fi

    # 7. depmod
    depmod -a
    ok "Module dependencies updated"

    echo
    ok "Uninstall complete."
}

# ─── Main ───────────────────────────────────────────────────────────────────
require_root() {
    if [ "$EUID" -ne 0 ]; then
        echo -e "${RED}Run with sudo.${NC}" >&2
        exit 1
    fi
}

main() {
    # Read-only commands: no root, no prereqs needed
    case "${1:-}" in
        --status|-s)
            print_status
            exit 0
            ;;
        --help|-h)
            echo "Usage: $0 [--install|--uninstall|--status|--help]"
            echo "  (no args)  Interactive mode — prompts install/uninstall"
            exit 0
            ;;
    esac

    # Everything below modifies the system — require root + prerequisites
    require_root
    check_prereqs

    # Handle flags
    case "${1:-}" in
        --install|-i)
            do_install
            exit 0
            ;;
        --uninstall|-u)
            do_uninstall
            exit 0
            ;;
    esac

    # Interactive mode
    IFS='|' read -r state modprobe < <(detect_state)
    print_status

    if [ "$state" = "installed" ] && [ "$modprobe" -eq 1 ]; then
        echo -n "tuxedo-drivers are installed. Uninstall? [y/N] "
        read -r ans
        case "$ans" in
            [yY]|[yY][eE][sS]) do_uninstall ;;
            *) echo "Aborted." ;;
        esac
    elif [ "$state" = "broken" ] || [ "$state" = "partial" ]; then
        echo -n "tuxedo-drivers are in a broken/partial state. Clean and uninstall? [Y/n] "
        read -r ans
        case "$ans" in
            [nN]|[nN][oO]) echo "Aborted." ;;
            *) do_uninstall ;;
        esac
    else
        echo -n "tuxedo-drivers are not installed. Install? [Y/n] "
        read -r ans
        case "$ans" in
            [nN]|[nN][oO]) echo "Aborted." ;;
            *) do_install ;;
        esac
    fi
}

main "$@"

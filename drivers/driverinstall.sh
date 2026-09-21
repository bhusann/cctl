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

# Modules to scan for conflicting installations
CHECK_MODULES=(
    "clevo_acpi"
    "tuxedo_io"
    "tuxedo_keyboard"
    "clevo_wmi"
    "uniwill_wmi"
    "uniwill_acpi"
    "tuxedo_compatibility_check"
)

# Strict module unloading dependency order:
# 1. Peripheral/leaf modules: clevo_acpi, clevo_wmi, uniwill_wmi, uniwill_acpi
# 2. tuxedo_io
# 3. tuxedo_keyboard (clevo_acpi and tuxedo_io depend on it)
# 4. tuxedo_compatibility_check (tuxedo_keyboard imports tuxedo_is_compatible from it; must unload dead last)
UNLOAD_ORDER=(
    "clevo_acpi"
    "clevo_wmi"
    "uniwill_wmi"
    "uniwill_acpi"
    "tuxedo_io"
    "tuxedo_keyboard"
    "tuxedo_compatibility_check"
)

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
    if [ $need_dkms -eq 1 ]; then fail "dkms not found"; fi
    if [ $need_headers -eq 1 ]; then fail "Kernel headers not found at $KERNEL_BUILD"; fi

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
    if [ $need_dkms -eq 1 ]; then pkgs+=("dkms"); fi
    if [ $need_headers -eq 1 ]; then
        local hpkg
        hpkg="$(get_headers_pkg "$pm")"
        if [ -n "$hpkg" ]; then pkgs+=("$hpkg"); fi
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

# ─── Fix missing autoconf.h (arch-headers workaround) ────────────────
fix_autoconf() {
    local autoconf="${KERNEL_BUILD}/include/generated/autoconf.h"
    local autoconf_dir="${KERNEL_BUILD}/include/generated"
    local config_dir="${KERNEL_BUILD}/include/config"
    local autoconf_dst="${config_dir}/auto.conf"
    local dotconfig="${KERNEL_BUILD}/.config"
    if [ -f "$autoconf" ] && [ -s "$autoconf" ]; then
        return 0  # already fine
    fi
    if [ ! -f "$dotconfig" ]; then
        fail "Cannot generate autoconf.h: ${dotconfig} not found"
        return 1
    fi
    warn "autoconf.h missing - seeding from .config"
    local arch
    for arch in arm arm64 loongarch mips powerpc riscv s390 sparc x86; do
        mkdir -p "${KERNEL_BUILD}/arch/${arch}/crypto"
        touch "${KERNEL_BUILD}/arch/${arch}/crypto/Kconfig"
    done
    mkdir -p "$autoconf_dir" "$config_dir"
    awk -F= '/^CONFIG_/ && !/^CONFIG_CC_VERSION_TEXT/ {
        if ($2 == "y")      print "#define " $1 " 1"
        else if ($2 == "m") print "#define " $1 "_MODULE 1"
        else if ($2 != "")  print "#define " $1 " " $2
    }' "$dotconfig" > "$autoconf"
    if [ ! -s "$autoconf" ]; then
        fail "Generated autoconf.h is empty — .config may be corrupt"
        return 1
    fi
    grep "^CONFIG_" "$dotconfig" | grep -v "^CONFIG_CC_VERSION_TEXT" \
        > "$autoconf_dst"
    touch "${autoconf_dst}.cmd"
    ok "autoconf.h seeded from .config (${KERNEL})"
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

# ─── Module & DKMS Conflict Management ─────────────────────────────────────
find_conflicting_dkms() {
    local pkgs=()
    local seen=()

    # 1. Search /var/lib/dkms for module files
    for m in "${CHECK_MODULES[@]}"; do
        while IFS= read -r modfile; do
            [ -z "$modfile" ] && continue
            local p
            p="$(echo "$modfile" | awk -F'/' '{print $5 "/" $6}')"
            if [ -n "$p" ] && [[ ! " ${seen[*]:-} " =~ " ${p} " ]]; then
                seen+=("$p")
                pkgs+=("$p")
            fi
        done < <(find /var/lib/dkms -name "${m}.ko*" 2>/dev/null || true)
    done

    # 2. Search /usr/src/*/dkms.conf for BUILT_MODULE_NAME
    for conf in /usr/src/*/dkms.conf; do
        [ -f "$conf" ] || continue
        for m in "${CHECK_MODULES[@]}"; do
            if grep -q "BUILT_MODULE_NAME.*${m}" "$conf" 2>/dev/null; then
                local pname pver
                pname="$(grep -E '^[[:space:]]*PACKAGE_NAME=' "$conf" 2>/dev/null | head -1 | cut -d'=' -f2 | tr -d '"'\'' ' || true)"
                pver="$(grep -E '^[[:space:]]*PACKAGE_VERSION=' "$conf" 2>/dev/null | head -1 | cut -d'=' -f2 | tr -d '"'\'' ' || true)"
                if [ -n "$pname" ] && [ -n "$pver" ]; then
                    local p="${pname}/${pver}"
                    if [[ ! " ${seen[*]:-} " =~ " ${p} " ]]; then
                        seen+=("$p")
                        pkgs+=("$p")
                    fi
                fi
                break
            fi
        done
    done

    echo "${pkgs[*]:-}"
}

get_loaded_modules() {
    local loaded=()
    for m in "${CHECK_MODULES[@]}"; do
        if grep -q "^${m} " /proc/modules 2>/dev/null; then
            loaded+=("$m")
        fi
    done
    echo "${loaded[*]:-}"
}

unload_active_modules() {
    local unloaded_any=0
    for m in "${UNLOAD_ORDER[@]}"; do
        if grep -q "^${m} " /proc/modules 2>/dev/null; then
            if rmmod "$m" 2>/dev/null || modprobe -r "$m" 2>/dev/null; then
                ok "Unloaded $m"
                unloaded_any=1
            else
                sleep 0.2
                if rmmod "$m" 2>/dev/null || modprobe -r "$m" 2>/dev/null; then
                    ok "Unloaded $m"
                    unloaded_any=1
                else
                    warn "Could not unload $m (module in use)"
                fi
            fi
        fi
    done
    if [ $unloaded_any -eq 0 ]; then
        info "No active modules were loaded"
    fi
    return 0
}

clean_stale_module_files() {
    for udir in /lib/modules/*/updates; do
        if [ -d "$udir" ]; then
            for m in "${CHECK_MODULES[@]}"; do
                find "$udir" -name "${m}.ko*" -delete 2>/dev/null || true
            done
            find "$udir" -type d -empty -delete 2>/dev/null || true
        fi
    done
    depmod -a
    ok "Cleaned stale module files and updated depmod"
    return 0
}

remove_conflicting_dkms() {
    local pkgs=("$@")
    for pkg in "${pkgs[@]}"; do
        [ -z "$pkg" ] && continue
        local pname="${pkg%%/*}"
        local pver="${pkg##*/}"

        info "Removing DKMS package: ${pkg}..."
        dkms remove "${pkg}" --all 2>/dev/null || true
        ok "DKMS package removed: ${pkg}"

        local dkms_lib="/var/lib/dkms/${pname}"
        if [ -d "$dkms_lib" ]; then
            rm -rf "$dkms_lib"
            ok "Cleaned DKMS state from ${dkms_lib}"
        fi

        local src_dir="/usr/src/${pname}-${pver}"
        if [ "$src_dir" != "$SOURCE_TARGET" ] && [ -d "$src_dir" ]; then
            rm -rf "$src_dir"
            ok "Cleaned source directory ${src_dir}"
        fi
    done

    clean_stale_module_files
    return 0
}

# ─── Install ────────────────────────────────────────────────────────────────
do_install() {
    local auto_yes="${1:-0}"
    header "Install"

    # Check for existing DKMS packages and loaded modules
    local conflict_pkgs=()
    local dkms_found
    dkms_found="$(find_conflicting_dkms)"
    if [ -n "$dkms_found" ]; then
        read -ra conflict_pkgs <<< "$dkms_found"
    fi

    local loaded_mods=()
    local mods_found
    mods_found="$(get_loaded_modules)"
    if [ -n "$mods_found" ]; then
        read -ra loaded_mods <<< "$mods_found"
    fi

    if [ ${#conflict_pkgs[@]} -gt 0 ] || [ ${#loaded_mods[@]} -gt 0 ]; then
        echo
        info "Existing driver installation detected:"
        if [ ${#conflict_pkgs[@]} -gt 0 ]; then
            for p in "${conflict_pkgs[@]}"; do
                warn "  DKMS package: $p"
            done
        else
            info "  DKMS: none"
        fi

        if [ ${#loaded_mods[@]} -gt 0 ]; then
            warn "  Loaded kernel module(s): ${loaded_mods[*]}"
        else
            info "  Loaded kernel module(s): none"
        fi
        echo

        if [ "$auto_yes" -ne 1 ]; then
            echo -n "  Uninstall existing driver and install cctl drivers? [Y/n] "
            read -r ans
            case "$ans" in
                [nN]|[nN][oO])
                    echo -e "${YELLOW}Installation aborted by user.${NC}"
                    exit 0
                    ;;
            esac
        fi

        # 1. Unload active modules in strict dependency order:
        #    1. clevo_acpi, clevo_wmi, uniwill_wmi, uniwill_acpi
        #    2. tuxedo_io
        #    3. tuxedo_keyboard
        #    4. tuxedo_compatibility_check (depends on tuxedo_keyboard unloading first)
        info "Unloading active kernel modules..."
        unload_active_modules

        # 2. Remove existing DKMS package(s) and clean stale module files
        if [ ${#conflict_pkgs[@]} -gt 0 ]; then
            remove_conflicting_dkms "${conflict_pkgs[@]}"
        else
            clean_stale_module_files
        fi
    fi

    # 3. Source → /usr/src
    if [ -d "$SOURCE_TARGET" ]; then
        warn "Replacing existing source at ${SOURCE_TARGET}"
        rm -rf "$SOURCE_TARGET"
    fi
    cp -r "$SCRIPT_DIR" "$SOURCE_TARGET"
    ok "Source copied to ${SOURCE_TARGET}"

    # 4. DKMS add
    if dkms status "${PACKAGE}/${VERSION}" 2>/dev/null | grep -q "added\|installed"; then
        warn "DKMS already registered, skipping add"
    else
        dkms add "${PACKAGE}/${VERSION}"
        ok "DKMS registered"
    fi

    # 5. Fix autoconf.h if needed
    fix_autoconf || true

    # 6. DKMS build
    info "Building modules..."
    if ! dkms build "${PACKAGE}/${VERSION}" 2>/dev/null; then
        # Retry with autoconf fix if it failed
        fix_autoconf
        dkms build "${PACKAGE}/${VERSION}"
    fi
    ok "Build successful"

    # 7. DKMS install — force to overwrite any stale .ko files
    dkms install --force "${PACKAGE}/${VERSION}"
    ok "Install successful"

    # 8. Modprobe config
    echo "$MODPROBE_OPTIONS" > "$MODPROBE_FILE"
    ok "modprobe config written"

    # 9. Clean stale updates/src/ if it lingered
    local updates_src="/lib/modules/${KERNEL}/updates/src"
    if [ -d "$updates_src" ]; then
        rm -rf "$updates_src"
        ok "Cleaned stale updates/src/"
    fi

    # 10. depmod
    depmod -a
    ok "Module dependencies updated"

    # 11. Summary
    echo
    echo -e "  ${GREEN}── Installed ──${NC}"
    for m in "${MODULES[@]}"; do
        local f
        f="$(modinfo -F filename "$m" 2>/dev/null || echo "not found")"
        echo "    $m → $f"
    done
    echo

    # 12. Load kernel modules in forward dependency order:
    #     tuxedo_keyboard first, then clevo_acpi and tuxedo_io
    info "Loading kernel modules..."
    modprobe tuxedo_keyboard 2>/dev/null || true
    modprobe clevo_acpi 2>/dev/null || true
    modprobe tuxedo_io 2>/dev/null || true

    if grep -q "^clevo_acpi " /proc/modules && \
       grep -q "^tuxedo_keyboard " /proc/modules && \
       grep -q "^tuxedo_io " /proc/modules; then
        ok "Kernel modules loaded successfully"
    else
        warn "Could not auto-load all modules (reboot may be required)"
    fi

    # 13. Verify loaded modules with lsmod
    echo
    info "Active driver modules in kernel:"
    lsmod | grep -E "clevo_acpi|tuxedo_keyboard|tuxedo_io" || true
    echo

    # 14. Initialize keyboard backlight to 100% brightness and Green color
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

    # 1. Unload active kernel modules first in correct dependency order
    info "Unloading active kernel modules..."
    unload_active_modules

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
    clean_stale_module_files

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
    local auto_yes=0
    local action=""

    while [ $# -gt 0 ]; do
        case "$1" in
            --status|-s)
                print_status
                exit 0
                ;;
            --help|-h)
                echo "Usage: $0 [--install|--uninstall|--status|--help] [-y|--yes]"
                echo "  (no args)  Interactive mode — prompts install/uninstall"
                exit 0
                ;;
            --install|-i)
                action="install"
                ;;
            --uninstall|-u)
                action="uninstall"
                ;;
            -y|--yes)
                auto_yes=1
                ;;
        esac
        shift
    done

    # Everything below modifies the system — require root + prerequisites
    require_root
    check_prereqs

    case "$action" in
        install)
            do_install "$auto_yes"
            exit 0
            ;;
        uninstall)
            do_uninstall
            exit 0
            ;;
    esac

    # Interactive mode
    IFS='|' read -r state modprobe < <(detect_state)
    print_status

    if [ "$state" = "installed" ] && [ "$modprobe" -eq 1 ]; then
        echo -n "tuxedo-drivers are installed. Reinstall or Uninstall? [r/U/q] "
        read -r ans
        case "$ans" in
            [rR]) do_install 0 ;;
            [uU]|[yY]|[yY][eE][sS]) do_uninstall ;;
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
            *) do_install 0 ;;
        esac
    fi
}

main "$@"

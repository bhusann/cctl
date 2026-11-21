#!/usr/bin/env bash
# One-shot installer for cctl-gui: builds the binary, installs it to
# /usr/local/bin with a passwordless sudoers rule, and enables the boot
# service that serves the browser panel and restores your last session.
# Run from a clone of this repository: ./install.sh
set -euo pipefail
cd "$(dirname "$0")"

die(){ printf 'Error: %s\n' "$1" >&2; exit 1; }

command -v gcc >/dev/null 2>&1 || die "gcc not found — install a C compiler first."
pkg-config --exists dbus-1 2>/dev/null || die "dbus development files not found.
  Arch/CachyOS: sudo pacman -S dbus
  Debian/Ubuntu: sudo apt install libdbus-1-dev
  Fedora:        sudo dnf install dbus-devel"

echo "Building cctl (browser GUI + NVIDIA support)…"
make

echo "Installing cctl to /usr/local/bin (passwordless sudo rule)…"
./cctl install --noconfirm

if command -v systemctl >/dev/null 2>&1; then
  echo "Installing cctl-gui.service (panel at boot, restores last session)…"
  sudo cp systemd/cctl-gui.service /etc/systemd/system/cctl-gui.service
  sudo systemctl enable --now cctl-gui.service
else
  echo "No systemd detected — start the panel manually with: cctl gui"
fi

echo
echo "Done:"
echo "  cctl is on your PATH in any shell:  cctl status"
echo "  Browser panel:  http://127.0.0.1:8723/"
echo "  First-run driver setup (fans, keyboard, battery):  cctl drivers-manage"

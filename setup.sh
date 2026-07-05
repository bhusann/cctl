#!/bin/bash
# Auto-compile legacygpu.ko if kernel version changed or .ko missing.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULE="legacygpu"
KO="${MODULE}.ko"
MARKER="${SCRIPT_DIR}/.kernel_version"

CURRENT_KVER="$(uname -r)"
SAVED_KVER=""
[ -f "$MARKER" ] && SAVED_KVER="$(cat "$MARKER")"

if [ -f "${SCRIPT_DIR}/${KO}" ] && [ "$SAVED_KVER" = "$CURRENT_KVER" ]; then
    echo "legacygpu.ko up to date for $CURRENT_KVER"
    exit 0
fi

echo "Building ${MODULE}.ko for $CURRENT_KVER..."
make -C "$SCRIPT_DIR" clean 2>/dev/null
make -C "$SCRIPT_DIR"
if [ $? -eq 0 ]; then
    rm -f "${SCRIPT_DIR}"/*.o "${SCRIPT_DIR}"/*.cmd "${SCRIPT_DIR}"/*.mod* \
          "${SCRIPT_DIR}"/modules.order "${SCRIPT_DIR}"/Module.symvers \
          "${SCRIPT_DIR}"/.legacygpu.* "${SCRIPT_DIR}"/.module-common.* \
          "${SCRIPT_DIR}"/..module-common.* "${SCRIPT_DIR}"/.modules.order.* \
          "${SCRIPT_DIR}"/.Module.symvers.*
    echo "$CURRENT_KVER" > "$MARKER"
    echo "Done: ${SCRIPT_DIR}/${KO}"
else
    echo "Build failed."
    exit 1
fi

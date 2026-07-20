#!/bin/bash
# Auto-compile legacygpu.ko if kernel version changed or .ko missing.
# Lives in legacymethod/ — the Makefile is one level up.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PARENT_DIR="$(dirname "$SCRIPT_DIR")"
MODULE="legacygpu"
KO="${MODULE}.ko"
KO_PATH="${SCRIPT_DIR}/${KO}"
MARKER="${SCRIPT_DIR}/.kernel_version"

CURRENT_KVER="$(uname -r)"
SAVED_KVER=""
[ -f "$MARKER" ] && SAVED_KVER="$(cat "$MARKER")"

if [ -f "$KO_PATH" ] && [ "$SAVED_KVER" = "$CURRENT_KVER" ]; then
    echo "legacygpu.ko up to date for $CURRENT_KVER"
    exit 0
fi

echo "Building ${MODULE}.ko for $CURRENT_KVER..."
make -C "$PARENT_DIR" clean 2>/dev/null
make -C "$PARENT_DIR"
if [ $? -eq 0 ]; then
    cp "${SCRIPT_DIR}/${KO}" "$KO_PATH"
    rm -f "${SCRIPT_DIR}"/*.o "${SCRIPT_DIR}"/*.cmd \
          "${SCRIPT_DIR}"/*.mod* "${SCRIPT_DIR}"/modules.order \
          "${SCRIPT_DIR}"/Module.symvers "${SCRIPT_DIR}"/.legacygpu.* \
          "${SCRIPT_DIR}"/.module-common.* "${SCRIPT_DIR}"/..module-common.*
    echo "$CURRENT_KVER" > "$MARKER"
    echo "Done: $KO_PATH"
else
    echo "Build failed."
    exit 1
fi

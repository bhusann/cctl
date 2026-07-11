#!/bin/bash
# Auto-compile legacygpu.ko if kernel version changed or .ko missing.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULE_DIR="${SCRIPT_DIR}/legacymethod"
MODULE="legacygpu"
KO="${MODULE}.ko"
KO_PATH="${MODULE_DIR}/${KO}"
MARKER="${MODULE_DIR}/.kernel_version"

CURRENT_KVER="$(uname -r)"
SAVED_KVER=""
[ -f "$MARKER" ] && SAVED_KVER="$(cat "$MARKER")"

if [ -f "$KO_PATH" ] && [ "$SAVED_KVER" = "$CURRENT_KVER" ]; then
    echo "legacygpu.ko up to date for $CURRENT_KVER"
    exit 0
fi

echo "Building ${MODULE}.ko for $CURRENT_KVER..."
make -C "$SCRIPT_DIR" clean 2>/dev/null
make -C "$SCRIPT_DIR"
if [ $? -eq 0 ]; then
    cp "${SCRIPT_DIR}/legacymethod/${KO}" "$KO_PATH"
    rm -f "${SCRIPT_DIR}"/legacymethod/*.o "${SCRIPT_DIR}"/legacymethod/*.cmd \
          "${SCRIPT_DIR}"/legacymethod/*.mod* "${SCRIPT_DIR}"/legacymethod/modules.order \
          "${SCRIPT_DIR}"/legacymethod/Module.symvers "${SCRIPT_DIR}"/legacymethod/.legacygpu.* \
          "${SCRIPT_DIR}"/legacymethod/.module-common.* "${SCRIPT_DIR}"/legacymethod/..module-common.*
    echo "$CURRENT_KVER" > "$MARKER"
    echo "Done: $KO_PATH"
else
    echo "Build failed."
    exit 1
fi

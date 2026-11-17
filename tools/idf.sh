#!/bin/sh
# Default ESP-IDF for this project: v6.1.x.
#
# Override with IDF_EXPORT=/path/to/esp-idf/export.sh
# Example: IDF_EXPORT=$HOME/esp/esp-idf/export.sh ./tools/idf.sh build
set -e

if [ -n "$IDF_EXPORT" ]; then
    export_script="$IDF_EXPORT"
elif [ -f "$HOME/esp/esp-idf-6.1.1/export.sh" ]; then
    export_script="$HOME/esp/esp-idf-6.1.1/export.sh"
else
    export_script="$HOME/esp/esp-idf/export.sh"
fi

# ESP-IDF's virtualenv is tied to a Python version (3.10 here).  On systems
# where "python3" is older, give the export script a python3 -> python3.10
# shim instead of asking the user to fix PATH.
if command -v python3.10 >/dev/null 2>&1; then
    shim="${TMPDIR:-/tmp}/finder-idf-py310"
    mkdir -p "$shim"
    ln -sf "$(command -v python3.10)" "$shim/python3"
    PATH="$shim:$PATH"
    export PATH
fi

. "$export_script" >/dev/null 2>&1
exec idf.py "$@"

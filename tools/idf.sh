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

. "$export_script" >/dev/null 2>&1
exec idf.py "$@"

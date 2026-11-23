#!/bin/bash
# Build the firmware for one board.
# Usage: tools/build.sh <esp install dir> <path to this repo> <devkit|reva>
set -eo pipefail
ESP=$1; REPO=$2; BOARD=$3
. "$ESP/esp-idf/export.sh" >/dev/null
. "$ESP/esp-matter/export.sh" >/dev/null
python3 -m pip install -q -r "$ESP/esp-matter/requirements.txt"
cd "$REPO/firmware"
export IDF_TARGET=esp32c6
for d in build build-reva; do [ -f $d/CMakeCache.txt ] || rm -rf $d; done
if [ "$BOARD" = reva ]; then
  idf.py -B build-reva -D SDKCONFIG=build-reva/sdkconfig \
         -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.board.reva" build
else
  idf.py -B build -D SDKCONFIG=build/sdkconfig build
fi
echo BUILD_OK

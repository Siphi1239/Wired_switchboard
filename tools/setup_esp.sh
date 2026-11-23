#!/bin/bash
# Toolchain setup for the Wired Switchboard firmware (ESP-IDF v6.0.2, esp-matter release/v1.6.1)
# Usage: tools/setup_esp.sh <install dir, e.g. ~/esp> <path to this repo>
# Needs network access to github.com, dl.espressif.com, components(-file).espressif.com,
# chrome-infra-packages.appspot.com and storage.googleapis.com (Matter's CIPD tools).
set -euo pipefail
ESP=$1; REPO=$2
cd "$ESP"
export IDF_GITHUB_ASSETS="dl.espressif.com/github_assets"
if [ ! -d esp-idf ]; then
  git clone -q -b v6.0.2 --depth 1 --shallow-submodules --recursive https://github.com/espressif/esp-idf.git
fi
./esp-idf/install.sh esp32c6
if [ ! -d esp-matter ]; then
  git clone -q -b release/v1.6.1 --depth 1 https://github.com/espressif/esp-matter.git
  (cd esp-matter && git submodule update --init --depth 1)
fi
cd esp-matter/connectedhomeip/connectedhomeip
./scripts/checkout_submodules.py --platform esp32 linux --shallow
for p in "$REPO"/firmware/patches/*.patch; do
  if git apply --check "$p" 2>/dev/null; then git apply "$p"; else echo "already applied: $(basename "$p")"; fi
done
cd "$ESP/esp-matter"
set +u; . "$ESP/esp-idf/export.sh"; set -u
# Bootstrap Matter (gn, zap via CIPD). Its optional pip step for the host
# test environment can fail with a dependency conflict on Python 3.13; the
# firmware build does not need it, so continue and install esp-matter's
# own requirements into the IDF environment.
./install.sh --no-host-tool || echo "WARNING: esp-matter install.sh reported an error (see above)"
python3 -m pip install -q -r requirements.txt
echo SETUP_DONE

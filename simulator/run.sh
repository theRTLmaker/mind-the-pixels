#!/bin/sh
# Build the Mac simulator and render every scenario into simulator/out.
# Extra arguments are passed to mtp_sim, e.g. ./run.sh partly-cloudy
set -e
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build build
./build/mtp_sim --out out "$@"
if [ -z "$NO_OPEN" ] && command -v open >/dev/null; then
    open out/index.html
fi

#!/bin/bash
# Build a CMake/Ninja tree with distcc's mirrored-tree mode.
#
#   mirror-build.sh BUILD_DIR [ninja arguments...]
#
# 1. Builds the precompiled headers (and what they depend on) locally, since
#    the helpers compile against copies of them.
# 2. Copies the working tree to the ',mirror' hosts: distcc --mirror-sync.
# 3. Runs the build.
#
# On the main Mac, configure its remote Mac over the Thunderbolt 4 bridge:
#
#   distcc-mirror init --helper remote-mac --build build
#   eval "$(distcc-mirror env)"
#   mirror-build.sh build
#
# remote-mac is any SSH name/address for the remote role. init detects each
# Mac's cores and sets slot budgets. The final Ninja build uses their combined
# slots (distcc -j) unless an explicit -j argument is supplied.
#
# Set DISTCC=/path/to/distcc if the right distcc is not first on PATH.

set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 BUILD_DIR [ninja arguments...]" >&2
  exit 2
fi
build="$1"
shift
distcc="${DISTCC:-distcc}"

start=$(date +%s)
pch_targets=()
while IFS= read -r t; do
  pch_targets+=("$t")
done < <(ninja -C "$build" -t targets all | sed -n 's/^\(.*\.pch\): .*/\1/p')

if [[ ${#pch_targets[@]} -gt 0 ]]; then
  # distcc builds PCHs locally; what they depend on may be distributed.
  ninja -C "$build" "${pch_targets[@]}"
fi
"$distcc" --mirror-sync
synced=$(date +%s)
echo "mirror-build: PCHs and sync took $((synced - start))s" >&2

explicit_jobs=false
for arg in "$@"; do
  case "$arg" in -j|-j[0-9]*|--jobs|--jobs=*) explicit_jobs=true;; esac
done
if "$explicit_jobs"; then
  ninja -C "$build" "$@"
else
  ninja -C "$build" -j "$("$distcc" -j)" "$@"
fi

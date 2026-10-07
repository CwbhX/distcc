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
# Set up the environment first (see doc/mirrored-tree-design.md), e.g. for
# a build dir reached through a symlink:
#
#   export DISTCC_HOSTS="172.31.250.2:3634/14,mirror localhost/17"
#   export DISTCC_MIRROR_ROOTS="$HOME/Git/Ohmly"
#   export DISTCC_MIRROR_EXCLUDE=".git:build-release:output:tmp"
#   export DISTCC_MIRROR_PATHMAP="/Volumes/ExternalSSD/Developer/Ohmly/build-dev=$HOME/Git/Ohmly/build-dev"
#   export DISTCC_MIRROR_EXTRA="$HOME/Github/kicad-mac-builder/build/wxwidgets-dest"
#   export DISTCC_MIRROR_SSH=m6
#   export CCACHE_PREFIX=distcc        # or a wrapper that runs distcc
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

ninja -C "$build" "$@"

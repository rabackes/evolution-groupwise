#!/bin/bash
# Runs lintian over the .deb built by build.sh.
# Usage: containers/debian/build.sh && containers/debian/lint.sh

set -euo pipefail

repo_root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$repo_root"

image=evolution-groupwise-debian-build
out_dir="$repo_root/containers/debian/out"

if ! ls "$out_dir"/*.deb >/dev/null 2>&1; then
    echo "Kein .deb in $out_dir - erst build.sh laufen lassen." >&2
    exit 1
fi

podman build -f containers/debian/Containerfile -t "$image" .

podman run --rm \
    --userns=keep-id --security-opt label=disable \
    -v "$out_dir:/out:ro" \
    "$image" \
    bash -c 'lintian /out/*.deb /out/*.changes'

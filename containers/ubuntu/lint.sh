#!/bin/bash
# Runs lintian over the .deb built by build.sh.
# Usage: containers/ubuntu/build.sh && containers/ubuntu/lint.sh

set -euo pipefail

repo_root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$repo_root"

image=evolution-groupwise-ubuntu-build
out_dir="$repo_root/containers/ubuntu/out"

if ! ls "$out_dir"/*.deb >/dev/null 2>&1; then
    echo "Kein .deb in $out_dir - erst build.sh laufen lassen." >&2
    exit 1
fi

podman build -f containers/ubuntu/Containerfile -t "$image" .

podman run --rm \
    --userns=keep-id --security-opt label=disable \
    -v "$out_dir:/out:ro" \
    "$image" \
    bash -c 'lintian /out/*.deb /out/*.changes'

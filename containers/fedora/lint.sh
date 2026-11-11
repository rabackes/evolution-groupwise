#!/bin/bash
# Runs rpmlint (Fedora's own config) over the RPMs built by build.sh.
# Usage: containers/fedora/build.sh && containers/fedora/lint.sh

set -euo pipefail

repo_root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$repo_root"

image=evolution-groupwise-fedora-build
out_dir="$repo_root/containers/fedora/out"

if ! ls "$out_dir"/*.rpm >/dev/null 2>&1; then
    echo "Keine RPMs in $out_dir - erst build.sh laufen lassen." >&2
    exit 1
fi

podman build -f containers/fedora/Containerfile -t "$image" .

podman run --rm \
    --userns=keep-id --security-opt label=disable \
    -v "$out_dir:/out:ro" \
    "$image" \
    bash -c '
        shopt -s extglob
        rpmlint /out/!(*-debuginfo-*|*-debugsource-*).rpm
    '

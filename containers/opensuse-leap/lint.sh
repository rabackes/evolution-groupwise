#!/bin/bash
# Runs rpmlint (openSUSE's own config) over the real RPMs built by build.sh
# (main + lang + SRPM; -debuginfo/-debugsource are always "unclean" by their
# nature, see containers/opensuse-tumbleweed - skip them).
# Usage: containers/opensuse-leap/build.sh && containers/opensuse-leap/lint.sh

set -euo pipefail

repo_root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$repo_root"

image=evolution-groupwise-leap-build
out_dir="$repo_root/containers/opensuse-leap/out"

if ! ls "$out_dir"/*.rpm >/dev/null 2>&1; then
    echo "Keine RPMs in $out_dir - erst build.sh laufen lassen." >&2
    exit 1
fi

podman build -f containers/opensuse-leap/Containerfile -t "$image" .

podman run --rm \
    --userns=keep-id --security-opt label=disable \
    -v "$out_dir:/out:ro" \
    "$image" \
    bash -c '
        shopt -s extglob
        rpmlint /out/!(*-debuginfo-*|*-debugsource-*).rpm
    '

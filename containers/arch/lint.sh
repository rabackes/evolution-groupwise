#!/bin/bash
# Runs namcap (Arch's own lint tool) over the PKGBUILD and the real package
# built by build.sh (skips -debug, which is always noisy by nature - same
# convention as -debuginfo/-debugsource on the RPM side).
# Usage: containers/arch/build.sh && containers/arch/lint.sh

set -euo pipefail

repo_root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$repo_root"

image=evolution-groupwise-arch-build
out_dir="$repo_root/containers/arch/out"

if ! ls "$out_dir"/*.pkg.tar.* >/dev/null 2>&1; then
    echo "Kein Paket in $out_dir - erst build.sh laufen lassen." >&2
    exit 1
fi

podman build -f containers/arch/Containerfile -t "$image" .

podman run --rm \
    --userns=keep-id --security-opt label=disable \
    -v "$repo_root/packaging/arch:/pkgbuild:ro" \
    -v "$out_dir:/out:ro" \
    "$image" \
    bash -c '
        shopt -s extglob
        namcap /pkgbuild/PKGBUILD
        namcap /out/!(*-debug-*).pkg.tar.*
    '

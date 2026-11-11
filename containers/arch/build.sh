#!/bin/bash
# Builds evolution-groupwise for Arch Linux using the project's own, already
# validated packaging/arch/PKGBUILD (contributed via PR #2 by Felix Haun,
# 2026-11-10). Unlike the other distro containers here, this PKGBUILD is
# intentionally pinned to a specific, SHA-256-verified source commit, not the
# current working tree or a moving branch - see packaging/arch/README.md.
# This script just automates running makepkg for it inside a clean container;
# it does not change what gets built.
#
# makepkg refuses to run as root, which is exactly what --userns=keep-id
# without an explicit --user override already gives (the container's default
# process is the unprivileged host user).
#
# Usage: containers/arch/build.sh
# Output: the .pkg.tar.* lands in containers/arch/out/ (git-ignored).

set -euo pipefail

repo_root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$repo_root"

image=evolution-groupwise-arch-build
out_dir="$repo_root/containers/arch/out"
mkdir -p "$out_dir"

podman build -f containers/arch/Containerfile -t "$image" .

podman run --rm \
    --userns=keep-id --security-opt label=disable \
    -e HOME=/tmp/build \
    -v "$repo_root/packaging/arch/PKGBUILD:/pkgbuild/PKGBUILD:ro" \
    -v "$out_dir:/out" \
    "$image" \
    bash -c '
        set -euo pipefail
        mkdir -p "$HOME"
        workdir=$(mktemp -d)
        cp /pkgbuild/PKGBUILD "$workdir/"
        cd "$workdir"
        makepkg --noconfirm
        cp "$workdir"/*.pkg.tar.* /out/
    '

echo "Fertig. Pakete in $out_dir:"
ls -1 "$out_dir"

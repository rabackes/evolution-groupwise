#!/bin/bash
# Builds the evolution-groupwise .deb for Debian 13 (trixie) in a podman
# container, from the current working tree of this repo (committed or not;
# untracked files are not included since the source comes from "git archive").
#
# Usage: containers/debian/build.sh
# Output: .deb/.ddeb/.changes/.buildinfo land in containers/debian/out/ (git-ignored).

set -euo pipefail

repo_root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$repo_root"

image=evolution-groupwise-debian-build
out_dir="$repo_root/containers/debian/out"
mkdir -p "$out_dir"

podman build -f containers/debian/Containerfile -t "$image" .

podman run --rm \
    --userns=keep-id --security-opt label=disable \
    -e HOME=/tmp/build \
    -v "$repo_root:/build:ro" \
    -v "$out_dir:/out" \
    "$image" \
    bash -c '
        set -euo pipefail
        mkdir -p "$HOME"
        workdir=$(mktemp -d)
        mkdir "$workdir/src"
        git -C /build archive HEAD | tar -x -C "$workdir/src"
        cd "$workdir/src"
        dpkg-buildpackage -us -uc -b
        cp "$workdir"/*.deb "$workdir"/*.changes "$workdir"/*.buildinfo /out/
        cp "$workdir"/*.ddeb /out/ 2>/dev/null || true
    '

echo "Fertig. Pakete in $out_dir:"
ls -1 "$out_dir"

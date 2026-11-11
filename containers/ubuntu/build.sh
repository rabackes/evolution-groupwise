#!/bin/bash
# Builds the evolution-groupwise .deb for Ubuntu in a podman container, from
# the current working tree of this repo (committed or not; untracked files
# are not included since the source comes from "git archive").
#
# Uses the same debian/ packaging as the Debian 13 target; the changelog's
# distribution field ("trixie") is Debian-specific metadata and does not
# affect the build.
#
# Usage: containers/ubuntu/build.sh
# Output: .deb/.changes/.buildinfo land in containers/ubuntu/out/ (git-ignored).

set -euo pipefail

repo_root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$repo_root"

image=evolution-groupwise-ubuntu-build
out_dir="$repo_root/containers/ubuntu/out"
mkdir -p "$out_dir"

podman build -f containers/ubuntu/Containerfile -t "$image" .

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
        cp "$workdir"/*.deb "$workdir"/*.ddeb "$workdir"/*.changes "$workdir"/*.buildinfo /out/
    '

echo "Fertig. Pakete in $out_dir:"
ls -1 "$out_dir"

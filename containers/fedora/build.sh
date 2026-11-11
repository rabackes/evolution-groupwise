#!/bin/bash
# Builds the evolution-groupwise RPM for Fedora in a podman container, from the
# current working tree of this repo (committed or not; untracked files are not
# included since the source comes from "git archive").
#
# Usage: containers/fedora/build.sh
# Output: RPMs/SRPM land in containers/fedora/out/ (git-ignored).

set -euo pipefail

repo_root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
cd "$repo_root"

image=evolution-groupwise-fedora-build
out_dir="$repo_root/containers/fedora/out"
mkdir -p "$out_dir"

podman build -f containers/fedora/Containerfile -t "$image" .

podman run --rm \
    --userns=keep-id --security-opt label=disable \
    -e HOME=/tmp/build \
    -v "$repo_root:/build:ro" \
    -v "$out_dir:/out" \
    "$image" \
    bash -c '
        set -euo pipefail
        mkdir -p "$HOME"
        rpmdev-setuptree
        version=$(rpmspec -q --qf "%{version}\n" /build/packaging/rpm/evolution-groupwise.spec | head -1)
        name=evolution-groupwise
        cp /build/packaging/rpm/evolution-groupwise.spec ~/rpmbuild/SPECS/
        git -C /build archive --prefix="${name}-${version}/" -o "$HOME/rpmbuild/SOURCES/${name}-${version}.tar.gz" HEAD
        rpmbuild -ba ~/rpmbuild/SPECS/evolution-groupwise.spec
        cp ~/rpmbuild/RPMS/*/*.rpm ~/rpmbuild/SRPMS/*.rpm /out/
    '

echo "Fertig. Pakete in $out_dir:"
ls -1 "$out_dir"

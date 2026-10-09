# RPM packages (openSUSE, Fedora)

Needs evolution-data-server and Evolution 3.52 or later: openSUSE Tumbleweed, openSUSE Leap 16,
current Fedora. (openSUSE Leap 15 ships 3.42, which is too old.)

## Local build

```sh
# openSUSE: tools and build dependencies
sudo zypper install rpm-build rpmdevtools
sudo zypper source-install --build-deps-only evolution-groupwise   # if in a repository, or:
sudo zypper install cmake gcc gettext-tools python3 openssl evolution-data-server-devel \
    evolution-devel libsoup-devel libxml2-devel libical-glib-devel

# the source tarball, from the project directory
VERSION=0.3.0
git archive --format=tar.gz --prefix=evolution-groupwise-$VERSION/ -o ~/rpmbuild/SOURCES/evolution-groupwise-$VERSION.tar.gz HEAD
#   (without git: tar czf ~/rpmbuild/SOURCES/evolution-groupwise-$VERSION.tar.gz --transform "s,^\.,evolution-groupwise-$VERSION," --exclude=./build .)

rpmbuild -ba packaging/rpm/evolution-groupwise.spec
sudo zypper install ~/rpmbuild/RPMS/x86_64/evolution-groupwise-*.rpm
```

`rpmdev-setuptree` (Fedora) or `mkdir -p ~/rpmbuild/{SOURCES,SPECS,RPMS,SRPMS,BUILD}` creates the
build tree.

## Open Build Service

The spec file works in OBS unchanged; the tests (`%check`) start a mock POA on 127.0.0.1, which the
build sandbox allows.

## After installing

Remove a development install first (see the main README), then restart Evolution and the
evolution-data-server services (`evolution --force-shutdown`).

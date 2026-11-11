# Arch Linux

Install `base-devel`, then build as a regular user:

```sh
sudo pacman -S --needed base-devel
cd packaging/arch
makepkg --syncdeps
sudo pacman -U evolution-groupwise-*.pkg.tar.zst
```

Alternatively, `makepkg -si` builds, tests and installs interactively. The
PKGBUILD downloads a SHA-256-verified archive pinned to the shared compatibility
commit (currently v0.11.2); it does not build from your working tree or a moving branch.
Update `_commit`, `pkgver` and the checksum together for a newer source revision.

Evolution and evolution-data-server 3.52 or later are required. On Arch, these
packages also provide the development headers; there are no separate `-dev`
packages. CMake, gettext, pkgconf and Python are build dependencies; OpenSSL is
needed by the local mock POA's TLS tests. `check()` runs CTest without live accounts.
The default Evolution UI module is included. For a manual CMake build without
it, follow the main README with `-DWITH_EVOLUTION_UI=OFF`.

Installation uses `DESTDIR` and the module locations queried through pkg-config
(Camel providers, EDS address book/calendar backends, registry modules and Evolution
modules). Private libraries are installed below the CMake library directory in
`evolution-groupwise`, with an RPATH for the modules. No EDS ABI number is hardcoded.
Rebuild the package after an Evolution/EDS ABI upgrade.

Remove any previous private-prefix development install, then close Evolution and
run `evolution --force-shutdown` before starting it again. The account assistant
should offer **GroupWise**. Do not install a second copy of the modules through
`EDS_EXTRA_PREFIXES` at the same time.

Validation: official Arch container, Evolution/EDS 3.62.1 and libical 4.0.7;
Ubuntu 24.04 container, Evolution/EDS 3.52.3 and libical 3.0.17. Both UI settings
build and pass all six CTest entries. GUI account setup and a live POA are not
covered by these container tests.

`namcap` reports Evolution's private libraries as unresolved because they live in
`/usr/lib/evolution`, outside its default search path. They are supplied by the
required `evolution` package; staged module dependency checks resolve them with
that directory and the staged private library directory. The PKGBUILD has no
assigned Arch package maintainer yet (also reported by namcap).

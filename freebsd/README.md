# FreeBSD Ports draft

The local port draft is at `ports/devel/github-desktop-tui`. It packages the
immutable `v0.7.0` source tag and uses the MIT license included in that archive.
It is not part of the official FreeBSD Ports Collection yet. The release
archive excludes this overlay and Homebrew formula via `.gitattributes`, so
their checksums do not depend on packaging metadata.

From the repository root, register the overlay once, then validate the port
with the configured Poudriere tree and jail:

```sh
doas poudriere ports -c -p ghdesktop -m null \
  -M "$PWD/freebsd/ports" -f none
doas poudriere testport -j 15-amd64 -p default \
  -O ghdesktop -o devel/github-desktop-tui
```

The committed `distinfo` matches the GitHub `v0.7.0` source archive. To refresh
it after changing the source tag, run from the port directory:

```sh
make -C freebsd/ports/devel/github-desktop-tui \
  PORTSDIR=/usr/local/poudriere/ports/default DISTDIR=/tmp makesum
```

A Ports submission should use a released, immutable source tag, pass `portlint`
and `poudriere testport` on supported FreeBSD releases, and include the
maintainer's confirmed contact address. The project's CI performs clean CMake
builds and CTest runs on FreeBSD 14.5 and 15.1.

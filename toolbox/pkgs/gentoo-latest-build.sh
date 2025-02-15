#!/usr/bin/env bash
export LDFLAGS="-ltinfo -lncurses"

# configure xNVMe and build dependencies (fio, libvfn, and SPDK)
meson setup builddir --prefix=/usr

# build xNVMe
meson compile -C builddir

# install xNVMe
meson install -C builddir

# uninstall xNVMe
# cd builddir && meson --internal uninstall

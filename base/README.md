This directory contains by source files used by both binaries but where each
binary uses a version tuned by `#ifdef`.

The per-binary directories symlink symlink to `*.c` files. `*.h` files define
their respective `#ifdef` and then include `../base/*.h`. Includes in the
`*.c` files are resolved relative to the symlink, so they will include their
specialised `*.h`.

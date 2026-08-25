#!/bin/sh
# Regenerate the build system. Only needed from a git checkout; a release
# tarball ships a working ./configure.
set -e
cd "$(dirname "$0")"
mkdir -p build-aux m4
autoreconf --install --force --warnings=all "$@"
echo "now run: ./configure && make"

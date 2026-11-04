#!/bin/sh
# Compile transparent TCP redirection natively; no guest image or device.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/linpad-sockredirect-test.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CC:-cc}" -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -DGUEST_ARM64=1 -I"$root" -pthread \
    "$root/tests/unit/sockredirect-test.c" "$root/fs/sockredirect.c" -o "$build/sockredirect-test"
"$build/sockredirect-test"

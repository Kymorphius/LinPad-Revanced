#!/bin/sh
# Compile in-place listener repair with the production readiness code; no
# guest image or device.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/linpad-sockrestart-test.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
case $(uname -s) in
    Darwin) dead_strip=-Wl,-dead_strip ;;
    Linux) dead_strip=-Wl,--gc-sections ;;
    *) echo 'sockrestart-test requires Darwin kqueue or Linux epoll' >&2; exit 1 ;;
esac
"${CC:-cc}" -std=gnu11 -O2 -Wall -DGUEST_ARM64=1 -I"$root" \
    -ffunction-sections -fdata-sections -pthread "$dead_strip" \
    "$root/tests/unit/sockrestart-test.c" "$root/fs/sockrestart.c" \
    "$root/fs/poll.c" "$root/fs/real.c" -o "$build/sockrestart-test"
"$build/sockrestart-test"

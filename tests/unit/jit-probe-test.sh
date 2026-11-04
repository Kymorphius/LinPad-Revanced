#!/bin/sh
# Exercise jit/probe.h on an Apple Silicon Mac, without the app or a simulator.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/linpad-jit-probe.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CC:-cc}" -std=gnu11 -O1 -Wall -Wextra -Werror -I"$root" "$@" \
    "$root/tests/unit/jit-probe-test.c" -o "$build/jit-probe-test"
codesign -s - "$build/jit-probe-test" >/dev/null 2>&1 || true
status=0
"$build/jit-probe-test" || status=$?
# 2: this host cannot make the test region (the test says so); that is not a failure
[ "$status" -eq 2 ] && exit 0
exit "$status"

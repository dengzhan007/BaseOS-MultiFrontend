#!/bin/sh
set -eu
HERE="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I "$HERE/src" \
  "$HERE/tests/test-control-center-input.c" -o "$TMP/test-control-center-input"
"$TMP/test-control-center-input"

#!/bin/sh
set -eu

HERE="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT="$HERE/overlay/usr/sbin/boot-selector-held"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

# Either active-low shoulder key is sufficient.
printf '%s\n' ' gpio-10 (                    |GPIO Key L1          ) in  lo' > "$TMP/l1"
BASEOS_GPIO_DEBUG="$TMP/l1" sh "$SCRIPT"
printf '%s\n' ' gpio-12 (                    |GPIO Key R1          ) in  lo' > "$TMP/r1"
BASEOS_GPIO_DEBUG="$TMP/r1" sh "$SCRIPT"
printf '%s\n' \
  ' gpio-10 (                    |GPIO Key L1          ) in  lo' \
  ' gpio-12 (                    |GPIO Key R1          ) in  lo' > "$TMP/both"
BASEOS_GPIO_DEBUG="$TMP/both" sh "$SCRIPT"

# Released, absent, or unrelated GPIO lines must not trigger the selector.
printf '%s\n' ' gpio-10 (                    |GPIO Key L1          ) in  hi' > "$TMP/released"
if BASEOS_GPIO_DEBUG="$TMP/released" sh "$SCRIPT"; then
  echo "released L1 reported held" >&2; exit 1
fi
printf '%s\n' ' gpio-99 (                    |GPIO Key L1          ) in  lo' > "$TMP/wrong-gpio"
if BASEOS_GPIO_DEBUG="$TMP/wrong-gpio" sh "$SCRIPT"; then
  echo "wrong GPIO reported held" >&2; exit 1
fi
if BASEOS_GPIO_DEBUG="$TMP/missing" sh "$SCRIPT"; then
  echo "missing GPIO state reported held" >&2; exit 1
fi

echo "boot-selector-held tests passed"

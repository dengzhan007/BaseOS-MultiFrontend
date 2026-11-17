#!/bin/sh
# Bounded-wait boot selector tests.
#
# A counting `sleep` stub is placed first on PATH so every scenario is
# deterministic: no test depends on wall-clock timing or on the host's sleep
# supporting fractional seconds. The stub materialises a GPIO state file on a
# chosen poll count, which is how "file/line appears late" is simulated.
set -eu

HERE="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT="$HERE/overlay/usr/sbin/boot-selector-held"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

BIN="$TMP/bin"
mkdir -p "$BIN"
cat > "$BIN/sleep" <<'STUB'
#!/bin/sh
c=0
[ ! -f "$STUB_COUNT" ] || read -r c < "$STUB_COUNT"
c=$((c + 1))
printf '%s\n' "$c" > "$STUB_COUNT"
if [ -n "${STUB_APPEAR_AFTER:-}" ] && [ "$c" -ge "$STUB_APPEAR_AFTER" ] \
	&& [ -n "${STUB_CONTENT:-}" ] && [ -f "$STUB_CONTENT" ]; then
	cat "$STUB_CONTENT" > "$GPIO"
fi
exit 0
STUB
chmod +x "$BIN/sleep"

# Exact formatting as captured from the device's /sys/kernel/debug/gpio.
HEADER='gpiochip1: GPIOs 0-287, parent: platform/pio, pio:'
L1_HI=' gpio-10  (                    |GPIO Key L1         ) in  hi    '
L1_LO=' gpio-10  (                    |GPIO Key L1         ) in  lo    '
R1_HI=' gpio-12  (                    |GPIO Key R1         ) in  hi    '
R1_LO=' gpio-12  (                    |GPIO Key R1         ) in  lo    '
OTHER=' gpio-207 (                    |wlan_hostwake       ) in  lo    '

fail() { echo "FAIL: $*" >&2; exit 1; }

# run CASE TRIES [APPEAR_AFTER] [CONTENT_FILE]
# State files live under $TMP/gpio-<CASE> and $TMP/count-<CASE>.
run() {
	case_id=$1
	tries=$2
	appear_after=${3:-}
	content=${4:-}
	count="$TMP/count-$case_id"
	gpio="$TMP/gpio-$case_id"
	env PATH="$BIN:$PATH" \
		STUB_COUNT="$count" \
		STUB_APPEAR_AFTER="$appear_after" \
		STUB_CONTENT="$content" \
		GPIO="$gpio" \
		BASEOS_GPIO_DEBUG="$gpio" \
		BASEOS_GPIO_WAIT_TRIES="$tries" \
		BASEOS_GPIO_WAIT_INTERVAL=0.01 \
		sh "$SCRIPT"
}

expect_held() { run "$@" || fail "$1: selector was not requested"; echo "ok: $1 -> held"; }
expect_not_held() { if run "$@"; then fail "$1: selector requested unexpectedly"; fi; echo "ok: $1 -> not held"; }

# --- 1. L1 pressed (real device output format) -------------------------------
printf '%s\n' "$HEADER" "$L1_LO" "$R1_HI" > "$TMP/gpio-l1"
expect_held l1 20

# --- 2. R1 pressed -----------------------------------------------------------
printf '%s\n' "$HEADER" "$L1_HI" "$R1_LO" > "$TMP/gpio-r1"
expect_held r1 20

# --- 3. Both pressed ---------------------------------------------------------
printf '%s\n' "$HEADER" "$L1_LO" "$R1_LO" > "$TMP/gpio-both"
expect_held both 20

# --- 4. Neither pressed ------------------------------------------------------
printf '%s\n' "$HEADER" "$L1_HI" "$R1_HI" > "$TMP/gpio-released"
expect_not_held released 5

# --- 5. Wrong GPIO number / lookalike label ----------------------------------
printf '%s\n' "$HEADER" ' gpio-99  (                    |GPIO Key L1         ) in  lo' \
	' gpio-131 (                    |GPIO Key Menu       ) in  lo' > "$TMP/gpio-wrong"
expect_not_held wrong 5

# --- 6. Full real-device dump with an unrelated active-low line ---------------
printf '%s\n' "$HEADER" "$L1_HI" "$R1_HI" "$OTHER" > "$TMP/gpio-realmix"
expect_not_held realmix 5

# --- 7. GPIO file never exists ------------------------------------------------
expect_not_held nofile 5

# --- 8. GPIO file exists but L1/R1 lines never appear -------------------------
printf '%s\n' "$HEADER" "$OTHER" > "$TMP/gpio-nolines"
expect_not_held nolines 5

# --- 9. GPIO file appears late ------------------------------------------------
printf '%s\n' "$HEADER" "$L1_LO" > "$TMP/content-latefile"
expect_held latefile 20 3 "$TMP/content-latefile"

# --- 10. L1/R1 lines appear late ----------------------------------------------
printf '%s\n' "$HEADER" "$OTHER" > "$TMP/gpio-latelines"
printf '%s\n' "$HEADER" "$OTHER" "$R1_LO" > "$TMP/content-latelines"
expect_held latelines 20 3 "$TMP/content-latelines"

# --- 11. Timeout is bounded (no infinite wait) --------------------------------
# With no GPIO file and no content, the loop must stop after TRIES-1 sleeps and
# fail closed. The stub's counter proves the bound was honoured.
if run timeout 7; then fail "timeout: selector must not be requested"; fi
read -r sleeps < "$TMP/count-timeout"
[ "$sleeps" -eq 6 ] || fail "timeout: expected 6 bounded sleeps, got $sleeps"
echo "ok: timeout -> bounded after $sleeps sleeps"

# --- 12. First-pass success incurs no extra sleep -----------------------------
[ ! -s "$TMP/count-l1" ] || fail "L1 pressed should not sleep"
echo "ok: normal held path -> zero extra sleeps"

echo "boot-selector-held tests passed"

# Keep this test in the normal RG34XX validation sequence.

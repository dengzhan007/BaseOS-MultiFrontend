#!/bin/sh
# Exercise the real H700 frontend session and generic layout adapters.
# No hardware, privileged mounts or changes outside a disposable container.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
. "$HERE/tools/docker-platform.sh"

docker run --rm -i --platform "$BASEOS_DOCKER_PLATFORM_HOST" \
  -v "$HERE/overlay/usr/sbin/frontend-session":/tmp/frontend-session.src:ro \
  -v "$HERE/overlay/usr/sbin/baseos-frontend-compat":/tmp/baseos-frontend-compat.src:ro \
  -v "$HERE/overlay/usr/sbin/baseos-frontend-discovery":/tmp/baseos-frontend-discovery.src:ro \
  -v "$HERE/overlay/usr/share/baseos/boot-log.sh":/usr/share/baseos/boot-log.sh:ro \
  alpine:3.20 sh -eu <<'TEST'
mkdir -p /usr/local/bin /usr/sbin /mnt/sdcard /data /run /dev
export PATH=/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
cp /tmp/frontend-session.src /usr/sbin/frontend-session
cp /tmp/baseos-frontend-compat.src /usr/sbin/baseos-frontend-compat
cp /tmp/baseos-frontend-discovery.src /usr/sbin/baseos-frontend-discovery
chmod 755 /usr/sbin/frontend-session /usr/sbin/baseos-frontend-compat \
  /usr/sbin/baseos-frontend-discovery
ln -s /mnt/sdcard /mnt/SDCARD
mkdir -p /data/boot-pair-v2
: > /data/boot-pair-v2/complete
BOOT_LOG=/mnt/sdcard/baseos-boot.log
RAM_LOG=/tmp/baseos-boot.log
EXPANSION_LOG=/data/baseos-boot.log

cat > /usr/local/bin/mountpoint <<'EOF'
#!/bin/sh
[ ! -e /tmp/no-card ]
EOF
cat > /usr/local/bin/mount <<'EOF'
#!/bin/sh
exit 1
EOF
cat > /usr/local/bin/sleep <<'EOF'
#!/bin/sh
echo "$1" >> /tmp/sleep.log
if [ "$1" = 3600 ]; then
  kill -TERM "$PPID"
elif [ "$1" = 0.1 ] && [ -f /tmp/gpu-ready-after ]; then
  if [ "$(wc -l < /tmp/sleep.log)" -ge "$(cat /tmp/gpu-ready-after)" ]; then
    : > /dev/mali0
  fi
fi
exit 0
EOF
cat > /usr/sbin/baseos-update <<'EOF'
#!/bin/sh
echo "$*" >> /tmp/update.log
EOF
cat > /usr/bin/baseos-splash <<'EOF'
#!/bin/sh
echo "$*" >> /tmp/splash.log
EOF
cat > /lib/ld-linux-aarch64.so.1 <<'EOF'
#!/bin/sh
env > /tmp/frontend-env.log
[ "$#" -eq 1 ] || exit 90
case "$1" in
  /mnt/sdcard/System/application)
    [ "$(od -An -N4 -tx1 "$1" | tr -d ' \n')" = 7f454c46 ] || exit 91
    [ "$PWD" = /mnt/sdcard ] || exit 92
    [ "$BASEOS_CARD_ROOT" = /mnt/sdcard ] || exit 93
    [ "$SLOT_ROOT" = /mnt/sdcard ] || exit 94
    echo binary > /tmp/frontend.log
    ;;
  *) exit 95 ;;
esac
EOF
chmod 755 /usr/local/bin/* /usr/sbin/baseos-update /usr/bin/baseos-splash /lib/ld-linux-aarch64.so.1

cat > /mnt/sdcard/launch.sh <<'EOF'
#!/bin/sh
[ "$PWD" = /mnt/sdcard ] || exit 81
echo script > /tmp/frontend.log
echo frontend-stdout
echo frontend-stderr >&2
if [ -f /tmp/frontend-exit ]; then exit "$(cat /tmp/frontend-exit)"; fi
EOF
mkdir -p /mnt/sdcard/.system/h700/paks/sample.pak
cat > /mnt/sdcard/.system/h700/paks/sample.pak/launch.sh <<'EOF'
#!/bin/sh
[ "$PWD" = /mnt/sdcard/.system/h700/paks/sample.pak ] || exit 82
echo pak > /tmp/frontend.log
EOF
mkdir -p /mnt/sdcard/runtime/scripts
cat > /mnt/sdcard/runtime/scripts/runtime.sh <<'EOF'
#!/bin/sh
[ "$PWD" = /mnt/sdcard ] || exit 83
[ "$ROMS_PATH" = /mnt/sdcard/Roms/ ] || exit 84
echo runtime > /tmp/frontend.log
EOF
mkdir -p /mnt/sdcard/System
printf '\177ELF\002\001\001\000binary fixture\n' > /mnt/sdcard/System/application

fail() { echo "FAIL: $*" >&2; exit 1; }
reset_card() {
  rm -rf /mnt/sdcard
  mkdir -p /mnt/sdcard
  rm -f /tmp/*.log /tmp/no-card /tmp/gpu-ready-after /tmp/frontend-exit \
    "$EXPANSION_LOG" /run/boot-frontend-exec /run/usb-storage-device \
    /run/usb-storage-ready /run/usb-storage-failed
  : > /dev/mali0
}
run_session() {
  status=0
  timeout 5 sh -x /usr/sbin/frontend-session >/tmp/session-console.log 2>&1 || status=$?
  if [ "$status" -ne 0 ]; then
    echo "frontend-session exit=$status; shell trace follows:" >&2
    cat /tmp/session-console.log >&2
    echo "boot log follows:" >&2
    cat "$BOOT_LOG" >&2 || :
  fi
}
check_handoff() {
  [ -s /run/boot-frontend-exec ] || fail "missing first handoff marker"
  grep -Eq 'Startup: .+ s Linux to menu \(legacy-frontend(, after .+ s waiting for the GPU)?\)$' "$BOOT_LOG" \
    || fail "generic frontend name missing from boot timing log"
}

# A generic package-root script is found without any frontend name.
reset_card
: > "$BOOT_LOG"
run_session
[ "$status" -eq 0 ] || { cat /tmp/session-console.log >&2; fail "generic script launch returned $status"; }
[ "$(cat /tmp/frontend.log)" = script ] || fail "generic launch.sh was not selected"
[ "$(cat /tmp/update.log)" = confirm ] || fail "system update was not confirmed before handoff"
check_handoff
grep -q frontend-stdout /tmp/compat.log || fail "frontend stdout was not captured in its RAM log"
! grep -q frontend-stdout "$BOOT_LOG" || fail "frontend stdout leaked into the shared boot log"
[ ! -e "$EXPANSION_LOG" ] && [ ! -e "$RAM_LOG" ] || fail "boot log spool was not flushed"
echo "ok: generic root script launch and boot-log handoff"

# The layout adapter finds an arbitrary .pak/launch.sh and uses its directory.
reset_card
mkdir -p /mnt/sdcard/.system/h700/paks/sample.pak
cat > /mnt/sdcard/.system/h700/paks/sample.pak/launch.sh <<'EOF'
#!/bin/sh
[ "$PWD" = /mnt/sdcard/.system/h700/paks/sample.pak ] || exit 82
echo pak > /tmp/frontend.log
EOF
run_session
[ "$status" -eq 0 ] || { cat /tmp/session-console.log >&2; fail "pak script launch returned $status"; }
[ "$(cat /tmp/frontend.log)" = pak ] || fail "arbitrary pak entrypoint was not launched"
echo "ok: generic pak layout is selected by directory structure"

# A scripts/runtime.sh layout receives card-root paths and per-profile aliases.
reset_card
mkdir -p /mnt/sdcard/runtime/scripts
cat > /mnt/sdcard/runtime/scripts/runtime.sh <<'EOF'
#!/bin/sh
[ "$PWD" = /mnt/sdcard ] || exit 83
[ "$ROMS_PATH" = /mnt/sdcard/Roms/ ] || exit 84
echo runtime > /tmp/frontend.log
EOF
run_session
[ "$status" -eq 0 ] || { cat /tmp/session-console.log >&2; fail "runtime script launch returned $status"; }
[ "$(cat /tmp/frontend.log)" = runtime ] || fail "runtime layout was not launched"
echo "ok: generic runtime layout uses shared card-root paths"

# Any ELF payload under System/ is discovered without a product-specific name.
reset_card
mkdir -p /mnt/sdcard/System
printf '\177ELF\002\001\001\000binary fixture\n' > /mnt/sdcard/System/application
run_session
[ "$status" -eq 0 ] || { cat /tmp/session-console.log >&2; fail "generic System ELF returned $status"; }
[ "$(cat /tmp/frontend.log)" = binary ] || fail "generic System ELF layout was not launched"
echo "ok: arbitrary System ELF payload uses the generic binary adapter"

# GPU readiness remains bounded and does not depend on frontend naming.
reset_card
mkdir -p /mnt/sdcard/.system/h700/paks/sample.pak
cat > /mnt/sdcard/.system/h700/paks/sample.pak/launch.sh <<'EOF'
#!/bin/sh
echo pak > /tmp/frontend.log
EOF
rm -f /dev/mali0
echo 3 > /tmp/gpu-ready-after
run_session
[ "$status" -eq 0 ] || { cat /tmp/session-console.log >&2; fail "GPU delayed handoff returned $status"; }
[ "$(cat /tmp/frontend.log)" = pak ] || fail "pak failed after GPU readiness"
[ "$(grep -c '^0.1$' /tmp/sleep.log)" -eq 3 ] || fail "GPU wait was not bounded to the expected probe count"
echo "ok: GPU readiness wait works with the generic adapter"

# Respawn appends diagnostics but records the startup timing only once.
first_handoff=$(cat /run/boot-frontend-exec)
printf '42\n' > /tmp/frontend-exit
run_session
[ "$status" -eq 42 ] || fail "frontend exit status was not propagated"
[ "$(cat /run/boot-frontend-exec)" = "$first_handoff" ] || fail "respawn replaced the original handoff timestamp"
[ "$(grep -c ' Startup: ' "$BOOT_LOG")" -eq 1 ] || fail "respawn duplicated startup timing"
grep -q 'Frontend started again: legacy-frontend' "$BOOT_LOG" || fail "respawn diagnostic missing"
echo "ok: respawn preserves the first timing record and exit status"

# No frontend is a healthy BaseOS boot; the session remains alive for card insertion.
reset_card
: > /tmp/no-card
run_session
[ "$status" -eq 1 ] || fail "empty-card session should return the no-card idle status"
grep -q 'no card mounted' "$BOOT_LOG" || grep -q 'no frontend on card' "$BOOT_LOG" || fail "empty-card state was not logged"
echo "frontend session tests passed"
TEST

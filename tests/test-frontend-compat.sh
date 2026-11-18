#!/bin/sh
# Regression tests for automatic wrapping and safe removal of legacy frontend folders.
set -eu
HERE=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
DISCOVERY="$HERE/overlay/usr/sbin/baseos-frontend-discovery"
MANAGER="$HERE/overlay/usr/sbin/baseos-frontend-manager"
HELPER="$HERE/overlay/usr/sbin/baseos-frontend-compat"
export BASEOS_DISCOVERY_LIB="$DISCOVERY"
TMP=${TMPDIR:-/tmp}/baseos-compat-test-$$
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
TF1=$TMP/card
TF2=$TMP/tf2
CATALOG=$TMP/catalog
mkdir -p "$TF1/Frontends/NextUI" "$TF1/Frontends/NextUI-copy" "$TF1/Frontends/Next'UI" "$TF1/Frontends/街机" "$TF2/Frontends"
cat > "$TF1/Frontends/NextUI/launch.sh" <<'EOF'
#!/bin/sh
printf '%s\n' "$PWD" > "$COMPAT_TEST_RESULT"
printf '%s\n' "$BASEOS_COMPAT_PROGRAM_DIR" >> "$COMPAT_TEST_RESULT"
EOF
cat > "$TF1/Frontends/NextUI-copy/launch.sh" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$TF1/Frontends/Next'UI/launch.sh" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$TF1/Frontends/街机/launch.sh" <<'EOF'
#!/bin/sh
exit 0
EOF
printf '%s\n' 'frontend_id=auto' > "$TF1/baseos.conf"
cp "$HELPER" "$TMP/baseos-frontend-compat"
chmod 0755 "$TMP/baseos-frontend-compat"

fail() { echo "FAIL: $*" >&2; exit 1; }
compat_scan() {
  BASEOS_DISCOVERY_LIB="$DISCOVERY" BASEOS_COMPAT_TOOL="$TMP/baseos-frontend-compat" "$TMP/baseos-frontend-compat" scan "$TF2" "$TF1"
}
run_manager() {
  BASEOS_DISCOVERY_LIB="$DISCOVERY" \
  BASEOS_FRONTEND_TF1="$TF1" BASEOS_FRONTEND_TF2="$TF2" \
  BASEOS_FRONTEND_CATALOG="$CATALOG" \
  BASEOS_COMPAT_TOOL="$TMP/baseos-frontend-compat" \
  sh "$MANAGER" "$@"
}

compat_scan
STATE=$TF1/.baseos/compat.scan-state
[ -f "$STATE" ] || fail "compatibility scan state was not recorded"
state_before=$(stat -c '%y' "$STATE")
sleep 3
compat_scan
state_after=$(stat -c '%y' "$STATE")
[ "$state_before" = "$state_after" ] || fail "unchanged compatibility scan rewrote its state"
sleep 3
mkdir -p "$TF1/Frontends/LateFrontend"
printf '%s\n' '#!/bin/sh' 'exit 0' > "$TF1/Frontends/LateFrontend/launch.sh"
sleep 3
touch "$TF1/Frontends"
compat_scan
[ -f "$TF1/.baseos/compat/LateFrontend/baseos-frontend.conf" ] || fail "new frontend folder did not invalidate the compatibility scan cache"
SIDE=$TF1/.baseos/compat/NextUI
[ -f "$SIDE/baseos-frontend.conf" ] || fail "compatibility manifest was not generated"
[ -f "$SIDE/launch.sh" ] || fail "compatibility launcher was not generated"
[ -f "$SIDE/profiles/default.conf" ] || fail "default compatibility profile was not generated"
grep -qx 'compat_api=1' "$SIDE/baseos-frontend.conf" || fail "compatibility API marker missing"
[ ! -e "$TF1/Frontends/NextUI/baseos-frontend.conf" ] || fail "original frontend directory was modified"

# H700 native layouts are automatically wrapped outside the frontend payload.
mkdir -p "$TF1/Frontends/H700Slot/System" \
  "$TF1/Frontends/LEI-V4/System" \
  "$TF1/Frontends/LUCKLUCK-SLOT/System" \
  "$TF1/Frontends/UnknownELFs/System" \
  "$TF1/Frontends/H700NextUI/.system/h700/paks/MinUI.pak" \
  "$TF1/Frontends/H700Spruce/spruce/scripts"
printf '\177ELF\002\001\001\000compat-binary-fixture' > "$TF1/Frontends/H700Slot/System/slot"
# LEI and LUCK contain auxiliary ELF files before/alongside the real frontend.
printf '\177ELF\002\001\001\000opening-animation' > "$TF1/Frontends/LEI-V4/System/baseos-opening"
printf '\177ELF\002\001\001\000libretro-core' > "$TF1/Frontends/LUCKLUCK-SLOT/System/fbneo_libretro.so"
printf '\177ELF\002\001\001\000lei-slot-main' > "$TF1/Frontends/LEI-V4/System/slot"
printf '\177ELF\002\001\001\000luck-slot-main' > "$TF1/Frontends/LUCKLUCK-SLOT/System/slot"
printf '\177ELF\002\001\001\000unknown-helper' > "$TF1/Frontends/UnknownELFs/System/helper"
[ "$(od -An -N4 -tx1 "$TF1/Frontends/H700Slot/System/slot" | tr -d '[:space:]')" = 7f454c46 ] || fail "binary fixture does not contain an ELF magic header"
printf '%s\\n' '#!/bin/sh' 'exit 0' > "$TF1/Frontends/H700NextUI/.system/h700/paks/MinUI.pak/launch.sh"
printf '%s\\n' '#!/bin/sh' 'exit 0' > "$TF1/Frontends/H700Spruce/spruce/scripts/runtime.sh"
cp "$TF1/Frontends/H700Slot/System/slot" "$TMP/slot.before"
compat_scan
SLOT_SIDE=$TF1/.baseos/compat/H700Slot
NEXTUI_SIDE=$TF1/.baseos/compat/H700NextUI
SPRUCE_SIDE=$TF1/.baseos/compat/H700Spruce
[ -f "$SLOT_SIDE/baseos-frontend.conf" ] || fail "System/slot layout was not recognized"
[ -f "$NEXTUI_SIDE/baseos-frontend.conf" ] || fail "NextUI H700 pak layout was not recognized"
[ -f "$SPRUCE_SIDE/baseos-frontend.conf" ] || fail "spruce runtime layout was not recognized"
grep -qx 'compat_entrypoint=System/slot' "$SLOT_SIDE/baseos-frontend.conf" || fail "Slot entrypoint metadata is wrong"
grep -qx 'compat_mode=binary-card' "$SLOT_SIDE/baseos-frontend.conf" || fail "generic card-binary launch mode is wrong"
for id in LEI-V4 LUCKLUCK-SLOT; do
  [ -f "$TF1/.baseos/compat/$id/baseos-frontend.conf" ] || fail "$id was not recognized"
  grep -qx 'compat_entrypoint=System/slot' "$TF1/.baseos/compat/$id/baseos-frontend.conf" || fail "$id selected an auxiliary ELF instead of System/slot"
  grep -qx 'compat_mode=binary-card' "$TF1/.baseos/compat/$id/baseos-frontend.conf" || fail "$id did not use binary-card mode"
done
[ ! -e "$TF1/.baseos/compat/UnknownELFs/baseos-frontend.conf" ] || fail "unknown arbitrary ELF was incorrectly selected as a frontend"
[ "$("$TMP/baseos-frontend-compat" probe "$TF1/Frontends/LEI-V4")" = "System/slot|binary-card" ] || fail "LEI probe selected the wrong entrypoint"
[ "$("$TMP/baseos-frontend-compat" probe "$TF1/Frontends/LUCKLUCK-SLOT")" = "System/slot|binary-card" ] || fail "LUCK probe selected the wrong entrypoint"
if "$TMP/baseos-frontend-compat" probe "$TF1/Frontends/UnknownELFs" >/dev/null 2>&1; then
  fail "unknown arbitrary ELF directory unexpectedly passed probe"
fi
grep -qx 'compat_entrypoint=.system/h700/paks/MinUI.pak/launch.sh' "$NEXTUI_SIDE/baseos-frontend.conf" || fail "NextUI entrypoint metadata is wrong"
grep -qx 'compat_mode=script-pak' "$NEXTUI_SIDE/baseos-frontend.conf" || fail "pak script launch mode is wrong"
grep -qx 'compat_entrypoint=spruce/scripts/runtime.sh' "$SPRUCE_SIDE/baseos-frontend.conf" || fail "spruce entrypoint metadata is wrong"
grep -qx 'compat_mode=script-runtime' "$SPRUCE_SIDE/baseos-frontend.conf" || fail "runtime script launch mode is wrong"
grep -F -q 'FAVORITES_PATH="$BASEOS_FAVORITES_DIR/pyui-favorites.json"' "$HELPER" || fail "runtime adapter does not isolate favorites"
grep -F -q 'RECENTS_PATH="$BASEOS_RECENTS_DIR/pyui-recents.json"' "$HELPER" || fail "runtime adapter does not isolate recents"
grep -F -q 'APPS_CONFIG_PATH="$BASEOS_CONFIG_DIR/pyui-apps.json"' "$HELPER" || fail "runtime adapter does not isolate app configuration"
grep -F -q 'COLLECTIONS_PATH="$BASEOS_CONFIG_DIR/collections/"' "$HELPER" || fail "runtime adapter does not isolate collections"
sh -n "$SPRUCE_SIDE/launch.sh" || fail "spruce wrapper has invalid shell syntax"
printf '%s\n' '#!/bin/sh' 'printf "%s\\n" "$PWD" "$FAVORITES_PATH" "$RECENTS_PATH" "$APPS_CONFIG_PATH" "$COLLECTIONS_PATH" "$STATE_PATH" "$ROMS_PATH" > "$COMPAT_TEST_RESULT"' > "$TF1/Frontends/H700Spruce/spruce/scripts/runtime.sh"
BASEOS_CARD_ROOT=$TF1
BASEOS_FAVORITES_DIR=$TMP/private/favorites
BASEOS_RECENTS_DIR=$TMP/private/recents
BASEOS_CONFIG_DIR=$TMP/private/config
export BASEOS_CARD_ROOT BASEOS_FAVORITES_DIR BASEOS_RECENTS_DIR BASEOS_CONFIG_DIR
COMPAT_TEST_RESULT=$TMP/spruce-launch-result
export COMPAT_TEST_RESULT
/bin/sh "$SPRUCE_SIDE/launch.sh"
[ "$(sed -n '1p' "$COMPAT_TEST_RESULT")" = "$TF1" ] || fail "spruce runtime did not launch from the card root"
[ "$(sed -n '2p' "$COMPAT_TEST_RESULT")" = "$TMP/private/favorites/pyui-favorites.json" ] || fail "spruce favorites path was not isolated"
[ "$(sed -n '3p' "$COMPAT_TEST_RESULT")" = "$TMP/private/recents/pyui-recents.json" ] || fail "spruce recents path was not isolated"
[ "$(sed -n '4p' "$COMPAT_TEST_RESULT")" = "$TMP/private/config/pyui-apps.json" ] || fail "spruce apps path was not isolated"
[ "$(sed -n '5p' "$COMPAT_TEST_RESULT")" = "$TMP/private/config/collections/" ] || fail "spruce collections path was not isolated"
[ "$(sed -n '6p' "$COMPAT_TEST_RESULT")" = "$TMP/private/config/pyui-state.json" ] || fail "spruce state path was not isolated"
[ "$(sed -n '7p' "$COMPAT_TEST_RESULT")" = "$TF1/Roms/" ] || fail "spruce ROM path does not use the shared Games alias"
grep -F -q 'exec "$BASEOS_COMPAT_TOOL" launch' "$SLOT_SIDE/launch.sh" || fail "binary adapter does not delegate to the generic launcher"
cmp -s "$TMP/slot.before" "$TF1/Frontends/H700Slot/System/slot" || fail "compat scan modified the original Slot binary"
[ ! -e "$TF1/Frontends/H700Slot/baseos-frontend.conf" ] || fail "compat scan wrote metadata into original frontend folder"

COMPAT_TEST_RESULT=$TMP/launch-result
export COMPAT_TEST_RESULT
/bin/sh "$SIDE/launch.sh"
[ "$(sed -n '1p' "$COMPAT_TEST_RESULT")" = "$TF1/Frontends/NextUI" ] || fail "compat launcher did not preserve the original working directory"
[ "$(sed -n '2p' "$COMPAT_TEST_RESULT")" = "$TF1/Frontends/NextUI" ] || fail "compat program directory was not exported"

run_manager catalog
grep -F -q 'NextUI--default|NextUI|' "$CATALOG" || fail "legacy folder was not added to selector catalog"
grep -F -q 'NextUI-copy--default|NextUI-copy|' "$CATALOG" || fail "renamed full-copy folder was not discovered independently"
# IDs that are suffixes of another valid ID must not be dropped as duplicates.
grep -F -q 'H700NextUI--default|H700NextUI|' "$CATALOG" || fail "H700 NextUI profile missing"
grep -F -q 'NextUI--default|NextUI|' "$CATALOG" || fail "NextUI profile was hidden by prefix-like H700NextUI ID"
grep -F -q "Next-UI--default|Next'UI|" "$CATALOG" || fail "folder names containing quotes were not safely wrapped"
unicode_id="frontend-$(printf '%s' '街机' | cksum | awk '{print $1}')"
grep -F -q "$unicode_id--default|街机|" "$CATALOG" || fail "all-Unicode folder name was not assigned a stable ID"
. "$DISCOVERY"
baseos_frontend_scan_root "$TF1" NextUI || fail "legacy simple frontend_id pin was not resolved"
[ "$BASEOS_SCAN_ID" = "NextUI--default" ] || fail "legacy simple frontend_id did not resolve to its default profile"
[ "$BASEOS_SCAN_KIND" = "compat-package" ] || fail "simple frontend_id pin did not select the compatibility package"
[ "$(wc -l < "$CATALOG" | tr -d ' ')" = 10 ] || fail "expected eight independent compatibility entries"

# Y action disables the compatibility entry but preserves metadata and program
# files. Disabled entries stay listed and cannot be selected by boot discovery.
run_manager uninstall-compat NextUI--default
[ -d "$TF1/Frontends/NextUI" ] || fail "disabling compatibility deleted original program files"
[ -d "$SIDE" ] || fail "disabling compatibility deleted its BaseOS metadata"
[ -f "$TF1/.baseos/compat-disabled/NextUI" ] || fail "compatibility disable marker missing"
run_manager catalog
grep -F -q 'NextUI--default|' "$CATALOG" || fail "disabled compatibility entry disappeared from catalog"
grep -F -q '|compat|disabled' "$CATALOG" || fail "compatibility disabled status was not exposed to selector"
. "$DISCOVERY"
baseos_frontend_scan_root "$TF1" NextUI--default && fail "disabled compatibility frontend was still discoverable at boot"

# Re-enable through the same catalog entry. BaseOS reuses the saved metadata.
run_manager enable-compat NextUI--default
[ ! -e "$TF1/.baseos/compat-disabled/NextUI" ] || fail "re-enable left the compatibility disable marker"
run_manager catalog
grep -F -q '|compat|enabled' "$CATALOG" || fail "re-enabled compatibility status did not return to enabled"
baseos_frontend_scan_root "$TF1" NextUI--default || fail "re-enabled compatibility frontend was not discoverable"

# X action explicitly deletes the original program directory after confirmation,
# along with its BaseOS compatibility metadata, disable marker and path map.
mkdir -p "$TF1/.baseos/frontend-paths"
printf '%s\n' 'paths_api=1' 'frontend_id=NextUI' > "$TF1/.baseos/frontend-paths/NextUI.conf"
run_manager delete-compat NextUI--default
[ ! -e "$TF1/.baseos/frontend-paths/NextUI.conf" ] || fail "full compatibility deletion left its BaseOS path map"
[ ! -e "$TF1/Frontends/NextUI" ] || fail "full compatibility deletion left the original program directory"
[ ! -e "$TF1/.baseos/compat/NextUI" ] || fail "full compatibility deletion left sidecar metadata"
[ ! -e "$TF1/.baseos/compat-disabled/NextUI" ] || fail "full compatibility deletion left disable marker"
[ -d "$TF1/Frontends/NextUI-copy" ] || fail "deleting one copy removed another frontend"

# Card-root payload archives: NextUI-style frontends hard-code their runtime
# root to $SDCARD/.system/<platform>, so BaseOS expands the release archive at
# the card root on first launch and removes it again on confirmed deletion.
mkdir -p "$TF1/Frontends/ZipUI"
sleep 3
touch "$TF1/Frontends"
python3 - "$TF1/Frontends/ZipUI/MinUI.zip" "$TMP/pak-pwd" <<'PY'
import sys
import zipfile

path, result = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(path, "w") as archive:
    # Release archives carry every H700-family platform; the wrapper must pick
    # this device's branch, not whichever entry the archive happens to list first.
    archive.writestr(".system/tg5040/paks/MinUI.pak/launch.sh", "#!/bin/sh\nexit 0\n")
    archive.writestr(
        ".system/h700/paks/MinUI.pak/launch.sh",
        "#!/bin/sh\nprintf '%s\\n' \"$PWD\" > \"" + result + "\"\n",
    )
    archive.writestr(".system/h700/bin/nextui.elf", "stub")
    archive.writestr(".tmp_update/updater", "stub")
    archive.writestr("Roms/README.txt", "shared tree\n")
PY
export BASEOS_PLATFORM=h700
compat_scan
ZIPCONF=$TF1/.baseos/compat/ZipUI/baseos-frontend.conf
[ -f "$ZIPCONF" ] || fail "payload archive was not wrapped"
grep -qx 'compat_mode=zip-card' "$ZIPCONF" || fail "payload archive did not select zip-card mode"
grep -qx 'compat_entrypoint=MinUI.zip' "$ZIPCONF" || fail "payload archive entrypoint is not the archive"
grep -qx 'compat_entrypoint_stat=.*' "$ZIPCONF" || fail "payload archive stat was not recorded"

BASEOS_CARD_ROOT="$TF1" sh "$TF1/.baseos/compat/ZipUI/launch.sh"
[ -f "$TMP/pak-pwd" ] || fail "payload archive launcher did not run"
[ "$(cat "$TMP/pak-pwd")" = "$TF1/.system/h700/paks/MinUI.pak" ] ||
  fail "payload archive launcher ran outside the expanded package"
[ -f "$TF1/.baseos/payload/ZipUI.conf" ] || fail "payload install manifest was not recorded"
grep -qx 'path=.system' "$TF1/.baseos/payload/ZipUI.conf" || fail "payload manifest lost the runtime root"
if grep -qx 'path=Roms' "$TF1/.baseos/payload/ZipUI.conf"; then
  fail "payload manifest claimed a shared data tree"
fi

# An unchanged archive must not be expanded twice; the recorded payload is
# reused so a normal boot does not pay the unpack cost again.
rm -f "$TF1/.system/h700/bin/nextui.elf"
BASEOS_CARD_ROOT="$TF1" sh "$TF1/.baseos/compat/ZipUI/launch.sh"
if [ -f "$TF1/.system/h700/bin/nextui.elf" ]; then
  fail "second launch re-expanded an unchanged archive"
fi

# A payload shared by two frontends survives until the last one is removed,
# and uninstall never touches the shared data trees expanded beside it.
mkdir -p "$TF1/Frontends/ZipUI2"
cp "$TF1/Frontends/ZipUI/MinUI.zip" "$TF1/Frontends/ZipUI2/MinUI.zip"
sleep 3
touch "$TF1/Frontends"
compat_scan
run_manager catalog
BASEOS_CARD_ROOT="$TF1" sh "$TF1/.baseos/compat/ZipUI2/launch.sh"
run_manager delete-compat ZipUI--default
[ -d "$TF1/.system" ] || fail "uninstall removed a payload still claimed by another frontend"
[ ! -e "$TF1/Frontends/ZipUI" ] || fail "uninstall left the archive package directory"
run_manager delete-compat ZipUI2--default
[ ! -e "$TF1/.system" ] || fail "last uninstall left the expanded payload"
[ ! -e "$TF1/.tmp_update" ] || fail "last uninstall left the updater payload"
[ -f "$TF1/Roms/README.txt" ] || fail "uninstall removed the shared data tree"

echo "PASS: automatic legacy compatibility, independent full-copy installs, unregister and confirmed full deletion"

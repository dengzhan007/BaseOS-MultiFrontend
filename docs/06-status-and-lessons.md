# 06. Hardware validation and constraints

## 1. Validation coverage

Hardware validation is specific to each device. A successful image build or
QEMU smoke test does not prove hardware support on every target.

| Capability | Validated coverage | Remaining checks |
| --- | --- | --- |
| Boot, storage expansion and NextUI launch | RG40XX V | Per-target first-boot and upgrade checks |
| Display, input, audio and GLES | NextUI on RG40XX V | Per-target validation |
| Static boot logo and status pills | RG40XX V; rotated panel on RG28XX | Other panel profiles |
| Suspend-to-RAM | RG40XX V | Long-duration standby current |
| Wi-Fi | RG40XX V association; RG34XX SP network access | Per-target startup and reconnect checks |
| SSH/SFTP | RG40XX V | Per-target validation |
| USB adb | RG40XX V cable-connected boot; shell and file transfer | Per-target validation |
| USB mass storage | RG40XX V TF1 data partition | Whole TF2 with real cards |
| HDMI | RG40XX V hotplug in both directions | Per-target validation |
| Bluetooth audio | Daemon startup | Pairing and playback |
| A/B updates | RG40XX V: slot flip, slot B boot, `/data` persistence and trial confirmation. RG34XX: an updater restart into the written slot from a host-attached card, with trial confirmation | Hardware rollback after failed boots |
| GPT visibility attributes | RG40XX V boot | Windows drive-letter and format-prompt behavior |
| Charger-only boot and PMIC shutdown | RG SP; POWER-hold startup on RG34XX SP | Per-target charging and power behavior |
| Hostname and mDNS | RG34XX SP network validation | Per-target and host-network compatibility |
| Boot files without the initramfs, and their migration | RG SP: update from 1.3.0, migration on first boot, warm restarts, a cold power-on, an empty TF2 slot, sleep and wake. RG40XX V: update from 1.3.0, migration on first boot, warm restarts with Slot on TF2, sleep and wake. RG34XX SP: the same | The other eight targets |
| USB host controllers started by `h700_late_usb.ko` | RG SP: six root hubs present, adb over USB, sleep and wake, a USB audio adapter on the OTG port. RG40XX V and RG34XX SP: six root hubs present, adb over USB, sleep and wake | A device on one of the five controllers the module starts; the other eight targets |
| Fast native loader (1.5.x) | Users report the 1.5.1 fast start on RG SP, RG40XX V, RG28XX, RG34XX SP, RG34XX and RG35XX Plus. RG28XX (1 GiB LPDDR4): the 1.5.2 loader with the zero-tuned LPDDR4 fix, warm restarts, sleep and a cold power-on | RG40XX H and RG CubeXX with 1.5.2 (both fell back to stock on 1.5.1); RG35XX H, RG35XX Pro and RG35XX SP |
| `/usr/lib` served from a squashfs image | RG SP: update from the previous build through the updater, warm restarts, sleep and wake, all on-device checks. RG40XX V and RG34XX SP: update from 1.3.0, warm restarts, sleep and wake | The other eight targets; a normal power-off and power-on; a 32-bit program |
| Cloud-built upgrade artifact | RG34XX: the artifact from the GitHub Actions run written to the card over USB, the updater restart into the new slot, trial confirmation, and the frontend back in 8.3 s with the card left writable and no `FSCK` record | The other targets; artifacts from a run whose caches were cold |

## 2. Boot and build constraints

- Root ext4 needs a journal and the feature mask the vendor kernel supports.
  `/init` must be a regular executable script. See
  [boot chain](00-boot-chain-and-partitions.md).
- Boot-critical scripts must be executable. Tool source hashes must match the
  build stamp before packaging. See [image build](02-image-build-and-flash.md).
- The container steps in `build-rootfs.sh`, `build-image.sh` and
  `build-update.sh` are single-quoted arguments. A single quote inside one of
  those bodies ends the argument early: `sh` receives a truncated script and
  exits with `line N: syntax error: unexpected end of file`, naming one of the
  leftover words as the script. Use double quotes inside those bodies. See
  [cloud builds](02-image-build-and-flash.md#cloud-builds-github-actions).
- A payload built without `--native-release` carries no `native-generation`,
  so a native device rejects it during validation without ever showing a
  progress bar and keeps the file on the card. CI ships
  `manifest/native/rg34xx` and asserts the field before uploading. See
  [cloud builds](02-image-build-and-flash.md#cloud-builds-github-actions).
- `/dev/input` must not have a modification time of zero, or SDL without udev
  finds no gamepad. A kernel that registers its input devices within the first
  second leaves it at zero, so `rcS` sets it. See
  [boot files](12-kernel-gzip.md#a-side-effect-on-devinput).
- HDMI switching needs the `debugfs` display interface, which `rcS` mounts.
  See [rootfs and init](01-rootfs-and-init.md).
- Framebuffer draws must finish before frontend handoff. Panel rotation comes
  from the device profile. See [boot splash](04-boot-splash.md).
- Keep the vendor USB role selection, and check configfs attribute contents
  rather than their synthetic file sizes. See [USB access](08-usb-adb-and-otg.md).
- Growing a partition while a sibling is mounted requires `BLKPG`, which
  `gptgrow` uses. See [storage expansion](03-first-boot-and-expand.md).
- A restart that goes through the kernel never comes back on this hardware: the
  panel stays dark, the power LED goes out, and holding POWER does nothing
  until the reset button. `umount` and `remount` of the card block during
  shutdown, so `rcK` never returns and init never reaches `machine_restart`;
  once the kernel is inside its shutdown path, `sysrq-trigger` cannot recover
  it either. `baseos-reboot` now asks the PMIC for the restart, which is the
  reset the button performs and owes nothing to that path. Measured on RG34XX
  with a host attached: ten consecutive restarts, then an updater restart into
  a freshly written slot, all returning to the frontend untouched, with no new
  `FSCK` record on the card. See
  [the restart record](../experiments/h700-boot-speed/evidence/rg40xxv-plain-restart.txt).
- A double-escaped C literal quietly disabled the selector's default badge for
  a whole release: `strcspn(value, "\\r\\n")` compares against the two-character
  `\r`-`\n` text, not carriage return and newline, so `frontend_id` kept its
  trailing newline and never matched a catalog id. The badge code was present
  and unreachable, and `strings` on the binary showed every translated label,
  so only a framebuffer dump of the running page exposed it. When a C string
  constant is pasted out of shell code, check the backslash count.
- Reading key state with `EVIOCGKEY` is not reliable on the H700 nodes. The
  button-test page now rebuilds a key-down bitmap from the events it already
  reads, and accepts SELECT and START in either order when the second press
  lands within about a second, so the exit chord no longer depends on the
  ioctl or on both fingers arriving in the same poll.

## 3. Development validation

QEMU tests cover generic userspace. Use hardware validation for vendor-kernel
interfaces, drivers, display orientation, suspend and power handling. After
changing the harvest, check the runtime library closure on the target.

See [CONTRIBUTING.md](../CONTRIBUTING.md) for the build and test commands, and
the relevant subsystem document for its acceptance criteria.

## 4. Open work

- Make the rootfs read-only after initialization to reduce damage from power loss.
- Complete the remaining hardware checks above across supported H700 models.
- Define a `/data` schema migration before storing state that cannot be
  regenerated.

## 5. Frontend boundary

BaseOS owns hardware support, system services, storage, updates and the session
handoff. Frontends are installed separately on the card and are not BaseOS
build dependencies.

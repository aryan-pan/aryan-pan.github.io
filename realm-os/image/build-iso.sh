#!/usr/bin/env bash
# Builds a bootable RealmOS live ISO (BIOS and UEFI) on top of Ubuntu 24.04.
# Run as root on an Ubuntu 24.04 host:  sudo ./build-iso.sh
# Output: $OUT/realmos.iso (default ./out). Write it to a USB stick with
#   sudo dd if=out/realmos.iso of=/dev/sdX bs=4M status=progress
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
OUT="${OUT:-$HERE/out}"
SUITE=noble
MIRROR="${MIRROR:-http://archive.ubuntu.com/ubuntu}"
KERNEL="${KERNEL:-linux-image-generic}"
ROOT="$OUT/rootfs"
ISO="$OUT/iso"

[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
for tool in debootstrap mksquashfs grub-mkrescue xorriso meson ninja; do
	command -v "$tool" >/dev/null || { echo "missing $tool (apt install debootstrap squashfs-tools grub-pc-bin grub-efi-amd64-bin mtools xorriso meson)" >&2; exit 1; }
done

echo "==> building realmd"
meson setup "$REPO/realmd/build" "$REPO/realmd" --buildtype=release >/dev/null 2>&1 || true
ninja -C "$REPO/realmd/build"

mkdir -p "$OUT"
if [ ! -x "$ROOT/usr/bin/apt-get" ]; then
	echo "==> bootstrapping Ubuntu $SUITE"
	debootstrap --variant=minbase --components=main,universe "$SUITE" "$ROOT" "$MIRROR"
fi

cleanup() {
	for m in dev/pts dev proc sys run; do mountpoint -q "$ROOT/$m" && umount -l "$ROOT/$m"; done
	rm -f "$ROOT/usr/sbin/policy-rc.d"
}
trap cleanup EXIT
for m in dev dev/pts proc sys run; do mkdir -p "$ROOT/$m"; mount --bind "/$m" "$ROOT/$m"; done
printf '#!/bin/sh\nexit 101\n' > "$ROOT/usr/sbin/policy-rc.d"
chmod +x "$ROOT/usr/sbin/policy-rc.d"
cp /etc/resolv.conf "$ROOT/etc/resolv.conf" 2>/dev/null || true

cat > "$ROOT/etc/apt/sources.list" <<SRC
deb $MIRROR $SUITE main universe
deb $MIRROR $SUITE-updates main universe
deb $MIRROR $SUITE-security main universe
SRC

echo "==> installing packages"
chroot "$ROOT" /bin/bash -euo pipefail <<CHROOT
export DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
apt-get update -q
apt-get install -y -q --no-install-recommends \
	$KERNEL live-boot live-boot-initramfs-tools systemd-sysv systemd-resolved dbus libpam-systemd udev kmod \
	sudo locales ca-certificates network-manager iproute2 \
	libwlroots12t64 libcairo2 libxkbcommon0 xkb-data fonts-dejavu-core \
	libgl1-mesa-dri libegl1 libgles2 adwaita-icon-theme \
	foot thunar mousepad epiphany-browser
id player >/dev/null 2>&1 || useradd -m -s /bin/bash -G sudo,video,render,input,audio player
echo player:realm | chpasswd
echo realm > /etc/hostname
printf '127.0.0.1 localhost\n127.0.1.1 realm\n' > /etc/hosts
echo 'LANG=C.UTF-8' > /etc/default/locale
# An empty machine-id makes each installed machine generate its own, and with it its own character.
: > /etc/machine-id
apt-get clean
CHROOT

echo "==> installing RealmOS"
install -Dm755 "$REPO/realmd/build/realmd" "$ROOT/usr/local/bin/realmd"
cp -r "$HERE/overlay/." "$ROOT/"
chroot "$ROOT" systemctl enable NetworkManager systemd-resolved >/dev/null 2>&1 || true
chroot "$ROOT" update-initramfs -u -k all

echo "==> assembling ISO"
rm -rf "$ISO"
mkdir -p "$ISO/live" "$ISO/boot/grub"
cp "$(ls -1 "$ROOT"/boot/vmlinuz-* | sort -V | tail -1)" "$ISO/live/vmlinuz"
cp "$(ls -1 "$ROOT"/boot/initrd.img-* | sort -V | tail -1)" "$ISO/live/initrd.img"
cleanup
trap - EXIT
mksquashfs "$ROOT" "$ISO/live/filesystem.squashfs" -comp zstd -e boot -noappend -quiet
cat > "$ISO/boot/grub/grub.cfg" <<'CFG'
set timeout=3
set default=0
menuentry "RealmOS" {
	linux /live/vmlinuz boot=live quiet
	initrd /live/initrd.img
}
menuentry "RealmOS (verbose boot)" {
	linux /live/vmlinuz boot=live
	initrd /live/initrd.img
}
CFG
grub-mkrescue -o "$OUT/realmos.iso" "$ISO" -- -volid REALMOS
echo "==> $OUT/realmos.iso"
ls -lh "$OUT/realmos.iso"

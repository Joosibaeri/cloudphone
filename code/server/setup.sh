#!/bin/bash
set -e

BASE_DIR="/userdata/vm"
ROOTFS="$BASE_DIR/arch-rootfs"
IMG_URL="https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.gz"

mkdir -p "$BASE_DIR"

if [ ! -f "$BASE_DIR/bootstrap.tar.gz" ]; then
    curl -L "$IMG_URL" -o "$BASE_DIR/bootstrap.tar.gz"
fi

rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"

tar -xzf "$BASE_DIR/bootstrap.tar.gz" -C "$BASE_DIR"
mv "$BASE_DIR"/root.x86_64/* "$ROOTFS"
rm -rf "$BASE_DIR"/root.x86_64

echo "nameserver 1.1.1.1" > "$ROOTFS/etc/resolv.conf"

mount --bind /proc "$ROOTFS/proc"
mount --bind /sys "$ROOTFS/sys"
mount --bind /dev "$ROOTFS/dev"
mount --bind /dev/pts "$ROOTFS/dev/pts"

arch-chroot "$ROOTFS" pacman-key --init
arch-chroot "$ROOTFS" pacman-key --populate archlinux
arch-chroot "$ROOTFS" pacman -Sy --noconfirm base nano

echo "Arch RootFS bereit."

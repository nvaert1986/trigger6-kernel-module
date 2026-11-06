#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Build the Debian packages trigger6-dkms (kernel module source for DKMS) and
# trigger6-fast (helper, udev rule, systemd user service) from a source tree.
# Needs gcc and dpkg-deb; meant to run on Debian 13, for example:
#
#   podman run --rm -v "$PWD:/src:ro" -v "$PWD/dist:/out" docker.io/library/debian:13 \
#     sh -c 'apt-get update && apt-get install -y gcc dpkg-dev && sh /src/packaging/deb/build-deb.sh /src /out'
#
# Usage: build-deb.sh <source dir> <output dir> [debian revision]
set -eu

SRC=$(cd "$1" && pwd)
OUT=$2
REV=${3:-1}
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$SRC/dkms.conf")
# Debian version: a pre-release like 1.2.0-beta1 becomes 1.2.0~beta1, which
# sorts below the final 1.2.0. DKMS and /usr/src keep the module's own VER.
DEBVER=$(printf '%s' "$VER" | tr '-' '~')
ARCH=$(dpkg --print-architecture)
DATE=$(date -R -d "@${SOURCE_DATE_EPOCH:-$(date +%s)}")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

# copyright and changelog.Debian.gz for package $2 in tree $1
docs() {
	mkdir -p "$1/usr/share/doc/$2"
	{
		echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
		echo "Upstream-Name: trigger6"
		echo "Source: https://github.com/nvaert1986/trigger6-kernel-module"
		echo
		echo "Files: *"
		echo "Copyright: trigger6 contributors (see README.md, \"Origin, license and credits\")"
		echo "License: GPL-2.0-only"
		echo " On Debian systems, the full text is in /usr/share/common-licenses/GPL-2."
	} > "$1/usr/share/doc/$2/copyright"
	{
		echo "$2 ($DEBVER-$REV) unstable; urgency=medium"
		echo
		echo "  * Release $VER. The full changelog is in README.md of the source."
		echo
		echo " -- nvaert1986 <25772781+nvaert1986@users.noreply.github.com>  $DATE"
	} | gzip -9n > "$1/usr/share/doc/$2/changelog.Debian.gz"
}

# --- trigger6-dkms -----------------------------------------------------------
P=$WORK/dkms
D=$P/usr/src/trigger6-$VER
mkdir -p "$D/kernel" "$P/etc/modprobe.d" "$P/DEBIAN"
cp "$SRC"/kernel/Kbuild "$SRC"/kernel/Makefile "$SRC"/kernel/*.c "$SRC"/kernel/*.h "$D/kernel/"
rm -f "$D"/kernel/*.mod.c
cp "$SRC/dkms.conf" "$SRC/LICENSE" "$SRC/README.md" "$D/"
install -m 0644 "$SRC/packaging/debian13/trigger6.conf" "$P/etc/modprobe.d/trigger6.conf"
docs "$P" trigger6-dkms

cat > "$P/DEBIAN/control" <<EOF
Package: trigger6-dkms
Version: $DEBVER-$REV
Architecture: all
Maintainer: nvaert1986 <25772781+nvaert1986@users.noreply.github.com>
Depends: dkms (>= 3.0), kmod
Recommends: linux-headers-amd64 | linux-headers-generic, trigger6-fast
Section: kernel
Priority: optional
Homepage: https://github.com/nvaert1986/trigger6-kernel-module
Description: DRM/KMS driver for MCT Trigger 6 USB to HDMI adapters (DKMS)
 Out-of-tree kernel module for the MCT "T6 USB Station" chipset, as in the
 StarTech USB32HDES USB 3.0 to HDMI adapter (USB ID 0711:5601): display,
 hardware cursor, gamma table and HDMI audio. DKMS builds it for every
 installed kernel that has headers.
EOF
echo /etc/modprobe.d/trigger6.conf > "$P/DEBIAN/conffiles"

cat > "$P/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
if [ "\$1" = configure ]; then
	dkms add -m trigger6 -v $VER >/dev/null 2>&1 || true
	for k in /lib/modules/*; do
		[ -e "\$k/build" ] || continue
		dkms install -m trigger6 -v $VER -k "\${k##*/}" || \\
			echo "trigger6: DKMS build for \${k##*/} failed" >&2
	done
fi
exit 0
EOF
cat > "$P/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e
dkms remove -m trigger6 -v $VER --all >/dev/null 2>&1 || true
exit 0
EOF
chmod 0755 "$P/DEBIAN/postinst" "$P/DEBIAN/prerm"
dpkg-deb --root-owner-group --build "$P" "$OUT/trigger6-dkms_${DEBVER}-${REV}_all.deb"

# --- trigger6-fast -----------------------------------------------------------
P=$WORK/fast
mkdir -p "$P/usr/bin" "$P/usr/lib/udev/rules.d" "$P/usr/lib/systemd/user" "$P/DEBIAN"
gcc -O2 -Wall -Wextra -pthread -o "$P/usr/bin/trigger6-fast" "$SRC/userspace/trigger6-fast.c" -ldl
strip "$P/usr/bin/trigger6-fast"
install -m 0644 "$SRC/packaging/debian13/71-trigger6.rules" "$P/usr/lib/udev/rules.d/71-trigger6.rules"
install -m 0644 "$SRC/packaging/debian13/trigger6-fast.service" "$P/usr/lib/systemd/user/trigger6-fast.service"
docs "$P" trigger6-fast

cat > "$P/DEBIAN/control" <<EOF
Package: trigger6-fast
Version: $DEBVER-$REV
Architecture: $ARCH
Maintainer: nvaert1986 <25772781+nvaert1986@users.noreply.github.com>
Depends: libc6 (>= 2.34), libyuv0
Recommends: trigger6-dkms, systemd
Section: utils
Priority: optional
Homepage: https://github.com/nvaert1986/trigger6-kernel-module
Description: SIMD NV12 helper for the trigger6 USB display driver
 User-space helper that converts the desktop picture to NV12 with libyuv for
 the trigger6 driver (about 60 instead of 29 frames per second at 1920x1080).
 A udev rule gives the logged-in user access to the adapter and starts the
 helper as a systemd user service when the adapter appears.
EOF
cat > "$P/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ] && command -v udevadm >/dev/null 2>&1; then
	udevadm control --reload-rules >/dev/null 2>&1 || true
	udevadm trigger --subsystem-match=drm --subsystem-match=misc >/dev/null 2>&1 || true
fi
exit 0
EOF
chmod 0755 "$P/DEBIAN/postinst"
dpkg-deb --root-owner-group --build "$P" "$OUT/trigger6-fast_${DEBVER}-${REV}_${ARCH}.deb"

ls -l "$OUT"

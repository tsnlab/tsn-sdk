#!/bin/bash
# install.sh - Install or remove the tsn-xdma driver (xdma.ko) with DKMS.
#
# Usage (run as root):
#   sudo ./install.sh [--version <version>] [--kernel <kernel-release>]
#   sudo ./install.sh --uninstall
#   sudo ./install.sh --load-only /path/to/xdma.ko
#
#   --version <version>       DKMS version, usually a short Git commit ID (default: 1.0.0).
#   --kernel <kernel-release> Install only for this kernel; otherwise use all kernels with headers.
#   --uninstall               Remove all tsn-xdma versions and their configuration files.
#   --load-only <module>      Load a built module for this boot without changing DKMS or configuration.
#
# Installation steps:
#   1. Copy the driver source to /usr/src/tsn-xdma-<version>.
#   2. Remove other tsn-xdma versions from the target kernel.
#   3. Register, build, and install with DKMS. Check the build for SIMD register use.
#   4. Set /etc/modprobe.d/tsn-xdma.conf. Back up a different existing file in /var/backups.
#   5. Configure /etc/modules-load.d/tsn-xdma.conf to load xdma at boot.
#   6. Install a udev rule to apply the RPS mask to TSN receive queues.
#   7. Update the initramfs for the target kernel.
# The script can be run again with the same settings.
set -euo pipefail

PKG="tsn-xdma"
VERSION="1.0.0"
KERNEL=""
UNINSTALL=0
LOAD_ONLY=""

MODPROBE_CONF="/etc/modprobe.d/tsn-xdma.conf"
MODLOAD_CONF="/etc/modules-load.d/tsn-xdma.conf"
RPS_RULE="/etc/udev/rules.d/90-tsn-xdma-rps.rules"
RPS_HELPER="/usr/lib/udev/tsn-xdma-rps"
BACKUP_DIR="/var/backups"
DKMS_TREE="/var/lib/dkms"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRIVER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

log() { echo "[$PKG] $*"; }
die() {
	echo "[$PKG] Error: $*" >&2
	exit 1
}

usage() {
	sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-0}"
}

while [ $# -gt 0 ]; do
	case "$1" in
	--version)
		[ $# -ge 2 ] || die "--version requires a value."
		VERSION="$2"
		shift 2
		;;
	--kernel)
		[ $# -ge 2 ] || die "--kernel requires a value."
		KERNEL="$2"
		shift 2
		;;
	--load-only)
		[ $# -ge 2 ] || die "--load-only requires a built xdma.ko."
		LOAD_ONLY="$2"
		shift 2
		;;
	--uninstall)
		UNINSTALL=1
		shift
		;;
	-h | --help)
		usage 0
		;;
	*)
		echo "Unknown argument: $1" >&2
		usage 1
		;;
	esac
done

[ "$(id -u)" -eq 0 ] || die "Root privileges are required. Run with sudo."
if [ -n "$LOAD_ONLY" ]; then
	[ "$UNINSTALL" -eq 0 ] && [ -z "$KERNEL" ] && [ "$VERSION" = "1.0.0" ] ||
		die "--load-only cannot be combined with installation options."
	[ -f "$LOAD_ONLY" ] || die "Module file not found: $LOAD_ONLY"
	[ ! -d /sys/module/xdma ] || die "Unload the current xdma module before using --load-only."
	modinfo -p "$LOAD_ONLY" | grep -q '^tsn_mode:' ||
		die "This module does not expose the tsn_mode parameter."
	/sbin/insmod "$LOAD_ONLY" tsn_mode=1 rx_poll_mode=0 pmac_rx_enable=1
	log "Loaded the TSN driver for this boot."
	exit 0
fi
command -v dkms >/dev/null 2>&1 || die "DKMS is missing. Run 'apt install dkms'."

if ! printf '%s' "$VERSION" | grep -Eq '^[A-Za-z0-9][A-Za-z0-9._+-]*$'; then
	die "Invalid version format: $VERSION"
fi

SRC_DIR="/usr/src/${PKG}-${VERSION}"

# 파일 내용이 다르면 /var/backups 에 보관한다. 같으면 보관하지 않는다.
backup_if_differs() {
	local file="$1" new="$2"
	if [ -f "$file" ] && ! cmp -s "$file" "$new"; then
		mkdir -p "$BACKUP_DIR"
		local dst
		dst="$BACKUP_DIR/$(basename "$file").$(date +%Y%m%d-%H%M%S).bak"
		cp -p "$file" "$dst"
		log "Backed up $file to $dst."
	fi
}

# 내용이 다를 때만 파일을 바꾼다.
write_file() {
	local file="$1" content="$2" tmp
	tmp="$(mktemp)"
	printf '%s\n' "$content" >"$tmp"
	if [ -f "$file" ] && cmp -s "$file" "$tmp"; then
		rm -f "$tmp"
		log "$file is already up to date."
		return 0
	fi
	backup_if_differs "$file" "$tmp"
	mkdir -p "$(dirname "$file")"
	install -m 0644 "$tmp" "$file"
	rm -f "$tmp"
	log "Wrote $file."
}

reload_udev_rules() {
	if [ -S /run/udev/control ] && command -v udevadm >/dev/null 2>&1; then
		udevadm control --reload-rules
	fi
}

update_initramfs() {
	local k="$1"
	if command -v update-initramfs >/dev/null 2>&1; then
		log "Updating initramfs for $k"
		update-initramfs -u -k "$k"
	else
		log "update-initramfs is unavailable. Skipping the initramfs update for $k."
	fi
}

# DKMS 트리에 있는 tsn-xdma 버전 목록. kernel-* 링크는 버전이 아니다.
installed_versions() {
	local d name
	[ -d "$DKMS_TREE/$PKG" ] || return 0
	for d in "$DKMS_TREE/$PKG"/*/; do
		[ -d "$d" ] || continue
		name="$(basename "$d")"
		case "$name" in
		kernel-* | source) continue ;;
		esac
		echo "$name"
	done
}

do_uninstall() {
	local v k kernels=()
	for v in $(installed_versions); do
		log "dkms remove $PKG/$v (all kernels)"
		dkms remove -m "$PKG" -v "$v" --all || log "dkms remove $PKG/$v failed. Continuing."
	done
	for v in /usr/src/"${PKG}"-*; do
		[ -e "$v" ] || continue
		rm -rf "$v"
		log "Removed $v."
	done
	for f in "$MODPROBE_CONF" "$MODLOAD_CONF" "$RPS_RULE" "$RPS_HELPER"; do
		if [ -f "$f" ]; then
			mkdir -p "$BACKUP_DIR"
			cp -p "$f" "$BACKUP_DIR/$(basename "$f").$(date +%Y%m%d-%H%M%S).uninstall.bak"
			rm -f "$f"
			log "Removed $f (backup in $BACKUP_DIR)."
		fi
	done
	reload_udev_rules
	for k in /lib/modules/*/; do
		k="$(basename "$k")"
		[ -d "/lib/modules/$k/kernel" ] && kernels+=("$k")
	done
	for k in "${kernels[@]}"; do
		update_initramfs "$k"
	done
	log "Uninstall complete. To unload a running module, reboot or run 'rmmod xdma'."
}

if [ "$UNINSTALL" -eq 1 ]; then
	do_uninstall
	exit 0
fi

# 대상 커널 목록. 헤더(/lib/modules/<k>/build)가 있어야 빌드한다.
KERNELS=()
if [ -n "$KERNEL" ]; then
	[ -e "/lib/modules/$KERNEL/build" ] ||
		die "Headers for kernel $KERNEL are missing. Run 'apt install linux-headers-$KERNEL'."
	KERNELS=("$KERNEL")
else
	for d in /lib/modules/*/; do
		k="$(basename "$d")"
		if [ -e "/lib/modules/$k/build" ]; then
			KERNELS+=("$k")
		else
			log "Skipping kernel $k because its headers are missing."
		fi
	done
	[ "${#KERNELS[@]}" -gt 0 ] ||
		die "No installed kernel has headers. Run 'apt install linux-headers-\$(uname -r)'."
fi
log "Version $VERSION, target kernels: ${KERNELS[*]}"

# 1. 소스를 /usr/src 로 복사한다. 빌드 산출물은 넣지 않는다.
for p in xdma include dkms.conf dkms/check_nofpu.sh dkms/90-tsn-xdma-rps.rules dkms/tsn-xdma-rps; do
	[ -e "$DRIVER_DIR/$p" ] || die "Source file $p is missing from $DRIVER_DIR"
done
rm -rf "$SRC_DIR"
mkdir -p "$SRC_DIR/dkms"
cp -a "$DRIVER_DIR/xdma" "$DRIVER_DIR/include" "$SRC_DIR/"
cp -a "$DRIVER_DIR/dkms/check_nofpu.sh" "$SRC_DIR/dkms/"
for f in COPYING LICENSE; do
	[ -f "$DRIVER_DIR/$f" ] && cp -a "$DRIVER_DIR/$f" "$SRC_DIR/"
done
find "$SRC_DIR/xdma" \( -name '*.o' -o -name '*.ko' -o -name '*.mod' -o -name '*.mod.c' \
	-o -name '.*.cmd' -o -name 'modules.order' -o -name 'Module.symvers' -o -name '*.o.ur-safe' \) \
	-delete
chmod 0755 "$SRC_DIR/dkms/check_nofpu.sh"
sed "s/^VERSION=.*/VERSION=\"${VERSION}\"/" "$DRIVER_DIR/dkms.conf" >"$SRC_DIR/dkms.conf"
log "Copied the sources to $SRC_DIR."

# 2. 대상 커널에서 다른 버전을 제거한다. 두 버전이 같은 xdma.ko 를 다투지 않게 한다.
for v in $(installed_versions); do
	[ "$v" = "$VERSION" ] && continue
	if [ -n "$KERNEL" ]; then
		log "dkms remove $PKG/$v (kernel $KERNEL)"
		dkms remove -m "$PKG" -v "$v" -k "$KERNEL" || log "$PKG/$v is not installed for kernel $KERNEL."
	else
		log "dkms remove $PKG/$v (all kernels)"
		dkms remove -m "$PKG" -v "$v" --all || log "dkms remove $PKG/$v failed. Continuing."
	fi
done

# 3. dkms add, build, 검사, install.
if [ ! -d "$DKMS_TREE/$PKG/$VERSION" ]; then
	dkms add -m "$PKG" -v "$VERSION"
else
	log "$PKG/$VERSION is already registered with DKMS. Rebuilding with the updated sources."
fi

for k in "${KERNELS[@]}"; do
	log "Building for kernel $k"
	dkms build -m "$PKG" -v "$VERSION" -k "$k" --force

	# POST_BUILD 실패를 빌드 실패로 처리하지 않는 DKMS 버전이 있다. 그래서 여기서 다시 검사한다.
	ko=""
	# dkms 2.x 는 <커널>/<arch>/module 에, dkms 3.x 는 install 전까지 build/xdma 에 (압축해) 둔다.
	for cand in "$DKMS_TREE/$PKG/$VERSION/$k"/*/module/xdma.ko* "$DKMS_TREE/$PKG/$VERSION/build/xdma"/xdma.ko*; do
		[ -f "$cand" ] && ko="$cand" && break
	done
	[ -n "$ko" ] || die "Could not find the built xdma.ko for kernel $k."
	if ! "$SRC_DIR/dkms/check_nofpu.sh" "$ko"; then
		dkms remove -m "$PKG" -v "$VERSION" -k "$k" || true
		die "SIMD check failed. Installation for kernel $k was skipped."
	fi

	log "Installing for kernel $k"
	dkms install -m "$PKG" -v "$VERSION" -k "$k" --force
done

# 4. modprobe 설정.
# Ubuntu depmod 는 커널 내장 drivers/dma/xilinx/xdma.ko 를 extra/ 보다 우선한다.
# 그래서 install 줄로 우리 모듈을 직접 insmod 한다.
# DKMS 버전에 따라 모듈이 updates/dkms(dkms 2.8.7 실측) 또는 extra 에 가고 압축(.ko.zst 등)될 수 있다.
# updates/dkms 를 먼저 찾는다. extra 에는 예전에 손으로 넣은 모듈이 남아 있을 수 있다(.bak 등 다른 이름은 고르지 않음).
read -r -d '' MODPROBE_CONTENT <<'EOF' || true
# tsn-xdma: driver/dkms/install.sh 가 쓴 파일이다. 직접 고치지 않는다.
# 커널 내장 xdma(drivers/dma/xilinx)가 아니라 DKMS 로 설치한 TSN xdma.ko 를 적재한다.
install xdma ko=; for f in updates/dkms/xdma.ko updates/dkms/xdma.ko.zst updates/dkms/xdma.ko.xz extra/xdma.ko extra/xdma.ko.zst extra/xdma.ko.xz; do [ -f /lib/modules/$(uname -r)/$f ] && ko=/lib/modules/$(uname -r)/$f && break; done; [ -n "$ko" ] && /sbin/insmod "$ko" tsn_mode=1 rx_poll_mode=0 pmac_rx_enable=1
options xdma tsn_mode=1 rx_poll_mode=0 pmac_rx_enable=1
EOF
write_file "$MODPROBE_CONF" "$MODPROBE_CONTENT"

# 5. 부팅 시 자동 적재.
write_file "$MODLOAD_CONF" "xdma"

# 6. TSN 포트가 등록될 때 CPU1-3으로 수신 처리를 분산한다.
backup_if_differs "$RPS_HELPER" "$SCRIPT_DIR/tsn-xdma-rps"
install -D -m 0755 "$SCRIPT_DIR/tsn-xdma-rps" "$RPS_HELPER"
backup_if_differs "$RPS_RULE" "$SCRIPT_DIR/90-tsn-xdma-rps.rules"
install -D -m 0644 "$SCRIPT_DIR/90-tsn-xdma-rps.rules" "$RPS_RULE"
reload_udev_rules

# 7. initramfs 갱신.
for k in "${KERNELS[@]}"; do
	update_initramfs "$k"
done

log "Installation complete. Verify with 'dkms status $PKG'."
log "To load the driver into the running kernel: modprobe -r xdma 2>/dev/null; modprobe xdma"

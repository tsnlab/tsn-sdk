#!/bin/bash
# install.sh - tsn-xdma 드라이버(xdma.ko)를 DKMS 로 설치하거나 제거한다.
#
# 사용법(root 로 실행한다):
#   sudo ./install.sh [--version <버전>] [--kernel <커널 버전>]
#   sudo ./install.sh --uninstall
#
#   --version <버전>   DKMS 패키지 버전. 보통 git short SHA 를 넣는다. 기본값 1.0.0.
#   --kernel <버전>    이 커널에만 설치한다. 없으면 헤더가 있는 모든 커널에 설치한다.
#   --uninstall        모든 버전의 tsn-xdma 를 제거하고 설정 파일을 지운다.
#
# 하는 일:
#   1. 드라이버 소스를 /usr/src/tsn-xdma-<버전> 으로 복사한다.
#   2. 다른 버전의 tsn-xdma 를 대상 커널에서 제거한다.
#   3. dkms add, build, install 을 한다. 빌드 뒤 check_nofpu.sh 로 SIMD 사용을 검사한다.
#   4. /etc/modprobe.d/tsn-xdma.conf 를 쓴다. 내용이 다른 기존 파일은 /var/backups 에 보관한다.
#   5. /etc/modules-load.d/tsn-xdma.conf 에 xdma 를 쓴다(부팅 시 자동 적재).
#   6. TSN-SE 수신 큐의 RPS 마스크를 적용하는 udev 규칙을 설치한다.
#   7. 대상 커널의 initramfs 를 갱신한다.
# 여러 번 실행해도 결과가 같다.
set -euo pipefail

PKG="tsn-xdma"
VERSION="1.0.0"
KERNEL=""
UNINSTALL=0

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
	echo "[$PKG] 오류: $*" >&2
	exit 1
}

usage() {
	sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-0}"
}

while [ $# -gt 0 ]; do
	case "$1" in
	--version)
		[ $# -ge 2 ] || die "--version 에 값이 없습니다."
		VERSION="$2"
		shift 2
		;;
	--kernel)
		[ $# -ge 2 ] || die "--kernel 에 값이 없습니다."
		KERNEL="$2"
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
		echo "알 수 없는 인자: $1" >&2
		usage 1
		;;
	esac
done

[ "$(id -u)" -eq 0 ] || die "root 권한이 필요합니다. sudo 로 실행하십시오."
command -v dkms >/dev/null 2>&1 || die "dkms 가 없습니다. 'apt install dkms' 를 실행하십시오."

if ! printf '%s' "$VERSION" | grep -Eq '^[A-Za-z0-9][A-Za-z0-9._+-]*$'; then
	die "버전 형식이 잘못되었습니다: $VERSION"
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
		log "기존 $file 을 $dst 에 보관했습니다."
	fi
}

# 내용이 다를 때만 파일을 바꾼다.
write_file() {
	local file="$1" content="$2" tmp
	tmp="$(mktemp)"
	printf '%s\n' "$content" >"$tmp"
	if [ -f "$file" ] && cmp -s "$file" "$tmp"; then
		rm -f "$tmp"
		log "$file 은 이미 최신입니다."
		return 0
	fi
	backup_if_differs "$file" "$tmp"
	mkdir -p "$(dirname "$file")"
	install -m 0644 "$tmp" "$file"
	rm -f "$tmp"
	log "$file 을 썼습니다."
}

reload_udev_rules() {
	if [ -S /run/udev/control ] && command -v udevadm >/dev/null 2>&1; then
		udevadm control --reload-rules
	fi
}

update_initramfs() {
	local k="$1"
	if command -v update-initramfs >/dev/null 2>&1; then
		log "initramfs 갱신: $k"
		update-initramfs -u -k "$k"
	else
		log "update-initramfs 가 없습니다. initramfs 갱신을 건너뜁니다($k)."
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
		log "dkms remove $PKG/$v (모든 커널)"
		dkms remove -m "$PKG" -v "$v" --all || log "dkms remove $PKG/$v 실패. 계속합니다."
	done
	for v in /usr/src/"${PKG}"-*; do
		[ -e "$v" ] || continue
		rm -rf "$v"
		log "$v 를 지웠습니다."
	done
	for f in "$MODPROBE_CONF" "$MODLOAD_CONF" "$RPS_RULE" "$RPS_HELPER"; do
		if [ -f "$f" ]; then
			mkdir -p "$BACKUP_DIR"
			cp -p "$f" "$BACKUP_DIR/$(basename "$f").$(date +%Y%m%d-%H%M%S).uninstall.bak"
			rm -f "$f"
			log "$f 를 지웠습니다(사본은 $BACKUP_DIR)."
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
	log "제거를 마쳤습니다. 적재된 모듈은 재부팅하거나 'rmmod xdma' 로 내리십시오."
}

if [ "$UNINSTALL" -eq 1 ]; then
	do_uninstall
	exit 0
fi

# 대상 커널 목록. 헤더(/lib/modules/<k>/build)가 있어야 빌드한다.
KERNELS=()
if [ -n "$KERNEL" ]; then
	[ -e "/lib/modules/$KERNEL/build" ] ||
		die "커널 $KERNEL 의 헤더가 없습니다. 'apt install linux-headers-$KERNEL' 를 실행하십시오."
	KERNELS=("$KERNEL")
else
	for d in /lib/modules/*/; do
		k="$(basename "$d")"
		if [ -e "/lib/modules/$k/build" ]; then
			KERNELS+=("$k")
		else
			log "커널 $k 은 헤더가 없어 건너뜁니다."
		fi
	done
	[ "${#KERNELS[@]}" -gt 0 ] ||
		die "헤더가 설치된 커널이 없습니다. 'apt install linux-headers-\$(uname -r)' 를 실행하십시오."
fi
log "버전 $VERSION, 대상 커널: ${KERNELS[*]}"

# 1. 소스를 /usr/src 로 복사한다. 빌드 산출물은 넣지 않는다.
for p in xdma include dkms.conf dkms/check_nofpu.sh dkms/90-tsn-xdma-rps.rules dkms/tsn-xdma-rps; do
	[ -e "$DRIVER_DIR/$p" ] || die "소스에 $p 가 없습니다: $DRIVER_DIR"
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
log "소스를 $SRC_DIR 에 복사했습니다."

# 2. 대상 커널에서 다른 버전을 제거한다. 두 버전이 같은 xdma.ko 를 다투지 않게 한다.
for v in $(installed_versions); do
	[ "$v" = "$VERSION" ] && continue
	if [ -n "$KERNEL" ]; then
		log "dkms remove $PKG/$v (커널 $KERNEL)"
		dkms remove -m "$PKG" -v "$v" -k "$KERNEL" || log "커널 $KERNEL 에 $PKG/$v 가 없습니다."
	else
		log "dkms remove $PKG/$v (모든 커널)"
		dkms remove -m "$PKG" -v "$v" --all || log "dkms remove $PKG/$v 실패. 계속합니다."
	fi
done

# 3. dkms add, build, 검사, install.
if [ ! -d "$DKMS_TREE/$PKG/$VERSION" ]; then
	dkms add -m "$PKG" -v "$VERSION"
else
	log "$PKG/$VERSION 은 이미 DKMS 에 등록되어 있습니다. 소스를 갱신해 다시 빌드합니다."
fi

for k in "${KERNELS[@]}"; do
	log "빌드: 커널 $k"
	dkms build -m "$PKG" -v "$VERSION" -k "$k" --force

	# POST_BUILD 실패를 빌드 실패로 처리하지 않는 DKMS 버전이 있다. 그래서 여기서 다시 검사한다.
	ko=""
	# dkms 2.x 는 <커널>/<arch>/module 에, dkms 3.x 는 install 전까지 build/xdma 에 (압축해) 둔다.
	for cand in "$DKMS_TREE/$PKG/$VERSION/$k"/*/module/xdma.ko* "$DKMS_TREE/$PKG/$VERSION/build/xdma"/xdma.ko*; do
		[ -f "$cand" ] && ko="$cand" && break
	done
	[ -n "$ko" ] || die "빌드된 xdma.ko 를 찾지 못했습니다(커널 $k)."
	if ! "$SRC_DIR/dkms/check_nofpu.sh" "$ko"; then
		dkms remove -m "$PKG" -v "$VERSION" -k "$k" || true
		die "SIMD 검사 실패. 커널 $k 에 설치하지 않았습니다."
	fi

	log "설치: 커널 $k"
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
install xdma ko=; for f in updates/dkms/xdma.ko updates/dkms/xdma.ko.zst updates/dkms/xdma.ko.xz extra/xdma.ko extra/xdma.ko.zst extra/xdma.ko.xz; do [ -f /lib/modules/$(uname -r)/$f ] && ko=/lib/modules/$(uname -r)/$f && break; done; [ -n "$ko" ] && /sbin/insmod "$ko" se_mode=1 rx_poll_mode=0 pmac_rx_enable=1
options xdma se_mode=1 rx_poll_mode=0 pmac_rx_enable=1
EOF
write_file "$MODPROBE_CONF" "$MODPROBE_CONTENT"

# 5. 부팅 시 자동 적재.
write_file "$MODLOAD_CONF" "xdma"

# 6. TSN-SE 포트가 등록될 때 CPU1-3으로 수신 처리를 분산한다.
backup_if_differs "$RPS_HELPER" "$SCRIPT_DIR/tsn-xdma-rps"
install -D -m 0755 "$SCRIPT_DIR/tsn-xdma-rps" "$RPS_HELPER"
backup_if_differs "$RPS_RULE" "$SCRIPT_DIR/90-tsn-xdma-rps.rules"
install -D -m 0644 "$SCRIPT_DIR/90-tsn-xdma-rps.rules" "$RPS_RULE"
reload_udev_rules

# 7. initramfs 갱신.
for k in "${KERNELS[@]}"; do
	update_initramfs "$k"
done

log "설치를 마쳤습니다. 'dkms status $PKG' 로 상태를 확인하십시오."
log "실행 중 커널에 바로 적재하려면: modprobe -r xdma 2>/dev/null; modprobe xdma"

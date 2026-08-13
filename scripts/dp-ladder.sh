#!/bin/sh
# Walk the DaynaPort acceptance ladder inside IRIS: bring dp0 up and prove
# packets actually move. Picks up where scripts/iris-build.sh leaves off —
# that one compiles, links and boots a kernel; this one exercises the driver.
#
#   1 detected   dp0 attaches (INQUIRY + the type-3 dispatch)
#   2 MAC        read from the device via RETRIEVE STATISTICS, not the
#                00:80:19:00:00:NN placeholder dp_init() makes up
#   3 ARP        arp -a resolves the gateway  <- first proof of both directions
#   4 ping       replies from the NAT gateway
#   5 TCP        a handshake crosses the bus
#
# Requires an IRIS built with --features daynaport (and chd, for .chd disks):
#     cd ../iris && cargo build --release --features lightning,chd,daynaport
# and a DaynaPort target in the machine config — ci/iris-irix{53,65}-dayna.toml
# are ci/iris-irix{53,65}.toml plus:
#     [scsi.3]
#     kind = "daynaport"
#
# RUN scripts/iris-build.sh --release <rel> --boot-test FIRST. This script boots
# the kernel that produced, and does not build anything itself.
#
# Usage:
#   scripts/dp-ladder.sh --release 6.5 [--image PATH] [--config PATH]
#       [--iris-dir ../iris] [--work-hda PATH] [--ip 192.168.10.2]
#
# Config resolution matches iris-build.sh: flag > $IRIX{53,65}_IMAGE >
# ci/local.conf (falling back to ../irixscsitb/ci/local.conf).
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)

RELEASE=""
IMAGE=""
CONFIG=""
IRIS_DIR="${IRIS_DIR:-}"
WORK_HDA=""
GUEST_IP=192.168.10.2
GATEWAY_IP=192.168.10.1
NETMASK=0xffffff00
ROOT_PW="${IRIX_ROOT_PASSWORD:-}"

die() { echo "dp-ladder: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--release)  RELEASE="$2"; shift 2 ;;
		--image)    IMAGE="$2"; shift 2 ;;
		--config)   CONFIG="$2"; shift 2 ;;
		--iris-dir) IRIS_DIR="$2"; shift 2 ;;
		--work-hda) WORK_HDA="$2"; shift 2 ;;
		--ip)       GUEST_IP="$2"; shift 2 ;;
		-h|--help)  sed -n '2,30p' "$0"; exit 0 ;;
		*)          die "unknown option: $1" ;;
	esac
done

CONF="$REPO/ci/local.conf"
[ -f "$CONF" ] || CONF="$REPO/../irixscsitb/ci/local.conf"
conf_get() {
	[ -f "$CONF" ] || return 0
	sed -n "s/^$1=//p" "$CONF" | head -1 | sed 's/^"//; s/"$//'
}

case "$RELEASE" in
	5.3|53) RELEASE=5.3; IMG_KEY=IRIX53_IMAGE
	        : "${CONFIG:=$REPO/ci/iris-irix53-dayna.toml}" ;;
	6.5|65) RELEASE=6.5; IMG_KEY=IRIX65_IMAGE
	        : "${CONFIG:=$REPO/ci/iris-irix65-dayna.toml}" ;;
	*)      die "--release must be 5.3 or 6.5" ;;
esac

[ -n "$IMAGE" ] || IMAGE=$(eval "printf %s \"\${$IMG_KEY:-}\"")
[ -n "$IMAGE" ] || IMAGE=$(conf_get "$IMG_KEY")
[ -n "$IMAGE" ] || die "no boot disk: pass --image, set \$$IMG_KEY, or add it to $CONF"
[ -f "$IMAGE" ] || die "boot disk not found: $IMAGE"

[ -n "$IRIS_DIR" ] || IRIS_DIR=$(conf_get IRIS_DIR)
[ -n "$IRIS_DIR" ] || IRIS_DIR="$REPO/../iris"
IRIS="$IRIS_DIR/target/release/iris"
CI_BIN="$IRIS_DIR/target/release/iris-ci"
[ -x "$IRIS" ]   || die "iris not found at $IRIS"
[ -x "$CI_BIN" ] || die "iris-ci not found at $CI_BIN"

grep -q 'kind[ 	]*=[ 	]*"daynaport"' "$CONFIG" \
	|| die "$CONFIG has no [scsi.N] kind = \"daynaport\" target"

# Boot the kernel the build just linked, which lives in that build's overlay.
# iris-build.sh puts it in <its workdir>/overlay; --work-hda points into the
# same workdir, so derive it. Override by setting IRIS_CHD_DIFF_DIR yourself.
if [ -z "${IRIS_CHD_DIFF_DIR:-}" ] && [ -n "$WORK_HDA" ]; then
	_bd=$(cd "$(dirname "$WORK_HDA")" && pwd)
	[ -d "$_bd/overlay" ] && IRIS_CHD_DIFF_DIR="$_bd/overlay"
fi
[ -n "${IRIS_CHD_DIFF_DIR:-}" ] && export IRIS_CHD_DIFF_DIR

WORKDIR=$(mktemp -d /tmp/dpladder.XXXXXX)
CONSOLE="$WORKDIR/console.log"
SOCK="/tmp/iris-dpl.$$.sock"

CI() { IRIS_SOCKET="$SOCK" "$CI_BIN" "$@"; }
cleanup() {
	CI quit >/dev/null 2>&1 || true
	sleep 2
	kill "$(cat "$WORKDIR/iris.pid" 2>/dev/null)" 2>/dev/null || true
	rm -f "$SOCK"
}
trap cleanup EXIT INT TERM

ser_wait() { CI -q serial-wait "$1" --timeout "$2" >/dev/null 2>&1; }

echo ">>> launching IRIS (boot=$IMAGE)"
set -- --ci --config "$CONFIG" --ci-socket "$SOCK" --scsi1 "$IMAGE" --serial-log "$CONSOLE"
[ -n "$WORK_HDA" ] && set -- "$@" --scsi2 "$WORK_HDA"
( cd "$WORKDIR" && "$IRIS" "$@" > "$WORKDIR/iris.log" 2>&1 & echo $! > "$WORKDIR/iris.pid" )
i=0
until CI ping >/dev/null 2>&1; do
	i=$((i+1)); [ "$i" -lt 30 ] || { cat "$WORKDIR/iris.log"; die "control socket never came up"; }
	sleep 1
done

CI start >/dev/null
ser_wait "Option?" 120 || die "PROM menu never appeared (see $CONSOLE)"

if [ "$RELEASE" = 5.3 ]; then
	# 5.3 never promotes /unix.install (that happens on a clean shutdown, which
	# a PROM-driven boot skips), so name the kernel explicitly. EFS has no log,
	# so sash can read a freshly written kernel.
	echo ">>> booting 5.3 single-user (unix.install)"
	CI serial-send "5" >/dev/null;                       ser_wait ">>" 30   || die "no command monitor"
	CI serial-send "boot -f dksc(0,1,8)sash" >/dev/null; ser_wait "sash" 60 || die "sash never loaded"
	sleep 1
	CI serial-send "boot -f dksc(0,1,0)unix.install initstate=s" >/dev/null
	ser_wait "Single User Mode" 170 || ser_wait "Single User Mode" 170 \
		|| { tail -30 "$CONSOLE"; die "no single-user prompt"; }
	CI serial-send "$ROOT_PW" >/dev/null
	sleep 3
	run() {
		echo "--- $1"
		CI serial-send "$1 ; echo DP-'STEP'-DONE" >/dev/null
		ser_wait "DP-STEP-DONE" "${2:-90}" || echo "    (timed out)"
	}
else
	# 6.5's boot test shuts the guest down cleanly, which promotes
	# /unix.install to /unix — so the default boot IS the new kernel. Do not
	# name unix.install here: root is XFS, sash cannot replay its log, and a
	# just-written kernel is still in it ("execute format error").
	echo ">>> booting 6.5 multiuser"
	CI serial-send "1" >/dev/null
	ser_wait "console login" 170 || ser_wait "console login" 170 || ser_wait "console login" 170 \
		|| { tail -30 "$CONSOLE"; die "no login prompt"; }
	if [ -n "$ROOT_PW" ]; then CI -q login root --password "$ROOT_PW" >/dev/null
	else CI -q login root >/dev/null; fi
	run() { echo "--- $1"; CI run "$1" --timeout "${2:-90}" 2>&1 | tail -20; }
fi

echo
echo ">>> rung 1: attached?"
run "/usr/etc/ifconfig -a"

echo ">>> rung 2: MAC from the device (watch for the driver's MAC log line)"
run "/usr/etc/ifconfig dp0 $GUEST_IP netmask $NETMASK up" 120
run "/usr/etc/ifconfig dp0"

echo ">>> rung 3 + 4: ARP and ping the NAT gateway"
run "/usr/etc/ping -c 4 $GATEWAY_IP" 120
run "/usr/etc/arp -a"

echo ">>> rung 5: TCP through the gateway"
run "echo quit | /usr/bsd/telnet $GATEWAY_IP 21" 120

echo ">>> interface counters (Ipkts/Opkts is the honest measure)"
run "/usr/etc/netstat -in"
# The kernel's own verdict on what arrived: bad checksums here mean the record
# payload is wrong; a healthy run shows an "echo reply" input histogram entry.
run "/usr/etc/netstat -s | sed -n '/^icmp:/,/^igmp:/p'"

echo
echo ">>> done. Full console log: $CONSOLE"
echo "    IRIS side: telnet 127.0.0.1 8888 -> 'scsi dayna' for the target's own counters."

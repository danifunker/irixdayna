#!/bin/sh
# Build EVERY board object for one IRIX release inside a single IRIS guest, and
# package them into an inst tardist (Software Manager installable) with gendist.
# The release-time superset of iris-build.sh (which builds one board and boots
# it). This one does NOT boot-test - it cross-builds foreign-board objects that
# this guest cannot boot - it proves them by compiling, then packages.
#
#   --release 5.3   boots the 5.3 guest; builds o32 objects for IP20 IP22 IP12
#                   IP19; gendist -> dp-<ver>-53.tardist
#   --release 6.5   boots the 6.5 guest; builds n32/64 objects for IP22 IP32
#                   IP28 IP30 IP35; gendist -> dp-<ver>-65.tardist
#
# Outputs into --outdir:
#   dp-<board>.o           one per board (e.g. dp-ip20.o)
#   dp-<ver>-<53|65>.tardist   the inst product trio, tarred
#   master.c               the linked master.c for the native board (grep dp_)
#
# gendist ships in inst_dev.sw ("Software Packager"), part of the IDO dev
# option - present on any dev disk that can build the driver at all. If a disk
# somehow lacks it, the objects still build and only the tardist is skipped.
#
# Config resolution matches iris-build.sh (flag > env > shared/ci/local.conf).
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)

RELEASE=""; IMAGE=""; IRIS_DIR="${IRIS_DIR:-}"; CONFIG=""; RB=""
OUTDIR="$REPO/dist"; WORKDIR=""; VERSION=""; DO_GENDIST=1
ROOT_PW="${IRIX_ROOT_PASSWORD:-}"

die() { echo "iris-release: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--release)   RELEASE="$2"; shift 2 ;;
		--image)     IMAGE="$2"; shift 2 ;;
		--iris-dir)  IRIS_DIR="$2"; shift 2 ;;
		--config)    CONFIG="$2"; shift 2 ;;
		--rb-cli)    RB="$2"; shift 2 ;;
		--outdir)    OUTDIR="$2"; shift 2 ;;
		--workdir)   WORKDIR="$2"; shift 2 ;;
		--version)   VERSION="$2"; shift 2 ;;
		--no-gendist) DO_GENDIST=0; shift ;;
		-h|--help)   sed -n '2,40p' "$0"; exit 0 ;;
		*)           die "unknown option: $1" ;;
	esac
done

CONF="$REPO/shared/ci/local.conf"
[ -f "$CONF" ] || CONF="$REPO/../irixscsitb/ci/local.conf"
conf_get() { [ -f "$CONF" ] || return 0; sed -n "s/^$1=//p" "$CONF" | head -1 | sed 's/^"//; s/"$//'; }

case "$RELEASE" in
	5.3|53) RELEASE=5.3; IMG_KEY=IRIX53_IMAGE; TARDIST_SUF=53; SUBSYS=o32
	        ABI="o32"; ABI_DESC="o32, IRIX 5.3"; SRCDIR="$REPO/irix5.3"
	        BOARDS="IP20 IP22 IP12 IP19"; SMKW="INCLUDE"
	        : "${CONFIG:=$REPO/irix5.3/ci/iris-irix53.toml}" ;;
	6.5|65) RELEASE=6.5; IMG_KEY=IRIX65_IMAGE; TARDIST_SUF=65; SUBSYS=n32
	        ABI="n32/64"; ABI_DESC="n32 + 64-bit, IRIX 6.5"; SRCDIR="$REPO/irix6.5"
	        BOARDS="IP22 IP32 IP28 IP30 IP35"; SMKW="USE"
	        : "${CONFIG:=$REPO/irix6.5/ci/iris-irix65.toml}" ;;
	*)      die "--release must be 5.3 or 6.5" ;;
esac

[ -n "$VERSION" ] || VERSION=$(conf_get RELEASE_VERSION)
[ -n "$VERSION" ] || VERSION="0.0.0"
# inst orders upgrades by a NUMERIC version: first 10 digits of $VERSION.
INSTVER=$(printf %s "$VERSION" | tr -cd '0-9' | cut -c1-10)
[ -n "$INSTVER" ] || INSTVER=0

[ -n "$IMAGE" ] || IMAGE=$(eval "printf %s \"\${$IMG_KEY:-}\"")
[ -n "$IMAGE" ] || IMAGE=$(conf_get "$IMG_KEY")
[ -n "$IMAGE" ] || die "no boot disk: pass --image or set \$$IMG_KEY"
[ -f "$IMAGE" ] || die "boot disk not found: $IMAGE"


[ -n "$IRIS_DIR" ] || IRIS_DIR=$(conf_get IRIS_DIR)
[ -n "$IRIS_DIR" ] || IRIS_DIR="$REPO/../iris"
IRIS="$IRIS_DIR/target/release/iris"
CI_BIN="$IRIS_DIR/target/release/iris-ci"
[ -x "$IRIS" ]   || die "iris not found at $IRIS"
[ -x "$CI_BIN" ] || die "iris-ci not found at $CI_BIN"

[ -n "$RB" ] || RB=$(conf_get RB_CLI); [ -n "$RB" ] || RB="rb-cli"
command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || die "rb-cli not found ($RB)"

[ -n "$WORKDIR" ] || WORKDIR=$(mktemp -d /tmp/dprel.XXXXXX)
mkdir -p "$WORKDIR"
: "${IRIS_CHD_DIFF_DIR:=$WORKDIR/overlay}"; export IRIS_CHD_DIFF_DIR
mkdir -p "$IRIS_CHD_DIFF_DIR"

SOCK="/tmp/iris-dprel.$$.sock"
STAGE="$WORKDIR/stage"; HDA="$WORKDIR/work.hda"; CONSOLE="$WORKDIR/console.log"
CI() { IRIS_SOCKET="$SOCK" "$CI_BIN" "$@"; }
cleanup() { CI quit >/dev/null 2>&1 || true; sleep 1; kill "$(cat "$WORKDIR/iris.pid" 2>/dev/null)" 2>/dev/null || true; rm -f "$SOCK"; }
trap cleanup EXIT INT TERM

ser_send() { CI serial-send "$1"; }
ser_wait() { CI -q serial-wait "$1" --timeout "$2" >/dev/null 2>&1; }
ser_wait_long() {
	_p="$1"; _n="$2"; _l="$3"
	while [ "$_n" -gt 0 ]; do
		ser_wait "$_p" 170 && return 0
		_n=$(($_n - 1)); echo "    ... still waiting for $_l"
	done
	echo "iris-release: timed out waiting for $_l; console tail:" >&2
	tail -25 "$CONSOLE" >&2 || true; return 1
}

# ---- 1. stage sources + inst inputs on the work disk ----------------------
echo ">>> staging $RELEASE sources + inst product inputs"
rm -rf "$STAGE"; mkdir -p "$STAGE/src/master.d" "$STAGE/out" "$STAGE/inst"
cp "$SRCDIR/if_dp.c" "$SRCDIR/dp_proto.c" "$SRCDIR/sgi_ether.h" "$SRCDIR/Makefile" "$STAGE/src/"
cp "$SRCDIR/master.d/dp" "$STAGE/src/master.d/"
# inst product: spec (placeholders filled here), the board-pick exitop helper,
# the right master.d, and the .sm keyword file. The idb is generated in-guest
# once we know which objects actually built.
sed -e "s/@VERSION@/$INSTVER/" -e "s/@SUBSYS@/$SUBSYS/" \
    -e "s#@ABI@#$ABI#" -e "s#@ABI_DESC@#$ABI_DESC#" \
    "$REPO/inst/dp.spec" > "$STAGE/inst/dp.spec"
cp "$REPO/inst/dpinstall" "$STAGE/inst/dpinstall"
cp "$SRCDIR/master.d/dp" "$STAGE/inst/master.d-dp"
echo "$SMKW: dp" > "$STAGE/inst/dp.sm"
# The whole in-guest packaging runs from one staged script, so no fragile
# back-to-back serial sends (which garbled the idb when done inline).
cp "$REPO/shared/scripts/guest-mkdist.sh" "$STAGE/inst/guest-mkdist.sh"

echo ">>> assembling work disk $HDA"
rm -f "$HDA"
"$RB" -q --progress never new hd sgi-efs "$HDA" --size 128M --heads 16 --sectors 63 --from-dir "$STAGE"

# ---- 2. launch + boot -----------------------------------------------------
launch_iris() {
	echo ">>> launching IRIS (headless, boot=$IMAGE)"
	set -- --ci --config "$CONFIG" --ci-socket "$SOCK" --scsi1 "$IMAGE" --scsi2 "$HDA" --serial-log "$CONSOLE"
	( cd "$WORKDIR" && "$IRIS" "$@" > "$WORKDIR/iris.log" 2>&1 & echo $! > "$WORKDIR/iris.pid" )
	_i=0; until CI ping >/dev/null 2>&1; do _i=$((_i+1)); [ "$_i" -lt 30 ] || die "iris socket never came up"; sleep 1; done
}
boot_guest() {
	CI start
	ser_wait "Option?" 90 || die "PROM menu never appeared (see $CONSOLE)"
	if [ "$RELEASE" = 5.3 ]; then
		echo ">>> booting 5.3 single-user"
		ser_send "5"; ser_wait ">>" 30 || die "no command monitor"
		ser_send "boot -f dksc(0,1,8)sash"; ser_wait "sash" 60 || die "sash never loaded"; sleep 1
		ser_send "boot -f dksc(0,1,0)unix initstate=s"
		ser_wait_long "Single User Mode" 2 "single-user prompt" || return 1
		ser_send "$ROOT_PW"; sleep 2
	else
		echo ">>> booting 6.5 multiuser"
		ser_send "1"; ser_wait_long "console login" 3 "console login prompt" || return 1
		if [ -n "$ROOT_PW" ]; then CI -q login root --password "$ROOT_PW"; else CI -q login root; fi
	fi
}
launch_iris; boot_guest || exit 1

echo ">>> mounting work disk"
ser_send "test -d /mnt || mkdir /mnt ; mount /dev/dsk/dks0d2s0 /mnt && echo DP-'MNT'-OK"
ser_wait "DP-MNT-OK" 60 || { echo "mount failed:" >&2; tail -10 "$CONSOLE" >&2; exit 1; }
ser_send "rm -rf /tmp/dpb; mkdir /tmp/dpb && cp -r /mnt/src/* /tmp/dpb && echo DP-'COPY'-OK"
ser_wait "DP-COPY-OK" 60 || { tail -10 "$CONSOLE" >&2; exit 1; }

# ---- 3. build every board -------------------------------------------------
# Quiet production flags: canary only (no per-op / per-second logging).
CFLAGS_REL="-I. -DDP_CHECK_ETHERIF"
if [ "$RELEASE" = 6.5 ]; then _pre="BUILTIN=1"; else _pre=""; fi
BUILT=""
for b in $BOARDS; do
	echo ">>> compiling $RELEASE / $b"
	ser_send "cd /tmp/dpb && smake clean >/dev/null 2>&1 ; smake CPUBOARD=$b $_pre XCFLAGS=\"$CFLAGS_REL\" && cp dp.o /mnt/out/dp-`echo $b|tr A-Z a-z`.o && echo DP-'OK'-$b || echo DP-'FAIL'-$b"
	if ser_wait_long "DP-OK-$b" 3 "compile $b"; then
		BUILT="$BUILT $b"
	else
		echo "iris-release: WARNING $b failed to build (continuing); console tail:" >&2
		tail -20 "$CONSOLE" >&2 || true
	fi
done
[ -n "$BUILT" ] || die "no board built for $RELEASE"
ser_send "sync ; echo DP-'SYNC'-OK"; ser_wait "DP-SYNC-OK" 30 || true

# ---- 4. gendist the inst tardist ------------------------------------------
TARDIST="dp-$VERSION-$TARDIST_SUF.tardist"
if [ "$DO_GENDIST" = 1 ]; then
	# Provision gendist on 5.3 from the IDO CD if it is not already present.
	ser_send "test -x /usr/sbin/gendist && echo DP-'GD'-YES || echo DP-'GD'-NO"
	if ser_wait "DP-GD-YES" 20; then
		:
	else
		echo ">>> gendist not on this disk (build it from a disk with the IDO"
		echo "    dev option installed) - skipping the tardist, objects still ship"
		DO_GENDIST=0
	fi
fi

if [ "$DO_GENDIST" = 1 ]; then
	echo ">>> gendist: building the inst tardist (one staged script)"
	# One serial send -> nothing to garble. tee so the guest script's output
	# lands both in the console log (which we grep for the verdict) and in a
	# gendist.log on the work disk (extracted below for diagnostics). The
	# script prints GENDIST-OK only after the tardist is written.
	ser_send "sh /mnt/inst/guest-mkdist.sh $TARDIST 2>&1 | tee /mnt/out/gendist.log ; echo DP-'MKD'-DONE"
	ser_wait_long "DP-MKD-DONE" 3 "gendist" || true
	if grep -qa "GENDIST-OK" "$CONSOLE" 2>/dev/null; then
		echo ">>> tardist built: $TARDIST"
	else
		echo "iris-release: gendist did NOT produce a tardist (objects still ship)." >&2
		echo "              See gendist.log in $OUTDIR after this run." >&2
		DO_GENDIST=0
	fi
fi

ser_send "cd / && umount /mnt && sync && echo DP-'DONE'-OK"
ser_wait "DP-DONE-OK" 90 || true
CI quit >/dev/null 2>&1 || true; sleep 2

# ---- 5. pull artifacts back -----------------------------------------------
mkdir -p "$OUTDIR"
echo ">>> extracting artifacts to $OUTDIR"
for b in $BUILT; do
	o="dp-`echo $b|tr A-Z a-z`.o"
	"$RB" -q get --force "$HDA@1" "/out/$o" "$OUTDIR/$o" 2>/dev/null \
		&& echo "    $o" || echo "    (missing $o)"
done
if [ "$DO_GENDIST" = 1 ]; then
	"$RB" -q get --force "$HDA@1" "/out/$TARDIST" "$OUTDIR/$TARDIST" 2>/dev/null \
		&& echo "    $TARDIST" || echo "    (tardist not produced)"
fi
# gendist log is always useful (success or not) - best-effort.
"$RB" -q get --force "$HDA@1" "/out/gendist.log" "$OUTDIR/gendist-$TARDIST_SUF.log" 2>/dev/null || true

echo
echo "Done. $RELEASE release artifacts in $OUTDIR (boards built:$BUILT)."
echo "Console log: $CONSOLE"

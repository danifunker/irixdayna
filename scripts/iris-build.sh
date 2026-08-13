#!/bin/sh
# Compile the DaynaPort driver natively inside IRIX via the IRIS emulator,
# moving files in and out on a WORK DISK — an SGI EFS hard-disk image
# assembled by rb-cli on the host and attached to the guest as a second SCSI
# drive. No networking is needed in the guest: no DHCP, no NVRAM eaddr, no NFS.
#
# Adapted from the same pipeline in ../irixscsitb; the conventions (ci/*.toml,
# ci/local.conf, work-disk transfer, serial-driven PROM boot) are deliberately
# identical so the two repos stay familiar to each other.
#
#   --release 5.3   IRIX 5.3 guest, builds irix5.3/dp.o (o32/coff, IP22).
#                   Boots SINGLE-USER via sash — no rc2 services to hang a
#                   headless boot.
#   --release 6.5   IRIX 6.5 guest, builds the root dp.o. Boots MULTIUSER.
#
# WHY NATIVE, NOT CROSS: this is a kernel object. It must be compiled with the
# exact CFLAGS the target kernel was built with — on 5.3 those come from
# /var/sysgen/Makefile.kernio and include -coff, -Wc,-pic0, -Wx,-G8 and the
# R4000 errata workarounds. No cross-toolchain reproduces that, and a
# mismatched kernel object does not fail to link, it corrupts the kernel.
#
# WHAT THIS PROVES, AND WHAT IT DOES NOT
#
#   compile (default)  Resolves every "does this API exist / is this the right
#                      shape" question. This is most of the porting risk.
#   --autoconfig       Also runs `autoconfig -f` to LINK a real kernel with the
#                      driver in it. This is the symbol-resolution check: an
#                      undefined ether_attach/m_vget/itimeout fails HERE, which
#                      is exactly what you want to know. Uses the disposable
#                      overlay, so the boot image is untouched either way.
#   --boot-test        Also reboots into the kernel it just linked and waits
#                      for dp_init()'s banner. Proves the driver survives lboot
#                      and that its bus scan does not panic. Does NOT reach
#                      ether_attach() (no DaynaPort to match), so the
#                      DP_CHECK_ETHERIF canary stays unexercised.
#   NOT TESTED         Actual packet flow. IRIS emulates Indy hardware; it has
#                      no DaynaPort SCSI target, so nothing answers the vendor
#                      CDBs. Functional testing needs either real hardware with
#                      a BlueSCSI/ZuluSCSI, or a DaynaPort device model added to
#                      the emulator. Do not read a green run as "it works".
#
# THE BOOT DISK IS NEVER MODIFIED. ci/*.toml set overlay = true, so every guest
# write lands in <image>.chd.diff.chd next to the image; --fresh deletes it.
#
# Prereqs:
#   - iris + iris-ci (build ../iris, or scripts/fetch-iris.sh in irixscsitb)
#   - an installed IRIX boot disk (.chd) with cc + make + /var/sysgen.
#     Root must have an EMPTY password (or set IRIX_ROOT_PASSWORD).
#   - rb-cli with `new hd sgi-efs --from-dir`.
#
# Usage:
#   scripts/iris-build.sh --release 5.3 [--image PATH] [--iris-dir ../iris]
#       [--config ci/iris-irix53.toml] [--rb-cli rb-cli] [--outdir dist]
#       [--workdir DIR] [--fresh] [--autoconfig] [--boot-test] [--cflags "..."]
#
# Boot disk resolution (first match wins):
#   1. --image PATH
#   2. $IRIX53_IMAGE / $IRIX65_IMAGE
#   3. ci/local.conf (KEY=VALUE, parsed not sourced) — copy local.conf.example
#      If this repo has no ci/local.conf, ../irixscsitb/ci/local.conf is used
#      as a fallback, since the same two disk images serve both projects.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)

RELEASE=""
IMAGE=""
IRIS_DIR="${IRIS_DIR:-}"
CONFIG=""
RB=""
OUTDIR="$REPO/dist"
WORKDIR=""
FRESH=0
DO_AUTOCONFIG=0
DO_BOOTTEST=0
EXTRA_CFLAGS="-I. -DDP_LOG -DDP_LOG_SCSI -DDP_CHECK_ETHERIF"
ROOT_PW="${IRIX_ROOT_PASSWORD:-}"

die() { echo "iris-build: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--release)    RELEASE="$2"; shift 2 ;;
		--image)      IMAGE="$2"; shift 2 ;;
		--iris-dir)   IRIS_DIR="$2"; shift 2 ;;
		--config)     CONFIG="$2"; shift 2 ;;
		--rb-cli)     RB="$2"; shift 2 ;;
		--outdir)     OUTDIR="$2"; shift 2 ;;
		--workdir)    WORKDIR="$2"; shift 2 ;;
		--cflags)     EXTRA_CFLAGS="$2"; shift 2 ;;
		--fresh)      FRESH=1; shift ;;
		--autoconfig) DO_AUTOCONFIG=1; shift ;;
		--boot-test)  DO_AUTOCONFIG=1; DO_BOOTTEST=1; shift ;;
		-h|--help)    sed -n '2,60p' "$0"; exit 0 ;;
		*)            die "unknown option: $1" ;;
	esac
done

# ---- config file (this repo, else the irixscsitb one) ----------------------
CONF="$REPO/ci/local.conf"
[ -f "$CONF" ] || CONF="$REPO/../irixscsitb/ci/local.conf"
conf_get() {
	[ -f "$CONF" ] || return 0
	sed -n "s/^$1=//p" "$CONF" | head -1 | sed 's/^"//; s/"$//'
}

case "$RELEASE" in
	5.3|53) RELEASE=5.3; IMG_KEY=IRIX53_IMAGE
	        : "${CONFIG:=$REPO/ci/iris-irix53.toml}"
	        SRCDIR="$REPO/irix5.3"; OBJ=dp-irix53.o; CPUBOARD=IP22 ;;
	6.5|65) RELEASE=6.5; IMG_KEY=IRIX65_IMAGE
	        : "${CONFIG:=$REPO/ci/iris-irix65.toml}"
	        SRCDIR="$REPO";        OBJ=dp-irix65.o; CPUBOARD=IP22 ;;
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

[ -n "$RB" ] || RB=$(conf_get RB_CLI)
[ -n "$RB" ] || RB="rb-cli"
command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || die "rb-cli not found ($RB)"

[ -n "$WORKDIR" ] || WORKDIR=$(mktemp -d /tmp/dpbuild.XXXXXX)
mkdir -p "$WORKDIR"

SOCK="/tmp/iris-dp.$$.sock"
STAGE="$WORKDIR/stage"
HDA="$WORKDIR/work.hda"
CONSOLE="$WORKDIR/console.log"

CI() { IRIS_SOCKET="$SOCK" "$CI_BIN" "$@"; }

cleanup() {
	CI quit >/dev/null 2>&1 || true
	sleep 1
	kill "$(cat "$WORKDIR/iris.pid" 2>/dev/null)" 2>/dev/null || true
	rm -f "$SOCK"
}
trap cleanup EXIT INT TERM

# Serial helpers. Same approach as irixscsitb: iris-ci's own marker machinery
# greps for "\nIRIS-CI-RC=", which any shell that colours its output breaks.
# Instead each guest step ends in echo TOKEN-'OK' — quoted when TYPED so the
# command echo cannot match, contiguous when PRINTED — and we wait for the
# printed form. A single iris-ci wait caps at 300s, so long waits loop.
ser_send() { CI serial-send "$1"; }
ser_wait() { CI -q serial-wait "$1" --timeout "$2" >/dev/null 2>&1; }
ser_wait_long() {
	_p="$1"; _n="$2"; _l="$3"
	while [ "$_n" -gt 0 ]; do
		ser_wait "$_p" 170 && return 0
		_n=$(($_n - 1))
		echo "    ... still waiting for $_l"
	done
	echo "iris-build: timed out waiting for $_l; last console output:" >&2
	tail -25 "$CONSOLE" >&2 || true
	return 1
}

# ---- 1. stage the sources --------------------------------------------------
echo ">>> staging $RELEASE sources from $SRCDIR"
rm -rf "$STAGE"; mkdir -p "$STAGE/src" "$STAGE/src/master.d" "$STAGE/out"
cp "$SRCDIR/if_dp.c" "$SRCDIR/sgi_ether.h" "$SRCDIR/Makefile" "$STAGE/src/"
cp "$SRCDIR/master.d/dp" "$STAGE/src/master.d/"
echo "build output lands here" > "$STAGE/out/README"

# ---- 2. build the work disk ------------------------------------------------
# 64 MB EFS, 16 heads x 63 sectors (the geometry IRIS models).
echo ">>> assembling work disk $HDA"
rm -f "$HDA"
"$RB" -q --progress never new hd sgi-efs "$HDA" --size 64M --heads 16 --sectors 63 \
	--from-dir "$STAGE"

# ---- 3. launch iris --------------------------------------------------------
if [ "$FRESH" = 1 ]; then
	echo ">>> --fresh: dropping overlay ${IMAGE}.diff.chd"
	rm -f "${IMAGE}.diff.chd" "${IMAGE}.overlay" "${IMAGE}.overlay.dirty"
fi

launch_iris() {
	echo ">>> launching IRIS (headless, boot=$IMAGE, work=$HDA)"
	( cd "$WORKDIR" && "$IRIS" --ci --config "$CONFIG" --ci-socket "$SOCK" \
		--scsi1 "$IMAGE" --scsi2 "$HDA" --serial-log "$CONSOLE" \
		> "$WORKDIR/iris.log" 2>&1 & echo $! > "$WORKDIR/iris.pid" )
	_i=0
	until CI ping >/dev/null 2>&1; do
		_i=$((_i+1)); [ "$_i" -lt 30 ] || die "iris control socket never came up (see $WORKDIR/iris.log)"
		sleep 1
	done
}

# boot_guest — PROM menu through to a usable root shell.
boot_guest() {
	_kern="${1:-unix}"
	CI start
	ser_wait "Option?" 90 || die "PROM menu never appeared (see $CONSOLE)"
	if [ "$RELEASE" = 5.3 ]; then
		echo ">>> booting single-user (command monitor -> sash -> unix initstate=s)"
		ser_send "5"
		ser_wait ">>" 30 || die "command monitor prompt not seen"
		ser_send "boot -f dksc(0,1,8)sash"
		ser_wait "sash" 60 || die "sash never loaded"
		sleep 1
		ser_send "boot -f dksc(0,1,0)$_kern initstate=s"
		ser_wait_long "Single User Mode" 2 "single-user prompt" || return 1
		ser_send "$ROOT_PW"
		sleep 2
	else
		echo ">>> booting multiuser"
		ser_send "1"
		ser_wait_long "console login" 3 "console login prompt" || return 1
		if [ -n "$ROOT_PW" ]; then CI -q login root --password "$ROOT_PW"
		else CI -q login root; fi
	fi
	return 0
}

launch_iris

# ---- 4. boot ---------------------------------------------------------------
boot_guest || exit 1

# ---- 5. mount work disk and compile ----------------------------------------
# Guest lines are csh-AND-sh clean (`;` `&&` `||` `( )` only — no `$?`, no
# `{ }`, no redirects): root's login shell varies by image.
echo ">>> mounting work disk"
ser_send "test -d /mnt || mkdir /mnt ; mount /dev/dsk/dks0d2s0 /mnt && echo DP-'MNT'-OK"
ser_wait "DP-MNT-OK" 60 || { echo "work disk mount failed:" >&2; tail -10 "$CONSOLE" >&2; exit 1; }

# CPUBOARD is mandatory: 5.3's Makefile.kernio defines CFLAGS only inside
# "#if defined(CPUBOARD)". Unset means an EMPTY CFLAGS — no -D_KERNEL, no
# -coff — which compiles but produces an object that would wreck the kernel.
echo ">>> compiling ($RELEASE, CPUBOARD=$CPUBOARD)"
ser_send "rm -rf /tmp/dpb; mkdir /tmp/dpb && cp -r /mnt/src/* /tmp/dpb && cd /tmp/dpb && echo DP-'COPY'-OK"
ser_wait "DP-COPY-OK" 60 || { tail -10 "$CONSOLE" >&2; exit 1; }

ser_send "cd /tmp/dpb && smake CPUBOARD=$CPUBOARD MYCFLAGS=\"$EXTRA_CFLAGS\" && echo DP-'BUILD'-OK || echo DP-'BUILD'-FAIL"
if ! ser_wait_long "DP-BUILD-OK" 3 "native compile"; then
	echo "iris-build: COMPILE FAILED — console tail:" >&2
	tail -40 "$CONSOLE" >&2
	exit 1
fi

ser_send "cp /tmp/dpb/dp.o /mnt/out/$OBJ && echo DP-'OUT'-OK"
ser_wait "DP-OUT-OK" 60 || { tail -10 "$CONSOLE" >&2; exit 1; }

# ---- 5b. optional: link a real kernel (symbol-resolution check) ------------
# This is where an undefined ether_attach / m_vget / itimeout would surface.
# Everything it writes goes to the disposable overlay.
if [ "$DO_AUTOCONFIG" = 1 ]; then
	echo ">>> linking a kernel with the driver (autoconfig)"
	ser_send "cd /tmp/dpb && cp master.d/dp /var/sysgen/master.d/dp && cp dp.o /var/sysgen/boot/dp.o && echo DP-'INST'-OK"
	ser_wait "DP-INST-OK" 60 || { tail -10 "$CONSOLE" >&2; exit 1; }
	ser_send "grep -c '^INCLUDE: dp' /var/sysgen/system/irix.sm ; echo DP-'GREP'-DONE"
	ser_wait "DP-GREP-DONE" 30 || true
	ser_send "echo 'INCLUDE: dp' >> /var/sysgen/system/irix.sm ; echo DP-'SM'-OK"
	ser_wait "DP-SM-OK" 30 || true
	ser_send "/etc/autoconfig -f && echo DP-'AC'-OK || echo DP-'AC'-FAIL"
	if ! ser_wait_long "DP-AC-OK" 4 "autoconfig kernel link"; then
		echo "iris-build: KERNEL LINK FAILED — this is the symbol check." >&2
		echo "            Undefined symbols below name the wrong assumption:" >&2
		tail -40 "$CONSOLE" >&2
		exit 1
	fi
	ser_send "cp /var/sysgen/master.c /mnt/out/master.c ; echo DP-'MC'-OK"
	ser_wait "DP-MC-OK" 60 || true
	# autoconfig stages the new kernel as /unix.install; the promotion to
	# /unix happens during a clean shutdown, which a PROM-driven boot skips.
	# Log what is actually there so a failed boot test is diagnosable.
	ser_send "ls -l /unix /unix.install ; echo DP-'LS'-DONE"
	ser_wait "DP-LS-DONE" 30 || true
fi

ser_send "cd / && umount /mnt && sync && echo DP-'XFER'-OK"
ser_wait "DP-XFER-OK" 90 || { echo "umount/sync failed:" >&2; tail -10 "$CONSOLE" >&2; exit 1; }

CI quit >/dev/null 2>&1 || true
sleep 2
kill "$(cat "$WORKDIR/iris.pid" 2>/dev/null)" 2>/dev/null || true
rm -f "$SOCK"
sleep 1

# ---- 5c. optional: boot the kernel we just linked ---------------------------
# The new /unix lives in the copy-on-write overlay, so relaunching WITHOUT
# --fresh boots it. This proves the driver survives lboot AND that dp_init()
# runs its bus scan without panicking against a real (emulated) WD33C93.
#
# It does NOT exercise ether_attach(), and therefore not the DP_CHECK_ETHERIF
# canary: ether_attach is only reached from dp_do_attach(), which only runs
# when the INQUIRY match finds a DaynaPort. IRIS has no such target, so the
# scan correctly finds nothing. The struct etherif question stays open until
# this runs against real hardware or an emulated DaynaPort.
if [ "$DO_BOOTTEST" = 1 ]; then
	# Boot /unix.install directly: autoconfig stages the new kernel there and
	# only a clean shutdown renames it to /unix. Driving the PROM ourselves
	# skips that, so booting "unix" would silently boot the OLD kernel — which
	# looks exactly like "the driver never initialised".
	echo ">>> rebooting into the newly linked kernel (/unix.install)"
	launch_iris
	if ! boot_guest unix.install; then
		echo "iris-build: THE NEW KERNEL DID NOT BOOT." >&2
		echo "            The driver panics or hangs at init. Console tail:" >&2
		tail -40 "$CONSOLE" >&2
		echo "            (the pristine image is untouched; --fresh resets the overlay)" >&2
		exit 1
	fi
	echo ">>> kernel booted; checking for the driver banner"
	if ser_wait "DaynaPort SCSI/Link driver" 20; then
		echo ">>> dp_init() ran."
	else
		# The banner is printed during boot, so it may already have scrolled
		# past the wait window; fall back to grepping the console log.
		if grep -q "DaynaPort SCSI/Link driver" "$CONSOLE"; then
			echo ">>> dp_init() ran (found in console log)."
		else
			echo "iris-build: kernel booted but dp_init() never announced itself." >&2
			echo "            Check 'INCLUDE: dp' and the master.d flags." >&2
			exit 1
		fi
	fi
	if grep -q "LAYOUT MISMATCH" "$CONSOLE"; then
		echo "iris-build: DP_CHECK_ETHERIF CANARY TRIPPED - sgi_ether.h is wrong." >&2
		grep -A6 "LAYOUT MISMATCH" "$CONSOLE" >&2
		exit 1
	fi
	CI quit >/dev/null 2>&1 || true
	sleep 2
fi

# ---- 6. pull the object back -----------------------------------------------
echo ">>> extracting $OBJ from the work disk"
mkdir -p "$OUTDIR"
"$RB" -q get --force "$HDA@1" "/out/$OBJ" "$OUTDIR/$OBJ"
[ -s "$OUTDIR/$OBJ" ] || die "no $OBJ on the work disk (build failed? see $CONSOLE)"
if [ "$DO_AUTOCONFIG" = 1 ]; then
	"$RB" -q get --force "$HDA@1" /out/master.c "$OUTDIR/master-$RELEASE.c" 2>/dev/null || true
	[ -s "$OUTDIR/master-$RELEASE.c" ] && \
		echo ">>> generated master.c: $OUTDIR/master-$RELEASE.c (grep it for dp_)"
fi

echo ">>> built:"
file "$OUTDIR/$OBJ" || true
ls -la "$OUTDIR/$OBJ"

echo
echo "Done. Native $RELEASE kernel object; boot disk untouched."
echo "Console log: $CONSOLE"
echo "Work dir kept for inspection: $WORKDIR"
if [ "$DO_AUTOCONFIG" = 0 ]; then
	echo
	echo "NOTE: this compiled the driver but did not link a kernel."
	echo "      Re-run with --autoconfig to check symbol resolution."
fi
echo "NOTE: nothing here tests packet flow — IRIS has no DaynaPort target."

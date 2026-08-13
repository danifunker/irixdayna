#!/bin/sh
# Build an EFS CD-ROM image carrying the 5.3 driver, for machines with no
# network yet: drop it on a BlueSCSI/ZuluSCSI SD card as a CD image, mount it
# on the target and run install.sh.
#
#   scripts/mk-dp-cd.sh [--out dist/dp-irix53.iso] [--rb-cli rb-cli]
#
# Expects the per-board objects to exist already:
#   dist/irix53-ip20/dp.o    dist/irix53-ip22/dp.o
# Build them with:
#   scripts/iris-build.sh --release 5.3 --cpuboard IP20 \
#       --cflags "-DDP_LOG -DDP_CHECK_ETHERIF" --outdir dist/irix53-ip20
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO/dist/dp-irix53.iso"
RB=rb-cli
while [ $# -gt 0 ]; do
	case "$1" in
		--out)    OUT="$2"; shift 2 ;;
		--rb-cli) RB="$2"; shift 2 ;;
		-h|--help) sed -n '2,14p' "$0"; exit 0 ;;
		*) echo "mk-dp-cd: unknown option: $1" >&2; exit 1 ;;
	esac
done
command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || { echo "mk-dp-cd: rb-cli not found ($RB)" >&2; exit 1; }

STAGE=$(mktemp -d /tmp/dpcd.XXXXXX)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/dp"

for b in ip20 ip22; do
	src="$REPO/dist/irix53-$b/dp.o"
	[ -f "$src" ] || { echo "mk-dp-cd: missing $src - build it first (see --help)" >&2; exit 1; }
	cp "$src" "$STAGE/dp/dp-$b.o"
done

# THE 5.3 MASTER FILE, not the 6.5 one in the repo root. They differ by three
# characters in a place nobody looks: 6.5 uses flags "nscR" plus a
# +thread_class directive, neither of which 5.3's lboot can parse, and
# installing it makes autoconfig fail with parse errors.
cp "$REPO/irix5.3/master.d/dp"        "$STAGE/dp/master.d-dp"
cp "$REPO/installer/irix53/install.sh"   "$STAGE/dp/install.sh"
cp "$REPO/installer/irix53/uninstall.sh" "$STAGE/dp/uninstall.sh"
cp "$REPO/dist/irix53-ip20/INSTALL.txt"  "$STAGE/dp/README.txt" 2>/dev/null || true
chmod +x "$STAGE/dp/install.sh" "$STAGE/dp/uninstall.sh"

grep '^+thread_class' "$STAGE/dp/master.d-dp" >/dev/null 2>&1 && {
	echo "mk-dp-cd: staged master file is the 6.5 one - refusing" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
"$RB" -q --progress never optical new sgi-efs "$OUT" --size auto --from-dir "$STAGE"
echo "built $OUT"
"$RB" -q optical browse "$OUT" || true
echo
echo "On the target (N = the CD's SCSI id):"
echo "    mount -t efs -o ro /dev/dsk/dks0d\${N}s7 /mnt"
echo "    sh /mnt/dp/install.sh"

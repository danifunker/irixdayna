#!/bin/sh
# Wrap the inst tardist into mountable release media:
#   dp-<ver>-<53|65>.iso     EFS CD-ROM, product unpacked at the volume root so
#                            IRIX 'mount -t efs <dev>s7 /CDROM; inst -f /CDROM'
#                            installs it directly.
#   dp-<ver>-<53|65>.hda     dvh+EFS hard-disk image, same layout - attach as a
#                            SCSI disk (e.g. a BlueSCSI HDD), mount, inst -f.
#   dp-<ver>-<53|65>.tar.gz  the .tardist + README, for scp/NFS delivery.
#
# The tardist itself already carries every board object; these are just the
# delivery vehicles. Input dir is what iris-release.sh produced (the tardist
# and the loose objects); this reads only the tardist.
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
RELEASE=""; VERSION=""; INDIR=""; OUTDIR=""; RB=""
die() { echo "package-dp: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
	case "$1" in
		--release) RELEASE="$2"; shift 2 ;;
		--version) VERSION="$2"; shift 2 ;;
		--indir)   INDIR="$2"; shift 2 ;;
		--outdir)  OUTDIR="$2"; shift 2 ;;
		--rb-cli)  RB="$2"; shift 2 ;;
		-h|--help) sed -n '2,20p' "$0"; exit 0 ;;
		*)         die "unknown option: $1" ;;
	esac
done

case "$RELEASE" in
	5.3|53) SUF=53; KW="INCLUDE"; ABI="o32 (IRIX 5.3)" ;;
	6.5|65) SUF=65; KW="USE";     ABI="n32/64 (IRIX 6.5)" ;;
	*)      die "--release must be 5.3 or 6.5" ;;
esac
[ -n "$VERSION" ] || die "--version required"
[ -n "$INDIR" ]   || die "--indir required"
[ -n "$OUTDIR" ]  || OUTDIR="$INDIR"
[ -n "$RB" ] || RB="rb-cli"
command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || die "rb-cli not found ($RB)"

TARDIST="$INDIR/dp-$VERSION-$SUF.tardist"
[ -f "$TARDIST" ] || die "no tardist at $TARDIST (run iris-release.sh --release $RELEASE first)"

mkdir -p "$OUTDIR"
STAGE=$(mktemp -d /tmp/dppkg.XXXXXX)
trap 'rm -rf "$STAGE"' EXIT INT TERM
cp "$TARDIST" "$STAGE/"

cat > "$STAGE/README.txt" <<EOF
DaynaPort SCSI/Link Ethernet driver $VERSION -- $ABI

This medium carries an IRIX inst product (dp / dp.idb / dp.sw) with every
supported board object. Install it with the Software Manager:

  # CD:  mount -t efs -o ro /dev/dsk/dks0dNs7 /CDROM   (N = the CD's SCSI id)
  # HDD: mount -t efs      /dev/dsk/dks0dNs0 /CDROM   (N = the disk's SCSI id)
  inst -f /CDROM
      Inst> install dp.sw.driver
      Inst> go
      Inst> quit
  /var/sysgen/dp/dpinstall        # picks YOUR board by hinv, relinks the kernel
  /etc/shutdown -y -g0 -i6        # clean reboot; dp0 appears

'$KW: dp' is the config line inst installs. To remove: inst/swmgr -> remove
dp.sw.driver, then /etc/autoconfig -f and reboot.
EOF

echo ">>> building dp-$VERSION-$SUF.iso"
rm -f "$OUTDIR/dp-$VERSION-$SUF.iso"
"$RB" -q --progress never optical new sgi-efs "$OUTDIR/dp-$VERSION-$SUF.iso" \
	--from-dir "$STAGE" --expand-archives --flatten-folders --size auto

echo ">>> building dp-$VERSION-$SUF.hda"
rm -f "$OUTDIR/dp-$VERSION-$SUF.hda"
"$RB" -q --progress never new hd sgi-efs "$OUTDIR/dp-$VERSION-$SUF.hda" \
	--from-dir "$STAGE" --expand-archives --flatten-folders \
	--size 32M --heads 16 --sectors 63

echo ">>> building dp-$VERSION-$SUF.tar.gz"
( cd "$STAGE" && tar czf "$OUTDIR/dp-$VERSION-$SUF.tar.gz" "dp-$VERSION-$SUF.tardist" README.txt )

echo ">>> release media in $OUTDIR:"
ls -l "$OUTDIR"/dp-"$VERSION"-"$SUF".iso "$OUTDIR"/dp-"$VERSION"-"$SUF".hda "$OUTDIR"/dp-"$VERSION"-"$SUF".tar.gz

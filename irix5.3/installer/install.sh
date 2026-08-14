#!/bin/sh
# Install the DaynaPort SCSI/Link driver from this CD. Run as root.
#
#   mount -t efs -o ro /dev/dsk/dks0dNs7 /mnt      (N = this CD's SCSI id)
#   sh /mnt/dp/install.sh
#
# Pick the object matching YOUR board - check with: hinv | head -1
set -e
here=`dirname "$0"`

board=`hinv | head -1 | sed -n 's/.*\(IP[0-9][0-9]*\).*/\1/p'`
case "$board" in
IP20) obj=dp-ip20.o ;;
IP22) obj=dp-ip22.o ;;
*)    echo "This CD carries IP20 and IP22 objects; hinv says $board." >&2
      echo "Build from source for that board: smake CPUBOARD=$board" >&2
      exit 1 ;;
esac
echo "Board $board -> installing $obj"

# The 6.5 master file uses flags "nscR" and a +thread_class directive, neither
# of which 5.3's lboot understands - installing it makes autoconfig spew parse
# errors. Refuse rather than write it.
if grep '^+thread_class' "$here/master.d-dp" >/dev/null 2>&1; then
    echo "ERROR: master.d-dp on this disc is the 6.5 file (has +thread_class)." >&2
    echo "       It would break autoconfig on 5.3. Not installing." >&2
    exit 1
fi

cp "$here/$obj"          /var/sysgen/boot/dp.o
cp "$here/master.d-dp"   /var/sysgen/master.d/dp
echo 'INCLUDE: dp'     > /var/sysgen/system/dp.sm

# An earlier hand-install may have put the same line in irix.sm. Naming the
# driver twice is not better than once.
if grep -s 'INCLUDE: *dp' /var/sysgen/system/irix.sm >/dev/null 2>&1; then
    echo ""
    echo "WARNING: irix.sm also contains an 'INCLUDE: dp' line. Remove it -"
    echo "         the driver is now named by /var/sysgen/system/dp.sm."
fi

echo ""
echo "Installed. Now:"
echo "    /etc/autoconfig -f"
echo "    grep -c dp_start /var/sysgen/master.c    # expect >= 1"
echo "    /etc/shutdown -y -g0 -i6                 # clean: promotes /unix.install"

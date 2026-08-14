#!/bin/sh
# Remove the DaynaPort driver from this IRIX 5.3 system.
#
# Takes the driver back out of the kernel. Your previous kernel also survives
# as /unix.save - if a kernel ever fails to boot, the PROM command monitor can
# start it directly:
#     boot -f dksc(0,1,0)unix.save
set -e
rm -f /var/sysgen/system/dp.sm
rm -f /var/sysgen/boot/dp.o
rm -f /var/sysgen/master.d/dp
echo "Removed dp.sm, boot/dp.o and master.d/dp."
echo ""
echo "If an earlier install appended 'INCLUDE: dp' to"
echo "/var/sysgen/system/irix.sm by hand, remove that line too - otherwise"
echo "lboot will look for a driver that is no longer there:"
grep -n 'INCLUDE: *dp' /var/sysgen/system/irix.sm 2>/dev/null || echo "  (none found - good)"
echo ""
echo "Then relink and reboot cleanly:"
echo "    /etc/autoconfig -f"
echo "    /etc/shutdown -y -g0 -i6"

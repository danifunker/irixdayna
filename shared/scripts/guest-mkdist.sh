#!/bin/sh
# Runs INSIDE the IRIX guest (staged onto the work disk, invoked with a single
# serial command so nothing is garbled by back-to-back sends). Assembles the
# inst product from the board objects in /mnt/out and the inst inputs in
# /mnt/inst, runs gendist, and tars the product trio to /mnt/out/$1.
#
# Prints exactly one of: GENDIST-OK  /  GENDIST-FAIL  on the last line.
#   $1  tardist filename (written under /mnt/out)
TARDIST="$1"
GD=/tmp/gd

rm -rf "$GD"; mkdir -p "$GD/boot" "$GD/dist"
cp /mnt/inst/dp.spec      "$GD/dp.spec"
cp /mnt/inst/dpinstall    "$GD/dpinstall"; chmod 755 "$GD/dpinstall"
cp /mnt/inst/master.d-dp  "$GD/master.d-dp"
cp /mnt/inst/dp.sm        "$GD/dp.sm"
cp /mnt/out/dp-ip*.o      "$GD/boot/" 2>/dev/null

# idb: gendist wants entries sorted by destination path. We EMIT them already
# sorted rather than piping through sort(1) - IRIX 5.3's System V sort rejects
# the POSIX -k flag ("Incorrect usage") and would hand back an empty file.
# Order of the dest-path first characters: boot < dp < master.d < system, and
# the boot glob expands in sorted order, so this is correctly ordered.
# Sources are relative to -sbase ($GD).
{
    for o in "$GD"/boot/dp-ip*.o; do
        [ -f "$o" ] || continue
        bn=`basename "$o"`
        echo "f 0644 root sys var/sysgen/boot/$bn boot/$bn dp.sw.driver"
    done
    echo "f 0755 root sys var/sysgen/dp/dpinstall dpinstall dp.sw.driver"
    echo "f 0644 root sys var/sysgen/master.d/dp master.d-dp dp.sw.driver"
    echo "f 0644 root sys var/sysgen/system/dp.sm dp.sm dp.sw.driver"
} > "$GD/dp.idb"

echo "=== dp.idb ==="; cat "$GD/dp.idb"
echo "=== gendist ==="
cd "$GD"
if gendist -verbose -sbase "$GD" -idb "$GD/dp.idb" -spec "$GD/dp.spec" -dist "$GD/dist" -all; then
    if [ -f "$GD/dist/dp" ] && [ -f "$GD/dist/dp.sw" ]; then
        ( cd "$GD/dist" && tar cf "/mnt/out/$TARDIST" dp dp.idb dp.sw ) && echo GENDIST-OK
    else
        echo "gendist produced no product trio in $GD/dist:"; ls -l "$GD/dist"
        echo GENDIST-FAIL
    fi
else
    echo GENDIST-FAIL
fi

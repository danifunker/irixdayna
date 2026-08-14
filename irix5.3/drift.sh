#!/bin/sh
#
# drift.sh - verify that the shared region of irix5.3/if_dp.c is still
# byte-for-byte identical to the corresponding region of ../irix6.5/if_dp.c.
#
# The two drivers deliberately duplicate the DaynaPort protocol code
# (RX multi-packet parser, TX ring, runqueue, CDB builders and the
# etherif handlers) rather than hiding the large 5.3/6.5 differences
# behind #ifdefs.  That is only safe if the duplication stays exact, so
# this check exists to make divergence loud instead of silent.
#
# A protocol fix belongs in BOTH files.
#
# Exit 0 if identical, 1 if drifted.

set -e

here=`dirname "$0"`
new="$here/if_dp.c"
old="$here/../irix6.5/if_dp.c"

for f in "$new" "$old"; do
    if [ ! -f "$f" ]; then
        echo "drift: cannot find $f" >&2
        exit 2
    fi
done

# The shared region starts at the dp_scsi_cmd_locked banner comment in
# both files.  It ends at the first thing that follows dp_eio_ioctl():
# "#ifdef DP_MODULE" in the 6.5 driver, the "END SHARED" banner in ours.
extract_65() {
    awk '/^\/\* dp_scsi_cmd_locked/{f=1} /^#ifdef DP_MODULE/{f=0} f' "$1"
}
extract_53() {
    awk '/^\/\* dp_scsi_cmd_locked/{f=1} /^\/\* =====/{if(f)f=0} f' "$1"
}

tmp_old=`mktemp /tmp/dpdrift.XXXXXX`
tmp_new=`mktemp /tmp/dpdrift.XXXXXX`
trap 'rm -f "$tmp_old" "$tmp_new"' 0 1 2 3 15

extract_65 "$old" > "$tmp_old"
extract_53 "$new" > "$tmp_new"

if [ ! -s "$tmp_old" ] || [ ! -s "$tmp_new" ]; then
    echo "drift: FAILED to extract shared region - markers moved?" >&2
    echo "       6.5 region: `wc -l < $tmp_old` lines" >&2
    echo "       5.3 region: `wc -l < $tmp_new` lines" >&2
    exit 2
fi

if diff -u "$tmp_old" "$tmp_new" > /dev/null 2>&1; then
    echo "drift: OK - shared region identical (`wc -l < $tmp_new | tr -d ' '` lines)"
    exit 0
fi

echo "drift: DIVERGED - irix6.5/if_dp.c and irix5.3/if_dp.c no longer agree." >&2
echo "       (-) 6.5   (+) 5.3" >&2
echo >&2
diff -u "$tmp_old" "$tmp_new" >&2 || true
exit 1

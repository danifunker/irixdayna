#!/bin/sh
# Resolve the IRIX boot disk (.chd) for a build, printing its absolute path on
# STDOUT. One image-acquisition path for the GitHub Actions workflow AND local
# runs.
#
# Source, first match wins:
#   1. $IRIX53_IMAGE / $IRIX65_IMAGE   a local .chd path on the runner (used
#                                      as-is; for self-hosted runners / devs).
#   2. $IRIX53_DISK_URL / $IRIX65_DISK_URL   a download URL — e.g. a GitHub
#                                      release asset's download link. Fetched
#                                      with plain curl, so the asset must be
#                                      reachable without auth (public release,
#                                      S3, presigned, ...). Accepts a bare .chd
#                                      or a .zip containing one.
#
# Idempotent: an existing non-empty --dest is reused (actions/cache-friendly).
#
# Usage:
#   IMG=$(shared/scripts/fetch-image.sh --release 5.3 [--dest guest.chd])
#   shared/scripts/fetch-image.sh --release 6.5 --check-only
set -eu

die() { echo "fetch-image: $*" >&2; exit 1; }
abspath() { case "$1" in /*) printf '%s\n' "$1" ;; *) printf '%s/%s\n' "$(pwd)" "$1" ;; esac; }

RELEASE=""; DEST=""; MODE="fetch"
while [ $# -gt 0 ]; do
	case "$1" in
		--release)    RELEASE="$2"; shift 2 ;;
		--dest)       DEST="$2"; shift 2 ;;
		--check-only) MODE="check"; shift ;;
		-h|--help)    sed -n '2,20p' "$0"; exit 0 ;;
		*)            die "unknown option: $1" ;;
	esac
done

case "$RELEASE" in
	5.3|53) LOCAL="${IRIX53_IMAGE:-}"; URL="${IRIX53_DISK_URL:-}"; LKEY=IRIX53_IMAGE; UKEY=IRIX53_DISK_URL ;;
	6.5|65) LOCAL="${IRIX65_IMAGE:-}"; URL="${IRIX65_DISK_URL:-}"; LKEY=IRIX65_IMAGE; UKEY=IRIX65_DISK_URL ;;
	*)      die "--release must be 5.3 or 6.5" ;;
esac
[ -n "$DEST" ] || DEST="guest-disk-$RELEASE.chd"

no_source() { die "no boot disk for --release $RELEASE: set \$$LKEY (a local path) or \$$UKEY (a download URL)"; }

if [ "$MODE" = check ]; then
	{ [ -n "$LOCAL" ] || [ -n "$URL" ]; } && { echo "fetch-image: $RELEASE source present" >&2; exit 0; }
	no_source
fi

# 1. local path — trust it, don't copy
if [ -n "$LOCAL" ]; then
	[ -f "$LOCAL" ] || die "\$$LKEY points at $LOCAL which does not exist"
	abspath "$LOCAL"; exit 0
fi
[ -n "$URL" ] || no_source

# cache hit
if [ -s "$DEST" ]; then
	echo "fetch-image: reusing existing $DEST (cache hit)" >&2
	abspath "$DEST"; exit 0
fi

echo "fetch-image: downloading the $RELEASE boot disk" >&2
tmp="$DEST.download"; rm -f "$tmp"
curl -fsSL "$URL" -o "$tmp" || die "download failed: $URL"
# accept a bare .chd or a .zip that contains one
if unzip -l "$tmp" >/dev/null 2>&1; then
	command -v unzip >/dev/null 2>&1 || die "downloaded a zip but no unzip"
	xdir="$DEST.unzip"; rm -rf "$xdir"; unzip -q -o "$tmp" -d "$xdir"
	chd=$(find "$xdir" -iname '*.chd' | head -1)
	[ -n "$chd" ] || die "no .chd inside the downloaded zip"
	mv "$chd" "$DEST"; rm -rf "$xdir" "$tmp"
else
	mv "$tmp" "$DEST"
fi
[ -s "$DEST" ] || die "download produced an empty file"
echo "fetch-image: $DEST ($(wc -c < "$DEST" | tr -d ' ') bytes)" >&2
abspath "$DEST"

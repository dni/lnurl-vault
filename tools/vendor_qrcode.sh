#!/usr/bin/env bash
#
# Vendors ricmoo/QRCode into src/ui/, at a pinned commit, verified by hash.
#
# WHY THIS IS PINNED. This library is compiled into a firmware that holds
# bearer secrets, and both workflows used to fetch it with
# `git clone --depth 1`, i.e. whatever the default branch happened to point at
# when the release was cut. Nobody had to compromise anything for that to go
# wrong; upstream force-pushing, or a tag moving, silently changes what ships,
# and there is no record afterwards of what actually went in. The commit and
# the two file hashes below are that record, and a mismatch fails the build
# rather than being reported and ignored.
#
# WHY IT IS VENDORED AT ALL, rather than declared as a dependency: neither
# PlatformIO's LDF nor ESP-IDF's component manager will take it (an
# Arduino-style library.properties in the first case, a repo that is not
# component-shaped in the second) -- platformio.ini's own comments spell both
# out, with the exact errors, and src/ui/qr_display.c's header comment covers
# the rest.
#
# Used by .github/workflows/ci.yml and release.yml, and usable by hand for a
# local build -- see README.md's "Build & flash".
set -euo pipefail

# github.com/ricmoo/QRCode. Last upstream commit is from 2020: the library is
# dormant, which makes pinning free and makes an unexpected change a much
# louder signal than it would be for a live dependency.
QRCODE_COMMIT="eafbde494979abc2445c363cc2602230bcbe299c"
QRCODE_C_SHA256="340c1690bb046a5b2198829af1d9287c54419322b8cb41552b3e3506cfe73fb0"
QRCODE_H_SHA256="dd9ce6fe5cb0c8be11e84dccad9c12488f2ce7366d573148d245c1754429862a"

DEST="${1:-src/ui}"

# sha256sum on Linux and CI, shasum on macOS. Both print "<hash>  <path>".
sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  else
    shasum -a 256 "$1" | cut -d' ' -f1
  fi
}

expect_sha256() {
  local path="$1" want="$2" got
  got="$(sha256_of "$path")"
  if [ "$got" != "$want" ]; then
    echo "ERROR: $(basename "$path") does not match its pinned hash." >&2
    echo "  expected $want" >&2
    echo "  actual   $got" >&2
    echo "Refusing to vendor unverified third-party code into a firmware that" >&2
    echo "holds bearer secrets. If this change is intended, update the hash in" >&2
    echo "$0 in the same commit that explains why." >&2
    exit 1
  fi
}

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# Fetch exactly the pinned object rather than cloning a branch. --depth 1 on a
# specific commit keeps it to one object's history.
git -C "$tmpdir" init -q
git -C "$tmpdir" remote add origin https://github.com/ricmoo/QRCode
git -C "$tmpdir" fetch -q --depth 1 origin "$QRCODE_COMMIT"
git -C "$tmpdir" checkout -q FETCH_HEAD

src_c="$(find "$tmpdir" -maxdepth 2 -name 'qrcode.c' | head -1)"
src_h="$(find "$tmpdir" -maxdepth 2 -name 'qrcode.h' | head -1)"
if [ -z "$src_c" ] || [ -z "$src_h" ]; then
  echo "ERROR: qrcode.c/qrcode.h not found at $QRCODE_COMMIT" >&2
  exit 1
fi

# Verified BEFORE the patch below, so the hashes describe upstream's bytes
# rather than ours.
expect_sha256 "$src_c" "$QRCODE_C_SHA256"
expect_sha256 "$src_h" "$QRCODE_H_SHA256"

mkdir -p "$DEST"
cp "$src_c" "$DEST/qrcode.c"

# The one patch this library needs: it defines bool/true/false itself, which
# are real keywords under the -std=gnu23 that this ESP-IDF version hardcodes
# for chip targets (and that no build_flags override can beat, since IDF's own
# -std= comes later on the compile line). See qr_display.c's header comment.
#
# grep -v rather than `sed -i`: the in-place flag takes an argument on BSD sed
# and not on GNU sed, so a single invocation cannot work on both macOS and CI.
# That is not hypothetical -- the GNU form was in both workflows and failed the
# moment anyone ran the same step locally on a Mac.
grep -v -e 'typedef unsigned char bool;' \
        -e 'static const bool false = 0;' \
        -e 'static const bool true = 1;' \
        "$src_h" > "$DEST/qrcode.h"

echo "vendored ricmoo/QRCode @ ${QRCODE_COMMIT} into ${DEST} (hashes verified)"

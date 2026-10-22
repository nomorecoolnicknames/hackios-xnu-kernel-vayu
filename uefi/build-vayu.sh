#!/bin/bash
# Build the patched Mu-Silicium UEFI for vayu (POCO X3 Pro).
#
# usage: uefi/build-vayu.sh
# env:   MU=/path/to/Mu-Silicium   (default: ../Mu-Silicium next to this repo)
#        VENV=/path/to/venv        (default: ../venv)
#
# Idempotent: re-running keeps the existing clone, skips an already-applied
# patch, and reuses the venv. It does NOT flash anything.
set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
MU=${MU:-$ROOT/Mu-Silicium}
VENV=${VENV:-$ROOT/venv}
BASE=$(cat "$HERE/BASE")

if [ ! -d "$MU/.git" ]; then
	echo "build-vayu: cloning Mu-Silicium at $BASE"
	git init -q "$MU"
	git -C "$MU" remote add origin https://github.com/Project-Silicium/Mu-Silicium.git
	git -C "$MU" fetch --depth 1 origin "$BASE"
	git -C "$MU" checkout -q FETCH_HEAD
fi

cd "$MU"
# Top-level submodules only; stuart --update brings what the build needs.
git submodule update --init --depth 1

if git apply --reverse --check "$HERE/mu-silicium.patch" >/dev/null 2>&1; then
	echo "build-vayu: XNUP patch already applied"
else
	echo "build-vayu: applying XNUP patch"
	git apply --whitespace=nowarn "$HERE/mu-silicium.patch"
fi

DEST=Silicon/Silicium/SiliciumPkg/Library/BootManagerLib
mkdir -p "$DEST"
cp "$HERE/files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.c" "$DEST/"
cp "$HERE/files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.h" "$DEST/"

if [ ! -x "$VENV/bin/python" ]; then
	echo "build-vayu: creating $VENV"
	python3 -m venv "$VENV"
	"$VENV/bin/pip" install -q --upgrade pip
fi
"$VENV/bin/pip" install -q -r pip-requirements.txt

echo "build-vayu: building vayuPkg"
source "$VENV/bin/activate"
python3 build_uefi.py -d vayu -r RELEASE

FD="$MU/Build/vayuPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd"
[ -f "$FD" ] || { echo "build-vayu: no $FD" >&2; exit 1; }
echo "build-vayu: FD $FD ($(stat -c %s "$FD") bytes)"
sha256sum "$FD"

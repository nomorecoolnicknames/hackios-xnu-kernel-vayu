#!/bin/bash
# Build the patched Mu-Silicium UEFI for the POCO X3 Pro (vayu): the same XNUP hook as the POCO X5 Pro's port
# (../../poco-x5-pro/uefi: mu-silicium.patch and XnuLoader.[ch]) on Mu-Silicium's own vayuPkg.
#
# usage: devices/poco-x3-pro/uefi/build.sh
# env:   MU=/path/to/Mu-Silicium   (default: Mu-Silicium at the top of the repository)
#        VENV=/path/to/venv        (default: venv at the top of the repository)
#
# Idempotent: re-running keeps the clone, skips a patch that is already applied and reuses the venv. It does
# not flash anything. The result is Mu-Silicium/Build/vayuPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd, which
# mkxnuimg.py (DEVICE=poco-x3-pro) picks up.
set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
HOOK=$ROOT/devices/poco-x5-pro/uefi
MU=${MU:-$ROOT/Mu-Silicium}
VENV=${VENV:-$ROOT/venv}
BASE=$(cat "$HERE/BASE")

if [ ! -d "$MU/.git" ]; then
	echo "build.sh: cloning Mu-Silicium at $BASE"
	git init -q "$MU"
	git -C "$MU" remote add origin https://github.com/Project-Silicium/Mu-Silicium.git
	git -C "$MU" fetch --depth 1 origin "$BASE"
	git -C "$MU" checkout -q FETCH_HEAD
fi

cd "$MU"
# top-level submodules only; stuart --update brings what the build needs
git submodule update --init --depth 1

# Mu-Silicium's RequiredSubmodule second argument is "recursive", not "required": keep the test-only
# OpenSSL/SPDM trees out of the update
if git apply --reverse --check "$HERE/no-recursive-submodules.patch" >/dev/null 2>&1; then
	echo "build.sh: no-recursion patch already applied"
else
	echo "build.sh: applying no-recursion patch"
	git apply --whitespace=nowarn "$HERE/no-recursive-submodules.patch"
fi

if git apply --reverse --check "$HOOK/mu-silicium.patch" >/dev/null 2>&1; then
	echo "build.sh: XNUP patch already applied"
else
	echo "build.sh: applying XNUP patch"
	git apply --whitespace=nowarn "$HOOK/mu-silicium.patch"
fi

DEST=Silicon/Silicium/SiliciumPkg/Library/BootManagerLib
mkdir -p "$DEST"
cp "$HOOK/files/$DEST/XnuLoader.c" "$HOOK/files/$DEST/XnuLoader.h" "$DEST/"

if [ ! -x "$VENV/bin/python" ]; then
	echo "build.sh: creating $VENV"
	python3 -m venv "$VENV"
	"$VENV/bin/pip" install -q --upgrade pip
fi
"$VENV/bin/pip" install -q -r pip-requirements.txt

echo "build.sh: building vayuPkg"
source "$VENV/bin/activate"
python3 build_uefi.py -d vayu -r RELEASE

FD="$MU/Build/vayuPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd"
[ -f "$FD" ] || { echo "build.sh: no $FD" >&2; exit 1; }
echo "build.sh: FD $FD ($(stat -c %s "$FD") bytes)"
sha256sum "$FD"

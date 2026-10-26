# The POCO X3 Pro's own programs for the user-space image. Not run by itself: userland/build.sh sources
# it (in userland/, with $DYN, $LDID and obj/ set up) when DEVICE=poco-x3-pro, and stages what it names:
#   DEVICE_BIN   programs for /redwood/bin       DEVICE_LIB   libraries for /redwood/lib
#   DEVICE_APPS  application bundles in obj/ for /Applications of the system image
# Every program is signed ad hoc with ldid -Cadhoc: the code directory's CS_ADHOC flag (0x2) is what makes
# the kernel's trust shim treat a program as a platform binary, and `ldid -S` alone leaves it 0 (launchd
# panics on a non-platform binary run as the job com.apple.BlueTool). Without ldid the programs are
# built unsigned, with a warning.
V=../devices/$DEVICE/userland/src
sign() { # sign OBJ [ENTITLEMENTS]
	if [ -n "$2" ]; then ${LDID:-ldid} -Cadhoc -S"$2" "$1" 2>/dev/null; else ${LDID:-ldid} -Cadhoc -S "$1" 2>/dev/null; fi ||
		echo "build.sh: no ldid: $(basename "$1") is unsigned" >&2
}

# the answer to bluetoothd's boot-script request where BlueTool would be (Qualcomm controller)
$DYN -fblocks -o obj/bluetool_stub $V/bluetool_stub.c
sign obj/bluetool_stub
# one burst on the IR blaster, /dev/vayu-ir (kernel vayu_ir=1): vayu_irtest [carrier_hz] [ms]
$DYN -Wall -Wextra -o obj/vayu_irtest $V/vayu_irtest.c
sign obj/vayu_irtest
# a song into the Music library, through MusicLibrary.framework (private): signed with what Music.app holds
$DYN -fobjc-arc -F${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk/System/Library/PrivateFrameworks \
	-framework Foundation -framework MediaPlayer -framework MusicLibrary -lsqlite3 -o obj/music_import $V/music_import.m
sign obj/music_import $V/music_import.entitlements
# files into the Files app's local storage and pictures into Photos
$DYN -fobjc-arc -framework Foundation -framework Photos -o obj/media_import $V/media_import.m
sign obj/media_import $V/media_import.entitlements
# video: decode (VideoToolbox hardware and software), AVPlayer, encode
$DYN -fobjc-arc -framework Foundation -framework AVFoundation -framework VideoToolbox -framework CoreMedia \
	-framework CoreVideo -o obj/video_test $V/video_test.m
# an MP3 decoded and played with no application in between
$DYN -fobjc-arc -framework Foundation -framework AVFoundation -framework AudioToolbox -o obj/play_test $V/play_test.m
# have LaunchServices rebuild its database of system applications and plug-ins; and run iOS's own first-boot
# data migrators in one process (signed with datamigrator's entitlements, taken from the system image)
$DYN -fobjc-arc -framework Foundation -o obj/ls_rebuild $V/ls_rebuild.m
sign obj/ls_rebuild $V/ls_rebuild.entitlements
$DYN -fobjc-arc -framework Foundation -o obj/run_migrator $V/run_migrator.m
if command -v ${LDID:-ldid} >/dev/null && [ -f $IOS/rootfs/usr/libexec/datamigrator ] &&
    ${LDID:-ldid} -e $IOS/rootfs/usr/libexec/datamigrator > obj/datamigrator.entitlements 2>/dev/null; then
	sign obj/run_migrator obj/datamigrator.entitlements
else
	sign obj/run_migrator
fi
# the accelerometer to iOS, in backboardd beside libhakios (does nothing without /dev/vayu-motion)
$DYN -dynamiclib -install_name /redwood/lib/libhakios.motion.dylib -framework CoreFoundation -framework IOKit -lobjc \
	-o obj/libhakios.motion.dylib $V/motion.c
INJECT="$INJECT
/redwood/lib/libhakios.motion.dylib backboardd"

# the ambient-light and proximity sensors to iOS, in backboardd beside the two above: a virtual HID
# service each (ALS 0x20/0x41, proximity 0xFF00/0x08), fed from /dev/vayu-als and /dev/vayu-prox
# (or /dev/vayu-motion v2). Nothing at all happens without those devices: vayu_slpi=6 vayu_ssc_als=1
$DYN -dynamiclib -install_name /redwood/lib/libhakios.sensors.dylib -framework CoreFoundation -framework IOKit -lobjc \
	-o obj/libhakios.sensors.dylib $V/sensors.c
INJECT="$INJECT
/redwood/lib/libhakios.sensors.dylib backboardd"

# HakiosMP3.app: a minimal MP3 player of our own, to tell a player's bug from another app's; everything it
# does is logged as "mp3play:". A plain directory of real files (a bundle of symbolic links is refused by
# LaunchServices), copied into /Applications of the system image.
$DYN -fobjc-arc -framework Foundation -framework UIKit -framework AVFoundation -framework CoreGraphics \
	-o obj/HakiosMP3 $V/mp3_app.m
sign obj/HakiosMP3 $V/mp3_app.entitlements
rm -rf obj/HakiosMP3.app
mkdir -p obj/HakiosMP3.app
cp obj/HakiosMP3 obj/HakiosMP3.app/
cp $V/mp3_app.plist obj/HakiosMP3.app/Info.plist
printf 'APPL????' > obj/HakiosMP3.app/PkgInfo
# its icon: a white note on green (no alpha channel: iOS masks an icon itself)
python3 - obj/HakiosMP3.app <<'PY'
import struct, sys, zlib
out = sys.argv[1]
for size, scale in ((120, 2), (180, 3)):
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            u, v = x / size, y / size
            head = (u - 0.38) ** 2 + ((v - 0.70) * 1.3) ** 2 < 0.012
            stem = 0.455 < u < 0.50 and 0.22 < v < 0.70
            flag = 0.50 <= u < 0.66 and 0.22 < v < 0.30
            rows += bytes((255, 255, 255) if head or stem or flag else (40, 170, 90))
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b''))
    open('%s/AppIcon60x60@%dx.png' % (out, scale), 'wb').write(png)
PY

DEVICE_BIN="bluetool_stub vayu_irtest music_import media_import video_test play_test ls_rebuild run_migrator"
DEVICE_LIB="libhakios.motion.dylib libhakios.sensors.dylib"
DEVICE_APPS="HakiosMP3.app"

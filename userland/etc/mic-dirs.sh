# The Voice Memos recordings directory (stream 5, 2026-11-04).
#
# A device has /var/mobile/Media/Recordings from its first boot; our /private/var is a fresh ramfs
# made from /etc/var.skel at every boot, and that template had no Recordings (it has Media, DCIM,
# PhotoData, Photos, Downloads, iTunes_Control...). The app-path microphone test (mic_app, TEST26)
# printed "path /var/mobile/Media/Recordings: No such file or directory" in all four variants, and
# Voice Memos writes its recordings through the same AVAudioRecorder path.
#
# The template (/etc/var.skel) now carries the directory as well (init mkdir+chmod+chown 0755
# 501:501 before var_store's restore), so this script is the second, idempotent half: it runs after
# the restore, from rc.jobs, and prints what it found, so the run log has the state either way.
for d in "/var/mobile/Media/Recordings"; do
	/var/jb/bin/mkdir -p "$d"
	/var/jb/bin/chown 501:501 "$d"
	# chmod is a coreutils one: /var/jb/bin on this prefix, /var/jb/usr/bin on others (fp-dirs.sh
	# needs only mkdir/chown/ls, so the prefix's chmod has not been used by a boot before).
	c=/var/jb/bin/chmod
	[ -x "$c" ] || c=/var/jb/usr/bin/chmod
	[ -x "$c" ] && "$c" 0755 "$d"
done
/var/jb/bin/ls -lad /var/mobile/Media /var/mobile/Media/Recordings

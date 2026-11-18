# The application path of the microphone, on demand (stream 5, 2026-11-04).
#
# Voice Memos records through AVAudioSession + AVAudioRecorder; record_test is the same pair, so its
# numbers are the ones that decide whether the app path works when the UI is not reachable. It writes
# /tmp/record_test.wav (on the phone), prints the meters every half second and, at the end, the peak
# of the file, its RMS and the RMS of each tenth of a second in dBFS.
#
# Run it over SSH, not from rc.jobs: two readers of /dev/hakios-mic at once share one ring, and a
# gate that runs while Voice Memos records would spoil the recording it is meant to measure.
#
#   /var/jb/bin/sh /redwood/etc/mic-gate.sh          # wait for the node, then record 5 s
#   /var/jb/bin/sh /redwood/etc/mic-gate.sh 10       # 10 s
secs=${1:-5}
i=0
while [ ! -c /dev/hakios-mic ] && [ $i -lt 240 ]; do
	/var/jb/bin/sleep 1
	i=$((i + 1))
done
if [ ! -c /dev/hakios-mic ]; then
	echo "mic-gate: /dev/hakios-mic is not there after 240 s (vayu_mic=1, vayu_jack>=2?)"
	echo "mic-gate: the kernel's line would be: vayu-audio: mic: /dev/hakios-mic is made"
fi
/var/jb/bin/sleep 2
echo "mic-gate: recording $secs s through AVAudioSession/AVAudioRecorder (record_test)"
/redwood/bin/record_test "$secs"
echo "mic-gate: done; the file is /tmp/record_test.wav"

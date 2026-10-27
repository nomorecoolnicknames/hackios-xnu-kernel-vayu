/*
 * HakiosAudio.driver: the output device of iOS on this machine.
 *
 * Audio devices reach the audio server's hardware layer (the "HAL" of CoreAudio, inside
 * mediaserverd) in two ways: kernel drivers through IOAudio2 -- that is how an iPhone's own
 * speaker, codec and microphones arrive, and none of it exists here -- and plug-ins in
 * /System/Library/Audio/Plug-Ins/HAL, which is how Bluetooth, AirPlay, CarPlay and USB audio
 * arrive. This is such a plug-in. Its interface is the one macOS documents for user-space
 * audio drivers (AudioServerPlugIn: a CFPlugIn with one COM-style table of functions; the HAL
 * asks for objects and their properties, then runs the I/O); the iOS SDK has no header for it,
 * so what is needed of it is declared below.
 *
 * Above the HAL sits the virtual audio device (/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin),
 * through which everything in iOS is routed. It knows an iPhone's own devices by their names
 * (their UIDs) and makes a port of each: "Speaker" becomes the speaker port, the route an iPhone
 * with nothing plugged in plays through, with a volume, the volume display and the place in
 * Control Center that go with it. So this presents four devices under those names:
 *
 *   "Speaker"   a stereo output whose samples are written to /dev/hakios-audio, where the
 *               kernel takes them to whatever can play them (see sys/hakios.audio.h), and a
 *               stereo input that is always silent (on an iPhone: what the amplifiers measure)
 *   "Hawking"   one silent input and nothing else. The virtual audio device will not build the
 *               speaker route without a second virtual device of its own (its type 'vhaw', a
 *               mandatory companion of the default one in the iPhone X's routing tables) whose
 *               only port is this input; without it everything stays on the null port.
 *   "Actuator"  one output of one channel: the iPhone's Taptic Engine, on which iOS plays its
 *               haptics as waveforms (through the chain 'd22_actuator_general'); its samples go to
 *               the kernel's motor driver, /dev/hakios-haptic (see actuator_write())
 *   "Codec"     one input of one channel: on an iPhone, the audio codec that the built-in
 *               microphones are on, of which the virtual audio device makes the built-in microphone
 *               port; its samples come from the kernel, /dev/hakios-mic (the phone's main
 *               microphone, boot-arg mic=4), which listens only while this input runs (see codec_read())
 *
 * CoreAudio's own null device ("Null_Device", described in the framework's Info.plist, playing
 * into nowhere) stays as it is: it is the only device the virtual audio device takes while it
 * starts, and ours are taken a moment later, when it says it allows all devices.
 *
 * What is played through the speaker port passes the iPhone X's chain of effects for its own
 * speakers first ('d22_speaker_general', from /Library/Audio/Tunings/D22: equalisers,
 * compressors, speaker protection), at the volume iOS has set.
 *
 * The device has no clock but the system's: it claims to play exactly HAKIOS_AUDIO_RATE frames
 * a second of mach_absolute_time(), and the HAL's I/O thread paces itself by that.
 *
 * The speaker's output has a volume control, as the iPhone's own speaker has: what is set on it
 * goes to the kernel (ioctl HAKIOS_AUDIO_SET_VOLUME), which sets the amplifiers' level by it -- on
 * the phone, so far under the loudest it allows. Its scale is 48 dB in a straight line of
 * decibels, 3 dB for each of iOS's sixteen steps; 0 is silence. What iOS does with it (the phone,
 * 15 October): the virtual audio device sets it once, to its top (0 dB), when it takes the
 * device, and leaves it there -- its routing tables keep the iPhone X's speaker in software volume
 * mode, so the volume buttons and Control Center scale the samples themselves, and at 0 what is
 * written is silence (on which the kernel keeps the amplifiers off).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreAudioTypes/CoreAudioTypes.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hakios.audio.h"

/* ---- the AudioServerPlugIn interface (macOS: CoreAudio/AudioServerPlugIn.h, AudioHardwareBase.h) ---- */

typedef UInt32 AudioObjectID;
typedef struct {
	UInt32 mSelector, mScope, mElement;
} AudioObjectPropertyAddress;
typedef struct {
	AudioStreamBasicDescription mFormat;
	AudioValueRange mSampleRateRange;
} AudioStreamRangedDescription;
typedef struct {
	UInt32 mClientID;
	pid_t mProcessID;
	Boolean mIsNativeEndian;
	CFStringRef mBundleID;
} AudioServerPlugInClientInfo;
typedef struct {
	UInt64 mIOCycleCounter;
	UInt32 mNominalIOBufferFrameSize;
	AudioTimeStamp mCurrentTime, mInputTime, mOutputTime;
	Float64 mMasterHostTicksPerFrame, mDeviceHostTicksPerFrame;
} AudioServerPlugInIOCycleInfo;

typedef struct AudioServerPlugInHostInterface AudioServerPlugInHostInterface;
typedef const AudioServerPlugInHostInterface *AudioServerPlugInHostRef;
struct AudioServerPlugInHostInterface {
	OSStatus (*PropertiesChanged)(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress *);
	OSStatus (*CopyFromStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef *);
	OSStatus (*WriteToStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef);
	OSStatus (*DeleteFromStorage)(AudioServerPlugInHostRef, CFStringRef);
	OSStatus (*RequestDeviceConfigurationChange)(AudioServerPlugInHostRef, AudioObjectID, UInt64, void *);
};

typedef struct AudioServerPlugInDriverInterface AudioServerPlugInDriverInterface;
typedef AudioServerPlugInDriverInterface **AudioServerPlugInDriverRef;
struct AudioServerPlugInDriverInterface {
	void *_reserved;
	HRESULT (*QueryInterface)(void *, REFIID, LPVOID *);
	ULONG (*AddRef)(void *);
	ULONG (*Release)(void *);
	OSStatus (*Initialize)(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
	OSStatus (*CreateDevice)(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
	OSStatus (*DestroyDevice)(AudioServerPlugInDriverRef, AudioObjectID);
	OSStatus (*AddDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*RemoveDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*PerformDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	OSStatus (*AbortDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	Boolean (*HasProperty)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
	OSStatus (*IsPropertySettable)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
	OSStatus (*GetPropertyDataSize)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32 *);
	OSStatus (*GetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, UInt32 *, void *);
	OSStatus (*SetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, const void *);
	OSStatus (*StartIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*StopIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*GetZeroTimeStamp)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
	OSStatus (*WillDoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
	OSStatus (*BeginIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
	OSStatus (*DoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *, void *, void *);
	OSStatus (*EndIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
};

#define kAudioServerPlugInTypeUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0x44, 0x3A, 0xBA, 0xB8, 0xE7, 0xB3, 0x49, 0x1A, \
	    0xB9, 0x85, 0xBE, 0xB9, 0x18, 0x70, 0x30, 0xDB)
#define kAudioServerPlugInDriverInterfaceUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0xEE, 0xA5, 0x77, 0x3D, 0xCC, 0x43, \
	    0x49, 0xF1, 0x8E, 0x00, 0x8F, 0x96, 0xE7, 0xD2, 0x3B, 0x17)

enum {
	kAudioHardwareUnknownPropertyError = 'who?', kAudioHardwareBadPropertySizeError = '!siz',
	kAudioHardwareIllegalOperationError = 'nope', kAudioHardwareBadObjectError = '!obj',
	kAudioHardwareUnsupportedOperationError = 'unop', kAudioDeviceUnsupportedFormatError = '!dat',

	kAudioObjectPropertyScopeGlobal = 'glob', kAudioObjectPropertyScopeInput = 'inpt', kAudioObjectPropertyScopeOutput = 'outp',

	kAudioObjectClassID = 'aobj', kAudioPlugInClassID = 'aplg', kAudioDeviceClassID = 'adev', kAudioStreamClassID = 'astr',

	kAudioObjectPropertyBaseClass = 'bcls', kAudioObjectPropertyClass = 'clas', kAudioObjectPropertyOwner = 'stdv',
	kAudioObjectPropertyName = 'lnam', kAudioObjectPropertyManufacturer = 'lmak', kAudioObjectPropertyOwnedObjects = 'ownd',

	kAudioPlugInPropertyBoxList = 'box#', kAudioPlugInPropertyTranslateUIDToBox = 'uidb',
	kAudioPlugInPropertyDeviceList = 'dev#', kAudioPlugInPropertyTranslateUIDToDevice = 'uidd',
	kAudioPlugInPropertyResourceBundle = 'rsrc',

	kAudioDevicePropertyDeviceUID = 'uid ', kAudioDevicePropertyModelUID = 'muid', kAudioDevicePropertyTransportType = 'tran',
	kAudioDevicePropertyRelatedDevices = 'akin', kAudioDevicePropertyClockDomain = 'clkd',
	kAudioDevicePropertyDeviceIsAlive = 'livn', kAudioDevicePropertyDeviceIsRunning = 'goin',
	kAudioDevicePropertyDeviceIsRunningSomewhere = 'gone', kAudioDevicePropertyHogMode = 'oink',
	kAudioDevicePropertyDeviceCanBeDefaultDevice = 'dflt', kAudioDevicePropertyDeviceCanBeDefaultSystemDevice = 'sflt',
	kAudioDevicePropertyLatency = 'ltnc', kAudioDevicePropertyStreams = 'stm#', kAudioObjectPropertyControlList = 'ctrl',
	kAudioDevicePropertySafetyOffset = 'saft', kAudioDevicePropertyNominalSampleRate = 'nsrt',
	kAudioDevicePropertyAvailableNominalSampleRates = 'nsr#', kAudioDevicePropertyIsHidden = 'hidn',
	kAudioDevicePropertyPreferredChannelsForStereo = 'dch2', kAudioDevicePropertyZeroTimeStampPeriod = 'ring',
	kAudioSelectorControlClassID = 'slct', kAudioDataSourceControlClassID = 'dsrc',
	kAudioSelectorControlPropertyCurrentItem = 'scci', kAudioSelectorControlPropertyAvailableItems = 'scai',
	kAudioSelectorControlPropertyItemName = 'scin', kAudioSelectorControlPropertyItemKind = 'clkk',

	kAudioStreamPropertyIsActive = 'sact', kAudioStreamPropertyDirection = 'sdir', kAudioStreamPropertyTerminalType = 'term',
	kAudioStreamPropertyStartingChannel = 'schn', kAudioStreamPropertyLatency = 'ltnc',
	kAudioStreamPropertyVirtualFormat = 'sfmt', kAudioStreamPropertyAvailableVirtualFormats = 'sfma',
	kAudioStreamPropertyPhysicalFormat = 'pft ', kAudioStreamPropertyAvailablePhysicalFormats = 'pfta',

	kAudioStreamTerminalTypeSpeaker = 'spkr', kAudioStreamTerminalTypeMicrophone = 'micr',

	kAudioServerPlugInIOOperationReadInput = 'read', kAudioServerPlugInIOOperationWriteMix = 'rite',

	kAudioLevelControlClassID = 'levl', kAudioVolumeControlClassID = 'vlme',
	kAudioControlPropertyScope = 'cscp', kAudioControlPropertyElement = 'celm',
	kAudioLevelControlPropertyScalarValue = 'lcsv', kAudioLevelControlPropertyDecibelValue = 'lcdv',
	kAudioLevelControlPropertyDecibelRange = 'lcdr', kAudioLevelControlPropertyConvertScalarToDecibels = 'lcsd',
	kAudioLevelControlPropertyConvertDecibelsToScalar = 'lcds',

	kAudioBooleanControlClassID = 'togl', kAudioMuteControlClassID = 'mute', kAudioBooleanControlPropertyValue = 'bcvl',
	kAudioDevicePropertyJackIsConnected = 'jack', kAudioStreamTerminalTypeHeadphones = 'hdph',
};

/* ---- the devices ---------------------------------------------------------------------------------- */

/*
 * Object numbers are ours to choose. A device's streams follow it: its output (if it has one),
 * then its input.
 */
enum {
	OBJECT_PLUGIN = 1,
	OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, OBJECT_SPEAKER_INPUT,
	OBJECT_HAWKING, OBJECT_HAWKING_INPUT,
	OBJECT_ACTUATOR, OBJECT_ACTUATOR_OUTPUT,
	OBJECT_CODEC, OBJECT_CODEC_INPUT,
	OBJECT_SPEAKER_VOLUME,          /* the speaker output's volume control */
	OBJECT_CODEC_SOURCE,            /* the codec input's data source control: its microphones */
	OBJECT_HEADPHONES, OBJECT_HEADPHONES_OUTPUT,    /* the 3.5 mm jack, while a plug is in (boot-arg hp_usb=1) */
	OBJECT_HEADPHONES_VOLUME, OBJECT_HEADPHONES_MUTE,
	OBJECT_LAST = OBJECT_HEADPHONES_MUTE
};

#define VOLUME_RANGE_DB         48.0    /* from the loudest the kernel allows down to the quietest step above 0 */

struct device {
	AudioObjectID object;
	CFStringRef uid;                /* the name the virtual audio device knows it by */
	AudioObjectID output, input;    /* its streams; 0 = none */
	UInt32 output_channels, input_channels;
	/* its clock, running while anybody does I/O */
	unsigned running;               /* how many clients have started I/O */
	UInt64 anchor;                  /* mach_absolute_time() when the first of them did */
	UInt64 periods;                 /* whole timestamp periods since */
	Float64 rate;                   /* its sample rate: HAKIOS_AUDIO_RATE, or SPEAKER_RATE_44 for the speaker */
};

static struct device devices[] = {
	{ .object = OBJECT_SPEAKER, .uid = CFSTR("Speaker"), .output = OBJECT_SPEAKER_OUTPUT, .input = OBJECT_SPEAKER_INPUT,
	  .output_channels = HAKIOS_AUDIO_CHANNELS, .input_channels = 2, .rate = HAKIOS_AUDIO_RATE },
	{ .object = OBJECT_HAWKING, .uid = CFSTR("Hawking"), .input = OBJECT_HAWKING_INPUT, .input_channels = 1,
	  .rate = HAKIOS_AUDIO_RATE },
	{ .object = OBJECT_ACTUATOR, .uid = CFSTR("Actuator"), .output = OBJECT_ACTUATOR_OUTPUT, .output_channels = 1,
	  .rate = HAKIOS_AUDIO_RATE },
	{ .object = OBJECT_CODEC, .uid = CFSTR("Codec"), .input = OBJECT_CODEC_INPUT, .input_channels = 1,
	  .rate = HAKIOS_AUDIO_RATE },
	{ .object = OBJECT_HEADPHONES, .uid = CFSTR("Headphones"), .output = OBJECT_HEADPHONES_OUTPUT,
	  .output_channels = HAKIOS_AUDIO_CHANNELS, .rate = HAKIOS_AUDIO_RATE },
};
#define DEVICES                 (sizeof(devices) / sizeof(devices[0]))

#define TRANSPORT_BUILT_IN      'bltn'
/*
 * The 3.5 mm jack (the bring-up notes). The iPhone X's VirtualAudio makes a wired-headphones
 * port for its Codec only from the iPhone X's routing database, which has none; it makes one for any USB audio
 * device that has an output volume and mute control (Device_USB_Aspen). So the jack is a device of that kind,
 * "Headphones", listed only while the kernel says a plug is in (kern.vayu_jack >= 1, jack3), polled here. Its samples
 * go to /dev/hakios-audio like the speaker's; the kernel moves them to the jack by itself (jack4). Off unless
 * boot-arg hp_usb=1; hp_codecjack=1 also has the Codec say 'jack' (the check that VirtualAudio ignores it there).
 */
#define TRANSPORT_USB           'usb '
static Boolean hp_enabled, hp_codec_jack;       /* boot-args hp_usb=1, hp_codecjack=1 */
static volatile Boolean hp_present;             /* a plug is in: the device is listed */
static Float32 hp_volume = 1.0f;                /* its volume control, 0 to 1: a gain on its samples */
static Boolean hp_muted;
static int16_t hp_scaled[16384 * HAKIOS_AUDIO_CHANNELS];
/*
 * The codec's input has data sources, one for each microphone, and the virtual audio device will not
 * take a codec without them (VirtualAudio asks for kAudioDevicePropertyDataSources on its input, and
 * keeps those it knows: 'imic', 'smic', 'fmic', 'lmic', 'tmic', 'umic', 'imi1' to 'imi4'). A plug-in
 * has them as a data source control on the input, which the HAL answers those properties from. There
 * is one microphone here, at the bottom: 'imic', the iPhone's internal (bottom) one.
 */
#define MIC_SOURCE              'imic'
#define MANUFACTURER            CFSTR("redwood")
/* how often a device says where its clock is, in frames: also the largest I/O buffer the HAL will use */
#define TIMESTAMP_PERIOD        16384

static AudioServerPlugInHostRef host;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int sink = -1;                   /* /dev/hakios-audio */
static Float64 ticks_per_second;        /* mach_absolute_time() ticks in a second */
/*
 * The speaker also runs at 44.1 kHz, converted here to the kernel's 48 kHz. On vayu (runs 194827, 001804,
 * 022501) every 44.1 kHz source -- an MP3 in Filza, the ringtones -- made the virtual audio device ask for 44100
 * and, refused ('!dat': this device offered 48000 alone), its route was torn down and almost nothing was played;
 * a 48 kHz WAV played through. Now the device offers both, and at 44100 its samples are resampled (linear
 * interpolation, which is enough for a phone's speaker) before they go to /dev/hakios-audio.
 */
#define SPEAKER_RATE_44         44100.0
static int16_t resample_prev[HAKIOS_AUDIO_CHANNELS];
static Float64 resample_pos;            /* where the next output frame lies, in input frames from the frame before the buffer */
static int16_t resampled[(TIMESTAMP_PERIOD * 48000 / 44100 + 4) * HAKIOS_AUDIO_CHANNELS];
static UInt64 written, refused;         /* frames since the speaker's I/O started */
static Boolean inactive[OBJECT_LAST + 1];       /* streams the HAL has switched off */
static Float32 volume = 1.0f;           /* the speaker's volume control, 0 to 1 (iOS sets it once it routes there) */

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

/* The volume control's scale: a straight line of decibels over VOLUME_RANGE_DB. */
static Float32
scalar_to_db(Float32 scalar)
{
	scalar = scalar < 0 ? 0 : scalar > 1 ? 1 : scalar;
	return (Float32)(-VOLUME_RANGE_DB * (1.0 - scalar));
}

static Float32
db_to_scalar(Float32 db)
{
	Float32 scalar = (Float32)(1.0 + db / VOLUME_RANGE_DB);

	return scalar < 0 ? 0 : scalar > 1 ? 1 : scalar;
}

/* The volume, to the kernel. With the lock held. */
static void
volume_to_kernel(void)
{
	struct hakios_audio_volume v = { (uint32_t)lround(-scalar_to_db(volume) * 8), volume <= 0 };

	if (sink < 0 || ioctl(sink, HAKIOS_AUDIO_SET_VOLUME, &v) < 0) {
		say("hakios: audio: the volume %.3f could not be given to the kernel (%s)\n", volume, strerror(errno));
		return;
	}
	say("hakios: audio: the volume is %.3f: %s%.1f dB under the loudest\n", volume, v.muted ? "silence, " : "",
	    v.below / 8.0);
}

/* The device with this object number, or the one this stream belongs to; NULL = neither. */
static struct device *
device_of(AudioObjectID object)
{
	for (unsigned i = 0; i < DEVICES; i++) {
		if (object == devices[i].object || (object != 0 && (object == devices[i].output || object == devices[i].input))) {
			return &devices[i];
		}
	}
	return NULL;
}

/* The one format of a stream: 16-bit samples at HAKIOS_AUDIO_RATE, so many channels. */
static AudioStreamBasicDescription
format_of(AudioObjectID stream)
{
	const struct device *d = device_of(stream);
	UInt32 channels = stream == d->output ? d->output_channels : d->input_channels;

	return (AudioStreamBasicDescription) {
		       .mSampleRate = d->rate, .mFormatID = kAudioFormatLinearPCM,
		       .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
		       .mBytesPerPacket = 2 * channels, .mFramesPerPacket = 1, .mBytesPerFrame = 2 * channels,
		       .mChannelsPerFrame = channels, .mBitsPerChannel = 16,
	};
}

static HRESULT
query_interface(void *driver, REFIID uuid, LPVOID *interface)
{
	CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
	Boolean ours = CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);

	CFRelease(requested);
	if (interface == NULL || !ours) {
		return E_NOINTERFACE;
	}
	*interface = driver;            /* one driver, never freed: no counting */
	return S_OK;
}

static ULONG
add_ref(__unused void *driver)
{
	return 1;
}

static ULONG
release(__unused void *driver)
{
	return 1;
}

/*
 * The jack, every 250 ms: kern.vayu_jack (-1 driver off, 0 none, 1 headphones, 2 headset) and kern.vayu_jack_gen (up by
 * one at every event, so that a quick out-and-in between two looks is not missed). A change lists or unlists the device.
 */
static void *
hp_watch(__unused void *arg)
{
	int last_gen = -1;
	Boolean said_missing = false;

	pthread_setname_np("redwood jack");
	for (;;) {
		int jack = -1, gen = 0;
		size_t size = sizeof(jack);

		if (sysctlbyname("kern.vayu_jack", &jack, &size, NULL, 0) != 0) {
			if (!said_missing) {
				say("redwood: audio: no kern.vayu_jack (a kernel without jack3, or vayu_jack=3 not given): no headphones\n");
				said_missing = true;
			}
			jack = -1;
		}
		size = sizeof(gen);
		if (sysctlbyname("kern.vayu_jack_gen", &gen, &size, NULL, 0) != 0) {
			gen = 0;
		}
		if (gen != last_gen || (jack >= 1) != hp_present) {
			Boolean present = jack >= 1;
			AudioObjectPropertyAddress changed[2] = {
				{ kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, 0 },
				{ kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, 0 },
			};

			if (present != hp_present) {
				say("redwood: audio: jack: %s (kern.vayu_jack %d, event %d)\n", present ? "plugged: Headphones listed" :
				    "unplugged: Headphones gone", jack, gen);
				hp_present = present;
				if (host != NULL) {
					AudioObjectPropertyAddress jack_changed = { kAudioDevicePropertyJackIsConnected,
					    kAudioObjectPropertyScopeGlobal, 0 };

					host->PropertiesChanged(host, OBJECT_PLUGIN, 2, changed);
					if (hp_codec_jack) {
						host->PropertiesChanged(host, OBJECT_CODEC, 1, &jack_changed);
					}
				}
			}
			last_gen = gen;
		}
		usleep(250000);
	}
	return NULL;
}

static OSStatus
initialize(__unused AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef the_host)
{
	mach_timebase_info_data_t timebase;

	host = the_host;
	mach_timebase_info(&timebase);
	ticks_per_second = 1e9 * timebase.denom / timebase.numer;
	sink = open(HAKIOS_AUDIO_DEVICE, O_WRONLY);
	say("hakios: audio: the speaker is with the HAL (%s: %s)\n", HAKIOS_AUDIO_DEVICE, sink < 0 ? strerror(errno) : "open");
	{
		char arguments[4096] = "";
		size_t size = sizeof(arguments) - 1;
		pthread_t thread;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
			hp_enabled = strstr(arguments, "hp_usb=1") != NULL;
			hp_codec_jack = strstr(arguments, "hp_codecjack=1") != NULL;
		}
		say("hakios: audio: headphones as a USB device: %s%s\n", hp_enabled ? "on" : "off (hp_usb=1 to try)",
		    hp_codec_jack ? ", and 'jack' on the Codec" : "");
		if (hp_enabled && pthread_create(&thread, NULL, hp_watch, NULL) == 0) {
			pthread_detach(thread);
		}
	}
	return 0;
}

static OSStatus
create_device(__unused AudioServerPlugInDriverRef driver, __unused CFDictionaryRef description,
    __unused const AudioServerPlugInClientInfo *client, __unused AudioObjectID *object)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
destroy_device(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
device_client(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object,
    __unused const AudioServerPlugInClientInfo *client)
{
	return 0;
}

static OSStatus
configuration_change(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt64 action,
    __unused void *info)
{
	return 0;
}

/* The HAL's go for a change asked for in set_property_data(): the speaker's new rate (action, in Hz). */
static OSStatus
perform_configuration_change(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, UInt64 action,
    __unused void *info)
{
	struct device *d = device_of(object);

	if (d == NULL || object != OBJECT_SPEAKER || (action != 44100 && action != HAKIOS_AUDIO_RATE)) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	d->rate = (Float64)action;
	d->anchor = mach_absolute_time();
	d->periods = 0;
	memset(resample_prev, 0, sizeof(resample_prev));
	resample_pos = 0;
	pthread_mutex_unlock(&lock);
	say("redwood: audio: the speaker runs at %llu Hz now%s\n", action, action == HAKIOS_AUDIO_RATE ? "" :
	    " (resampled to 48000 for the kernel)");
	return 0;
}

/* Is this a rate the device can run at? */
static bool
rate_supported(const struct device *d, Float64 rate)
{
	return rate == HAKIOS_AUDIO_RATE || (d->object == OBJECT_SPEAKER && rate == SPEAKER_RATE_44);
}

/* Ask the HAL to change the speaker's rate (it calls perform_configuration_change() when it is safe). */
static OSStatus
request_rate(struct device *d, Float64 rate)
{
	if (rate == d->rate) {
		return 0;
	}
	if (host == NULL) {
		return kAudioHardwareUnsupportedOperationError;
	}
	return host->RequestDeviceConfigurationChange(host, d->object, (UInt64)rate, NULL);
}

/* A property's value: `size` bytes of it, or a list with nothing in it, or (neither) no such property. */
struct value {
	UInt32 size;
	bool empty;
	bool string;                    /* a CFString: whoever is given it gets a reference of their own */
	union {
		UInt32 words[4];
		Float64 number;
		Float32 decimal;
		CFStringRef string;
		AudioValueRange range;
		AudioStreamBasicDescription format;
		AudioStreamRangedDescription ranged;
		AudioValueRange ranges[2];
		AudioStreamRangedDescription rangeds[2];
	} u;
};

static struct value
property(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier)
{
	UInt32 selector = address->mSelector, scope = address->mScope;
	const struct device *d = device_of(object);
	struct value v;

	memset(&v, 0, sizeof(v));
#define WORD(x)         (v.u.words[0] = (x), v.size = sizeof(UInt32))
#define LIST(x)         (v.u.words[v.size / sizeof(UInt32)] = (x), v.size += sizeof(UInt32))
#define STRING(x)       (v.u.string = (x), v.string = true, v.size = sizeof(CFStringRef))
#define FLOAT(x)        (v.u.decimal = (x), v.size = sizeof(Float32))
	if (object == OBJECT_SPEAKER_VOLUME) {
		switch (selector) {
		case kAudioObjectPropertyBaseClass:
			WORD(kAudioLevelControlClassID);
			break;
		case kAudioObjectPropertyClass:
			WORD(kAudioVolumeControlClassID);
			break;
		case kAudioObjectPropertyOwner:
			WORD(OBJECT_SPEAKER);
			break;
		case kAudioObjectPropertyOwnedObjects:
			v.empty = true;
			break;
		case kAudioControlPropertyScope:
			WORD(kAudioObjectPropertyScopeOutput);
			break;
		case kAudioControlPropertyElement:
			WORD(0);
			break;
		case kAudioLevelControlPropertyScalarValue:
			FLOAT(volume);
			break;
		case kAudioLevelControlPropertyDecibelValue:
			FLOAT(scalar_to_db(volume));
			break;
		case kAudioLevelControlPropertyDecibelRange:
			v.u.range.mMinimum = -VOLUME_RANGE_DB;
			v.u.range.mMaximum = 0;
			v.size = sizeof(AudioValueRange);
			break;
		case kAudioLevelControlPropertyConvertScalarToDecibels:
		case kAudioLevelControlPropertyConvertDecibelsToScalar:
			FLOAT(0);       /* (converts what it is given: get_property_data()) */
			break;
		}
		return v;
	}
	if (object == OBJECT_HEADPHONES_VOLUME || object == OBJECT_HEADPHONES_MUTE) {
		Boolean mute = object == OBJECT_HEADPHONES_MUTE;

		switch (selector) {
		case kAudioObjectPropertyBaseClass:
			WORD(mute ? kAudioBooleanControlClassID : kAudioLevelControlClassID);
			break;
		case kAudioObjectPropertyClass:
			WORD(mute ? kAudioMuteControlClassID : kAudioVolumeControlClassID);
			break;
		case kAudioObjectPropertyOwner:
			WORD(OBJECT_HEADPHONES);
			break;
		case kAudioObjectPropertyOwnedObjects:
			v.empty = true;
			break;
		case kAudioControlPropertyScope:
			WORD(kAudioObjectPropertyScopeOutput);
			break;
		case kAudioControlPropertyElement:
			WORD(0);
			break;
		case kAudioBooleanControlPropertyValue:
			if (mute) {
				WORD(hp_muted);
			}
			break;
		case kAudioLevelControlPropertyScalarValue:
			if (!mute) {
				FLOAT(hp_volume);
			}
			break;
		case kAudioLevelControlPropertyDecibelValue:
			if (!mute) {
				FLOAT(scalar_to_db(hp_volume));
			}
			break;
		case kAudioLevelControlPropertyDecibelRange:
			if (!mute) {
				v.u.range.mMinimum = -VOLUME_RANGE_DB;
				v.u.range.mMaximum = 0;
				v.size = sizeof(AudioValueRange);
			}
			break;
		case kAudioLevelControlPropertyConvertScalarToDecibels:
		case kAudioLevelControlPropertyConvertDecibelsToScalar:
			if (!mute) {
				FLOAT(0);
			}
			break;
		}
		return v;
	}
	if (object == OBJECT_CODEC_SOURCE) {
		switch (selector) {
		case kAudioObjectPropertyBaseClass:
			WORD(kAudioSelectorControlClassID);
			break;
		case kAudioObjectPropertyClass:
			WORD(kAudioDataSourceControlClassID);
			break;
		case kAudioObjectPropertyOwner:
			WORD(OBJECT_CODEC);
			break;
		case kAudioObjectPropertyOwnedObjects:
			v.empty = true;
			break;
		case kAudioControlPropertyScope:
			WORD(kAudioObjectPropertyScopeInput);
			break;
		case kAudioControlPropertyElement:
			WORD(0);
			break;
		case kAudioSelectorControlPropertyCurrentItem:
		case kAudioSelectorControlPropertyAvailableItems:
			WORD(MIC_SOURCE);
			break;
		case kAudioSelectorControlPropertyItemName:
			if (qualifier_size == sizeof(UInt32) && qualifier != NULL && *(const UInt32 *)qualifier == MIC_SOURCE) {
				STRING(CFSTR("Bottom"));
			} else {
				STRING(CFSTR(""));
			}
			break;
		}
		return v;
	}
	if (object != OBJECT_PLUGIN && d == NULL) {
		return v;
	}
	switch (selector) {
	case kAudioObjectPropertyBaseClass:
		WORD(kAudioObjectClassID);
		return v;
	case kAudioObjectPropertyClass:
		WORD(object == OBJECT_PLUGIN ? kAudioPlugInClassID : object == d->object ? kAudioDeviceClassID : kAudioStreamClassID);
		return v;
	case kAudioObjectPropertyOwner:
		WORD(object == OBJECT_PLUGIN ? 0 : object == d->object ? OBJECT_PLUGIN : d->object);
		return v;
	}
	if (object == OBJECT_PLUGIN) {
		switch (selector) {
		case kAudioObjectPropertyManufacturer:
			STRING(MANUFACTURER);
			break;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList:
			for (unsigned i = 0; i < DEVICES; i++) {
				if (devices[i].object != OBJECT_HEADPHONES || hp_present) {
					LIST(devices[i].object);
				}
			}
			break;
		case kAudioPlugInPropertyBoxList:
			v.empty = true;
			break;
		case kAudioPlugInPropertyTranslateUIDToBox:
			WORD(0);
			break;
		case kAudioPlugInPropertyTranslateUIDToDevice:
			WORD(0);
			for (unsigned i = 0; i < DEVICES; i++) {
				if (qualifier_size == sizeof(CFStringRef) && qualifier != NULL && *(const CFStringRef *)qualifier != NULL &&
				    CFEqual(*(const CFStringRef *)qualifier, devices[i].uid) &&
				    (devices[i].object != OBJECT_HEADPHONES || hp_present)) {
					WORD(devices[i].object);
				}
			}
			break;
		case kAudioPlugInPropertyResourceBundle:
			STRING(CFSTR(""));
			break;
		}
	} else if (object == d->object) {
		switch (selector) {
		case kAudioObjectPropertyManufacturer:
			STRING(MANUFACTURER);
			break;
		case kAudioObjectPropertyName:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
			STRING(d->uid);
			break;
		case kAudioDevicePropertyTransportType:
			WORD(d->object == OBJECT_HEADPHONES ? TRANSPORT_USB : TRANSPORT_BUILT_IN);
			break;
		case kAudioDevicePropertyJackIsConnected:
			/* the jack: always plugged while listed; the Codec only with hp_codecjack=1 (the check) */
			if (d->object == OBJECT_HEADPHONES) {
				WORD(1);
			} else if (d->object == OBJECT_CODEC && hp_codec_jack) {
				WORD(hp_present);
			}
			break;
		case kAudioDevicePropertyRelatedDevices:
			WORD(d->object);
			break;
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyIsHidden:
			WORD(0);
			break;
		case kAudioDevicePropertyDeviceIsAlive:
			WORD(1);
			break;
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
			/*
			 * By scope, as an input-only device needs (R10): the Codec has no output, and
			 * answering the output's "no" for every scope (this said `d->output != 0`) is
			 * what a device the audio server will not take as the microphone looks like.
			 */
			WORD(scope == kAudioObjectPropertyScopeInput ? d->input != 0 :
			    scope == kAudioObjectPropertyScopeOutput ? d->output != 0 : d->input != 0 || d->output != 0);
			break;
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			/* the system's own sounds are played: an input alone is never that */
			WORD(d->output != 0);
			break;
		case kAudioDevicePropertyDeviceIsRunning:
			WORD(d->running != 0);
			break;
		case kAudioDevicePropertyDeviceIsRunningSomewhere:
			/* the Codec is watched by the HAL for a while after its I/O stops (R10) */
			WORD(d->running != 0);
			break;
		case kAudioDevicePropertyHogMode:
			/* nobody has the device to themselves */
			WORD((UInt32)-1);
			break;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyStreams:
			/* the streams of one direction, or of both (and the speaker's volume control, which it owns) */
			if (scope != kAudioObjectPropertyScopeInput && d->output != 0) {
				LIST(d->output);
			}
			if (scope != kAudioObjectPropertyScopeOutput && d->input != 0) {
				LIST(d->input);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_SPEAKER) {
				LIST(OBJECT_SPEAKER_VOLUME);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_CODEC && scope != kAudioObjectPropertyScopeOutput) {
				LIST(OBJECT_CODEC_SOURCE);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_HEADPHONES && scope != kAudioObjectPropertyScopeInput) {
				LIST(OBJECT_HEADPHONES_VOLUME);
				LIST(OBJECT_HEADPHONES_MUTE);
			}
			v.empty = v.size == 0;
			break;
		case kAudioObjectPropertyControlList:
			if (d->object == OBJECT_SPEAKER) {
				LIST(OBJECT_SPEAKER_VOLUME);
			}
			if (d->object == OBJECT_CODEC) {
				LIST(OBJECT_CODEC_SOURCE);
			}
			if (d->object == OBJECT_HEADPHONES) {
				LIST(OBJECT_HEADPHONES_VOLUME);
				LIST(OBJECT_HEADPHONES_MUTE);
			}
			v.empty = v.size == 0;
			break;
		case kAudioDevicePropertyNominalSampleRate:
			v.u.number = d->rate;
			v.size = sizeof(Float64);
			break;
		case kAudioDevicePropertyAvailableNominalSampleRates:
			v.u.ranges[0].mMinimum = v.u.ranges[0].mMaximum = HAKIOS_AUDIO_RATE;
			v.size = sizeof(AudioValueRange);
			if (d->object == OBJECT_SPEAKER) {
				v.u.ranges[1].mMinimum = v.u.ranges[1].mMaximum = SPEAKER_RATE_44;
				v.size = 2 * sizeof(AudioValueRange);
			}
			break;
		case kAudioDevicePropertyPreferredChannelsForStereo:
			v.u.words[0] = 1;
			v.u.words[1] = 2;
			v.size = 2 * sizeof(UInt32);
			break;
		case kAudioDevicePropertyZeroTimeStampPeriod:
			WORD(TIMESTAMP_PERIOD);
			break;
		}
	} else {
		switch (selector) {
		case kAudioStreamPropertyIsActive:
			WORD(!inactive[object]);
			break;
		case kAudioStreamPropertyDirection:
			WORD(object == d->input);
			break;
		case kAudioStreamPropertyTerminalType:
			WORD(object == d->input ? kAudioStreamTerminalTypeMicrophone :
			    d->object == OBJECT_HEADPHONES ? kAudioStreamTerminalTypeHeadphones : kAudioStreamTerminalTypeSpeaker);
			break;
		case kAudioStreamPropertyStartingChannel:
			WORD(1);
			break;
		case kAudioStreamPropertyLatency:
			WORD(0);
			break;
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			v.u.format = format_of(object);
			v.size = sizeof(AudioStreamBasicDescription);
			break;
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			v.u.rangeds[0].mFormat = format_of(object);
			v.u.rangeds[0].mFormat.mSampleRate = HAKIOS_AUDIO_RATE;
			v.u.rangeds[0].mSampleRateRange.mMinimum = v.u.rangeds[0].mSampleRateRange.mMaximum = HAKIOS_AUDIO_RATE;
			v.size = sizeof(AudioStreamRangedDescription);
			if (d->object == OBJECT_SPEAKER) {
				v.u.rangeds[1] = v.u.rangeds[0];
				v.u.rangeds[1].mFormat.mSampleRate = SPEAKER_RATE_44;
				v.u.rangeds[1].mSampleRateRange.mMinimum = v.u.rangeds[1].mSampleRateRange.mMaximum = SPEAKER_RATE_44;
				v.size = 2 * sizeof(AudioStreamRangedDescription);
			}
			break;
		}
	}
#undef WORD
#undef LIST
#undef STRING
#undef FLOAT
	return v;
}

/*
 * What the HAL (or the virtual audio device through it) asks of an object that it does not have,
 * the first times for each object, selector and scope: what a device it will not take may lack.
 */
#define UNKNOWN_TOLD            64
static UInt32 unknown_told[UNKNOWN_TOLD][3];
static unsigned unknown_count;

static void
unknown(AudioObjectID object, const AudioObjectPropertyAddress *address)
{
	UInt32 s = address->mSelector, c = address->mScope;

	for (unsigned i = 0; i < unknown_count; i++) {
		if (unknown_told[i][0] == object && unknown_told[i][1] == s && unknown_told[i][2] == c) {
			return;
		}
	}
	if (unknown_count >= UNKNOWN_TOLD) {
		return;
	}
	unknown_told[unknown_count][0] = object;
	unknown_told[unknown_count][1] = s;
	unknown_told[unknown_count][2] = c;
	unknown_count++;
	say("hakios: audio: asked of object %u (%s) what it does not have: '%c%c%c%c' (%#x), scope '%c%c%c%c', element %u\n",
	    (unsigned)object, device_of(object) ? CFStringGetCStringPtr(device_of(object)->uid, kCFStringEncodingUTF8) : "-",
	    (int)(s >> 24 & 0xff), (int)(s >> 16 & 0xff), (int)(s >> 8 & 0xff), (int)(s & 0xff), (unsigned)s,
	    (int)(c >> 24 & 0xff), (int)(c >> 16 & 0xff), (int)(c >> 8 & 0xff), (int)(c & 0xff), (unsigned)address->mElement);
}

static Boolean
has_property(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused pid_t client,
    const AudioObjectPropertyAddress *address)
{
	struct value v;

	if (address == NULL) {
		return false;
	}
	v = property(object, address, 0, NULL);
	if (v.size == 0 && !v.empty) {
		unknown(object, address);
	}
	return v.size != 0 || v.empty;
}

static OSStatus
is_property_settable(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, Boolean *settable)
{
	const struct device *d = device_of(object);

	if (address == NULL || settable == NULL || !has_property(driver, object, client, address)) {
		return kAudioHardwareUnknownPropertyError;
	}
	/* the rate and the formats can be "set", to the ones there are; the volume can be set */
	if (object == OBJECT_SPEAKER_VOLUME) {
		*settable = address->mSelector == kAudioLevelControlPropertyScalarValue ||
		    address->mSelector == kAudioLevelControlPropertyDecibelValue;
	} else if (object == OBJECT_HEADPHONES_VOLUME) {
		*settable = address->mSelector == kAudioLevelControlPropertyScalarValue ||
		    address->mSelector == kAudioLevelControlPropertyDecibelValue;
	} else if (object == OBJECT_HEADPHONES_MUTE) {
		*settable = address->mSelector == kAudioBooleanControlPropertyValue;
	} else if (object == OBJECT_CODEC_SOURCE) {
		*settable = address->mSelector == kAudioSelectorControlPropertyCurrentItem;
	} else if (object == OBJECT_PLUGIN) {
		*settable = false;
	} else if (object == d->object) {
		*settable = address->mSelector == kAudioDevicePropertyNominalSampleRate;
	} else {
		*settable = address->mSelector == kAudioStreamPropertyIsActive ||
		    address->mSelector == kAudioStreamPropertyVirtualFormat || address->mSelector == kAudioStreamPropertyPhysicalFormat;
	}
	return 0;
}

static OSStatus
get_property_data_size(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 *size)
{
	if (address == NULL || size == NULL || !has_property(driver, object, client, address)) {
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ? kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	*size = property(object, address, qualifier_size, qualifier).size;
	return 0;
}

static OSStatus
get_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 room, UInt32 *size,
    void *data)
{
	struct value v;

	if (address == NULL || size == NULL || data == NULL || !has_property(driver, object, client, address)) {
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ? kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	if ((object == OBJECT_SPEAKER_VOLUME || object == OBJECT_HEADPHONES_VOLUME) &&
	    (address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ||
	    address->mSelector == kAudioLevelControlPropertyConvertDecibelsToScalar)) {
		/* what is in the buffer, converted where it is */
		if (room < sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(Float32 *)data = address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ?
		    scalar_to_db(*(Float32 *)data) : db_to_scalar(*(Float32 *)data);
		*size = sizeof(Float32);
		return 0;
	}
	pthread_mutex_lock(&lock);
	v = property(object, address, qualifier_size, qualifier);
	pthread_mutex_unlock(&lock);
	if (v.size > room) {
		/* a list may be asked for in part; anything else must fit */
		if (address->mSelector != kAudioObjectPropertyOwnedObjects && address->mSelector != kAudioDevicePropertyStreams &&
		    address->mSelector != kAudioPlugInPropertyDeviceList) {
			return kAudioHardwareBadPropertySizeError;
		}
		v.size = room - room % sizeof(UInt32);
	}
	memcpy(data, &v.u, v.size);
	if (v.string) {
		CFRetain(v.u.string);
	}
	*size = v.size;
	return 0;
}

static OSStatus
set_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, __unused UInt32 qualifier_size, __unused const void *qualifier, UInt32 size,
    const void *data)
{
	Boolean settable = false;

	if (address == NULL || data == NULL || is_property_settable(driver, object, client, address, &settable) != 0) {
		return kAudioHardwareUnknownPropertyError;
	}
	if (!settable) {
		return kAudioHardwareIllegalOperationError;
	}
	if (object == OBJECT_SPEAKER_VOLUME) {
		AudioObjectPropertyAddress changed[2] = {
			{ kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0 },
			{ kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0 },
		};
		Float32 wanted;

		if (size != sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		wanted = *(const Float32 *)data;
		wanted = address->mSelector == kAudioLevelControlPropertyDecibelValue ? db_to_scalar(wanted) :
		    wanted < 0 ? 0 : wanted > 1 ? 1 : wanted;
		pthread_mutex_lock(&lock);
		volume = wanted;
		volume_to_kernel();
		pthread_mutex_unlock(&lock);
		if (host != NULL) {
			host->PropertiesChanged(host, OBJECT_SPEAKER_VOLUME, 2, changed);
		}
		return 0;
	}
	if (object == OBJECT_HEADPHONES_VOLUME || object == OBJECT_HEADPHONES_MUTE) {
		AudioObjectPropertyAddress changed[2] = {
			{ object == OBJECT_HEADPHONES_MUTE ? kAudioBooleanControlPropertyValue : kAudioLevelControlPropertyScalarValue,
			  kAudioObjectPropertyScopeGlobal, 0 },
			{ kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0 },
		};

		if (object == OBJECT_HEADPHONES_MUTE ? size != sizeof(UInt32) : size != sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		pthread_mutex_lock(&lock);
		if (object == OBJECT_HEADPHONES_MUTE) {
			hp_muted = *(const UInt32 *)data != 0;
		} else {
			Float32 wanted = *(const Float32 *)data;

			hp_volume = address->mSelector == kAudioLevelControlPropertyDecibelValue ? db_to_scalar(wanted) :
			    wanted < 0 ? 0 : wanted > 1 ? 1 : wanted;
		}
		say("redwood: audio: headphones volume %.3f%s\n", hp_volume, hp_muted ? ", muted" : "");
		pthread_mutex_unlock(&lock);
		if (host != NULL) {
			host->PropertiesChanged(host, object, object == OBJECT_HEADPHONES_MUTE ? 1 : 2, changed);
		}
		return 0;
	}
	if (object == OBJECT_CODEC_SOURCE) {
		/* the one microphone there is */
		return size == sizeof(UInt32) ? (*(const UInt32 *)data == MIC_SOURCE ? 0 : kAudioHardwareIllegalOperationError) :
		       kAudioHardwareBadPropertySizeError;
	}
	if (address->mSelector == kAudioStreamPropertyIsActive) {
		if (size != sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		pthread_mutex_lock(&lock);
		inactive[object] = *(const UInt32 *)data == 0;
		pthread_mutex_unlock(&lock);
		/* the microphone's stream, on or off: the run's record of whether iOS takes the input (R10) */
		if (device_of(object) != NULL && device_of(object)->input == object) {
			say("hakios: audio: the microphone's stream is %s\n", *(const UInt32 *)data != 0 ? "on" : "off");
		}
		return 0;
	}
	if (address->mSelector == kAudioDevicePropertyNominalSampleRate) {
		/*
		 * Only HAKIOS_AUDIO_RATE. Anything else is answered "done" and left as it is, with no change
		 * announced: the virtual audio device then reads the rate back and converts to it itself. Refusing it
		 * ('!dat') was what stopped the sound on the phone (vayu, 24 October, run 194827): a ringtone asked its
		 * route for 44100, the refusal tore the route down and nothing was written again until a restart.
		 */
		if (size != sizeof(Float64)) {
			return kAudioHardwareBadPropertySizeError;
		}
		if (rate_supported(device_of(object), *(const Float64 *)data)) {
			return request_rate(device_of(object), *(const Float64 *)data);
		}
		say("redwood: audio: asked for %.0f Hz, keeps %.0f\n", *(const Float64 *)data, device_of(object)->rate);
		return 0;
	}
	/* a format: the one we have, or nothing */
	if (size != sizeof(AudioStreamBasicDescription)) {
		return kAudioHardwareBadPropertySizeError;
	} else {
		const AudioStreamBasicDescription *wanted = data, format = format_of(object);

		if (wanted->mFormatID != format.mFormatID || wanted->mChannelsPerFrame != format.mChannelsPerFrame ||
		    wanted->mBitsPerChannel != format.mBitsPerChannel || !(wanted->mFormatFlags & kAudioFormatFlagIsSignedInteger)) {
			say("redwood: audio: format refused on object %u: %.0f Hz, %u channels, %u bits, flags 0x%x\n", object,
			    wanted->mSampleRate, wanted->mChannelsPerFrame, wanted->mBitsPerChannel, wanted->mFormatFlags);
			return kAudioDeviceUnsupportedFormatError;
		}
		/* another rate: the device's, if it has it, else kept as it is like the nominal rate above */
		if (wanted->mSampleRate != format.mSampleRate && rate_supported(device_of(object), wanted->mSampleRate)) {
			return request_rate(device_of(object), wanted->mSampleRate);
		}
		if (wanted->mSampleRate != format.mSampleRate) {
			say("redwood: audio: format at %.0f Hz asked for on object %u, keeps %.0f\n", wanted->mSampleRate, object,
			    format.mSampleRate);
		}
		return 0;
	}
}

/*
 * The microphone (the device "Codec"): /dev/hakios-mic is open while the input runs, and the kernel
 * listens only while it is open. A read gives what has come and never waits; what it does not
 * give is silence. Said when it starts and stops, with how much came.
 */
static int mic = -1;
static UInt64 mic_frames, mic_heard, mic_short;
static int mic_peak;
static time_t mic_retry_at;
static int mic_retry_said;
static UInt64 mic_zero_reads, mic_zero_frames;   /* reads the kernel filled with exact zeros, and their frames */
static UInt64 mic_empty_reads;                  /* reads that gave nothing at all */
static time_t mic_empty_since;
static unsigned mic_reopens;

static void
codec_open(void)
{
	mic = open("/dev/hakios-mic", O_RDONLY | O_NONBLOCK);
	mic_frames = mic_heard = mic_short = 0;
	mic_peak = 0;
	mic_zero_reads = mic_zero_frames = mic_empty_reads = 0;
	mic_empty_since = 0;
	mic_reopens = 0;
	say("hakios: audio: the microphone is listened to (/dev/hakios-mic: %s)\n", mic < 0 ? strerror(errno) : "open");
	/* what iOS is offered as its built-in microphone (R10), for the record of a run */
	say("hakios: audio: the microphone's input: built in, 1 channel, %d Hz, 16-bit signed (the HAL's \"Codec\")\n",
	    HAKIOS_AUDIO_RATE);
}

static void
codec_close(void)
{
	say("hakios: audio: the microphone stops: %llu.%02llu s asked for, %llu.%02llu s of it from the kernel (peak %d of 32768), "
	    "%llu reads short, %llu of them with exact zeros (%llu.%02llu s), %llu with nothing at all\n",
	    mic_frames / HAKIOS_AUDIO_RATE, mic_frames % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE,
	    mic_heard / HAKIOS_AUDIO_RATE, mic_heard % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE, mic_peak, mic_short,
	    mic_zero_reads, mic_zero_frames / HAKIOS_AUDIO_RATE, mic_zero_frames % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE,
	    mic_empty_reads);
	if (mic >= 0) {
		close(mic);
	}
	mic = -1;
	mic_retry_at = 0;
	mic_retry_said = 0;
	mic_empty_since = 0;
}

static void
codec_read(int16_t *samples, UInt32 frames)
{
	/*
	 * The node is made at boot, but start_io() can run before it exists (or the open can fail
	 * once): the old code then gave zeros for the whole I/O cycle, because it retried only on
	 * the next StartIO. Try again once a second while the input is running; one failure line,
	 * not one a second.
	 */
	if (mic < 0) {
		time_t now = time(NULL);

		if (mic_retry_at == 0 || now - mic_retry_at >= 1) {
			mic_retry_at = now;
			mic = open("/dev/hakios-mic", O_RDONLY | O_NONBLOCK);
			if (mic >= 0) {
				say("hakios: audio: /dev/hakios-mic opened late: the input starts from here\n");
			} else if (!mic_retry_said) {
				mic_retry_said = 1;
				say("hakios: audio: /dev/hakios-mic is not open (%s): the input is silent until it is\n",
				    strerror(errno));
			}
		}
	}
	ssize_t got = mic >= 0 ? read(mic, samples, (size_t)frames * 2) : -1;
	UInt32 have = got > 0 ? (UInt32)got / 2 : 0;

	if (have > 0 && mic_heard == 0) {
		say("hakios: audio: the microphone's first samples, %llu.%02llu s after the input started\n",
		    mic_frames / HAKIOS_AUDIO_RATE, mic_frames % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE);
	}
	if (have == 0) {
		/*
		 * Nothing at all, not even the ring's silence: the kernel's capture is not feeding. Its node
		 * keeps the state it had, so closing and opening it is what makes the kernel take the capture's
		 * path down and up again (vayu_audio.c: ios_mic_close/ios_mic_open, ios_wants).
		 */
		mic_empty_reads++;
		if (mic >= 0 && mic_reopens < 3) {
			time_t now = time(NULL);

			if (mic_empty_since == 0) {
				mic_empty_since = now;
			} else if (now - mic_empty_since >= 1) {
				mic_reopens++;
				say("hakios: audio: /dev/hakios-mic gave nothing for a second: reopening it (the kernel's capture "
				    "path starts again; %u of 3)\n", mic_reopens);
				close(mic);
				mic = open("/dev/hakios-mic", O_RDONLY | O_NONBLOCK);
				mic_empty_since = 0;
				if (mic < 0) {
					say("hakios: audio: /dev/hakios-mic did not open again (%s)\n", strerror(errno));
				}
			}
		}
	} else {
		bool zero = true;

		for (UInt32 i = 0; i < have && zero; i++) {
			zero = samples[i] == 0;
		}
		mic_empty_since = 0;
		if (zero) {
			mic_zero_reads++;
			mic_zero_frames += have;
			if (mic_zero_reads == 1) {
				say("hakios: audio: the kernel's samples are exactly zero (%u at once): the DSP's capture is "
				    "silent, not the room\n", have);
			}
		}
	}
	for (UInt32 i = 0; i < have; i++) {
		int v = samples[i] < 0 ? -samples[i] : samples[i];

		mic_peak = v > mic_peak ? v : mic_peak;
	}
	if (have < frames) {
		memset(samples + have, 0, (size_t)(frames - have) * 2);
		mic_short += mic_heard > 0;
	}
	mic_frames += frames;
	mic_heard += have;
}

static OSStatus
start_io(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->running++ == 0) {
		d->anchor = mach_absolute_time();
		d->periods = 0;
		if (d->output != 0) {
			written = refused = 0;
			say("hakios: audio: playing\n");
		}
		if (d->object == OBJECT_CODEC) {
			codec_open();
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
stop_io(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->running != 0 && --d->running == 0) {
		if (d->object == OBJECT_CODEC) {
			codec_close();
		} else if (d->output != 0) {
			say("hakios: audio: stopped after %llu.%02llu s (%llu frames not taken by %s)\n", written / HAKIOS_AUDIO_RATE,
			    written % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE, refused, HAKIOS_AUDIO_DEVICE);
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

/* Where the device's clock was last at a multiple of TIMESTAMP_PERIOD frames, and when. */
static OSStatus
get_zero_time_stamp(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client,
    Float64 *sample_time, UInt64 *host_time, UInt64 *seed)
{
	UInt64 now = mach_absolute_time();
	struct device *d = device_of(object);
	Float64 ticks_per_period;

	if (d == NULL) {
		return kAudioHardwareBadObjectError;
	}
	ticks_per_period = ticks_per_second / d->rate * TIMESTAMP_PERIOD;
	pthread_mutex_lock(&lock);
	while (now >= d->anchor + (UInt64)((Float64)(d->periods + 1) * ticks_per_period)) {
		d->periods++;
	}
	*sample_time = (Float64)d->periods * TIMESTAMP_PERIOD;
	*host_time = d->anchor + (UInt64)((Float64)d->periods * ticks_per_period);
	*seed = 1;
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
will_do_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt32 client,
    UInt32 operation, Boolean *will_do, Boolean *in_place)
{
	*will_do = operation == kAudioServerPlugInIOOperationReadInput || operation == kAudioServerPlugInIOOperationWriteMix;
	*in_place = true;
	return 0;
}

static OSStatus
begin_end_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt32 client,
    __unused UInt32 operation, __unused UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle)
{
	return 0;
}

/*
 * What iOS plays on the actuator -- the haptics: the system's feedbacks and the ringer's vibration,
 * CoreHaptics' patterns, UIKit's feedbacks, as waveforms of one channel -- goes to the kernel's motor
 * driver (/dev/hakios-haptic, bsd/dev/hakios.haptic.c, haptic=5), which plays Android's own
 * waveforms on the phone's motor by it. (iOS plays on this device at all only because the device
 * tree says the phone's haptics are "closed-loop": see mkidentity.py.) Said the first times, with
 * how loud.
 */
static int haptic = -1;          /* the motor driver; -2: not there (the kernel runs without haptic=5) */
static unsigned haptic_said;

static void
actuator_write(const int16_t *samples, UInt32 frames)
{
	int peak = 0;

	for (UInt32 i = 0; i < frames; i++) {
		int v = samples[i] < 0 ? -samples[i] : samples[i];

		peak = v > peak ? v : peak;
	}
	if (haptic == -1) {
		haptic = open("/dev/hakios-haptic", O_WRONLY | O_NONBLOCK);
		haptic = haptic >= 0 ? haptic : -2;
	}
	if (haptic >= 0) {
		(void)write(haptic, samples, (size_t)frames * 2);
	}
	if (peak > 0 && haptic_said < 12) {
		haptic_said++;
		say("hakios: audio: the actuator plays: %u frames, peak %d of 32768%s\n", (unsigned)frames, peak,
		    haptic >= 0 ? "" : " (no /dev/hakios-haptic: not felt)");
	}
}

/* 44.1 kHz frames to 48 kHz, into resampled[]: linear interpolation, carried across buffers. */
static UInt32
resample_44_to_48(const int16_t *in, UInt32 frames)
{
	const Float64 step = SPEAKER_RATE_44 / HAKIOS_AUDIO_RATE;
	const UInt32 room = sizeof(resampled) / sizeof(resampled[0]) / HAKIOS_AUDIO_CHANNELS;
	UInt32 out = 0;

	if (frames == 0) {
		return 0;
	}
	/* input frame i of this buffer is at position i + 1; position 0 is the last frame of the one before */
	while (resample_pos < (Float64)frames && out < room) {
		UInt32 i = (UInt32)resample_pos;
		Float64 f = resample_pos - i;

		for (UInt32 c = 0; c < HAKIOS_AUDIO_CHANNELS; c++) {
			int a = i == 0 ? resample_prev[c] : in[(i - 1) * HAKIOS_AUDIO_CHANNELS + c];
			int b = in[i * HAKIOS_AUDIO_CHANNELS + c];

			resampled[out * HAKIOS_AUDIO_CHANNELS + c] = (int16_t)(a + (b - a) * f);
		}
		out++;
		resample_pos += step;
	}
	resample_pos -= frames;
	for (UInt32 c = 0; c < HAKIOS_AUDIO_CHANNELS; c++) {
		resample_prev[c] = in[(frames - 1) * HAKIOS_AUDIO_CHANNELS + c];
	}
	return out;
}

static OSStatus
do_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, AudioObjectID stream,
    __unused UInt32 client, UInt32 operation, UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle, void *buffer,
    __unused void *secondary)
{
	const struct device *d = device_of(stream);

	if (d == NULL) {
		return kAudioHardwareBadObjectError;
	}
	if (operation == kAudioServerPlugInIOOperationReadInput && stream == OBJECT_CODEC_INPUT) {
		codec_read(buffer, frames);
	} else if (operation == kAudioServerPlugInIOOperationReadInput) {
		/* the speaker's and Hawking's inputs: silence */
		memset(buffer, 0, (size_t)frames * 2 * d->input_channels);
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == OBJECT_ACTUATOR_OUTPUT) {
		actuator_write(buffer, frames);
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == d->output) {
		const int16_t *out = buffer;
		UInt32 count = frames;

		if (d->rate == SPEAKER_RATE_44) {
			count = resample_44_to_48(buffer, frames);
			out = resampled;
		}
		if (d->object == OBJECT_HEADPHONES) {
			/* its volume and mute are ours to apply: a gain on the samples (the kernel's volume is the speaker's) */
			UInt32 n = frames * HAKIOS_AUDIO_CHANNELS, room = sizeof(hp_scaled) / sizeof(hp_scaled[0]);
			Float32 gain = hp_muted ? 0 : hp_volume;

			n = n < room ? n : room;
			for (UInt32 i = 0; i < n; i++) {
				hp_scaled[i] = (int16_t)lrintf((Float32)((const int16_t *)buffer)[i] * gain);
			}
			out = hp_scaled;
			count = n / HAKIOS_AUDIO_CHANNELS;
		}
		/* the kernel takes it all or leaves it: this thread must not wait */
		if (sink < 0 || write(sink, out, (size_t)count * HAKIOS_AUDIO_FRAME) < 0) {
			refused += count;
		}
		written += count;
	}
	return 0;
}

static AudioServerPlugInDriverInterface interface = {
	NULL, query_interface, add_ref, release, initialize, create_device, destroy_device, device_client, device_client,
	perform_configuration_change, configuration_change, has_property, is_property_settable, get_property_data_size,
	get_property_data, set_property_data, start_io, stop_io, get_zero_time_stamp, will_do_io_operation,
	begin_end_io_operation, do_io_operation, begin_end_io_operation,
};
static AudioServerPlugInDriverInterface *driver = &interface;

/* The factory named in the bundle's Info.plist. */
void *
HakiosAudio_Create(__unused CFAllocatorRef allocator, CFUUIDRef type)
{
	return CFEqual(type, kAudioServerPlugInTypeUUID) ? &driver : NULL;
}

/* Host test of the Vertical Audio driver (macOS). Loads the driver module like
 * coreaudiod would (dlopen + the CFPlugIn factory) and drives its
 * AudioServerPlugInDriverInterface directly: objects, properties, clock, and the
 * loopback from the hidden feed device to the "Vertical Audio" input. Nothing is
 * installed and coreaudiod is not involved.
 *
 *   vertical-audio-driver-tests <driver module (.so/.dylib) | VerticalAudio.driver bundle>
 *
 * A .driver bundle is loaded through CFPlugIn, from its Info.plist (factories and
 * types), the way coreaudiod loads it. */

#include "audio-device-ids.hpp"

#include <CoreAudio/AudioServerPlugIn.h>
#include <dlfcn.h>
#include <mach/mach_time.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

#define EXPECT(cond, what)                                                         \
	do {                                                                       \
		g_checks++;                                                        \
		if (!(cond)) {                                                     \
			g_failures++;                                              \
			std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, what); \
		}                                                                  \
	} while (0)

AudioServerPlugInDriverRef D = nullptr;
int g_propertyChanges = 0;

OSStatus hostPropertiesChanged(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress *)
{
	g_propertyChanges++;
	return 0;
}
OSStatus hostCopyFromStorage(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef *out)
{
	*out = nullptr;
	return 0;
}
OSStatus hostWriteToStorage(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef)
{
	return 0;
}
OSStatus hostDeleteFromStorage(AudioServerPlugInHostRef, CFStringRef)
{
	return 0;
}
OSStatus hostRequestConfigurationChange(AudioServerPlugInHostRef, AudioObjectID, UInt64, void *)
{
	return 0;
}

const AudioServerPlugInHostInterface kHost = {hostPropertiesChanged, hostCopyFromStorage, hostWriteToStorage,
					      hostDeleteFromStorage, hostRequestConfigurationChange};

AudioServerPlugInDriverInterface &I()
{
	return **D;
}

AudioObjectPropertyAddress addr(AudioObjectPropertySelector s,
				AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
	return {s, scope, kAudioObjectPropertyElementMain};
}

template<typename T>
T get(AudioObjectID obj, AudioObjectPropertySelector s,
      AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal, OSStatus *status = nullptr)
{
	T value{};
	UInt32 used = 0;
	const AudioObjectPropertyAddress a = addr(s, scope);
	const OSStatus st = I().GetPropertyData(D, obj, 0, &a, 0, nullptr, sizeof(T), &used, &value);
	if (status)
		*status = st;
	return value;
}

std::string str(AudioObjectID obj, AudioObjectPropertySelector s)
{
	CFStringRef v = get<CFStringRef>(obj, s);
	char buf[256] = {};
	if (v)
		CFStringGetCString(v, buf, sizeof(buf), kCFStringEncodingUTF8);
	return buf;
}

std::vector<AudioObjectID> objects(AudioObjectID obj, AudioObjectPropertySelector s,
				   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
	const AudioObjectPropertyAddress a = addr(s, scope);
	UInt32 size = 0;
	if (I().GetPropertyDataSize(D, obj, 0, &a, 0, nullptr, &size) != 0)
		return {};
	std::vector<AudioObjectID> out(size / sizeof(AudioObjectID));
	UInt32 used = 0;
	if (size)
		I().GetPropertyData(D, obj, 0, &a, 0, nullptr, size, &used, out.data());
	out.resize(used / sizeof(AudioObjectID));
	return out;
}

AudioObjectID deviceForUID(const char *uid)
{
	CFStringRef s = CFStringCreateWithCString(nullptr, uid, kCFStringEncodingUTF8);
	const AudioObjectPropertyAddress a = addr(kAudioPlugInPropertyTranslateUIDToDevice);
	AudioObjectID out = kAudioObjectUnknown;
	UInt32 used = 0;
	I().GetPropertyData(D, kAudioObjectPlugInObject, 0, &a, sizeof(s), &s, sizeof(out), &used, &out);
	CFRelease(s);
	return out;
}

AudioServerPlugInIOCycleInfo cycleAt(Float64 sampleTime)
{
	AudioServerPlugInIOCycleInfo c = {};
	c.mInputTime.mSampleTime = sampleTime;
	c.mOutputTime.mSampleTime = sampleTime;
	c.mInputTime.mFlags = c.mOutputTime.mFlags = kAudioTimeStampSampleTimeValid;
	return c;
}

void writeFeed(AudioObjectID feed, Float64 outputSampleTime, std::vector<float> &buf)
{
	const AudioServerPlugInIOCycleInfo c = cycleAt(outputSampleTime);
	const UInt32 frames = (UInt32)(buf.size() / 2);
	I().DoIOOperation(D, feed, feed + 1, 0, kAudioServerPlugInIOOperationWriteMix, frames, &c, buf.data(), nullptr);
}

void readInput(AudioObjectID input, Float64 inputSampleTime, std::vector<float> &buf)
{
	const AudioServerPlugInIOCycleInfo c = cycleAt(inputSampleTime);
	const UInt32 frames = (UInt32)(buf.size() / 2);
	I().DoIOOperation(D, input, input + 1, 0, kAudioServerPlugInIOOperationReadInput, frames, &c, buf.data(),
			  nullptr);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <driver module>\n", argv[0]);
		return 2;
	}
	const std::string path = argv[1];
	if (path.size() > 7 && path.compare(path.size() - 7, 7, ".driver") == 0) {
		/* Like coreaudiod: CFPlugIn finds the factory of the AudioServerPlugIn type. */
		CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path.c_str(),
								       (CFIndex)path.size(), true);
		CFPlugInRef plugin = url ? CFPlugInCreate(nullptr, url) : nullptr;
		CFArrayRef factories =
			plugin ? CFPlugInFindFactoriesForPlugInTypeInPlugIn(kAudioServerPlugInTypeUUID, plugin)
			       : nullptr;
		EXPECT(factories && CFArrayGetCount(factories) == 1,
		       "Info.plist declares one AudioServerPlugIn factory");
		if (!factories || CFArrayGetCount(factories) < 1)
			return 1;
		auto factory = static_cast<CFUUIDRef>(CFArrayGetValueAtIndex(factories, 0));
		void *instance = CFPlugInInstanceCreate(nullptr, factory, kAudioServerPlugInTypeUUID);
		EXPECT(instance != nullptr, "CFPlugIn creates the driver");
		if (!instance)
			return 1;
		auto **unknown = static_cast<IUnknownVTbl **>(instance);
		void *iface = nullptr;
		(*unknown)->QueryInterface(unknown, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &iface);
		D = static_cast<AudioServerPlugInDriverRef>(iface);
		std::printf("loaded %s through CFPlugIn\n", path.c_str());
	} else {
		void *module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!module) {
			std::fprintf(stderr, "dlopen: %s\n", dlerror());
			return 1;
		}
		using Factory = void *(*)(CFAllocatorRef, CFUUIDRef);
		auto create = reinterpret_cast<Factory>(dlsym(module, "VerticalAudio_Create"));
		EXPECT(create != nullptr, "factory VerticalAudio_Create exported");
		if (!create)
			return 1;
		EXPECT(create(nullptr, CFUUIDGetConstantUUIDWithBytes(nullptr, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
								      13, 14, 15, 16)) == nullptr,
		       "factory refuses other plug-in types");
		D = static_cast<AudioServerPlugInDriverRef>(create(nullptr, kAudioServerPlugInTypeUUID));
	}
	EXPECT(D != nullptr, "factory returns the driver");
	if (!D)
		return 1;

	/* IUnknown */
	void *iface = nullptr;
	EXPECT(I().QueryInterface(D, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &iface) == S_OK &&
		       iface == D,
	       "QueryInterface gives the driver interface");
	EXPECT(I().QueryInterface(D, CFUUIDGetUUIDBytes(IUnknownUUID), &iface) == S_OK, "QueryInterface IUnknown");
	EXPECT(I().QueryInterface(D, CFUUIDGetUUIDBytes(kAudioServerPlugInTypeUUID), &iface) == E_NOINTERFACE,
	       "QueryInterface refuses an unknown interface");
	I().Release(D);
	I().Release(D);

	EXPECT(I().Initialize(D, &kHost) == 0, "Initialize");

	/* Plug-in object */
	const auto devices = objects(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList);
	EXPECT(devices.size() == 2, "two devices");
	EXPECT(get<AudioClassID>(kAudioObjectPlugInObject, kAudioObjectPropertyClass) == kAudioPlugInClassID,
	       "plug-in class");
	EXPECT(str(kAudioObjectPlugInObject, kAudioObjectPropertyManufacturer) == "The OnAir Company", "manufacturer");
	const AudioObjectID input = deviceForUID(VERTICAL_AUDIO_INPUT_UID);
	const AudioObjectID feed = deviceForUID(VERTICAL_AUDIO_FEED_UID);
	EXPECT(input != kAudioObjectUnknown && feed != kAudioObjectUnknown && input != feed,
	       "UIDs translate to the devices");
	EXPECT(deviceForUID("nope") == kAudioObjectUnknown, "unknown UID");
	EXPECT(objects(kAudioObjectPlugInObject, kAudioPlugInPropertyBoxList).empty(), "no box");

	/* "Vertical Audio": visible microphone, input only */
	EXPECT(str(input, kAudioObjectPropertyName) == VERTICAL_AUDIO_DEVICE_NAME, "input device name");
	EXPECT(str(input, kAudioDevicePropertyDeviceUID) == VERTICAL_AUDIO_INPUT_UID, "input UID");
	EXPECT(get<UInt32>(input, kAudioDevicePropertyIsHidden) == 0, "input is visible");
	EXPECT(objects(input, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput).size() == 1,
	       "input has an input stream");
	EXPECT(objects(input, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput).empty(),
	       "input has no output stream (not in speaker lists)");
	EXPECT(get<UInt32>(input, kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeInput) == 1,
	       "input can be the default microphone");
	EXPECT(get<UInt32>(input, kAudioDevicePropertyDeviceCanBeDefaultSystemDevice) == 0, "never the system device");
	EXPECT(get<UInt32>(input, kAudioDevicePropertyTransportType) == kAudioDeviceTransportTypeVirtual,
	       "virtual transport");
	EXPECT(get<Float64>(input, kAudioDevicePropertyNominalSampleRate) == 48000.0, "48 kHz");

	/* Feed: hidden, output only */
	EXPECT(get<UInt32>(feed, kAudioDevicePropertyIsHidden) == 1, "feed is hidden");
	EXPECT(objects(feed, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput).size() == 1,
	       "feed has an output stream");
	EXPECT(objects(feed, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput).empty(),
	       "feed has no input stream");
	EXPECT(get<UInt32>(feed, kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioObjectPropertyScopeOutput) == 0,
	       "feed is never a default device");
	EXPECT(get<UInt32>(feed, kAudioDevicePropertyClockDomain) ==
		       get<UInt32>(input, kAudioDevicePropertyClockDomain),
	       "one clock domain");

	/* coreaudiod drops a device when one property it asks for while adding it is
	 * unknown: those are answered in every scope. */
	for (AudioObjectID dev : {input, feed}) {
		for (AudioObjectPropertyScope scope : {kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyScopeInput,
						       kAudioObjectPropertyScopeOutput}) {
			OSStatus st = 0, all = 0;
			get<UInt32>(dev, kAudioDevicePropertyLatency, scope, &st);
			all |= st;
			get<UInt32>(dev, kAudioDevicePropertySafetyOffset, scope, &st);
			all |= st;
			UInt32 stereo[2] = {0, 0};
			UInt32 used = 0;
			const AudioObjectPropertyAddress pa =
				addr(kAudioDevicePropertyPreferredChannelsForStereo, scope);
			all |= I().GetPropertyData(D, dev, 0, &pa, 0, nullptr, sizeof(stereo), &used, stereo);
			UInt32 size = 0;
			const AudioObjectPropertyAddress la = addr(kAudioDevicePropertyPreferredChannelLayout, scope);
			all |= I().GetPropertyDataSize(D, dev, 0, &la, 0, nullptr, &size);
			std::vector<char> layout(size);
			all |= I().GetPropertyData(D, dev, 0, &la, 0, nullptr, size, &used, layout.data());
			const AudioObjectPropertyAddress ca = addr(kAudioObjectPropertyCustomPropertyInfoList, scope);
			all |= I().GetPropertyDataSize(D, dev, 0, &ca, 0, nullptr, &size);
			get<CFStringRef>(dev, kAudioDevicePropertyConfigurationApplication, scope, &st);
			all |= st;
			EXPECT(all == 0 && stereo[0] == 1 && stereo[1] == 2,
			       "latency, safety offset, stereo channels, layout, custom properties in every scope");
		}
	}

	/* Streams */
	const AudioObjectID inStream =
		objects(input, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput).at(0);
	const AudioObjectID feedStream =
		objects(feed, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput).at(0);
	EXPECT(get<UInt32>(inStream, kAudioStreamPropertyDirection) == 1, "input stream direction");
	EXPECT(get<UInt32>(feedStream, kAudioStreamPropertyDirection) == 0, "feed stream direction");
	EXPECT(get<UInt32>(inStream, kAudioStreamPropertyLatency) == 128, "input stream latency = loopback");
	const auto fmt = get<AudioStreamBasicDescription>(inStream, kAudioStreamPropertyVirtualFormat);
	EXPECT(fmt.mSampleRate == 48000.0 && fmt.mChannelsPerFrame == 2 && fmt.mBitsPerChannel == 32 &&
		       (fmt.mFormatFlags & kAudioFormatFlagIsFloat),
	       "48 kHz stereo float");
	{
		AudioStreamBasicDescription other = fmt;
		other.mSampleRate = 44100.0;
		const AudioObjectPropertyAddress a = addr(kAudioStreamPropertyVirtualFormat);
		EXPECT(I().SetPropertyData(D, inStream, 0, &a, 0, nullptr, sizeof(other), &other) != 0,
		       "other format refused");
		EXPECT(I().SetPropertyData(D, inStream, 0, &a, 0, nullptr, sizeof(fmt), &fmt) == 0,
		       "same format accepted");
	}
	{
		const AudioObjectPropertyAddress a = addr(kAudioObjectPropertyName);
		EXPECT(I().HasProperty(D, input, 0, &a), "HasProperty name");
		const AudioObjectPropertyAddress bogus = addr('zzzz');
		EXPECT(!I().HasProperty(D, input, 0, &bogus), "unknown property");
		EXPECT(!I().HasProperty(D, 99, 0, &a), "unknown object");
	}

	/* IO operations */
	Boolean willDo = false, inPlace = false;
	I().WillDoIOOperation(D, input, 0, kAudioServerPlugInIOOperationReadInput, &willDo, &inPlace);
	EXPECT(willDo, "input reads input");
	I().WillDoIOOperation(D, input, 0, kAudioServerPlugInIOOperationWriteMix, &willDo, &inPlace);
	EXPECT(!willDo, "input does not write");
	I().WillDoIOOperation(D, feed, 0, kAudioServerPlugInIOOperationWriteMix, &willDo, &inPlace);
	EXPECT(willDo, "feed writes the mix");

	/* Clock: both devices share their time stamps; one period = 16384 frames at 48 kHz. */
	g_propertyChanges = 0;
	EXPECT(I().StartIO(D, feed, 1) == 0, "StartIO feed");
	EXPECT(I().StartIO(D, input, 2) == 0, "StartIO input");
	EXPECT(g_propertyChanges == 2, "is-running notifications");
	EXPECT(get<UInt32>(input, kAudioDevicePropertyDeviceIsRunning) == 1, "input running");
	Float64 s1 = 0, s2 = 0;
	UInt64 h1 = 0, h2 = 0, seed = 0;
	I().GetZeroTimeStamp(D, feed, 0, &s1, &h1, &seed);
	I().GetZeroTimeStamp(D, input, 0, &s2, &h2, &seed);
	EXPECT(s1 == s2 && h1 == h2, "same zero time stamp on both devices");
	{
		mach_timebase_info_data_t tb;
		mach_timebase_info(&tb);
		const double nsPerTick = (double)tb.numer / tb.denom;
		const uint64_t start = mach_absolute_time();
		Float64 s = 0;
		UInt64 h = 0;
		/* Wait for the next period (16384 frames = 341 ms). */
		do {
			I().GetZeroTimeStamp(D, input, 0, &s, &h, &seed);
		} while (s == s1 && (mach_absolute_time() - start) * nsPerTick < 1e9);
		EXPECT(s == s1 + 16384, "next zero time stamp one period later");
		const double ms = (double)(h - h1) * nsPerTick / 1e6;
		EXPECT(std::fabs(ms - 16384.0 / 48.0) < 0.01, "period = 341.33 ms of host time");
		std::printf("zero time stamp period: %.3f ms (expected 341.333)\n", ms);
	}

	/* Loopback: what the feed writes at sample time T is read by the input at T + 128. */
	const UInt32 N = 256;
	std::vector<float> out(2 * N), in(2 * N);
	int64_t mismatches = 0;
	for (int cycle = 0; cycle < 400; cycle++) {
		const int64_t T = 1000000 + cycle * (int64_t)N;
		for (UInt32 i = 0; i < N; i++)
			out[2 * i] = out[2 * i + 1] =
				std::sin((double)(T + i) * 0.01) * 0.5f + 0.01f * (float)(cycle % 7);
		writeFeed(feed, (Float64)T, out);
		readInput(input, (Float64)(T + 128), in); /* frames T .. T+255 */
		for (UInt32 i = 0; i < 2 * N; i++)
			if (in[i] != out[i])
				mismatches++;
	}
	EXPECT(mismatches == 0, "input gets the feed 128 frames (2.67 ms) later, bit-exact");
	std::printf("loopback: 400 cycles of 256 frames, %lld mismatches, latency 128 frames = %.2f ms\n",
		    (long long)mismatches, 128 * 1000.0 / 48000.0);

	/* Read once: the same frames are silent the second time. */
	readInput(input, 1000000 + 128, in);
	bool silent = true;
	for (float v : in)
		silent = silent && v == 0.0f;
	EXPECT(silent, "frames already read are silent");

	/* Writes while nobody listens are dropped; a new listener starts from silence. */
	EXPECT(I().StopIO(D, input, 2) == 0, "StopIO input");
	EXPECT(get<UInt32>(input, kAudioDevicePropertyDeviceIsRunning) == 0, "input stopped");
	std::fill(out.begin(), out.end(), 0.75f);
	writeFeed(feed, 5000000, out);
	EXPECT(I().StartIO(D, input, 2) == 0, "StartIO input again");
	readInput(input, 5000000 + 128, in);
	silent = true;
	for (float v : in)
		silent = silent && v == 0.0f;
	EXPECT(silent, "no stale audio for a new listener");
	EXPECT(I().StopIO(D, input, 2) == 0 && I().StopIO(D, feed, 1) == 0, "StopIO both");
	EXPECT(I().StopIO(D, feed, 1) != 0, "extra StopIO refused");

	std::printf("%d checks, %d failed\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}

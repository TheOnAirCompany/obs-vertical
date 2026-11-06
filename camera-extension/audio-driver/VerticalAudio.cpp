/*
Vertical for OBS - Vertical Audio driver
Copyright (C) 2026 The OnAir Company

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/* AudioServerPlugIn (HAL plug-in) that publishes the "Vertical Audio" microphone.
 * Loaded by coreaudiod from /Library/Audio/Plug-Ins/HAL/VerticalAudio.driver.
 *
 * Two devices share one clock and one ring (see docs/VIRTUAL-AUDIO.md):
 *
 *   "Vertical Audio Feed"  hidden, output only. The OBS plugin finds it by UID and
 *                          plays the vertical mix into it.
 *   "Vertical Audio"       visible, input only: what TikTok LIVE Studio and other apps
 *                          pick as their microphone.
 *
 * The feed is hidden so that "Vertical Audio" never shows in output (speaker) lists:
 * nobody can send the system sound into it by mistake. Both devices tick on the host
 * clock at 48 kHz from a shared anchor, so the same sample time means the same instant
 * on both: the feed writes at its output sample time, the input reads kLoopbackFrames
 * behind its input sample time. No IPC, no shared memory: coreaudiod moves the audio
 * between the processes, which also works inside its sandbox. */

#include "audio-device-ids.hpp"
#include "loopback-ring.hpp"

#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace {

using vertical::core::LoopbackRing;

/* ---------------------------------------------------------------- constants */

enum : AudioObjectID {
	kObjectPlugIn = kAudioObjectPlugInObject,
	kObjectInputDevice = 2,
	kObjectInputStream = 3,
	kObjectFeedDevice = 4,
	kObjectFeedStream = 5,
};

constexpr Float64 kSampleRate = 48000.0;
constexpr UInt32 kChannels = 2;
constexpr UInt32 kBytesPerFrame = kChannels * sizeof(Float32);
/* Frames between two zero time stamps; also the ring size. */
constexpr UInt32 kZeroTimeStampPeriod = 16384;
/* The input reads this far behind the output: margin for the feed's IO thread to finish
 * writing (kLoopbackFrames in src/core/sync.hpp). */
constexpr UInt32 kLoopbackFrames = 128;
/* Same clock domain on both devices: aggregates need no drift correction. */
constexpr UInt32 kClockDomain = 0x56455254; /* 'VERT' */

CFStringRef manufacturer()
{
	return CFSTR("The OnAir Company");
}

/* ---------------------------------------------------------------- state */

AudioServerPlugInHostRef gHost = nullptr;
std::atomic<ULONG> gRefCount{0};

std::mutex gStateLock; /* StartIO / StopIO; never taken on the IO threads */
UInt32 gInputIOCount = 0;
UInt32 gFeedIOCount = 0;
std::atomic<bool> gInputRunning{false};
std::atomic<UInt64> gAnchorHostTime{0};
Float64 gHostTicksPerFrame = 0.0;

LoopbackRing &ring()
{
	static LoopbackRing r(kZeroTimeStampPeriod, (int)kChannels);
	return r;
}

bool isDevice(AudioObjectID id)
{
	return id == kObjectInputDevice || id == kObjectFeedDevice;
}

bool isStream(AudioObjectID id)
{
	return id == kObjectInputStream || id == kObjectFeedStream;
}

AudioObjectID streamOf(AudioObjectID device)
{
	return device == kObjectInputDevice ? kObjectInputStream : kObjectFeedStream;
}

AudioObjectID deviceOf(AudioObjectID stream)
{
	return stream == kObjectInputStream ? kObjectInputDevice : kObjectFeedDevice;
}

bool deviceIsInput(AudioObjectID device)
{
	return device == kObjectInputDevice;
}

/* Does `scope` select the stream of this device? */
bool scopeHasStream(AudioObjectID device, AudioObjectPropertyScope scope)
{
	if (scope == kAudioObjectPropertyScopeGlobal)
		return true;
	return deviceIsInput(device) ? scope == kAudioObjectPropertyScopeInput
				     : scope == kAudioObjectPropertyScopeOutput;
}

AudioStreamBasicDescription streamFormat()
{
	AudioStreamBasicDescription f = {};
	f.mSampleRate = kSampleRate;
	f.mFormatID = kAudioFormatLinearPCM;
	f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
	f.mBytesPerPacket = kBytesPerFrame;
	f.mFramesPerPacket = 1;
	f.mBytesPerFrame = kBytesPerFrame;
	f.mChannelsPerFrame = kChannels;
	f.mBitsPerChannel = 32;
	return f;
}

void notify(AudioObjectID object, AudioObjectPropertySelector selector)
{
	if (!gHost)
		return;
	const AudioObjectPropertyAddress address = {selector, kAudioObjectPropertyScopeGlobal,
						    kAudioObjectPropertyElementMain};
	gHost->PropertiesChanged(gHost, object, 1, &address);
}

/* ---------------------------------------------------------------- property helpers */

template<typename T> OSStatus put(UInt32 inDataSize, UInt32 *outDataSize, void *outData, const T &value)
{
	if (inDataSize < sizeof(T))
		return kAudioHardwareBadPropertySizeError;
	std::memcpy(outData, &value, sizeof(T));
	*outDataSize = sizeof(T);
	return kAudioHardwareNoError;
}

/* Copies up to inDataSize / sizeof(T) items. */
template<typename T>
OSStatus putArray(UInt32 inDataSize, UInt32 *outDataSize, void *outData, const T *items, UInt32 count)
{
	const UInt32 fit = inDataSize / (UInt32)sizeof(T);
	const UInt32 n = count < fit ? count : fit;
	if (n)
		std::memcpy(outData, items, n * sizeof(T));
	*outDataSize = n * (UInt32)sizeof(T);
	return kAudioHardwareNoError;
}

/* ---------------------------------------------------------------- plug-in object */

bool plugInHas(const AudioObjectPropertyAddress *a)
{
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
	case kAudioObjectPropertyOwner:
	case kAudioObjectPropertyManufacturer:
	case kAudioObjectPropertyOwnedObjects:
	case kAudioPlugInPropertyBoxList:
	case kAudioPlugInPropertyTranslateUIDToBox:
	case kAudioPlugInPropertyDeviceList:
	case kAudioPlugInPropertyTranslateUIDToDevice:
	case kAudioPlugInPropertyResourceBundle:
		return true;
	default:
		return false;
	}
}

OSStatus plugInSize(const AudioObjectPropertyAddress *a, UInt32 *size)
{
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
		*size = sizeof(AudioClassID);
		return 0;
	case kAudioObjectPropertyOwner:
	case kAudioPlugInPropertyTranslateUIDToBox:
	case kAudioPlugInPropertyTranslateUIDToDevice:
		*size = sizeof(AudioObjectID);
		return 0;
	case kAudioObjectPropertyManufacturer:
	case kAudioPlugInPropertyResourceBundle:
		*size = sizeof(CFStringRef);
		return 0;
	case kAudioObjectPropertyOwnedObjects:
	case kAudioPlugInPropertyDeviceList:
		*size = 2 * sizeof(AudioObjectID);
		return 0;
	case kAudioPlugInPropertyBoxList:
		*size = 0;
		return 0;
	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

OSStatus plugInGet(const AudioObjectPropertyAddress *a, UInt32 qualifierSize, const void *qualifier, UInt32 inDataSize,
		   UInt32 *outDataSize, void *outData)
{
	static const AudioObjectID devices[2] = {kObjectInputDevice, kObjectFeedDevice};
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
		return put<AudioClassID>(inDataSize, outDataSize, outData, kAudioObjectClassID);
	case kAudioObjectPropertyClass:
		return put<AudioClassID>(inDataSize, outDataSize, outData, kAudioPlugInClassID);
	case kAudioObjectPropertyOwner:
		return put<AudioObjectID>(inDataSize, outDataSize, outData, kAudioObjectUnknown);
	case kAudioObjectPropertyManufacturer:
		return put<CFStringRef>(inDataSize, outDataSize, outData, manufacturer());
	case kAudioPlugInPropertyResourceBundle:
		return put<CFStringRef>(inDataSize, outDataSize, outData, CFSTR(""));
	case kAudioObjectPropertyOwnedObjects:
	case kAudioPlugInPropertyDeviceList:
		return putArray(inDataSize, outDataSize, outData, devices, 2);
	case kAudioPlugInPropertyBoxList:
		*outDataSize = 0;
		return 0;
	case kAudioPlugInPropertyTranslateUIDToBox:
		return put<AudioObjectID>(inDataSize, outDataSize, outData, kAudioObjectUnknown);
	case kAudioPlugInPropertyTranslateUIDToDevice: {
		if (qualifierSize < sizeof(CFStringRef) || !qualifier)
			return kAudioHardwareBadPropertySizeError;
		CFStringRef uid = *static_cast<const CFStringRef *>(qualifier);
		AudioObjectID found = kAudioObjectUnknown;
		if (uid && CFStringCompare(uid, CFSTR(VERTICAL_AUDIO_INPUT_UID), 0) == kCFCompareEqualTo)
			found = kObjectInputDevice;
		else if (uid && CFStringCompare(uid, CFSTR(VERTICAL_AUDIO_FEED_UID), 0) == kCFCompareEqualTo)
			found = kObjectFeedDevice;
		return put<AudioObjectID>(inDataSize, outDataSize, outData, found);
	}
	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

/* ---------------------------------------------------------------- devices */

bool deviceHas(AudioObjectID device, const AudioObjectPropertyAddress *a)
{
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
	case kAudioObjectPropertyOwner:
	case kAudioObjectPropertyName:
	case kAudioObjectPropertyManufacturer:
	case kAudioObjectPropertyOwnedObjects:
	case kAudioDevicePropertyDeviceUID:
	case kAudioDevicePropertyModelUID:
	case kAudioDevicePropertyTransportType:
	case kAudioDevicePropertyRelatedDevices:
	case kAudioDevicePropertyClockDomain:
	case kAudioDevicePropertyDeviceIsAlive:
	case kAudioDevicePropertyDeviceIsRunning:
	case kAudioDevicePropertyDeviceCanBeDefaultDevice:
	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
	case kAudioObjectPropertyControlList:
	case kAudioDevicePropertyNominalSampleRate:
	case kAudioDevicePropertyAvailableNominalSampleRates:
	case kAudioDevicePropertyIsHidden:
	case kAudioDevicePropertyZeroTimeStampPeriod:
	case kAudioDevicePropertyClockIsStable:
	case kAudioDevicePropertyStreams:
		return true;
	case kAudioDevicePropertyLatency:
	case kAudioDevicePropertySafetyOffset:
	case kAudioDevicePropertyPreferredChannelsForStereo:
	case kAudioDevicePropertyPreferredChannelLayout:
		return scopeHasStream(device, a->mScope) && a->mScope != kAudioObjectPropertyScopeGlobal;
	default:
		return false;
	}
}

OSStatus deviceSize(AudioObjectID device, const AudioObjectPropertyAddress *a, UInt32 *size)
{
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
		*size = sizeof(AudioClassID);
		return 0;
	case kAudioObjectPropertyOwner:
	case kAudioDevicePropertyRelatedDevices:
		*size = sizeof(AudioObjectID);
		return 0;
	case kAudioObjectPropertyName:
	case kAudioObjectPropertyManufacturer:
	case kAudioDevicePropertyDeviceUID:
	case kAudioDevicePropertyModelUID:
		*size = sizeof(CFStringRef);
		return 0;
	case kAudioObjectPropertyOwnedObjects:
		*size = sizeof(AudioObjectID);
		return 0;
	case kAudioDevicePropertyStreams:
		*size = scopeHasStream(device, a->mScope) ? (UInt32)sizeof(AudioObjectID) : 0;
		return 0;
	case kAudioObjectPropertyControlList:
		*size = 0;
		return 0;
	case kAudioDevicePropertyTransportType:
	case kAudioDevicePropertyClockDomain:
	case kAudioDevicePropertyDeviceIsAlive:
	case kAudioDevicePropertyDeviceIsRunning:
	case kAudioDevicePropertyDeviceCanBeDefaultDevice:
	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
	case kAudioDevicePropertyLatency:
	case kAudioDevicePropertySafetyOffset:
	case kAudioDevicePropertyIsHidden:
	case kAudioDevicePropertyZeroTimeStampPeriod:
	case kAudioDevicePropertyClockIsStable:
		*size = sizeof(UInt32);
		return 0;
	case kAudioDevicePropertyNominalSampleRate:
		*size = sizeof(Float64);
		return 0;
	case kAudioDevicePropertyAvailableNominalSampleRates:
		*size = sizeof(AudioValueRange);
		return 0;
	case kAudioDevicePropertyPreferredChannelsForStereo:
		*size = 2 * sizeof(UInt32);
		return 0;
	case kAudioDevicePropertyPreferredChannelLayout:
		*size = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) +
				 kChannels * sizeof(AudioChannelDescription));
		return 0;
	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

OSStatus deviceGet(AudioObjectID device, const AudioObjectPropertyAddress *a, UInt32 inDataSize, UInt32 *outDataSize,
		   void *outData)
{
	const bool input = deviceIsInput(device);
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
		return put<AudioClassID>(inDataSize, outDataSize, outData, kAudioObjectClassID);
	case kAudioObjectPropertyClass:
		return put<AudioClassID>(inDataSize, outDataSize, outData, kAudioDeviceClassID);
	case kAudioObjectPropertyOwner:
		return put<AudioObjectID>(inDataSize, outDataSize, outData, kObjectPlugIn);
	case kAudioObjectPropertyName:
		return put<CFStringRef>(inDataSize, outDataSize, outData,
					input ? CFSTR(VERTICAL_AUDIO_DEVICE_NAME) : CFSTR("Vertical Audio Feed"));
	case kAudioObjectPropertyManufacturer:
		return put<CFStringRef>(inDataSize, outDataSize, outData, manufacturer());
	case kAudioDevicePropertyDeviceUID:
		return put<CFStringRef>(inDataSize, outDataSize, outData,
					input ? CFSTR(VERTICAL_AUDIO_INPUT_UID) : CFSTR(VERTICAL_AUDIO_FEED_UID));
	case kAudioDevicePropertyModelUID:
		return put<CFStringRef>(inDataSize, outDataSize, outData, CFSTR(VERTICAL_AUDIO_MODEL_UID));
	case kAudioObjectPropertyOwnedObjects: {
		const AudioObjectID stream = streamOf(device);
		return putArray(inDataSize, outDataSize, outData, &stream, 1);
	}
	case kAudioDevicePropertyStreams: {
		const AudioObjectID stream = streamOf(device);
		return putArray(inDataSize, outDataSize, outData, &stream, scopeHasStream(device, a->mScope) ? 1 : 0);
	}
	case kAudioObjectPropertyControlList:
		*outDataSize = 0;
		return 0;
	case kAudioDevicePropertyRelatedDevices:
		return put<AudioObjectID>(inDataSize, outDataSize, outData, device);
	case kAudioDevicePropertyTransportType:
		return put<UInt32>(inDataSize, outDataSize, outData, kAudioDeviceTransportTypeVirtual);
	case kAudioDevicePropertyClockDomain:
		return put<UInt32>(inDataSize, outDataSize, outData, kClockDomain);
	case kAudioDevicePropertyDeviceIsAlive:
	case kAudioDevicePropertyClockIsStable:
		return put<UInt32>(inDataSize, outDataSize, outData, 1);
	case kAudioDevicePropertyDeviceIsRunning: {
		std::lock_guard<std::mutex> lock(gStateLock);
		const UInt32 running = (input ? gInputIOCount : gFeedIOCount) > 0 ? 1 : 0;
		return put<UInt32>(inDataSize, outDataSize, outData, running);
	}
	case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		/* The microphone may be the default input; the feed is never a default. */
		return put<UInt32>(inDataSize, outDataSize, outData,
				   input && a->mScope != kAudioObjectPropertyScopeOutput ? 1 : 0);
	case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
		return put<UInt32>(inDataSize, outDataSize, outData, 0);
	case kAudioDevicePropertyLatency:
	case kAudioDevicePropertySafetyOffset:
		return put<UInt32>(inDataSize, outDataSize, outData, 0);
	case kAudioDevicePropertyIsHidden:
		return put<UInt32>(inDataSize, outDataSize, outData, input ? 0 : 1);
	case kAudioDevicePropertyZeroTimeStampPeriod:
		return put<UInt32>(inDataSize, outDataSize, outData, kZeroTimeStampPeriod);
	case kAudioDevicePropertyNominalSampleRate:
		return put<Float64>(inDataSize, outDataSize, outData, kSampleRate);
	case kAudioDevicePropertyAvailableNominalSampleRates: {
		const AudioValueRange range = {kSampleRate, kSampleRate};
		return putArray(inDataSize, outDataSize, outData, &range, 1);
	}
	case kAudioDevicePropertyPreferredChannelsForStereo: {
		const UInt32 channels[2] = {1, 2};
		if (inDataSize < sizeof(channels))
			return kAudioHardwareBadPropertySizeError;
		std::memcpy(outData, channels, sizeof(channels));
		*outDataSize = sizeof(channels);
		return 0;
	}
	case kAudioDevicePropertyPreferredChannelLayout: {
		const UInt32 size = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) +
					     kChannels * sizeof(AudioChannelDescription));
		if (inDataSize < size)
			return kAudioHardwareBadPropertySizeError;
		auto *layout = static_cast<AudioChannelLayout *>(outData);
		std::memset(layout, 0, size);
		layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
		layout->mNumberChannelDescriptions = kChannels;
		layout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
		layout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
		*outDataSize = size;
		return 0;
	}
	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

/* ---------------------------------------------------------------- streams */

bool streamHas(const AudioObjectPropertyAddress *a)
{
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
	case kAudioObjectPropertyOwner:
	case kAudioObjectPropertyName:
	case kAudioObjectPropertyOwnedObjects:
	case kAudioStreamPropertyIsActive:
	case kAudioStreamPropertyDirection:
	case kAudioStreamPropertyTerminalType:
	case kAudioStreamPropertyStartingChannel:
	case kAudioStreamPropertyLatency:
	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:
	case kAudioStreamPropertyAvailableVirtualFormats:
	case kAudioStreamPropertyAvailablePhysicalFormats:
		return true;
	default:
		return false;
	}
}

OSStatus streamSize(const AudioObjectPropertyAddress *a, UInt32 *size)
{
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
	case kAudioObjectPropertyClass:
		*size = sizeof(AudioClassID);
		return 0;
	case kAudioObjectPropertyOwner:
		*size = sizeof(AudioObjectID);
		return 0;
	case kAudioObjectPropertyName:
		*size = sizeof(CFStringRef);
		return 0;
	case kAudioObjectPropertyOwnedObjects:
		*size = 0;
		return 0;
	case kAudioStreamPropertyIsActive:
	case kAudioStreamPropertyDirection:
	case kAudioStreamPropertyTerminalType:
	case kAudioStreamPropertyStartingChannel:
	case kAudioStreamPropertyLatency:
		*size = sizeof(UInt32);
		return 0;
	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:
		*size = sizeof(AudioStreamBasicDescription);
		return 0;
	case kAudioStreamPropertyAvailableVirtualFormats:
	case kAudioStreamPropertyAvailablePhysicalFormats:
		*size = sizeof(AudioStreamRangedDescription);
		return 0;
	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

OSStatus streamGet(AudioObjectID stream, const AudioObjectPropertyAddress *a, UInt32 inDataSize, UInt32 *outDataSize,
		   void *outData)
{
	const bool input = stream == kObjectInputStream;
	switch (a->mSelector) {
	case kAudioObjectPropertyBaseClass:
		return put<AudioClassID>(inDataSize, outDataSize, outData, kAudioObjectClassID);
	case kAudioObjectPropertyClass:
		return put<AudioClassID>(inDataSize, outDataSize, outData, kAudioStreamClassID);
	case kAudioObjectPropertyOwner:
		return put<AudioObjectID>(inDataSize, outDataSize, outData, deviceOf(stream));
	case kAudioObjectPropertyName:
		return put<CFStringRef>(inDataSize, outDataSize, outData,
					input ? CFSTR("Vertical Audio") : CFSTR("Vertical Audio Feed"));
	case kAudioObjectPropertyOwnedObjects:
		*outDataSize = 0;
		return 0;
	case kAudioStreamPropertyIsActive:
		return put<UInt32>(inDataSize, outDataSize, outData, 1);
	case kAudioStreamPropertyDirection:
		return put<UInt32>(inDataSize, outDataSize, outData, input ? 1 : 0);
	case kAudioStreamPropertyTerminalType:
		return put<UInt32>(inDataSize, outDataSize, outData,
				   input ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeLine);
	case kAudioStreamPropertyStartingChannel:
		return put<UInt32>(inDataSize, outDataSize, outData, 1);
	case kAudioStreamPropertyLatency:
		return put<UInt32>(inDataSize, outDataSize, outData, input ? kLoopbackFrames : 0);
	case kAudioStreamPropertyVirtualFormat:
	case kAudioStreamPropertyPhysicalFormat:
		return put<AudioStreamBasicDescription>(inDataSize, outDataSize, outData, streamFormat());
	case kAudioStreamPropertyAvailableVirtualFormats:
	case kAudioStreamPropertyAvailablePhysicalFormats: {
		AudioStreamRangedDescription d = {};
		d.mFormat = streamFormat();
		d.mSampleRateRange.mMinimum = kSampleRate;
		d.mSampleRateRange.mMaximum = kSampleRate;
		return putArray(inDataSize, outDataSize, outData, &d, 1);
	}
	default:
		return kAudioHardwareUnknownPropertyError;
	}
}

/* ---------------------------------------------------------------- IUnknown */

HRESULT queryInterface(void *driver, REFIID uuid, LPVOID *out);
ULONG addRef(void *driver);
ULONG release(void *driver);

/* ---------------------------------------------------------------- driver interface */

OSStatus initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef host)
{
	gHost = host;
	mach_timebase_info_data_t timebase = {};
	mach_timebase_info(&timebase);
	gHostTicksPerFrame = (1000000000.0 / kSampleRate) * (Float64)timebase.denom / (Float64)timebase.numer;
	ring();
	return kAudioHardwareNoError;
}

OSStatus createDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *)
{
	return kAudioHardwareUnsupportedOperationError;
}

OSStatus destroyDevice(AudioServerPlugInDriverRef, AudioObjectID)
{
	return kAudioHardwareUnsupportedOperationError;
}

OSStatus addDeviceClient(AudioServerPlugInDriverRef, AudioObjectID device, const AudioServerPlugInClientInfo *)
{
	return isDevice(device) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

OSStatus removeDeviceClient(AudioServerPlugInDriverRef, AudioObjectID device, const AudioServerPlugInClientInfo *)
{
	return isDevice(device) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

OSStatus performConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *)
{
	return kAudioHardwareNoError;
}

OSStatus abortConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *)
{
	return kAudioHardwareNoError;
}

Boolean hasProperty(AudioServerPlugInDriverRef, AudioObjectID object, pid_t, const AudioObjectPropertyAddress *a)
{
	if (!a)
		return false;
	if (object == kObjectPlugIn)
		return plugInHas(a);
	if (isDevice(object))
		return deviceHas(object, a);
	if (isStream(object))
		return streamHas(a);
	return false;
}

OSStatus isPropertySettable(AudioServerPlugInDriverRef, AudioObjectID object, pid_t,
			    const AudioObjectPropertyAddress *a, Boolean *settable)
{
	if (!a || !settable)
		return kAudioHardwareIllegalOperationError;
	*settable = false;
	if (object == kObjectPlugIn)
		return plugInHas(a) ? 0 : kAudioHardwareUnknownPropertyError;
	if (isDevice(object)) {
		if (!deviceHas(object, a))
			return kAudioHardwareUnknownPropertyError;
		*settable = a->mSelector == kAudioDevicePropertyNominalSampleRate;
		return 0;
	}
	if (isStream(object)) {
		if (!streamHas(a))
			return kAudioHardwareUnknownPropertyError;
		*settable = a->mSelector == kAudioStreamPropertyVirtualFormat ||
			    a->mSelector == kAudioStreamPropertyPhysicalFormat;
		return 0;
	}
	return kAudioHardwareBadObjectError;
}

OSStatus getPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID object, pid_t,
			     const AudioObjectPropertyAddress *a, UInt32, const void *, UInt32 *size)
{
	if (!a || !size)
		return kAudioHardwareIllegalOperationError;
	if (object == kObjectPlugIn)
		return plugInSize(a, size);
	if (isDevice(object))
		return deviceHas(object, a) ? deviceSize(object, a, size) : kAudioHardwareUnknownPropertyError;
	if (isStream(object))
		return streamSize(a, size);
	return kAudioHardwareBadObjectError;
}

OSStatus getPropertyData(AudioServerPlugInDriverRef, AudioObjectID object, pid_t, const AudioObjectPropertyAddress *a,
			 UInt32 qualifierSize, const void *qualifier, UInt32 inDataSize, UInt32 *outDataSize,
			 void *outData)
{
	if (!a || !outDataSize || (!outData && inDataSize))
		return kAudioHardwareIllegalOperationError;
	*outDataSize = 0;
	if (object == kObjectPlugIn)
		return plugInGet(a, qualifierSize, qualifier, inDataSize, outDataSize, outData);
	if (isDevice(object))
		return deviceHas(object, a) ? deviceGet(object, a, inDataSize, outDataSize, outData)
					    : kAudioHardwareUnknownPropertyError;
	if (isStream(object))
		return streamGet(object, a, inDataSize, outDataSize, outData);
	return kAudioHardwareBadObjectError;
}

bool sameFormat(const AudioStreamBasicDescription &f)
{
	const AudioStreamBasicDescription ours = streamFormat();
	return f.mSampleRate == ours.mSampleRate && f.mFormatID == ours.mFormatID &&
	       f.mChannelsPerFrame == ours.mChannelsPerFrame && f.mBitsPerChannel == ours.mBitsPerChannel &&
	       (f.mFormatFlags & kAudioFormatFlagIsFloat);
}

OSStatus setPropertyData(AudioServerPlugInDriverRef, AudioObjectID object, pid_t, const AudioObjectPropertyAddress *a,
			 UInt32, const void *, UInt32 inDataSize, const void *inData)
{
	if (!a)
		return kAudioHardwareIllegalOperationError;
	/* One format only: accept it (nothing changes), refuse anything else. */
	if (isDevice(object) && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
		if (inDataSize < sizeof(Float64) || !inData)
			return kAudioHardwareBadPropertySizeError;
		Float64 rate = 0;
		std::memcpy(&rate, inData, sizeof(rate));
		return rate == kSampleRate ? kAudioHardwareNoError : kAudioHardwareIllegalOperationError;
	}
	if (isStream(object) &&
	    (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
		if (inDataSize < sizeof(AudioStreamBasicDescription) || !inData)
			return kAudioHardwareBadPropertySizeError;
		AudioStreamBasicDescription f = {};
		std::memcpy(&f, inData, sizeof(f));
		return sameFormat(f) ? kAudioHardwareNoError : kAudioDeviceUnsupportedFormatError;
	}
	return kAudioHardwareUnknownPropertyError;
}

OSStatus startIO(AudioServerPlugInDriverRef, AudioObjectID device, UInt32)
{
	if (!isDevice(device))
		return kAudioHardwareBadObjectError;
	bool notifyRunning = false;
	{
		std::lock_guard<std::mutex> lock(gStateLock);
		if (gInputIOCount == 0 && gFeedIOCount == 0)
			gAnchorHostTime.store(mach_absolute_time(), std::memory_order_release);
		UInt32 &count = deviceIsInput(device) ? gInputIOCount : gFeedIOCount;
		if (count == 0) {
			notifyRunning = true;
			if (deviceIsInput(device)) {
				/* Nothing older than this start may be heard. */
				ring().clear();
				gInputRunning.store(true, std::memory_order_release);
			}
		}
		count++;
	}
	if (notifyRunning)
		notify(device, kAudioDevicePropertyDeviceIsRunning);
	return kAudioHardwareNoError;
}

OSStatus stopIO(AudioServerPlugInDriverRef, AudioObjectID device, UInt32)
{
	if (!isDevice(device))
		return kAudioHardwareBadObjectError;
	bool notifyRunning = false;
	{
		std::lock_guard<std::mutex> lock(gStateLock);
		UInt32 &count = deviceIsInput(device) ? gInputIOCount : gFeedIOCount;
		if (count == 0)
			return kAudioHardwareIllegalOperationError;
		count--;
		if (count == 0) {
			notifyRunning = true;
			if (deviceIsInput(device))
				gInputRunning.store(false, std::memory_order_release);
		}
	}
	if (notifyRunning)
		notify(device, kAudioDevicePropertyDeviceIsRunning);
	return kAudioHardwareNoError;
}

/* Both devices derive their time stamps from the same anchor: the same sample time is
 * the same host time on both. */
OSStatus getZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID device, UInt32, Float64 *sampleTime,
			  UInt64 *hostTime, UInt64 *seed)
{
	if (!isDevice(device))
		return kAudioHardwareBadObjectError;
	const UInt64 anchor = gAnchorHostTime.load(std::memory_order_acquire);
	const UInt64 now = mach_absolute_time();
	const Float64 ticksPerPeriod = gHostTicksPerFrame * kZeroTimeStampPeriod;
	const Float64 periods = now > anchor ? std::floor((Float64)(now - anchor) / ticksPerPeriod) : 0.0;
	*sampleTime = periods * kZeroTimeStampPeriod;
	*hostTime = anchor + (UInt64)(periods * ticksPerPeriod);
	*seed = 1;
	return kAudioHardwareNoError;
}

OSStatus willDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID device, UInt32, UInt32 operation, Boolean *willDo,
			   Boolean *inPlace)
{
	if (!isDevice(device))
		return kAudioHardwareBadObjectError;
	const bool yes = deviceIsInput(device) ? operation == kAudioServerPlugInIOOperationReadInput
					       : operation == kAudioServerPlugInIOOperationWriteMix;
	if (willDo)
		*willDo = yes;
	if (inPlace)
		*inPlace = true;
	return kAudioHardwareNoError;
}

OSStatus beginIOOperation(AudioServerPlugInDriverRef, AudioObjectID device, UInt32, UInt32, UInt32,
			  const AudioServerPlugInIOCycleInfo *)
{
	return isDevice(device) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

OSStatus doIOOperation(AudioServerPlugInDriverRef, AudioObjectID device, AudioObjectID, UInt32, UInt32 operation,
		       UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *mainBuffer, void *)
{
	if (!isDevice(device) || !cycle)
		return kAudioHardwareBadObjectError;
	if (!mainBuffer || frames == 0)
		return kAudioHardwareNoError;
	if (device == kObjectFeedDevice && operation == kAudioServerPlugInIOOperationWriteMix) {
		/* Nobody listens: nothing to keep. */
		if (gInputRunning.load(std::memory_order_acquire))
			ring().write((int64_t)cycle->mOutputTime.mSampleTime, static_cast<const float *>(mainBuffer),
				     frames);
	} else if (device == kObjectInputDevice && operation == kAudioServerPlugInIOOperationReadInput) {
		ring().readAndClear((int64_t)cycle->mInputTime.mSampleTime - (int64_t)kLoopbackFrames,
				    static_cast<float *>(mainBuffer), frames);
	}
	return kAudioHardwareNoError;
}

OSStatus endIOOperation(AudioServerPlugInDriverRef, AudioObjectID device, UInt32, UInt32, UInt32,
			const AudioServerPlugInIOCycleInfo *)
{
	return isDevice(device) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

AudioServerPlugInDriverInterface gInterface = {
	nullptr,
	queryInterface,
	addRef,
	release,
	initialize,
	createDevice,
	destroyDevice,
	addDeviceClient,
	removeDeviceClient,
	performConfigurationChange,
	abortConfigurationChange,
	hasProperty,
	isPropertySettable,
	getPropertyDataSize,
	getPropertyData,
	setPropertyData,
	startIO,
	stopIO,
	getZeroTimeStamp,
	willDoIOOperation,
	beginIOOperation,
	doIOOperation,
	endIOOperation,
};
AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

HRESULT queryInterface(void *driver, REFIID uuid, LPVOID *out)
{
	if (driver != gDriverRef || !out)
		return kAudioHardwareBadObjectError;
	CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(nullptr, uuid);
	if (!requested)
		return kAudioHardwareIllegalOperationError;
	HRESULT result = E_NOINTERFACE;
	if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
		gRefCount.fetch_add(1);
		*out = gDriverRef;
		result = S_OK;
	}
	CFRelease(requested);
	return result;
}

ULONG addRef(void *driver)
{
	if (driver != gDriverRef)
		return 0;
	return gRefCount.fetch_add(1) + 1;
}

ULONG release(void *driver)
{
	if (driver != gDriverRef)
		return 0;
	ULONG current = gRefCount.load();
	while (current > 0 && !gRefCount.compare_exchange_weak(current, current - 1)) {
	}
	return current > 0 ? current - 1 : 0;
}

} // namespace

/* CFPlugIn factory named in Info.plist (CFPlugInFactories). */
extern "C" __attribute__((visibility("default"))) void *VerticalAudio_Create(CFAllocatorRef, CFUUIDRef requestedType)
{
	if (!CFEqual(requestedType, kAudioServerPlugInTypeUUID))
		return nullptr;
	gRefCount.fetch_add(1);
	return gDriverRef;
}

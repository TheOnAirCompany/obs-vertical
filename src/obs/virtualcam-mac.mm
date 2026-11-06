/*
Vertical for OBS
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

/* Plugin side of the Vertical Camera: an OBS output fed by the vertical canvas video
 * that enqueues IOSurface-backed CVPixelBuffers into the sink stream of our CoreMediaIO
 * camera extension. CoreMediaIO moves the buffers to the extension process without a
 * copy; the extension forwards them to camera clients (TikTok LIVE Studio, ...).
 *
 * CPU cost: the frame arrives in NV12 at the camera size (libobs converts and scales on
 * the GPU when the canvas output already is NV12, as it is by default); the only CPU
 * work is one memcpy per plane into the pooled pixel buffer. */

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreMediaIO/CMIOHardware.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <obs-module.h>
#include <media-io/video-io.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "virtualcam.hpp"
#include "av-sync.hpp"

#include <util/platform.h>

namespace vertical::virtualcam {

    namespace {

        constexpr OSType kPixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;

        struct VirtualCam {
            obs_output_t *output = nullptr;
            uint32_t width = kDefaultWidth;
            uint32_t height = kDefaultHeight;

            std::mutex lock; /* guards the CoreMedia objects below (video thread vs UI thread) */
            CVPixelBufferPoolRef pool = nullptr;
            CMFormatDescriptionRef format = nullptr;
            CMSimpleQueueRef queue = nullptr;
            CMIODeviceID device = 0;
            CMIOStreamID sink = 0;
            CFStringRef matrix = nullptr; /* YCbCr matrix attachment, from the canvas colorspace */
            std::atomic<bool> running {false};
            uint64_t dropped = 0;
            /* Audio/video offset: frames held back before they go to the sink. They are
             * the pooled IOSurface buffers themselves (no copy); the pool grows to fit. */
            std::deque<CMSampleBufferRef> delayed;
        };

        /* ---------------------------------------------------------------- CMIO lookup */

        std::vector<CMIOObjectID> cmioDevices()
        {
            CMIOObjectPropertyAddress address {kCMIOHardwarePropertyDevices, kCMIOObjectPropertyScopeGlobal,
                                               kCMIOObjectPropertyElementMain};
            UInt32 size = 0;
            if (CMIOObjectGetPropertyDataSize(kCMIOObjectSystemObject, &address, 0, nullptr, &size) != noErr || !size)
                return {};
            std::vector<CMIOObjectID> devices(size / sizeof(CMIOObjectID));
            UInt32 used = 0;
            if (CMIOObjectGetPropertyData(kCMIOObjectSystemObject, &address, 0, nullptr, size, &used, devices.data()) !=
                noErr)
                return {};
            devices.resize(used / sizeof(CMIOObjectID));
            return devices;
        }

        CMIODeviceID findDevice()
        {
            NSString *wanted = [NSString stringWithUTF8String:kDeviceUID];
            for (CMIOObjectID device : cmioDevices()) {
                CMIOObjectPropertyAddress address {kCMIODevicePropertyDeviceUID, kCMIOObjectPropertyScopeGlobal,
                                                   kCMIOObjectPropertyElementMain};
                CFStringRef uid = nullptr;
                UInt32 used = 0;
                if (CMIOObjectGetPropertyData(device, &address, 0, nullptr, sizeof(uid), &used, &uid) != noErr || !uid)
                    continue;
                const bool match = [(__bridge NSString *) uid caseInsensitiveCompare:wanted] == NSOrderedSame;
                CFRelease(uid);
                if (match)
                    return device;
            }
            return 0;
        }

        /* The extension adds the source stream first and the sink second. */
        CMIOStreamID findSinkStream(CMIODeviceID device)
        {
            CMIOObjectPropertyAddress address {kCMIODevicePropertyStreams, kCMIOObjectPropertyScopeGlobal,
                                               kCMIOObjectPropertyElementMain};
            UInt32 size = 0;
            if (CMIOObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr)
                return 0;
            std::vector<CMIOStreamID> streams(size / sizeof(CMIOStreamID));
            if (streams.size() < 2)
                return 0;
            UInt32 used = 0;
            if (CMIOObjectGetPropertyData(device, &address, 0, nullptr, size, &used, streams.data()) != noErr)
                return 0;
            return streams[1];
        }

        void queueAltered(CMIOStreamID, void *, void *) {}

        /* ---------------------------------------------------------------- output callbacks */

        const char *getName(void *)
        {
            return obs_module_text("VirtualCam.Name");
        }

        void getDefaults(obs_data_t *settings)
        {
            obs_data_set_default_int(settings, "width", kDefaultWidth);
            obs_data_set_default_int(settings, "height", kDefaultHeight);
        }

        void *create(obs_data_t *settings, obs_output_t *output)
        {
            auto *vc = new VirtualCam();
            vc->output = output;
            vc->width = (uint32_t) obs_data_get_int(settings, "width");
            vc->height = (uint32_t) obs_data_get_int(settings, "height");
            return vc;
        }

        void releaseMedia(VirtualCam *vc)
        {
            std::lock_guard<std::mutex> guard(vc->lock);
            for (CMSampleBufferRef held : vc->delayed)
                CFRelease(held);
            vc->delayed.clear();
            if (vc->device && vc->sink)
                CMIODeviceStopStream(vc->device, vc->sink);
            if (vc->queue) {
                /* Buffers still waiting were never handed to the extension: release them. */
                while (const void *pending = CMSimpleQueueDequeue(vc->queue))
                    CFRelease(pending);
                CFRelease(vc->queue);
            }
            if (vc->format)
                CFRelease(vc->format);
            if (vc->pool)
                CVPixelBufferPoolRelease(vc->pool);
            vc->queue = nullptr;
            vc->format = nullptr;
            vc->pool = nullptr;
            vc->device = 0;
            vc->sink = 0;
        }

        void destroy(void *data)
        {
            auto *vc = static_cast<VirtualCam *>(data);
            releaseMedia(vc);
            delete vc;
        }

        void update(void *data, obs_data_t *settings)
        {
            /* Takes effect at the next start. */
            auto *vc = static_cast<VirtualCam *>(data);
            vc->width = (uint32_t) obs_data_get_int(settings, "width");
            vc->height = (uint32_t) obs_data_get_int(settings, "height");
        }

        bool start(void *data)
        {
            auto *vc = static_cast<VirtualCam *>(data);
            if (!obs_output_can_begin_data_capture(vc->output, 0))
                return false;

            video_t *video = obs_output_video(vc->output);
            if (!video) {
                obs_output_set_last_error(vc->output, obs_module_text("VirtualCam.Error.NoCanvas"));
                return false;
            }
            const struct video_output_info *info = video_output_get_info(video);
            if (!vc->width || !vc->height) {
                vc->width = info->width;
                vc->height = info->height;
            }
            /* Even sizes: 4:2:0 chroma planes. */
            vc->width &= ~1u;
            vc->height &= ~1u;

            /* NV12, video range, at the camera size. When the canvas already outputs NV12 at
	 * this size libobs passes frames through; otherwise it converts on the GPU. */
            struct video_scale_info conversion = {};
            conversion.format = VIDEO_FORMAT_NV12;
            conversion.width = vc->width;
            conversion.height = vc->height;
            conversion.range = VIDEO_RANGE_PARTIAL;
            conversion.colorspace = info->colorspace == VIDEO_CS_601 ? VIDEO_CS_601 : VIDEO_CS_709;
            obs_output_set_video_conversion(vc->output, &conversion);
            vc->matrix = conversion.colorspace == VIDEO_CS_601 ? kCVImageBufferYCbCrMatrix_ITU_R_601_4
                                                               : kCVImageBufferYCbCrMatrix_ITU_R_709_2;

            bool ready = false;
            {
                std::lock_guard<std::mutex> guard(vc->lock);

                vc->device = findDevice();
                if (!vc->device) {
                    obs_output_set_last_error(vc->output, obs_module_text("VirtualCam.Error.NotInstalled"));
                    return false;
                }
                vc->sink = findSinkStream(vc->device);
                if (!vc->sink) {
                    vc->device = 0;
                    obs_output_set_last_error(vc->output, obs_module_text("VirtualCam.Error.NotStarted"));
                    return false;
                }

                NSDictionary *attributes = @{
                    (id) kCVPixelBufferPixelFormatTypeKey: @(kPixelFormat),
                    (id) kCVPixelBufferWidthKey: @(vc->width),
                    (id) kCVPixelBufferHeightKey: @(vc->height),
                    (id) kCVPixelBufferIOSurfacePropertiesKey: @ {},
                };
                if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef) attributes,
                                            &vc->pool) != kCVReturnSuccess ||
                    CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kPixelFormat, (int32_t) vc->width,
                                                   (int32_t) vc->height, nullptr, &vc->format) != noErr ||
                    CMIOStreamCopyBufferQueue(vc->sink, queueAltered, nullptr, &vc->queue) != noErr ||
                    CMIODeviceStartStream(vc->device, vc->sink) != noErr) {
                    obs_output_set_last_error(vc->output, obs_module_text("VirtualCam.Error.NotStarted"));
                } else {
                    ready = true;
                }
            }
            if (!ready) {
                releaseMedia(vc);
                return false;
            }

            vc->dropped = 0;
            vc->running = true;
            if (!obs_output_begin_data_capture(vc->output, 0)) {
                vc->running = false;
                releaseMedia(vc);
                return false;
            }
            obs_log(LOG_INFO, "virtual camera started (%ux%u)", vc->width, vc->height);
            return true;
        }

        void stop(void *data, uint64_t)
        {
            auto *vc = static_cast<VirtualCam *>(data);
            vc->running = false;
            obs_output_end_data_capture(vc->output);
            releaseMedia(vc);
            obs_log(LOG_INFO, "virtual camera stopped (%llu frames dropped)", (unsigned long long) vc->dropped);
        }

        void copyPlane(uint8_t *dst, size_t dstStride, const uint8_t *src, size_t srcStride, size_t rowBytes,
                       size_t rows)
        {
            if (dstStride == srcStride) {
                memcpy(dst, src, dstStride * rows);
                return;
            }
            for (size_t y = 0; y < rows; y++)
                memcpy(dst + y * dstStride, src + y * srcStride, rowBytes);
        }

        /* Hands one frame to the extension. Live, not buffered: when the extension is
         * behind, the oldest frame in the sink queue is dropped. */
        void enqueue(VirtualCam *vc, CMSampleBufferRef sample)
        {
            if (CMSimpleQueueGetCount(vc->queue) >= CMSimpleQueueGetCapacity(vc->queue)) {
                if (const void *old = CMSimpleQueueDequeue(vc->queue))
                    CFRelease(old);
                vc->dropped++;
            }
            /* On success the queue owns the reference; CoreMediaIO releases it once sent. */
            if (CMSimpleQueueEnqueue(vc->queue, sample) != noErr) {
                CFRelease(sample);
                vc->dropped++;
            }
        }

        void rawVideo(void *data, struct video_data *frame)
        {
            auto *vc = static_cast<VirtualCam *>(data);
            if (!vc->running)
                return;
            vertical::avsync::reportVideoLag((double) (int64_t) (os_gettime_ns() - frame->timestamp) / 1e6);

            std::lock_guard<std::mutex> guard(vc->lock);
            if (!vc->queue || !vc->pool)
                return;

            CVPixelBufferRef buffer = nullptr;
            if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, vc->pool, &buffer) != kCVReturnSuccess) {
                vc->dropped++;
                return;
            }

            CVPixelBufferLockBaseAddress(buffer, 0);
            const size_t planes = CVPixelBufferGetPlaneCount(buffer); /* 2 for NV12 */
            for (size_t p = 0; p < planes && p < MAX_AV_PLANES; p++) {
                if (!frame->data[p])
                    break;
                auto *dst = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(buffer, p));
                const size_t rows = CVPixelBufferGetHeightOfPlane(buffer, p);
                /* NV12: luma is width bytes per row, interleaved chroma too (width/2 pairs). */
                copyPlane(dst, CVPixelBufferGetBytesPerRowOfPlane(buffer, p), frame->data[p], frame->linesize[p],
                          vc->width, rows);
            }
            CVPixelBufferUnlockBaseAddress(buffer, 0);

            CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey, vc->matrix, kCVAttachmentMode_ShouldPropagate);
            CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2,
                                  kCVAttachmentMode_ShouldPropagate);
            CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
                                  kCVAttachmentMode_ShouldPropagate);

            /* OBS timestamps are host-clock nanoseconds, the clock the extension streams use. */
            CMSampleTimingInfo timing = {};
            timing.duration = kCMTimeInvalid;
            timing.presentationTimeStamp = CMTimeMake((int64_t) frame->timestamp, NSEC_PER_SEC);
            timing.decodeTimeStamp = kCMTimeInvalid;

            CMSampleBufferRef sample = nullptr;
            const OSStatus status = CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, buffer, true, nullptr,
                                                                       nullptr, vc->format, &timing, &sample);
            CVPixelBufferRelease(buffer);
            if (status != noErr || !sample) {
                vc->dropped++;
                return;
            }

            /* A negative offset delays the image by whole frames: hold this many. When
             * the delay shrinks, the extra frames are dropped (never sent in a burst). */
            const size_t hold = (size_t) std::max(0, vertical::avsync::videoDelayFrames());
            vc->delayed.push_back(sample);
            while (vc->delayed.size() > hold + 1) {
                CFRelease(vc->delayed.front());
                vc->delayed.pop_front();
            }
            if (vc->delayed.size() > hold) {
                CMSampleBufferRef due = vc->delayed.front();
                vc->delayed.pop_front();
                enqueue(vc, due);
            }
        }

    }  // namespace

    void registerOutput()
    {
        struct obs_output_info info = {};
        info.id = kOutputId;
        info.flags = OBS_OUTPUT_VIDEO;
        info.get_name = getName;
        info.create = create;
        info.destroy = destroy;
        info.start = start;
        info.stop = stop;
        info.raw_video = rawVideo;
        info.update = update;
        info.get_defaults = getDefaults;
        obs_register_output(&info);
    }

    bool isExtensionInstalled()
    {
        @autoreleasepool {
            return findDevice() != 0;
        }
    }

    bool isHostAppPresent()
    {
        return [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:kHostAppPath]];
    }

    bool openHostApp(bool install)
    {
        @autoreleasepool {
            NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:kHostAppPath]];
            if (![[NSFileManager defaultManager] fileExistsAtPath:url.path])
                return false;
            NSWorkspaceOpenConfiguration *config = [NSWorkspaceOpenConfiguration configuration];
            config.arguments = install ? @[@"--install"] : @[];
            config.activates = YES;
            [[NSWorkspace sharedWorkspace] openApplicationAtURL:url configuration:config
                                              completionHandler:^(NSRunningApplication *, NSError *error) {
                                                  if (error)
                                                      obs_log(LOG_WARNING, "cannot open the Vertical Camera app: %s",
                                                              error.localizedDescription.UTF8String);
                                              }];
            return true;
        }
    }

}  // namespace vertical::virtualcam

#endif /* __APPLE__ */

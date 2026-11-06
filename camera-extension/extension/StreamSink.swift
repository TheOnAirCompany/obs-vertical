// Vertical for OBS - Vertical Camera extension
// Copyright (C) 2026 The OnAir Company
// SPDX-License-Identifier: GPL-2.0-or-later

import CoreMediaIO
import Foundation

/// The stream the OBS plugin writes into. The plugin finds the device by its UID, takes
/// the second stream (this one), copies its buffer queue and enqueues CMSampleBuffers.
final class StreamSink: NSObject, CMIOExtensionStreamSource {
    private(set) var stream: CMIOExtensionStream!

    let device: CMIOExtensionDevice
    let formats: [CMIOExtensionStreamFormat]

    private var client: CMIOExtensionClient?
    var activeFormatIndex: Int = 0

    init(localizedName: String, streamID: UUID, formats: [CMIOExtensionStreamFormat], device: CMIOExtensionDevice) {
        self.device = device
        self.formats = formats
        super.init()
        stream = CMIOExtensionStream(
            localizedName: localizedName, streamID: streamID, direction: .sink, clockType: .hostTime, source: self)
    }

    var availableProperties: Set<CMIOExtensionProperty> {
        return [
            .streamActiveFormatIndex,
            .streamFrameDuration,
            .streamSinkBufferQueueSize,
            .streamSinkBuffersRequiredForStartup,
            .streamSinkBufferUnderrunCount,
            .streamSinkEndOfData,
        ]
    }

    func streamProperties(forProperties properties: Set<CMIOExtensionProperty>) throws
        -> CMIOExtensionStreamProperties
    {
        let result = CMIOExtensionStreamProperties(dictionary: [:])
        if properties.contains(.streamActiveFormatIndex) {
            result.activeFormatIndex = activeFormatIndex
        }
        if properties.contains(.streamFrameDuration) {
            result.frameDuration = CMTime(value: 1, timescale: cameraFrameRates.max()!)
        }
        if properties.contains(.streamSinkBufferQueueSize) {
            // Small queue: the camera must stay live, never lag behind the canvas.
            result.sinkBufferQueueSize = 2
        }
        if properties.contains(.streamSinkBuffersRequiredForStartup) {
            result.sinkBuffersRequiredForStartup = 1
        }
        return result
    }

    func setStreamProperties(_ streamProperties: CMIOExtensionStreamProperties) throws {
        if let index = streamProperties.activeFormatIndex, index >= 0, index < formats.count {
            activeFormatIndex = index
        }
    }

    func authorizedToStartStream(for client: CMIOExtensionClient) -> Bool {
        // Any local process may write. Restricting writers (e.g. to signed OBS) is an
        // open question: CMIOExtensionClient exposes the pid and signing identifier.
        self.client = client
        return true
    }

    func startStream() throws {
        guard let deviceSource = device.source as? DeviceSource, let client = client else {
            throw CameraError.noDevice
        }
        deviceSource.startStreamingSink(client: client)
    }

    func stopStream() throws {
        guard let deviceSource = device.source as? DeviceSource else {
            throw CameraError.noDevice
        }
        deviceSource.stopStreamingSink()
    }
}

// Vertical for OBS - Vertical Camera extension
// Copyright (C) 2026 The OnAir Company
// SPDX-License-Identifier: GPL-2.0-or-later

import CoreMediaIO
import Foundation
import os.log

/// The stream camera clients read (TikTok LIVE Studio, browsers, video call apps).
final class StreamSource: NSObject, CMIOExtensionStreamSource {
    private(set) var stream: CMIOExtensionStream!

    let device: CMIOExtensionDevice
    let formats: [CMIOExtensionStreamFormat]

    private let lock = NSLock()
    private var _activeFormatIndex = 0
    private var _frameRate: Int32 = 30

    init(localizedName: String, streamID: UUID, formats: [CMIOExtensionStreamFormat], device: CMIOExtensionDevice) {
        self.device = device
        self.formats = formats
        super.init()
        stream = CMIOExtensionStream(
            localizedName: localizedName, streamID: streamID, direction: .source, clockType: .hostTime, source: self)
    }

    var activeFormatIndex: Int {
        lock.lock()
        defer { lock.unlock() }
        return _activeFormatIndex
    }

    var frameRate: Int32 {
        lock.lock()
        defer { lock.unlock() }
        return _frameRate
    }

    var availableProperties: Set<CMIOExtensionProperty> {
        return [.streamActiveFormatIndex, .streamFrameDuration]
    }

    func streamProperties(forProperties properties: Set<CMIOExtensionProperty>) throws
        -> CMIOExtensionStreamProperties
    {
        let result = CMIOExtensionStreamProperties(dictionary: [:])
        if properties.contains(.streamActiveFormatIndex) {
            result.activeFormatIndex = activeFormatIndex
        }
        if properties.contains(.streamFrameDuration) {
            result.frameDuration = CMTime(value: 1, timescale: frameRate)
        }
        return result
    }

    func setStreamProperties(_ streamProperties: CMIOExtensionStreamProperties) throws {
        var rateChanged = false
        lock.lock()
        if let index = streamProperties.activeFormatIndex, index >= 0, index < formats.count {
            _activeFormatIndex = index
        }
        if let duration = streamProperties.frameDuration, duration.value > 0 {
            let fps = Int32((Double(duration.timescale) / Double(duration.value)).rounded())
            let valid = cameraFrameRates.min(by: { abs($0 - fps) < abs($1 - fps) }) ?? 30
            rateChanged = valid != _frameRate
            _frameRate = valid
        }
        lock.unlock()
        if rateChanged, let deviceSource = device.source as? DeviceSource {
            deviceSource.frameRateChanged()
        }
    }

    func authorizedToStartStream(for client: CMIOExtensionClient) -> Bool {
        return true
    }

    func startStream() throws {
        guard let deviceSource = device.source as? DeviceSource else {
            throw CameraError.noDevice
        }
        deviceSource.startStreaming()
    }

    func stopStream() throws {
        guard let deviceSource = device.source as? DeviceSource else {
            throw CameraError.noDevice
        }
        deviceSource.stopStreaming()
    }
}

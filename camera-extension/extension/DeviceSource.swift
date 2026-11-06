// Vertical for OBS - Vertical Camera extension
// Copyright (C) 2026 The OnAir Company
// SPDX-License-Identifier: GPL-2.0-or-later
//
// One device, two streams:
//   - source: what camera clients (TikTok LIVE Studio, browsers, ...) read;
//   - sink:   what the OBS plugin writes into (CMIOStreamCopyBufferQueue + CMSimpleQueueEnqueue).
// Frames written by the plugin are IOSurface-backed CVPixelBuffers: they cross the
// process boundary without a copy. When the plugin is not sending, the source shows a
// placeholder ("Start the Vertical camera in OBS").

import CoreGraphics
import CoreMediaIO
import CoreText
import Foundation
import IOKit.audio
import VideoToolbox
import os.log

/// Formats offered to camera clients. Portrait only; 4:2:0 bi-planar video range
/// (the native format of the OBS canvas, so the plugin side never converts on the CPU).
struct CameraFormat {
    let width: Int32
    let height: Int32
}

let cameraFormats: [CameraFormat] = [
    CameraFormat(width: 1080, height: 1920),
    CameraFormat(width: 720, height: 1280),
]

let cameraFrameRates: [Int32] = [30, 60]
let cameraPixelFormat: OSType = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange

final class DeviceSource: NSObject, CMIOExtensionDeviceSource {
    private(set) var device: CMIOExtensionDevice!

    private var streamSource: StreamSource!
    private var streamSink: StreamSink!

    /// Every mutable member below is only touched on this queue.
    private let queue = DispatchQueue(label: "company.theonair.vertical-camera.device", qos: .userInteractive)

    private var descriptions: [CMFormatDescription] = []
    private var pools: [CVPixelBufferPool] = []

    private var sourceClients = 0
    private var sinkRunning = false
    private var lastSinkFrameNs: UInt64 = 0

    private var placeholderTimer: DispatchSourceTimer?
    private var consumeTimer: DispatchSourceTimer?
    private var placeholders: [Int: CVPixelBuffer] = [:]
    private var transfer: VTPixelTransferSession?
    private var sampleDescription: CMFormatDescription?
    private var sentFrames: UInt64 = 0

    /// The plugin is considered gone when no frame arrived for this long.
    private let sinkTimeoutNs: UInt64 = 500_000_000

    init(localizedName: String, deviceUUID: UUID, sourceUUID: UUID, sinkUUID: UUID) {
        super.init()

        device = CMIOExtensionDevice(
            localizedName: localizedName, deviceID: deviceUUID, legacyDeviceID: nil, source: self)

        var streamFormats: [CMIOExtensionStreamFormat] = []
        for format in cameraFormats {
            var description: CMFormatDescription?
            CMVideoFormatDescriptionCreate(
                allocator: kCFAllocatorDefault,
                codecType: cameraPixelFormat,
                width: format.width,
                height: format.height,
                extensions: nil,
                formatDescriptionOut: &description
            )
            guard let description = description else {
                fatalError("Cannot create the \(format.width)x\(format.height) format description")
            }
            descriptions.append(description)

            let attributes: NSDictionary = [
                kCVPixelBufferWidthKey: format.width,
                kCVPixelBufferHeightKey: format.height,
                kCVPixelBufferPixelFormatTypeKey: cameraPixelFormat,
                kCVPixelBufferIOSurfacePropertiesKey: [CFString: CFTypeRef](),
            ]
            var pool: CVPixelBufferPool?
            CVPixelBufferPoolCreate(kCFAllocatorDefault, nil, attributes, &pool)
            guard let pool = pool else {
                fatalError("Cannot create the \(format.width)x\(format.height) pixel buffer pool")
            }
            pools.append(pool)

            let durations = cameraFrameRates.map { CMTime(value: 1, timescale: $0) }
            streamFormats.append(
                CMIOExtensionStreamFormat(
                    formatDescription: description,
                    maxFrameDuration: CMTime(value: 1, timescale: cameraFrameRates.min()!),
                    minFrameDuration: CMTime(value: 1, timescale: cameraFrameRates.max()!),
                    validFrameDurations: durations
                ))
        }

        streamSource = StreamSource(
            localizedName: "Vertical Camera", streamID: sourceUUID, formats: streamFormats, device: device)
        streamSink = StreamSink(
            localizedName: "Vertical Camera Input", streamID: sinkUUID, formats: streamFormats, device: device)

        do {
            // Order matters: the plugin takes the second stream of the device as the sink.
            try device.addStream(streamSource.stream)
            try device.addStream(streamSink.stream)
        } catch {
            fatalError("Failed to add the streams: \(error.localizedDescription)")
        }

        VTPixelTransferSessionCreate(allocator: kCFAllocatorDefault, pixelTransferSessionOut: &transfer)
    }

    // MARK: Device properties

    var availableProperties: Set<CMIOExtensionProperty> {
        return [.deviceTransportType, .deviceModel]
    }

    func deviceProperties(forProperties properties: Set<CMIOExtensionProperty>) throws
        -> CMIOExtensionDeviceProperties
    {
        let result = CMIOExtensionDeviceProperties(dictionary: [:])
        if properties.contains(.deviceTransportType) {
            result.transportType = kIOAudioDeviceTransportTypeVirtual
        }
        if properties.contains(.deviceModel) {
            result.model = "Vertical Camera"
        }
        return result
    }

    func setDeviceProperties(_ deviceProperties: CMIOExtensionDeviceProperties) throws {}

    // MARK: Source stream (camera clients)

    func startStreaming() {
        queue.async {
            self.sourceClients += 1
            if self.placeholderTimer == nil {
                self.startPlaceholderTimer()
            }
        }
    }

    func stopStreaming() {
        queue.async {
            self.sourceClients = max(0, self.sourceClients - 1)
            if self.sourceClients == 0 {
                self.placeholderTimer?.cancel()
                self.placeholderTimer = nil
            }
        }
    }

    /// Restart the placeholder timer at the new rate when a client changes the frame rate.
    func frameRateChanged() {
        queue.async {
            guard self.placeholderTimer != nil else { return }
            self.placeholderTimer?.cancel()
            self.placeholderTimer = nil
            self.startPlaceholderTimer()
        }
    }

    private func startPlaceholderTimer() {
        let timer = DispatchSource.makeTimerSource(flags: .strict, queue: queue)
        let fps = streamSource.frameRate
        timer.schedule(deadline: .now(), repeating: 1.0 / Double(fps), leeway: .milliseconds(1))
        timer.setEventHandler { [weak self] in
            self?.sendPlaceholderIfIdle()
        }
        placeholderTimer = timer
        timer.resume()
    }

    private func sendPlaceholderIfIdle() {
        let now = DispatchTime.now().uptimeNanoseconds
        if sinkRunning && now - lastSinkFrameNs < sinkTimeoutNs {
            return
        }
        let index = streamSource.activeFormatIndex
        guard let buffer = placeholder(for: index) else { return }
        send(buffer, formatIndex: index)
    }

    // MARK: Sink stream (the OBS plugin)

    func startStreamingSink(client: CMIOExtensionClient) {
        queue.async { [weak self] in
            guard let self = self else { return }
            self.sinkRunning = true
            self.consumeTimer?.cancel()
            let timer = DispatchSource.makeTimerSource(flags: .strict, queue: self.queue)
            // Poll faster than the highest frame rate so a frame never waits a full period.
            timer.schedule(
                deadline: .now(), repeating: 1.0 / (Double(cameraFrameRates.max()!) * 2.0),
                leeway: .milliseconds(1))
            timer.setEventHandler { [weak self] in
                self?.consume(from: client)
            }
            self.consumeTimer = timer
            timer.resume()
        }
    }

    func stopStreamingSink() {
        queue.async {
            self.sinkRunning = false
            self.consumeTimer?.cancel()
            self.consumeTimer = nil
        }
    }

    private func consume(from client: CMIOExtensionClient) {
        guard sinkRunning else { return }
        streamSink.stream.consumeSampleBuffer(from: client) { [weak self] sampleBuffer, sequence, _, _, error in
            guard let self = self else { return }
            if let error = error {
                os_log(.debug, log: extensionLog, "consumeSampleBuffer: %{public}@", error.localizedDescription)
                return
            }
            guard let sampleBuffer = sampleBuffer else { return }
            self.queue.async {
                self.lastSinkFrameNs = DispatchTime.now().uptimeNanoseconds
                let hostNs = self.hostTimeNs()
                if self.sourceClients > 0, let image = CMSampleBufferGetImageBuffer(sampleBuffer) {
                    self.send(image, formatIndex: self.streamSource.activeFormatIndex)
                }
                self.streamSink.stream.notifyScheduledOutputChanged(
                    CMIOExtensionScheduledOutput(sequenceNumber: sequence, hostTimeInNanoseconds: hostNs))
            }
        }
    }

    // MARK: Frames

    private func hostTimeNs() -> UInt64 {
        let now = CMClockGetTime(CMClockGetHostTimeClock())
        return UInt64(max(0, now.seconds) * Double(NSEC_PER_SEC))
    }

    /// Sends a frame to the camera clients, scaled to the active format when the plugin
    /// writes another size (VTPixelTransferSession runs on the GPU).
    private func send(_ image: CVPixelBuffer, formatIndex: Int) {
        guard formatIndex >= 0 && formatIndex < descriptions.count else { return }
        let format = cameraFormats[formatIndex]
        var output = image
        if CVPixelBufferGetWidth(image) != Int(format.width) || CVPixelBufferGetHeight(image) != Int(format.height)
            || CVPixelBufferGetPixelFormatType(image) != cameraPixelFormat
        {
            var scaled: CVPixelBuffer?
            CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pools[formatIndex], &scaled)
            guard let scaled = scaled, let transfer = transfer,
                VTPixelTransferSessionTransferImage(transfer, from: image, to: scaled) == noErr
            else {
                return
            }
            output = scaled
        }

        let hostNs = hostTimeNs()
        var timing = CMSampleTimingInfo(
            duration: CMTime(value: 1, timescale: streamSource.frameRate),
            presentationTimeStamp: CMTime(value: CMTimeValue(hostNs), timescale: CMTimeScale(NSEC_PER_SEC)),
            decodeTimeStamp: .invalid)
        // The sample's format description must match the image buffer, attachments
        // included (YCbCr matrix, primaries, transfer function set by OBS or by the pixel
        // transfer): the plain stream format does not, and CoreMedia refuses the sample.
        guard let description = sampleDescription(for: output) else { return }
        var sample: CMSampleBuffer?
        let status = CMSampleBufferCreateForImageBuffer(
            allocator: kCFAllocatorDefault,
            imageBuffer: output,
            dataReady: true,
            makeDataReadyCallback: nil,
            refcon: nil,
            formatDescription: description,
            sampleTiming: &timing,
            sampleBufferOut: &sample
        )
        guard status == noErr, let sample = sample else {
            os_log(.error, log: extensionLog, "cannot make a sample buffer: %d", status)
            return
        }
        streamSource.stream.send(sample, discontinuity: [], hostTimeInNanoseconds: hostNs)
        sentFrames += 1
        if sentFrames == 1 {
            os_log(.info, log: extensionLog, "first frame sent to the camera clients")
        }
    }

    /// Centered, wrapped text drawn with Core Text into a bitmap context.
    private func drawCentered(
        _ text: String, in context: CGContext, rect: CGRect, size: CGFloat, bold: Bool, white: CGFloat
    ) {
        let font =
            CTFontCreateUIFontForLanguage(bold ? .emphasizedSystem : .system, size, nil)
            ?? CTFontCreateWithName("Helvetica" as CFString, size, nil)
        var alignment = CTTextAlignment.center
        let paragraph = withUnsafePointer(to: &alignment) {
            CTParagraphStyleCreate(
                [CTParagraphStyleSetting(spec: .alignment, valueSize: MemoryLayout<CTTextAlignment>.size, value: $0)],
                1)
        }
        let attributes: [CFString: Any] = [
            kCTFontAttributeName: font,
            kCTForegroundColorAttributeName: CGColor(gray: white, alpha: 1),
            kCTParagraphStyleAttributeName: paragraph,
        ]
        let string = CFAttributedStringCreate(nil, text as CFString, attributes as CFDictionary)!
        let setter = CTFramesetterCreateWithAttributedString(string)
        let frame = CTFramesetterCreateFrame(
            setter, CFRange(location: 0, length: 0), CGPath(rect: rect, transform: nil), nil)
        CTFrameDraw(frame, context)
    }

    /// Format description of the samples made from `image`: kept while it matches.
    private func sampleDescription(for image: CVPixelBuffer) -> CMFormatDescription? {
        if let current = sampleDescription, CMVideoFormatDescriptionMatchesImageBuffer(current, imageBuffer: image) {
            return current
        }
        var description: CMFormatDescription?
        CMVideoFormatDescriptionCreateForImageBuffer(
            allocator: kCFAllocatorDefault, imageBuffer: image, formatDescriptionOut: &description)
        sampleDescription = description
        return description
    }

    /// Black frame with a short message, drawn once per format and kept.
    private func placeholder(for index: Int) -> CVPixelBuffer? {
        if let cached = placeholders[index] {
            return cached
        }
        let format = cameraFormats[index]
        let width = Int(format.width)
        let height = Int(format.height)

        var bgra: CVPixelBuffer?
        let attributes: NSDictionary = [kCVPixelBufferIOSurfacePropertiesKey: [CFString: CFTypeRef]()]
        CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_32BGRA, attributes, &bgra)
        guard let bgra = bgra else { return nil }

        CVPixelBufferLockBaseAddress(bgra, [])
        if let base = CVPixelBufferGetBaseAddress(bgra),
            let context = CGContext(
                data: base, width: width, height: height, bitsPerComponent: 8,
                bytesPerRow: CVPixelBufferGetBytesPerRow(bgra), space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        {
            context.setFillColor(CGColor(red: 0.07, green: 0.08, blue: 0.10, alpha: 1))
            context.fill(CGRect(x: 0, y: 0, width: width, height: height))

            // Core Text only: the extension runs without access to the window server, and
            // AppKit text drawing (NSFont, NSString.draw) asks for it and blocks.
            let mid = CGFloat(height) / 2
            drawCentered(
                "Vertical Camera", in: context,
                rect: CGRect(x: 0, y: mid, width: CGFloat(width), height: CGFloat(width) * 0.09),
                size: CGFloat(width) * 0.06, bold: true, white: 0.92)
            drawCentered(
                "Start the virtual camera in the Vertical dock of OBS", in: context,
                rect: CGRect(
                    x: CGFloat(width) * 0.1, y: mid - CGFloat(width) * 0.12, width: CGFloat(width) * 0.8,
                    height: CGFloat(width) * 0.11),
                size: CGFloat(width) * 0.035, bold: false, white: 0.62)
        }
        CVPixelBufferUnlockBaseAddress(bgra, [])

        var nv12: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pools[index], &nv12)
        guard let nv12 = nv12, let transfer = transfer,
            VTPixelTransferSessionTransferImage(transfer, from: bgra, to: nv12) == noErr
        else {
            return nil
        }
        placeholders[index] = nv12
        return nv12
    }
}

enum CameraError: Error {
    case noDevice
}

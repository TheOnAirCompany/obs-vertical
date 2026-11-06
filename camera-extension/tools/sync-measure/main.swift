// Vertical for OBS - sync measurement tool
// Copyright (C) 2026 The OnAir Company
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Opens "Vertical Camera" and "Vertical Audio" like any app would (AVFoundation), asks
// the plugin for its Sync test (a white flash and a 1 kHz beep stamped at the same OBS
// time), finds the flash in the frames and the beep in the audio, and prints:
//
//   offset    audio onset - flash onset (positive: the sound comes after the image)
//   audio     OBS time of the beep -> its time stamp at the "Vertical Audio" input
//   video     OBS time of the flash -> its time stamp at the "Vertical Camera" output
//
// The plugin listens for the trigger only while its Settings dialog is open (or when OBS
// runs with VERTICAL_SYNC_REMOTE=1) and writes the OBS time of each test to
// $TMPDIR/vertical-sync-test.json. Both sides use the host clock.
//
//   vertical-sync-measure [--count 5] [--interval 3] [--audio-only] [--no-trigger]

import AVFoundation
import CoreMedia
import Foundation
import notify

setvbuf(stdout, nil, _IOLBF, 0)

// MARK: Options

var count = 5
var interval = 3.0
var audioOnly = false
var trigger = true
var arguments = CommandLine.arguments.dropFirst().makeIterator()
while let arg = arguments.next() {
    switch arg {
    case "--count": count = Int(arguments.next() ?? "") ?? count
    case "--interval": interval = Double(arguments.next() ?? "") ?? interval
    case "--audio-only": audioOnly = true
    case "--no-trigger": trigger = false
    default:
        print("usage: vertical-sync-measure [--count N] [--interval s] [--audio-only] [--no-trigger]")
        exit(2)
    }
}

let notificationName = "company.theonair.vertical.sync-test"
let eventFile = (NSTemporaryDirectory() as NSString).appendingPathComponent("vertical-sync-test.json")

func hostSeconds() -> Double {
    CMClockGetTime(CMClockGetHostTimeClock()).seconds
}

/// OBS time (host clock, seconds) of the last sync test, from the plugin's file.
func lastEventTime() -> Double? {
    guard let data = FileManager.default.contents(atPath: eventFile),
        let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
        let ns = json["t0_ns"] as? NSNumber
    else { return nil }
    return ns.doubleValue / 1e9
}

// MARK: Devices

func findDevice(_ type: AVMediaType, name: String, uid: String?) -> AVCaptureDevice? {
    var types: [AVCaptureDevice.DeviceType] = []
    if #available(macOS 14.0, *) {
        types = type == .video ? [.external, .builtInWideAngleCamera] : [.microphone]
    } else {
        types = type == .video ? [.externalUnknown, .builtInWideAngleCamera] : [.builtInMicrophone, .externalUnknown]
    }
    let found = AVCaptureDevice.DiscoverySession(deviceTypes: types, mediaType: type, position: .unspecified).devices
    return found.first { ($0.uniqueID == uid) || $0.localizedName == name }
}

func requestAccess(_ type: AVMediaType) -> Bool {
    if AVCaptureDevice.authorizationStatus(for: type) == .authorized { return true }
    let done = DispatchSemaphore(value: 0)
    var granted = false
    AVCaptureDevice.requestAccess(for: type) {
        granted = $0
        done.signal()
    }
    done.wait()
    return granted
}

// MARK: Detection

final class Detector: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate,
    AVCaptureAudioDataOutputSampleBufferDelegate
{
    let lock = NSLock()
    var flashes: [(pts: Double, arrival: Double)] = []
    var beeps: [(pts: Double, arrival: Double)] = []
    private var lastLuma: Double = 0
    private var quietSince: Double = 0
    private var inBeep = false
    var audioFrames = 0
    var videoFrames = 0
    var lastAudioArrivalMinusPts: Double = 0

    func captureOutput(
        _ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection
    ) {
        let arrival = hostSeconds()
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer).seconds
        if let image = CMSampleBufferGetImageBuffer(sampleBuffer) {
            video(image, pts: pts, arrival: arrival)
        } else {
            audio(sampleBuffer, pts: pts, arrival: arrival)
        }
    }

    private func video(_ image: CVPixelBuffer, pts: Double, arrival: Double) {
        CVPixelBufferLockBaseAddress(image, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(image, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddressOfPlane(image, 0) else { return }
        let width = CVPixelBufferGetWidthOfPlane(image, 0)
        let height = CVPixelBufferGetHeightOfPlane(image, 0)
        let stride = CVPixelBufferGetBytesPerRowOfPlane(image, 0)
        let luma = base.assumingMemoryBound(to: UInt8.self)
        var sum = 0
        var n = 0
        // Centre region, every 4th pixel.
        for y in Swift.stride(from: height / 2 - 64, to: height / 2 + 64, by: 4) {
            for x in Swift.stride(from: width / 2 - 64, to: width / 2 + 64, by: 4) {
                sum += Int(luma[y * stride + x])
                n += 1
            }
        }
        let mean = Double(sum) / Double(max(n, 1))
        lock.lock()
        videoFrames += 1
        if mean > 200 && lastLuma < 150 {
            flashes.append((pts, arrival))
        }
        lastLuma = mean
        lock.unlock()
    }

    private func audio(_ sample: CMSampleBuffer, pts: Double, arrival: Double) {
        guard let format = CMSampleBufferGetFormatDescription(sample),
            let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(format)?.pointee
        else { return }
        let rate = asbd.mSampleRate
        let frames = CMSampleBufferGetNumSamples(sample)
        var list = AudioBufferList()
        var block: CMBlockBuffer?
        guard
            CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
                sample, bufferListSizeNeededOut: nil, bufferListOut: &list,
                bufferListSize: MemoryLayout<AudioBufferList>.size, blockBufferAllocator: nil,
                blockBufferMemoryAllocator: nil, flags: 0, blockBufferOut: &block) == noErr,
            let data = list.mBuffers.mData
        else { return }
        let isFloat = asbd.mFormatFlags & kAudioFormatFlagIsFloat != 0
        let channels = Int(asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0 ? 1 : asbd.mChannelsPerFrame)
        lock.lock()
        defer { lock.unlock() }
        audioFrames += frames
        lastAudioArrivalMinusPts = arrival - pts
        for i in 0..<frames {
            let v: Double
            if isFloat {
                v = Double(data.assumingMemoryBound(to: Float32.self)[i * channels])
            } else if asbd.mBitsPerChannel == 16 {
                v = Double(data.assumingMemoryBound(to: Int16.self)[i * channels]) / 32768.0
            } else {
                v = Double(data.assumingMemoryBound(to: Int32.self)[i * channels]) / 2147483648.0
            }
            let t = pts + Double(i) / rate
            if abs(v) > 0.2 {
                if !inBeep && t - quietSince > 0.2 {
                    beeps.append((t, arrival))
                }
                inBeep = true
                quietSince = t
            } else if inBeep && t - quietSince > 0.05 {
                inBeep = false
            }
        }
    }

    func take() -> (flash: (pts: Double, arrival: Double)?, beep: (pts: Double, arrival: Double)?) {
        lock.lock()
        defer { lock.unlock() }
        let f = flashes.last
        let b = beeps.last
        flashes.removeAll()
        beeps.removeAll()
        return (f, b)
    }
}

// MARK: Session

guard requestAccess(.audio), audioOnly || requestAccess(.video) else {
    print("Camera or microphone access denied (System Settings > Privacy & Security).")
    exit(1)
}
guard let mic = findDevice(.audio, name: "Vertical Audio", uid: "company.theonair.vertical-audio.input") else {
    print("\"Vertical Audio\" not found: is VerticalAudio.driver installed in /Library/Audio/Plug-Ins/HAL?")
    exit(1)
}
let camera = audioOnly ? nil : findDevice(.video, name: "Vertical Camera", uid: nil)
if !audioOnly && camera == nil {
    print("\"Vertical Camera\" not found: activate it with Vertical Camera.app (or use --audio-only).")
    exit(1)
}

let session = AVCaptureSession()
let detector = Detector()
let queue = DispatchQueue(label: "vertical-sync-measure", qos: .userInteractive)
session.beginConfiguration()
do {
    session.addInput(try AVCaptureDeviceInput(device: mic))
    if let camera = camera {
        session.addInput(try AVCaptureDeviceInput(device: camera))
    }
} catch {
    print("Cannot open the devices: \(error.localizedDescription)")
    exit(1)
}
let audioOut = AVCaptureAudioDataOutput()
audioOut.audioSettings = [
    AVFormatIDKey: kAudioFormatLinearPCM, AVLinearPCMIsFloatKey: true, AVLinearPCMBitDepthKey: 32,
    AVLinearPCMIsNonInterleaved: false, AVSampleRateKey: 48000, AVNumberOfChannelsKey: 2,
]
audioOut.setSampleBufferDelegate(detector, queue: queue)
session.addOutput(audioOut)
if camera != nil {
    let videoOut = AVCaptureVideoDataOutput()
    videoOut.videoSettings = [
        kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
    ]
    videoOut.alwaysDiscardsLateVideoFrames = true
    videoOut.setSampleBufferDelegate(detector, queue: queue)
    session.addOutput(videoOut)
}
session.commitConfiguration()
session.startRunning()
print("Listening to \"\(mic.localizedName)\"" + (camera.map { " and \"\($0.localizedName)\"" } ?? "") + ".")
Thread.sleep(forTimeInterval: 1.5)

// MARK: Measure

var offsets: [Double] = []
var audioLatencies: [Double] = []
var videoLatencies: [Double] = []
for i in 1...count {
    _ = detector.take()
    let before = lastEventTime()
    if trigger {
        notify_post(notificationName)
    } else {
        print("Click \"Sync test\" in the Vertical settings…")
    }
    Thread.sleep(forTimeInterval: interval)
    let (flash, beep) = detector.take()
    let t0 = lastEventTime()
    let fresh = t0 != nil && t0 != before
    var line = String(format: "#%d", i)
    if let beep = beep, let flash = flash {
        let offset = (beep.pts - flash.pts) * 1000
        offsets.append(offset)
        line += String(format: "  offset %+.1f ms (by arrival %+.1f ms)", offset, (beep.arrival - flash.arrival) * 1000)
    }
    if let beep = beep {
        if fresh, let t0 = t0 {
            audioLatencies.append((beep.pts - t0) * 1000)
            line += String(format: "  audio %.1f ms", (beep.pts - t0) * 1000)
        }
    } else {
        line += "  no beep"
    }
    if let flash = flash {
        if fresh, let t0 = t0 {
            videoLatencies.append((flash.pts - t0) * 1000)
            line += String(format: "  video %.1f ms", (flash.pts - t0) * 1000)
        }
    } else if camera != nil {
        line += "  no flash"
    }
    if !fresh {
        line += "  (no sync test from OBS: is the Vertical settings dialog open?)"
    }
    print(line)
}
session.stopRunning()

func summary(_ name: String, _ values: [Double]) {
    guard !values.isEmpty else { return }
    let sorted = values.sorted()
    let mean = values.reduce(0, +) / Double(values.count)
    print(
        String(
            format: "%@: median %.1f ms, mean %.1f ms, min %.1f, max %.1f (n=%d)", name, sorted[sorted.count / 2], mean,
            sorted.first!, sorted.last!, values.count))
}
print("")
summary("audio/video offset (+ = sound after image)", offsets)
summary("OBS -> Vertical Audio", audioLatencies)
summary("OBS -> Vertical Camera", videoLatencies)
print(String(format: "audio frames %d, video frames %d", detector.audioFrames, detector.videoFrames))
exit(offsets.isEmpty && audioLatencies.isEmpty ? 1 : 0)

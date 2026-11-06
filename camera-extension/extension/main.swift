// Vertical for OBS - Vertical Camera extension
// Copyright (C) 2026 The OnAir Company
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Entry point of the CoreMediaIO Camera Extension. The device, source and sink UUIDs
// come from Info.plist (VerticalCameraDeviceUUID, ...): the plugin finds the device by
// the same device UUID (kDeviceUID in src/obs/virtualcam.hpp).

import CoreMediaIO
import Foundation
import os.log

let extensionLog = OSLog(subsystem: "company.theonair.vertical-camera.extension", category: "camera")

private func plistUUID(_ key: String) -> UUID {
    guard let text = Bundle.main.object(forInfoDictionaryKey: key) as? String, let uuid = UUID(uuidString: text)
    else {
        fatalError("Missing or invalid \(key) in Info.plist")
    }
    return uuid
}

let providerSource = ProviderSource(
    clientQueue: nil,
    deviceUUID: plistUUID("VerticalCameraDeviceUUID"),
    sourceUUID: plistUUID("VerticalCameraSourceUUID"),
    sinkUUID: plistUUID("VerticalCameraSinkUUID")
)

CMIOExtensionProvider.startService(provider: providerSource.provider)
CFRunLoopRun()

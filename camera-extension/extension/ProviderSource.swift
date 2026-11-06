// Vertical for OBS - Vertical Camera extension
// Copyright (C) 2026 The OnAir Company
// SPDX-License-Identifier: GPL-2.0-or-later

import CoreMediaIO
import Foundation

/// The provider owns one device: "Vertical Camera".
final class ProviderSource: NSObject, CMIOExtensionProviderSource {
    private(set) var provider: CMIOExtensionProvider!
    private var deviceSource: DeviceSource!

    init(clientQueue: DispatchQueue?, deviceUUID: UUID, sourceUUID: UUID, sinkUUID: UUID) {
        super.init()
        provider = CMIOExtensionProvider(source: self, clientQueue: clientQueue)
        deviceSource = DeviceSource(
            localizedName: "Vertical Camera",
            deviceUUID: deviceUUID,
            sourceUUID: sourceUUID,
            sinkUUID: sinkUUID
        )
        do {
            try provider.addDevice(deviceSource.device)
        } catch {
            fatalError("Failed to add the Vertical Camera device: \(error.localizedDescription)")
        }
    }

    func connect(to client: CMIOExtensionClient) throws {}

    func disconnect(from client: CMIOExtensionClient) {}

    var availableProperties: Set<CMIOExtensionProperty> {
        return [.providerName, .providerManufacturer]
    }

    func providerProperties(forProperties properties: Set<CMIOExtensionProperty>) throws
        -> CMIOExtensionProviderProperties
    {
        let result = CMIOExtensionProviderProperties(dictionary: [:])
        if properties.contains(.providerName) {
            result.name = "Vertical for OBS"
        }
        if properties.contains(.providerManufacturer) {
            result.manufacturer = "The OnAir Company"
        }
        return result
    }

    func setProviderProperties(_ providerProperties: CMIOExtensionProviderProperties) throws {}
}

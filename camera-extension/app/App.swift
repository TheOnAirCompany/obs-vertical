// Vertical for OBS - Vertical Camera host app
// Copyright (C) 2026 The OnAir Company
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A system extension can only be activated by the app bundle that embeds it
// (Contents/Library/SystemExtensions), and that app must live in /Applications.
// This tiny app does exactly that: Install / Uninstall the Vertical Camera.

import AppKit
import SystemExtensions

let extensionIdentifier = "company.theonair.vertical-camera.extension"

final class ExtensionManager: NSObject, OSSystemExtensionRequestDelegate {
    var onStatus: ((String) -> Void)?

    func activate() {
        let request = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: extensionIdentifier, queue: .main)
        request.delegate = self
        onStatus?("Installing...")
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func deactivate() {
        let request = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: extensionIdentifier, queue: .main)
        request.delegate = self
        onStatus?("Uninstalling...")
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func request(
        _ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties,
        withExtension ext: OSSystemExtensionProperties
    ) -> OSSystemExtensionRequest.ReplacementAction {
        // Always take the version shipped with this app (upgrades and downgrades).
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        onStatus?(
            "Approve the Vertical Camera in System Settings > General > Login Items & Extensions > Camera Extensions.")
    }

    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        switch result {
        case .completed:
            onStatus?("Done. Restart the apps that use cameras to see the change.")
        case .willCompleteAfterReboot:
            onStatus?("Done. Restart the Mac to complete.")
        @unknown default:
            onStatus?("Done.")
        }
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        let nsError = error as NSError
        var message = "Failed: \(nsError.localizedDescription) (code \(nsError.code))."
        if nsError.domain == OSSystemExtensionErrorDomain
            && nsError.code == OSSystemExtensionError.unsupportedParentBundleLocation.rawValue
        {
            message = "Move Vertical Camera.app to the Applications folder, then try again."
        }
        onStatus?(message)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow!
    private let manager = ExtensionManager()
    private let status = NSTextField(wrappingLabelWithString: "")

    func applicationDidFinishLaunching(_ notification: Notification) {
        let title = NSTextField(labelWithString: "Vertical Camera")
        title.font = .systemFont(ofSize: 18, weight: .semibold)

        let text = NSTextField(
            wrappingLabelWithString:
                "Adds a 9:16 camera named \"Vertical Camera\" to macOS. Start it from the Vertical dock in OBS, then pick it in TikTok LIVE Studio or any app that uses a camera."
        )
        text.textColor = .secondaryLabelColor

        let install = NSButton(title: "Install", target: self, action: #selector(install))
        install.keyEquivalent = "\r"
        let uninstall = NSButton(title: "Uninstall", target: self, action: #selector(uninstall))
        let buttons = NSStackView(views: [install, uninstall])
        buttons.spacing = 8

        status.textColor = .secondaryLabelColor
        status.preferredMaxLayoutWidth = 380

        let stack = NSStackView(views: [title, text, buttons, status])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 12
        stack.edgeInsets = NSEdgeInsets(top: 20, left: 20, bottom: 20, right: 20)
        stack.translatesAutoresizingMaskIntoConstraints = false
        text.preferredMaxLayoutWidth = 380

        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 420, height: 220), styleMask: [.titled, .closable],
            backing: .buffered, defer: false)
        window.title = "Vertical Camera"
        window.contentView = NSView()
        window.contentView!.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: window.contentView!.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: window.contentView!.trailingAnchor),
            stack.topAnchor.constraint(equalTo: window.contentView!.topAnchor),
            stack.bottomAnchor.constraint(lessThanOrEqualTo: window.contentView!.bottomAnchor),
        ])
        window.center()
        window.makeKeyAndOrderFront(nil)

        manager.onStatus = { [weak self] message in self?.status.stringValue = message }
        if !Bundle.main.bundlePath.hasPrefix("/Applications/") {
            status.stringValue = "Move this app to the Applications folder before installing."
        }

        // "Vertical Camera.app --install" / "--uninstall": used by the .pkg postinstall
        // script and by the OBS plugin ("Install the camera" button).
        let args = CommandLine.arguments
        if args.contains("--install") {
            manager.activate()
        } else if args.contains("--uninstall") {
            manager.deactivate()
        }
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return true
    }

    @objc private func install() { manager.activate() }
    @objc private func uninstall() { manager.deactivate() }
}

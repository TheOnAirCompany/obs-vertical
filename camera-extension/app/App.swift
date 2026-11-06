// Vertical for OBS - Vertical Camera host app
// Copyright (C) 2026 The OnAir Company
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A system extension can only be activated by the app bundle that embeds it
// (Contents/Library/SystemExtensions), and that app must live in /Applications.
// This tiny app does exactly that: Install / Uninstall the Vertical Camera.

import AVFoundation
import AppKit
import SystemExtensions

let extensionIdentifier = "company.theonair.vertical-camera.extension"

/// Installs, updates and removes the camera extension.
///
/// Updating over an activated extension: macOS swaps the build by itself when the app is
/// replaced, but the camera then disappears from the apps until a reboot. What works is a
/// real deactivation followed by a new activation. So an install first asks which builds
/// are installed (an older enabled one is deactivated first), then activates the bundled
/// one and waits for the "Vertical Camera" device: "Ready" only once apps can see it.
/// When it does not appear, or the activation would wait for a reboot, the extension is
/// deactivated and activated again, once; if the device is still missing, a restart is
/// needed.
final class ExtensionManager: NSObject, OSSystemExtensionRequestDelegate {
    var onStatus: ((String) -> Void)?
    var onFinished: ((Bool) -> Void)?

    private enum Step {
        case idle, inspecting, removingOld, installing, uninstalling
    }
    private var step = Step.idle
    private var fellBack = false

    /// Version of the extension embedded in this app ("0.3.0 (3)").
    let bundledVersion: (short: String, build: String) = {
        let url = Bundle.main.bundleURL.appendingPathComponent(
            "Contents/Library/SystemExtensions/\(extensionIdentifier).systemextension")
        let info = Bundle(url: url)?.infoDictionary ?? [:]
        return (info["CFBundleShortVersionString"] as? String ?? "?", info["CFBundleVersion"] as? String ?? "?")
    }()

    func activate() {
        fellBack = false
        step = .inspecting
        onStatus?("Checking the installed camera...")
        let request = OSSystemExtensionRequest.propertiesRequest(
            forExtensionWithIdentifier: extensionIdentifier, queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func deactivate() {
        step = .uninstalling
        submitDeactivation()
        onStatus?("Uninstalling...")
    }

    private func submitActivation() {
        step = .installing
        onStatus?("Installing \(bundledVersion.short) (\(bundledVersion.build))...")
        let request = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: extensionIdentifier, queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    private func submitDeactivation() {
        let request = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: extensionIdentifier, queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func request(_ request: OSSystemExtensionRequest, foundProperties properties: [OSSystemExtensionProperties]) {
        guard step == .inspecting else { return }
        let older = properties.filter {
            $0.isEnabled && !$0.isUninstalling
                && ($0.bundleVersion != bundledVersion.build || $0.bundleShortVersion != bundledVersion.short)
        }
        if let old = older.first {
            step = .removingOld
            onStatus?(
                "Updating the camera from \(old.bundleShortVersion) (\(old.bundleVersion)): removing the old version..."
            )
            submitDeactivation()
        } else {
            submitActivation()
        }
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

    /// True when apps can see the camera (its device is published).
    static func cameraVisible() -> Bool {
        let uuid = Bundle.main.bundleURL.appendingPathComponent(
            "Contents/Library/SystemExtensions/\(extensionIdentifier).systemextension"
        )
        let deviceUID =
            (Bundle(url: uuid)?.infoDictionary?["VerticalCameraDeviceUUID"] as? String)?.uppercased() ?? ""
        var types: [AVCaptureDevice.DeviceType] = [.externalUnknown]
        if #available(macOS 14.0, *) {
            types = [.external]
        }
        let devices = AVCaptureDevice.DiscoverySession(deviceTypes: types, mediaType: .video, position: .unspecified)
            .devices
        return devices.contains { $0.uniqueID.uppercased() == deviceUID || $0.localizedName == "Vertical Camera" }
    }

    /// After an activation: "Ready" once the device shows, else deactivate + activate once.
    private func waitForCamera(attempt: Int = 0) {
        if ExtensionManager.cameraVisible() {
            step = .idle
            onStatus?(
                "Ready: Vertical Camera \(bundledVersion.short) (\(bundledVersion.build)) is available to the apps.")
            onFinished?(true)
            return
        }
        if attempt < 20 {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.waitForCamera(attempt: attempt + 1) }
            return
        }
        if !fellBack {
            fellBack = true
            step = .removingOld
            onStatus?("The camera does not show yet: reinstalling it...")
            submitDeactivation()
            return
        }
        step = .idle
        onStatus?("Installed, but the camera does not show yet: restart the Mac to complete.")
        onFinished?(false)
    }

    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        switch step {
        case .removingOld:
            // The old build is gone (or goes at the next reboot): install ours now.
            submitActivation()
        case .installing where result == .willCompleteAfterReboot && !fellBack:
            fellBack = true
            step = .removingOld
            onStatus?("The update would wait for a restart: removing the old version first...")
            submitDeactivation()
        case .installing:
            onStatus?("Waiting for the camera to show...")
            waitForCamera()
        case .uninstalling:
            step = .idle
            onStatus?(result == .completed ? "Uninstalled." : "Uninstalled. Restart the Mac to complete.")
            onFinished?(true)
        default:
            break
        }
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        let nsError = error as NSError
        // Nothing to remove (first install): go on with the activation.
        if step == .removingOld && nsError.domain == OSSystemExtensionErrorDomain
            && nsError.code == OSSystemExtensionError.extensionNotFound.rawValue
        {
            submitActivation()
            return
        }
        if step == .inspecting {
            submitActivation()
            return
        }
        step = .idle
        var message = "Failed: \(nsError.localizedDescription) (code \(nsError.code))."
        if nsError.domain == OSSystemExtensionErrorDomain
            && nsError.code == OSSystemExtensionError.unsupportedParentBundleLocation.rawValue
        {
            message = "Move Vertical Camera.app to the Applications folder, then try again."
        }
        onStatus?(message)
        onFinished?(false)
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
        // "--quit-when-done": the package's postinstall updates the camera, then the app
        // goes away by itself.
        let args = CommandLine.arguments
        if args.contains("--quit-when-done") {
            manager.onFinished = { _ in
                DispatchQueue.main.asyncAfter(deadline: .now() + 4) { NSApp.terminate(nil) }
            }
        }
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

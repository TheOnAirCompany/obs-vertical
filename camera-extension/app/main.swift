// Vertical for OBS - Vertical Camera host app
// Copyright (C) 2026 The OnAir Company
// SPDX-License-Identifier: GPL-2.0-or-later

import AppKit

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()

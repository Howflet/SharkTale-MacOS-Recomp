// Capture exactly one window (by CGWindowID) to a PNG with ScreenCaptureKit:
//   wincap <windowID> <out.png>
// The content filter is the single window, so nothing else on screen is recorded.
import AppKit
import CoreGraphics
import Foundation
import ImageIO
import ScreenCaptureKit
import UniformTypeIdentifiers

_ = NSApplication.shared  // connects to the window server (required by CoreGraphics)
let id = CGWindowID(UInt32(CommandLine.arguments[1])!)
let out = URL(fileURLWithPath: CommandLine.arguments[2])

func fail(_ message: String) -> Never {
    FileHandle.standardError.write((message + "\n").data(using: .utf8)!)
    exit(1)
}

let done = DispatchSemaphore(value: 0)
Task {
    do {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
        guard let window = content.windows.first(where: { $0.windowID == id }) else { fail("window \(id) not shareable") }
        let filter = SCContentFilter(desktopIndependentWindow: window)
        let config = SCStreamConfiguration()
        config.width = Int(window.frame.width) * 2
        config.height = Int(window.frame.height) * 2
        config.showsCursor = false
        let image = try await SCScreenshotManager.captureImage(contentFilter: filter, configuration: config)
        let dest = CGImageDestinationCreateWithURL(out as CFURL, UTType.png.identifier as CFString, 1, nil)!
        CGImageDestinationAddImage(dest, image, nil)
        if !CGImageDestinationFinalize(dest) { fail("could not write \(out.path)") }
    } catch {
        fail("capture failed: \(error)")
    }
    done.signal()
}
done.wait()

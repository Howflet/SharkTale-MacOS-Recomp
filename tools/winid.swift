// Print "<windowID>\t<owner>\t<title>\t<w>x<h>" for on-screen windows whose
// owner or title contains the argument (case-insensitive). Used to screenshot
// only the game window: screencapture -l <windowID>.
import CoreGraphics
import Foundation

let needle = CommandLine.arguments.dropFirst().first?.lowercased() ?? ""
let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] ?? []
for w in list {
    let owner = w[kCGWindowOwnerName as String] as? String ?? ""
    let title = w[kCGWindowName as String] as? String ?? ""
    guard owner.lowercased().contains(needle) || title.lowercased().contains(needle) else { continue }
    let id = w[kCGWindowNumber as String] as? Int ?? 0
    let b = w[kCGWindowBounds as String] as? [String: Double] ?? [:]
    print("\(id)\t\(owner)\t\(title)\t\(Int(b["Width"] ?? 0))x\(Int(b["Height"] ?? 0))")
}

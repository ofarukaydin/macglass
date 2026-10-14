import CoreGraphics
import Foundation

/// A virtual monitor whose single mode follows the client's window size.
@MainActor
final class VirtualDisplay {
    private var display: CGVirtualDisplay?
    private let id: CGDirectDisplayID
    let refreshRate: Double

    var displayID: CGDirectDisplayID { id }

    /// Destroys the display now rather than whenever the last reference goes away.
    func remove() {
        stopWatching()
        display = nil
    }

    init(pixelWidth: Int, pixelHeight: Int, refreshRate: Double) {
        let desc = CGVirtualDisplayDescriptor()
        desc.setDispatchQueue(.main)
        desc.name = "MacGlass"
        desc.maxPixelsWide = 7680
        desc.maxPixelsHigh = 4320
        desc.sizeInMillimeters = CGSize(width: 900, height: 380)
        desc.productID = 0x4D47
        desc.vendorID = 0x4D47
        desc.serialNum = 1
        desc.terminationHandler = { _, _ in log("virtual display terminated") }
        let display = CGVirtualDisplay(descriptor: desc)
        self.display = display
        id = display.displayID
        self.refreshRate = refreshRate
        resize(pixelWidth: pixelWidth, pixelHeight: pixelHeight)
    }

    /// Even sizes get a 2x (Retina) mode; macOS falls back to 1x for odd or very small sizes.
    @discardableResult
    func resize(pixelWidth w: Int, pixelHeight h: Int) -> Bool {
        let hidpi = w % 2 == 0 && h % 2 == 0
        let settings = CGVirtualDisplaySettings()
        settings.hiDPI = hidpi ? 1 : 0
        settings.modes = [CGVirtualDisplayMode(
            width: UInt(hidpi ? w / 2 : w), height: UInt(hidpi ? h / 2 : h), refreshRate: refreshRate)]
        return display?.apply(settings) ?? false
    }

    /// Makes this the Mac's only screen: every other display mirrors it, so the menu bar, Dock and
    /// windows all live here. Displays that appear later (opening the lid) are mirrored too.
    /// Mirroring ends by itself if this display goes away, even if MacGlass crashes.
    func becomeOnlyScreen() {
        guard display != nil else { return }  // a session that ended while it was still starting
        mirrorOthers()
        if !watching {
            CGDisplayRegisterReconfigurationCallback(displayAdded, watcher.toOpaque())
            watching = true
        }
    }

    func releaseOtherScreens() {
        stopWatching()
        configure { cfg in
            for id in onlineDisplays() where CGDisplayMirrorsDisplay(id) == displayID {
                CGConfigureDisplayMirrorOfDisplay(cfg, id, kCGNullDirectDisplay)
            }
        }
    }

    fileprivate func mirrorOthers() {
        guard display != nil else { return }
        let others = onlineDisplays().filter {
            $0 != displayID && CGDisplayIsActive($0) != 0 && CGDisplayMirrorsDisplay($0) != displayID
        }
        guard !others.isEmpty else { return }
        configure { cfg in
            for id in others { CGConfigureDisplayMirrorOfDisplay(cfg, id, displayID) }
        }
        log("mirroring \(others.count) other display(s) onto MacGlass")
    }

    private var watching = false
    /// What the reconfiguration callback is given. Kept alive for good (a few bytes per session):
    /// a callback already queued when this display goes away must still find a valid object.
    private lazy var watcher = Unmanaged.passRetained(WeakDisplay(self))

    private func stopWatching() {
        guard watching else { return }
        CGDisplayRemoveReconfigurationCallback(displayAdded, watcher.toOpaque())
        watching = false
    }

    private func configure(_ body: (CGDisplayConfigRef?) -> Void) {
        var cfg: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&cfg) == .success else { return }
        body(cfg)
        let err = CGCompleteDisplayConfiguration(cfg, .forSession)
        if err != .success { log("display configuration failed: \(err.rawValue)") }
    }

    /// Pixel size macOS actually applied, which can lag a resize by ~0.3 s.
    var currentPixelSize: (Int, Int)? {
        guard let mode = CGDisplayCopyDisplayMode(displayID) else { return nil }
        return (mode.pixelWidth, mode.pixelHeight)
    }
}

private func onlineDisplays() -> [CGDirectDisplayID] {
    var count: UInt32 = 0
    CGGetOnlineDisplayList(0, nil, &count)
    var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
    CGGetOnlineDisplayList(count, &ids, &count)
    return Array(ids.prefix(Int(count)))
}

private final class WeakDisplay {
    weak var display: VirtualDisplay?
    init(_ display: VirtualDisplay) { self.display = display }
}

private let displayAdded: CGDisplayReconfigurationCallBack = { _, flags, user in
    guard flags.contains(.addFlag), let user else { return }
    let weak = Unmanaged<WeakDisplay>.fromOpaque(user).takeUnretainedValue()
    DispatchQueue.main.async { MainActor.assumeIsolated { weak.display?.mirrorOthers() } }
}

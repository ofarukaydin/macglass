import AppKit

/// Sends the Mac's cursor shape whenever it changes. The client shows it as its own local cursor,
/// so the pointer moves without streaming latency (the video is captured without a cursor). Also
/// sends where the cursor is, for when it moves without the client's input (the client marks it
/// with a dot while it isn't forwarding input, e.g. while automation moves the mouse).
@MainActor
final class CursorWatcher {
    /// Called with an MG_CURSOR payload.
    var onChange: ((Data) -> Void)?
    /// Called with an MG_CURSOR_POS payload.
    var onMove: ((Data) -> Void)?
    /// Display whose backing scale (Retina 2x or 1x) the cursor image is rendered at.
    var displayID: CGDirectDisplayID = 0

    private var timer: Timer?
    private var lastHash: Int?
    private var lastPosition: (UInt32, UInt32, Bool)?

    func start() {
        lastHash = nil
        lastPosition = nil
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.poll() }
        }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    private var scale: CGFloat {
        let bounds = CGDisplayBounds(displayID)
        guard bounds.width > 0, let mode = CGDisplayCopyDisplayMode(displayID) else { return 2 }
        return CGFloat(mode.pixelWidth) / bounds.width
    }

    private func poll() {
        pollPosition()
        pollShape()
    }

    private func pollPosition() {
        guard let location = CGEvent(source: nil)?.location else { return }
        let bounds = CGDisplayBounds(displayID), scale = self.scale
        let visible = bounds.contains(location)
        let x = UInt32(max(0, ((location.x - bounds.minX) * scale).rounded()))
        let y = UInt32(max(0, ((location.y - bounds.minY) * scale).rounded()))
        guard lastPosition.map({ $0 != (x, y, visible) }) ?? true else { return }
        lastPosition = (x, y, visible)
        var payload = Data(capacity: 12)
        payload.appendLE(x)
        payload.appendLE(y)
        payload.appendLE(UInt32(visible ? 1 : 0))
        onMove?(payload)
    }

    private func pollShape() {
        guard let cursor = NSCursor.currentSystem else { return }
        let scale = self.scale
        let w = Int((cursor.image.size.width * scale).rounded()), h = Int((cursor.image.size.height * scale).rounded())
        guard (1...512).contains(w), (1...512).contains(h) else { return }

        var pixels = [UInt8](repeating: 0, count: w * h * 4)
        let drawn = pixels.withUnsafeMutableBytes { buf -> Bool in
            guard let ctx = CGContext(
                data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
            else { return false }
            NSGraphicsContext.saveGraphicsState()
            NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: false)
            cursor.image.draw(in: NSRect(x: 0, y: 0, width: w, height: h))
            NSGraphicsContext.restoreGraphicsState()
            return true
        }
        guard drawn else { return }
        let hotX = Int((cursor.hotSpot.x * scale).rounded()), hotY = Int((cursor.hotSpot.y * scale).rounded())

        var hasher = Hasher()
        hasher.combine(w)
        hasher.combine(h)
        hasher.combine(hotX)
        hasher.combine(hotY)
        pixels.withUnsafeBytes { hasher.combine(bytes: $0) }
        let hash = hasher.finalize()
        guard hash != lastHash else { return }
        lastHash = hash

        // CoreGraphics gives premultiplied alpha; the client expects straight alpha.
        for i in stride(from: 0, to: pixels.count, by: 4) {
            let a = Int(pixels[i + 3])
            guard a != 0 && a != 255 else { continue }
            for c in 0..<3 { pixels[i + c] = UInt8(min(255, Int(pixels[i + c]) * 255 / a)) }
        }
        var payload = Data(capacity: 8 + pixels.count)
        payload.appendLE(UInt16(w))
        payload.appendLE(UInt16(h))
        payload.appendLE(UInt16(min(max(hotX, 0), w - 1)))
        payload.appendLE(UInt16(min(max(hotY, 0), h - 1)))
        payload.append(contentsOf: pixels)
        onChange?(payload)
    }
}

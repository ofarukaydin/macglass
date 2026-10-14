import CoreMedia
import CoreVideo
import Foundation

/// Decides whether a captured frame goes out whole or as a few small patches covering only what
/// changed. Changes in frames that were skipped (link busy) are remembered so none are missed.
/// Used on the capture queue, except `invalidate`.
final class FramePlanner {
    static let patchSize = 512  // 2.3 ms to encode on an M1 Pro, versus 7-17 ms for a whole frame
    static let maxPatches = 3   // each encode costs ~1.5 ms of fixed overhead

    enum Plan {
        case nothing
        case full
        case patches([CGRect])  // patchSize x patchSize windows, in frame pixels
    }

    private let lock = NSLock()
    private var needFull = true
    private var pending: [CGRect] = []
    private var lastSize = (0, 0)

    /// The next frame must be sent whole (new client, resize).
    func invalidate() {
        lock.withLock {
            needFull = true
            pending.removeAll()
        }
    }

    /// `dirty` is nil when the change region is unknown.
    func plan(width: Int, height: Int, dirty: [CGRect]?, sending: Bool) -> Plan {
        lock.lock()
        defer { lock.unlock() }
        if (width, height) != lastSize { needFull = true }
        guard let dirty, !needFull else {
            if !sending { needFull = true; return .nothing }
            needFull = false
            pending.removeAll()
            lastSize = (width, height)
            return .full
        }
        pending.append(contentsOf: dirty)
        guard sending else { return .nothing }
        let rects = pending
        pending.removeAll()
        if rects.isEmpty { return .nothing }

        let p = CGFloat(Self.patchSize)
        guard width >= Self.patchSize, height >= Self.patchSize else { return .full }
        var groups: [CGRect] = []
        for r in rects {
            let a = aligned(r, width, height)
            if a.isEmpty { continue }
            if a.width > p || a.height > p { return .full }
            if let i = groups.firstIndex(where: { $0.union(a).width <= p && $0.union(a).height <= p }) {
                groups[i] = groups[i].union(a)
            } else {
                groups.append(a)
                if groups.count > Self.maxPatches { return .full }
            }
        }
        if groups.isEmpty { return .nothing }
        // Place a fixed-size window over each group, kept inside the frame.
        return .patches(groups.map { g in
            CGRect(x: min(g.minX, CGFloat(width) - p), y: min(g.minY, CGFloat(height) - p), width: p, height: p)
        })
    }

    /// Expands to a 16-pixel grid (ProRes macroblocks) and clips to the frame.
    private func aligned(_ r: CGRect, _ width: Int, _ height: Int) -> CGRect {
        let x0 = floor(r.minX / 16) * 16, y0 = floor(r.minY / 16) * 16
        let x1 = ceil(r.maxX / 16) * 16, y1 = ceil(r.maxY / 16) * 16
        return CGRect(x: x0, y: y0, width: x1 - x0, height: y1 - y0)
            .intersection(CGRect(x: 0, y: 0, width: width, height: height))
    }
}

/// Copies a region of a captured frame into a pooled patch-sized buffer for the encoder. Handles
/// the capture's two-plane 4:4:4 layout (16-bit luma, 32-bit interleaved chroma per pixel).
final class PatchCopier {
    private var pool: CVPixelBufferPool?

    init() {
        let attrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange,
            kCVPixelBufferWidthKey as String: FramePlanner.patchSize,
            kCVPixelBufferHeightKey as String: FramePlanner.patchSize,
            kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](),
        ]
        CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool)
    }

    func copy(_ src: CVPixelBuffer, _ rect: CGRect) -> CVPixelBuffer? {
        guard let pool else { return nil }
        var out: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &out) == kCVReturnSuccess, let dst = out else { return nil }
        CVPixelBufferLockBaseAddress(src, .readOnly)
        CVPixelBufferLockBaseAddress(dst, [])
        defer {
            CVPixelBufferUnlockBaseAddress(dst, [])
            CVPixelBufferUnlockBaseAddress(src, .readOnly)
        }
        guard CVPixelBufferGetPlaneCount(src) == 2, CVPixelBufferGetPixelFormatType(src) == CVPixelBufferGetPixelFormatType(dst)
        else { return nil }
        let x = Int(rect.minX), y = Int(rect.minY), w = Int(rect.width), h = Int(rect.height)
        for (plane, bytesPerPixel) in [(0, 2), (1, 4)] {
            guard let s = CVPixelBufferGetBaseAddressOfPlane(src, plane), let d = CVPixelBufferGetBaseAddressOfPlane(dst, plane)
            else { return nil }
            let sStride = CVPixelBufferGetBytesPerRowOfPlane(src, plane), dStride = CVPixelBufferGetBytesPerRowOfPlane(dst, plane)
            for row in 0..<h {
                memcpy(d + row * dStride, s + (y + row) * sStride + x * bytesPerPixel, w * bytesPerPixel)
            }
        }
        CVBufferPropagateAttachments(src, dst)  // keep the colour space tags
        return dst
    }
}

/// Hands encoded messages to the server in capture order. Full frames and patches are encoded
/// in separate sessions that finish out of order; an old full frame must never overwrite a newer
/// patch on the client.
final class Sequencer {
    typealias Message = (type: UInt32, header: Data, body: Data)
    var send: ((Message) -> Void)?

    private let lock = NSLock()
    private var next = 0, sendNext = 0
    private var done: [Int: Message?] = [:]

    func reserve() -> Int {
        lock.withLock {
            defer { next += 1 }
            return next
        }
    }

    /// `message` is nil when encoding failed; the slot is skipped.
    func complete(_ seq: Int, _ message: Message?) {
        lock.lock()
        defer { lock.unlock() }
        done[seq] = .some(message)
        while let ready = done.removeValue(forKey: sendNext) {
            if let m = ready { send?(m) }
            sendNext += 1
        }
    }
}

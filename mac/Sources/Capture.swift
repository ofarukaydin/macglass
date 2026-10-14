import CoreMedia
import ScreenCaptureKit

/// Captures the virtual display with ScreenCaptureKit. Frames arrive only when content changes.
@MainActor
final class Capturer: NSObject {
    let displayID: CGDirectDisplayID
    let fps: Double
    /// Called on the capture queue with a BGRA frame, its display time in nanoseconds and the
    /// rectangles (in frame pixels) that changed since the previous frame, or nil if unknown.
    nonisolated(unsafe) var onFrame: ((CVPixelBuffer, UInt64, [CGRect]?) -> Void)?
    /// Called on the audio queue with system audio, or never if audio is off.
    nonisolated(unsafe) var onAudio: ((CMSampleBuffer) -> Void)?
    var captureAudio = true

    private var stream: SCStream?
    private var size = (0, 0)
    private var lastRepair = 0.0
    private let output = Output()
    /// Readable from the capture queue; written only on the main actor.
    nonisolated(unsafe) private(set) var expectedSize = (0, 0)
    private let queue = DispatchQueue(label: "macglass.capture", qos: .userInteractive)
    private let audioQueue = DispatchQueue(label: "macglass.audio", qos: .userInteractive)

    init(displayID: CGDirectDisplayID, fps: Double) {
        self.displayID = displayID
        self.fps = fps
        super.init()
        output.owner = self
    }

    private func configuration() -> SCStreamConfiguration {
        let c = SCStreamConfiguration()
        c.width = size.0
        c.height = size.1
        c.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps.rounded()))
        c.queueDepth = 4
        // 10-bit 4:4:4 YUV without alpha. From BGRA, VideoToolbox's ProRes 4444 also encodes the
        // (always opaque) alpha channel, and FFmpeg's Vulkan ProRes decoder hangs on such frames.
        c.pixelFormat = kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange
        c.colorMatrix = CGDisplayStream.yCbCrMatrix_ITU_R_709_2
        c.colorSpaceName = CGColorSpace.sRGB
        c.showsCursor = false  // the client draws the cursor locally (see CursorWatcher)
        c.captureResolution = .best
        c.capturesAudio = captureAudio
        c.sampleRate = 48000
        c.channelCount = 2
        c.excludesCurrentProcessAudio = true
        return c
    }

    func start(width: Int, height: Int) async throws {
        await stop()
        size = (width, height)
        expectedSize = size
        var display: SCDisplay?
        for _ in 0..<30 {  // a new virtual display takes a moment to become capturable
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
            display = content.displays.first { $0.displayID == displayID }
            if display != nil { break }
            try await Task.sleep(for: .milliseconds(100))
        }
        guard let display else { throw MGError("virtual display is not visible to ScreenCaptureKit") }
        // Read the size after the awaits: a resize may have arrived in the meantime.
        let s = SCStream(
            filter: SCContentFilter(display: display, excludingWindows: []),
            configuration: configuration(), delegate: output)
        try s.addStreamOutput(output, type: .screen, sampleHandlerQueue: queue)
        if captureAudio { try s.addStreamOutput(output, type: .audio, sampleHandlerQueue: audioQueue) }
        try await s.startCapture()
        stream = s
        log("capture started at \(size.0)x\(size.1)")
    }

    func resize(width: Int, height: Int) async throws {
        size = (width, height)
        expectedSize = size
        try await stream?.updateConfiguration(configuration())
    }

    /// ScreenCaptureKit can keep a display's previous resolution when capture starts right after a
    /// mode change. Reapplying the configuration fixes it.
    fileprivate func frameSizeMismatch(_ w: Int, _ h: Int) {
        let now = ProcessInfo.processInfo.systemUptime
        guard (w, h) != size, now - lastRepair > 1, let stream else { return }
        lastRepair = now
        log("capture delivered \(w)x\(h) instead of \(size.0)x\(size.1); reapplying the configuration")
        Task { try? await stream.updateConfiguration(configuration()) }
    }

    func stop() async {
        guard let s = stream else { return }
        stream = nil
        try? await s.stopCapture()
        log("capture stopped")
    }

    private final class Output: NSObject, SCStreamOutput, SCStreamDelegate {
        weak var owner: Capturer?

        func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
            if type == .audio {
                owner?.onAudio?(sb)
                return
            }
            guard type == .screen,
                let info = (CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false)
                    as? [[SCStreamFrameInfo: Any]])?.first,
                let raw = info[.status] as? Int, SCFrameStatus(rawValue: raw) == .complete,
                let pixels = CMSampleBufferGetImageBuffer(sb)
            else { return }
            let shown = (info[.displayTime] as? UInt64) ?? mach_absolute_time()
            let w = CVPixelBufferGetWidth(pixels), h = CVPixelBufferGetHeight(pixels)
            // Verified on macOS 27: dirty rects are in frame pixels, not points.
            let dirty = (info[.dirtyRects] as? [Any])?.compactMap {
                CGRect(dictionaryRepresentation: $0 as! CFDictionary)
            }
            if let owner, (w, h) != owner.expectedSize {
                DispatchQueue.main.async { MainActor.assumeIsolated { owner.frameSizeMismatch(w, h) } }
            }
            owner?.onFrame?(pixels, machToNs(shown), dirty)
        }

        func stream(_ stream: SCStream, didStopWithError error: Error) {
            log("capture stopped with error: \(error)")
        }
    }
}

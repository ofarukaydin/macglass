import AppKit

/// Two-way clipboard sync. macOS has no pasteboard change notification, so it is polled.
@MainActor
final class ClipboardSync {
    /// Called with (kind, content) when something new is copied on the Mac.
    var onLocalChange: ((UInt8, Data) -> Void)?

    private var seenChangeCount = NSPasteboard.general.changeCount
    private var timer: Timer?

    func start() {
        // Don't send whatever was copied before the client connected.
        seenChangeCount = NSPasteboard.general.changeCount
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.poll() }
        }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    /// Text wins over images, matching what a plain paste would insert.
    private func poll() {
        let pb = NSPasteboard.general
        guard pb.changeCount != seenChangeCount else { return }
        seenChangeCount = pb.changeCount
        if let text = pb.string(forType: .string) {
            onLocalChange?(MG.clipText, Data(text.utf8))
        } else if let png = pb.data(forType: .png)
            ?? pb.data(forType: .tiff).flatMap({ NSBitmapImageRep(data: $0)?.representation(using: .png, properties: [:]) })
        {
            onLocalChange?(MG.clipPNG, png)
        }
    }

    func apply(kind: UInt8, content: Data) {
        let pb = NSPasteboard.general
        switch kind {
        case MG.clipText:
            pb.clearContents()
            pb.setString(String(decoding: content, as: UTF8.self), forType: .string)
        case MG.clipPNG:
            pb.clearContents()
            pb.setData(content, forType: .png)
        default:
            return
        }
        seenChangeCount = pb.changeCount  // our own write; don't send it back
    }
}

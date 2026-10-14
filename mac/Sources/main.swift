// MacGlass server: a virtual display on the Mac, streamed as ProRes to the Linux client
// over the Thunderbolt link. The client's window size drives the display's resolution.
import AppKit
import VideoToolbox

setvbuf(stdout, nil, _IOLBF, 0)

struct Options {
    var port = MG.defaultPort
    var interface = "bridge0"
    var width = 5120
    var height = 2160
    var hz = 120.0
    var forcedProRes: CMVideoCodecType?  // default: 4444, or 422 HQ where 4444 can't do 120 fps
    var audio = true

    static func parse() -> Options {
        var o = Options()
        var args = CommandLine.arguments.dropFirst().makeIterator()
        while let arg = args.next() {
            if arg == "--no-audio" {
                o.audio = false
                continue
            }
            let value = args.next() ?? ""
            switch arg {
            case "--port": o.port = UInt16(value) ?? o.port
            case "--interface": o.interface = value
            case "--width": o.width = Int(value) ?? o.width
            case "--height": o.height = Int(value) ?? o.height
            case "--hz": o.hz = Double(value) ?? o.hz
            case "--codec":
                switch value {
                case "lt": o.forcedProRes = kCMVideoCodecType_AppleProRes422LT
                case "422": o.forcedProRes = kCMVideoCodecType_AppleProRes422
                case "hq": o.forcedProRes = kCMVideoCodecType_AppleProRes422HQ
                case "4444": o.forcedProRes = kCMVideoCodecType_AppleProRes4444
                default: log("unknown codec \(value); use lt, 422, hq or 4444")
                }
            default: log("unknown option \(arg)")
            }
        }
        return o
    }
}

let options = Options.parse()

@MainActor
func run() async throws {
    if !CGPreflightScreenCaptureAccess() {
        CGRequestScreenCaptureAccess()
        throw MGError("Screen Recording permission is missing: allow MacGlass in System Settings, then restart it")
    }

    let encoders = FullFrameEncoders(forcedProRes: options.forcedProRes)
    let server = Server(port: options.port, interface: options.interface)
    let input = InputInjector()
    let clipboard = ClipboardSync()
    let power = PowerKeeper()
    let cursor = CursorWatcher()
    let speakers = SpeakerMute()

    onTermination {
        power.release()
        speakers.unmute()
    }

    cursor.onChange = { payload in server.send(type: MG.cursor, header: payload) }
    cursor.onMove = { payload in server.send(type: MG.cursorPos, header: payload, counted: false) }

    clipboard.onLocalChange = { kind, content in
        server.send(type: MG.clipboard, header: Data([kind]), body: content)
    }

    // Small changes go out as patches, everything else as full frames (see FramePlanner).
    let planner = FramePlanner()
    let copier = PatchCopier()
    let patchEncoder = ProResEncoder(codec: options.forcedProRes ?? kCMVideoCodecType_AppleProRes4444)
    let sequencer = Sequencer()
    sequencer.send = { m in server.send(type: m.type, header: m.header, body: m.body) }

    let onFrame: (CVPixelBuffer, UInt64, [CGRect]?) -> Void = { pixels, ns, dirty in
        let w = CVPixelBufferGetWidth(pixels), h = CVPixelBufferGetHeight(pixels)
        switch planner.plan(width: w, height: h, dirty: dirty, sending: server.canSend) {
        case .nothing:
            return
        case .full:
            let seq = sequencer.reserve()
            encoders.encode(pixels) { data, codec, key in
                var header = Data(capacity: MG.frameHeaderSize)
                header.appendLE(UInt32(w))
                header.appendLE(UInt32(h))
                header.appendLE(ns)
                header.appendLE(codec)
                header.appendLE(key ? MG.frameKey : 0)
                sequencer.complete(seq, data.map { (MG.frame, header, $0) })
            }
        case .patches(let rects):
            for r in rects {
                guard let patch = copier.copy(pixels, r) else {
                    planner.invalidate()
                    continue
                }
                var header = Data(capacity: MG.patchHeaderSize)
                header.appendLE(UInt32(w))
                header.appendLE(UInt32(h))
                header.appendLE(UInt32(r.minX))
                header.appendLE(UInt32(r.minY))
                header.appendLE(ns)
                let seq = sequencer.reserve()
                patchEncoder.encode(patch) { data in sequencer.complete(seq, data.map { (MG.patch, header, $0) }) }
            }
        }
    }
    let onAudio: (CMSampleBuffer) -> Void = { sample in
        guard let (rate, channels, samples) = interleavedFloat32(sample) else { return }
        var header = Data(capacity: 8)
        header.appendLE(UInt32(rate))
        header.appendLE(UInt32(channels))
        server.send(type: MG.audio, header: header, body: samples, counted: false)
    }
    server.onConnect = {
        var hello = Data()
        hello.appendLE(MG.protocolVersion)
        hello.appendLE(kCMVideoCodecType_AppleProRes4444)
        server.send(type: MG.hello, header: hello)
        encoders.request(hevc: false)  // until the client says otherwise
        encoders.requestKeyframe()
        planner.invalidate()
        Task { @MainActor in
            connection += 1
            let mine = connection
            power.hold()
            if options.audio { speakers.mute() }
            // A quick reconnect must not create our display while the last one is still going
            // away: both have the same identity, and macOS then hides the new one from capture.
            await sessionEnding?.value
            try? await Task.sleep(for: .milliseconds(300))  // let the display system wake up
            // The client may have left (or been replaced) meanwhile; a display made for it now
            // would linger and hide the next client's display in the same way.
            guard mine == connection else { return }
            clipboard.start()
            let s = Session(size: requestedSize ?? (options.width, options.height), onFrame: onFrame,
                            onAudio: options.audio ? onAudio : nil)
            session = s  // before any await, so later resizes find it
            input.displayID = s.display.displayID
            cursor.displayID = s.display.displayID
            cursor.start()
            await s.start()
        }
    }
    server.onDisconnect = {
        input.releaseAll()
        input.displayID = 0
        Task { @MainActor in
            connection += 1
            // Release before the first await: a quick reconnect's hold() must come after this.
            power.release()
            speakers.unmute()
            let s = session
            session = nil
            requestedSize = nil
            cursor.stop()
            clipboard.stop()
            let previous = sessionEnding  // several may be under way after quick reconnects
            sessionEnding = Task { @MainActor in
                await previous?.value
                await s?.end()
            }
        }
    }
    server.onMessage = { type, payload in
        if type == MG.resize && payload.count >= 8 {
            resize(Int(payload.readLE(UInt32.self, at: 0)), Int(payload.readLE(UInt32.self, at: 4)))
        } else if type == MG.setCodec && payload.count >= 4 {
            let hevc = payload.readLE(UInt32.self, at: 0) == kCMVideoCodecType_HEVC
            log("client asked for \(hevc ? "HEVC 4:4:4" : "ProRes") full frames")
            encoders.request(hevc: hevc)
            planner.invalidate()
        } else if type == MG.keyframe {
            encoders.requestKeyframe()
            planner.invalidate()
        } else if type == MG.clipboard && !payload.isEmpty {
            Task { @MainActor in clipboard.apply(kind: payload[0], content: payload.dropFirst()) }
        } else {
            input.handle(type, payload)
        }
    }
    func resize(_ w: Int, _ h: Int) {
        planner.invalidate()
        Task { @MainActor in
            requestedSize = (w, h)  // also covers a resize that arrives before the session exists
            await session?.resize(w, h)
        }
    }
    try server.start()
    do { try DiscoveryResponder(port: options.port, interface: options.interface).start() } catch { log("discovery: \(error)") }
    if !AXIsProcessTrusted() { log("Accessibility permission is missing: the accessibility proxy will return kAXErrorAPIDisabled") }
    do { try axProxy.start() } catch { log("accessibility proxy: \(error)") }
    do { try axProxy.startTCP(port: options.port + 1, interface: options.interface) } catch { log("accessibility proxy: \(error)") }
}

let axProxy = AXProxy()

@MainActor var session: Session?
@MainActor var sessionEnding: Task<Void, Never>?
@MainActor var connection = 0  // bumped by every connect and disconnect
nonisolated(unsafe) var signalSources: [DispatchSourceSignal] = []

/// Runs `cleanup` on SIGINT/SIGTERM (e.g. `pkill MacGlass`) before exiting.
func onTermination(_ cleanup: @escaping () -> Void) {
    for sig in [SIGINT, SIGTERM] {
        signal(sig, SIG_IGN)
        let source = DispatchSource.makeSignalSource(signal: sig, queue: .main)
        source.setEventHandler {
            cleanup()
            exit(0)
        }
        source.resume()
        signalSources.append(source)
    }
}
@MainActor var requestedSize: (Int, Int)?

/// The virtual display and its capture for one connected client. The display exists only while a
/// client is connected, so no invisible monitor is left behind to swallow windows.
@MainActor
final class Session {
    let display: VirtualDisplay
    let capturer: Capturer
    private let initialSize: (Int, Int)

    init(size: (Int, Int), onFrame: @escaping (CVPixelBuffer, UInt64, [CGRect]?) -> Void,
         onAudio: ((CMSampleBuffer) -> Void)?) {
        initialSize = (size.0 & ~1, size.1 & ~1)
        display = VirtualDisplay(pixelWidth: initialSize.0, pixelHeight: initialSize.1, refreshRate: options.hz)
        capturer = Capturer(displayID: display.displayID, fps: options.hz)
        capturer.onFrame = onFrame
        capturer.onAudio = onAudio
        capturer.captureAudio = onAudio != nil
    }

    func start() async {
        // macOS first restores the resolution it remembers for this display, then applies ours.
        let (w, h) = await appliedSize(initialSize)
        display.becomeOnlyScreen()
        do { try await capturer.start(width: w, height: h) } catch { log("capture failed: \(error)") }
    }

    /// Waits up to a second for a mode switch, returning the size macOS actually applied.
    private func appliedSize(_ wanted: (Int, Int)) async -> (Int, Int) {
        var applied = display.currentPixelSize
        for _ in 0..<100 where applied.map({ $0 != wanted }) ?? true {
            try? await Task.sleep(for: .milliseconds(10))
            applied = display.currentPixelSize
        }
        return applied ?? wanted
    }

    func resize(_ w: Int, _ h: Int) async {
        let w = min(max(w, 640), 7680), h = min(max(h, 360), 4320)
        guard display.resize(pixelWidth: w, pixelHeight: h) else {
            log("resize to \(w)x\(h) rejected")
            return
        }
        let (aw, ah) = await appliedSize((w, h))
        display.becomeOnlyScreen()  // a mode change can drop the mirror
        do {
            try await capturer.resize(width: aw, height: ah)
            log("resized to \(aw)x\(ah)")
        } catch {
            log("capture resize failed: \(error)")
        }
    }

    /// Returns once macOS has removed the display.
    func end() async {
        await capturer.stop()
        display.releaseOtherScreens()
        let id = display.displayID
        display.remove()
        for _ in 0..<50 where CGDisplayIsOnline(id) != 0 {
            try? await Task.sleep(for: .milliseconds(20))
        }
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
Task { @MainActor in
    do {
        try await run()
    } catch {
        log("error: \(error)")
        exit(1)
    }
}
app.run()

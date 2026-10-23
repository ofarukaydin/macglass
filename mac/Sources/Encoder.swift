import CoreMedia
import VideoToolbox

/// Hardware ProRes encoder. Each frame is encoded independently (intra-only).
final class ProResEncoder {
    let codec: CMVideoCodecType

    private var session: VTCompressionSession?
    private var width = 0, height = 0
    private var frameIndex: Int64 = 0

    init(codec: CMVideoCodecType) { self.codec = codec }

    /// Called on the capture queue. Recreates the session when the frame size changes.
    /// `done` runs on a VideoToolbox queue with the encoded frame, or nil on failure.
    func encode(_ pixels: CVPixelBuffer, done: @escaping (Data?) -> Void) {
        let w = CVPixelBufferGetWidth(pixels), h = CVPixelBufferGetHeight(pixels)
        if session == nil || w != width || h != height {
            if let old = session { VTCompressionSessionInvalidate(old) }
            session = makeSession(w, h)
            width = w
            height = h
        }
        guard let s = session else { return done(nil) }
        frameIndex += 1
        let status = VTCompressionSessionEncodeFrame(
            s, imageBuffer: pixels, presentationTimeStamp: CMTime(value: frameIndex, timescale: 1000),
            duration: .invalid, frameProperties: nil, infoFlagsOut: nil
        ) { status, _, sample in
            guard status == noErr, let sample, let block = CMSampleBufferGetDataBuffer(sample) else {
                log("encode failed: \(status)")
                return done(nil)
            }
            let length = CMBlockBufferGetDataLength(block)
            var data = Data(count: length)
            data.withUnsafeMutableBytes {
                _ = CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: $0.baseAddress!)
            }
            done(data)
        }
        if status != noErr {
            log("encode rejected: \(status)")
            done(nil)
        }
    }

    private func makeSession(_ w: Int, _ h: Int) -> VTCompressionSession? {
        var session: VTCompressionSession?
        let spec = [kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder as String: true] as CFDictionary
        let status = VTCompressionSessionCreate(
            allocator: nil, width: Int32(w), height: Int32(h), codecType: codec,
            encoderSpecification: spec, imageBufferAttributes: nil, compressedDataAllocator: nil,
            outputCallback: nil, refcon: nil, compressionSessionOut: &session)
        guard status == noErr, let s = session else {
            log("cannot create \(w)x\(h) ProRes session: \(status)")
            return nil
        }
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        // The Linux client converts with the BT.709 matrix.
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_YCbCrMatrix, value: kCVImageBufferYCbCrMatrix_ITU_R_709_2)
        VTCompressionSessionPrepareToEncodeFrames(s)
        log("encoder ready for \(w)x\(h)")
        return s
    }
}

/// Hardware HEVC 4:4:4 10-bit encoder for full frames when the client decodes on an Intel media
/// engine. Frames reference earlier ones, so each output is an Annex-B access unit with the
/// parameter sets in front of every keyframe, and keyframes can be forced (new client, decode
/// error, codec switch). Measured on an M1 Pro: ~70 fps at 5120x2160, 48-50 dB PSNR at quality 0.9.
final class HEVCEncoder {
    private var session: VTCompressionSession?
    private var width = 0, height = 0
    private var frameIndex: Int64 = 0
    private static let startCode: [UInt8] = [0, 0, 0, 1]

    /// Called on the capture queue. `done` gets the access unit and whether it is a keyframe.
    func encode(_ pixels: CVPixelBuffer, forceKeyframe: Bool, done: @escaping (Data?, Bool) -> Void) {
        let w = CVPixelBufferGetWidth(pixels), h = CVPixelBufferGetHeight(pixels)
        if session == nil || w != width || h != height {
            if let old = session { VTCompressionSessionInvalidate(old) }
            session = makeSession(w, h)  // a new session starts with a keyframe anyway
            width = w
            height = h
        }
        guard let s = session else { return done(nil, false) }
        frameIndex += 1
        let props = forceKeyframe ? [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary : nil
        let status = VTCompressionSessionEncodeFrame(
            s, imageBuffer: pixels, presentationTimeStamp: CMTime(value: frameIndex, timescale: 1000),
            duration: .invalid, frameProperties: props, infoFlagsOut: nil
        ) { status, _, sample in
            guard status == noErr, let sample else {
                log("HEVC encode failed: \(status)")
                return done(nil, false)
            }
            let (data, key) = Self.annexB(sample)
            done(data, key)
        }
        if status != noErr {
            log("HEVC encode rejected: \(status)")
            done(nil, false)
        }
    }

    private static func annexB(_ sample: CMSampleBuffer) -> (Data?, Bool) {
        let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[CFString: Any]]
        let key = !((attachments?.first?[kCMSampleAttachmentKey_NotSync] as? Bool) ?? false)
        guard let block = CMSampleBufferGetDataBuffer(sample) else { return (nil, key) }
        var out = Data()
        if key, let fd = CMSampleBufferGetFormatDescription(sample) {
            var count = 0
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                fd, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil,
                parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
            for i in 0..<count {
                var p: UnsafePointer<UInt8>?
                var n = 0
                CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                    fd, parameterSetIndex: i, parameterSetPointerOut: &p, parameterSetSizeOut: &n,
                    parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
                if let p {
                    out.append(contentsOf: startCode)
                    out.append(p, count: n)
                }
            }
        }
        // VideoToolbox emits 4-byte big-endian NAL lengths; replace them with start codes.
        let length = CMBlockBufferGetDataLength(block)
        var avcc = Data(count: length)
        avcc.withUnsafeMutableBytes { _ = CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: $0.baseAddress!) }
        var offset = 0
        while offset + 4 <= length {
            let n = Int(avcc[offset]) << 24 | Int(avcc[offset + 1]) << 16 | Int(avcc[offset + 2]) << 8 | Int(avcc[offset + 3])
            guard offset + 4 + n <= length else { break }
            out.append(contentsOf: startCode)
            out.append(avcc[(offset + 4)..<(offset + 4 + n)])
            offset += 4 + n
        }
        return (out, key)
    }

    private func makeSession(_ w: Int, _ h: Int) -> VTCompressionSession? {
        var session: VTCompressionSession?
        let spec = [kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder as String: true] as CFDictionary
        let status = VTCompressionSessionCreate(
            allocator: nil, width: Int32(w), height: Int32(h), codecType: kCMVideoCodecType_HEVC,
            encoderSpecification: spec, imageBufferAttributes: nil, compressedDataAllocator: nil,
            outputCallback: nil, refcon: nil, compressionSessionOut: &session)
        guard status == noErr, let s = session else {
            log("cannot create \(w)x\(h) HEVC session: \(status)")
            return nil
        }
        // "HEVC_Main44410_AutoLevel" has no SDK constant but is accepted and yields 4:4:4 10-bit.
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ProfileLevel, value: "HEVC_Main44410_AutoLevel" as CFString)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_Quality, value: 0.9 as CFNumber)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: 1200 as CFNumber)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_YCbCrMatrix, value: kCVImageBufferYCbCrMatrix_ITU_R_709_2)
        VTCompressionSessionPrepareToEncodeFrames(s)
        log("HEVC 4:4:4 encoder ready for \(w)x\(h)")
        return s
    }
}

/// Picks and owns the full-frame encoder. The client asks for ProRes or HEVC (MG_SET_CODEC). For
/// ProRes, 4444 is used unless the frame is too big for an M1's ProRes engine to keep up 120 fps
/// (on an M1 Pro 4444 tops out at ~113 fps at 5120x2160), where 422 HQ (~137 fps) takes over.
/// Later engines have room to spare: an M5 Pro encodes 4444 at 5120x2160 at ~380 fps.
final class FullFrameEncoders {
    static let proRes4444PixelLimit = cpuBrand().hasPrefix("Apple M1") ? 9_500_000 : Int.max

    private static func cpuBrand() -> String {
        var size = 0
        sysctlbyname("machdep.cpu.brand_string", nil, &size, nil, 0)
        var buf = [CChar](repeating: 0, count: size)
        sysctlbyname("machdep.cpu.brand_string", &buf, &size, nil, 0)
        return String(cString: buf)
    }

    private let forcedProRes: CMVideoCodecType?
    private let lock = NSLock()
    private var wantHEVC = false
    private var forceKeyframe = true
    private var proRes: [CMVideoCodecType: ProResEncoder] = [:]
    private let hevc = HEVCEncoder()

    /// `forcedProRes` pins one ProRes flavour (--codec) instead of choosing by frame size.
    init(forcedProRes: CMVideoCodecType?) { self.forcedProRes = forcedProRes }

    func request(hevc: Bool) {
        lock.withLock {
            if wantHEVC != hevc { forceKeyframe = true }
            wantHEVC = hevc
        }
    }

    func requestKeyframe() { lock.withLock { forceKeyframe = true } }

    /// Called on the capture queue; `done` gets the frame, its codec and whether it is a keyframe.
    func encode(_ pixels: CVPixelBuffer, done: @escaping (Data?, CMVideoCodecType, Bool) -> Void) {
        let (useHEVC, key) = lock.withLock { () -> (Bool, Bool) in
            defer { forceKeyframe = false }
            return (wantHEVC, forceKeyframe)
        }
        if useHEVC {
            hevc.encode(pixels, forceKeyframe: key) { data, isKey in done(data, kCMVideoCodecType_HEVC, isKey) }
            return
        }
        let pixelCount = CVPixelBufferGetWidth(pixels) * CVPixelBufferGetHeight(pixels)
        let codec = forcedProRes
            ?? (pixelCount > Self.proRes4444PixelLimit ? kCMVideoCodecType_AppleProRes422HQ : kCMVideoCodecType_AppleProRes4444)
        let encoder = proRes[codec] ?? ProResEncoder(codec: codec)
        proRes[codec] = encoder
        encoder.encode(pixels) { data in done(data, codec, true) }  // ProRes frames are all intra
    }
}

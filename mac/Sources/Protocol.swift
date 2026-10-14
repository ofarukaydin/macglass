import Foundation

// Mirrors protocol.h at the repository root; keep them in sync.
enum MG {
    static let protocolVersion: UInt32 = 8
    static let defaultPort: UInt16 = 47800

    static let hello: UInt32 = 1
    static let frame: UInt32 = 2
    static let cursor: UInt32 = 3
    static let patch: UInt32 = 4
    static let audio: UInt32 = 5
    static let cursorPos: UInt32 = 6
    static let resize: UInt32 = 10
    static let pointer: UInt32 = 11
    static let button: UInt32 = 12
    static let scroll: UInt32 = 13
    static let key: UInt32 = 14
    static let releaseAll: UInt32 = 15
    static let clipboard: UInt32 = 16
    static let setCodec: UInt32 = 17
    static let keyframe: UInt32 = 18

    static let frameKey: UInt32 = 1

    static let clipText: UInt8 = 0
    static let clipPNG: UInt8 = 1

    static let discoverQuery = "MACGLASS?"
    static let discoverReply = "MACGLASS"

    static let frameHeaderSize = 24
    static let patchHeaderSize = 24
}

extension Data {
    mutating func appendLE<T: FixedWidthInteger>(_ value: T) {
        var le = value.littleEndian
        Swift.withUnsafeBytes(of: &le) { append(contentsOf: $0) }
    }

    func readLE<T: FixedWidthInteger>(_: T.Type, at offset: Int) -> T {
        var value: T = 0
        _ = Swift.withUnsafeMutableBytes(of: &value) { copyBytes(to: $0, from: offset..<offset + MemoryLayout<T>.size) }
        return T(littleEndian: value)
    }

    func readFloat(at offset: Int) -> Float { Float(bitPattern: readLE(UInt32.self, at: offset)) }
}

struct MGError: Error, CustomStringConvertible {
    let description: String
    init(_ description: String) { self.description = description }
}

func log(_ message: String) {
    print(message)
    fflush(stdout)
}

private let timebase: mach_timebase_info_data_t = {
    var tb = mach_timebase_info_data_t()
    mach_timebase_info(&tb)
    return tb
}()

func machToNs(_ t: UInt64) -> UInt64 { t * UInt64(timebase.numer) / UInt64(timebase.denom) }

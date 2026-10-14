import AppKit
import ImageIO
import ScreenCaptureKit

/// The non-Accessibility calls the proxy exposes alongside AX, in the same style: CoreGraphics
/// events (CGEventRefs are handles "v1", "v2", ...), window and display lists, and ScreenCaptureKit
/// screenshots. Enum arguments are their raw values (CGEventType, CGMouseButton, CGEventTapLocation,
/// CGEventFlags, CGEventField, CGWindowListOption, CGScrollEventUnit).
final class CGEventHandles {
    private let lock = NSLock()
    private var events: [String: CGEvent] = [:]
    private var next = 1

    func add(_ event: CGEvent) -> String {
        lock.withLock {
            let h = "v\(next)"
            next += 1
            events[h] = event
            return h
        }
    }

    func event(_ handle: String) -> CGEvent? { lock.withLock { events[handle] } }
    func release(_ handle: String) { _ = lock.withLock { events.removeValue(forKey: handle) } }
}

/// Private but stable since 10.x: the CGWindowID behind an AXWindow element.
@_silgen_name("_AXUIElementGetWindow")
private func _AXUIElementGetWindow(_ element: AXUIElement, _ window: UnsafeMutablePointer<CGWindowID>) -> AXError

extension AXProxy {
    /// Returns nil when `fn` isn't one of these.
    func callCG(_ fn: String, _ a: [Any]) throws -> Any? {
        let source = CGEventSource(stateID: .hidSystemState)
        switch fn {
        case "CGEventCreate":
            guard let e = CGEvent(source: source) else { throw AXCallError.usage("CGEventCreate failed") }
            return events.add(e)
        case "CGEventCreateKeyboardEvent":
            guard let e = CGEvent(keyboardEventSource: source, virtualKey: CGKeyCode(try arg(a, 0, Int.self)),
                                  keyDown: try arg(a, 1, Bool.self))
            else { throw AXCallError.usage("CGEventCreateKeyboardEvent failed") }
            return events.add(e)
        case "CGEventCreateMouseEvent":
            guard let type = CGEventType(rawValue: UInt32(try arg(a, 0, Int.self))),
                let button = CGMouseButton(rawValue: UInt32(try arg(a, 2, Int.self))),
                let e = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: try point(a, 1),
                                mouseButton: button)
            else { throw AXCallError.usage("CGEventCreateMouseEvent failed") }
            return events.add(e)
        case "CGEventCreateScrollWheelEvent2":
            guard let units = CGScrollEventUnit(rawValue: UInt32(try arg(a, 0, Int.self))),
                let e = CGEvent(scrollWheelEvent2Source: source, units: units,
                                wheelCount: UInt32(try arg(a, 1, Int.self)),
                                wheel1: Int32(a.count > 2 ? try arg(a, 2, Int.self) : 0),
                                wheel2: Int32(a.count > 3 ? try arg(a, 3, Int.self) : 0),
                                wheel3: Int32(a.count > 4 ? try arg(a, 4, Int.self) : 0))
            else { throw AXCallError.usage("CGEventCreateScrollWheelEvent2 failed") }
            return events.add(e)
        case "CGEventKeyboardSetUnicodeString":
            let utf16 = Array(try arg(a, 1, String.self).utf16)
            try event(a, 0).keyboardSetUnicodeString(stringLength: utf16.count, unicodeString: utf16)
            return NSNull()
        case "CGEventKeyboardGetUnicodeString":
            var buffer = [UniChar](repeating: 0, count: 64)
            var length = 0
            try event(a, 0).keyboardGetUnicodeString(maxStringLength: buffer.count, actualStringLength: &length,
                                                     unicodeString: &buffer)
            return String(utf16CodeUnits: buffer, count: length)
        case "CGEventGetFlags":
            return Int(try event(a, 0).flags.rawValue)
        case "CGEventSetFlags":
            try event(a, 0).flags = CGEventFlags(rawValue: UInt64(try arg(a, 1, Int.self)))
            return NSNull()
        case "CGEventGetLocation":
            let p = try event(a, 0).location
            return ["point": [p.x, p.y]]
        case "CGEventSetLocation":
            try event(a, 0).location = try point(a, 1)
            return NSNull()
        case "CGEventGetType":
            return Int(try event(a, 0).type.rawValue)
        case "CGEventSetType":
            guard let type = CGEventType(rawValue: UInt32(try arg(a, 1, Int.self))) else { throw AXCallError.usage("bad CGEventType") }
            try event(a, 0).type = type
            return NSNull()
        case "CGEventGetIntegerValueField":
            return try event(a, 0).getIntegerValueField(try field(a, 1))
        case "CGEventSetIntegerValueField":
            try event(a, 0).setIntegerValueField(try field(a, 1), value: Int64(try arg(a, 2, Int.self)))
            return NSNull()
        case "CGEventGetDoubleValueField":
            return try event(a, 0).getDoubleValueField(try field(a, 1))
        case "CGEventSetDoubleValueField":
            try event(a, 0).setDoubleValueField(try field(a, 1), value: try arg(a, 2, Double.self))
            return NSNull()
        case "CGEventPost":
            guard let tap = CGEventTapLocation(rawValue: UInt32(try arg(a, 0, Int.self))) else { throw AXCallError.usage("bad CGEventTapLocation") }
            try event(a, 1).post(tap: tap)
            return NSNull()
        case "CGWarpMouseCursorPosition":
            return Int(CGWarpMouseCursorPosition(try point(a, 0)).rawValue)
        case "CGWindowListCopyWindowInfo":
            let option = CGWindowListOption(rawValue: UInt32(try arg(a, 0, Int.self)))
            let relative = CGWindowID(a.count > 1 ? try arg(a, 1, Int.self) : 0)
            return encode(CGWindowListCopyWindowInfo(option, relative))
        case "CGMainDisplayID":
            return Int(CGMainDisplayID())
        case "CGGetActiveDisplayList":
            var ids = [CGDirectDisplayID](repeating: 0, count: 32)
            var count: UInt32 = 0
            CGGetActiveDisplayList(UInt32(ids.count), &ids, &count)
            return ids.prefix(Int(count)).map { Int($0) }
        case "CGDisplayBounds":
            let r = CGDisplayBounds(CGDirectDisplayID(try arg(a, 0, Int.self)))
            return ["rect": [r.origin.x, r.origin.y, r.width, r.height]]
        case "_AXUIElementGetWindow":
            guard let handle = a.first as? String ?? (a.first as? [String: Any])?["element"] as? String,
                let el = handles.element(handle)
            else { throw AXCallError.usage("argument 1: unknown element handle") }
            var id: CGWindowID = 0
            let err = _AXUIElementGetWindow(el, &id)
            if err != .success { throw AXCallError(err) }
            return Int(id)
        case "SCScreenshotManager.captureImage":
            return try screenshot(a)
        default:
            return nil
        }
    }

    // MARK: Screenshots

    /// args: content filter, configuration.
    ///   filter: {"display": CGDirectDisplayID, "excludingWindows": [CGWindowID]}
    ///        or {"desktopIndependentWindow": CGWindowID}
    ///   configuration (all optional): {"width", "height", "sourceRect": [x, y, w, h], "showsCursor",
    ///        "scalesToFit", "ignoreShadowsSingleWindow", "shouldBeOpaque"}
    /// Returns {"png": base64, "width", "height"}.
    private func screenshot(_ a: [Any]) throws -> Any {
        guard let filterSpec = a.first as? [String: Any] else { throw AXCallError.usage("argument 1: expected a content filter object") }
        let configSpec = a.count > 1 ? (a[1] as? [String: Any] ?? [:]) : [:]
        let result = blocking { () async throws -> CGImage in
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
            let filter: SCContentFilter
            var pointSize: CGSize
            var scale: CGFloat
            if let windowID = (filterSpec["desktopIndependentWindow"] as? NSNumber)?.uint32Value {
                guard let window = content.windows.first(where: { $0.windowID == windowID }) else {
                    throw AXCallError.usage("no window \(windowID)")
                }
                filter = SCContentFilter(desktopIndependentWindow: window)
                pointSize = window.frame.size
                scale = CGFloat(filter.pointPixelScale)
            } else if let displayID = (filterSpec["display"] as? NSNumber)?.uint32Value {
                guard let display = content.displays.first(where: { $0.displayID == displayID }) else {
                    throw AXCallError.usage("no display \(displayID)")
                }
                let excluded = Set((filterSpec["excludingWindows"] as? [NSNumber] ?? []).map { $0.uint32Value })
                filter = SCContentFilter(display: display, excludingWindows: content.windows.filter { excluded.contains($0.windowID) })
                pointSize = display.frame.size
                scale = CGFloat(filter.pointPixelScale)
            } else {
                throw AXCallError.usage("content filter needs \"display\" or \"desktopIndependentWindow\"")
            }
            let config = SCStreamConfiguration()
            if let r = configSpec["sourceRect"] as? [Double], r.count == 4 {
                config.sourceRect = CGRect(x: r[0], y: r[1], width: r[2], height: r[3])
                pointSize = config.sourceRect.size
            }
            config.width = (configSpec["width"] as? Int) ?? Int(pointSize.width * scale)
            config.height = (configSpec["height"] as? Int) ?? Int(pointSize.height * scale)
            config.showsCursor = (configSpec["showsCursor"] as? Bool) ?? false
            if let v = configSpec["scalesToFit"] as? Bool { config.scalesToFit = v }
            if let v = configSpec["ignoreShadowsSingleWindow"] as? Bool { config.ignoreShadowsSingleWindow = v }
            if let v = configSpec["shouldBeOpaque"] as? Bool { config.shouldBeOpaque = v }
            return try await SCScreenshotManager.captureImage(contentFilter: filter, configuration: config)
        }
        let image = try result.get()
        let png = NSMutableData()
        guard let dest = CGImageDestinationCreateWithData(png, "public.png" as CFString, 1, nil) else {
            throw AXCallError.usage("PNG encoder unavailable")
        }
        CGImageDestinationAddImage(dest, image, nil)
        guard CGImageDestinationFinalize(dest) else { throw AXCallError.usage("PNG encoding failed") }
        return ["png": (png as Data).base64EncodedString(), "width": image.width, "height": image.height]
    }

    /// Connection threads are plain threads, so they can wait for async work.
    private func blocking<T>(_ work: @escaping () async throws -> T) -> Result<T, Error> {
        let done = DispatchSemaphore(value: 0)
        nonisolated(unsafe) var result: Result<T, Error> = .failure(AXCallError.usage("no result"))
        Task.detached {
            do { result = .success(try await work()) } catch { result = .failure(error) }
            done.signal()
        }
        done.wait()
        return result
    }

    // MARK: Arguments

    private func arg<T>(_ a: [Any], _ i: Int, _ type: T.Type) throws -> T {
        guard i < a.count else { throw AXCallError.usage("missing argument \(i + 1)") }
        let v = a[i]
        if T.self == Int.self, let n = v as? NSNumber { return n.intValue as! T }
        if T.self == Double.self, let n = v as? NSNumber { return n.doubleValue as! T }
        if T.self == Bool.self, let n = v as? NSNumber { return n.boolValue as! T }
        if let v = v as? T { return v }
        throw AXCallError.usage("argument \(i + 1): expected \(T.self)")
    }

    private func event(_ a: [Any], _ i: Int) throws -> CGEvent {
        guard let handle = try? arg(a, i, String.self), let e = events.event(handle) else {
            throw AXCallError.usage("argument \(i + 1): unknown event handle")
        }
        return e
    }

    /// {"point": [x, y]} or [x, y]
    private func point(_ a: [Any], _ i: Int) throws -> CGPoint {
        guard i < a.count else { throw AXCallError.usage("missing argument \(i + 1)") }
        let list = (a[i] as? [String: Any])?["point"] as? [NSNumber] ?? a[i] as? [NSNumber]
        guard let list, list.count == 2 else { throw AXCallError.usage("argument \(i + 1): expected {\"point\": [x, y]}") }
        return CGPoint(x: list[0].doubleValue, y: list[1].doubleValue)
    }

    private func field(_ a: [Any], _ i: Int) throws -> CGEventField {
        guard let f = CGEventField(rawValue: UInt32(try arg(a, i, Int.self))) else { throw AXCallError.usage("bad CGEventField") }
        return f
    }
}

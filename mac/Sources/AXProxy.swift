import AppKit
import ApplicationServices
import Darwin

/// Exposes the macOS Accessibility API, function for function, on a TCP port on the Thunderbolt
/// bridge (and a local Unix socket): MacGlass has the Accessibility permission, other processes don't.
///
/// Newline-delimited JSON. A request names an AX function and its arguments,
///   {"id": 1, "fn": "AXUIElementCopyAttributeValue", "args": ["e1", "AXChildren"]}
/// and gets the function's result or its AXError:
///   {"id": 1, "result": [{"element": "e2"}, ...]}
///   {"id": 1, "error": "kAXErrorAttributeUnsupported", "code": -25205}
/// AXUIElementRefs and AXObserverRefs are passed as handles ("e5", "o1"); the same element always
/// gets the same handle, and CFRelease drops one. Values map to JSON as strings, numbers, booleans,
/// null, arrays, and tagged objects: {"element"}, {"point": [x, y]}, {"size": [w, h]},
/// {"rect": [x, y, w, h]}, {"range": [location, length]}, {"axerror": code}, {"url"},
/// {"dictionary": {...}}, {"attributedString"}, {"date"}, {"data": base64}. Observer
/// notifications arrive on the connection that registered them as
///   {"observer": "o1", "notification": "AXFocusedUIElementChanged", "element": {...}, "info": {...}}
final class AXProxy {
    static let socketPath: String = {
        let dir = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/MacGlass", isDirectory: true)
        try? FileManager.default.createDirectory(
            at: dir, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        return dir.appendingPathComponent("ax.sock").path
    }()

    let handles = AXHandles()
    let events = CGEventHandles()
    private(set) var observerLoop: CFRunLoop!

    func start() throws {
        startObserverThread()
        let path = Self.socketPath
        unlink(path)
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw MGError("accessibility socket: \(String(cString: strerror(errno)))") }
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        addr.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        let bytes = Array(path.utf8)
        guard bytes.count < MemoryLayout.size(ofValue: addr.sun_path) else { throw MGError("socket path too long: \(path)") }
        withUnsafeMutableBytes(of: &addr.sun_path) { $0.copyBytes(from: bytes) }
        let bound = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
        }
        guard bound == 0, chmod(path, 0o600) == 0, listen(fd, 8) == 0 else {
            throw MGError("accessibility socket \(path): \(String(cString: strerror(errno)))")
        }
        log("accessibility proxy on \(path)")
        acceptLoop(fd)
    }

    /// The same protocol over TCP, reachable only through `interface` (the Thunderbolt bridge):
    /// the cable leads to one machine, so there is no further authentication.
    func startTCP(port: UInt16, interface: String) throws {
        let fd = socket(AF_INET6, SOCK_STREAM, 0)
        guard fd >= 0 else { throw MGError("socket: \(String(cString: strerror(errno)))") }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
        var index = if_nametoindex(interface)
        guard index != 0 else { throw MGError("no network interface named \(interface)") }
        guard setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &index, socklen_t(MemoryLayout<UInt32>.size)) == 0 else {
            throw MGError("cannot bind to \(interface): \(String(cString: strerror(errno)))")
        }
        var addr = sockaddr_in6()
        addr.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
        addr.sin6_family = sa_family_t(AF_INET6)
        addr.sin6_port = port.bigEndian
        addr.sin6_addr = in6addr_any
        let bound = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in6>.size)) }
        }
        guard bound == 0, listen(fd, 8) == 0 else { throw MGError("cannot listen on port \(port): \(String(cString: strerror(errno)))") }
        log("accessibility proxy on port \(port) (\(interface) only)")
        acceptLoop(fd)
    }

    private func acceptLoop(_ fd: Int32) {
        Thread {
            while true {
                let client = accept(fd, nil, nil)
                guard client >= 0 else { continue }
                var one: Int32 = 1
                setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
                setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))  // fails harmlessly on Unix sockets
                let connection = AXConnection(fd: client, proxy: self)
                Thread { connection.run() }.start()
            }
        }.start()
    }

    /// Observer callbacks are delivered on this thread's run loop.
    private func startObserverThread() {
        let ready = DispatchSemaphore(value: 0)
        Thread {
            self.observerLoop = CFRunLoopGetCurrent()
            var context = CFRunLoopSourceContext()
            let keepAlive = CFRunLoopSourceCreate(nil, 0, &context)  // the loop needs a source to run
            CFRunLoopAddSource(CFRunLoopGetCurrent(), keepAlive, .defaultMode)
            ready.signal()
            CFRunLoopRun()
        }.start()
        ready.wait()
    }

    // MARK: Calls

    func handle(_ line: Data, _ connection: AXConnection) -> [String: Any] {
        guard let request = try? JSONSerialization.jsonObject(with: line) as? [String: Any] else {
            return ["error": "bad request: expected {\"fn\": ..., \"args\": [...]}", "code": 0]
        }
        var reply: [String: Any] = ["id": request["id"] ?? NSNull()]
        if let batch = request["batch"] as? [[String: Any]] {
            reply["results"] = batch.map { perform($0, connection) }
        } else {
            reply.merge(perform(request, connection)) { _, new in new }
        }
        return reply
    }

    private func perform(_ request: [String: Any], _ connection: AXConnection) -> [String: Any] {
        guard let fn = request["fn"] as? String else { return ["error": "missing \"fn\"", "code": 0] }
        do {
            return ["result": try call(fn, request["args"] as? [Any] ?? [], connection)]
        } catch let error as AXCallError {
            return ["error": error.name, "code": error.code]
        } catch {
            return ["error": "\(error)", "code": 0]
        }
    }

    private func call(_ fn: String, _ a: [Any], _ connection: AXConnection) throws -> Any {
        switch fn {
        case "AXIsProcessTrusted":
            return AXIsProcessTrusted()
        case "AXUIElementCreateSystemWide":
            return encode(AXUIElementCreateSystemWide())
        case "AXUIElementCreateApplication":
            return encode(AXUIElementCreateApplication(pid_t(try int(a, 0))))
        case "AXUIElementGetPid":
            var pid: pid_t = 0
            try check(AXUIElementGetPid(try element(a, 0), &pid))
            return Int(pid)
        case "AXUIElementCopyAttributeNames":
            var names: CFArray?
            try check(AXUIElementCopyAttributeNames(try element(a, 0), &names))
            return encode(names)
        case "AXUIElementCopyAttributeValue":
            var value: CFTypeRef?
            try check(AXUIElementCopyAttributeValue(try element(a, 0), try string(a, 1) as CFString, &value))
            return encode(value)
        case "AXUIElementCopyAttributeValues":
            var values: CFArray?
            try check(AXUIElementCopyAttributeValues(
                try element(a, 0), try string(a, 1) as CFString, try int(a, 2), try int(a, 3), &values))
            return encode(values)
        case "AXUIElementCopyMultipleAttributeValues":
            guard a.count > 1, let names = a[1] as? [String] else { throw AXCallError.usage("expected [attribute names]") }
            let options = AXCopyMultipleAttributeOptions(rawValue: UInt32(a.count > 2 ? try int(a, 2) : 0))
            var values: CFArray?
            try check(AXUIElementCopyMultipleAttributeValues(try element(a, 0), names as CFArray, options, &values))
            return encode(values)
        case "AXUIElementGetAttributeValueCount":
            var count: CFIndex = 0
            try check(AXUIElementGetAttributeValueCount(try element(a, 0), try string(a, 1) as CFString, &count))
            return count
        case "AXUIElementIsAttributeSettable":
            var settable: DarwinBoolean = false
            try check(AXUIElementIsAttributeSettable(try element(a, 0), try string(a, 1) as CFString, &settable))
            return settable.boolValue
        case "AXUIElementSetAttributeValue":
            guard a.count > 2 else { throw AXCallError.usage("expected element, attribute, value") }
            try check(AXUIElementSetAttributeValue(try element(a, 0), try string(a, 1) as CFString, try decode(a[2])))
            return NSNull()
        case "AXUIElementCopyActionNames":
            var names: CFArray?
            try check(AXUIElementCopyActionNames(try element(a, 0), &names))
            return encode(names)
        case "AXUIElementCopyActionDescription":
            var description: CFString?
            try check(AXUIElementCopyActionDescription(try element(a, 0), try string(a, 1) as CFString, &description))
            return encode(description)
        case "AXUIElementPerformAction":
            try check(AXUIElementPerformAction(try element(a, 0), try string(a, 1) as CFString))
            return NSNull()
        case "AXUIElementCopyParameterizedAttributeNames":
            var names: CFArray?
            try check(AXUIElementCopyParameterizedAttributeNames(try element(a, 0), &names))
            return encode(names)
        case "AXUIElementCopyParameterizedAttributeValue":
            guard a.count > 2 else { throw AXCallError.usage("expected element, attribute, parameter") }
            var value: CFTypeRef?
            try check(AXUIElementCopyParameterizedAttributeValue(
                try element(a, 0), try string(a, 1) as CFString, try decode(a[2]), &value))
            return encode(value)
        case "AXUIElementCopyElementAtPosition":
            var found: AXUIElement?
            try check(AXUIElementCopyElementAtPosition(try element(a, 0), Float(try double(a, 1)), Float(try double(a, 2)), &found))
            return encode(found)
        case "AXUIElementSetMessagingTimeout":
            try check(AXUIElementSetMessagingTimeout(try element(a, 0), Float(try double(a, 1))))
            return NSNull()
        case "AXObserverCreate":
            return try connection.createObserver(pid_t(try int(a, 0)))
        case "AXObserverAddNotification":
            try check(connection.addNotification(try string(a, 0), try element(a, 1), try string(a, 2)))
            return NSNull()
        case "AXObserverRemoveNotification":
            try check(connection.removeNotification(try string(a, 0), try element(a, 1), try string(a, 2)))
            return NSNull()
        case "CFRelease":
            let handle = try string(a, 0)
            if handle.hasPrefix("o") {
                connection.releaseObserver(handle)
            } else if handle.hasPrefix("v") {
                events.release(handle)
            } else {
                handles.release(handle)
            }
            return NSNull()
        case "NSWorkspace.runningApplications":
            return DispatchQueue.main.sync {
                NSWorkspace.shared.runningApplications.map { app -> [String: Any] in
                    [
                        "processIdentifier": Int(app.processIdentifier),
                        "localizedName": app.localizedName ?? NSNull(),
                        "bundleIdentifier": app.bundleIdentifier ?? NSNull(),
                        "activationPolicy": app.activationPolicy.rawValue,
                        "isActive": app.isActive,
                        "isHidden": app.isHidden,
                    ]
                }
            }
        default:
            if let result = try callCG(fn, a) { return result }
            throw AXCallError.usage("unknown function \(fn)")
        }
    }

    // MARK: Arguments

    private func element(_ a: [Any], _ i: Int) throws -> AXUIElement {
        guard i < a.count else { throw AXCallError.usage("missing argument \(i + 1)") }
        let handle = (a[i] as? String) ?? ((a[i] as? [String: Any])?["element"] as? String)
        guard let handle, let el = handles.element(handle) else { throw AXCallError.usage("argument \(i + 1): unknown element handle") }
        return el
    }

    private func string(_ a: [Any], _ i: Int) throws -> String {
        guard i < a.count, let s = a[i] as? String else { throw AXCallError.usage("argument \(i + 1): expected a string") }
        return s
    }

    private func int(_ a: [Any], _ i: Int) throws -> Int {
        guard i < a.count, let n = a[i] as? NSNumber else { throw AXCallError.usage("argument \(i + 1): expected a number") }
        return n.intValue
    }

    private func double(_ a: [Any], _ i: Int) throws -> Double {
        guard i < a.count, let n = a[i] as? NSNumber else { throw AXCallError.usage("argument \(i + 1): expected a number") }
        return n.doubleValue
    }

    private func check(_ err: AXError) throws {
        if err != .success { throw AXCallError(err) }
    }

    // MARK: Values

    func encode(_ value: CFTypeRef?) -> Any {
        guard let v = value else { return NSNull() }
        let type = CFGetTypeID(v)
        switch type {
        case AXUIElementGetTypeID():
            return ["element": handles.handle(for: v as! AXUIElement)]
        case AXValueGetTypeID():
            return encodeAXValue(v as! AXValue)
        case CFStringGetTypeID():
            return v as! String
        case CFBooleanGetTypeID():
            return CFBooleanGetValue((v as! CFBoolean))
        case CFNumberGetTypeID():
            let n = v as! NSNumber
            return CFNumberIsFloatType((v as! CFNumber)) ? n.doubleValue : n.int64Value
        case CFArrayGetTypeID():
            return (v as! [AnyObject]).map { encode($0) }
        case CFDictionaryGetTypeID():
            var out: [String: Any] = [:]
            for (key, value) in v as! [AnyHashable: AnyObject] { out["\(key)"] = encode(value) }
            return ["dictionary": out]
        case CFURLGetTypeID():
            return ["url": (v as! URL).absoluteString]
        case CFAttributedStringGetTypeID():
            return ["attributedString": (v as! NSAttributedString).string]
        case CFDateGetTypeID():
            return ["date": (v as! Date).timeIntervalSince1970]
        case CFDataGetTypeID():
            return ["data": (v as! Data).base64EncodedString()]
        case CFNullGetTypeID():
            return NSNull()
        default:
            return ["type": CFCopyTypeIDDescription(type) as String, "description": CFCopyDescription(v) as String]
        }
    }

    private func encodeAXValue(_ v: AXValue) -> Any {
        switch AXValueGetType(v) {
        case .cgPoint:
            var p = CGPoint.zero
            AXValueGetValue(v, .cgPoint, &p)
            return ["point": [p.x, p.y]]
        case .cgSize:
            var s = CGSize.zero
            AXValueGetValue(v, .cgSize, &s)
            return ["size": [s.width, s.height]]
        case .cgRect:
            var r = CGRect.zero
            AXValueGetValue(v, .cgRect, &r)
            return ["rect": [r.origin.x, r.origin.y, r.width, r.height]]
        case .cfRange:
            var r = CFRange()
            AXValueGetValue(v, .cfRange, &r)
            return ["range": [r.location, r.length]]
        case .axError:
            var e = AXError.success
            AXValueGetValue(v, .axError, &e)
            return ["axerror": Int(e.rawValue), "name": AXCallError(e).name]
        default:
            return ["axvalue": "illegal"]
        }
    }

    func decode(_ j: Any) throws -> CFTypeRef {
        switch j {
        case let s as String:
            return s as CFString
        case let n as NSNumber:
            return n  // JSON booleans arrive as CFBoolean, numbers as CFNumber
        case let list as [Any]:
            return try list.map { try decode($0) } as CFArray
        case is NSNull:
            return kCFNull
        case let d as [String: Any]:
            if let handle = d["element"] as? String {
                guard let el = handles.element(handle) else { throw AXCallError.usage("unknown element handle \(handle)") }
                return el
            }
            if let p = d["point"] as? [Double], p.count == 2 {
                var v = CGPoint(x: p[0], y: p[1])
                return AXValueCreate(.cgPoint, &v)!
            }
            if let s = d["size"] as? [Double], s.count == 2 {
                var v = CGSize(width: s[0], height: s[1])
                return AXValueCreate(.cgSize, &v)!
            }
            if let r = d["rect"] as? [Double], r.count == 4 {
                var v = CGRect(x: r[0], y: r[1], width: r[2], height: r[3])
                return AXValueCreate(.cgRect, &v)!
            }
            if let r = d["range"] as? [Int], r.count == 2 {
                var v = CFRange(location: r[0], length: r[1])
                return AXValueCreate(.cfRange, &v)!
            }
            if let u = d["url"] as? String, let url = URL(string: u) {
                return url as CFURL
            }
            if let inner = d["dictionary"] as? [String: Any] {
                var out: [String: CFTypeRef] = [:]
                for (key, value) in inner { out[key] = try decode(value) }
                return out as CFDictionary
            }
            throw AXCallError.usage("unknown value object \(d.keys.sorted())")
        default:
            throw AXCallError.usage("unsupported value \(j)")
        }
    }
}

/// Handles for AXUIElementRefs. Equal elements (CFEqual) share one handle.
final class AXHandles {
    private struct Key: Hashable {
        let element: AXUIElement
        static func == (a: Key, b: Key) -> Bool { CFEqual(a.element, b.element) }
        func hash(into hasher: inout Hasher) { hasher.combine(CFHash(element)) }
    }

    private let lock = NSLock()
    private var byHandle: [String: AXUIElement] = [:]
    private var byElement: [Key: String] = [:]
    private var next = 1

    func handle(for element: AXUIElement) -> String {
        lock.withLock {
            if let h = byElement[Key(element: element)] { return h }
            let h = "e\(next)"
            next += 1
            byHandle[h] = element
            byElement[Key(element: element)] = h
            return h
        }
    }

    func element(_ handle: String) -> AXUIElement? { lock.withLock { byHandle[handle] } }

    func release(_ handle: String) {
        lock.withLock {
            if let el = byHandle.removeValue(forKey: handle) { byElement[Key(element: el)] = nil }
        }
    }
}

struct AXCallError: Error {
    let code: Int
    let name: String

    init(_ err: AXError) {
        code = Int(err.rawValue)
        name = Self.names[err.rawValue] ?? "AXError \(err.rawValue)"
    }

    private init(code: Int, name: String) {
        self.code = code
        self.name = name
    }

    static func usage(_ message: String) -> AXCallError { AXCallError(code: 0, name: message) }

    private static let names: [Int32: String] = [
        -25200: "kAXErrorFailure", -25201: "kAXErrorIllegalArgument", -25202: "kAXErrorInvalidUIElement",
        -25203: "kAXErrorInvalidUIElementObserver", -25204: "kAXErrorCannotComplete",
        -25205: "kAXErrorAttributeUnsupported", -25206: "kAXErrorActionUnsupported",
        -25207: "kAXErrorNotificationUnsupported", -25208: "kAXErrorNotImplemented",
        -25209: "kAXErrorNotificationAlreadyRegistered", -25210: "kAXErrorNotificationNotRegistered",
        -25211: "kAXErrorAPIDisabled", -25212: "kAXErrorNoValue",
        -25213: "kAXErrorParameterizedAttributeUnsupported", -25214: "kAXErrorNotEnoughPrecision",
    ]
}

/// One client. Owns the observers it created; they go away with it.
final class AXConnection {
    private let fd: Int32
    private unowned let proxy: AXProxy
    private let writeLock = NSLock()
    private let observerLock = NSLock()
    private var observers: [String: AXObserver] = [:]
    private static var nextObserver = 1
    private static let observerCounter = NSLock()
    private var calls = 0

    init(fd: Int32, proxy: AXProxy) {
        self.fd = fd
        self.proxy = proxy
    }

    func run() {
        log("accessibility client connected")
        var buffer = Data()
        var chunk = [UInt8](repeating: 0, count: 65536)
        while true {
            let n = read(fd, &chunk, chunk.count)
            if n < 0 && errno == EINTR { continue }
            if n <= 0 { break }
            buffer.append(contentsOf: chunk[0..<n])
            while let newline = buffer.firstIndex(of: 0x0A) {
                let line = buffer.subdata(in: buffer.startIndex..<newline)
                buffer = buffer.subdata(in: (newline + 1)..<buffer.endIndex)
                if line.isEmpty { continue }
                calls += 1
                send(proxy.handle(line, self))
            }
        }
        for handle in observerLock.withLock({ Array(observers.keys) }) { releaseObserver(handle) }
        close(fd)
        log("accessibility client disconnected after \(calls) call(s)")
    }

    func send(_ message: [String: Any]) {
        guard var data = try? JSONSerialization.data(withJSONObject: message) else { return }
        data.append(0x0A)
        writeLock.withLock {
            data.withUnsafeBytes { raw in
                var p = raw.baseAddress!
                var left = raw.count
                while left > 0 {
                    let n = write(fd, p, left)
                    if n < 0 && errno == EINTR { continue }
                    if n <= 0 { return }
                    p += n
                    left -= n
                }
            }
        }
    }

    func createObserver(_ pid: pid_t) throws -> String {
        var observer: AXObserver?
        let err = AXObserverCreateWithInfoCallback(pid, axObserverCallback, &observer)
        guard err == .success, let observer else { throw AXCallError(err) }
        let handle = Self.observerCounter.withLock { () -> String in
            defer { Self.nextObserver += 1 }
            return "o\(Self.nextObserver)"
        }
        observerLock.withLock { observers[handle] = observer }
        let loop = proxy.observerLoop!
        CFRunLoopPerformBlock(loop, CFRunLoopMode.defaultMode.rawValue) {
            CFRunLoopAddSource(loop, AXObserverGetRunLoopSource(observer), .defaultMode)
        }
        CFRunLoopWakeUp(loop)
        return handle
    }

    func addNotification(_ handle: String, _ element: AXUIElement, _ name: String) -> AXError {
        guard let observer = observerLock.withLock({ observers[handle] }) else { return .invalidUIElementObserver }
        let context = Unmanaged.passUnretained(ObserverContext(connection: self, handle: handle)).toOpaque()
        return AXObserverAddNotification(observer, element, name as CFString, context)
    }

    func removeNotification(_ handle: String, _ element: AXUIElement, _ name: String) -> AXError {
        guard let observer = observerLock.withLock({ observers[handle] }) else { return .invalidUIElementObserver }
        return AXObserverRemoveNotification(observer, element, name as CFString)
    }

    /// Removed on the observer thread, so no callback for it can be running at the same time.
    func releaseObserver(_ handle: String) {
        guard let observer = observerLock.withLock({ observers.removeValue(forKey: handle) }) else { return }
        let loop = proxy.observerLoop!
        CFRunLoopPerformBlock(loop, CFRunLoopMode.defaultMode.rawValue) { [self] in
            CFRunLoopRemoveSource(loop, AXObserverGetRunLoopSource(observer), .defaultMode)
            _ = self  // keep the connection alive until no callback can reach it
        }
        CFRunLoopWakeUp(loop)
    }

    fileprivate func deliver(_ handle: String, _ element: AXUIElement, _ name: CFString, _ info: CFDictionary?) {
        send([
            "observer": handle,
            "notification": name as String,
            "element": proxy.encode(element),
            "info": proxy.encode(info),
        ])
    }
}

/// What a notification registration's refcon points to. Kept for the process lifetime: AX holds
/// the pointer until the notification is removed, which may never be asked for.
private final class ObserverContext {
    let connection: AXConnection
    let handle: String
    static var all: [ObserverContext] = []
    static let lock = NSLock()

    init(connection: AXConnection, handle: String) {
        self.connection = connection
        self.handle = handle
        Self.lock.withLock { Self.all.append(self) }
    }
}

private func axObserverCallback(
    _ observer: AXObserver, _ element: AXUIElement, _ name: CFString, _ info: CFDictionary?,
    _ refcon: UnsafeMutableRawPointer?
) {
    guard let refcon else { return }
    let context = Unmanaged<ObserverContext>.fromOpaque(refcon).takeUnretainedValue()
    context.connection.deliver(context.handle, element, name, info)
}

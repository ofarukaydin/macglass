import Darwin
import Foundation

/// Single-client TCP server bound to one interface (the Thunderbolt bridge), so the screen
/// stream is never reachable over Wi-Fi or Ethernet. A new client replaces the current one: after
/// a pulled cable or a suspended PC the old connection can look alive for a while, and the viewer
/// reconnecting must not wait for that.
final class Server {
    let port: UInt16
    let interface: String
    var onConnect: (() -> Void)?
    var onDisconnect: (() -> Void)?
    /// Client messages, called on the server's read thread in arrival order.
    var onMessage: ((UInt32, Data) -> Void)?

    /// Encoded frames queued but not yet written; beyond this new frames are dropped so
    /// latency can't build up behind a slow link.
    static let maxInFlight = 2

    private let sendQueue = DispatchQueue(label: "macglass.send", qos: .userInteractive)
    private let lock = NSLock()
    private var clientFD: Int32 = -1
    private var latestFD: Int32 = -1  // the most recently accepted client; older ones give way
    private var inFlight = 0
    /// Held for a connection's whole life, so the next one starts only after onDisconnect.
    private let turn = NSLock()

    init(port: UInt16, interface: String) {
        self.port = port
        self.interface = interface
    }

    func start() throws {
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
        guard bound == 0, listen(fd, 4) == 0 else { throw MGError("cannot listen on port \(port): \(String(cString: strerror(errno)))") }
        log("listening on port \(port) (\(interface) only)")
        Thread { self.acceptLoop(fd) }.start()
    }

    var canSend: Bool {
        lock.lock()
        defer { lock.unlock() }
        return clientFD >= 0 && inFlight < Self.maxInFlight
    }

    /// Uncounted messages (audio) don't hold back video frames.
    func send(type: UInt32, header: Data, body: Data = Data(), counted: Bool = true) {
        lock.lock()
        let fd = clientFD
        if fd >= 0 && counted { inFlight += 1 }
        lock.unlock()
        guard fd >= 0 else { return }
        sendQueue.async {
            var head = Data(capacity: 8 + header.count)
            head.appendLE(type)
            head.appendLE(UInt32(header.count + body.count))
            head.append(header)
            if !(writeAll(fd, head) && writeAll(fd, body)) { shutdown(fd, SHUT_RDWR) }
            if counted {
                self.lock.lock()
                self.inFlight -= 1
                self.lock.unlock()
            }
        }
    }

    private func acceptLoop(_ listenFD: Int32) {
        while true {
            let fd = accept(listenFD, nil, nil)
            guard fd >= 0 else { continue }
            configure(fd)
            lock.lock()
            let current = clientFD
            latestFD = fd
            lock.unlock()
            if current >= 0 {
                log("a new client replaces the current one")
                shutdown(current, SHUT_RDWR)
            }
            Thread { self.serve(fd) }.start()
        }
    }

    private func configure(_ fd: Int32) {
        func set(_ level: Int32, _ option: Int32, _ value: Int32) {
            var v = value
            setsockopt(fd, level, option, &v, socklen_t(MemoryLayout<Int32>.size))
        }
        set(IPPROTO_TCP, TCP_NODELAY, 1)
        set(SOL_SOCKET, SO_NOSIGPIPE, 1)
        set(SOL_SOCKET, SO_SNDBUF, 32 << 20)
        // Notice a client that vanished (cable pulled, PC asleep) within seconds, idle or not, so
        // the Mac stops streaming and may sleep again.
        set(SOL_SOCKET, SO_KEEPALIVE, 1)
        set(IPPROTO_TCP, TCP_KEEPALIVE, 2)
        set(IPPROTO_TCP, TCP_KEEPINTVL, 1)
        set(IPPROTO_TCP, TCP_KEEPCNT, 3)
        set(IPPROTO_TCP, TCP_RXT_CONNDROPTIME, 5)
    }

    private func serve(_ fd: Int32) {
        turn.lock()
        defer { turn.unlock() }
        lock.lock()
        let superseded = latestFD != fd
        if !superseded {
            clientFD = fd
            inFlight = 0
        }
        lock.unlock()
        if superseded {  // another client arrived while this one waited for its turn
            close(fd)
            return
        }
        log("client connected")
        onConnect?()
        readLoop(fd)
        lock.lock()
        clientFD = -1
        lock.unlock()
        sendQueue.sync {}  // let queued writes finish before the descriptor is reused
        close(fd)
        log("client disconnected")
        onDisconnect?()
    }

    private func readLoop(_ fd: Int32) {
        while true {
            guard let head = readExactly(fd, 8) else { return }
            let type = head.readLE(UInt32.self, at: 0)
            let length = Int(head.readLE(UInt32.self, at: 4))
            guard length <= 64 << 20, let payload = readExactly(fd, length) else { return }
            onMessage?(type, payload)
        }
    }
}

private func writeAll(_ fd: Int32, _ data: Data) -> Bool {
    data.withUnsafeBytes { raw -> Bool in
        guard var p = raw.baseAddress else { return true }
        var left = raw.count
        while left > 0 {
            let n = write(fd, p, left)
            if n < 0 {
                if errno == EINTR { continue }
                return false
            }
            p += n
            left -= n
        }
        return true
    }
}

private func readExactly(_ fd: Int32, _ count: Int) -> Data? {
    var data = Data(count: count)
    let ok = data.withUnsafeMutableBytes { raw -> Bool in
        guard var p = raw.baseAddress else { return true }
        var left = count
        while left > 0 {
            let n = read(fd, p, left)
            if n < 0 && errno == EINTR { continue }
            if n <= 0 { return false }
            p += n
            left -= n
        }
        return true
    }
    return ok ? data : nil
}
